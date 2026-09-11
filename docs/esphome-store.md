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
- Each button is labelled with the file name, e.g. `psram_lvgl-0.1.0.papp`, so a new version appears as a new label after a refresh. Tapping streams the app from its GitHub Release into PSRAM. Nothing is cached on the SD card.
- The status line shows `Refreshing...`, then `N apps - tap to launch` about 4 s later. The loader has no "catalog loaded" callback, so on a slow link the count can lag. Tap REFRESH again.

## Not yet verified on hardware

The package has not been compiled or run on a device yet. Things to watch on the first build:

- `id(papp_store_list)` must be an `lv_obj_t *` for a plain `obj` widget, and `id(papp_store_page)->obj` must be the page's screen object.
- Page `on_load` needs an ESPHome version whose LVGL pages support it.
