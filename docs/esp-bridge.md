# ESP bridge: let the team build, flash and read logs on your device

`tools/esp_bridge/esp_bridge.py` runs on the machine the ESP32-P4 is plugged into. It joins the PAPP Conversions project on EhGI **as its own agent** (for example `@esp-bridge`). When someone in the project asks it to, it compiles, uploads or reads logs with your local ESPHome, then replies in the same thread with the result, the last 40 log lines and the full log as a file.

It connects **out** to EhGI, the same way coding agents do. EhGI can't reach into your network, so nothing on your machine has to accept incoming connections. "EhGI runs it" means the team posts a request and the bridge carries it out.

```
agent / human in EhGI ──"@esp-bridge upload …"──▶ EhGI hub ◀── bridge (your machine) ──▶ esphome ──▶ /dev/ttyUSB0 or IP
                        ◀── result + log file ────────────────┘
```

## Setup (once)

1. **Give the bridge a seat.** In PAPP Conversions: *Add an agent* → name it `esp-bridge` → copy its token.
2. **Get the tool** on the machine with the device:
   ```sh
   git clone https://github.com/NonaSuomy/papp-conversions.git ~/code/papp-bridge/tool
   mkdir -p ~/.config/esp-bridge
   cp ~/code/papp-bridge/tool/tools/esp_bridge/config.example.toml ~/.config/esp-bridge/bridge.toml
   ```
3. **Edit `bridge.toml`:** handle, who may send jobs, which YAML files, which devices (`/dev/ttyUSB0`, the device IP for OTA), which actions. Point `[esphome].bin` at `~/code/esphome006/venv/bin/esphome`.
4. **Store the token privately:**
   ```sh
   printf 'EHGI_BRIDGE_TOKEN=%s\n' 'ac_…' > ~/.config/esp-bridge/env && chmod 600 ~/.config/esp-bridge/env
   ```
5. **Check, then try it without running anything:**
   ```sh
   set -a; . ~/.config/esp-bridge/env; set +a
   PY=~/code/esphome006/venv/bin/python
   $PY ~/code/papp-bridge/tool/tools/esp_bridge/esp_bridge.py --config ~/.config/esp-bridge/bridge.toml check
   $PY ~/code/papp-bridge/tool/tools/esp_bridge/esp_bridge.py --config ~/.config/esp-bridge/bridge.toml --dry-run serve
   ```
   Post `@esp-bridge status` in the project; it answers. Then stop it and run `serve` without `--dry-run`.
6. **Serial access:** your user needs to be in the `dialout` group (`sudo usermod -aG dialout $USER`, then log in again).

Optional, to keep it running as a user service (`~/.config/systemd/user/esp-bridge.service`):
```ini
[Unit]
Description=ESP bridge for EhGI
After=network-online.target

[Service]
EnvironmentFile=%h/.config/esp-bridge/env
ExecStart=%h/code/esphome006/venv/bin/python %h/code/papp-bridge/tool/tools/esp_bridge/esp_bridge.py --config %h/.config/esp-bridge/bridge.toml serve
Restart=on-failure

[Install]
WantedBy=default.target
```
`systemctl --user enable --now esp-bridge`. Stop it from EhGI with the agent's **Stop** button, or with `systemctl --user stop esp-bridge`.

## Sending it jobs

Mention the bridge on one line: an action, a YAML file, then `key=value` options.

| Request | Does |
|---|---|
| `@esp-bridge status` | Shows allowed actions and devices |
| `@esp-bridge config esphome/device.yaml ref=main` | `esphome config` (validate) |
| `@esp-bridge compile esphome/device.yaml ref=claude/store-page` | `esphome compile` at that branch |
| `@esp-bridge upload esphome/device.yaml ref=main device=/dev/ttyUSB0` | Compile and flash |
| `@esp-bridge run device.yaml source=local device=10.13.37.60 seconds=90` | Flash your local config over OTA, then capture 90 s of logs |
| `@esp-bridge logs device.yaml source=local device=/dev/ttyUSB0 seconds=60` | Capture 60 s of logs |
| `@esp-bridge launch url=https://github.com/NonaSuomy/papp-conversions/releases/download/psram_lvgl-v0.1.1/psram_lvgl-0.1.1.papp` | Stream and start that PAPP on the device |
| `@esp-bridge close` | Close the running PAPP |
| `@esp-bridge catalog` | Reload the store list on the device |

- **Where the code comes from.** `source=repo` (the default) builds from a clean checkout of this repository at `ref=`. `extra_files` (e.g. your `secrets.yaml`) are copied in first and never committed. `source=local` builds a file in your `[local].dir` as it is.
- **Agents** can send the same fields as message data: `post_message { text: "@esp-bridge compile", data: { esp_bridge: { action: "compile", yaml: "esphome/device.yaml", ref: "main" } } }`.
- **Replies:** the bridge answers in the request's thread with ⏳ when it starts, then ✅/❌ with the last 40 log lines and the full log attached. It runs one job at a time; others wait their turn.

## Launching and closing PAPPs on demand

`launch`, `close` and `catalog` go straight to the running device over the ESPHome native API. They don't rebuild anything. They need two things:

1. **The device** includes the control package, which adds the API actions `papp_launch(url)`, `papp_close` and `papp_refresh_catalog` (`esphome/device_control.yaml`):
   ```yaml
   packages:
     papp_control: github://NonaSuomy/papp-conversions/esphome/device_control.yaml@main
   ```
   The device must already have `api:` with an encryption key. Home Assistant can call the same actions as `esphome.<device>_papp_launch`.
2. **The bridge** has a `[device_api]` section: the device's `host`, and the name of its API key in `secrets.yaml` (`encryption_key_secret`). Run it with the ESPHome venv's python, which already has `aioesphomeapi`.

`launch` only accepts `https://` URLs to a `.papp` under `[device_api].allowed_url_prefixes` (by default this repo's release downloads and its Pages site). Combine it with `logs … seconds=60` to watch what the app does.

## What it will and won't do

- **Only the listed actions**, each a fixed `esphome` command (`config`, `compile`, `upload`, `run --no-logs` followed by `logs`, `logs`) or one of the three device API actions. There's no shell and no free-form flags. Anything else is refused with a reason.
- **Only listed requesters, YAML patterns, devices and refs.** Paths must stay inside the checkout or the local directory. Log capture is capped (`max_log_seconds`, `max_log_bytes`).
- **Secrets are masked.** Every value in the configured `secrets.yaml` files is replaced with `***` before anything is posted.
- **Trust model: read this.** Building an ESPHome config runs code on this machine: external components' Python, PlatformIO scripts and anything else that config pulls in. So the bridge is exactly as trustworthy as whoever can push the refs you allow. Keep `allowed_refs` to branches in this repository (people with write access). **Never allow `pull/*`**, because anyone on GitHub can open a pull request. For extra isolation, run the bridge as a separate user with access to only the serial port and its checkout.
- **Start small.** Enable `status`, `config` and `compile` first, and add `upload`, `run` and `logs` once you're comfortable.

## Tests

`python3 -m unittest discover -s tests` covers request parsing, every allowlist refusal (devices, paths, refs, option injection, disabled actions), the exact `esphome` argv, secret masking, and process time/size limits. CI runs it on every change to `tools/esp_bridge/`.
