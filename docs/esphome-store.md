# Adding the PAPP Store page to an ESPHome device

`esphome/store_page.yaml` is a drop-in [package](https://esphome.io/components/packages.html). It adds an LVGL page, **PAPP STORE**, that lists every app in the store and launches the one you tap.

## 1. Load the loader from this repo

Replace the local `papp_loader` source with this repository:

```yaml
external_components:
  - source: github://NonaSuomy/papp-conversions@main
    components: [papp_loader]
```

## 2. Point the loader at the store

```yaml
papp_loader:
  id: papp_runtime
  catalog_url: https://nonasuomy.github.io/papp-conversions/
  lvgl_id: lvgl_component
  data_root: /sd          # where app data (game files) goes; default /sd
  download_data: true     # default; false never downloads app data
  data_search: [/sd, /usb0]   # default; where you may already keep app files
  # ... display_id, touchscreen_id, speaker_id, usb_hidx_id, path as before
```

### Several library sources

Instead of one `catalog_url`, list up to eight named sources. Each is an HTTP(S) catalog page or a folder of `.papp` files:

```yaml
papp_loader:
  catalogs:
    - name: SD / USB
      url: /sd/roms/papp/          # a folder: the .papp files there on every data_search root
    - name: Network
      url: http://192.168.1.10:8000/
    - name: Store
      url: https://nonasuomy.github.io/papp-conversions/
  default_catalog: Store           # optional; the first one otherwise
```

- With more than one source, the library list starts with a `◀ Store ▶` row. Tap it, or press **left/right** on the d-pad or buttons, to switch; up/down still move through the apps.
- A folder source lists the folder on each `data_search` root, so `/sd/roms/papp/` also shows `/usb0/roms/papp/`. Apps from the second and later roots are labelled, for example `doom (usb0)`. Folder sources work offline.
- Switching while a catalog is still loading shows the one you picked once the fetch returns, so pages no longer need a "refresh twice" script.
- From YAML lambdas: `id(papp_runtime)->select_catalog("Network");`, `->next_catalog(1)` / `(-1)`, and `->refresh_catalog()`.
- `catalogs` and `catalog_url` can't be used together. `catalog_url` alone behaves as before.

### Store view (ESPHOMEBREW)

```yaml
papp_loader:
  library_style: grid        # default: list
  install_dir: /sd/roms/papp # default; where Install puts apps
```

With `library_style: grid` the library becomes an app store, like the Switch Homebrew App Store:

- **Grid.** Each app is a tile with its icon, title, version and download size (the PAPP plus its data). A green tick marks an installed app; an amber arrow marks one with a newer version in the store. Apps without a listing get a coloured tile with their initials.
- **Detail page.** Tap a tile, or press **A**, to open it: a large icon, the author, version, category and licence, the PAPP size and data size, the about text, the controls, the original project the port is based on (e.g. PrBoom 2.5.0), and the repo and commit it is built from.
- **Buttons.** **Stream** runs the app straight from the network, as the list does. **Install** downloads the `.papp` (checked against its sha256) and its data into `install_dir` and saves its listing next to it, so the app appears in a folder source such as `SD / USB` and runs offline. An installed app shows **Launch**, plus **Update to vX** when the store has a newer version. **Back** closes the page.
- **Controls.** The d-pad moves between tiles and buttons, **A** opens or presses, **B** goes back, **L/R** switch sources and **Select** opens the side menu. Touch works on everything.
- **Side menu.** A tab on the right edge of the library page slides out a menu for the source on screen: its name, where it reads from, how many apps it has, **Refresh**, and any buttons you give that source:

  ```yaml
  catalogs:
    - name: Storage
      url: /sd/roms/papp/
      actions:
        - label: Mount SD
          then:
            - script.execute: sd_mount
        - label: Eject SD
          then:
            - script.execute: sd_eject
    - name: GitHub
      url: https://nonasuomy.github.io/papp-conversions/
  ```

  Up to six buttons per source, each running any ESPHome actions. The menu starts parked, so the grid can use the whole page width: give the list (`set_catalog_container`) the full width and leave the right edge free for the tab (34 px).
- **Listings** come from the `.json` file next to each `.papp` (`psram_doom-0.1.1.json`; see [building.md](building.md#store-listing-optional)). The GitHub store publishes them. They load in the background after the list: tiles appear at once and their icons fill in.
- **A LAN server or SD folder** gets its listings from `tools/make_listing.py`:

  ```sh
  python3 tools/make_listing.py /srv/papp --base-url http://10.20.30.158:8000/
  python3 tools/make_listing.py /srv/papp --base-url http://10.20.30.158:8000/ --mirror-data
  ```

  It writes `<app>.json` next to every `.papp`, with that file's size and sha256 (checked by Install). Apps that are also in the store (`doom.papp` matches `psram_doom`) get the store's title, icon, about, controls, licence and upstream project, plus an `<app>.files` data list, so their game data downloads like it does from the store. With `--mirror-data` the data files are downloaded into `data/` on the server and listed from there. Other apps get a basic listing; put a `<app>.png` (up to 256×256) next to one for its icon. Edit a `.json` by hand if you like: rerunning keeps your changes (`--force` starts over).
- The icons are drawn as plain LVGL objects, so no extra LVGL widgets are needed. Larger title text and smaller detail text are used when the config has those Montserrat sizes (e.g. `montserrat_28` and `montserrat_16`); otherwise everything uses the default font.
- **Screen.** Apps that can choose their canvas size (see [Canvas size](#canvas-size)) also get a **Screen** button on their detail page.

### Canvas size

Apps draw on a *canvas* centred on the panel. Every app starts with the 800×480 canvas the loader has always used, so existing apps look and behave exactly as before. Apps written for it can switch to another size, up to the whole panel ([building.md](building.md#canvas-size-for-app-authors)): Tulip, for example, can use all 1024×600 pixels.

```yaml
papp_loader:
  canvas_width: 1024    # the canvas offered to apps that choose their size;
  canvas_height: 600    # default: the whole panel, or 800x480 if the display reports no size
  panel_width: 1024     # optional: the panel's size; default: the display's own
  panel_height: 600
```

- `canvas_width`/`canvas_height` are what an app is offered when it asks (and has no per-app setting). They must be even, at least 320×240 and fit the panel. Apps that never ask stay at 800×480 whatever you set here.
- `panel_width`/`panel_height` are only needed if the display reports its size wrongly. Both pairs go together (both or neither).
- The three frame buffers are allocated once for the whole panel: about 3.7 MB of PSRAM for 1024×600 instead of 2.3 MB. If that much is not free at boot, the log says so and every app stays at 800×480.
- A smaller canvas is centred with black around it and moves less data each frame.
- The close button stays right of the canvas when there is room (as with 800×480). A canvas too wide for that has no close button drawn: taps in its top-right corner reach the app, and holding that corner for 2 s closes the app.
- Screenshots and the remote view capture the canvas at its size. When an app switches size, and when one that switched closes, the whole panel is cleared.

**Per-app Screen setting.** An app whose listing has `"canvas"` ([building.md](building.md#store-listing-optional)) gets a **Screen** button on its detail page. Each press moves to the next choice and saves it: **Default** (`canvas_width`/`canvas_height`), then the panel size, 1024×600, 800×480 and 640×480 (those that fit the panel), or the sizes the listing names. The app gets it the next time it starts.

The settings live in `<install_dir>/settings.json` (by default `/sd/roms/papp/settings.json`), by app name — the `.papp` file name without its `-version` suffix, so a streamed and an installed copy share one:

```json
{
  "psram_tulip": {"canvas": "800x480"}
}
```

Edit it by hand (or with the bridge's `writefile`) if you like; other entries and fields are kept when the store rewrites it. From lambdas: `id(papp_runtime)->get_app_canvas("psram_tulip")` (`""` for Default), `->set_app_canvas("psram_tulip", "800x480")` (`""` resets it; false for a size this panel can't show) and `->canvas_choices()`.

## 3. Include the page

```yaml
packages:
  papp_store: github://NonaSuomy/papp-conversions/esphome/store_page.yaml@main
```

The page appears in the LVGL page order, so the existing ◀ / ▶ buttons in `top_layer` reach it. To add a shortcut on `main_page`:

```yaml
- button:
    width: 95
    height: 25
    x: 455
    y: 50
    widgets:
      - label: {text: "STORE", align: CENTER, text_font: roboto10}
    on_click:
      - lvgl.page.show: papp_store_page
```

If your ids differ, override them before the include:

```yaml
substitutions:
  papp_loader_id: papp_runtime       # your papp_loader id
  papp_lvgl_id: lvgl_component       # your lvgl id
  papp_font_small: roboto10          # a small font id you already define
  papp_store_refresh: 10min          # auto-refresh interval
  papp_store_bottom_clearance: "56"  # space kept free for a bottom nav bar
```

## How it behaves

- **On opening the page:** the loader is handed the list (`set_catalog_container`) and the catalog reloads (`refresh_catalog`).
- **REFRESH** reloads it on demand. It also reloads every `papp_store_refresh`, but only while the store page is on screen and no app is running (LVGL is paused while a PAPP runs).
- Each button is labelled with the file name, e.g. `psram_lvgl-0.1.0.papp`, so a new version appears as a new label after a refresh. Tapping streams the app from GitHub Pages into PSRAM. The app itself is not cached on the SD card.
- **App data.** Before a game starts, the loader reads its data list (`psram_doom-0.1.0.files`, next to the `.papp`). It first looks for each file under `data_root` and every `data_search` root (`/sd/roms/doom/doom1.wad`, `/usb0/roms/doom/doom1.wad`, ...). Only files found nowhere are downloaded, into `data_root`. Each download goes to `<file>.part`, is checked against its sha256, and is only then renamed, so an interrupted download never leaves a broken file. A file that is already on any of those storages is **never replaced or downloaded again**, whatever its size, so your own full `duke3d.grp` stays. An app without a list starts straight away. If a download fails, the app does not start; the status line says why, and the test report (if `report_url` is set) says `data_failed`. A close request (the `papp_close` API action, or a tap on the loader's close area) cancels a data download.
- **Progress.** While the data and then the `.papp` download, a bar and a status line (`1/2 doom1.wad  1.2 / 4.1 MB`, then `Loading psram_doom-0.1.0.papp  120 / 513 KB`) appear under the header. The bar hides again when the app starts; the status line stays after a failure until the next launch.
- **Files on other storage.** Apps open their files at fixed `/sd/...` paths. When an app opens a file for reading and it is not on the SD card, the loader tries the same path under `data_root` and the `data_search` roots, for example `/usb0/roms/quake/id1/pak0.pak`. Data kept on a USB drive, or downloaded there with `data_root: /usb0`, works without changing the apps. Writes (save games, configs) always go where the app asked, normally the SD card. A root that isn't mounted is simply skipped.
- The status line shows `Refreshing...`, then `N apps - tap to launch` about 4 s later. The loader has no "catalog loaded" callback, so on a slow link the count can lag. Tap REFRESH again.

### Progress on your own page

If you use your own store page instead of the package, add a track with a fill object and a label, then hand them to the loader when the page loads. Plain objects are used so this works whichever LVGL widgets your config enables.

```yaml
- obj:
    id: my_progress_track       # hidden by the loader when idle
    width: 100%
    height: 6
    pad_all: 0
    border_width: 0
    hidden: true
    widgets:
      - obj: {id: my_progress, width: 1, height: 100%, border_width: 0, bg_color: 0x38BDF8}
- label: {id: my_progress_label, text: "", hidden: true}
```

```yaml
on_load:
  - lambda: id(papp_runtime)->set_progress_widgets(id(my_progress), id(my_progress_label));
```

From lambdas you can also read `id(papp_runtime)->is_loading()`, `get_load_progress()` (0..1, or -1) and `get_load_status()`.

## USB keyboard and mouse in apps

With `usb_hidx_id` set, the loader passes USB keys and mouse/touchpad motion to the running app itself. It only needs usb_hidx's sensors to exist, with no lambdas:

```yaml
text_sensor:
  - platform: usb_hidx
    type: keyboard          # key presses -> the app's keyboard
    internal: true
sensor:
  - platform: usb_hidx
    type: mouse
    x_delta: true           # mouse/touchpad motion -> the app's mouse
    internal: true
  - platform: usb_hidx
    type: mouse
    y_delta: true
    internal: true
binary_sensor:
  - platform: usb_hidx
    type: mouse
    left_button: true       # clicks (also the K400 touchpad's)
    internal: true
  - platform: usb_hidx
    type: mouse
    right_button: true
    internal: true
```

If your YAML already calls `enqueue_keyboard_text()` or `enqueue_mouse_delta()` from these sensors, remove those lambdas, or every key and movement arrives twice.

## Not yet verified on hardware

The package has not been compiled or run on a device yet. Things to watch on the first build:

- `id(papp_store_list)` must be an `lv_obj_t *` for a plain `obj` widget, and `id(papp_store_page)->obj` must be the page's screen object.
- Page `on_load` needs an ESPHome version whose LVGL pages support it.
- The app data download and progress widgets are new: CI compiles the loader for the P4 without LVGL, and host-tests the list parser and SHA-256, but the LVGL progress code is only compiled in a real device build.
- The canvas size is compiled for the P4 by CI and its geometry (canvas and close-button placement, sizes, app names) is host-tested, but it has not run on a panel yet: the store's Screen button is LVGL code, and a full-panel canvas has not been drawn on a device.
