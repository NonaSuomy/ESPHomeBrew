# Test reports: how a device tells us whether an app worked

## By hand (works today)

Open a **PAPP test report** issue (Issues → New issue → PAPP test report). Pick the app, the result, and paste the device log. The hub mirrors GitHub issues into the project, so the agents see it.

## Automatically: `report_url`

With `report_url` set, `papp_loader` POSTs one JSON report after every app run:

```yaml
papp_loader:
  # ...
  report_url: http://homeassistant.local:8123/api/webhook/papp-test-report
  report_log_bytes: 4096   # optional, 256-32768: how much of the app's log to keep
```

```json
{
  "device": "esp32-p4-elecrow-aio",
  "app": "psram_lvgl-0.1.0.papp",
  "source": "https://github.com/NonaSuomy/esphomebrew/releases/download/psram_lvgl-v0.1.0/psram_lvgl-0.1.0.papp",
  "outcome": "exited",
  "result": 0,
  "runtime_ms": 48213,
  "abi": 1,
  "log": "last log lines the app printed...\n"
}
```

| `outcome` | Meaning | `result` |
|---|---|---|
| `exited` | The app returned on its own | the app's return code |
| `closed` | Closed from the loader (on-screen X or toggle button) | the app's return code |
| `load_failed` | Download or load failed; also sets `error` (e.g. `ESP_ERR_HTTP_CONNECT`) | the `esp_err_t` |
| `start_failed` | The worker task could not be created | `-1` |

`log` holds the newest `report_log_bytes` of what the app printed through `log_printf`, collected while it ran. The request runs on its own task, so it never blocks ESPHome. It's skipped when the network is down, and the result is logged as `Test report sent (HTTP 200)` or `Test report failed: ...`.

The device holds no GitHub token. Point `report_url` at something you control that forwards the report.

## Forwarding to GitHub with Home Assistant

The GitHub token stays in Home Assistant's `secrets.yaml`. Use a fine-grained token with **Issues: read and write** on this repository only.

```yaml
# configuration.yaml
rest_command:
  papp_test_report:
    url: "https://api.github.com/repos/NonaSuomy/esphomebrew/issues/{{ issue }}/comments"
    method: POST
    headers:
      Authorization: !secret papp_github_token   # "Bearer github_pat_..."
      Accept: application/vnd.github+json
    content_type: application/json
    payload: "{{ {'body': body} | tojson }}"

automation:
  - alias: PAPP test report to GitHub
    triggers:
      - trigger: webhook
        webhook_id: papp-test-report
        allowed_methods: [POST]
        local_only: true
    actions:
      - action: rest_command.papp_test_report
        data:
          issue: 1   # the issue that collects device reports
          body: |
            **{{ trigger.json.app }}** on `{{ trigger.json.device }}`: **{{ trigger.json.outcome }}** (result {{ trigger.json.result }}{% if trigger.json.error is defined %}, {{ trigger.json.error }}{% endif %}, {{ (trigger.json.runtime_ms / 1000) | round(1) }} s)

            ```
            {{ trigger.json.log }}
            ```
```

One issue can collect every report, or you can use one per app.
