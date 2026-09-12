#!/usr/bin/env python3
"""Build every app under apps/ into a .papp.

Each app is described by apps/<name>/papp.json. Sources are fetched from their
upstream repositories at the pinned commit rather than copied into this repo.
The compile and link steps mirror RetroESP32-P4's PowerShell build scripts
(tools/build_lvgl_papp.ps1 for "lvgl", tools/build_psram_app.ps1 for "plain",
and tools/build_<game>_papp.ps1 for "custom" recipes), so a .papp built here
matches one built with those scripts.

Needs the ESP-IDF RISC-V toolchain (riscv32-esp-elf-*) on PATH and git.

    python3 tools/build_papp.py                 # all apps -> dist/
    python3 tools/build_papp.py psram_lvgl      # one app
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APPS = ROOT / "apps"

# The PAPP SDK (ABI header, linker script, packer) is taken from the app's own
# source repository at the same commit, so an app always builds against the
# loader ABI it was written for.
SDK_INCLUDE = "components/psram_app_loader/include"
SDK_PATHS = [SDK_INCLUDE, "tools/psram_app.ld", "tools/pack_papp.py"]

PAPP_MAGIC = 0x50415050
PAPP_ABI_VERSION = 1
PAPP_HEADER = struct.Struct("<IIIIIIII")
LINK_BASE = 0x4A000000

CC = "riscv32-esp-elf-gcc"
CXX = "riscv32-esp-elf-g++"
OBJCOPY = "riscv32-esp-elf-objcopy"
NM = "riscv32-esp-elf-nm"
SIZE = "riscv32-esp-elf-size"

# ESP32-P4 RISC-V ABI; must match ESP-IDF.
ARCH_FLAGS = ["-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f"]

# -fno-tree-loop-distribute-patterns stops GCC turning the byte loops inside the
# app's own memset()/memcpy() into calls to memset()/memcpy() (infinite recursion).
CFLAGS = [
    "-mcmodel=medany",
    "-fno-common",
    "-ffunction-sections",
    "-fdata-sections",
    "-fno-tree-loop-distribute-patterns",
    "-ffreestanding",
    "-Os",
    "-DPAPP_APP_SIDE=1",
] + ARCH_FLAGS

# One LOAD segment holds text+data, which ld flags as RWX; that is expected here.
LDFLAGS = [
    "-nostartfiles",
    "-nodefaultlibs",
    "-nostdlib",
    "-Wl,--gc-sections",
    "-Wl,--entry=app_entry",
    "-Wl,--no-relax",
    "-Wl,--no-warn-rwx-segments",
] + ARCH_FLAGS

# "custom" builds link newlib (-lc -lgcc -lm). Its heap and lock entry points
# are wrapped so they go through the loader's app_services_t instead.
NEWLIB_WRAPS = [
    "malloc", "free", "calloc", "realloc",
    "_malloc_r", "_free_r", "_calloc_r", "_realloc_r",
    "__retarget_lock_init", "__retarget_lock_init_recursive",
    "__retarget_lock_close", "__retarget_lock_close_recursive",
    "__retarget_lock_acquire", "__retarget_lock_try_acquire",
    "__retarget_lock_acquire_recursive", "__retarget_lock_try_acquire_recursive",
    "__retarget_lock_release", "__retarget_lock_release_recursive",
]
NEWLIB_LIBS = ["-lc", "-lgcc", "-lm"]


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, text=True, **kwargs)


def fetch(repo: str, ref: str, dest: Path, sparse: list[str] | None = None) -> Path:
    """Check out `repo` at the exact commit `ref` into `dest` (cached by ref and paths)."""
    stamp = dest / ".papp-ref"
    key = "\n".join([ref, *sorted(sparse or [])])
    if stamp.exists() and stamp.read_text().strip() == key:
        return dest
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    run(["git", "init", "-q"], cwd=dest)
    run(["git", "remote", "add", "origin", repo], cwd=dest)
    if sparse:
        run(["git", "sparse-checkout", "set", "--no-cone", *sparse], cwd=dest)
    run(["git", "fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", ref], cwd=dest)
    run(["git", "checkout", "-q", "FETCH_HEAD"], cwd=dest)
    stamp.write_text(key)
    return dest


def parse_header(data: bytes) -> dict:
    if len(data) < PAPP_HEADER.size:
        raise ValueError("file is shorter than the 32-byte PAPP header")
    magic, version, entry, text, data_size, bss, flags, _ = PAPP_HEADER.unpack_from(data)
    if magic != PAPP_MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x}")
    if version != PAPP_ABI_VERSION:
        raise ValueError(f"ABI version {version}, expected {PAPP_ABI_VERSION}")
    if PAPP_HEADER.size + text + data_size != len(data):
        raise ValueError("header sizes do not match the file length")
    return {"entry_offset": entry, "text_size": text, "data_size": data_size, "bss_size": bss, "flags": flags}


class Unit:
    """One source file to compile: compiler, flags and object path."""

    def __init__(self, src: Path, obj: Path, flags: list[str], compiler: str = CC):
        self.src, self.obj, self.flags, self.compiler = src, obj, flags, compiler


def compile_one(unit: Unit) -> None:
    unit.obj.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([unit.compiler, *unit.flags, "-c", "-o", str(unit.obj), str(unit.src)], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"compile failed: {unit.src}\n{result.stderr}")


def source_file(root: Path, rel: str) -> Path:
    """Resolve a manifest path inside the source checkout, refusing to leave it."""
    root = root.resolve()
    path = (root / rel).resolve()
    if path != root and root not in path.parents:
        raise ValueError(f"path '{rel}' leaves the source checkout")
    return path


def custom_units(manifest: dict, src_root: Path, build_dir: Path) -> tuple[list[Unit], list[str]]:
    """Compile units and extra link flags for a "custom" recipe.

    Mirrors RetroESP32-P4's tools/build_<game>_papp.ps1: explicit source lists
    per directory, one include list, C and C++ flags, and newlib with its heap
    wrapped through the loader.
    """
    includes = [f"-I{source_file(src_root, inc)}" for inc in manifest.get("includes", [])]
    base = ARCH_FLAGS + ["-mcmodel=medany"]
    cflags = base + manifest.get("cflags", []) + includes
    cxxflags = base + manifest.get("cxxflags", []) + includes
    units: list[Unit] = []
    for group in manifest["groups"]:
        extra = [f"-I{source_file(src_root, inc)}" for inc in group.get("includes", [])]
        for name in group["files"]:
            src = source_file(src_root, f"{group['dir']}/{name}")
            obj = build_dir / (group.get("prefix", "") + Path(name).stem + ".o")
            if src.suffix in (".cpp", ".cc", ".cxx"):
                units.append(Unit(src, obj, cxxflags + extra, CXX))
            elif src.suffix == ".c":
                units.append(Unit(src, obj, cflags + extra))
            else:
                raise ValueError(f"{name}: not a C or C++ source")
    objs = [u.obj for u in units]
    if len(set(objs)) != len(objs):
        raise ValueError("two sources map to the same object file; give a group a 'prefix'")
    ldflags = list(manifest.get("ldflags", []))
    if manifest.get("newlib"):
        ldflags += [f"-Wl,--wrap={sym}" for sym in NEWLIB_WRAPS] + NEWLIB_LIBS
    return units, ldflags


def build_app(manifest_path: Path, cache: Path, out: Path, jobs: int) -> dict:
    manifest = json.loads(manifest_path.read_text())
    name = manifest["name"]
    if manifest_path.parent.name != name:
        raise ValueError(f"{manifest_path}: name '{name}' must match its folder")
    print(f"=== {name} ===", flush=True)

    source = manifest["source"]
    build = manifest["build"]
    sparse = [source["path"], *SDK_PATHS]
    if build == "custom":
        sparse += [g["dir"] for g in manifest["groups"]] + manifest.get("includes", [])
    sdk = src_root = fetch(source["repo"], source["ref"], cache / f"src-{name}-{source['ref'][:12]}", sparse)
    app_dir = src_root / source["path"]

    cflags = CFLAGS + [f"-I{sdk / SDK_INCLUDE}", f"-I{app_dir}"]
    build_dir = cache / "build" / name
    if build_dir.exists():
        shutil.rmtree(build_dir)
    units: list[Unit] = []
    ldflags = list(LDFLAGS)
    linker = CC

    if build == "custom":
        units, link_tail = custom_units(manifest, src_root, build_dir)
        # newlib is linked, so -nostdlib goes; libraries follow the objects.
        ldflags = [f for f in LDFLAGS if f != "-nostdlib"]
        if any(u.compiler == CXX for u in units):
            linker = CXX
    elif build == "lvgl":
        lvgl = manifest["lvgl"]
        lvgl_dir = fetch(lvgl["repo"], lvgl["ref"], cache / f"lvgl-{lvgl['ref'][:12]}", ["/src/", "/*.h"])
        # lv_conf.h lives in the app folder; "lvgl/..." and "src/..." include styles both resolve.
        cflags += ["-DLV_CONF_INCLUDE_SIMPLE=1", f"-I{lvgl_dir}", f"-I{lvgl_dir / 'src'}"]
        # Everything under lvgl/src except src/drivers (SDL/X11/Linux backends we don't use).
        for c in sorted((lvgl_dir / "src").rglob("*.c")):
            rel = c.relative_to(lvgl_dir / "src")
            if rel.parts[0] == "drivers":
                continue
            units.append(Unit(c, build_dir / "lvgl" / ("_".join(rel.parts)[:-2] + ".o"), cflags))
        # libgcc supplies compiler helpers (64-bit divide etc.); safe because the
        # linker script binds the app at its real runtime address.
        link_tail = ["-lgcc"]
    elif build == "plain":
        link_tail = []
    else:
        raise ValueError(f"{name}: unknown build type '{build}'")

    if build != "custom":
        for c in sorted(app_dir.glob("*.c")):
            units.append(Unit(c, build_dir / (c.stem + ".o"), cflags))
    print(f"  compiling {len(units)} files", flush=True)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(compile_one, units))

    elf = build_dir / f"{name}.elf"
    link = [linker, *ldflags, f"-T{sdk / 'tools/psram_app.ld'}", "-o", str(elf), *[str(u.obj) for u in units], *link_tail]
    run(link)
    run([SIZE, str(elf)])

    binary = build_dir / f"{name}.bin"
    run([OBJCOPY, "-O", "binary", str(elf), str(binary)])
    bin_size = binary.stat().st_size

    # .bss is NOLOAD, so it is not in the flat binary; the loader must be told how
    # much to allocate and zero. Getting this wrong corrupts the device heap.
    nm = run([NM, str(elf)], capture_output=True).stdout
    bss_end = next((int(line.split()[0], 16) for line in nm.splitlines() if line.endswith(" _bss_end")), None)
    if bss_end is None:
        raise RuntimeError(f"{name}: _bss_end not found; refusing to pack with bss_size=0")
    bss_size = max(0, bss_end - (LINK_BASE + bin_size))

    out.mkdir(parents=True, exist_ok=True)
    papp = out / f"{name}.papp"
    run([sys.executable, str(sdk / "tools/pack_papp.py"), str(binary), str(papp), "--entry-offset", "0", "--bss-size", str(bss_size)])

    data = papp.read_bytes()
    header = parse_header(data)
    info = {
        "name": name,
        "title": manifest.get("title", name),
        "version": manifest["version"],
        "description": manifest.get("description", ""),
        "file": papp.name,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "abi": PAPP_ABI_VERSION,
        **header,
        "source": {"repo": source["repo"], "ref": source["ref"], "path": source["path"]},
        "sdk": {"repo": source["repo"], "ref": source["ref"]},
    }
    (out / f"{name}.json").write_text(json.dumps(info, indent=2) + "\n")
    print(f"  {papp.name}: {len(data)} bytes, text={header['text_size']} bss={header['bss_size']}, sha256 {info['sha256'][:16]}", flush=True)
    return info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("apps", nargs="*", help="app names (default: every apps/*/papp.json)")
    parser.add_argument("--out", type=Path, default=ROOT / "dist")
    parser.add_argument("--cache", type=Path, default=ROOT / ".cache")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()

    manifests = [APPS / n / "papp.json" for n in args.apps] if args.apps else sorted(APPS.glob("*/papp.json"))
    if not manifests:
        print("no apps found under apps/", file=sys.stderr)
        return 1
    built = [build_app(m, args.cache.resolve(), args.out.resolve(), args.jobs) for m in manifests]
    (args.out / "build.json").write_text(json.dumps({"apps": built}, indent=2) + "\n")
    print(f"built {len(built)} app(s) into {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
