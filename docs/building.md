# Building PAPPs

Every app in this store lives in `apps/<name>/papp.json`. GitHub Actions (`.github/workflows/build-papps.yml`) builds all of them on every push and pull request, and uploads the `.papp` files as the `papps` artifact.

## App manifest

```json
{
  "name": "psram_lvgl",
  "title": "LVGL touch demo",
  "version": "0.1.0",
  "description": "What it does, in one line.",
  "build": "lvgl",
  "source": { "repo": "https://github.com/giltal/RetroESP32-P4", "ref": "<full commit SHA>", "path": "apps/psram_lvgl" },
  "lvgl": { "repo": "https://github.com/lvgl/lvgl", "ref": "<full commit SHA>" }
}
```

- `name` must match the folder name. It becomes `<name>.papp`, and the file name is what the device's store list shows.
- `source` is fetched at the exact commit. Pin a full SHA, not a branch, so a build can be reproduced.
- `build` is `lvgl` (compiles LVGL's `src/` except `src/drivers`, plus the app's `*.c`, and links `libgcc`) or `plain` (the app's `*.c` only).
- The app folder must contain its `*.c` files and, for LVGL apps, `lv_conf.h`.

Sources and the PAPP SDK (`psram_app.h`, `psram_app.ld`, `pack_papp.py`) come from [giltal/RetroESP32-P4](https://github.com/giltal/RetroESP32-P4) at a pinned commit. They are fetched at build time rather than copied here, because upstream has no license file.

## What the build does

`tools/build_papp.py` mirrors upstream `tools/build_lvgl_papp.ps1`:

1. Compile with `riscv32-esp-elf-gcc -march=rv32imafc_zicsr_zifencei -mabi=ilp32f -mcmodel=medany -ffreestanding -fno-tree-loop-distribute-patterns -Os -DPAPP_APP_SIDE=1`.
2. Link with `psram_app.ld` at `0x4A000000`, entry `app_entry`, `--gc-sections --no-relax`.
3. `objcopy -O binary`, then work out `.bss` from the `_bss_end` symbol. The build refuses to pack without it, because a wrong `.bss` size corrupts the device heap.
4. Pack with `pack_papp.py` (32-byte header: magic `PAPP`, ABI 1, entry/text/data/bss sizes).
5. Check the header, and write `<name>.json` (size, sha256, sizes, pinned sources) plus `dist/build.json`.

## Building locally

With ESP-IDF installed (it provides `riscv32-esp-elf-gcc`):

```sh
. $IDF_PATH/export.sh
python3 tools/build_papp.py              # all apps -> dist/
python3 tools/build_papp.py psram_lvgl   # one app
```

Or use the same container CI uses:

```sh
docker run --rm -v "$PWD:/work" -w /work espressif/idf:v6.1 python3 tools/build_papp.py
```
