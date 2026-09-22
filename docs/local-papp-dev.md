# Local PAPP development

`dev-papps/` is an intentionally gitignored folder for local PAPP builds. It
is useful for testing an app over the LAN without uploading it to the SD card
or waiting for GitHub Actions and Pages.

From the repository root:

```sh
mkdir -p dev-papps
source /home/nonasuomy/esp/esp-idf/export.sh
python3 tools/build_papp.py --out dev-papps psram_lvgl
python3 -m http.server 8000 --bind 10.20.30.158 \
  --directory /home/nonasuomy/code/esphomebrew/dev-papps
```

Build another app by replacing `psram_lvgl` with its manifest name, or omit
the app name to build every app. `build_papp.py` writes the `.papp` and its
sidecar JSON into `dev-papps/`; the JSON contains the app version, listing
metadata, and embedded icon.

The Elecrow YAML already points its LAN source at
`http://10.20.30.158:8000/`. On the device, refresh the LAN/library source,
open the app, and choose **Stream**. The PAPP is downloaded into PSRAM and
run directly; it is not written to the SD card.

Keep the server terminal open while the device loads the app. Stop it with
`Ctrl-C` when finished.
