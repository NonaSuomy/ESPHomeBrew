"""Tests for tools/esp_bridge: request parsing, allowlists, argv and masking.

Run: python3 -m unittest discover -s tests
"""

import os
import sys
import tempfile
import textwrap
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "esp_bridge"))

import esp_bridge as eb  # noqa: E402


def make_config(tmp: Path, **overrides) -> eb.Config:
    repo = tmp / "repo"
    (repo / "esphome").mkdir(parents=True)
    (repo / "esphome" / "device.yaml").write_text("esphome:\n  name: device\n")
    local = tmp / "local"
    local.mkdir()
    (local / "office.yaml").write_text("esphome:\n  name: office\n")
    (local / "secrets.yaml").write_text("wifi_password: 'hunter2-long'\napi_key: abcdef123456\nshort: ab\n")
    toml = textwrap.dedent(f"""
        [hub]
        project_id = "p1"
        handle = "esp-bridge"
        allowed_requesters = ["claude", "@Nona"]
        [repo]
        url = "https://example.invalid/repo.git"
        path = "{repo.as_posix()}"
        allowed_yaml = ["esphome/*.yaml"]
        allowed_refs = ["main", "claude/*"]
        [local]
        dir = "{local.as_posix()}"
        [esphome]
        bin = "/opt/esphome/bin/esphome"
        devices = ["/dev/ttyUSB0", "10.0.0.5"]
        max_log_seconds = 120
        [actions]
        enabled = {overrides.get("enabled", '["status", "config", "compile", "upload", "logs", "run"]')}
    """)
    path = tmp / "bridge.toml"
    path.write_text(toml)
    return eb.Config.load(path, need_token=False)


class ParseTests(unittest.TestCase):
    def test_text_request(self):
        req = eb.parse_request("hey\n@esp-bridge upload esphome/device.yaml ref=claude/fix device=/dev/ttyUSB0", None, "esp-bridge")
        self.assertEqual((req.action, req.yaml, req.ref, req.device, req.source), ("upload", "esphome/device.yaml", "claude/fix", "/dev/ttyUSB0", "repo"))

    def test_mention_is_case_insensitive_and_backticks_are_stripped(self):
        req = eb.parse_request("`@ESP-Bridge logs office.yaml source=local device=10.0.0.5 seconds=30`", None, "esp-bridge")
        self.assertEqual((req.action, req.source, req.seconds), ("logs", "local", 30))

    def test_structured_data_wins(self):
        req = eb.parse_request("anything", {"esp_bridge": {"action": "compile", "yaml": "esphome/device.yaml", "ref": "main"}}, "esp-bridge")
        self.assertEqual((req.action, req.yaml), ("compile", "esphome/device.yaml"))

    def test_messages_without_a_request_are_ignored(self):
        self.assertIsNone(eb.parse_request("no mention here", None, "esp-bridge"))
        self.assertIsNone(eb.parse_request("@esp-bridge", None, "esp-bridge"))

    def test_only_lines_that_start_with_the_mention_are_requests(self):
        # Seen live: the bridge's own pasted startup line, a quoted mention and
        # a mention inside a sentence were all taken as requests.
        ignored = [
            "[nona@box tool]$ python esp_bridge.py serve\n@esp-bridge listening from seq 448 (dry run: False)",
            "It answered but not my `@esp-bridge status` (452). Is serve running?",
            "hey @esp-bridge can you compile?",
            "@esp-bridge-2 status",
        ]
        for text in ignored:
            with self.subTest(text=text):
                self.assertIsNone(eb.parse_request(text, None, "esp-bridge"))
        req = eb.parse_request("It took `@esp-bridge listening …` as a command. Now:\n\n@esp-bridge status", None, "esp-bridge")
        self.assertEqual(req.action, "status")
        req = eb.parse_request("> @ESP-Bridge launch url=https://x.invalid/a.papp", None, "esp-bridge")
        self.assertEqual((req.action, req.url), ("launch", "https://x.invalid/a.papp"))
        self.assertEqual(eb.parse_request("@esp-bridge status.", None, "esp-bridge").action, "status")

    def test_a_lone_request_line_with_a_typo_gets_help(self):
        with self.assertRaises(eb.BridgeError):
            eb.parse_request("@esp-bridge compil", None, "esp-bridge")

    def test_unknown_action_and_bad_seconds_are_refused(self):
        with self.assertRaises(eb.BridgeError):
            eb.parse_request("@esp-bridge rm -rf /", None, "esp-bridge")
        with self.assertRaises(eb.BridgeError):
            eb.parse_request("@esp-bridge logs x.yaml seconds=soon", None, "esp-bridge")


class ValidateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = make_config(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def req(self, text):
        return eb.parse_request("@esp-bridge " + text, None, "esp-bridge")

    def test_allowed_requests_pass(self):
        self.assertEqual(eb.validate(self.req("compile esphome/device.yaml ref=main"), self.cfg), self.cfg.repo_path)
        self.assertEqual(eb.validate(self.req("run office.yaml source=local device=10.0.0.5 seconds=60"), self.cfg), self.cfg.local_dir)

    def test_requesters_are_normalised(self):
        self.assertEqual(self.cfg.allowed_requesters, ["claude", "nona"])

    def test_refuses_what_is_not_allowlisted(self):
        cases = [
            "upload esphome/device.yaml device=/dev/ttyACM9",      # device not listed
            "upload esphome/device.yaml",                          # no device
            "compile esphome/../../etc/passwd.yaml",               # path escape
            "compile /etc/device.yaml",                            # absolute path
            "compile README.md",                                   # not an allowed file
            "compile esphome/device.yaml ref=--upload-pack=evil",  # option injection
            "compile esphome/device.yaml ref=a/../b",              # dotdot ref
            "compile esphome/device.yaml ref=pull/7/head",         # not an allowed ref (forks)
            "compile esphome/device.yaml ref=nona/x",              # not in allowed_refs here
            "logs office.yaml source=local device=10.0.0.5 seconds=999",
            "compile office.yaml source=elsewhere",
        ]
        for text in cases:
            with self.subTest(text=text), self.assertRaises(eb.BridgeError):
                eb.validate(self.req(text), self.cfg)

    def test_disabled_actions_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = make_config(Path(tmp), enabled='["status", "compile"]')
            with self.assertRaises(eb.BridgeError):
                eb.validate(self.req("upload esphome/device.yaml device=/dev/ttyUSB0"), cfg)


class CommandTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = make_config(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_argv_is_fixed(self):
        y = Path("/x/device.yaml")
        self.assertEqual(eb.esphome_argv(self.cfg, "compile", y, None)[1:], ["compile", str(y)])
        self.assertEqual(eb.esphome_argv(self.cfg, "upload", y, "/dev/ttyUSB0")[1:], ["upload", str(y), "--device", "/dev/ttyUSB0"])
        self.assertEqual(eb.esphome_argv(self.cfg, "run", y, "10.0.0.5")[1:], ["run", str(y), "--device", "10.0.0.5", "--no-logs"])

    def test_dry_run_reports_commands_without_running_them(self):
        runner = eb.Runner(self.cfg, dry_run=True)
        result = runner.execute(eb.parse_request("@esp-bridge run office.yaml source=local device=10.0.0.5 seconds=30", None, "esp-bridge"))
        self.assertTrue(result.ok)
        self.assertIn("run", result.log)
        self.assertIn("logs", result.log)
        self.assertIn("dry run", result.log)

    def test_status_needs_nothing(self):
        result = eb.Runner(self.cfg, dry_run=True).execute(eb.Request(action="status"))
        self.assertTrue(result.ok)
        self.assertIn("/dev/ttyUSB0", result.summary)

    def test_secrets_are_masked(self):
        secrets = eb.load_secret_values(self.cfg.secrets_files)
        self.assertIn("hunter2-long", secrets)
        self.assertNotIn("ab", secrets)  # too short to mask safely
        self.assertEqual(eb.mask("wifi hunter2-long key abcdef123456", secrets), "wifi *** key ***")

    def test_run_process_captures_output_and_stops(self):
        code, out, _ = eb.run_process([sys.executable, "-c", "print('hello')"], Path(self.tmp.name), 30, 1000)
        self.assertEqual((code, out.strip()), (0, "hello"))
        code, out, _ = eb.run_process([sys.executable, "-c", "import time\nprint('tick', flush=True)\ntime.sleep(30)"], Path(self.tmp.name), 60, 1000, stop_after=2)
        self.assertIsNone(code)
        self.assertIn("tick", out)

    def test_run_process_truncates(self):
        code, out, truncated = eb.run_process([sys.executable, "-c", "print('x' * 5000)"], Path(self.tmp.name), 30, 100)
        self.assertEqual((code, len(out), truncated), (0, 100, True))


STORE = "https://github.com/NonaSuomy/papp-conversions/releases/download/psram_lvgl-v0.1.1/psram_lvgl-0.1.1.papp"


def make_api_config(tmp: Path) -> eb.Config:
    cfg = make_config(tmp)
    (tmp / "local" / "secrets.yaml").write_text("api_key_016: 'c2VjcmV0LWtleS1ieXRlcy0xMjM0NTY3ODkwMTI='\n")
    cfg.secrets_files = [tmp / "local" / "secrets.yaml"]
    cfg.api_host, cfg.api_key = "10.0.0.5", eb.load_secret_map(cfg.secrets_files)["api_key_016"]
    cfg.allowed_url_prefixes = ["https://github.com/NonaSuomy/papp-conversions/releases/download/"]
    cfg.enabled = [*cfg.enabled, "launch", "close", "catalog"]
    return cfg


class DeviceApiTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = make_api_config(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()
        sys.modules.pop("aioesphomeapi", None)

    def req(self, text):
        return eb.parse_request("@esp-bridge " + text, None, "esp-bridge")

    def test_launch_urls_must_be_store_papps(self):
        eb.validate(self.req(f"launch url={STORE}"), self.cfg)
        for url in ["http://github.com/NonaSuomy/papp-conversions/releases/download/a/b.papp",
                    "https://evil.example/x.papp",
                    "https://github.com/NonaSuomy/papp-conversions/releases/download/a/readme.txt",
                    "https://github.com/NonaSuomy/papp-conversions/releases/download/../../other/x.papp",
                    ""]:
            with self.subTest(url=url), self.assertRaises(eb.BridgeError):
                eb.validate(self.req(f"launch url={url}"), self.cfg)
        eb.validate(self.req("close"), self.cfg)

    def test_device_actions_need_a_configured_host(self):
        self.cfg.api_host = None
        with self.assertRaises(eb.BridgeError):
            eb.validate(self.req("close"), self.cfg)

    def test_encryption_key_comes_from_secrets_by_name(self):
        self.assertEqual(self.cfg.api_key, "c2VjcmV0LWtleS1ieXRlcy0xMjM0NTY3ODkwMTI=")

    def test_launch_calls_the_papp_launch_api_action(self):
        calls = []

        class FakeService:
            def __init__(self, name):
                self.name = name

        class FakeClient:
            def __init__(self, host, port, password, *, noise_psk=None, client_info=None):
                calls.append(("init", host, port, noise_psk))

            async def connect(self, login=False):
                calls.append(("connect", login))

            async def list_entities_services(self):
                return [], [FakeService("papp_launch"), FakeService("papp_close")]

            async def execute_service(self, service, data):
                calls.append(("execute", service.name, data))

            async def disconnect(self):
                calls.append(("disconnect",))

        fake = type(sys)("aioesphomeapi")
        fake.APIClient = FakeClient
        sys.modules["aioesphomeapi"] = fake
        result = eb.Runner(self.cfg).execute(self.req(f"launch url={STORE}"))
        self.assertTrue(result.ok, result.summary)
        self.assertIn(("init", "10.0.0.5", 6053, self.cfg.api_key), calls)
        self.assertIn(("execute", "papp_launch", {"url": STORE}), calls)
        self.assertEqual(calls[-1], ("disconnect",))
        with self.assertRaises(eb.BridgeError):  # device without the package's actions
            eb.call_device_action(self.cfg, "papp_refresh_catalog", {})


class EventTests(unittest.TestCase):
    def test_system_and_github_messages_are_never_answered(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = make_config(Path(tmp))
            calls = []

            class FakeHub:
                def call(self, tool, args, timeout=90):
                    calls.append(tool)
                    return {}

            for kind in ("system", "github"):
                eb.handle_event({"from": kind, "from_kind": kind, "channel": "merge-requests", "id": "m1",
                                 "text": "@esp-bridge status"}, cfg, FakeHub(), eb.Runner(cfg, dry_run=True))
            self.assertEqual(calls, [])
            eb.handle_event({"from": "nona", "from_kind": "human", "channel": "general", "id": "m2",
                             "text": "@esp-bridge status"}, cfg, FakeHub(), eb.Runner(cfg, dry_run=True))
            self.assertIn("post_message", calls)


class TokenTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        make_config(Path(self.tmp.name))  # writes bridge.toml
        self.path = Path(self.tmp.name) / "bridge.toml"

    def tearDown(self):
        self.tmp.cleanup()
        os.environ.pop("EHGI_BRIDGE_TOKEN", None)

    def test_missing_or_placeholder_tokens_are_refused_before_connecting(self):
        for token in ["", "ac_PASTE_TOKEN", "paste-here", "ac_…"]:
            os.environ["EHGI_BRIDGE_TOKEN"] = token
            with self.subTest(token=token), self.assertRaises(SystemExit):
                eb.Config.load(self.path)
        os.environ["EHGI_BRIDGE_TOKEN"] = " ac_realLookingToken123 \n"
        self.assertEqual(eb.Config.load(self.path).token, "ac_realLookingToken123")

    def test_rejected_token_is_reported_once_without_retrying(self):
        os.environ["EHGI_BRIDGE_TOKEN"] = "ac_realLookingToken123"
        cfg = eb.Config.load(self.path)
        calls = []

        def refuse(request, timeout=None):
            calls.append(request.full_url)
            raise urllib.error.HTTPError(request.full_url, 401, "Unauthorized", {}, None)

        original = urllib.request.urlopen
        urllib.request.urlopen = refuse
        try:
            with self.assertRaises(eb.HubAuthError) as caught:
                eb.Hub(cfg).call("get_briefing", {})
        finally:
            urllib.request.urlopen = original
        self.assertEqual(len(calls), 1)
        self.assertIn("HTTP 401", str(caught.exception))
        self.assertIn("EHGI_BRIDGE_TOKEN", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
