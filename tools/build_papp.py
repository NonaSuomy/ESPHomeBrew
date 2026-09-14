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
import base64
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
AR = "riscv32-esp-elf-ar"

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


SHA = re.compile(r"[0-9a-f]{40}")
SUBMODULE_PATH = re.compile(r"^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$")


def manifest_paths(manifest: dict) -> list[str]:
    """Every source-tree path a recipe names: groups, includes and extra "paths"."""
    paths = list(manifest.get("paths", [])) + list(manifest.get("includes", []))
    for group in manifest.get("groups", []):
        paths += [group["dir"], *group.get("includes", [])]
    return [p for p in paths if not p.startswith(LOCAL_PREFIX) and p not in ("", ".")]


def check_submodules(name: str, manifest: dict) -> list[dict]:
    """Validate "submodules": extra repositories checked out inside the source tree.

    Upstream projects keep dependencies as git submodules, which a plain fetch
    does not bring along. Each entry pins one of them by full commit SHA:
    {"path": "micropython", "repo": "https://github.com/...", "ref": "<SHA>"}.
    """
    subs = manifest.get("submodules", [])
    if not isinstance(subs, list):
        raise ValueError(f"{name}: submodules must be a list")
    seen: set[str] = set()
    for sub in subs:
        path = sub.get("path", "")
        if not SUBMODULE_PATH.match(path) or any(part in (".", "..") for part in path.split("/")):
            raise ValueError(f"{name}: bad submodule path '{path}'")
        if any(path == other or path.startswith(other + "/") or other.startswith(path + "/") for other in seen):
            raise ValueError(f"{name}: submodule '{path}' overlaps another one")
        seen.add(path)
        if not SHA.fullmatch(sub.get("ref", "")):
            raise ValueError(f"{name}: submodule '{path}' needs a full commit SHA as ref")
        if not sub.get("repo", "").startswith(("https://", "file://")):
            raise ValueError(f"{name}: submodule '{path}' needs an https:// repo")
    return subs


def split_sparse(paths: list[str], subs: list[dict]) -> tuple[list[str], dict[str, list[str] | None]]:
    """Share the recipe's paths out between the main checkout and its submodules.

    A path inside a submodule becomes an anchored sparse pattern of that
    submodule. The submodule's own root (typically an include directory)
    adds nothing; a submodule with no paths inside it is checked out whole (None).
    """
    main: list[str] = []
    per_sub: dict[str, list[str]] = {sub["path"]: [] for sub in subs}
    for p in paths:
        owner = next((s["path"] for s in subs if p == s["path"] or p.startswith(s["path"] + "/")), None)
        if owner is None:
            main.append(p)
        elif p != owner:
            per_sub[owner].append("/" + p[len(owner) + 1:])
    return main, {k: (v or None) for k, v in per_sub.items()}


def fetch_submodules(src_root: Path, subs: list[dict], sparse: dict[str, list[str] | None]) -> None:
    """Check out each submodule at its pinned commit inside the source tree.

    When the upstream tree records the submodule (a gitlink), the manifest's
    ref must be that same commit, so a port always builds what upstream pins.
    """
    for sub in subs:
        listed = run(["git", "ls-tree", "HEAD", "--", sub["path"]], cwd=src_root, capture_output=True).stdout.split()
        if len(listed) >= 3 and listed[1] == "commit" and listed[2] != sub["ref"]:
            raise ValueError(f"submodule {sub['path']}: upstream pins {listed[2]}, manifest says {sub['ref']}")
        fetch(sub["repo"], sub["ref"], source_file(src_root, sub["path"]), sparse.get(sub["path"]))


def reset_checkout(root: Path) -> None:
    """Back to the pinned files; keeps fetch()'s cache stamp (and nested submodule checkouts)."""
    run(["git", "checkout", "-q", "--", "."], cwd=root)
    run(["git", "clean", "-q", "-fd", "-e", ".papp-ref"], cwd=root)


# Generated sources (a "prebuild" step's output) live in this folder of the
# source checkout, so recipes can compile them like any other upstream file.
GEN_DIR = ".papp-gen"


def prebuild_args(args: list[str], values: dict[str, str]) -> list[str]:
    """Fill {src}, {repo}, {gen}, {units}, {python} and {jobs} into a prebuild command."""
    out = []
    for arg in args:
        for key, value in values.items():
            arg = arg.replace("{" + key + "}", value)
        out.append(arg)
    return out


