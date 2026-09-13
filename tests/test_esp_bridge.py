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


STORE = "https://nonasuomy.github.io/papp-conversions/psram_lvgl-0.1.1.papp"
RELEASE = "https://github.com/NonaSuomy/papp-conversions/releases/download/dev-builds/psram_redalert.papp"


def make_api_config(tmp: Path) -> eb.Config:
    cfg = make_config(tmp)
    (tmp / "local" / "secrets.yaml").write_text("api_key_016: 'c2VjcmV0LWtleS1ieXRlcy0xMjM0NTY3ODkwMTI='\n")
    cfg.secrets_files = [tmp / "local" / "secrets.yaml"]
    cfg.api_host, cfg.api_key = "10.0.0.5", eb.load_secret_map(cfg.secrets_files)["api_key_016"]
    cfg.allowed_url_prefixes = ["https://github.com/NonaSuomy/papp-conversions/releases/download/",
                                "https://nonasuomy.github.io/papp-conversions/"]
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
        url = "https://github.com/NonaSuomy/papp-conversions/releases/download/dev-builds/psram_redalert.papp"
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


if __name__ == "__main__":
    unittest.main()
