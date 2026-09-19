"""Tests for tools/esp_bridge: request parsing, allowlists, argv and masking.

Run: python3 -m unittest discover -s tests
"""

import io
import os
import sys
import tempfile
import textwrap
import unittest
from unittest import mock
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
    cfg = eb.Config.load(path, need_token=False)
    cfg.show_requests = overrides.get("show_requests", False)  # keep test output quiet
    return cfg


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
        with self.assertRaises(eb.BridgeError):
            eb.parse_request('@esp-bridge compile "x.yaml', None, "esp-bridge")

    def test_a_stray_quote_in_a_longer_message_is_not_an_error(self):
        text = 'Its reply said "Bridge is up", and\n@esp-bridge " is up, and so on\nthen more text'
        self.assertIsNone(eb.parse_request(text, None, "esp-bridge"))

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


STORE = "https://nonasuomy.github.io/esphomebrew/psram_lvgl-0.1.1.papp"
RELEASE = "https://github.com/NonaSuomy/esphomebrew/releases/download/dev-builds/psram_redalert.papp"


def make_api_config(tmp: Path) -> eb.Config:
    cfg = make_config(tmp)
    (tmp / "local" / "secrets.yaml").write_text("api_key_016: 'c2VjcmV0LWtleS1ieXRlcy0xMjM0NTY3ODkwMTI='\n")
    cfg.secrets_files = [tmp / "local" / "secrets.yaml"]
    cfg.api_host, cfg.api_key = "10.0.0.5", eb.load_secret_map(cfg.secrets_files)["api_key_016"]
    cfg.allowed_url_prefixes = ["https://github.com/NonaSuomy/esphomebrew/releases/download/",
                                "https://nonasuomy.github.io/esphomebrew/"]
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
                    "https://github.com/NonaSuomy/esphomebrew/releases/download/a/readme.txt",
                    "https://github.com/NonaSuomy/esphomebrew/releases/download/../../other/x.papp",
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


def fake_stream_server(packets: bytes):
    """A one-shot TCP server on 127.0.0.1 that sends `packets` to its first client."""
    import socket
    import threading

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(("127.0.0.1", 0))
    server.listen(1)

    def serve():
        client, _ = server.accept()
        with client:
            client.sendall(packets)
        server.close()

    threading.Thread(target=serve, daemon=True).start()
    return server.getsockname()[1]


def stream_packet(magic: bytes, width: int, height: int, payload: bytes) -> bytes:
    return eb.STREAM_HEADER.pack(magic, width, height, len(payload), 1) + payload


class AppLogTests(unittest.TestCase):
    def tearDown(self):
        sys.modules.pop("aioesphomeapi", None)

    def test_launch_with_seconds_attaches_the_device_log(self):
        events = []

        class Message:
            def __init__(self, text):
                self.message = text

        class FakeService:
            name = "papp_launch"

        class FakeClient:
            def __init__(self, *args, **kwargs):
                pass

            async def connect(self, login=False):
                return None

            async def list_entities_services(self):
                return [], [FakeService()]

            def subscribe_logs(self, on_log, log_level=None):
                events.append("subscribe")
                self.on_log = on_log
                return lambda: events.append("unsubscribe")

            async def execute_service(self, service, data):
                events.append("launch")
                self.on_log(Message(b"\x1b[0;32m[I][papp_loader]: RA: starting\x1b[0m"))
                self.on_log(Message("[E][papp]: crash"))

            async def disconnect(self):
                return None

        fake = type(sys)("aioesphomeapi")
        fake.APIClient = FakeClient
        sys.modules["aioesphomeapi"] = fake
        with tempfile.TemporaryDirectory() as tmp:
            cfg = make_api_config(Path(tmp))
            req = eb.parse_request(f"@esp-bridge launch url={STORE} seconds=5", None, "esp-bridge")
            self.assertTrue(req.seconds_given)
            eb.validate(req, cfg)
            original_sleep = eb.time.sleep
            import asyncio
            real_sleep = asyncio.sleep

            async def no_wait(seconds):
                await real_sleep(0)

            asyncio.sleep = no_wait
            try:
                result = eb.Runner(cfg).execute(req)
            finally:
                asyncio.sleep = real_sleep
        self.assertEqual(events, ["subscribe", "launch", "unsubscribe"])  # log starts before the launch
        self.assertIn("[I][papp_loader]: RA: starting", result.log)
        self.assertNotIn("\x1b", result.log)
        self.assertIn("Device log for 5s attached", result.summary)
        plain = eb.parse_request(f"@esp-bridge launch url={STORE}", None, "esp-bridge")
        self.assertFalse(plain.seconds_given)