def write_units(units: list["Unit"], path: Path) -> None:
    """The compile units as JSON for prebuild steps (sources, compiler, flags)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps([{"src": str(u.src), "obj": u.obj.name, "compiler": u.compiler, "flags": u.flags}
                                for u in units], indent=1))


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

    def __init__(self, src: Path, obj: Path, flags: list[str], compiler: str = CC, archive: str | None = None):
        self.src, self.obj, self.flags, self.compiler = src, obj, flags, compiler
        self.archive = archive  # name of the static library this object goes into, if any


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
    """A group's file names; "*.cpp"-style entries expand in its dir, minus "exclude".

    "**/*.c" also finds files in subfolders; they are named by their path
    relative to the group's dir ("core/lv_obj.c").
    """
    names: list[str] = []
    for entry in group["files"]:
        if any(c in entry for c in "*?["):
            base = source_file(src_root, group["dir"])
            names += sorted(p.relative_to(base).as_posix() for p in base.glob(entry) if p.is_file())
        else:
            names.append(entry)
    excluded = set(group.get("exclude", []))
    unknown = excluded - set(names)
    if unknown:
        raise ValueError(f"{group['dir']}: exclude lists files that are not in the group: {sorted(unknown)}")
    return [n for n in dict.fromkeys(names) if n not in excluded]


def apply_patches(src_root: Path, patches: list[str], submodules: list[str] = ()) -> list[str]:
    """Reset the cached checkout (and its submodules), then apply the port's patches in order.

    Patch paths are relative to the source root, so one patch can also change
    files inside a submodule (micropython/py/...).
    """
    reset_checkout(src_root)
    for sub in submodules:
        reset_checkout(source_file(src_root, sub))
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
        # "archive": link the group as a static library, like upstream CMake
        # libraries: only objects that resolve something are pulled in, and the
        # app's own definitions win over the library's.
        archive = f"lib{group.get('prefix', 'group').strip('_')}.a" if group.get("archive") else None
        for name in group_files(src_root, group):
            src = source_file(src_root, f"{group['dir']}/{name}")
            # A file in a subfolder keeps its folder in the object name (core/lv_obj.c -> core_lv_obj.o).
            stem = str(Path(name).with_suffix("")).replace("\\", "/").replace("/", "_")
            obj = build_dir / (group.get("prefix", "") + stem + ".o")
            if src.suffix in (".cpp", ".cc", ".cxx"):
                units.append(Unit(src, obj, cxxflags + extra, CXX, archive))
            elif src.suffix == ".c":
                units.append(Unit(src, obj, cflags + extra, archive=archive))
            else:
                raise ValueError(f"{name}: not a C or C++ source")
    objs = [u.obj for u in units]
    if len(set(objs)) != len(objs):
        raise ValueError("two sources map to the same object file; give a group a 'prefix'")
    ldflags = list(manifest.get("ldflags", []))
    if manifest.get("newlib"):
        ldflags += [f"-Wl,--wrap={sym}" for sym in NEWLIB_WRAPS] + NEWLIB_LIBS
    return units, ldflags


def run_prebuild(manifest: dict, src_root: Path, build_dir: Path, cache: Path, env: dict, jobs: int = 0) -> None:
    """Run a recipe's "prebuild" commands: code generators that must run before compiling.

    Each command is a list of arguments run in the source checkout, with
    {src} (the checkout), {repo} (this repository), {gen} (the generated-files
    folder, emptied first), {units} (a JSON list of every compile unit with its
    compiler and flags, for generators that preprocess the sources, such as
    MicroPython's qstr extraction), {python} and {jobs} filled in. Generated
    sources are then compiled from "<GEN_DIR>/..." like upstream ones.
    """
    gen = src_root / GEN_DIR
    if gen.exists():
        shutil.rmtree(gen)
    gen.mkdir(parents=True)
    units_json = gen / "units.json"
    units, _ = custom_units(manifest, src_root, build_dir)
    write_units(units, units_json)
    values = {"src": str(src_root), "repo": str(ROOT), "gen": str(gen), "units": str(units_json),
              "python": sys.executable, "jobs": str(jobs or os.cpu_count() or 2)}
    for command in manifest["prebuild"]:
        if not isinstance(command, list) or not command or not all(isinstance(a, str) for a in command):
            raise ValueError("prebuild commands must be non-empty lists of strings")
        args = prebuild_args(command, values)
        print(f"  prebuild: {' '.join(args).replace(str(cache), '<cache>')}", flush=True)
        run(args, cwd=src_root, env=env)


MAX_ICON_BYTES = 64 * 1024
MAX_ICON_SIDE = 256
MAX_SCREENSHOTS = 3
MAX_SCREENSHOT_BYTES = 300 * 1024
MAX_SCREENSHOT_SIZE = (1024, 600)
STORE_TEXT_LIMITS = {"author": 60, "category": 30, "license": 60, "about": 2000, "changelog": 4000}
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def listing_png(name: str, app_dir: Path, rel: object, what: str, max_bytes: int, max_w: int, max_h: int) -> dict:
    """A PNG from the app's folder, checked for size and dimensions, as base64."""
    path = (app_dir / str(rel)).resolve()
    if app_dir.resolve() not in path.parents or not path.is_file():
        raise ValueError(f"{name}: {what} must be a file inside apps/{name}/")
    png = path.read_bytes()
    if len(png) > max_bytes or png[:8] != PNG_SIGNATURE or png[12:16] != b"IHDR":
        raise ValueError(f"{name}: {what} must be a PNG of at most {max_bytes // 1024} KB")
    width, height = struct.unpack(">II", png[16:24])
    if not (0 < width <= max_w and 0 < height <= max_h):
        raise ValueError(f"{name}: {what} is {width}x{height}; at most {max_w}x{max_h}")
    return {"type": "image/png", "width": width, "height": height, "base64": base64.b64encode(png).decode("ascii")}


UPSTREAM_LIMITS = {"project": 60, "version": 30, "url": 200}


def upstream_info(name: str, upstream: object) -> dict:
    """The original project an app is ported from: {"project", "version", "url"},
    only "project" required."""
    if not isinstance(upstream, dict) or "project" not in upstream or set(upstream) - set(UPSTREAM_LIMITS):
        raise ValueError(f"{name}: upstream must be an object with project and optional version and url")
    info = {}
    for key, limit in UPSTREAM_LIMITS.items():
        if key in upstream:
            value = upstream[key]
            if not isinstance(value, str) or not value.strip() or len(value) > limit:
                raise ValueError(f"{name}: upstream {key} must be text of 1 to {limit} characters")
            info[key] = value.strip()
    if "url" in info and not info["url"].startswith("https://"):
        raise ValueError(f"{name}: upstream url must start with https://")
    return info


# The canvas sizes an app may list: even, from 320x240 up (the loader's
# papp_canvas.h limits).
CANVAS_SIZE = re.compile(r"(\d{1,4})x(\d{1,4})")
CANVAS_MIN = (320, 240)
CANVAS_MAX_SIDE = 4096
MAX_CANVAS_SIZES = 8


def canvas_size(name: str, size: object) -> str:
    """One canvas size, "WIDTHxHEIGHT": even, from 320x240 up."""
    match = CANVAS_SIZE.fullmatch(size) if isinstance(size, str) else None
    width, height = (int(match[1]), int(match[2])) if match else (0, 0)
    if (not match or width < CANVAS_MIN[0] or height < CANVAS_MIN[1] or width > CANVAS_MAX_SIDE
            or height > CANVAS_MAX_SIDE or width % 2 or height % 2):
        raise ValueError(f"{name}: canvas size {size!r} must be WIDTHxHEIGHT, even, at least "
                         f"{CANVAS_MIN[0]}x{CANVAS_MIN[1]}")
    return f"{width}x{height}"


def canvas_info(name: str, canvas: object) -> bool | list[str]:
    """The listing's "canvas": the app picks its canvas with display_get_size /
    display_set_canvas (psram_app.h), so the store offers it a Screen setting.

    true: it draws at whatever size display_get_size offers. A list names the
    sizes it can draw ("1024x600", "800x480", ...), and only those are offered.
    Apps without it keep the 800x480 canvas. (An object with a recommended
    size goes through canvas_listing.)
    """
    if canvas is True:
        return True
    if not isinstance(canvas, list) or not 1 <= len(canvas) <= MAX_CANVAS_SIZES:
        raise ValueError(f"{name}: canvas must be true, a list of 1 to {MAX_CANVAS_SIZES} sizes such as "
                         "\"1024x600\", or {\"sizes\": [...], \"recommended\": \"800x480\"}")
    sizes: list[str] = []
    for size in canvas:
        text = canvas_size(name, size)
        if text in sizes:
            raise ValueError(f"{name}: canvas lists {text} twice")
        sizes.append(text)
    return sizes


def canvas_listing(name: str, canvas: object) -> dict:
    """The listing fields for papp.json's "canvas".

    Besides true and a list of sizes, "canvas" may be an object that also
    names the size the store should use when the user has not chosen one:
    {"sizes": ["800x480", "1024x600"], "recommended": "800x480"}, or
    {"recommended": "800x480"} for an app that takes any size. The listing
    keeps "canvas" as true or the list, which every loader reads, and carries
    the recommendation as "canvas_recommended".
    """
    if not isinstance(canvas, dict):
        return {"canvas": canvas_info(name, canvas)}
    if "recommended" not in canvas or set(canvas) - {"sizes", "recommended"}:
        raise ValueError(f"{name}: a canvas object has \"recommended\" and optional \"sizes\", nothing else")
    sizes = canvas_info(name, canvas["sizes"]) if "sizes" in canvas else True
    if sizes is True and "sizes" in canvas:
        raise ValueError(f"{name}: canvas sizes must be a list; leave it out for any size")
    recommended = canvas_size(name, canvas["recommended"])
    if sizes is not True and recommended not in sizes:
        raise ValueError(f"{name}: the recommended canvas {recommended} is not one of its sizes {sizes}")
    return {"canvas": sizes, "canvas_recommended": recommended}


# What an app needs on the card, shown with a tick or a cross on its store
# page: /sd/<path>, a folder ending in '/', or a pattern ('*', '?') in the
# last part. The loader looks for it under every data root.
MAX_REQUIRES = 24
REQUIRES_NOTE_LIMIT = 80
REQUIRES_SEGMENT = re.compile(r"[^/\\:\x00-\x1f\x7f]+")


def requires_info(name: str, requires: object) -> list[dict]:
    """The listing's "requires": [{"path": "/sd/...", "note": "...", "optional": true}]."""
    if not isinstance(requires, list) or not 1 <= len(requires) <= MAX_REQUIRES:
        raise ValueError(f"{name}: requires must list 1 to {MAX_REQUIRES} files or folders")
    out: list[dict] = []
    for item in requires:
        if not isinstance(item, dict) or "path" not in item or set(item) - {"path", "note", "optional"}:
            raise ValueError(f"{name}: each requires entry is {{\"path\", optional \"note\" and \"optional\"}}")
        path = item["path"]
        if not isinstance(path, str) or not path.startswith("/sd/") or len(path) > 200 or not path.isascii():
            raise ValueError(f"{name}: requires path {path!r} must be an ASCII /sd/... path of up to 200 characters")
        parts = path[len("/sd/"):].removesuffix("/").split("/")
        for i, part in enumerate(parts):
            if (not REQUIRES_SEGMENT.fullmatch(part) or part in (".", "..")
                    or (i < len(parts) - 1 and any(c in part for c in "*?"))):
                raise ValueError(f"{name}: requires path {path!r}: bad part {part!r} (patterns only in the last part)")
        entry = {"path": path}
        if "note" in item:
            note = item["note"]
            if not isinstance(note, str) or not note.strip() or len(note) > REQUIRES_NOTE_LIMIT:
                raise ValueError(f"{name}: requires note must be text of 1 to {REQUIRES_NOTE_LIMIT} characters")
            entry["note"] = note.strip()
        if "optional" in item:
            if not isinstance(item["optional"], bool):
                raise ValueError(f"{name}: requires optional must be true or false")
            if item["optional"]:
                entry["optional"] = True
        if any(e["path"] == path for e in out):
            raise ValueError(f"{name}: requires lists {path} twice")
        out.append(entry)
    return out


def store_info(name: str, manifest: dict, app_dir: Path) -> dict:
    """The store listing extras from papp.json: author, category, license, about,
    changelog, controls, upstream, canvas (and canvas_recommended), requires,
    icon and screenshots.

    All optional. Images are PNGs in the app's folder: an icon of at most
    256x256 and 64 KB, and up to three screenshots of at most 1024x600 and
    300 KB, carried as base64 until make_catalog publishes them.
    """
    info: dict = {}
    for key, limit in STORE_TEXT_LIMITS.items():
        if key in manifest:
            value = manifest[key]
            if not isinstance(value, str) or not value.strip() or len(value) > limit:
                raise ValueError(f"{name}: {key} must be text of 1 to {limit} characters")
            info[key] = value.strip()
    if "controls" in manifest:
        controls = manifest["controls"]
        if (not isinstance(controls, list) or not 1 <= len(controls) <= 20
                or not all(isinstance(c, str) and 0 < len(c.strip()) <= 60 for c in controls)):
            raise ValueError(f"{name}: controls must be 1 to 20 lines of up to 60 characters")
        info["controls"] = [c.strip() for c in controls]
    if "upstream" in manifest:
        info["upstream"] = upstream_info(name, manifest["upstream"])
    if "canvas" in manifest:
        info.update(canvas_listing(name, manifest["canvas"]))
    if "requires" in manifest:
        info["requires"] = requires_info(name, manifest["requires"])
    if "icon" in manifest:
        info["icon"] = listing_png(name, app_dir, manifest["icon"], "icon", MAX_ICON_BYTES, MAX_ICON_SIDE, MAX_ICON_SIDE)
    if "screenshots" in manifest:
        shots = manifest["screenshots"]
        if not isinstance(shots, list) or not 1 <= len(shots) <= MAX_SCREENSHOTS:
            raise ValueError(f"{name}: screenshots must list 1 to {MAX_SCREENSHOTS} PNG files")
        info["screenshots"] = [listing_png(name, app_dir, shot, "screenshot", MAX_SCREENSHOT_BYTES, *MAX_SCREENSHOT_SIZE)
                               for shot in shots]
    return info


def build_app(manifest_path: Path, cache: Path, out: Path, jobs: int) -> dict:
    manifest = json.loads(manifest_path.read_text())
    name = manifest["name"]
    if manifest_path.parent.name != name:
        raise ValueError(f"{manifest_path}: name '{name}' must match its folder")
    print(f"=== {name} ===", flush=True)
    data_files = check_data(name, manifest.get("data"))
    listing = store_info(name, manifest, manifest_path.parent)

    source = manifest["source"]
    build = manifest["build"]
    submodules = check_submodules(name, manifest)
    sparse = [source["path"], *SDK_PATHS]
    if build == "custom":
        sparse += manifest_paths(manifest)
    # Only upstream directories are checked out; "." as an include root,
    # generated files and this repository's own (local:) files need nothing fetched.
    sparse = [p for p in sparse if not p.startswith(LOCAL_PREFIX) and p not in ("", ".")
              and p.split("/")[0] != GEN_DIR]
    sparse, sub_sparse = split_sparse(sparse, submodules)
    sdk = src_root = fetch(source["repo"], source["ref"], cache / f"src-{name}-{source['ref'][:12]}", sparse)
    fetch_submodules(src_root, submodules, sub_sparse)
    app_dir = src_root / source["path"]
    sub_paths = [s["path"] for s in submodules]
    patches = apply_patches(src_root, manifest["patches"], sub_paths) if manifest.get("patches") else []
    linker_script = (source_file(src_root, manifest["linker_script"]) if manifest.get("linker_script")
                     else sdk / "tools/psram_app.ld")

    cflags = CFLAGS + [f"-I{sdk / SDK_INCLUDE}", f"-I{app_dir}"]
    build_dir = cache / "build" / name
    if build_dir.exists():
        shutil.rmtree(build_dir)
    units: list[Unit] = []
    ldflags = list(LDFLAGS)
    linker = CC

    env, epoch = reproducible_env(src_root)
    if build == "custom" and manifest.get("prebuild"):
        run_prebuild(manifest, src_root, build_dir, cache, env, jobs)
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
    for unit in units:
        unit.flags = unit.flags + [f"-ffile-prefix-map={cache}=/papp-src"]
    print(f"  compiling {len(units)} files (SOURCE_DATE_EPOCH={epoch})", flush=True)
    failures: list[str] = []

    def attempt(unit: Unit) -> None:
        try:
            compile_one(unit, env)
        except RuntimeError as error:
            failures.append(str(error))

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(attempt, units))
    if failures:
        # Report every failing file at once (a port fixes them in batches), with
        # just the error lines so the log stays readable.
        for failure in sorted(failures)[:40]:
            head, _, rest = failure.partition("\n")
            errors = [line for line in rest.splitlines() if " error:" in line or "fatal error" in line]
            print(f"  {head}", flush=True)
            for line in errors[:8]:
                print(f"    {line.replace(str(cache), '')}", flush=True)
        raise RuntimeError(f"{name}: {len(failures)} of {len(units)} files failed to compile")

    elf = build_dir / f"{name}.elf"
    archives: dict[str, list[str]] = {}
    for unit in units:
        if unit.archive:
            archives.setdefault(unit.archive, []).append(str(unit.obj))
    for library, objects in archives.items():
        run([AR, "rcs", str(build_dir / library), *objects])
    objects = [str(u.obj) for u in units if not u.archive] + [str(build_dir / library) for library in archives]
    link = [linker, *ldflags, f"-T{linker_script}", "-o", str(elf), *objects, *link_tail]
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
    if submodules:
        info["submodules"] = [{"path": s["path"], "repo": s["repo"], "ref": s["ref"]} for s in submodules]
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
    info.update(listing)
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
