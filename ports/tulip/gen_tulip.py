#!/usr/bin/env python3
"""Code generation for the Tulip PAPP (the "prebuild" step of apps/psram_tulip).

MicroPython ports build with make rules (py/mkrules.mk) that generate headers
before compiling; tools/build_papp.py compiles the sources itself, so this
script does the same generation from the list of compile units it is given:

  genhdr/mpversion.h         py/makeversionhdr.py
  lv_mpy.c                   the LVGL MicroPython binding (gen/gen_mpy.py of
                             lv_binding_micropython_tulip, as Tulip's Makefiles)
  genhdr/qstrdefs.generated.h, moduledefs.h, root_pointers.h
                             every unit preprocessed with -DNO_QSTR, then
                             py/makeqstrdefs.py, makeqstrdata.py,
                             makemoduledefs.py and make_root_pointers.py
  frozen_content.c           Tulip's Python (tulip/shared/py), AMY's Python
                             package, asyncio and this port's py/ folder, via
                             tools/makemanifest.py: as bytecode when mpy-cross
                             can be built with a host compiler, else as source
                             (compiled on import).
  sys_tar.c                  Tulip's /sys files (tulip/fs/tulip: examples,
                             images) as a tar that _boot.py unpacks.

    gen_tulip.py --src <checkout> --gen <out dir> --units <units.json> --jobs N
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PORT = Path(__file__).resolve().parent

# Tulip's frozen Python minus what this port replaces or cannot use.
TULIP_PY_SKIP = {
    "_boot.py",       # ports/tulip/py/_boot.py instead
}


def log(msg: str) -> None:
    print(f"  [gen] {msg}", flush=True)


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, **kwargs)


def unit_for(units: list[dict], suffix: str) -> dict:
    for unit in units:
        if unit["src"].replace("\\", "/").endswith(suffix):
            return unit
    raise SystemExit(f"gen_tulip: no compile unit for {suffix}")


def mp_version(mp: Path) -> str:
    text = (mp / "py/mpconfig.h").read_text()

    def get(name: str) -> int:
        return int(re.search(rf"#define {name}\s+\(?(\d+)\)?", text).group(1))

    version = f"v{get('MICROPY_VERSION_MAJOR')}.{get('MICROPY_VERSION_MINOR')}.{get('MICROPY_VERSION_MICRO')}"
    return version + ("-preview" if get("MICROPY_VERSION_PRERELEASE") else "")


def gen_version(mp: Path, gen: Path, mp_ref: str) -> None:
    env = {**os.environ, "MICROPY_GIT_TAG": mp_version(mp) + "-tulip-papp", "MICROPY_GIT_HASH": mp_ref}
    run([sys.executable, str(mp / "py/makeversionhdr.py"), str(gen / "genhdr/mpversion.h")], env=env)


def gen_lvgl(units: list[dict], lvb: Path, gen: Path) -> None:
    """lv_mpy.c: preprocess lvgl.h with pycparser's fake libc headers, then gen_mpy.py."""
    unit = unit_for(units, "lvgl/src/core/lv_obj.c")
    pp = gen / "lvgl.pp.c"
    fake = lvb / "pycparser/utils/fake_libc_include"
    cmd = [unit["compiler"], "-E", "-DLVGL_PREPROCESS", "-DPYCPARSER", f"-I{fake}", *unit["flags"],
           str(lvb / "lvgl/lvgl.h")]
    with open(pp, "wb") as out:
        run(cmd, stdout=out)
    with open(gen / "lv_mpy.c", "wb") as out:
        run([sys.executable, str(lvb / "gen/gen_mpy.py"), "-M", "lvgl", "-MP", "lv", "-MD", str(gen / "lv_mpy.json"),
             "-E", str(pp), str(lvb / "lvgl/lvgl.h")], stdout=out)
    log(f"lv_mpy.c: {(gen / 'lv_mpy.c').stat().st_size // 1024} KiB")


QSTR_HINT = re.compile(rb"MP_QSTR|MP_REGISTER|MP_ROM_QSTR|#\s*include\s*[\"<]py/")


def wants_qstr(src: Path, gen: Path) -> bool:
    """Only sources that can hold qstrs or module/root-pointer registrations
    (MicroPython itself, Tulip's modules, ulab, the LVGL binding, this port)
    are preprocessed; LVGL's and AMY's own C files have none."""
    if src.suffix not in (".c", ".cpp"):
        return False
    if src.parent == gen:
        return src.name == "lv_mpy.c"
    if "micropython" in src.parts:
        return True
    return bool(QSTR_HINT.search(src.read_bytes()))