class CrashCaptureTests(unittest.TestCase):
    """launch ... seconds=N serial=PORT: a crashing app reboots the device mid-capture."""

    def tearDown(self):
        sys.modules.pop("aioesphomeapi", None)
        sys.modules.pop("serial", None)

    def fake_api(self, events):
        class Message:
            def __init__(self, text):
                self.message = text

        class FakeService:
            name = "papp_launch"

        class FakeClient:
            def __init__(self, *args, **kwargs):
                pass

            async def connect(self, login=False):
                return None

            async def list_entities_services(self):
                return [], [FakeService()]

            def subscribe_logs(self, on_log, log_level=None):
                self.on_log = on_log
                return lambda: None

            async def execute_service(self, service, data):
                events.append("launch")
                self.on_log(Message(b"[I][papp_loader]: RA: video mode 640x400 8 bpp"))

            async def disconnect(self):
                raise ConnectionResetError("device rebooted")

        fake = type(sys)("aioesphomeapi")
        fake.APIClient = FakeClient
        sys.modules["aioesphomeapi"] = fake

    def fake_serial(self, opened):
        class FakeSerial:
            def __init__(self):
                self.port = self.baudrate = self.timeout = None
                self.dtr = self.rts = True
                self.chunks = [b"Guru Meditation Error: Core  0 panic'ed (Illegal instruction)\r\n", b"MEPC    : 0x4a01234c\r\n"]

            def open(self):
                opened.append((self.port, self.dtr, self.rts))

            def read(self, n):
                import time as t
                if self.chunks:
                    return self.chunks.pop(0)
                t.sleep(0.01)
                return b""

            def close(self):
                pass

        mod = type(sys)("serial")
        mod.Serial = FakeSerial
        sys.modules["serial"] = mod

    def test_serial_dump_and_log_survive_the_reboot(self):
        events, opened = [], []
        self.fake_api(events)
        self.fake_serial(opened)
        import asyncio
        real_sleep = asyncio.sleep

        async def short(seconds):
            await real_sleep(0.05)

        with tempfile.TemporaryDirectory() as tmp:
            cfg = make_api_config(Path(tmp))
            cfg.devices = ["/dev/ttyUSB0"]
            req = eb.parse_request(f"@esp-bridge launch url={STORE} seconds=5 serial=/dev/ttyUSB0", None, "esp-bridge")
            eb.validate(req, cfg)
            asyncio.sleep = short
            try:
                with mock.patch.object(eb, "load_papp_symbols", return_value=[]):
                    result = eb.Runner(cfg).execute(req)
            finally:
                asyncio.sleep = real_sleep
        self.assertEqual(opened, [("/dev/ttyUSB0", False, False)])  # no reset on open
        self.assertIn("RA: video mode", result.log)
        self.assertIn("=== serial /dev/ttyUSB0 ===", result.log)
        self.assertIn("Guru Meditation Error", result.log)
        self.assertIn("MEPC    : 0x4a01234c", result.log)
        self.assertIn("=== crash decoded ===", result.log)
        self.assertIn("crashed", result.summary)

    def test_serial_needs_an_allowed_port_and_seconds(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = make_api_config(Path(tmp))
            cfg.devices = ["/dev/ttyUSB0"]
            for text in [f"launch url={STORE} seconds=5 serial=/dev/ttyACM9",
                         f"launch url={STORE} serial=/dev/ttyUSB0",
                         f"launch url={STORE} seconds=9999"]:
                with self.subTest(text=text), self.assertRaises(eb.BridgeError):
                    eb.validate(eb.parse_request("@esp-bridge " + text, None, "esp-bridge"), cfg)


PANIC = """\
[I][papp_loader:1985][redalert]: PAPP: RA: video mode 640x400 8 bpp

abort() was called at PC 0x40093715 on core 0
Core  0 register dump:
MEPC    : 0x4ff0a146  RA      : 0x4ff0a0fc  SP      : 0x48661d30  GP      : 0x4ff0f300
Stack memory:
48661db0: 0x00000000 0x4934d044 0x4ff25000 0x400e88e2 0x4934cd74 0x4a0e762c 0x4934d044 0x400e87c2
48661dd0: 0x4a191b3c 0x4a0e762c 0x4934d044 0x400e8782 0x00000000 0x4a0e762c 0x4ff89874 0x4a0bbede

ELF file SHA256: {sha}

Rebooting...
rst:0xc (SW_CPU_RESET)
MEPC    : 0x40000001
"""


class CrashDecodeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = make_api_config(Path(self.tmp.name))
        build = Path(self.tmp.name) / "local" / ".esphome" / "build" / "p4" / ".pioenvs" / "p4"
        build.mkdir(parents=True)
        self.elf = build / "firmware.elf"
        self.elf.write_bytes(b"\x7fELF" + bytes(14) + (243).to_bytes(2, "little") + bytes(40))
        self.sha = __import__("hashlib").sha256(self.elf.read_bytes()).hexdigest()[:9]

    def tearDown(self):
        self.tmp.cleanup()

    def test_addresses_come_from_the_first_dump_only(self):
        firmware, app = eb.crash_addresses(PANIC.format(sha=self.sha))
        self.assertEqual(firmware[0], 0x40093715)  # the abort() call site first
        self.assertIn(0x400e8782, firmware)
        self.assertNotIn(0x48661d30, firmware)  # PSRAM stack pointer: data
        self.assertNotIn(0x40000001, firmware)  # after "Rebooting..."
        self.assertEqual(app, [0x4a0e762c, 0x4a191b3c, 0x4a0bbede])
        self.assertEqual(eb.crash_addresses("all fine\n"), ([], []))

    def test_decodes_firmware_and_app_addresses(self):
        sym = b"4a0bbe68 T _fclose_r\n4a0e762c D _impure_data\n4a196fe8 B _bss_end\n"

        def opener(url, timeout=0):
            self.assertTrue(url.endswith("/psram_redalert.sym"))
            return io.BytesIO(sym)

        seen = {}

        def fake_run(argv, **kwargs):
            seen["argv"] = argv
            out = "0x40093715: abort at abort.c:38\n0x4ff0a146: ?? ??:0\n0x400e8782: esphome::papp_loader::PappLoader::svc_file_close(void*) at papp_loader.cpp:1950\n"
            return eb.subprocess.CompletedProcess(argv, 0, out, "")

        self.cfg.addr2line = "riscv32-esp-elf-addr2line"
        url = "https://github.com/NonaSuomy/esphomebrew/releases/download/dev-builds/psram_redalert.papp"
        with mock.patch.object(eb.subprocess, "run", fake_run):
            text = eb.decode_crash(self.cfg, PANIC.format(sha=self.sha), url, opener=opener)
        self.assertIn("0x4a0bbede  _fclose_r + 0x76", text)
        self.assertIn("data: _impure_data + 0x0", text)
        self.assertIn("svc_file_close", text)
        self.assertNotIn("??", text)
        self.assertEqual(Path(seen["argv"][3]), self.elf)
        self.assertEqual(seen["argv"][4], "0x40093715")

    def test_esphome_crash_report_after_reboot_is_decoded(self):
        # What ESPHome's esp32 crash handler logs on the next boot (seen after
        # closing Red Alert); there is no serial dump to go on.
        report = (
            "[E][esp32.crash:302]:   BT0: 0x4FF0A146  (backtrace)\n"
            "[E][esp32.crash:302]:   BT1: 0x4FF0A0FC  (backtrace)\n"
            "[E][esp32.crash:302]:   BT3: 0x4009744C  (stack scan)\n"
            "[E][esp32.crash:358]: Use: addr2line -pfiaC -e firmware.elf 0x4FF0A146 0x4009744C\n"
            "[I][papp_loader:532]: PAPP catalog request started: http://10.13.37.84:8000/\n"
        )
        firmware, app = eb.crash_addresses(report)
        self.assertEqual(firmware[:3], [0x4FF0A146, 0x4FF0A0FC, 0x4009744C])
        self.assertEqual(app, [])
        self.assertEqual(eb.crash_addresses("[I][esp32.crash:100]: no crash recorded\n"), ([], []))

    def test_wrong_build_is_not_used(self):
        text = eb.decode_crash(self.cfg, PANIC.format(sha="0123abcde"), None)
        self.assertIn("none of the 1 firmware ELFs here has SHA256 0123abcde", text)
        self.assertIn("0x40093715", text)
        self.assertIn("no symbol list", text)


class ProxyTests(unittest.TestCase):
    def test_github_downloads_are_served_to_the_device_from_here(self):
        import io
        import struct as st
        papp = st.pack("<8I", 0x50415050, 1, 0, 4, 0, 0, 0, 0) + b"\x01\x02\x03\x04"
        with tempfile.TemporaryDirectory() as tmp:
            cfg = make_api_config(Path(tmp))
            cfg.api_host, cfg.proxy_port = "127.0.0.1", 0  # any free port
            url = eb.proxy_url(cfg, RELEASE, opener=lambda u, timeout=None: io.BytesIO(papp))
            self.assertTrue(url.startswith("http://127.0.0.1:"), url)
            self.assertTrue(url.endswith("-psram_redalert.papp"), url)
            with urllib.request.urlopen(url, timeout=5) as response:
                self.assertEqual(response.read(), papp)
            with self.assertRaises(urllib.error.HTTPError):  # only the exact file, no listing
                urllib.request.urlopen(url.rsplit("/", 1)[0] + "/", timeout=5)
            with self.assertRaises(eb.BridgeError):
                eb.proxy_url(cfg, RELEASE, opener=lambda u, timeout=None: io.BytesIO(b"<html>not a papp</html>" * 4))


class ScreenshotTests(unittest.TestCase):
    # Pixels as PAPPs draw them: RGB565 with red in the high 5 bits (Touch test's
    # C_RED is 0xF800; the live screenshot matched a photo of the panel).
    RED, GREEN, BLUE, WHITE = 0xF800, 0x07E0, 0x001F, 0xFFFF

    def frame(self):
        import array
        return array.array("H", [self.RED, self.GREEN, self.BLUE, self.WHITE]).tobytes()

    def test_png_has_the_right_size_and_colours(self):
        import struct as st
        import zlib as zl
        png = eb.rgb565_to_png(self.frame(), 2, 2)
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        width, height = st.unpack(">II", png[16:24])
        self.assertEqual((width, height), (2, 2))
        idat = png.index(b"IDAT")
        length = st.unpack(">I", png[idat - 4:idat])[0]
        rows = zl.decompress(png[idat + 4:idat + 4 + length])
        self.assertEqual(rows, bytes([0, 255, 0, 0, 0, 255, 0, 0, 0, 0, 255, 255, 255, 255]))
        with self.assertRaises(eb.BridgeError):
            eb.rgb565_to_png(b"\x00" * 6, 2, 2)

    def test_screenshot_is_read_past_thumbnails_and_audio(self):
        audio = eb.STREAM_AUDIO_HEADER.pack(b"PAPPAU01", 22050, 2, 16, 8, 1) + b"\x00" * 8
        packets = stream_packet(b"PAPPFB01", 1, 1, b"\x00\x00") + audio + stream_packet(b"PAPPSS01", 2, 2, self.frame())
        port = fake_stream_server(packets)
        import socket
        with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
            self.assertEqual(eb.read_screenshot(sock), (2, 2, self.frame()))

    def test_no_running_app_is_reported(self):
        port = fake_stream_server(stream_packet(b"PAPPSS01", 0, 0, b""))
        import socket
        with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
            with self.assertRaises(eb.BridgeError) as caught:
                eb.read_screenshot(sock)
        self.assertIn("No PAPP is running", str(caught.exception))

    def test_screenshot_job_posts_a_png(self):
        port = fake_stream_server(stream_packet(b"PAPPSS01", 2, 2, self.frame()))
        calls = []

        class FakeService:
            def __init__(self, name):
                self.name = name

        class FakeClient:
            def __init__(self, *args, **kwargs):
                pass

            async def connect(self, login=False):
                return None

            async def list_entities_services(self):
                return [], [FakeService("papp_screenshot")]

            async def execute_service(self, service, data):
                calls.append(service.name)

            async def disconnect(self):
                return None

        fake = type(sys)("aioesphomeapi")
        fake.APIClient = FakeClient
        sys.modules["aioesphomeapi"] = fake
        try:
            with tempfile.TemporaryDirectory() as tmp:
                cfg = make_api_config(Path(tmp))
                cfg.api_host, cfg.screen_port = "127.0.0.1", port
                cfg.enabled = [*cfg.enabled, "screenshot"]
                posted, uploaded = [], []

                class FakeHub:
                    def call(self, tool, args, timeout=90):
                        if tool == "post_message":
                            posted.append(args)
                        return {}

                    def upload(self, name, content, content_type="text/plain"):
                        uploaded.append((name, content[:8], content_type))
                        return "att-1"

                eb.handle_event({"from": "claude", "from_kind": "agent", "channel": "general", "id": "m",
                                 "text": "@esp-bridge screenshot"}, cfg, FakeHub(), eb.Runner(cfg))
        finally:
            sys.modules.pop("aioesphomeapi", None)
        self.assertEqual(calls, ["papp_screenshot"])
        self.assertEqual(uploaded[0][1:], (b"\x89PNG\r\n\x1a\n", "image/png"))
        self.assertEqual(posted[-1]["attachment_ids"], ["att-1"])
        self.assertIn("2×2", posted[-1]["text"])


def file_packet(status: int, payload: bytes) -> bytes:
    return eb.STREAM_FILE_HEADER.pack(b"PAPPFL01", status, len(payload), 1) + payload


class ReadFileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = make_api_config(Path(self.tmp.name))
        self.cfg.enabled = [*self.cfg.enabled, "readfile"]

    def tearDown(self):
        self.tmp.cleanup()
        sys.modules.pop("aioesphomeapi", None)

    def req(self, text):
        return eb.parse_request("@esp-bridge " + text, None, "esp-bridge")

    def test_only_sd_card_paths_are_allowed(self):
        for path in ["/sd/roms/redalert/DESYNCLOG.TXT", "/sd/", "/sd/roms/"]:
            eb.validate(self.req(f"readfile path={path}"), self.cfg)
        for path in ["/etc/passwd", "/sdcard/x", "/sd/../etc/x", "/sd/roms/..", "sd/x", "/sd/" + "a" * 130]:
            with self.subTest(path=path), self.assertRaises(eb.BridgeError):
                eb.validate(self.req(f"readfile path={path}"), self.cfg)
        with self.assertRaises(eb.BridgeError):
            eb.validate(self.req("readfile"), self.cfg)
        with self.assertRaises(eb.BridgeError):
            eb.validate(self.req('readfile "path=/sd/a b"'), self.cfg)

    def test_file_is_read_past_thumbnails_and_audio(self):
        audio = eb.STREAM_AUDIO_HEADER.pack(b"PAPPAU01", 22050, 2, 16, 4, 1) + b"\x00" * 4
        port = fake_stream_server(stream_packet(b"PAPPFB01", 1, 1, b"\x00\x00") + audio + file_packet(0, b"CRC[0]=1\n"))
        import socket
        with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
            self.assertEqual(eb.read_stream_file(sock), b"CRC[0]=1\n")

    def test_device_errors_are_reported(self):
        port = fake_stream_server(file_packet(1, b""))
        import socket
        with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
            with self.assertRaises(eb.BridgeError) as caught:
                eb.read_stream_file(sock)
        self.assertIn("not found", str(caught.exception))

    def test_text_is_shown_and_binary_is_not(self):
        text = eb.describe_file("/sd/a.txt", b"hello\n@esp-bridge launch x\n```\n")
        self.assertIn("hello", text)
        self.assertNotIn("\n@esp-bridge", text)
        self.assertEqual(text.count("```"), 2)
        self.assertIsNone(eb.request_words(text, "esp-bridge"))
        binary = eb.describe_file("/sd/main.mix", b"\x00\x01\x02\xff")
        self.assertIn("binary, 4 bytes", binary)
        self.assertNotIn("\x01", binary)
        long = eb.describe_file("/sd/big.txt", b"x" * (eb.READFILE_INLINE_CHARS + 10))
        self.assertIn(f"first {eb.READFILE_INLINE_CHARS} of", long)

    def test_serial_capture_is_allowed_with_seconds(self):
        self.cfg.devices = [*self.cfg.devices, "/dev/ttyUSB0"]
        eb.validate(self.req("readfile path=/sd/x.txt seconds=10 serial=/dev/ttyUSB0"), self.cfg)
        with self.assertRaises(eb.BridgeError):
            eb.validate(self.req("readfile path=/sd/x.txt serial=/dev/ttyUSB0"), self.cfg)

    def test_panic_lines_are_picked_out(self):
        serial = "I (1) boot\nassert failed: sdmmc_isr sdmmc_host.c:123 (ok)\nCore  0 register dump:\nE psram_mspi: MSPI PSRAM error\n"
        self.assertEqual([line for line in serial.splitlines() if eb.PANIC_LINE.search(line)],
                         ["assert failed: sdmmc_isr sdmmc_host.c:123 (ok)", "E psram_mspi: MSPI PSRAM error"])

    def test_readfile_job_posts_the_text(self):
        port = fake_stream_server(file_packet(0, b"CRC[0]=0000abcd\n"))
        calls = []

        class FakeService:
            def __init__(self, name):
                self.name = name

        class FakeClient:
            def __init__(self, *args, **kwargs):
                pass

            async def connect(self, login=False):
                return None

            async def list_entities_services(self):
                return [], [FakeService("papp_read_file")]

            async def execute_service(self, service, data):
                calls.append((service.name, data))

            async def disconnect(self):
                return None

        fake = type(sys)("aioesphomeapi")
        fake.APIClient = FakeClient
        sys.modules["aioesphomeapi"] = fake
        self.cfg.screen_port = port
        self.cfg.api_host = "127.0.0.1"
        posted, uploaded = [], []

        class FakeHub:
            def call(self, tool, args, timeout=90):
                if tool == "post_message":
                    posted.append(args)
                return {}

            def upload(self, name, content, content_type="text/plain"):
                uploaded.append(name)
                return "att-1"

        eb.handle_event({"from": "claude", "from_kind": "agent", "channel": "general", "id": "m",
                         "text": "@esp-bridge readfile path=/sd/roms/redalert/DESYNCLOG.TXT"},
                        self.cfg, FakeHub(), eb.Runner(self.cfg))
        self.assertEqual(calls, [("papp_read_file", {"path": "/sd/roms/redalert/DESYNCLOG.TXT"})])
        self.assertIn("CRC[0]=0000abcd", posted[-1]["text"])
        self.assertEqual(uploaded, [])


class WriteFileTests(unittest.TestCase):
    INI = "[Network]\nProtocol=tcp\nHost=10.20.30.158\n"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = make_api_config(Path(self.tmp.name))
        self.cfg.enabled = [*self.cfg.enabled, "writefile"]
        self.cfg.edit_requesters = ["nona", "claude"]
        self.runner = eb.Runner(self.cfg)

    def tearDown(self):
        self.tmp.cleanup()

    def req(self, path="/sd/roms/redalert/redalert.ini", content=None, requester="nona"):
        body = self.INI if content is None else content
        req = eb.parse_request(f"@esp-bridge writefile path={path}\n```ini\n{body}```\n", None, "esp-bridge")
        req.requester = requester
        return req

    def test_content_comes_from_the_code_block(self):
        self.assertEqual(self.req().content, self.INI)
        data = eb.parse_request("", {"esp_bridge": {"action": "writefile", "path": "/sd/a.ini", "content": "x=1\n"}},
                                "esp-bridge")
        self.assertEqual(data.content, "x=1\n")

    def test_writefile_is_limited(self):
        eb.validate(self.req(), self.cfg)
        refused = [self.req(requester="someone"), self.req(path="/sd/app.papp"), self.req(path="/sd/roms/"),
                   self.req(path="/sd/../boot.ini"), self.req(path="/etc/x.ini"),
                   self.req(content="Password=***\n"), self.req(content="x" * (eb.WRITEFILE_MAX_BYTES + 1))]
        for req in refused:
            with self.subTest(path=req.path, requester=req.requester), self.assertRaises(eb.BridgeError):
                eb.validate(req, self.cfg)
        no_block = eb.parse_request("@esp-bridge writefile path=/sd/a.ini", None, "esp-bridge")
        no_block.requester = "nona"
        with self.assertRaises(eb.BridgeError):
            eb.validate(no_block, self.cfg)

    def run_write(self, reads):
        req = self.req()
        eb.validate(req, self.cfg)
        with mock.patch.object(eb, "capture_file", side_effect=reads) as capture, \
                mock.patch.object(eb, "call_device_action") as action:
            result = self.runner.execute(req)
        return result, capture, action

    def test_write_is_verified_by_reading_back(self):
        result, capture, action = self.run_write([b"[Network]\nProtocol=udp\n", self.INI.encode()])
        self.assertTrue(result.ok)
        action.assert_called_once_with(self.cfg, "papp_write_file",
                                       {"path": "/sd/roms/redalert/redalert.ini", "data": self.INI})
        self.assertEqual(capture.call_count, 2)
        self.assertIn("+Protocol=tcp", result.summary)
        self.assertIn(".bak", result.summary)

    def test_refused_write_is_reported(self):
        old = b"[Network]\nProtocol=udp\n"
        result, _, _ = self.run_write([old, old])
        self.assertFalse(result.ok)
        self.assertIn("app is running", result.summary)


class DeleteFileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = make_api_config(Path(self.tmp.name))
        self.cfg.enabled = [*self.cfg.enabled, "deletefile"]
        self.cfg.edit_requesters = ["nona", "claude"]
        self.runner = eb.Runner(self.cfg)

    def tearDown(self):
        self.tmp.cleanup()

    def req(self, path="/sd/roms/redalert/SYNC001.TXT", requester="nona"):
        req = eb.parse_request(f"@esp-bridge deletefile path={path}", None, "esp-bridge")
        req.requester = requester
        return req

    def test_deletefile_is_limited(self):
        for path in ["/sd/roms/redalert/SYNC001.TXT", "/sd/roms/redalert/redalert.ini.bak", "/sd/x/crash.log"]:
            eb.validate(self.req(path), self.cfg)
        refused = [self.req(requester="someone"), self.req("/sd/roms/redalert/MAIN.MIX"), self.req("/sd/apps/doom.papp"),
                   self.req("/sd/roms/"), self.req("/sd/../x.txt"), self.req("/boot/x.txt")]
        for req in refused:
            with self.subTest(path=req.path, requester=req.requester), self.assertRaises(eb.BridgeError):
                eb.validate(req, self.cfg)

    def run_delete(self, listings):
        req = self.req()
        eb.validate(req, self.cfg)
        with mock.patch.object(eb, "capture_file", side_effect=listings) as capture, \
                mock.patch.object(eb, "call_device_action") as action:
            return self.runner.execute(req), capture, action

    def test_delete_is_checked_in_the_folder_listing(self):
        before = b"REDALERT.MIX\t25046328\nsync001.txt\t6755\n"
        result, capture, action = self.run_delete([before, b"REDALERT.MIX\t25046328\n"])
        self.assertTrue(result.ok)
        action.assert_called_once_with(self.cfg, "papp_delete_file", {"path": "/sd/roms/redalert/SYNC001.TXT"})
        self.assertEqual([call.args[1] for call in capture.call_args_list], ["/sd/roms/redalert/"] * 2)
        self.assertIn("6755 bytes", result.summary)

    def test_missing_or_refused_deletes_are_reported(self):
        result, _, action = self.run_delete([b"REDALERT.MIX\t25046328\n"])
        self.assertFalse(result.ok)
        self.assertIn("not on the card", result.summary)
        action.assert_not_called()
        listing = b"sync001.txt\t6755\n"
        result, _, _ = self.run_delete([listing, listing])
        self.assertFalse(result.ok)
        self.assertIn("still there", result.summary)


class ViewEditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = make_config(Path(self.tmp.name), enabled='["status", "config", "view", "edit"]')
        self.cfg.edit_requesters = ["nona", "claude"]
        self.runner = eb.Runner(self.cfg)
        self.office = Path(self.tmp.name) / "local" / "office.yaml"

    def tearDown(self):
        self.tmp.cleanup()

    def req(self, text, requester="claude", **data):
        if data:
            req = eb.parse_request("", {"esp_bridge": {"action": text, **data}}, "esp-bridge")
        else:
            req = eb.parse_request("@esp-bridge " + text, None, "esp-bridge")
        req.requester = requester
        return req

    def test_view_hides_secret_values_and_defuses_mentions(self):
        self.office.write_text("esphome:\n  name: office\nwifi:\n  password: plain-secret-1\n  ssid: !secret wifi_ssid\n"
                               "api:\n  encryption:\n    key: !secret api_key_016\nsubstitutions:\n  note: hunter2-long\n"
                               "  cmd: '@esp-bridge launch'\n")
        req = self.req("view office.yaml source=local")
        eb.validate(req, self.cfg)
        text = self.runner.execute(req).summary
        self.assertIn("password: ***", text)
        self.assertNotIn("plain-secret-1", text)
        self.assertIn("key: !secret api_key_016", text)
        self.assertNotIn("hunter2-long", text)
        self.assertIsNone(eb.request_words(text, "esp-bridge"))

    def test_long_view_attaches_the_whole_masked_file(self):
        self.office.write_text("wifi:\n  password: plain-secret-1\n" + "# filler line\n" * 2000 + "last: line\n")
        result = self.runner.execute(self.req("view office.yaml source=local"))
        self.assertIn("attached", result.summary)
        [(name, content, kind)] = result.files
        self.assertEqual((name, kind), ("office.yaml.txt", "text/plain"))
        self.assertTrue(content.rstrip().endswith(b"last: line"))
        self.assertNotIn(b"plain-secret-1", content)
        self.office.write_text("esphome:\n  name: office\n")
        self.assertEqual(self.runner.execute(self.req("view office.yaml source=local")).files, [])

    def test_edit_is_limited(self):
        cases = [self.req("edit", "someone", yaml="office.yaml", source="local", find="a", replace="b"),
                 self.req("edit", yaml="esphome/device.yaml", source="repo", find="a", replace="b"),
                 self.req("edit", yaml="secrets.yaml", source="local", find="a", replace="b"),
                 self.req("edit", yaml="office.yaml", source="local", replace="b"),
                 self.req("edit", yaml="office.yaml", source="local", find="a" * (eb.EDIT_TEXT_MAX + 1), replace="b")]
        for req in cases:
            with self.subTest(req=req), self.assertRaises(eb.BridgeError):
                eb.validate(req, self.cfg)
        with self.assertRaises(eb.BridgeError):
            eb.validate(self.req("view secrets.yaml source=local"), self.cfg)

    def edit(self, find, replace, config_result=(0, "INFO Configuration is valid!", False)):
        req = self.req("edit", yaml="office.yaml", source="local", find=find, replace=replace)
        eb.validate(req, self.cfg)
        with mock.patch.object(eb, "run_process", return_value=config_result) as run:
            return self.runner.execute(req), run

    def test_edit_replaces_once_keeps_a_backup_and_validates(self):
        result, run = self.edit("name: office", "name: office2")
        self.assertTrue(result.ok)
        self.assertEqual(self.office.read_text(), "esphome:\n  name: office2\n")
        backups = list(self.office.parent.glob("office.yaml.bak-*"))
        self.assertEqual([b.read_text() for b in backups], ["esphome:\n  name: office\n"])
        self.assertIn("+  name: office2", result.summary)
        self.assertEqual(run.call_args[0][0][1], "config")

    def test_rejected_edit_is_undone(self):
        result, _ = self.edit("name: office", "name: [broken", (1, "ERROR bad yaml", False))
        self.assertFalse(result.ok)
        self.assertEqual(self.office.read_text(), "esphome:\n  name: office\n")
        self.assertIn("original is back", result.summary)

    def test_find_must_match_exactly_once(self):
        with self.assertRaises(eb.BridgeError) as caught:
            self.edit("e", "x")
        self.assertIn("occurs", str(caught.exception))
        self.assertEqual(self.office.read_text(), "esphome:\n  name: office\n")

    def test_crlf_files_are_matched_and_kept(self):
        self.office.write_bytes(b"esphome:\r\n  name: office\r\nlogger:\r\n")
        result, _ = self.edit("  name: office\nlogger:", "  name: office\nlogger:\n  level: DEBUG")
        self.assertTrue(result.ok)
        self.assertEqual(self.office.read_bytes(), b"esphome:\r\n  name: office\r\nlogger:\r\n  level: DEBUG\r\n")


class DeviceStageTests(unittest.TestCase):
    def tearDown(self):
        sys.modules.pop("aioesphomeapi", None)

    def test_timeout_says_which_step_hung(self):
        import asyncio

        class SlowListClient:
            def __init__(self, *args, **kwargs):
                pass

            async def connect(self, login=False):
                return None

            async def list_entities_services(self):
                await asyncio.sleep(5)

            async def disconnect(self):
                return None

        fake = type(sys)("aioesphomeapi")
        fake.APIClient = SlowListClient
        sys.modules["aioesphomeapi"] = fake
        with tempfile.TemporaryDirectory() as tmp:
            cfg = make_api_config(Path(tmp))
            with self.assertRaises(eb.BridgeError) as caught:
                eb.call_device_action(cfg, "papp_close", {}, timeout=0.5)
        self.assertIn("listing the device's actions", str(caught.exception))

    def test_unresolvable_names_are_left_to_aioesphomeapi(self):
        self.assertEqual(eb.resolve_host("no-such-device.invalid", 6053), "no-such-device.invalid")
        self.assertEqual(eb.resolve_host("127.0.0.1", 6053), "127.0.0.1")


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

    def test_replies_never_mention_the_bridge_itself(self):
        # The hub rejects self-mentions, which silently dropped every status reply.
        with tempfile.TemporaryDirectory() as tmp:
            cfg = make_config(Path(tmp))
            posted = []

            class FakeHub:
                def call(self, tool, args, timeout=90):
                    if tool == "post_message":
                        posted.append(args["text"])
                    return {}

            eb.handle_event({"from": "claude", "from_kind": "agent", "channel": "general", "id": "m3",
                             "text": "@esp-bridge status"}, cfg, FakeHub(), eb.Runner(cfg, dry_run=True))
            self.assertEqual(len(posted), 1)
            self.assertIn("is up", posted[0])
            self.assertNotIn("@esp-bridge", posted[0].lower())


class ConsoleTests(unittest.TestCase):
    def test_requests_and_results_are_printed_by_default(self):
        import contextlib
        import io
        with tempfile.TemporaryDirectory() as tmp:
            cfg = make_config(Path(tmp), show_requests=True)
            # Default when bridge.toml has no [console] section: on.
            self.assertTrue(eb.Config.load(Path(tmp) / "bridge.toml", need_token=False).show_requests)

            class FakeHub:
                def call(self, tool, args, timeout=90):
                    return {}

                def upload(self, name, content, content_type="text/plain"):
                    return "att"

            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                eb.handle_event({"from": "nona", "from_kind": "human", "channel": "general", "id": "m",
                                 "text": "@esp-bridge compile esphome/device.yaml ref=main"}, cfg, FakeHub(),
                                eb.Runner(cfg, dry_run=True))
                eb.handle_event({"from": "mallory", "from_kind": "human", "channel": "general", "id": "m2",
                                 "text": "@esp-bridge status"}, cfg, FakeHub(), eb.Runner(cfg, dry_run=True))
            text = out.getvalue()
            self.assertIn("@nona in #general: compile esphome/device.yaml source=repo ref=main", text)
            self.assertIn("ok in", text)
            self.assertIn("@mallory in #general: refused", text)

    def test_show_requests_false_keeps_the_terminal_quiet(self):
        import contextlib
        import io
        with tempfile.TemporaryDirectory() as tmp:
            cfg = make_config(Path(tmp))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                eb.console(cfg, "hidden")
            self.assertEqual(out.getvalue(), "")


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


# ── serial reset and the serial fallback (#27) ──────────────────────────────

INCIDENT = ("Device API call failed while connecting to 10.0.0.5:6053: ReadFailedAPIError: "
            "[Errno 104] Connection reset by peer")
BOOT_LOG = "ESP-ROM:esp32p4-eco2-20240710\nrst:0x1 (POWERON),boot:0x30f (SPI_FAST_FLASH_BOOT)\n"


def fallback_config(tmp: Path, enabled: bool = True) -> eb.Config:
    cfg = make_api_config(tmp)  # devices: /dev/ttyUSB0 and 10.0.0.5; API host 10.0.0.5
    cfg.enabled = [*cfg.enabled, "screenshot", "reset"]
    cfg.fallback_serial, cfg.fallback_enabled = "/dev/ttyUSB0", enabled
    return cfg


class FakeClock:
    """Runner.clock / Runner.sleep without waiting."""

    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def fake_runner(cfg: eb.Config) -> tuple[eb.Runner, FakeClock]:
    runner, clock = eb.Runner(cfg), FakeClock()
    runner.clock, runner.sleep = clock, clock.sleep
    return runner, clock


class Device:
    """A scripted device: its network path is up or down; it records what the bridge did."""

    def __init__(self, up=False):
        self.up, self.events = up, []

    def action(self, cfg, name, data, timeout=30.0, log_seconds=0):
        self.events.append("action")
        if not self.up:
            raise eb.DeviceUnreachable(INCIDENT)
        return ""

    def probe(self, cfg, host, timeout=5.0):
        self.events.append("probe")
        return None if self.up else "ConnectionRefusedError: [Errno 111] Connection refused"

    def capture_class(self):
        device = self

        class Capture:
            def __init__(self, port, max_bytes, baud=115200, reset=False):
                self.port, self.reset, self.reset_done, self.error = port, reset, False, None

            def start(self):
                device.events.append(f"reset {self.port}" if self.reset else f"serial {self.port}")
                self.reset_done = self.reset

            def stop(self):
                return BOOT_LOG

        return Capture

    def patch(self):
        return mock.patch.multiple(eb, call_device_action=self.action, probe_network=self.probe,
                                   SerialCapture=self.capture_class())


def line_recorder():
    """A serial port that records every DTR/RTS change, like pyserial's Serial."""

    class LinePort:
        def __init__(self):
            self.__dict__.update(events=[], lines={"dtr": True, "rts": True}, port=None, baudrate=None,
                                 timeout=None, is_open=False, chunks=[BOOT_LOG.encode()])

        def __setattr__(self, name, value):
            if name in ("dtr", "rts"):
                self.events.append((name, value) if self.is_open else (name, value, "before open"))
                self.lines[name] = value
            else:
                self.__dict__[name] = value

        def __getattr__(self, name):
            if name in ("dtr", "rts"):
                return self.__dict__["lines"][name]
            raise AttributeError(name)

        def open(self):
            self.events.append(("open", self.port))
            self.is_open = True

        def read(self, n):
            if self.chunks:
                return self.chunks.pop(0)
            import time as t
            t.sleep(0.01)
            return b""

        def close(self):
            self.events.append(("close",))

    return LinePort


class ResetLineTests(unittest.TestCase):
    def tearDown(self):
        sys.modules.pop("serial", None)

    def test_en_is_pulsed_low_with_io0_high(self):
        port = line_recorder()()
        port.is_open = True
        eb.pulse_reset(port, sleep=lambda seconds: port.events.append(("sleep", seconds)))
        # RTS drives EN, DTR drives IO0 (asserted = low): DTR stays released so the
        # chip boots its firmware, RTS holds EN low for 100 ms, then lets go.
        self.assertEqual(port.events, [("dtr", False), ("rts", True), ("dtr", False), ("sleep", 0.1),
                                       ("rts", False), ("dtr", False)])
        self.assertEqual(port.lines, {"dtr": False, "rts": False})

    def test_reset_action_opens_without_a_reset_then_pulses_and_reads_the_boot_log(self):
        ports = []
        LinePort = line_recorder()

        def make():
            ports.append(LinePort())
            return ports[-1]

        mod = type(sys)("serial")
        mod.Serial = make
        sys.modules["serial"] = mod
        with tempfile.TemporaryDirectory() as tmp:
            cfg = fallback_config(Path(tmp), enabled=False)
            runner = eb.Runner(cfg)
            runner.sleep = lambda seconds: __import__("time").sleep(0.05)
            req = eb.parse_request("@esp-bridge reset serial=/dev/ttyUSB0 seconds=5", None, "esp-bridge")
            eb.validate(req, cfg)
            with mock.patch.object(eb, "probe_network") as probe:
                result = runner.execute(req)
        probe.assert_not_called()  # no device= to wait for
        events = ports[0].events
        self.assertEqual(events[:8], [("dtr", False, "before open"), ("rts", False, "before open"),
                                      ("open", "/dev/ttyUSB0"), ("dtr", False), ("rts", True), ("dtr", False),
                                      ("rts", False), ("dtr", False)])
        self.assertEqual(events[-1], ("close",))
        self.assertTrue(result.ok, result.summary)
        self.assertIn("reset over /dev/ttyUSB0", result.summary)
        self.assertIn("Serial boot log for 5s attached", result.summary)
        self.assertIn("=== serial /dev/ttyUSB0 ===", result.log)
        self.assertIn("rst:0x1 (POWERON)", result.log)

    def test_reset_can_wait_for_the_network_and_defaults_to_the_fallback_port(self):
        device = Device()
        with tempfile.TemporaryDirectory() as tmp:
            cfg = fallback_config(Path(tmp))
            runner, clock = fake_runner(cfg)
            runner.mark_down("10.0.0.5", INCIDENT)
            probes = iter(["ConnectionRefusedError: refused", "ConnectionRefusedError: refused", None])
            req = eb.parse_request("@esp-bridge reset device=10.0.0.5", None, "esp-bridge")
            with device.patch(), mock.patch.object(eb, "probe_network", side_effect=lambda *a, **k: next(probes)):
                result = runner.execute(req)
        self.assertEqual(device.events, ["reset /dev/ttyUSB0"])
        self.assertTrue(result.ok, result.summary)
        self.assertIn("10.0.0.5 back on the network after 6 s", result.summary)
        self.assertIn("network to 10.0.0.5 is back", result.summary)
        self.assertFalse(runner.is_down("10.0.0.5"))
        self.assertEqual(result.log, "")  # no seconds=: no boot log

    def test_a_port_that_cannot_be_opened_is_reported(self):
        mod = type(sys)("serial")

        class Busy:
            def open(self):
                raise OSError("[Errno 16] Device or resource busy")

        mod.Serial = Busy
        sys.modules["serial"] = mod
        with tempfile.TemporaryDirectory() as tmp:
            cfg = fallback_config(Path(tmp))
            with self.assertRaises(eb.BridgeError) as caught:
                eb.Runner(cfg).execute(eb.parse_request("@esp-bridge reset", None, "esp-bridge"))
        self.assertIn("busy", str(caught.exception))


class ResetAllowlistTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = fallback_config(Path(self.tmp.name), enabled=False)

    def tearDown(self):
        self.tmp.cleanup()

    def req(self, text):
        return eb.parse_request("@esp-bridge " + text, None, "esp-bridge")

    def test_reset_needs_an_allowlisted_serial_port(self):
        for text in ["reset serial=/dev/ttyUSB0", "reset serial=/dev/ttyUSB0 seconds=10",
                     "reset device=10.0.0.5 serial=/dev/ttyUSB0", "reset device=/dev/ttyUSB0", "reset"]:
            with self.subTest(text=text):
                eb.validate(self.req(text), self.cfg)
        self.assertEqual(eb.reset_target(self.req("reset device=10.0.0.5"), self.cfg), ("/dev/ttyUSB0", "10.0.0.5"))
        refused = ["reset serial=/dev/ttyACM9",                   # port not listed
                   "reset serial=10.0.0.5",                       # a network address is not a port
                   "reset device=10.9.9.9 serial=/dev/ttyUSB0",   # device not listed
                   "reset serial=/dev/ttyUSB0 device=/dev/ttyACM9",
                   "reset serial=/dev/ttyUSB0 seconds=9999"]
        for text in refused:
            with self.subTest(text=text), self.assertRaises(eb.BridgeError):
                eb.validate(self.req(text), self.cfg)
        self.cfg.fallback_serial = None
        with self.assertRaises(eb.BridgeError):  # no port given and no default
            eb.validate(self.req("reset"), self.cfg)
        self.cfg.enabled.remove("reset")
        with self.assertRaises(eb.BridgeError):
            eb.validate(self.req("reset serial=/dev/ttyUSB0"), self.cfg)

    def test_serial_still_only_goes_with_the_actions_that_read_it(self):
        with self.assertRaises(eb.BridgeError):
            eb.validate(self.req("catalog serial=/dev/ttyUSB0 seconds=5"), self.cfg)

    def test_fallback_port_must_be_allowlisted(self):
        path = Path(self.tmp.name) / "bridge.toml"
        base = path.read_text()
        for section in ['[fallback]\nserial = "/dev/ttyACM9"\nenabled = true\n',
                        '[fallback]\nserial = "10.0.0.5"\nenabled = true\n',
                        '[fallback]\nenabled = true\n']:
            path.write_text(base + section)
            with self.subTest(section=section), self.assertRaises(SystemExit):
                eb.Config.load(path, need_token=False)
        path.write_text(base + '[fallback]\nserial = "/dev/ttyUSB0"\nenabled = true\nwait_seconds = 90\n')
        cfg = eb.Config.load(path, need_token=False)
        self.assertEqual((cfg.fallback_port(), cfg.fallback_wait, cfg.fallback_recheck), ("/dev/ttyUSB0", 90, 30))
        path.write_text(base + '[fallback]\nserial = "/dev/ttyUSB0"\n')
        self.assertIsNone(eb.Config.load(path, need_token=False).fallback_port())  # off unless enabled
        path.write_text(base)
        self.assertIsNone(eb.Config.load(path, need_token=False).fallback_port())
        cfg.devices = ["10.0.0.5"]  # taken off the allowlist later: the fallback stops using it
        self.assertIsNone(cfg.fallback_port())


class FallbackDecisionTests(unittest.TestCase):
    def tearDown(self):
        sys.modules.pop("aioesphomeapi", None)

    @staticmethod
    def api_errors():
        class APIConnectionError(Exception):
            pass

        class ReadFailedAPIError(APIConnectionError):
            pass

        class TimeoutAPIError(APIConnectionError):
            pass

        class InvalidAuthAPIError(APIConnectionError):
            pass

        class ProtocolAPIError(APIConnectionError):
            pass

        class InvalidEncryptionKeyAPIError(ProtocolAPIError):
            pass

        class BadNameAPIError(APIConnectionError):
            pass

        return locals()

    def test_which_errors_are_network_failures(self):
        e = self.api_errors()
        for error in [ConnectionResetError(104, "Connection reset by peer"), ConnectionRefusedError(111, "refused"),
                      TimeoutError(), OSError(113, "No route to host"),
                      e["ReadFailedAPIError"]("[Errno 104] Connection reset by peer"), e["TimeoutAPIError"]("timeout"),
                      eb.DeviceUnreachable(INCIDENT)]:
            with self.subTest(error=repr(error)):
                self.assertTrue(eb.is_network_error(error))
        for error in [e["InvalidAuthAPIError"]("bad password"), e["InvalidEncryptionKeyAPIError"]("bad key"),
                      e["BadNameAPIError"]("expected device-a"), eb.BridgeError("no `papp_close` API action"),
                      ValueError("x")]:
            with self.subTest(error=repr(error)):
                self.assertFalse(eb.is_network_error(error))

    def fake_api(self, error):
        class FailingClient:
            def __init__(self, *args, **kwargs):
                pass

            async def connect(self, login=False):
                raise error

            async def disconnect(self):
                return None

        fake = type(sys)("aioesphomeapi")
        fake.APIClient = FailingClient
        sys.modules["aioesphomeapi"] = fake

    def test_device_api_errors_are_classified(self):
        e = self.api_errors()
        with tempfile.TemporaryDirectory() as tmp:
            cfg = make_api_config(Path(tmp))
            self.fake_api(e["ReadFailedAPIError"]("[Errno 104] Connection reset by peer"))
            with self.assertRaises(eb.DeviceUnreachable) as caught:
                eb.call_device_action(cfg, "papp_close", {})
            self.assertEqual(str(caught.exception), INCIDENT)  # the incident's message, unchanged
            self.assertIsNotNone(eb.probe_network(cfg, "10.0.0.5"))
            self.fake_api(e["InvalidAuthAPIError"]("Invalid password"))
            with self.assertRaises(eb.BridgeError) as caught:
                eb.call_device_action(cfg, "papp_close", {})
            self.assertNotIsInstance(caught.exception, eb.DeviceUnreachable)
            self.assertIsNone(eb.probe_network(cfg, "10.0.0.5"))  # it answered: the path works

    def test_esphome_output_is_classified(self):
        refused = "INFO Uploading firmware.bin\nERROR Connecting to 10.0.0.5 port 3232 failed: [Errno 111] Connection refused\n"
        self.assertEqual(eb.network_failure("upload", refused, False),
                         "ERROR Connecting to 10.0.0.5 port 3232 failed: [Errno 111] Connection refused")
        self.assertIsNone(eb.network_failure("upload", "ERROR Error auth result: Invalid password\n", False))
        self.assertIsNone(eb.network_failure("upload", refused, True))
        never = ("INFO Starting log output from 10.0.0.5 using esphome API\nWARNING Can't connect to ESPHome API for "
                 "office @ 10.0.0.5: Error connecting to 10.0.0.5: [Errno 111] Connect call failed (SocketAPIError)\n")
        self.assertIn("Can't connect to ESPHome API", eb.network_failure("logs", never, True))
        connected = "INFO Successfully connected to office @ 10.0.0.5 in 0.1s\n[W][api]: Connection reset by peer\n"
        self.assertIsNone(eb.network_failure("logs", connected, True))

    def test_non_network_failures_never_reset(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = fallback_config(Path(tmp))
            runner, _ = fake_runner(cfg)
            device = Device(up=True)
            missing = eb.BridgeError("The device has no `papp_close` API action.")
            with device.patch(), mock.patch.object(eb, "call_device_action", side_effect=missing) as action:
                with self.assertRaises(eb.BridgeError) as caught:
                    runner.execute(eb.parse_request("@esp-bridge close", None, "esp-bridge"))
        self.assertIs(caught.exception, missing)
        self.assertEqual(action.call_count, 1)
        self.assertEqual(device.events, [])  # no reset, no probe
        self.assertFalse(runner.is_down("10.0.0.5"))

    def test_without_the_fallback_the_network_error_stands(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = fallback_config(Path(tmp), enabled=False)
            runner, _ = fake_runner(cfg)
            device = Device()
            with device.patch(), self.assertRaises(eb.DeviceUnreachable):
                runner.execute(eb.parse_request("@esp-bridge catalog", None, "esp-bridge"))
        self.assertEqual(device.events, ["action", "action"])  # one quick network retry, no reset
        self.assertTrue(runner.is_down("10.0.0.5"))


class FallbackRouteTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cfg = fallback_config(Path(self.tmp.name))
        self.runner, self.clock = fake_runner(self.cfg)

    def tearDown(self):
        self.tmp.cleanup()

    def req(self, text, requester="nona"):
        req = eb.parse_request("@esp-bridge " + text, None, "esp-bridge")
        req.requester = requester
        return req

    def test_network_failure_resets_over_serial_and_retries(self):
        device = Device()
        attempts = iter([eb.DeviceUnreachable(INCIDENT), eb.DeviceUnreachable(INCIDENT), ""])

        def action(*args, **kwargs):
            device.events.append("action")
            outcome = next(attempts)
            if isinstance(outcome, Exception):
                raise outcome
            return outcome

        probes = iter(["ConnectionRefusedError: refused"] * 5 + [None])
        with device.patch(), mock.patch.object(eb, "call_device_action", action), \
                mock.patch.object(eb, "probe_network", side_effect=lambda *a, **k: device.events.append("probe") or next(probes)):
            result = self.runner.execute(self.req("close"))
        self.assertEqual(device.events, ["action", "action", "reset /dev/ttyUSB0"] + ["probe"] * 6 + ["action"])
        self.assertTrue(result.ok)
        first = result.summary.splitlines()[0]
        self.assertEqual(first, f"🔌 network failed ({INCIDENT}) twice; reset over /dev/ttyUSB0; "
                                "device back after 15 s; retried: ✅")
        self.assertIn("✅ `close` sent to 10.0.0.5", result.summary)
        self.assertIn("=== serial /dev/ttyUSB0 after the reset ===", result.log)
        self.assertIn("rst:0x1 (POWERON)", result.log)
        self.assertFalse(self.runner.is_down("10.0.0.5"))

    def test_a_device_that_does_not_come_back_is_reported_with_its_boot_log(self):
        device = Device()
        with device.patch(), self.assertRaises(eb.BridgeError) as caught:
            self.runner.execute(self.req("launch url=" + STORE))
        message = str(caught.exception)
        self.assertIn("reset over /dev/ttyUSB0", message)
        self.assertIn("device not back on the network after 60 s; not retried", message)
        self.assertIn("rst:0x1 (POWERON)", caught.exception.log)
        self.assertEqual(device.events.count("action"), 2)
        self.assertTrue(self.runner.is_down("10.0.0.5"))

    def test_failed_retry_is_reported(self):
        device = Device()
        with device.patch(), mock.patch.object(eb, "probe_network", return_value=None), \
                self.assertRaises(eb.BridgeError) as caught:
            self.runner.execute(self.req("catalog"))
        self.assertIn("device back after 0 s; retried: ❌ (Device API call failed", str(caught.exception))
        self.assertTrue(self.runner.is_down("10.0.0.5"))

    def test_network_logs_reset_and_retry(self):
        device = Device()
        never = "WARNING Can't connect to ESPHome API for office @ 10.0.0.5: [Errno 111] Connect call failed (SocketAPIError)\n"
        ok = "INFO Successfully connected to office @ 10.0.0.5 in 0.1s\n[I][app]: hello\n"
        with device.patch(), mock.patch.object(eb, "probe_network", return_value=None), \
                mock.patch.object(eb, "run_process", side_effect=[(None, never, False), (None, ok, False)]) as run:
            result = self.runner.execute(self.req("logs office.yaml source=local device=10.0.0.5 seconds=30"))
        self.assertEqual([call.args[0][-2:] for call in run.call_args_list], [["--device", "10.0.0.5"]] * 2)
        self.assertEqual(device.events, ["reset /dev/ttyUSB0"])
        self.assertTrue(result.ok)
        self.assertIn("network failed (WARNING Can't connect to ESPHome API", result.summary)
        self.assertIn("reset over /dev/ttyUSB0; device back after 0 s; retried: ✅", result.summary)
        self.assertIn("captured 30s of logs", result.summary)
        self.assertIn("[I][app]: hello", result.log)

    def test_ota_failure_flashes_the_same_build_over_serial(self):
        device = Device()
        refused = "INFO Uploading\nERROR Connecting to 10.0.0.5 port 3232 failed: [Errno 111] Connection refused\n"
        req = self.req("upload office.yaml source=local device=10.0.0.5")
        eb.validate(req, self.cfg)
        with device.patch(), mock.patch.object(eb, "run_process",
                                               side_effect=[(1, refused, False), (0, "INFO Successfully uploaded program.\n", False)]) as run:
            result = self.runner.execute(req)
        office = self.cfg.local_dir / "office.yaml"
        self.assertEqual([call.args[0] for call in run.call_args_list],
                         [eb.esphome_argv(self.cfg, "upload", office, "10.0.0.5"),
                          eb.esphome_argv(self.cfg, "upload", office, "/dev/ttyUSB0")])
        self.assertEqual(device.events, [])  # flashing, not a reset
        self.assertTrue(result.ok, result.summary)
        self.assertIn("network failed (ERROR Connecting to 10.0.0.5 port 3232 failed", result.summary)
        self.assertIn("flashing the same build over /dev/ttyUSB0 instead (serial flashing follows the `upload` rules)",
                      result.summary)
        self.assertIn("serial upload: ✅", result.summary)
        self.assertIn("upload `office.yaml` → `/dev/ttyUSB0`", result.summary)
        self.assertIn("=== over the network (10.0.0.5) ===", result.log)
        self.assertIn("Successfully uploaded", result.log)
        self.assertTrue(self.runner.is_down("10.0.0.5"))

    def test_only_network_ota_failures_go_to_serial(self):
        req = self.req("upload office.yaml source=local device=10.0.0.5")
        with mock.patch.object(eb, "run_process", return_value=(1, "ERROR Error auth result: Invalid password\n", False)) as run:
            result = self.runner.execute(req)
        self.assertEqual(run.call_count, 1)
        self.assertFalse(result.ok)
        self.assertNotIn("🔌", result.summary)
        self.cfg.devices = ["10.0.0.5"]  # fallback port no longer allowlisted: no serial flashing
        refused = "ERROR Connecting to 10.0.0.5 port 3232 failed: [Errno 111] Connection refused\n"
        with mock.patch.object(eb, "run_process", return_value=(1, refused, False)) as run:
            result = eb.Runner(self.cfg).execute(req)
        self.assertEqual(run.call_count, 1)
        self.assertFalse(result.ok)
        self.assertIn("exited 1", result.summary)

    def test_serial_upload_follows_the_upload_rules(self):
        self.cfg.enabled.remove("upload")
        with self.assertRaises(eb.BridgeError):
            self.runner.execute(self.req("upload office.yaml source=local device=10.0.0.5"))
        posted = []

        class FakeHub:
            def call(self, tool, args, timeout=90):
                if tool == "post_message":
                    posted.append(args["text"])
                return {}

        self.cfg.enabled.append("upload")
        with mock.patch.object(eb, "run_process") as run:
            eb.handle_event({"from": "mallory", "from_kind": "human", "channel": "general", "id": "m",
                             "text": "@esp-bridge upload office.yaml source=local device=10.0.0.5"},
                            self.cfg, FakeHub(), self.runner)
        run.assert_not_called()
        self.assertIn("not allowed", posted[-1])


class NetworkHealthTests(unittest.TestCase):
    def test_down_path_goes_straight_to_serial_and_swaps_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = fallback_config(Path(tmp))
            runner, clock = fake_runner(cfg)
            device = Device()
            req = lambda: eb.parse_request("@esp-bridge catalog", None, "esp-bridge")  # noqa: E731
            with device.patch():
                with self.assertRaises(eb.BridgeError):  # network fails, reset, device never comes back
                    runner.execute(req())
                self.assertTrue(runner.is_down("10.0.0.5"))
                self.assertIn("Network to 10.0.0.5: down since", runner.execute(eb.Request(action="status")).summary)

                device.events.clear()
                clock.now += 10  # within recheck_seconds: no network attempt, straight to serial
                with self.assertRaises(eb.BridgeError) as caught:
                    runner.execute(req())
                self.assertEqual(device.events[0], "reset /dev/ttyUSB0")
                self.assertNotIn("action", device.events)
                self.assertIn("went straight to serial", str(caught.exception))

                device.events.clear()
                clock.now += 31  # a light check is due; still down: one check, then serial
                with self.assertRaises(eb.BridgeError):
                    runner.execute(req())
                self.assertEqual(device.events[:2], ["probe", "reset /dev/ttyUSB0"])

                device.events.clear()
                device.up = True
                clock.now += 31  # the network answers again: back to it, and the reply says so
                result = runner.execute(req())
            self.assertEqual(device.events, ["probe", "action"])
            self.assertTrue(result.ok)
            self.assertRegex(result.summary, r"^🔌 network to 10\.0\.0\.5 is back \(down since \d\d:\d\d:\d\d\); using it again\n")
            self.assertIn("✅ `catalog` sent to 10.0.0.5", result.summary)
            self.assertFalse(runner.is_down("10.0.0.5"))
            self.assertNotIn("down since", runner.execute(eb.Request(action="status")).summary)

    def test_upload_goes_straight_to_serial_while_the_network_is_down(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = fallback_config(Path(tmp))
            runner, clock = fake_runner(cfg)
            runner.mark_down("10.0.0.5", "OTA refused")
            clock.now += 5
            req = eb.parse_request("@esp-bridge upload office.yaml source=local device=10.0.0.5", None, "esp-bridge")
            with mock.patch.object(eb, "run_process", return_value=(0, "INFO Successfully uploaded program.\n", False)) as run, \
                    mock.patch.object(eb, "probe_network") as probe:
                result = runner.execute(req)
        probe.assert_not_called()
        self.assertEqual(run.call_count, 1)
        self.assertEqual(run.call_args.args[0][-2:], ["--device", "/dev/ttyUSB0"])
        self.assertIn("went straight to serial", result.summary)
        self.assertTrue(result.ok)

    def test_a_working_network_clears_the_flag_without_a_reset(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = fallback_config(Path(tmp), enabled=False)  # health is tracked even without the fallback
            runner, clock = fake_runner(cfg)
            runner.mark_down("10.0.0.5", "reset by peer")
            device = Device(up=True)
            with device.patch():
                result = runner.execute(eb.parse_request("@esp-bridge close", None, "esp-bridge"))
        self.assertEqual(device.events, ["action"])  # not due for a check yet; tried the network, it worked
        self.assertIn("is back", result.summary)
        self.assertFalse(runner.is_down("10.0.0.5"))


if __name__ == "__main__":
    unittest.main()
