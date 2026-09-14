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
  "canvas": true,
  "icon": "icon.png",
  "screenshots": ["screen1.png", "screen2.png"]
}
```

- `author` (up to 60 characters), `category` (30), `license` (60), `about` (2000) and `changelog` (4000) are text; `controls` is 1–20 lines of up to 60 characters; `upstream` names the original project the port comes from (`project` up to 60 characters, optional `version` up to 30 and an `https://` `url`), shown next to the `source` repo and commit the app is built from. The fields follow the Homebrew App Store (hb-app.store) listing.
- `canvas` says the app chooses its canvas size ([below](#canvas-size-for-app-authors)), which gives it a **Screen** setting on its store page: `true` when it draws at whatever size `display_get_size` offers, or a list of the sizes it can draw, such as `["1024x600", "800x480"]` (up to 8, each even and at least 320x240; only those that fit the panel are offered). Leave it out for apps that keep 800×480; the setting would do nothing for them.
- To recommend one size, make `canvas` an object: `{"sizes": ["800x480", "1024x600"], "recommended": "800x480"}`, or `{"recommended": "800x480"}` for an app that takes any size. The recommended size must be one of `sizes`. The Screen setting then shows it with "(Recommended)", and an app with no saved setting gets it from `display_get_size` instead of the device's default canvas (when it fits the panel). The published listing keeps `canvas` as `true` or the list, which older loaders read, and adds `"canvas_recommended": "800x480"`.
- `requires` lists what the app needs on the card, including files the store cannot download (commercial game data). The app's store page shows each one with a tick when the loader finds it and a cross when not:

  ```json
  "requires": [
    {"path": "/sd/roms/openlara/DATA/*.PHD", "note": "Tomb Raider 1 PC levels: copy the game's DATA folder"},
    {"path": "/sd/roms/openlara/FMV/", "note": "The game's movies", "optional": true}
  ]
  ```

  - `path` is where the app looks, as it opens it: `/sd/` and a path of up to 200 ASCII characters. A trailing `/` means a folder. The last part may be a pattern with `*` and `?` (case is ignored), which needs at least one match. No `.` or `..` parts, backslashes or colons.
  - `note` (optional, up to 80 characters) says what it is. `optional: true` marks something the app runs without (shown in amber rather than red when missing).
  - Up to 24 entries. The loader looks for each one under every data root (`data_root`, then `data_search`, such as `/sd` and `/usb0`), only when the app's page opens.
  - The files under `data` (below), which the store downloads, count as required too: Publish store adds each one to the listing's `requires` as `{"path": "/sd/<target>", "download": true}`.
- `icon` is a PNG in the app's folder, at most 256×256 and 64 KB; `screenshots` lists up to three PNGs of at most 1024×600 and 300 KB. Use your own or freely licensed art, not official game logos.
- Publish store puts all of it, with the icon as base64, the `.papp` size and the data size, into `store.json` and into a sidecar next to each app (`psram_doom-0.1.1.json`, found by swapping `.papp` for `.json`). The icon is also published as `psram_doom-0.1.1.png`, which the web page shows, and screenshots as `psram_doom-0.1.1-screen1.png`, … (listed by URL only, to keep the JSON small). A LAN server or SD folder can carry the same sidecar next to its `.papp` files.

Sources come from the `source` repository at a pinned commit, and so does the PAPP SDK (`psram_app.h`, `psram_app.ld`, `pack_papp.py`), so an app always builds against the loader ABI of its own tree. They are fetched at build time rather than copied here, because upstream has no license file. Today the apps come from [NonaSuomy/RetroESP32-P4](https://github.com/NonaSuomy/RetroESP32-P4) (`papp-serial-upload`) and [giltal/RetroESP32-P4](https://github.com/giltal/RetroESP32-P4).

### Canvas size (for app authors)

An app draws on a canvas centred on the panel. It starts with 800×480, the size every existing app assumes. Two services at the end of the service table (`psram_app.h`) let it use another size, up to the whole panel (1024×600 on the Elecrow board):

```c
int w = 800, h = 480;
if (svc->display_get_size && svc->display_set_canvas) {   // NULL on older loaders
    svc->display_get_size(&w, &h);          // the Screen setting, else the listing's recommended size, else the device default
    if (svc->display_set_canvas(w, h) != 0) {
        w = 800;                            // refused: still 800x480
        h = 480;
    }
}
uint16_t *fb = svc->display_get_framebuffer();   // w x h RGB565, stride w
```

- Call `display_set_canvas` once, before drawing, from the task that draws. Sizes are even, at least 320×240 and at most the panel; `display_get_size`'s answer always qualifies. A switch clears the framebuffer and the panel to black.
- Everything on the display side then uses the new size: the framebuffer and `display_flush`, `display_clear`, `display_write_frame_rgb565` (one full canvas), `display_write_rect`, `display_write_frame_custom` and `display_emu_flush` (the scaled frame centred in the canvas), `touch_read` (canvas coordinates) and screenshots.
- An app with fixed sizes (a build-time resolution, say) picks the largest of its own sizes that fits in what `display_get_size` returns, or simply asks for the one size it has, and lists them under `canvas` in `papp.json`. An app built only for 1024×600 can call `display_set_canvas(1024, 600)` directly and draw scaled down (for example with `display_write_frame_custom`) if that fails.
- On a canvas as wide as the panel the loader's close button is not drawn: taps in the canvas's top-right 58×58 pixels (plus a small margin) reach the app, and only a touch held there for 2 s closes it. Offer your own way out too.
- Apps in this repository's `ports/` include `esphome/components/papp_loader/psram_app.h` and get the services from it. An app built against an older SDK header (such as RetroESP32-P4's) needs the two fields added after `net_resolve`, in the same order, or a newer header.

### MIDI and multi-touch (for app authors)

Three more services follow the canvas ones (NULL on older loaders, so null-check them):

```c
uint8_t buf[64];
int n = svc->midi_read ? svc->midi_read(buf, sizeof buf) : 0;  // plain MIDI bytes: 90 3C 64 = note on, middle C
static const uint8_t note_off[] = {0x80, 0x3C, 0x00};
if (svc->midi_write) svc->midi_write(note_off, sizeof note_off); // whole messages, SysEx included

papp_touch_point_t fingers[5];
int count = svc->touch_read_points ? svc->touch_read_points(fingers, 5) : 0;
```

- MIDI comes from a class-compliant USB-MIDI cable or keyboard through the `usb_midi` component, when the loader has `usb_midi_id:` set. Bytes that arrived before the app started are dropped. `midi_write` returns -1 when no device is plugged in.
- `touch_read_points` gives every finger on the panel (the GT911 reports up to 5), first finger first, in the same canvas coordinates as `touch_read`, each with an `id` that stays the same while that finger stays down.

### TLS / HTTPS connections (for app authors)

The loader makes TLS client connections for apps, so an app can speak HTTPS without carrying its own mbedTLS. Five services follow `touch_read_points` (NULL on older loaders):

```c
if (!svc->net_tls_connect) { /* older loader: no TLS */ }
int h = svc->net_tls_connect("example.com", 443);   // returns at once
while (h >= 0 && svc->net_tls_status(h) == 0)       // 0: still connecting
    svc->delay_ms(10);
if (h < 0 || svc->net_tls_status(h) < 0) { /* failed: see the device log */ }

static const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
for (int off = 0; off < (int)sizeof req - 1; ) {
    int n = svc->net_tls_send(h, req + off, sizeof req - 1 - off);
    if (n < 0) break;                               // 0: would block, send the same bytes again
    if (n == 0) svc->delay_ms(1); else off += n;
}
char buf[1024];
for (;;) {
    int n = svc->net_tls_recv(h, buf, sizeof buf);  // 0: nothing yet, -1: closed or failed
    if (n < 0) break;
    if (n == 0) { svc->delay_ms(5); continue; }
    /* use n bytes */
}
svc->net_tls_close(h);
```

- The server's certificate is checked against the firmware's CA bundle and must carry the name you pass, which is also sent as SNI. A server the bundle cannot vouch for (a self-signed certificate, a LAN address) fails; there is no way to skip the check. The loader logs why a connection failed (name not found, the esp-tls error and the certificate flags).
- Name lookup, the TCP connect and the handshake run on a loader task, so no call blocks the app (each step gives up after about 15 s). Once `net_tls_status` says 1, `net_tls_send` and `net_tls_recv` return at once, like `net_tcp_send` / `net_tcp_recv` but with 0 for "would block" / "nothing yet" and -1 for the end of the stream; afterwards `net_tls_status` tells a clean close (1) from an error (-1).
- After `net_tls_send` returns 0, call it again with the same bytes (mbedTLS may already hold them).
- `net_poll` also takes a TLS handle (readable, writable, failed), so an app can wait on TLS and plain sockets the same way.
- At most 4 sessions at once, connecting ones included. Each holds about 25 KB of the loader's internal RAM while open (mbedTLS's 16 KB input and 4 KB output buffers and its context), a little more and an 8 KB task stack during the handshake. Close sessions you are done with; the loader closes the rest when the app exits.
- Use a handle from one task at a time.

### App hand-off (for app authors)

An app can hand the device to another app and get it back afterwards. NetSurf, for example, opens a video link in the video player, which returns to the page when it quits. Three services follow `net_tls_close` (NULL on older loaders):

```c
// The caller: what it wants back, then the app to open and its argument.
if (svc->app_open) {
    if (svc->app_set_resume_arg) svc->app_set_resume_arg("http://example.com/page.html");
    if (svc->app_open("psram_video", "http://example.com/clip.mp4", 1) == 0) {
        /* accepted: the loader now asks this app to quit; quit */
    }
}

// Any app, at start: the argument it was launched with ("" from the store).
char arg[PAPP_APP_ARG_MAX];
int len = svc->app_get_arg ? svc->app_get_arg(arg, sizeof arg) : 0;   // 0: none
```

- `app_open(target, arg, return_after)`: `target` is an app's name (`"psram_video"`), the `http(s)://` URL of a `.papp`, or a `/sd/...` path of one. A name is looked up the way the store and library do: the installed copy (`<install_dir>/<name>.papp`), then each library source in the order the YAML lists them, a folder (its `.papp` files under every data search root) or an HTTP catalog page (fetched, which can take a few seconds). A source's `name-<version>.papp` counts too; with several versions the highest wins. It returns 0 when the request is accepted and -1 when the app is unknown, the device is offline (for a URL), `arg` is longer than `PAPP_APP_ARG_MAX - 1` (2047) bytes, the app is already closing or has a hand-off waiting, or `return_after` would make more than two apps wait.
- After 0, the loader closes the caller exactly as its close control does (the app sees the usual quit buttons), so an app that already quits on those needs nothing more; it may also return from `app_entry` at once. Then the target starts, and `app_get_arg` gives it `arg`.
- With `return_after`, the loader starts the caller again when the target quits, however it quits (its own exit, Menu held, the loader's close control), and also when the target fails to load. The caller then gets its resume argument from `app_get_arg`: what it last passed to `app_set_resume_arg`, else its own launch argument. Without `return_after` the target replaces the caller; if another app opened the caller with `return_after`, that app still comes back after the target.
- At most two apps wait to be returned to. Opening an app without coming back always works (it replaces the running one). A launch from the menu, a button or a remote command gets an empty argument and forgets any apps waiting.
- Guard against loops: an app that opens another as soon as it starts with a given argument should not hand that same argument back as its resume argument.

### Folder listing (for app authors)

`file_list_dir` follows the hand-off services (NULL on older loaders). It lists a folder on the card, for file pickers:

```c
static char names[16384];
int n = svc->file_list_dir ? svc->file_list_dir("/sd/videos", names, sizeof names) : -1;
for (const char *p = names; n > 0; n--, p += strlen(p) + 1) {
    /* p is one entry; a folder ends in '/' */
}
```

It returns how many entries it wrote (-1 when the folder cannot be opened). Each name is followed by a NUL; folders get a trailing `/`. Entries that do not fit in the buffer are left out, `.` and `..` never appear, and the order is the file system's (sort them yourself).

### File management (for app authors)

Four services follow `file_list_dir` (NULL on older loaders), for apps that manage files on the card, such as an archive extractor:

```c
papp_file_stat_t st;
if (svc->file_mkdir && svc->file_stat && svc->file_rename && svc->file_remove) {
    svc->file_mkdir("/sd/roms/game/levels");               // and any missing parents
    if (svc->file_stat("/sd/roms/game/levels/1.dat", &st) == 0 && !st.is_dir)
        svc->log_printf("%llu bytes\n", (unsigned long long)st.size);
    svc->file_rename("/sd/roms/game/levels/1.tmp", "/sd/roms/game/levels/1.dat");
    svc->file_remove("/sd/roms/game/levels/1.tmp");         // a file, or an empty folder
}
```

- Each returns 0 on success and -1 on an error.
- Paths are absolute, like `file_open`'s: `/sd/...` or a folder under one of the loader's data roots (`/usb0/...`). Anything else is refused, and so is a path with a `.` or `..` part, a backslash or a colon. A trailing `/` is ignored.
- Unlike `file_open`'s read fallback, no other root is tried: each call acts on exactly the path given.
- `file_mkdir` creates the folder and any missing parents; 0 also when it already exists, -1 when a file is in the way.
- `file_remove` deletes a file, or a folder only when it is empty. A root itself (`/sd`) is never removed.
- `file_rename` renames or moves; it never replaces an existing file (-1), and cannot move between two roots (`/sd` to `/usb0`): copy those.
- `file_stat` fills a `papp_file_stat_t` (`size`, `is_dir`, and `mtime` in seconds since 1970 where the file system keeps one, else 0); -1 when the path does not exist.

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

`path` is the file in `repo` at `ref`. `target` is where it goes under the device's data root. `size` and `sha256` pin the exact bytes: CI refuses anything else. Only list files that may be redistributed. The store page lists these under the app's required files, and the loader remembers each one it downloads, so **Uninstall** can offer to delete them ([esphome-store.md](esphome-store.md#store-view-esphomebrew)).

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
