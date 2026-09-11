# PAPP Conversions

**Goal (Nona, #plan seq 24-25):** a GitHub-hosted **PSRAM app store**. PAPPs are `<name>.papp` files: RISC-V (RV32IMAFC) programs an ESP32-P4 streams into PSRAM and runs. Built from this repo by GitHub Actions and browsed and launched from an **LVGL page in ESPHome**. Devices report back whether an app worked, with a log.

**Order of work** (board tasks):
1. #2 CI builds `apps/psram_lvgl` into a `.papp`.
2. #3 Publish the store (GitHub Pages catalog + release assets).
3. #4 PAPP Store LVGL page in `esp32-p4-elecrow-aio.yaml`.
4. #5 Feedback path (device result + log).
5. #6 Acceptance on device.
6. #7 OpenRA port: **on hold until Nona says go**.

**First test app:** [giltal/RetroESP32-P4 apps/psram_lvgl](https://github.com/giltal/RetroESP32-P4/tree/main/apps/psram_lvgl): LVGL v9.2 compiled into the app, 400x240 canvas scaled 2x, touch via `svc->touch_read`, exit on physical X.

**Target device:** Elecrow ESP32-P4 All-In-One (800x480, GT911 touch), ESPHome on ESP-IDF, Nona's YAML `esp32-p4-elecrow-aio.yaml` (attachment in #plan seq 25).

**Key facts:**
- `.papp` = 32-byte header (magic `0x50415050`, ABI 1, entry/text/data/bss) + raw image. ABI table in upstream `components/psram_app_loader/include/psram_app.h`; new services are append-only, so null-check them.
- The ESPHome `papp_loader` component (Nona's, not yet on GitHub; source attached in #plan seq 29) already has `launch_url` (HTTPS via cert bundle, 5 redirects, 15 s timeout) and a **catalog**: `catalog_url` HTML (max 64 KB), every href ending `.papp` becomes an LVGL list button.
- Upstream RetroESP32-P4 has **no license file**, so CI fetches it at a pinned commit instead of copying code here.

State: repo has no code yet; #2 is in progress.