def preprocess(unit: dict) -> tuple[bytes, str]:
    result = subprocess.run([unit["compiler"], "-E", "-DNO_QSTR", *unit["flags"], unit["src"]],
                            capture_output=True)
    if result.returncode != 0:
        errors = [line for line in result.stderr.decode(errors="replace").splitlines() if "error" in line]
        return b"", unit["src"] + ":\n    " + "\n    ".join(errors[:6])
    return result.stdout, ""


def gen_qstr(units: list[dict], mp: Path, gen: Path, jobs: int) -> None:
    hdr = gen / "genhdr"
    todo = [u for u in units if wants_qstr(Path(u["src"]), gen)]
    log(f"qstr: preprocessing {len(todo)} of {len(units)} units")
    last = hdr / "qstr.i.last"
    failures = []
    with open(last, "wb") as out, ThreadPoolExecutor(max_workers=jobs) as pool:
        for text, failure in pool.map(preprocess, todo):  # in unit order: same output every build
            out.write(text)
            if failure:
                failures.append(failure)
    if failures:
        # All of them at once: a port fixes include paths in batches.
        raise SystemExit("gen_tulip: preprocessing failed for\n" + "\n".join(failures[:40]))
    tool = str(mp / "py/makeqstrdefs.py")
    for mode, collected in (("qstr", "qstrdefs.collected.h"), ("module", "moduledefs.collected"),
                            ("root_pointer", "root_pointers.collected")):
        run([sys.executable, tool, "split", mode, str(last), str(hdr / mode), "_"])
        run([sys.executable, tool, "cat", mode, "_", str(hdr / mode), str(hdr / collected)])

    # qstrdefs.preprocessed.h: py/qstrdefs.h plus the collected qstrs through
    # the preprocessor with MicroPython's flags (py.mk).
    core = unit_for(units, "micropython/py/qstr.c")
    defs = (mp / "py/qstrdefs.h").read_bytes() + b"\n" + (hdr / "qstrdefs.collected.h").read_bytes()
    quoted = re.sub(rb"^Q\((.*)\)", rb'"Q(\1)"', defs, flags=re.M)
    result = run([core["compiler"], "-E", *core["flags"], "-"], input=quoted, capture_output=True)
    unquoted = re.sub(rb'^"(Q\(.*\))"', rb"\1", result.stdout, flags=re.M)
    (hdr / "qstrdefs.preprocessed.h").write_bytes(unquoted)
    with open(hdr / "qstrdefs.generated.h", "wb") as out:
        run([sys.executable, str(mp / "py/makeqstrdata.py"), str(hdr / "qstrdefs.preprocessed.h")], stdout=out)
    with open(hdr / "moduledefs.h", "wb") as out:
        run([sys.executable, str(mp / "py/makemoduledefs.py"), str(hdr / "moduledefs.collected")], stdout=out)
    with open(hdr / "root_pointers.h", "wb") as out:
        run([sys.executable, str(mp / "py/make_root_pointers.py"), str(hdr / "root_pointers.collected")], stdout=out)
    count = sum(1 for line in (hdr / "qstrdefs.generated.h").read_text().splitlines() if line.startswith("QDEF"))
    log(f"qstr: {count} qstrs")


