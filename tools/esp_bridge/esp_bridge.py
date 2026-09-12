#!/usr/bin/env python3
"""ESP bridge: lets the EhGI team build, flash and read logs from a device on
this machine, without giving anyone a shell.

It joins the project as its own agent (its own token), waits for requests that
@mention it, runs a fixed set of ESPHome commands with the local ESPHome venv,
and replies in the request's thread with the result and the full log.

Deny by default:
  * only the actions in [actions].enabled, each a fixed argv template; no shell,
    no free-form arguments;
  * only requests from handles in [hub].allowed_requesters;
  * only YAML files matching [repo]/[local] globs, only devices in [esphome].devices;
  * code comes from a clean checkout of the repository at an allowed git ref
    ([repo].allowed_refs), or from a local config directory the team cannot write.
    Building a config runs code on this machine, so only trust refs that people
    with write access push;
  * one job at a time, with time and log-size limits; secrets are masked in output.

Request format (reply in any channel, mention the bridge):
    @esp-bridge compile esphome/device.yaml ref=main
    @esp-bridge upload esphome/device.yaml ref=claude/fix device=/dev/ttyUSB0
    @esp-bridge run device.yaml source=local device=10.13.37.60 seconds=90
    @esp-bridge logs device.yaml source=local device=/dev/ttyUSB0 seconds=60
    @esp-bridge launch url=https://github.com/NonaSuomy/papp-conversions/releases/download/…/app.papp
    @esp-bridge close
    @esp-bridge catalog
    @esp-bridge status
launch/close/catalog call the papp_loader API actions from esphome/device_control.yaml
over the ESPHome native API (needs aioesphomeapi, which the ESPHome venv already has).
Agents may instead send the same fields as message data: {"esp_bridge": {...}}.

Usage:
    python esp_bridge.py --config bridge.toml check      # hub connectivity
    python esp_bridge.py --config bridge.toml serve      # run the bridge
    python esp_bridge.py --config bridge.toml --dry-run serve   # never runs esphome
    python esp_bridge.py --config bridge.toml local "compile esphome/device.yaml ref=main"
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import shlex
import socket
import shutil
import struct
import subprocess
import sys
import threading
import time
import tomllib
import urllib.error
import urllib.request
import uuid
import zlib
from dataclasses import dataclass, field
from pathlib import Path

ACTIONS = ("status", "config", "compile", "upload", "logs", "run", "launch", "close", "catalog", "screenshot")
# Bridge action -> ESPHome API action (esphome/device_control.yaml).
DEVICE_API_ACTIONS = {"launch": "papp_launch", "close": "papp_close", "catalog": "papp_refresh_catalog",
                      "screenshot": "papp_screenshot"}
NEEDS_YAML = {"config", "compile", "upload", "logs", "run"}
NEEDS_DEVICE = {"upload", "logs", "run"}
REF_RE = re.compile(r"^(?!-)(?!.*\.\.)[A-Za-z0-9._/-]{1,120}$")
TAIL_LINES = 40


class BridgeError(Exception):
    """A request that is refused or cannot run; the message is shown to the requester."""


# ── configuration ───────────────────────────────────────────────────────────


@dataclass
class Config:
    hub_url: str
    project_id: str
    token: str
    handle: str
    allowed_requesters: list[str]
    repo_url: str | None
    repo_path: Path | None
    repo_globs: list[str]
    allowed_refs: list[str]
    local_dir: Path | None
    local_globs: list[str]
    extra_files: dict[str, Path]
    esphome_bin: Path
    devices: list[str]
    enabled: list[str]
    timeouts: dict[str, int]
    max_log_seconds: int
    max_log_bytes: int
    secrets_files: list[Path] = field(default_factory=list)
    api_host: str | None = None
    api_port: int = 6053
    api_key: str | None = None
    screen_port: int = 3232
    proxy_port: int = 8765
    allowed_url_prefixes: list[str] = field(default_factory=list)
    token_env: str = "EHGI_BRIDGE_TOKEN"
    show_requests: bool = True

    @staticmethod
    def load(path: Path, *, need_token: bool = True) -> "Config":
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
        hub, repo, local, esp = raw.get("hub", {}), raw.get("repo", {}), raw.get("local", {}), raw.get("esphome", {})
        exp = lambda p: Path(os.path.expanduser(p)).resolve() if p else None  # noqa: E731
        token_env = hub.get("token_env", "EHGI_BRIDGE_TOKEN")
        token = os.environ.get(token_env, "").strip()
        if need_token and not token:
            raise SystemExit(f"Set {token_env} to the bridge agent's token.")
        if need_token and (not token.startswith("ac_") or "PASTE" in token.upper() or "…" in token):
            raise SystemExit(f"{token_env} does not look like an agent token. It should be the ac_... token shown when you "
                             "add the bridge's agent in the project, not a placeholder.")
        enabled = [a for a in raw.get("actions", {}).get("enabled", ["status", "config", "compile"]) if a in ACTIONS]
        cfg = Config(
            hub_url=hub.get("url", "https://ehgi.ai/api/mcp"),
            project_id=hub["project_id"],
            token=token,
            token_env=token_env,
            handle=hub.get("handle", "esp-bridge").lstrip("@"),
            allowed_requesters=[h.lstrip("@").lower() for h in hub.get("allowed_requesters", [])],
            repo_url=repo.get("url"),
            repo_path=exp(repo.get("path")),
            repo_globs=repo.get("allowed_yaml", ["esphome/*.yaml"]),
            # Building a config runs code on this machine (external components,
            # PlatformIO scripts), so only refs from people with write access.
            allowed_refs=repo.get("allowed_refs", ["main"]),
            local_dir=exp(local.get("dir")),
            local_globs=local.get("allowed_yaml", ["*.yaml"]),
            extra_files={dst: exp(src) for dst, src in repo.get("extra_files", {}).items()},
            esphome_bin=exp(esp.get("bin", "esphome")),
            devices=list(esp.get("devices", [])),
            enabled=enabled,
            timeouts={"config": esp.get("timeout_config", 300), "compile": esp.get("timeout_compile", 2400),
                      "upload": esp.get("timeout_upload", 900), "git": 300},
            max_log_seconds=int(esp.get("max_log_seconds", 300)),
            max_log_bytes=int(esp.get("max_log_bytes", 1_000_000)),
        )
        cfg.secrets_files = [p for p in [*cfg.extra_files.values(), exp(esp.get("secrets"))] if p and p.name.startswith("secrets")]
        if cfg.local_dir and (cfg.local_dir / "secrets.yaml").exists():
            cfg.secrets_files.append(cfg.local_dir / "secrets.yaml")
        api = raw.get("device_api", {})
        cfg.api_host = api.get("host")
        cfg.api_port = int(api.get("port", 6053))
        cfg.screen_port = int(api.get("screen_port", 3232))
        cfg.proxy_port = int(api.get("proxy_port", 8765))
        cfg.show_requests = bool(raw.get("console", {}).get("show_requests", True))
        env_key = os.environ.get(api["encryption_key_env"], "") if api.get("encryption_key_env") else ""
        cfg.api_key = env_key or load_secret_map(cfg.secrets_files).get(api.get("encryption_key_secret", "")) or None
        cfg.allowed_url_prefixes = list(api.get("allowed_url_prefixes", [
            "https://github.com/NonaSuomy/papp-conversions/releases/download/",
            "https://nonasuomy.github.io/papp-conversions/",
        ]))
        return cfg


# ── requests ────────────────────────────────────────────────────────────────


@dataclass
class Request:
    action: str
    yaml: str | None = None
    ref: str = "main"
    source: str = "repo"
    device: str | None = None
    seconds: int = 60
    url: str | None = None


def request_words(text: str, handle: str) -> list[str] | None:
    """The words of the request line in a chat message, or None if it has none.

    A request is a line that starts with the mention (`@esp-bridge compile x.yaml`,
    optionally in backticks or a quote). Mentions inside a sentence, quoted
    inline, or in pasted output (the bridge's own "@esp-bridge listening ..."
    line) are not requests. The first request line with a known action wins; a
    message that is nothing but one request line gets the "unknown action"
    help, so typos are answered while chatter is ignored.
    """
    mention = f"@{handle}".lower()
    candidates: list[list[str]] = []
    for line in text.splitlines():
        stripped = line.strip().lstrip(">").strip().strip("`").strip()
        if not stripped.lower().startswith(mention):
            continue
        rest = stripped[len(mention):]
        if rest and not rest[0].isspace():
            continue  # @esp-bridge-2, @esp-bridges, ...
        try:
            words = shlex.split(rest.strip().rstrip("`"))
        except ValueError as error:
            # A stray quote in chatter is not a request; a lone request line is.
            if len([ln for ln in text.splitlines() if ln.strip()]) == 1:
                raise BridgeError(f"Could not read the request: {error}.")
            continue
        if words:
            words[0] = words[0].strip("`.,:;!?").lower()
            candidates.append(words)
    for words in candidates:
        if words[0] in ACTIONS:
            return words
    only_line = len([ln for ln in text.splitlines() if ln.strip()]) == 1
    return candidates[0] if candidates and only_line else None


def parse_request(text: str, data: dict | None, handle: str) -> Request | None:
    """Return the request addressed to this bridge, or None if the message has none."""
    fields: dict[str, str] = {}
    if isinstance(data, dict) and isinstance(data.get("esp_bridge"), dict):
        fields = {k: str(v) for k, v in data["esp_bridge"].items()}
    else:
        words = request_words(text, handle)
        if words is None:
            return None
        fields["action"] = words[0]
        for word in words[1:]:
            if "=" in word:
                key, value = word.split("=", 1)
                fields[key.lower()] = value
            elif "yaml" not in fields:
                fields["yaml"] = word
    action = fields.get("action", "").lower()
    if action not in ACTIONS:
        raise BridgeError(f"Unknown action `{action}`. Use one of: {', '.join(ACTIONS)}.")
    try:
        seconds = int(fields.get("seconds", "60"))
    except ValueError:
        raise BridgeError("`seconds` must be a whole number.")
    return Request(action=action, yaml=fields.get("yaml"), ref=fields.get("ref", "main"),
                   source=fields.get("source", "repo").lower(), device=fields.get("device"), seconds=seconds,
                   url=fields.get("url"))


def _inside(base: Path, rel: str) -> Path:
    if not rel or rel.startswith(("/", "\\", "~")) or re.match(r"^[A-Za-z]:", rel):
        raise BridgeError("YAML paths must be relative.")
    target = (base / rel).resolve()
    if base != target and base not in target.parents:
        raise BridgeError("YAML path leaves the allowed directory.")
    return target


def validate(req: Request, cfg: Config) -> Path | None:
    """Check a request against the allowlists. Returns the base directory for YAML jobs."""
    if req.action not in cfg.enabled:
        raise BridgeError(f"`{req.action}` is not enabled on this bridge (enabled: {', '.join(cfg.enabled)}).")
    if req.action in NEEDS_DEVICE:
        if not req.device:
            raise BridgeError(f"`{req.action}` needs `device=` (allowed: {', '.join(cfg.devices) or 'none'}).")
        if req.device not in cfg.devices:
            raise BridgeError(f"Device `{req.device}` is not in this bridge's allowlist.")
    if req.action in DEVICE_API_ACTIONS:
        if not cfg.api_host:
            raise BridgeError("This bridge has no [device_api] host configured.")
        if req.action == "launch":
            if not req.url or not req.url.startswith("https://"):
                raise BridgeError("`launch` needs `url=https://…/app.papp`.")
            if not any(req.url.startswith(p) for p in cfg.allowed_url_prefixes):
                raise BridgeError(f"That URL is not in the allowed prefixes ({', '.join(cfg.allowed_url_prefixes)}).")
            if not req.url.lower().endswith(".papp") or ".." in req.url or any(c.isspace() for c in req.url):
                raise BridgeError("`url` must point at a .papp file.")
        return None
    if req.action in {"logs", "run"} and not 5 <= req.seconds <= cfg.max_log_seconds:
        raise BridgeError(f"`seconds` must be between 5 and {cfg.max_log_seconds}.")
    if req.action not in NEEDS_YAML:
        return None
    if not req.yaml:
        raise BridgeError(f"`{req.action}` needs a YAML file.")
    if req.source == "repo":
        if not (cfg.repo_url and cfg.repo_path):
            raise BridgeError("This bridge has no repository configured; use `source=local`.")
        if not REF_RE.match(req.ref):
            raise BridgeError("`ref` must be a branch, tag or commit SHA.")
        if not any(fnmatch.fnmatchcase(req.ref, g) for g in cfg.allowed_refs):
            raise BridgeError(f"Ref `{req.ref}` is not allowed on this bridge (allowed: {', '.join(cfg.allowed_refs)}).")
        base, globs = cfg.repo_path, cfg.repo_globs
    elif req.source == "local":
        if not cfg.local_dir:
            raise BridgeError("This bridge has no local directory configured; use `source=repo`.")
        base, globs = cfg.local_dir, cfg.local_globs
    else:
        raise BridgeError("`source` must be `repo` or `local`.")
    _inside(base, req.yaml)
    rel = Path(req.yaml).as_posix()
    if not any(fnmatch.fnmatch(rel, g) for g in globs):
        raise BridgeError(f"`{rel}` does not match the allowed files ({', '.join(globs)}).")
    return base


def esphome_argv(cfg: Config, action: str, yaml_path: Path, device: str | None) -> list[str]:
    """The only commands this bridge ever runs."""
    exe = str(cfg.esphome_bin)
    if action == "config":
        return [exe, "config", str(yaml_path)]
    if action == "compile":
        return [exe, "compile", str(yaml_path)]
    if action == "upload":
        return [exe, "upload", str(yaml_path), "--device", str(device)]
    if action == "run":
        return [exe, "run", str(yaml_path), "--device", str(device), "--no-logs"]
    if action == "logs":
        return [exe, "logs", str(yaml_path), "--device", str(device)]
    raise BridgeError(f"No command for `{action}`.")


# ── secrets masking ─────────────────────────────────────────────────────────


def load_secret_values(files: list[Path]) -> list[str]:
    values: list[str] = []
    for path in files:
        try:
            for line in path.read_text(encoding="utf-8").splitlines():
                m = re.match(r"^\s*[A-Za-z0-9_.-]+\s*:\s*(.+?)\s*$", line)
                if m and not m.group(1).startswith("#"):
                    value = m.group(1).strip().strip("'\"")
                    if len(value) >= 4:
                        values.append(value)
        except OSError:
            continue
    return sorted(set(values), key=len, reverse=True)


def load_secret_map(files: list[Path]) -> dict[str, str]:
    found: dict[str, str] = {}
    for path in files:
        try:
            for line in path.read_text(encoding="utf-8").splitlines():
                m = re.match(r"^\s*([A-Za-z0-9_.-]+)\s*:\s*(.+?)\s*$", line)
                if m and not m.group(2).startswith("#"):
                    found.setdefault(m.group(1), m.group(2).strip().strip("'\""))
        except OSError:
            continue
    return found


def mask(text: str, secrets: list[str]) -> str:
    for value in secrets:
        text = text.replace(value, "***")
    return text


# ── device API (papp_loader actions over the ESPHome native API) ────────────


def resolve_host(host: str, port: int) -> str:
    """The device address to dial: the system resolver's answer when it has one.

    Linux resolves `name.local` through Avahi/nss-mdns when installed; when it
    cannot, aioesphomeapi falls back to its own mDNS lookup of the name.
    """
    try:
        return socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)[0][4][0]
    except (socket.gaierror, IndexError, OSError):
        return host


def call_device_action(cfg: Config, action: str, data: dict[str, str], timeout: float = 30.0) -> None:
    """Execute an ESPHome API action on the configured device."""
    import asyncio
    import inspect

    try:
        from aioesphomeapi import APIClient
    except ImportError as error:
        raise BridgeError("aioesphomeapi is missing; run the bridge with the ESPHome venv's python.") from error

    # Which step was running when the time ran out tells a name/network problem
    # (resolving, connecting) from a device that is up but slow (listing, running).
    stage = ["resolving the name"]

    async def go() -> None:
        address = await asyncio.get_running_loop().run_in_executor(None, resolve_host, cfg.api_host, cfg.api_port)
        stage[0] = f"connecting to {address}:{cfg.api_port}" if address != cfg.api_host else f"connecting to {cfg.api_host}:{cfg.api_port}"
        client = APIClient(address, cfg.api_port, None, noise_psk=cfg.api_key, client_info="esp-bridge")
        await client.connect(login=True)
        try:
            stage[0] = "listing the device's actions"
            _, services = await client.list_entities_services()
            service = next((s for s in services if s.name == action), None)
            if service is None:
                raise BridgeError(f"The device has no `{action}` API action. Include esphome/device_control.yaml.")
            stage[0] = f"running `{action}`"
            result = client.execute_service(service, data)
            if inspect.isawaitable(result):
                await result
        finally:
            await client.disconnect()

    try:
        asyncio.run(asyncio.wait_for(go(), timeout))
    except BridgeError:
        raise
    except asyncio.TimeoutError as error:
        raise BridgeError(f"The device at {cfg.api_host} did not answer within {timeout:.0f}s "
                          f"(stuck while {stage[0]}).") from error
    except Exception as error:  # noqa: BLE001 - connection/auth problems are reported, not fatal
        raise BridgeError(f"Device API call failed while {stage[0]}: {type(error).__name__}: {error}") from error


# ── screenshots (papp_loader diagnostic stream, TCP port 3232) ──────────────

STREAM_HEADER = struct.Struct("<8sHHII")         # PAPPFB01 thumbnail / PAPPSS01 screenshot
STREAM_AUDIO_HEADER = struct.Struct("<8sIHHII")  # PAPPAU01


def rgb565_to_png(raw: bytes, width: int, height: int) -> bytes:
    """PNG of a papp_loader frame: little-endian RGB565, red in the high 5 bits.

    That is how PAPPs draw (e.g. 0xF800 is red). Checked on the first live
    screenshot against a photo of the panel running Touch test."""
    if len(raw) != width * height * 2:
        raise BridgeError(f"Screenshot is {len(raw)} bytes, expected {width * height * 2}.")
    five = bytes((v << 3) | (v >> 2) for v in range(32))
    six = bytes((v << 2) | (v >> 4) for v in range(64))
    pixels = memoryview(raw).cast("H")
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # PNG filter type 0 (none)
        for v in pixels[y * width:(y + 1) * width]:
            rows += bytes((five[v >> 11], six[(v >> 5) & 0x3F], five[v & 0x1F]))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
            + chunk(b"IEND", b""))


def read_screenshot(sock: socket.socket) -> tuple[int, int, bytes]:
    """Read stream packets until the PAPPSS01 screenshot; thumbnails and audio are skipped."""
    def exact(size: int) -> bytes:
        data = bytearray()
        while len(data) < size:
            part = sock.recv(min(65536, size - len(data)))
            if not part:
                raise BridgeError("The device closed the screen stream before the screenshot arrived.")
            data += part
        return bytes(data)

    while True:
        magic = exact(8)
        if magic in (b"PAPPFB01", b"PAPPSS01"):
            _, width, height, size, _seq = STREAM_HEADER.unpack(magic + exact(STREAM_HEADER.size - 8))
            if size != width * height * 2 or size > 4 * 1024 * 1024:
                raise BridgeError("The device sent a malformed screen stream header.")
            payload = exact(size)
            if magic == b"PAPPSS01":
                if width == 0 or height == 0:
                    raise BridgeError("No PAPP is running on the device, so there is nothing to capture. Launch one first.")
                return width, height, payload
        elif magic == b"PAPPAU01":
            header = STREAM_AUDIO_HEADER.unpack(magic + exact(STREAM_AUDIO_HEADER.size - 8))
            exact(header[4])
        else:
            raise BridgeError(f"Unexpected data on the screen stream ({magic!r}). Is the device's loader up to date?")


def capture_screenshot(cfg: Config, timeout: float = 20.0) -> tuple[int, int, bytes]:
    """Ask the device for a screenshot and read it from its diagnostic stream."""
    call_device_action(cfg, DEVICE_API_ACTIONS["screenshot"], {})
    address = resolve_host(cfg.api_host, cfg.screen_port)
    try:
        with socket.create_connection((address, cfg.screen_port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            return read_screenshot(sock)
    except (socket.timeout, TimeoutError) as error:
        raise BridgeError(f"No screenshot arrived from {cfg.api_host}:{cfg.screen_port} within {timeout:.0f}s.") from error
    except OSError as error:
        raise BridgeError(f"Could not open the screen stream on {cfg.api_host}:{cfg.screen_port}: {error}.") from error


# ── serving github.com downloads to the device ─────────────────────────────
#
# The device cannot verify github.com's TLS certificate chain (Sectigo's newer
# ECC root), so release downloads such as work-in-progress builds fail there.
# The bridge downloads them on this machine and serves them to the device over
# plain HTTP on the LAN, from a server that only knows those exact files.

PROXIED_PREFIXES = ("https://github.com/",)


class _ProxyServer:
    def __init__(self, port: int):
        import http.server

        files: dict[str, bytes] = {}

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802 - http.server API
                body = files.get(self.path.lstrip("/"))
                if body is None:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.files = files
        self.server = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


_proxy: _ProxyServer | None = None


def lan_address_towards(host: str, port: int) -> str:
    """This machine's address on the route to the device (no packet is sent)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect((resolve_host(host, port), port))
        return sock.getsockname()[0]


