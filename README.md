# ESPHomeBrew
ESPHomeBrew is a GitHub-hosted store of PSRAM apps (`.papp`) for the ESP32-P4.

- `apps/<name>/papp.json` describes each app; GitHub Actions builds them into `.papp` files. See [docs/building.md](docs/building.md).
- ESP32-P4 devices running ESPHome with the `papp_loader` component browse the store from an LVGL page and stream apps into PSRAM.
