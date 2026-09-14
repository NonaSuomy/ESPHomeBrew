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

### Store listing (optional)

What a store screen shows before an app is downloaded:

```json
{
  "author": "giltal",
  "category": "Game",
  "license": "GPL-2.0",
  "about": "A longer description for the app's detail screen.",
  "changelog": "0.1.1: sound fixes\n0.1.0: first release",
  "controls": ["D-pad: move", "A: fire", "Start: menu"],
  "upstream": {"project": "PrBoom", "version": "2.5.0", "url": "https://github.com/..."},
  "icon": "icon.png",
  "screenshots": ["screen1.png", "screen2.png"]
}
```

- `author` (up to 60 characters), `category` (30), `license` (60), `about` (2000) and `changelog` (4000) are text; `controls` is 1–20 lines of up to 60 characters; `upstream` names the original project the port comes from (`project` up to 60 characters, optional `version` up to 30 and an `https://` `url`), shown next to the `source` repo and commit the app is built from. The fields follow the Homebrew App Store (hb-app.store) listing.
- `icon` is a PNG in the app's folder, at most 256×256 and 64 KB; `screenshots` lists up to three PNGs of at most 1024×600 and 300 KB. Use your own or freely licensed art, not official game logos.
- Publish store puts all of it, with the icon as base64, the `.papp` size and the data size, into `store.json` and into a sidecar next to each app (`psram_doom-0.1.1.json`, found by swapping `.papp` for `.json`). The icon is also published as `psram_doom-0.1.1.png`, which the web page shows, and screenshots as `psram_doom-0.1.1-screen1.png`, … (listed by URL only, to keep the JSON small). A LAN server or SD folder can carry the same sidecar next to its `.papp` files.

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
- Only the directories named in `source.path`, `groups`, `includes` and `paths` (plus the SDK files) are checked out. `paths` lists extra upstream files a `prebuild` step reads.
- `"files": ["core/**/*.c"]` also finds files in subfolders; each object is named after its path (`core_lv_obj.o`), so same-named files in different folders do not collide.

#### Submodules

A plain fetch does not bring an upstream's git submodules along. `submodules` checks each one out inside the source tree at its own pinned commit:

```json
"submodules": [
  { "path": "micropython", "repo": "https://github.com/micropython/micropython", "ref": "<full commit SHA>" }
]
```

When the upstream tree records the submodule, `ref` must be the commit it pins; the build refuses anything else. Paths in `groups`, `includes` and `paths` that fall inside a submodule become its sparse checkout, and patches may change files in it (patch paths are relative to the source root, e.g. `micropython/py/gc.c`).

#### Prebuild

Some upstreams generate sources before compiling (MicroPython's qstr headers, frozen Python, bindings). `prebuild` lists commands run in the source checkout after the patches:

```json
"prebuild": [["{python}", "{repo}/ports/tulip/gen_tulip.py", "--src", "{src}", "--gen", "{gen}", "--units", "{units}"]]
```

`{src}` is the checkout, `{repo}` this repository, `{gen}` the folder `.papp-gen` inside the checkout (emptied first; compile its files with a group whose `dir` is `.papp-gen`), `{units}` a JSON list of every compile unit with its compiler and flags (for generators that preprocess the sources), `{python}` the Python running the build and `{jobs}` the parallel job count. The build's `SOURCE_DATE_EPOCH` applies, so generated files are reproducible too.

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