def proxy_url(cfg: Config, url: str, opener=urllib.request.urlopen) -> str:
    """Download `url` here and return the LAN URL the device can load it from."""
    global _proxy
    try:
        with opener(url, timeout=120) as response:
            body = response.read(64 * 1024 * 1024 + 1)
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise BridgeError(f"Could not download {url}: {error}.") from error
    if len(body) > 64 * 1024 * 1024 or len(body) < 32 or struct.unpack_from("<I", body)[0] != 0x50415050:
        raise BridgeError(f"{url} is not a .papp file.")
    if _proxy is None:
        try:
            _proxy = _ProxyServer(cfg.proxy_port)
        except OSError as error:
            raise BridgeError(f"Could not serve files on port {cfg.proxy_port}: {error}.") from error
    name = f"{hashlib.sha256(body).hexdigest()[:12]}-{url.rsplit('/', 1)[-1]}"
    _proxy.files.clear()  # only ever the latest launch
    _proxy.files[name] = body
    address = lan_address_towards(cfg.api_host, cfg.api_port)
    return f"http://{address}:{_proxy.port}/{name}"


# ── running jobs ────────────────────────────────────────────────────────────


@dataclass
class JobResult:
    ok: bool
    summary: str
    log: str
    seconds: float
    files: list[tuple[str, bytes, str]] = field(default_factory=list)  # (name, content, content type)


