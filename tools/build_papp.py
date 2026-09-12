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
import re
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


DATA_TARGET = re.compile(r"^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$")


def check_data(name: str, data: dict | None) -> dict | None:
    """Validate an app's "data" block: files the app needs on the card.

    Each file is pinned by repository commit, size and sha256; the store only
    publishes exactly those bytes. `target` is the path under the device's
    data root (for example roms/doom/doom1.wad -> /sd/roms/doom/doom1.wad).
    """
    if data is None:
        return None
    if not re.fullmatch(r"[0-9a-f]{40}", data.get("ref", "")):
        raise ValueError(f"{name}: data.ref must be a full commit SHA")
    if not data.get("repo", "").startswith("https://github.com/"):
        raise ValueError(f"{name}: data.repo must be a https://github.com/ repository")
    if not data.get("license"):
        raise ValueError(f"{name}: data.license must say why the files may be redistributed")
    files, targets = [], set()
    for f in data.get("files", []):
        target = f.get("target", "")
        if not DATA_TARGET.match(target) or any(part in (".", "..") for part in target.split("/")):
            raise ValueError(f"{name}: bad data target '{target}'")
        if target.lower() in targets:
            raise ValueError(f"{name}: data target '{target}' listed twice")
        targets.add(target.lower())
        if not isinstance(f.get("size"), int) or f["size"] <= 0:
            raise ValueError(f"{name}: {target}: size must be a positive integer")
        if not re.fullmatch(r"[0-9a-f]{64}", f.get("sha256", "")):
            raise ValueError(f"{name}: {target}: sha256 must be 64 hex digits")
        if not f.get("path") or f["path"].startswith("/") or ".." in f["path"].split("/"):
            raise ValueError(f"{name}: {target}: bad source path")
        files.append({k: f[k] for k in ("path", "target", "size", "sha256")})
    if not files:
        raise ValueError(f"{name}: data.files is empty")
    return {"repo": data["repo"], "ref": data["ref"], "license": data["license"], "files": files}


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


def compile_one(unit: Unit, env: dict | None = None) -> None:
    unit.obj.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([unit.compiler, *unit.flags, "-c", "-o", str(unit.obj), str(unit.src)],
                            capture_output=True, text=True, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"compile failed: {unit.src}\n{result.stderr}")


def reproducible_env(src_root: Path) -> tuple[dict, int]:
    """Compiler environment that makes __DATE__/__TIME__ the source commit's time.

    Some ports print their build time (WinQuake: __TIME__ __DATE__; PrBoom and
    Duke3D: __DATE__). Without this every CI run produces a different binary,
    and Publish store refuses a changed binary under an already released version.
    """
    epoch = int(run(["git", "log", "-1", "--format=%ct", "HEAD"], cwd=src_root, capture_output=True).stdout.strip())
    return {**os.environ, "SOURCE_DATE_EPOCH": str(epoch)}, epoch


LOCAL_PREFIX = "local:"


def source_file(root: Path, rel: str) -> Path:
    """Resolve a manifest path inside the source checkout, refusing to leave it.

    A path starting with "local:" is inside this repository instead (port glue
    code, patches, a linker script), so a port can keep its own files here while
    the upstream source is fetched at a pinned commit.
    """
    if rel.startswith(LOCAL_PREFIX):
        root, rel = ROOT, rel[len(LOCAL_PREFIX):]
    root = root.resolve()
    path = (root / rel).resolve()
    if path != root and root not in path.parents:
        raise ValueError(f"path '{rel}' leaves the {'repository' if root == ROOT.resolve() else 'source checkout'}")
    return path


def group_files(src_root: Path, group: dict) -> list[str]:
    """A group's file names; "*.cpp"-style entries expand in its dir, minus "exclude"."""
    names: list[str] = []
    for entry in group["files"]:
        if any(c in entry for c in "*?["):
            base = source_file(src_root, group["dir"])
            names += sorted(p.name for p in base.glob(entry) if p.is_file())
        else:
            names.append(entry)
    excluded = set(group.get("exclude", []))
    unknown = excluded - set(names)
    if unknown:
        raise ValueError(f"{group['dir']}: exclude lists files that are not in the group: {sorted(unknown)}")
    return [n for n in dict.fromkeys(names) if n not in excluded]


def apply_patches(src_root: Path, patches: list[str]) -> list[str]:
    """Reset the cached checkout, then apply the port's patches in order."""
    run(["git", "checkout", "-q", "--", "."], cwd=src_root)
    run(["git", "clean", "-q", "-fd"], cwd=src_root)
    applied: list[str] = []
    for pattern in patches:
        prefix = LOCAL_PREFIX if pattern.startswith(LOCAL_PREFIX) else ""
        directory, _, name = pattern[len(prefix):].rpartition("/")
        matches = sorted(source_file(src_root, prefix + (directory or ".")).glob(name))
        if not matches:
            raise ValueError(f"no patch matches '{pattern}'")
        for patch in matches:
            run(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=src_root)
            applied.append(patch.name)
    return applied


def pack_papp(binary: bytes, bss_size: int) -> bytes:
    """[32-byte PAPP header][flat .text+.rodata+.data], as RetroESP32-P4's pack_papp.py writes it."""
    if not binary:
        raise RuntimeError("empty binary")
    return PAPP_HEADER.pack(PAPP_MAGIC, PAPP_ABI_VERSION, 0, len(binary), 0, bss_size, 0, 0) + binary


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
        for name in group_files(src_root, group):
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
    data_files = check_data(name, manifest.get("data"))

    source = manifest["source"]
    build = manifest["build"]
    sparse = [source["path"], *SDK_PATHS]
    if build == "custom":
        sparse += [g["dir"] for g in manifest["groups"]] + manifest.get("includes", [])
        for group in manifest["groups"]:
            sparse += group.get("includes", [])
    # Only upstream directories are checked out; "." as an include root and
    # this repository's own (local:) files need nothing fetched.
    sparse = [p for p in sparse if not p.startswith(LOCAL_PREFIX) and p not in ("", ".")]
    sdk = src_root = fetch(source["repo"], source["ref"], cache / f"src-{name}-{source['ref'][:12]}", sparse)
    app_dir = src_root / source["path"]
    patches = apply_patches(src_root, manifest["patches"]) if manifest.get("patches") else []
    linker_script = (source_file(src_root, manifest["linker_script"]) if manifest.get("linker_script")
                     else sdk / "tools/psram_app.ld")

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
    # Same bytes on every machine and run: pinned timestamps, and the local
    # checkout path (which __FILE__ would embed) mapped to a fixed name.
    env, epoch = reproducible_env(src_root)
    for unit in units:
        unit.flags = unit.flags + [f"-ffile-prefix-map={cache}=/papp-src"]
    print(f"  compiling {len(units)} files (SOURCE_DATE_EPOCH={epoch})", flush=True)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(lambda unit: compile_one(unit, env), units))

    elf = build_dir / f"{name}.elf"
    link = [linker, *ldflags, f"-T{linker_script}", "-o", str(elf), *[str(u.obj) for u in units], *link_tail]
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
    papp.write_bytes(pack_papp(binary.read_bytes(), bss_size))

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
    if patches:
        info["patches"] = patches
    if manifest.get("linker_script"):
        info["linker_script"] = manifest["linker_script"]
    # Work in progress: built and kept as a CI artifact, but not released or
    # listed in the store until "publish" is dropped.
    if manifest.get("publish", True) is False:
        info["publish"] = False
    if data_files:
        info["data"] = data_files
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
