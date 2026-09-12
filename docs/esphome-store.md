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