def run_process(argv: list[str], cwd: Path, timeout: int, max_bytes: int, stop_after: int | None = None) -> tuple[int | None, str, bool]:
    """Run argv (no shell). Returns (exit code or None on timeout/stop, output, truncated)."""
    proc = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    chunks: list[bytes] = []
    size = 0
    truncated = False

    def reader() -> None:
        nonlocal size, truncated
        assert proc.stdout is not None
        for chunk in iter(lambda: proc.stdout.read(4096), b""):
            if size < max_bytes:
                chunks.append(chunk[: max_bytes - size])
                size += len(chunks[-1])
                truncated = truncated or size >= max_bytes
            else:
                truncated = True

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    limit = stop_after if stop_after is not None else timeout
    try:
        code = proc.wait(timeout=limit)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        code = None
    thread.join(timeout=5)
    if proc.stdout is not None:
        proc.stdout.close()
    return code, b"".join(chunks).decode("utf-8", "replace"), truncated


class Runner:
    def __init__(self, cfg: Config, dry_run: bool = False):
        self.cfg, self.dry_run = cfg, dry_run
        self.secrets = load_secret_values(cfg.secrets_files)

    def _git(self, *args: str) -> str:
        assert self.cfg.repo_path is not None
        code, out, _ = run_process(["git", *args], self.cfg.repo_path, self.cfg.timeouts["git"], 200_000)
        if code != 0:
            raise BridgeError(f"git {args[0]} failed:\n{out[-2000:]}")
        return out

    def checkout(self, ref: str) -> str:
        """Clean checkout of `ref`; returns the commit SHA."""
        assert self.cfg.repo_path is not None and self.cfg.repo_url is not None
        if not (self.cfg.repo_path / ".git").exists():
            self.cfg.repo_path.parent.mkdir(parents=True, exist_ok=True)
            code, out, _ = run_process(["git", "clone", "--quiet", self.cfg.repo_url, str(self.cfg.repo_path)],
                                       self.cfg.repo_path.parent, self.cfg.timeouts["git"], 200_000)
            if code != 0:
                raise BridgeError(f"git clone failed:\n{out[-2000:]}")
        self._git("fetch", "--quiet", "--force", "origin", ref)
        self._git("checkout", "--quiet", "--force", "--detach", "FETCH_HEAD")
        # Keep ESPHome's build cache; drop everything else that isn't tracked.
        self._git("clean", "-fdxq", "-e", ".esphome")
        for dst, src in self.cfg.extra_files.items():
            target = _inside(self.cfg.repo_path, dst)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, target)
        return self._git("rev-parse", "HEAD").strip()

    def execute(self, req: Request) -> JobResult:
        start = time.monotonic()
        if req.action == "status":
            enabled = ", ".join(self.cfg.enabled)
            devices = ", ".join(self.cfg.devices) or "none"
            return JobResult(True, f"Bridge `{self.cfg.handle}` is up. Actions: {enabled}. Devices: {devices}.", "", 0.0)
        base = validate(req, self.cfg)
        if req.action == "screenshot":
            if self.dry_run:
                return JobResult(True, f"📸 Screenshot would be taken on {self.cfg.api_host} (dry run).", "", 0.0)
            width, height, raw = capture_screenshot(self.cfg)
            png = rgb565_to_png(raw, width, height)
            name = time.strftime("screenshot-%Y%m%d-%H%M%S.png")
            return JobResult(True, f"📸 Screenshot of the running PAPP ({width}×{height}) from {self.cfg.api_host}.", "",
                             time.monotonic() - start, [(name, png, "image/png")])
        if req.action in DEVICE_API_ACTIONS:
            data = {"url": req.url or ""} if req.action == "launch" else {}
            served = ""
            if req.action == "launch" and req.url and req.url.startswith(PROXIED_PREFIXES) and not self.dry_run:
                data["url"] = proxy_url(self.cfg, req.url)
                served = f" (served from this machine as {data['url']})"
            if not self.dry_run:
                call_device_action(self.cfg, DEVICE_API_ACTIONS[req.action], data)
            what = f"`{req.action}`" + (f" `{req.url.rsplit('/', 1)[-1]}`" if req.url else "")
            return JobResult(True, f"✅ {what} sent to {self.cfg.api_host}{served}{' (dry run)' if self.dry_run else ''}.", "",
                             time.monotonic() - start)
        assert base is not None and req.yaml is not None
        where = "local config"
        if req.source == "repo":
            sha = "dry-run" if self.dry_run else self.checkout(req.ref)
            where = f"`{req.ref}` @ `{sha[:10]}`"
        yaml_path = _inside(base, req.yaml)
        if not self.dry_run and not yaml_path.exists():
            raise BridgeError(f"`{req.yaml}` does not exist at {where}.")
        steps: list[tuple[str, list[str], int, int | None]] = []
        if req.action in {"config", "compile", "upload", "run"}:
            timeout = self.cfg.timeouts["upload"] + self.cfg.timeouts["compile"] if req.action == "run" else self.cfg.timeouts.get(req.action, 900)
            steps.append((req.action, esphome_argv(self.cfg, req.action, yaml_path, req.device), timeout, None))
        if req.action in {"logs", "run"}:
            steps.append(("logs", esphome_argv(self.cfg, "logs", yaml_path, req.device), req.seconds + 30, req.seconds))
        log_parts: list[str] = []
        ok = True
        detail = ""
        for name, argv, timeout, stop_after in steps:
            log_parts.append(f"$ {' '.join(shlex.quote(a) for a in argv)}\n")
            if self.dry_run:
                log_parts.append("(dry run: not executed)\n")
                continue
            code, out, truncated = run_process(argv, yaml_path.parent, timeout, self.cfg.max_log_bytes, stop_after)
            log_parts.append(out + ("\n[log truncated]\n" if truncated else ""))
            if name == "logs" and stop_after is not None and code is None:
                detail = f"captured {stop_after}s of logs"
                continue
            if code != 0:
                ok = False
                detail = f"`{name}` {'timed out' if code is None else f'exited {code}'}"
                break
        log = mask("".join(log_parts), self.secrets)
        took = time.monotonic() - start
        verb = f"{req.action} `{req.yaml}`" + (f" → `{req.device}`" if req.device else "")
        summary = f"{'✅' if ok else '❌'} {verb} from {where}: {'ok' if ok else 'failed'}" + (f" ({detail})" if detail else "") + f" in {took:.0f}s."
        return JobResult(ok, summary, log, took)