def build_mpy_cross(mp: Path, jobs: int) -> Path | None:
    """mpy-cross needs a compiler for the build machine; without one, freeze as source."""
    host_cc = shutil.which("cc") or shutil.which("gcc")
    if host_cc is None:
        log("mpy-cross: no host C compiler, freezing Python as source")
        return None
    try:
        run(["make", "-C", str(mp / "mpy-cross"), f"-j{jobs}", "CC=" + host_cc, "V=0"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    except (subprocess.CalledProcessError, OSError) as error:
        detail = getattr(error, "stderr", b"") or b""
        log(f"mpy-cross: build failed ({detail.decode(errors='replace')[-400:]}); freezing Python as source")
        return None
    return mp / "mpy-cross/build/mpy-cross"


def gen_frozen(src: Path, mp: Path, gen: Path, jobs: int) -> None:
    frozen = gen / "frozen"
    frozen.mkdir()
    tulip_py = src / "tulip/shared/py"
    for py in sorted(tulip_py.glob("*.py")):
        if py.name not in TULIP_PY_SKIP:
            shutil.copy(py, frozen / py.name)
    for py in sorted((PORT / "py").glob("*.py")):
        shutil.copy(py, frozen / py.name)  # the port's own (and overrides)
    for package, base in (("amy", src / "amy/amy"), ("asyncio", mp / "extmod/asyncio")):
        (frozen / package).mkdir()
        for py in sorted(base.glob("*.py")):
            shutil.copy(py, frozen / package / py.name)
    mpy_cross = build_mpy_cross(mp, jobs)
    manifest = gen / "manifest.py"
    kind = "freeze" if mpy_cross else "freeze_as_str"
    manifest.write_text(f"{kind}({str(frozen)!r})\n")
    env = {**os.environ, "MICROPY_MPYCROSS": str(mpy_cross or sys.executable)}
    run([sys.executable, str(mp / "tools/makemanifest.py"), "-o", str(gen / "frozen_content.c"), "-b", str(gen),
         "-v", f"MPY_DIR={mp}", "-v", f"PORT_DIR={PORT}", "-v", "MPY_LIB_DIR=", "-v", f"BOARD_DIR={PORT}",
         str(manifest)], env=env, stdout=subprocess.DEVNULL)
    files = sum(1 for _ in frozen.rglob("*.py"))
    log(f"frozen: {files} modules as {'bytecode' if mpy_cross else 'source'}, "
        f"frozen_content.c {(gen / 'frozen_content.c').stat().st_size // 1024} KiB")


# Tulip's /sys: what tulip/fs_create.py puts in the "system" flash partition
# of a hardware Tulip (examples, images, editable copies of the built-in apps).
SYS_FOLDERS = ("app", "ex", "im")
SYS_EXTS = (".txt", ".png", ".py", ".json", ".obj", ".wav", ".mid")
SYS_APP_COPIES = ("drums", "juno6", "voices", "worldui")


def gen_sys(src: Path, gen: Path) -> None:
    """sys_tar.c: Tulip's system files as a tar in the binary; _boot.py
    unpacks it into /sys when it is missing or from another build."""
    import hashlib
    import io
    import tarfile

    home = src / "tulip/fs/tulip"
    files: dict[str, bytes] = {}
    for folder in SYS_FOLDERS:
        for path in sorted((home / folder).rglob("*")):
            if path.is_file() and path.suffix.lower() in SYS_EXTS:
                files[path.relative_to(home).as_posix()] = path.read_bytes()
    for app in SYS_APP_COPIES:
        files[f"ex/my_{app}.py"] = (src / f"tulip/shared/py/{app}.py").read_bytes()
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for name in sorted(files):
            info = tarfile.TarInfo(name)
            info.size = len(files[name])
            info.mtime = epoch
            info.mode = 0o644
            tar.addfile(info, io.BytesIO(files[name]))
    data = buf.getvalue()
    version = hashlib.sha256(data).hexdigest()[:16]
    with open(gen / "sys_tar.c", "w") as out:
        out.write("// Tulip's /sys files (tulip/fs/tulip), made by ports/tulip/gen_tulip.py.\n")
        out.write("#include <stddef.h>\n")
        out.write(f'const char papp_sys_version[] = "{version}";\n')
        out.write(f"const size_t papp_sys_tar_len = {len(data)};\n")
        out.write("const unsigned char papp_sys_tar[] = {\n")
        for i in range(0, len(data), 32):
            out.write(",".join(str(b) for b in data[i:i + 32]) + ",\n")
        out.write("};\n")
    log(f"sys: {len(files)} files, {len(data) // 1024} KiB tar, version {version}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", type=Path, required=True)
    parser.add_argument("--gen", type=Path, required=True)
    parser.add_argument("--units", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()

    src, gen = args.src.resolve(), args.gen.resolve()
    mp, lvb = src / "micropython", src / "lv_binding_micropython_tulip"
    units = json.loads(args.units.read_text())
    (gen / "genhdr").mkdir(parents=True, exist_ok=True)
    mp_ref = subprocess.run(["git", "rev-parse", "--short=10", "HEAD"], cwd=mp, capture_output=True,
                            text=True).stdout.strip()
    log(f"compiler: {units[0]['compiler']}, host cc: {shutil.which('cc') or shutil.which('gcc') or 'none'}")

    for name, step in (("version", lambda: gen_version(mp, gen, mp_ref)),
                       ("lvgl binding", lambda: gen_lvgl(units, lvb, gen)),
                       ("qstr", lambda: gen_qstr(units, mp, gen, args.jobs)),
                       ("frozen", lambda: gen_frozen(src, mp, gen, args.jobs)),
                       ("sys files", lambda: gen_sys(src, gen))):
        start = time.monotonic()
        step()
        log(f"{name}: {time.monotonic() - start:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
