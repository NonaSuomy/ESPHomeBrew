# ESP bridge: let the team build, flash and read logs on your device

`tools/esp_bridge/esp_bridge.py` runs on the machine the ESP32-P4 is plugged into. It joins the PAPP Conversions project on EhGI **as its own agent** (for example `@esp-bridge`). When someone in the project asks it to, it compiles, uploads or reads logs with your local ESPHome, then replies in the same thread with the result, the last 40 log lines and the full log as a file.

It connects **out** to EhGI, the same way coding agents do. EhGI can't reach into your network, so nothing on your machine has to accept incoming connections. "EhGI runs it" means the team posts a request and the bridge carries it out.

```
agent / human in EhGI ──"@esp-bridge upload …"──▶ EhGI hub ◀── bridge (your machine) ──▶ esphome ──▶ /dev/ttyUSB0 or IP
                        ◀── result + log file ────────────────┘
```

The [device bridge](device-bridge.md) (`tools/device_bridge`) is its sibling. It uses the same hub client and request format, but works on your machine instead of a device: it reads and edits files in the folders you list, runs allowlisted git commands and pushes to allowed branches.

## Setup (once)

1. **Give the bridge a seat.** On ehgi.ai, open the project and click **+ Add agent**. It's on the **Agents** page, and in the Agents list of the chat's team panel. The page is `https://ehgi.ai/p/<project id>/agents/new`; for PAPP Conversions that's https://ehgi.ai/p/3IDv1Ayo2F2gAuIzMSGL/agents/new.
   - **Handle:** `esp-bridge`. **Client:** **Other** (the bridge isn't a coding agent).
   - Click **Reserve seat and get token**, then **Copy token** on the next screen. Skip the install snippets there; they are for coding agents.
   - Don't paste the token into chat. If you lose it, the agent's page can issue a new one.
2. **Get the tool** on the machine with the device:
   ```sh
   git clone https://github.com/NonaSuomy/papp-conversions.git ~/code/papp-bridge/tool
   mkdir -p ~/.config/esp-bridge
   cp ~/code/papp-bridge/tool/tools/esp_bridge/config.example.toml ~/.config/esp-bridge/bridge.toml
   ```
3. **Edit `bridge.toml`:** handle, who may send jobs, which YAML files, which devices (`/dev/ttyUSB0`, the device IP for OTA), which actions. Point `[esphome].bin` at `~/code/esphome006/venv/bin/esphome`.
4. **Store the token privately.** This reads it without echoing it or saving it in your shell history (paste the bridge agent's `ac_…` token, then press Enter):
   ```sh
   read -rs T && printf 'EHGI_BRIDGE_TOKEN=%s\n' "$T" > ~/.config/esp-bridge/env && chmod 600 ~/.config/esp-bridge/env && unset T
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

**If `check` fails:**
- `does not look like an agent token` means the env file holds a placeholder or something that isn't an `ac_…` token.
- `The hub rejected the bridge's token (HTTP 401)` means the token isn't valid for this project: it's wrong, rotated, or its agent was removed. Copy it again from the bridge's agent in the project.
- The bridge stops on these instead of retrying.

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
| `@esp-bridge launch url=… seconds=30 serial=/dev/ttyUSB0` | Start it and return 30 s of device log plus the serial console (a crash's full panic dump and backtrace only go to serial). The port must be in `[esphome].devices` and not open elsewhere; the bridge opens it without resetting the board. If the device crashes, the reply ends with `=== crash decoded ===`: the app's addresses mapped through the dev build's `.sym` file, and the firmware's run through addr2line on the firmware ELF whose SHA256 matches the dump (see `firmware_elf`/`addr2line` in `config.example.toml`) |
| `@esp-bridge close` | Close the running PAPP |
| `@esp-bridge catalog` | Reload the store list on the device |
| `@esp-bridge screenshot` | Post an 800×480 PNG of the running PAPP in the thread |
| `@esp-bridge readfile path=/sd/roms/redalert/DESYNCLOG.TXT` | Post a text file from the device's SD card; `path=/sd/roms/` lists a directory |
| `@esp-bridge writefile path=/sd/roms/redalert/redalert.ini` + a code block | Replace a small text file on the SD card with the code block (`edit_requesters` only; not while an app runs) |
| `@esp-bridge deletefile path=/sd/roms/redalert/sync001.txt` | Remove one text or leftover (`.bak`, `.log`) file from the SD card (`edit_requesters` only; not while an app runs) |
| `@esp-bridge reset serial=/dev/ttyUSB0 seconds=10` | Hard-reset the device through its USB serial port and post 10 s of boot log (see [below](#when-the-network-fails-serial-reset-and-fallback)) |
| `@esp-bridge view device.yaml source=local` | Post the YAML with secret values hidden |
| `@esp-bridge edit device.yaml source=local "find=refresh: 1d" "replace=refresh: 0s"` | Change one exact piece of a local YAML, then validate it (see below) |

- **Where the code comes from.** `source=repo` (the default) builds from a clean checkout of this repository at `ref=`. `extra_files` (e.g. your `secrets.yaml`) are copied in first and never committed. `source=local` builds a file in your `[local].dir` as it is.
- **Agents** can send the same fields as message data: `post_message { text: "@esp-bridge compile", data: { esp_bridge: { action: "compile", yaml: "esphome/device.yaml", ref: "main" } } }`.
- **Replies:** the bridge answers in the request's thread with ⏳ when it starts, then ✅/❌ with the last 40 log lines and the full log attached. It runs one job at a time; others wait their turn.
- **In the bridge's terminal** every request is printed with who sent it and the result, e.g.
  ```
  [14:02:11] @claude in #general: launch url=https://nonasuomy.github.io/papp-conversions/psram_touchtest-0.1.0.papp
  [14:02:13]   ok in 1.8s: ✅ `launch` `psram_touchtest-0.1.0.papp` sent to 10.20.30.180.
  ```
  Refusals are printed too. To turn it off, set `[console] show_requests = false` in `bridge.toml`.

## Launching and closing PAPPs on demand

`launch`, `close`, `catalog`, `screenshot` and `readfile` go straight to the running device over the ESPHome native API. They don't rebuild anything. They need two things:

1. **The device** includes the control package, which adds the API actions `papp_launch(url)`, `papp_close`, `papp_refresh_catalog`, `papp_screenshot` and `papp_read_file(path)` (`esphome/device_control.yaml`). Use the long form with `refresh: 0s`: the short `github://…` form is only re-downloaded once a day, so a new action can be missing for a day.
   ```yaml
   packages:
     papp_control:
       url: https://github.com/NonaSuomy/papp-conversions
       ref: main
       files: [esphome/device_control.yaml]
       refresh: 0s
   ```
   The device must already have `api:`. Home Assistant can call the same actions as `esphome.<device>_papp_launch`.
2. **The bridge** has a `[device_api]` section: the device's `host`, and the name of its API key in `secrets.yaml` (`encryption_key_secret`). Leave that out if the device's `api:` has no encryption key. Run the bridge with the ESPHome venv's python, which already has `aioesphomeapi`.
   - **Use the device's IP for `host`**, not `name.local`. On the first live test, `launch` worked through `esp32-p4-elecrow-papp.local`, but a later `close` timed out twice. With the IP, both worked, which points at the name lookup on the bridge machine. If an action times out, the message names the step that hung: resolving the name, connecting, listing actions or running the action.

`launch` only accepts `https://` URLs to a `.papp` under `[device_api].allowed_url_prefixes` (by default this repo's release downloads and its Pages site). Combine it with `logs … seconds=60` to watch what the app does.

## Screenshots

`@esp-bridge screenshot` shows you what a running PAPP is drawing, which is useful while porting an app. The bridge calls `papp_screenshot`. The loader then sends one copy of the app's 800×480 canvas over its diagnostic stream on TCP port 3233 (`[device_api] screen_port`; not 3232, which ESPHome's OTA uses), and the bridge posts it as a PNG in the thread.

- It captures the PAPP canvas, the way the app drew it and the right way up. The loader's on-screen close button is not in it.
- With no app running, the bridge says so. The ESPHome/LVGL menu can't be captured this way.
- It needs a loader and `device_control.yaml` from after this was added, so rebuild the device once. The port only accepts a connection after an API request, and closes again when the bridge disconnects.
- A typical porting loop: `launch`, `screenshot`, `logs … seconds=30`, `close`, then change the app and repeat.

## Reading files from the SD card

`@esp-bridge readfile path=/sd/…` fetches a file an app wrote, such as a game's crash or desync report, without taking the card out. The bridge calls `papp_read_file(path)`, and the loader sends the file over the same diagnostic stream (a `PAPPFL01` packet). A path ending in `/` returns a listing instead, one `name<TAB>size` line per entry, with directories ending in `/`.

- Only paths under `/sd/` are served, without `..`, up to 1 MiB for a file and 64 KiB for a listing.
- Text is posted inline, up to 15,000 characters, with mentions defused so a file line can't address anyone or become a bridge request. Binary files are reported by size and SHA-256 only, so game data never leaves the device through the chat.
- It needs a loader and `device_control.yaml` with `papp_read_file`, and `readfile` in the bridge's `[actions].enabled`.

## Writing a config file on the SD card

`writefile` replaces a small text file on the card, such as a game's settings, without taking the card out. The new content is the code block in the same message:

````
@esp-bridge writefile path=/sd/roms/redalert/redalert.ini
```ini
[Network]
Protocol=tcp
Port=1234
Host=10.20.30.158
```
````

The bridge reads the old file, calls `papp_write_file(path, data)`, then reads the file back and compares. The reply shows the change as a diff, or says the write didn't happen. Agents can send the same thing as message data: `{"esp_bridge": {"action": "writefile", "path": "/sd/…", "content": "…"}}`.

- **Whole files.** The code block becomes the entire file, so start from a `readfile` of it and change what you need.
- **Text only:** `.ini`, `.cfg`, `.conf`, `.txt`, `.json`, `.yaml`, `.yml`, `.csv`, under `/sd/`, without `..`, up to 16 KiB. The loader enforces the same rules. A `.papp` or firmware image would be code, so neither side will write one.
- **Not while an app runs.** The loader refuses, because a running app may rewrite the file when it exits (Red Alert saves `redalert.ini`). Close the app first.
- **Backups.** The loader keeps the previous file as `<name>.bak`, and restores it if the write fails.
- **Who may write:** the same `[actions].edit_requesters` as `edit`. Lines with a hidden value (`***`) are refused, so a masked secret copied from a reply can't overwrite the real one.
- It needs a loader and `device_control.yaml` with `papp_write_file`, and `writefile` in `[actions].enabled`.

## Deleting a file from the SD card

`deletefile path=/sd/…` removes one file, for example a game's old debug reports. The bridge looks the file up in its folder's listing, calls `papp_delete_file(path)`, then lists the folder again to confirm the file is gone.

- **Only text and leftover files:** the `writefile` types plus `.bak` and `.log`. Game data (`.mix`, …), apps (`.papp`) and folders can't be removed this way, so a typo can't wipe a game.
- **Not while an app runs**, the same people as `writefile` (`[actions].edit_requesters`), and only under `/sd/` without `..`.
- It needs a loader and `device_control.yaml` with `papp_delete_file`, and `deletefile` in `[actions].enabled`.

## When the network fails: serial reset and fallback

After a heavy app, the device's API can stop answering, for example `Device API call failed while connecting to 10.20.30.180:6053: ReadFailedAPIError: [Errno 104] Connection reset by peer`. The bridge can then use the USB serial port that is plugged in next to it.

### `reset`

`@esp-bridge reset serial=/dev/ttyUSB0 [seconds=N] [device=10.20.30.180]` hard-resets the ESP32 the way esptool does. On the usual USB-UART auto-program circuit, RTS drives EN and DTR drives IO0. The bridge opens the port with both lines released, so opening it doesn't reset anything. It then holds EN low for 100 ms while IO0 stays high, so the chip boots its firmware rather than the ROM download mode.

- `serial=` must be a serial port in `[esphome].devices`. Leave it out to use `[fallback].serial`.
- `seconds=N` returns N seconds of serial boot log, like `launch … serial=`. Panic lines are quoted in the reply, and a crash dump is decoded.
- `device=<the device's IP>` also waits for the device's API to answer, up to `[fallback].wait_seconds`, and says how long it took.
- `reset` must be in `[actions].enabled`, and only `allowed_requesters` can use it.
- It needs a USB-UART adapter with RTS and DTR wired to EN and IO0 (a `ttyUSB*` port). The chip's own USB-Serial-JTAG port (`ttyACM*`) resets differently.

### Automatic fallback

Turn it on in `bridge.toml`. The port must also be listed in `[esphome].devices`:

```toml
[fallback]
serial = "/dev/ttyUSB0"
enabled = true
# wait_seconds = 60      # how long to wait for the API after a reset
# recheck_seconds = 30   # while the network is down, check it at most this often
```

When a network job fails with a connection-class error (refused, reset by peer, timed out, unreachable), the bridge takes the serial route:

| Job | Serial route |
|---|---|
| `launch`, `close`, `catalog`, `screenshot` | One quick network retry after 2 s. Then a reset over serial, a wait of up to `wait_seconds` for the API to answer (a connect check every 3 s), and one retry over the network |
| `logs … device=<IP>` (the log never connected) | A reset over serial, a wait for the API, then one retry of the capture |
| `upload … device=<IP>` (OTA) | The same build is flashed over serial with `esphome upload <yaml> --device /dev/ttyUSB0`. Serial flashing follows the same rules as `upload`: `upload` must be enabled, the requester must be allowed, and the port must be in `[esphome].devices` |

The reply lists every step, then the job's own result:

```
🔌 network failed (Device API call failed while connecting to 10.20.30.180:6053: ReadFailedAPIError: [Errno 104] Connection reset by peer) twice; reset over /dev/ttyUSB0; device back after 18 s; retried: ✅
✅ `catalog` sent to 10.20.30.180.
```

The serial boot log from the reset is attached with the job's log. If the device doesn't come back within `wait_seconds`, the job isn't retried, and the reply says so with the boot log attached.

Errors that a reset can't fix never trigger it. These include a wrong API key or password, another device at that address, a device without the needed API action, an OTA upload refused for its password or image, and a screen stream that fails after the API answered. `readfile`, `writefile`, `deletefile` and `run` don't fall back: a reset in the middle of a file write, or halfway through `run`, would not help.

**Network health.** The bridge remembers, in memory only, that "the network path to 10.20.30.180 is down since 14:02:11". While it is down, jobs go straight to the serial route (reset and retry, or serial upload) instead of waiting for the network to fail again. At most once every `recheck_seconds`, a job first makes a light API connect check. As soon as the network answers, the bridge goes back to it, and that job's reply begins with `🔌 network to 10.20.30.180 is back (down since 14:02:11); using it again`. `@esp-bridge status` shows whether the fallback is on and which paths are down. A restart of the bridge forgets the state.

**Try it:** `@esp-bridge reset serial=/dev/ttyUSB0 seconds=10` should post the boot log (`ESP-ROM:esp32p4…`, `rst:0x1 (POWERON)…`). The serial port must be free, so close any `esphome logs` or terminal that has it open. The fallback also works without `reset` in `[actions].enabled`, because `[fallback] enabled` is its own switch.

## Viewing and editing your local YAML

`view` posts one of your local YAML files, so the team can see how a device is set up. `edit` changes it without you having to open an editor.

- **`view`** shows the whole file. Values in your `secrets.yaml` files become `***`, and so do values written straight under password-, key-, token-, psk- or secret-like keys. `!secret name` references stay visible, because they are only names.
- **`edit`** replaces one exact piece of text. `find` must occur exactly once in the file, otherwise nothing changes. Before writing, the bridge saves the old file next to it as `<name>.bak-<date-time>`. It then runs `esphome config`; if that fails, it puts the original back and says why. The reply shows the change as a diff, with secrets hidden.
  - One-line changes can go in the chat line with quotes: `"find=refresh: 1d" "replace=refresh: 0s"`. For multi-line changes, agents send them as message data: `{"esp_bridge": {"action": "edit", "yaml": "device.yaml", "source": "local", "find": "…", "replace": "…"}}`.
- **Limits:** only `source=local` files that match `[local].allowed_yaml`, never `secrets*` files, and at most 4,000 characters for `find` and for `replace`. Repository files change through pull requests instead.
- **Who may edit:** an edited config can pull in external components that run code on your machine when it is compiled. So `edit` only works for the handles in `[actions].edit_requesters`, a subset of `allowed_requesters`. Nobody is listed by default.

## What it will and won't do

- **Only the listed actions**, each a fixed `esphome` command (`config`, `compile`, `upload`, `run --no-logs` followed by `logs`, `logs`) or one of the device API actions. `reset` only switches the serial port's RTS and DTR lines, and the serial fallback reuses the same `esphome upload` command with the allowlisted port. There's no shell and no free-form flags. Anything else is refused with a reason.
- **Only listed requesters, YAML patterns, devices and refs.** Paths must stay inside the checkout or the local directory. Log capture is capped (`max_log_seconds`, `max_log_bytes`).
- **Secrets are masked.** Every value in the configured `secrets.yaml` files is replaced with `***` before anything is posted.
- **Trust model: read this.** Building an ESPHome config runs code on this machine: external components' Python, PlatformIO scripts and anything else that config pulls in. So the bridge is exactly as trustworthy as whoever can push the refs you allow. Keep `allowed_refs` to branches in this repository (people with write access). **Never allow `pull/*`**, because anyone on GitHub can open a pull request. For extra isolation, run the bridge as a separate user with access to only the serial port and its checkout.
- **Start small.** Enable `status`, `config` and `compile` first, and add `upload`, `run` and `logs` once you're comfortable.

## Tests

`python3 -m unittest discover -s tests` covers request parsing, every allowlist refusal (devices, paths, refs, option injection, disabled actions), the exact `esphome` argv, secret masking, and process time/size limits. It also covers the serial fallback with the port and the API mocked: the order and timing of the RTS/DTR reset, which errors trigger the fallback, reset then retry, OTA then serial upload, and the network health flag. CI runs it on every change to `tools/esp_bridge/`.
