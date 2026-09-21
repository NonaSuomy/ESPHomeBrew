#!/usr/bin/env bash
# fetch_arnold.sh
#
# Downloads the unmodified Arnold GCW0 source files needed for the GX4000 core.
# Run from the  src/gx4000/arnold/  directory:
#
#   cd src/gx4000/arnold
#   bash fetch_arnold.sh
#
# Files render.c and render5.c are NOT downloaded here – those are our
# ESP32-modified versions already present in this directory.
#
# Source: https://github.com/gameblabla/arnold_gcw0  (GPL-2.0)

BASE="https://raw.githubusercontent.com/gameblabla/arnold_gcw0/master"

CPC=(
  cpcglob.h
  cpcdefs.h
  cpc.h
  cpc.c
  arnold.h
  arnold.c
  asic.h
  asic.c
  crtc.h
  crtc.c
  garray.h
  garray.c
  psg.h
  psg.c
  memory.h
  memory.c
  pal.h
  pal.c
  audioevent.h
  audioevent.c
  ramrom.h
  ramrom.c
  render.h
  device.h
  host.h
  cassette.h
  cheatdb.h
  endian.h
  fdc.h
  fdd.h
  fdi.h
  printer.h
  psgplay.h
  snapshot.h
  vortex.h
  westpha.h
  magnum.h
  amsdos.h
  multface.h
  kempston.h
  dumpym.h
  riff.c
  riff.h
  spo256.h
  yiq.h
  z8536.h
  packedimage.h
  headers.h
)

# Download CPC core files (cpc/ directory in repo)
for f in "${CPC[@]}"; do
  if [[ -f "$f" ]]; then
    echo "  SKIP  $f (already present)"
    continue
  fi
  echo "  GET   $f"
  curl -fsSL "${BASE}/cpc/${f}" -o "${f}" || { echo "FAILED: $f"; exit 1; }
done

# Additional subfolders used by cpc.h includes
mkdir -p debugger diskimage

for f in gdebug.h; do
  if [[ -f "debugger/$f" ]]; then
    echo "  SKIP  debugger/$f"
    continue
  fi
  echo "  GET   debugger/$f"
  curl -fsSL "${BASE}/cpc/debugger/${f}" -o "debugger/${f}" || { echo "FAILED: debugger/$f"; exit 1; }
done

for f in diskimg.h; do
  if [[ -f "diskimage/$f" ]]; then
    echo "  SKIP  diskimage/$f"
    continue
  fi
  echo "  GET   diskimage/$f"
  curl -fsSL "${BASE}/cpc/diskimage/${f}" -o "diskimage/${f}" || { echo "FAILED: diskimage/$f"; exit 1; }
done

# Z80 CPU core
mkdir -p z80
Z80_FILES=(z80.h z80.c z80gen.c z80funcs.h z80funcs2.h z80macros.h z80tables.h z80daa.h)
for f in "${Z80_FILES[@]}"; do
  if [[ -f "z80/$f" ]]; then
    echo "  SKIP  z80/$f"
    continue
  fi
  echo "  GET   z80/$f"
  curl -fsSL "${BASE}/cpc/z80/${f}" -o "z80/${f}" || { echo "FAILED: z80/$f"; exit 1; }
done

echo ""
echo "Done. Files downloaded to $(pwd)"
echo ""
echo "NOTE: render.c and render5.c are NOT re-downloaded."
echo "      The versions in this directory are the ESP32 line-rendering editions."