# ── hub (EhGI MCP over streamable HTTP) ─────────────────────────────────────


class HubAuthError(RuntimeError):
    """The hub rejected the token; retrying will not help."""


AUTH_HELP = ("The hub rejected the bridge's token (HTTP {code}). Put the token of the bridge's own agent "
             "(Add an agent in the project, e.g. esp-bridge) in {env}, then run `check` again. "
             "A token that was rotated or whose agent was removed also gives this.")


class Hub:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.session: str | None = None
        self.next_id = 1

    def _post(self, payload: dict, timeout: int = 90) -> dict | None:
        headers = {"Authorization": f"Bearer {self.cfg.token}", "Content-Type": "application/json",
                   "Accept": "application/json, text/event-stream"}
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        request = urllib.request.Request(self.cfg.hub_url, data=json.dumps(payload).encode(), headers=headers, method="POST")
        try:
            response = urllib.request.urlopen(request, timeout=timeout)
        except urllib.error.HTTPError as error:
            if error.code in (401, 403):
                raise HubAuthError(AUTH_HELP.format(code=error.code, env=self.cfg.token_env)) from None
            raise
        with response:
            self.session = response.headers.get("Mcp-Session-Id") or self.session
            body = response.read().decode("utf-8", "replace")
        if "id" not in payload:
            return None
        for line in body.splitlines():
            if line.startswith("data: "):
                return json.loads(line[6:])
        return json.loads(body) if body.strip().startswith("{") else None

    def connect(self) -> None:
        self.session = None
        self._post({"jsonrpc": "2.0", "id": 0, "method": "initialize",
                    "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "esp-bridge", "version": "1"}}})
        self._post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def call(self, tool: str, args: dict, timeout: int = 90) -> dict:
        for attempt in range(2):
            try:
                if self.session is None:
                    self.connect()
                self.next_id += 1
                message = self._post({"jsonrpc": "2.0", "id": self.next_id, "method": "tools/call",
                                      "params": {"name": tool, "arguments": args}}, timeout)
                if message is None or "error" in message:
                    raise RuntimeError(f"{tool}: {message and message.get('error')}")
                text = (message.get("result", {}).get("content") or [{}])[0].get("text", "{}")
                return json.loads(text)
            except HubAuthError:
                raise
            except (urllib.error.URLError, RuntimeError, json.JSONDecodeError, TimeoutError) as error:
                self.session = None
                if attempt == 1:
                    raise RuntimeError(f"hub call {tool} failed: {error}") from error
        raise AssertionError("unreachable")

    def upload(self, name: str, content: bytes, content_type: str = "text/plain") -> str | None:
        origin = self.cfg.hub_url.split("/api/")[0]
        boundary = uuid.uuid4().hex
        body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"files\"; filename=\"{name}\"\r\n"
                f"Content-Type: {content_type}\r\n\r\n").encode() + content + f"\r\n--{boundary}--\r\n".encode()
        request = urllib.request.Request(f"{origin}/api/projects/{self.cfg.project_id}/attachments", data=body, method="POST",
                                         headers={"Authorization": f"Bearer {self.cfg.token}",
                                                  "Content-Type": f"multipart/form-data; boundary={boundary}"})
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.loads(response.read())["attachments"][0]["id"]
        except (urllib.error.URLError, KeyError, IndexError, json.JSONDecodeError) as error:
            print(f"log upload failed: {error}", file=sys.stderr)
            return None


# ── service loop ────────────────────────────────────────────────────────────


def tail(text: str, lines: int = TAIL_LINES) -> str:
    rows = text.rstrip().splitlines()
    return "\n".join(rows[-lines:]).replace("```", "'''")


def describe(req: "Request") -> str:
    """The request as a one-line command, for the bridge's terminal."""
    parts = [req.action]
    if req.yaml:
        parts.append(req.yaml)
    if req.action in NEEDS_YAML:
        parts.append(f"source={req.source}")
        if req.source == "repo":
            parts.append(f"ref={req.ref}")
    if req.device:
        parts.append(f"device={req.device}")
    if req.action in ("logs", "run"):
        parts.append(f"seconds={req.seconds}")
    if req.url:
        parts.append(f"url={req.url}")
    return " ".join(parts)


def console(cfg: Config, text: str) -> None:
    """Show what the bridge is asked and what it answers ([console] show_requests)."""
    if not cfg.show_requests:
        return
    line = f"[{time.strftime('%H:%M:%S')}] {text}"
    try:
        print(line, flush=True)
    except UnicodeEncodeError:  # a console without emoji support
        encoding = sys.stdout.encoding or "ascii"
        print(line.encode(encoding, "replace").decode(encoding), flush=True)


def handle_event(event: dict, cfg: Config, hub: Hub, runner: Runner) -> None:
    author = str(event.get("from", "")).lower()
    if author == cfg.handle.lower():
        return
    # Only people and agents send jobs. System and GitHub feed messages (a PR
    # description quoting a request, task updates) are never answered.
    if event.get("from_kind", "human") not in ("human", "agent"):
        return
    thread = event.get("thread_id") or event.get("id")
    channel = event.get("channel")
    try:
        req = parse_request(event.get("text") or "", event.get("data"), cfg.handle)
    except BridgeError as error:
        req, refusal = None, str(error)
    else:
        refusal = None
    if req is None and refusal is None:
        return
    if author not in cfg.allowed_requesters:
        refusal = f"@{author} is not allowed to send jobs to this bridge."
    # The hub rejects a message that mentions its own author, so never write @<own handle>.
    own = re.compile(re.escape(f"@{cfg.handle}"), re.IGNORECASE)
    reply = lambda text, **extra: hub.call("post_message", {"channel": channel, "thread_id": thread, "text": own.sub(cfg.handle, text), **extra})  # noqa: E731
    where = f"#{channel}" if channel else "hub"
    if refusal:
        console(cfg, f"@{author} in {where}: refused: {refusal}")
        reply(f"🚫 {refusal}")
        return
    assert req is not None
    console(cfg, f"@{author} in {where}: {describe(req)}")
    try:
        validate(req, cfg) if req.action != "status" else None
    except BridgeError as error:
        console(cfg, f"  refused: {error}")
        reply(f"🚫 {error}")
        return
    if req.action != "status":
        reply(f"⏳ @{author} `{req.action}` started" + (f" on `{req.yaml}`" if req.yaml else "") + ".", mentions=[author])
    hub.call("set_status", {"state": "working", "note": f"{req.action} {req.yaml or ''}".strip()})
    try:
        result = runner.execute(req)
    except BridgeError as error:
        result = JobResult(False, f"❌ `{req.action}` could not run: {error}", "", 0.0)
    except Exception as error:  # noqa: BLE001 - report anything unexpected instead of dying
        result = JobResult(False, f"❌ `{req.action}` crashed: {type(error).__name__}: {error}", "", 0.0)
    finally:
        hub.call("set_status", {"state": "online", "note": f"ESP bridge ready ({', '.join(cfg.enabled)})"})
    console(cfg, f"  {'ok' if result.ok else 'failed'} in {result.seconds:.1f}s: {result.summary}")
    attachments = [hub.upload(name, content, kind) for name, content, kind in result.files]
    if result.log:
        attachments.append(hub.upload(f"{req.action}-{int(time.time())}.log", result.log.encode()))
    attachments = [a for a in attachments if a]
    text = f"@{author} {result.summary}"
    if result.files and not attachments:
        text += " (The file could not be attached.)"
    if result.log:
        text += f"\n```\n{tail(result.log)}\n```"
    reply(text, mentions=[author], **({"attachment_ids": attachments[:6]} if attachments else {}))


def serve(cfg: Config, dry_run: bool) -> None:
    hub, runner = Hub(cfg), Runner(cfg, dry_run)
    briefing = hub.call("get_briefing", {})
    since = briefing.get("envelope", {}).get("latest_seq", 0)
    hub.call("set_status", {"state": "online", "note": f"ESP bridge ready ({', '.join(cfg.enabled)})" + (" [dry run]" if dry_run else "")})
    print(f"Listening as @{cfg.handle} from seq {since} (dry run: {dry_run})", flush=True)
    failures = 0
    while True:
        try:
            result = hub.call("wait_for_activity", {"since_seq": since, "max_wait_seconds": 45, "only_for_me": True}, timeout=75)
            failures = 0
        except HubAuthError:
            raise
        except RuntimeError as error:
            failures += 1
            print(error, file=sys.stderr, flush=True)
            time.sleep(min(60, 2 ** failures))
            continue
        if result.get("envelope", {}).get("stop_requested"):
            print("Stop requested from the hub; exiting.", flush=True)
            hub.call("set_status", {"state": "offline", "note": "ESP bridge stopped"})
            return
        for event in result.get("events", []):
            try:
                handle_event(event, cfg, hub, runner)
            except RuntimeError as error:
                print(f"event {event.get('seq')}: {error}", file=sys.stderr, flush=True)
        since = max(since, result.get("next_seq", since))


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="replace")  # consoles without emoji support
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--dry-run", action="store_true", help="validate and report, but never run git or esphome")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("check", help="connect to the hub and print who this bridge is")
    sub.add_parser("serve", help="run the bridge")
    local = sub.add_parser("local", help="run one request locally, without the hub")
    local.add_argument("request", help='e.g. "compile esphome/device.yaml ref=main"')
    args = parser.parse_args()
    cfg = Config.load(args.config, need_token=args.command != "local")
    try:
        return run_command(args, cfg)
    except HubAuthError as error:
        print(error, file=sys.stderr)
        return 3


def run_command(args: argparse.Namespace, cfg: Config) -> int:
    if args.command == "check":
        me = Hub(cfg).call("get_briefing", {}).get("me", {})
        print(f"Connected as @{me.get('handle')} ({me.get('agent_id') or me.get('id')}); configured handle @{cfg.handle}.")
        if me.get("handle") != cfg.handle:
            print("Warning: [hub].handle does not match the token's agent handle.", file=sys.stderr)
        return 0
    if args.command == "local":
        try:
            req = parse_request(f"@{cfg.handle} {args.request}", None, cfg.handle)
            if req is None:
                raise BridgeError("Empty request.")
            result = Runner(cfg, args.dry_run).execute(req)
        except BridgeError as error:
            print(f"refused: {error}", file=sys.stderr)
            return 2
        print(result.summary)
        print(result.log)
        return 0 if result.ok else 1
    serve(cfg, args.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
