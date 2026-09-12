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
- `build` is one of:
  - `lvgl`: compiles LVGL's `src/` except `src/drivers`, plus the app's `*.c`, and links `libgcc`.
  - `plain`: the app's `*.c` only.
  - `custom`: an explicit recipe for bigger ports. See below.
- For `lvgl` and `plain`, the app folder must contain its `*.c` files and, for LVGL apps, `lv_conf.h`.

Sources come from the `source` repository at a pinned commit, and so does the PAPP SDK (`psram_app.h`, `psram_app.ld`, `pack_papp.py`), so an app always builds against the loader ABI of its own tree. They are fetched at build time rather than copied here, because upstream has no license file. Today the apps come from [NonaSuomy/RetroESP32-P4](https://github.com/NonaSuomy/RetroESP32-P4) (`papp-serial-upload`) and [giltal/RetroESP32-P4](https://github.com/giltal/RetroESP32-P4).

### Custom recipes

`custom` mirrors the upstream `tools/build_<game>_papp.ps1` scripts. Every path is relative to the source checkout, and none may leave it. Example (trimmed from `apps/psram_quake/papp.json`):

```json
{
  "build": "custom",
  "source": { "repo": "https://github.com/NonaSuomy/RetroESP32-P4", "ref": "<SHA>", "path": "apps/psram_quake" },
  "includes": ["apps/psram_quake/compat", "components/psram_app_loader/include", "components/quake/winquake"],
  "cflags": ["-std=gnu99", "-DPAPP_APP_SIDE=1", "-Os", "-fcommon", "-ffunction-sections", "-fdata-sections"],
  "cxxflags": ["-std=gnu++17", "-fno-exceptions", "-fno-rtti", "-DPAPP_APP_SIDE=1", "-Os"],
  "groups": [
    { "dir": "components/quake/winquake", "files": ["chase.c", "cmd.c"] },
    { "dir": "apps/psram_quake", "files": ["papp_mp3.cpp"], "prefix": "mp3_", "includes": ["apps/psram_quake/third_party/micro-mp3/opencore-mp3dec"] }
  ],
  "ldflags": ["-Wl,--allow-multiple-definition"],
  "newlib": true
}
```

- `-march=rv32imafc_zicsr_zifencei -mabi=ilp32f -mcmodel=medany` are always added. Everything else comes from `cflags`, and from `cxxflags` for `.cpp` files. g++ links if there is any C++.
- `groups` lists source files per directory. `prefix` keeps object names unique when two directories have a file with the same name (the build refuses a collision), and a group's `includes` apply only to its files.
- `newlib: true` links `-lc -lgcc -lm` and wraps `malloc`/`free`/`calloc`/`realloc`, their `_r` variants and the `__retarget_lock_*` functions, so newlib's heap goes through the loader, as the upstream scripts do.
- Only the directories named in `source.path`, `groups` and `includes` (plus the SDK files) are checked out.

### App data

An app that needs files on the card lists them under `data`. Publish store puts them on Pages and the loader downloads them (see [store.md](store.md#app-data-game-files)):

```json
"data": {
  "repo": "https://github.com/NonaSuomy/RetroESP32-P4",
  "ref": "<full commit SHA>",
  "license": "Why these files may be redistributed.",
  "files": [
    { "path": "SDcard/roms/doom/doom1.wad", "target": "roms/doom/doom1.wad", "size": 4196020, "sha256": "1d7d43be…" }
  ]
}
```

`path` is the file in `repo` at `ref`. `target` is where it goes under the device's data root. `size` and `sha256` pin the exact bytes: CI refuses anything else. Only list files that may be redistributed.

## What the build does

`tools/build_papp.py` mirrors upstream `tools/build_lvgl_papp.ps1` (and, for `custom`, the game scripts):

1. Compile with `riscv32-esp-elf-gcc -march=rv32imafc_zicsr_zifencei -mabi=ilp32f -mcmodel=medany -ffreestanding -fno-tree-loop-distribute-patterns -Os -DPAPP_APP_SIDE=1` (for `custom`, with the manifest's flags instead).
   Builds are reproducible: `SOURCE_DATE_EPOCH` is the source commit's time (WinQuake, PrBoom and Duke3D embed `__DATE__`/`__TIME__`), and `-ffile-prefix-map` hides the checkout path. The same commit always gives the same `.papp`, which Publish store relies on.
2. Link with `psram_app.ld` at `0x4A000000`, entry `app_entry`, `--gc-sections --no-relax`.
3. `objcopy -O binary`, then work out `.bss` from the `_bss_end` symbol. The build refuses to pack without it, because a wrong `.bss` size corrupts the device heap.
4. Pack with `pack_papp.py` (32-byte header: magic `PAPP`, ABI 1, entry/text/data/bss sizes).
5. Check the header, and write `<name>.json` (size, sha256, sizes, pinned sources, checked `data` list) plus `dist/build.json`.

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
