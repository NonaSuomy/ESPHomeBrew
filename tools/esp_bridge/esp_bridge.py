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
    @esp-bridge launch url=https://github.com/NonaSuomy/esphomebrew/releases/download/…/app.papp
    @esp-bridge close
    @esp-bridge catalog
    @esp-bridge status
    @esp-bridge view device.yaml source=local
    @esp-bridge edit device.yaml source=local "find=refresh: 1d" "replace=refresh: 0s"   # edit_requesters only
    @esp-bridge writefile path=/sd/roms/redalert/redalert.ini   # edit_requesters only; the new
                                                               # content is the message's code block
    @esp-bridge launch url=… seconds=30 serial=/dev/ttyUSB0   # also read the serial port
    @esp-bridge reset serial=/dev/ttyUSB0 seconds=10          # hard reset over serial + boot log
launch/close/catalog call the papp_loader API actions from esphome/device_control.yaml
over the ESPHome native API (needs aioesphomeapi, which the ESPHome venv already has).
With seconds=N they also return N seconds of device log; serial= (a port from
[esphome].devices) adds the serial console, where a crash's full panic dump goes.
With [fallback] enabled, a device whose network API fails is reset over serial
(then the job is retried over the network), and an OTA upload is flashed over
serial instead.
Agents may instead send the same fields as message data: {"esp_bridge": {...}}.

Usage:
    python esp_bridge.py --config bridge.toml check      # hub connectivity
    python esp_bridge.py --config bridge.toml serve      # run the bridge
    python esp_bridge.py --config bridge.toml --dry-run serve   # never runs esphome
    python esp_bridge.py --config bridge.toml local "compile esphome/device.yaml ref=main"
"""

from __future__ import annotations

import argparse
import difflib
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

ACTIONS = ("status", "config", "compile", "upload", "logs", "run", "launch", "close", "catalog", "screenshot",
           "readfile", "writefile", "deletefile", "view", "edit", "reset")
# Bridge action -> ESPHome API action (esphome/device_control.yaml).
DEVICE_API_ACTIONS = {"launch": "papp_launch", "close": "papp_close", "catalog": "papp_refresh_catalog",
                      "screenshot": "papp_screenshot", "readfile": "papp_read_file",
                      "writefile": "papp_write_file", "deletefile": "papp_delete_file"}
NEEDS_YAML = {"config", "compile", "upload", "logs", "run", "view", "edit"}
EDIT_TEXT_MAX = 4000  # characters in `find` / `replace`
# writefile: small text files only (a written .papp or firmware image would be
# code); the loader checks the same list.
WRITEFILE_MAX_BYTES = 16 * 1024
WRITEFILE_EXTENSIONS = (".ini", ".cfg", ".conf", ".txt", ".json", ".yaml", ".yml", ".csv")
# deletefile also takes leftovers, but never game data (.mix) or apps (.papp).
DELETEFILE_EXTENSIONS = (*WRITEFILE_EXTENSIONS, ".bak", ".log")
CODE_FENCE = re.compile(r"```[^\n`]*\n(.*?)```", re.S)
MASKED_VALUE = re.compile(r"(?m)[:=]\s*\*\*\*\s*$")
# Values of keys like these are hidden when a YAML is shown (`!secret` names stay visible).
INLINE_SECRET = re.compile(r"(?im)^(\s*-?\s*[\w.-]*(?:password|passwd|psk|key|token|secret)[\w.-]*\s*:\s*)(?!!secret\b)(\S.*)$")
NEEDS_DEVICE = {"upload", "logs", "run"}
REF_RE = re.compile(r"^(?!-)(?!.*\.\.)[A-Za-z0-9._/-]{1,120}$")
TAIL_LINES = 40

# Serial fallback ([fallback]): when the device's network path fails, these
# actions reset the device over serial and are retried once over the network;
# an OTA `upload` is flashed over serial instead.
FALLBACK_RESET_ACTIONS = {"launch", "close", "catalog", "screenshot", "logs"}
RESET_PULSE_SECONDS = 0.1   # EN held low, as esptool's hard reset does
NETWORK_RETRY_DELAY = 2.0   # one quick network retry of an API action before any reset
PROBE_TIMEOUT = 5.0         # the light API connect check
PROBE_INTERVAL = 3.0        # between checks while waiting for the device to come back
# An OTA upload that failed on the network (not on a wrong password or a bad image).
OTA_NETWORK_FAILURE = re.compile(
    r"Connecting to \S+ port \d+ failed|Error connecting to|Connection (?:reset|refused|aborted|timed out)|timed out|"
    r"No route to host|(?:Host|Network) is unreachable|Broken pipe|Error resolving IP address|Errno (?:32|104|110|111|113)\b",
    re.I)
# `esphome logs` over the API keeps reconnecting, so a dead network shows as
# connection warnings and no "Successfully connected" line.
LOGS_NETWORK_FAILURE = re.compile(r"Can't connect to ESPHome API|Error connecting to|Connection (?:reset|refused)|"
                                  r"No route to host|(?:Host|Network) is unreachable", re.I)
LOGS_CONNECTED = re.compile(r"Successfully connected to")
# aioesphomeapi errors a reset can't fix (a wrong key or password, another device at that address).
NOT_NETWORK_ERRORS = ("auth", "encryption", "badname", "badmac")


class BridgeError(Exception):
    """A request that is refused or cannot run; the message is shown to the requester."""

    def __init__(self, message: str = "", log: str = ""):
        super().__init__(message)
        self.log = log  # attached to the reply (e.g. what a failed fallback saw)


class DeviceUnreachable(BridgeError):
    """The device's network path failed (refused, reset, timed out): the serial fallback may help."""


def is_serial_port(name: str | None) -> bool:
    """A serial port name (/dev/ttyUSB0, COM3), as opposed to a network address."""
    return bool(name) and (name.startswith("/dev/") or re.fullmatch(r"COM\d+", name, re.I) is not None)


def is_network_error(error: BaseException) -> bool:
    """Connection-class failures (refused, reset, timed out, unreachable) that a reset may get around.

    A wrong API key or password, another device at the address, or a device
    without the needed action are not: resetting would not change them.
    """
    if isinstance(error, BridgeError):
        return isinstance(error, DeviceUnreachable)
    names = [cls.__name__.lower() for cls in type(error).__mro__]
    if any(word in name for name in names for word in NOT_NETWORK_ERRORS):
        return False
    return isinstance(error, OSError) or "apiconnectionerror" in names  # TimeoutError is an OSError


def network_failure(action: str, log: str, ok: bool) -> str | None:
    """Why an OTA `upload` or network `logs` run failed on the network, or None if it didn't."""
    if action == "upload" and not ok:
        found = OTA_NETWORK_FAILURE.search(log)
    elif action == "logs" and not LOGS_CONNECTED.search(log):
        found = LOGS_NETWORK_FAILURE.search(log)
    else:
        return None
    if not found:
        return None
    start = log.rfind("\n", 0, found.start()) + 1
    end = log.find("\n", found.end())
    return ANSI.sub("", log[start:end if end >= 0 else len(log)]).strip()[:300]


# ── configuration ───────────────────────────────────────────────────────────


def read_agent_token(token_env: str, need_token: bool = True) -> str:
    """The bridge agent's hub token from the environment; refuses placeholders before connecting."""
    token = os.environ.get(token_env, "").strip()
    if need_token and not token:
        raise SystemExit(f"Set {token_env} to the bridge agent's token.")
    if need_token and (not token.startswith("ac_") or "PASTE" in token.upper() or "…" in token):
        raise SystemExit(f"{token_env} does not look like an agent token. It should be the ac_... token shown when you "
                         "add the bridge's agent in the project, not a placeholder.")
    return token


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
    screen_port: int = 3233
    proxy_port: int = 8765
    allowed_url_prefixes: list[str] = field(default_factory=list)
    token_env: str = "EHGI_BRIDGE_TOKEN"
    show_requests: bool = True
    firmware_elf: list[str] = field(default_factory=list)  # globs; default: the local/repo build dirs
    addr2line: str | None = None
    edit_requesters: list[str] = field(default_factory=list)  # who may `edit` local YAML (none by default)
    fallback_serial: str | None = None  # [fallback] serial: default `reset` port, and the fallback's port
    fallback_enabled: bool = False      # [fallback] enabled: fall back to serial when the network fails
    fallback_wait: int = 60             # seconds to wait for the device's API after a reset
    fallback_recheck: int = 30          # while the network is down, check it at most this often

    @staticmethod
    def load(path: Path, *, need_token: bool = True) -> "Config":
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
        hub, repo, local, esp = raw.get("hub", {}), raw.get("repo", {}), raw.get("local", {}), raw.get("esphome", {})
        exp = lambda p: Path(os.path.expanduser(p)).resolve() if p else None  # noqa: E731
        token_env = hub.get("token_env", "EHGI_BRIDGE_TOKEN")
        token = read_agent_token(token_env, need_token)
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
        cfg.screen_port = int(api.get("screen_port", 3233))
        cfg.proxy_port = int(api.get("proxy_port", 8765))
        cfg.show_requests = bool(raw.get("console", {}).get("show_requests", True))
        cfg.edit_requesters = [h.lstrip("@").lower() for h in raw.get("actions", {}).get("edit_requesters", [])]
        elf = api.get("firmware_elf", [])
        cfg.firmware_elf = [elf] if isinstance(elf, str) else list(elf)
        cfg.addr2line = api.get("addr2line")
        env_key = os.environ.get(api["encryption_key_env"], "") if api.get("encryption_key_env") else ""
        cfg.api_key = env_key or load_secret_map(cfg.secrets_files).get(api.get("encryption_key_secret", "")) or None
        cfg.allowed_url_prefixes = list(api.get("allowed_url_prefixes", [
            "https://github.com/NonaSuomy/esphomebrew/releases/download/",
            "https://nonasuomy.github.io/esphomebrew/",
        ]))
        fallback = raw.get("fallback", {})
        cfg.fallback_serial = fallback.get("serial") or None
        cfg.fallback_enabled = bool(fallback.get("enabled", False))
        cfg.fallback_wait = int(fallback.get("wait_seconds", 60))
        cfg.fallback_recheck = int(fallback.get("recheck_seconds", 30))
        if cfg.fallback_serial and (cfg.fallback_serial not in cfg.devices or not is_serial_port(cfg.fallback_serial)):
            raise SystemExit(f"[fallback].serial `{cfg.fallback_serial}` must be a serial port that is also listed in "
                             "[esphome].devices.")
        if cfg.fallback_enabled and not cfg.fallback_serial:
            raise SystemExit("[fallback] is enabled but has no `serial` port.")
        return cfg

    def fallback_port(self) -> str | None:
        """The serial port the automatic fallback may use, or None when it is off."""
        port = self.fallback_serial
        if self.fallback_enabled and port and port in self.devices and is_serial_port(port):
            return port
        return None


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
    seconds_given: bool = False  # launch/close only capture device logs when asked
    serial: str | None = None  # also read this serial port during launch/close
    path: str | None = None  # readfile: a file or directory (ending in /) on the SD card
    find: str | None = None  # edit: text that must occur exactly once in the YAML
    replace: str | None = None  # edit: what replaces it
    requester: str = ""  # handle that sent the request (edit is limited to [actions].edit_requesters)
    content: str | None = None  # writefile: the new file (the message's code block)


def request_words(text: str, handle: str, actions: tuple[str, ...] = ACTIONS) -> list[str] | None:
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
        if words[0] in actions:
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
        block = CODE_FENCE.search(text)
        if block:
            fields["content"] = block.group(1)
    action = fields.get("action", "").lower()
    if action not in ACTIONS:
        raise BridgeError(f"Unknown action `{action}`. Use one of: {', '.join(ACTIONS)}.")
    try:
        seconds = int(fields.get("seconds", "60"))
    except ValueError:
        raise BridgeError("`seconds` must be a whole number.")
    return Request(action=action, yaml=fields.get("yaml"), ref=fields.get("ref", "main"),
                   source=fields.get("source", "repo").lower(), device=fields.get("device"), seconds=seconds,
                   url=fields.get("url"), seconds_given="seconds" in fields, serial=fields.get("serial"),
                   path=fields.get("path"), find=fields.get("find"), replace=fields.get("replace"),
                   content=fields.get("content"))


def validate_writefile(req: Request, cfg: Config) -> None:
    """writefile: a small text file under /sd/, from someone allowed to change the device."""
    if req.requester not in cfg.edit_requesters:
        raise BridgeError(f"@{req.requester} may not write device files on this bridge (see [actions].edit_requesters).")
    path = req.path or ""
    if (not path.startswith("/sd/") or ".." in path or path.endswith("/") or len(path) >= READFILE_PATH_MAX
            or any(c.isspace() or not c.isprintable() for c in path)):
        raise BridgeError("`writefile` needs `path=/sd/…` naming a file.")
    if not path.lower().endswith(WRITEFILE_EXTENSIONS):
        raise BridgeError(f"`writefile` only writes text files ({', '.join(WRITEFILE_EXTENSIONS)}).")
    if req.content is None:
        raise BridgeError("`writefile` takes the new content from a ``` code block in the same message.")
    if len(req.content.encode("utf-8")) > WRITEFILE_MAX_BYTES or chr(0) in req.content:
        raise BridgeError(f"`writefile` content must be text of at most {WRITEFILE_MAX_BYTES // 1024} KiB.")
    if MASKED_VALUE.search(req.content):
        raise BridgeError("The content has a hidden value (`***`); write the real value, or leave that line out.")


def validate_deletefile(req: Request, cfg: Config) -> None:
    """deletefile: one text or leftover file under /sd/, from someone allowed to change the device."""
    if req.requester not in cfg.edit_requesters:
        raise BridgeError(f"@{req.requester} may not delete device files on this bridge (see [actions].edit_requesters).")
    path = req.path or ""
    if (not path.startswith("/sd/") or ".." in path or path.endswith("/") or len(path) >= READFILE_PATH_MAX
            or any(c.isspace() or not c.isprintable() for c in path)):
        raise BridgeError("`deletefile` needs `path=/sd/…` naming one file.")
    if not path.lower().endswith(DELETEFILE_EXTENSIONS):
        raise BridgeError(f"`deletefile` only removes text and leftover files ({', '.join(DELETEFILE_EXTENSIONS)}).")


def reset_target(req: Request, cfg: Config) -> tuple[str, str | None]:
    """`reset`: (the serial port to pulse, the network address to wait for or None).

    `serial=` names the port ([fallback].serial when left out); `device=` is
    either that port again or the device's network address, whose API the
    bridge then waits for. The port must be in [esphome].devices; the address
    too, or be the [device_api] host.
    """
    wait_for = None
    port = req.serial
    if req.device:
        if is_serial_port(req.device):
            if port and port != req.device:
                raise BridgeError("`reset` got two different serial ports; give one with `serial=`.")
            port = req.device
        elif req.device in cfg.devices or req.device == cfg.api_host:
            wait_for = req.device
        else:
            raise BridgeError(f"Device `{req.device}` is not in this bridge's allowlist.")
    port = port or cfg.fallback_serial
    if not port:
        raise BridgeError(f"`reset` needs `serial=` (a serial port from: {', '.join(cfg.devices) or 'none'}).")
    if port not in cfg.devices:
        raise BridgeError(f"Serial port `{port}` is not in this bridge's allowlist.")
    if not is_serial_port(port):
        raise BridgeError(f"`reset` works through a serial port (/dev/tty…), and `{port}` is a network address.")
    return port, wait_for


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
    if (req.action in {"logs", "run"} or req.seconds_given) and not 5 <= req.seconds <= cfg.max_log_seconds:
        raise BridgeError(f"`seconds` must be between 5 and {cfg.max_log_seconds}.")
    if req.action == "reset":
        reset_target(req, cfg)
        return None
    if req.serial is not None:
        if req.action not in ("launch", "close", "readfile") or not req.seconds_given:
            raise BridgeError("`serial=` goes with `launch`/`close`/`readfile` and `seconds=`, or with `reset`.")
        if req.serial not in cfg.devices:
            raise BridgeError(f"Serial port `{req.serial}` is not in this bridge's allowlist.")
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
        if req.action == "readfile":
            path = req.path or ""
            if (not path.startswith("/sd/") or ".." in path or len(path) >= READFILE_PATH_MAX
                    or any(c.isspace() or not c.isprintable() for c in path)):
                raise BridgeError("`readfile` needs `path=/sd/…` (a file, or a directory ending in `/`).")
        if req.action == "writefile":
            validate_writefile(req, cfg)
        if req.action == "deletefile":
            validate_deletefile(req, cfg)
        return None
    if (req.action in {"logs", "run"} or req.seconds_given) and not 5 <= req.seconds <= cfg.max_log_seconds:
        raise BridgeError(f"`seconds` must be between 5 and {cfg.max_log_seconds}.")
    if req.action not in NEEDS_YAML:
        return None
    if not req.yaml:
        raise BridgeError(f"`{req.action}` needs a YAML file.")
    if req.action in ("view", "edit") and Path(req.yaml).name.lower().startswith("secrets"):
        raise BridgeError("Secrets files can't be shown or edited through the bridge.")
    if req.action == "edit":
        # An edited config can pull in external components that run code here when it is
        # compiled, so edits are limited to the handles the bridge owner lists.
        if req.requester not in cfg.edit_requesters:
            raise BridgeError(f"@{req.requester} may not edit YAML on this bridge (see [actions].edit_requesters).")
        if req.source != "local":
            raise BridgeError("`edit` only changes files in the local directory (`source=local`); change the repo with a PR.")
        if not req.find or req.replace is None:
            raise BridgeError("`edit` needs `find` (text that occurs exactly once) and `replace`.")
        if len(req.find) > EDIT_TEXT_MAX or len(req.replace) > EDIT_TEXT_MAX:
            raise BridgeError(f"`find` and `replace` are limited to {EDIT_TEXT_MAX} characters each.")
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


ANSI = re.compile(r"\x1b\[[0-9;]*m")


def call_device_action(cfg: Config, action: str, data: dict[str, str], timeout: float = 30.0,
                       log_seconds: int = 0) -> str:
    """Execute an ESPHome API action on the configured device.

    With log_seconds, the device's log is also captured over the same
    connection, from just before the action for that many seconds, and returned
    (what an app prints while it starts or crashes).
    """
    import asyncio
    import inspect

    try:
        from aioesphomeapi import APIClient
    except ImportError as error:
        raise BridgeError("aioesphomeapi is missing; run the bridge with the ESPHome venv's python.") from error

    # Which step was running when the time ran out tells a name/network problem
    # (resolving, connecting) from a device that is up but slow (listing, running).
    stage = ["resolving the name"]

    captured: list[str] = []
    done = [False]  # the action itself went through

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
            unsubscribe = None
            if log_seconds:
                import aioesphomeapi

                def on_log(message) -> None:
                    text = message.message
                    text = text.decode("utf-8", "replace") if isinstance(text, (bytes, bytearray)) else str(text)
                    captured.append(ANSI.sub("", text).rstrip())

                level = getattr(getattr(aioesphomeapi, "LogLevel", None), "LOG_LEVEL_DEBUG", None)
                unsubscribe = client.subscribe_logs(on_log, log_level=level)
            stage[0] = f"running `{action}`"
            result = client.execute_service(service, data)
            if inspect.isawaitable(result):
                await result
            done[0] = True
            if log_seconds:
                stage[0] = f"capturing {log_seconds}s of device log"
                await asyncio.sleep(log_seconds)
                if callable(unsubscribe):
                    unsubscribe()
        finally:
            try:
                await client.disconnect()
            except Exception:  # noqa: BLE001 - the device may have rebooted meanwhile
                pass

    try:
        asyncio.run(asyncio.wait_for(go(), timeout + log_seconds))
    except BridgeError:
        raise
    except Exception as error:  # noqa: BLE001
        if done[0] and log_seconds:
            # The action ran; losing the log connection afterwards (a crash
            # rebooting the device) is exactly what the caller wants to see.
            captured.append(f"[esp-bridge] device log ended early: {type(error).__name__}: {error}")
            return "\n".join(captured)
        if isinstance(error, asyncio.TimeoutError):
            raise DeviceUnreachable(f"The device at {cfg.api_host} did not answer within {timeout:.0f}s "
                                    f"(stuck while {stage[0]}).") from error
        kind = DeviceUnreachable if is_network_error(error) else BridgeError
        raise kind(f"Device API call failed while {stage[0]}: {type(error).__name__}: {error}") from error
    return "\n".join(captured)


def probe_network(cfg: Config, host: str, timeout: float = PROBE_TIMEOUT) -> str | None:
    """A light check of the network path to the device: connect to its API, then hang up.

    Returns None when the device answers (a refused login still proves the
    path works), otherwise why it did not.
    """
    import asyncio

    try:
        from aioesphomeapi import APIClient
    except ImportError as error:
        raise BridgeError("aioesphomeapi is missing; run the bridge with the ESPHome venv's python.") from error

    async def go() -> None:
        address = await asyncio.get_running_loop().run_in_executor(None, resolve_host, host, cfg.api_port)
        client = APIClient(address, cfg.api_port, None, noise_psk=cfg.api_key, client_info="esp-bridge")
        await client.connect(login=True)
        try:
            await client.disconnect()
        except Exception:  # noqa: BLE001 - it answered; that is all this checks
            pass

    try:
        asyncio.run(asyncio.wait_for(go(), timeout))
    except Exception as error:  # noqa: BLE001
        if isinstance(error, asyncio.TimeoutError):
            return f"no answer from {host}:{cfg.api_port} within {timeout:.0f}s"
        if is_network_error(error):
            return f"{type(error).__name__}: {error}"
    return None


def pulse_reset(port, sleep=time.sleep) -> None:
    """Hard-reset an ESP32 through a USB-serial adapter's control lines, as esptool does.

    The usual auto-program circuit ties RTS to EN and DTR to IO0; an asserted
    line pulls its pin low. IO0 stays high, so the chip boots its firmware
    rather than the ROM download mode, while EN is held low for 100 ms.
    """
    port.dtr = False        # IO0 high: a normal boot
    port.rts = True         # EN low: the chip is held in reset
    port.dtr = port.dtr     # Windows' usbser.sys only applies RTS with a DTR write (esptool does the same)
    sleep(RESET_PULSE_SECONDS)
    port.rts = False        # EN high: the chip starts
    port.dtr = port.dtr


class SerialCapture:
    """Read a serial port in the background (the full panic dump only goes there).

    DTR/RTS stay low so opening the port does not reset the board. With
    reset=True the board is then hard-reset (pulse_reset) and its boot log is
    what gets captured. Needs pyserial, which the ESPHome venv has.
    """

    def __init__(self, port: str, max_bytes: int, baud: int = 115200, reset: bool = False):
        self.port, self.max_bytes, self.baud = port, max_bytes, baud
        self.reset, self.reset_done = reset, False
        self.data = bytearray()
        self.error: str | None = None
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        try:
            import serial  # pyserial
        except ImportError:
            self.error = "pyserial is missing; run the bridge with the ESPHome venv's python"
            return
        try:
            port = serial.Serial()
            port.port, port.baudrate, port.timeout = self.port, self.baud, 0.2
            port.dtr = False
            port.rts = False
            port.open()
        except Exception as error:  # noqa: BLE001 - busy port, missing device, permissions
            self.error = f"could not open {self.port}: {error}"
            return
        if self.reset:
            try:
                pulse_reset(port)
                self.reset_done = True
            except Exception as error:  # noqa: BLE001 - an adapter without modem control lines
                self.error = f"could not reset through {self.port}: {error}"
                port.close()
                return

        def reader() -> None:
            try:
                while not self._stop.is_set():
                    chunk = port.read(4096)
                    if chunk and len(self.data) < self.max_bytes:
                        self.data += chunk[: self.max_bytes - len(self.data)]
            except Exception as error:  # noqa: BLE001
                self.error = f"reading {self.port} failed: {error}"
            finally:
                port.close()

        self._thread = threading.Thread(target=reader, daemon=True)
        self._thread.start()

    def stop(self) -> str:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)
        text = ANSI.sub("", self.data.decode("utf-8", "replace")).replace("\r\n", "\n")
        if self.error:
            text += f"\n[esp-bridge] serial: {self.error}"
        return text


# ── crash decoding ──────────────────────────────────────────────────────────
# A panic dump is only addresses. Firmware addresses are looked up in the
# firmware ELF this machine built (the dump's "ELF file SHA256" picks the
# right one); PAPP addresses (linked at 0x4A000000) in the app's symbol list,
# which dev builds publish next to the .papp.

# A live panic dump (serial), or the report ESPHome's esp32 crash handler logs
# after the reboot ("[E][esp32.crash:...]:   BT0: 0x4FF0A146 (backtrace)").
CRASH = re.compile(r"abort\(\) was called|Guru Meditation|panic'ed|^MEPC\s*:|^Backtrace:|esp32\.crash\S*:\s+(?:PC|BT\d+):", re.M)
# The lines of a serial panic that say what failed (the assert text only goes to serial).
PANIC_LINE = re.compile(r"assert failed|abort\(\) was called|Guru Meditation|panic'ed|stack overflow|"
                        r"MSPI PSRAM error|psram (?:pms|read address|rx|tx)|Stack protection|^E \w+: ", re.M)
ELF_SHA = re.compile(r"ELF file SHA256:\s*([0-9a-fA-F]{8,64})")
HEX = re.compile(r"0x([0-9a-fA-F]{8})\b")
PAPP_BASE, PAPP_END = 0x4A000000, 0x4C000000
PSRAM = (0x48000000, 0x4C000000)  # ESP32-P4: stacks and heap there are data, not code
DEFAULT_ELF_GLOBS = [".esphome/build/*/.pioenvs/*/firmware.elf", ".esphome/build/*/build/*.elf"]
ADDR2LINE_GLOBS = [
    "~/.platformio/packages/toolchain-*/bin/*-elf-addr2line",
    "~/.espressif/tools/*/*/*/bin/*-elf-addr2line",
    "~/.esphome/**/bin/*-elf-addr2line",
]


def crash_addresses(text: str) -> tuple[list[int], list[int]]:
    """Code-looking addresses in the first panic dump: (firmware, app), in order."""
    found = CRASH.search(text)
    if not found:
        return [], []
    dump = text[max(0, text.rfind("\n", 0, found.start())):]
    end = dump.find("Rebooting...")
    dump = dump[:end] if end > 0 else dump[:20000]
    firmware, app = [], []
    for match in HEX.finditer(dump):
        address = int(match.group(1), 16)
        if PAPP_BASE <= address < PAPP_END:
            if address not in app:
                app.append(address)
        elif 0x40000000 <= address < 0x50000000 and not PSRAM[0] <= address < PSRAM[1] and address not in firmware:
            firmware.append(address)
    return firmware[:80], app[:80]


def find_firmware_elf(cfg: Config, sha_prefix: str | None) -> tuple[Path | None, str]:
    import glob

    patterns = [os.path.expanduser(p) for p in cfg.firmware_elf]
    for base in (cfg.local_dir, cfg.repo_path / "esphome" if cfg.repo_path else None):
        if base and not cfg.firmware_elf:
            patterns += [str(base / p) for p in DEFAULT_ELF_GLOBS]
    candidates = sorted({Path(p) for pat in patterns for p in glob.glob(pat, recursive=True)},
                        key=lambda p: p.stat().st_mtime, reverse=True)
    if not candidates:
        return None, "no firmware ELF found (set [device_api] firmware_elf)"
    if not sha_prefix:
        return candidates[0], f"{candidates[0]} (newest; the dump had no ELF SHA256 to check it against)"
    for path in candidates:
        if hashlib.sha256(path.read_bytes()).hexdigest().startswith(sha_prefix.lower()):
            return path, str(path)
    return None, f"none of the {len(candidates)} firmware ELFs here has SHA256 {sha_prefix} (the device runs another build)"


def find_addr2line(cfg: Config, elf: Path) -> str | None:
    import glob

    if cfg.addr2line:
        return os.path.expanduser(cfg.addr2line)
    with open(elf, "rb") as f:
        machine = struct.unpack_from("<H", f.read(20), 18)[0]
    arch = "riscv32" if machine == 243 else "xtensa"
    tools = [shutil.which(f"{arch}-esp-elf-addr2line")]
    tools += [p for pat in ADDR2LINE_GLOBS for p in sorted(glob.glob(os.path.expanduser(pat), recursive=True))]
    return next((t for t in tools if t and Path(t).name.startswith(arch)), None)


def load_papp_symbols(url: str | None, cfg: Config, opener=urllib.request.urlopen) -> list[tuple[int, str, bool]]:
    """(address, name, is_code) from the .sym published next to a dev-build .papp."""
    if not url or not url.endswith(".papp") or not url.startswith(tuple(cfg.allowed_url_prefixes)):
        return []
    try:
        with opener(url[: -len(".papp")] + ".sym", timeout=60) as response:
            text = response.read(16 * 1024 * 1024).decode("utf-8", "replace")
    except (urllib.error.URLError, TimeoutError, OSError):
        return []
    symbols = []
    for line in text.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and len(parts[1]) == 1 and re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            symbols.append((int(parts[0], 16), parts[2], parts[1] in "TtWw"))
    return sorted(symbols)


def decode_crash(cfg: Config, text: str, url: str | None = None, opener=urllib.request.urlopen) -> str:
    """Readable backtrace for a panic dump in `text`, or "" when there is none."""
    firmware, app = crash_addresses(text)
    if not firmware and not app:
        return ""
    out = []
    if app:
        symbols = load_papp_symbols(url, cfg, opener)
        if symbols:
            import bisect

            starts = [s[0] for s in symbols]
            out.append("PAPP (app symbols):")
            for address in app:
                i = bisect.bisect_right(starts, address) - 1
                base, name, code = symbols[i] if i >= 0 else (address, "", False)
                if name and address - base < 0x4000:  # further away: unnamed data (strings, tables)
                    out.append(f"  0x{address:08x}  {'' if code else 'data: '}{name} + 0x{address - base:x}")
        else:
            out.append("PAPP addresses (no symbol list for this app): " + " ".join(f"0x{a:08x}" for a in app))
    if firmware:
        sha = ELF_SHA.search(text)
        elf, where = find_firmware_elf(cfg, sha.group(1) if sha else None)
        tool = find_addr2line(cfg, elf) if elf else None
        if elf and tool:
            try:
                result = subprocess.run([tool, "-pfiaC", "-e", str(elf), *(f"0x{a:08x}" for a in firmware)],
                                        capture_output=True, text=True, timeout=60)
                lines = [ln for ln in result.stdout.splitlines() if "?? ??:0" not in ln and ": ?? at" not in ln]
                out.append(f"Firmware ({where}):")
                out += [f"  {ln}" for ln in lines] or ["  (no code addresses)"]
            except (OSError, subprocess.SubprocessError) as error:
                out.append(f"Firmware: addr2line failed: {error}")
        else:
            out.append(f"Firmware addresses ({where if not elf else 'no addr2line found; set [device_api] addr2line'}): "
                       + " ".join(f"0x{a:08x}" for a in firmware[:24]))
    return "\n".join(out)


# ── screenshots (papp_loader diagnostic stream, TCP port 3233) ──────────────

STREAM_HEADER = struct.Struct("<8sHHII")         # PAPPFB01 thumbnail / PAPPSS01 screenshot
STREAM_AUDIO_HEADER = struct.Struct("<8sIHHII")  # PAPPAU01
STREAM_FILE_HEADER = struct.Struct("<8sIII")      # PAPPFL01: status, size, sequence
READFILE_PATH_MAX = 128   # the loader's request buffer, including the terminator
READFILE_INLINE_CHARS = 15000
FILE_STATUS = {1: "not found", 2: "larger than the loader sends (1 MiB)", 3: "not an allowed path",
               4: "could not be read"}


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


def read_stream_file(sock: socket.socket) -> bytes:
    """Read stream packets until the PAPPFL01 file packet; other packets are skipped."""
    def exact(size: int) -> bytes:
        data = bytearray()
        while len(data) < size:
            part = sock.recv(min(65536, size - len(data)))
            if not part:
                raise BridgeError("The device closed the stream before the file arrived.")
            data += part
        return bytes(data)

    while True:
        magic = exact(8)
        if magic == b"PAPPFL01":
            _, status, size, _seq = STREAM_FILE_HEADER.unpack(magic + exact(STREAM_FILE_HEADER.size - 8))
            if size > 1024 * 1024:
                raise BridgeError("The device sent a malformed file header.")
            payload = exact(size)
            if status:
                raise BridgeError(f"The file {FILE_STATUS.get(status, f'failed (status {status})')}.")
            return payload
        if magic in (b"PAPPFB01", b"PAPPSS01"):
            _, width, height, size, _seq = STREAM_HEADER.unpack(magic + exact(STREAM_HEADER.size - 8))
            if size > 4 * 1024 * 1024:
                raise BridgeError("The device sent a malformed screen stream header.")
            exact(size)
        elif magic == b"PAPPAU01":
            header = STREAM_AUDIO_HEADER.unpack(magic + exact(STREAM_AUDIO_HEADER.size - 8))
            exact(header[4])
        else:
            raise BridgeError(f"Unexpected data on the stream ({magic!r}). Does the device's loader have `readfile`?")


def capture_file(cfg: Config, path: str, timeout: float = 20.0) -> bytes:
    """Ask the device for one SD card file (or directory listing) and read it from the stream."""
    call_device_action(cfg, DEVICE_API_ACTIONS["readfile"], {"path": path})
    address = resolve_host(cfg.api_host, cfg.screen_port)
    try:
        with socket.create_connection((address, cfg.screen_port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            return read_stream_file(sock)
    except (socket.timeout, TimeoutError) as error:
        raise BridgeError(f"No file arrived from {cfg.api_host}:{cfg.screen_port} within {timeout:.0f}s.") from error
    except OSError as error:
        raise BridgeError(f"Could not open the stream on {cfg.api_host}:{cfg.screen_port}: {error}.") from error


def hide_inline_secrets(text: str) -> str:
    """Hide values written straight into a YAML under password/key/token-like keys."""
    return INLINE_SECRET.sub(lambda m: m.group(1) + "***", text)


def code_block(text: str, lang: str = "") -> str:
    """A chat code block whose lines can't mention anyone, become bridge requests or end it early."""
    shown = text[:READFILE_INLINE_CHARS].replace("@", "@\u200b").replace("```", "``\u200b`")
    more = f"\n(first {READFILE_INLINE_CHARS} of {len(text)} characters)" if len(text) > READFILE_INLINE_CHARS else ""
    return f"```{lang}\n{shown}\n```{more}"


def describe_file(path: str, content: bytes) -> str:
    """Chat text for a read file. Text is shown inline; other files only by size and hash,
    so game data never leaves the device through the chat."""
    try:
        text = content.decode("utf-8")
    except UnicodeDecodeError:
        text = None
    if text is None or "\0" in text:
        digest = hashlib.sha256(content).hexdigest()
        return f"📄 `{path}`: binary, {len(content)} bytes, sha256 `{digest[:16]}…` (not shown)."
    kind = "directory listing" if path.endswith("/") else f"{len(content)} bytes"
    return f"📄 `{path}` ({kind}):\n{code_block(text)}"


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


@dataclass
class StepsResult:
    """The outcome of a job's esphome commands."""
    ok: bool
    detail: str
    log: str
    device: str | None = None          # where they ran (the serial port after a serial upload)
    network_error: str | None = None   # set when an OTA upload / network logs failed on the network


@dataclass
class NetworkHealth:
    """The bridge's view of the network path to one device (in memory only)."""
    down_since: float | None = None  # time.time() of the failure; None while it works
    reason: str = ""
    last_check: float = 0.0          # Runner.clock() of the last network attempt or check


@dataclass
class Fallback:
    """What the serial fallback did during one job, for the reply and the log."""
    notes: list[str] = field(default_factory=list)
    log: str = ""

    def line(self) -> str:
        return "🔌 " + "; ".join(self.notes) if self.notes else ""

    def prefix(self) -> str:
        return self.line() + "\n" if self.notes else ""


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
        self.health: dict[str, NetworkHealth] = {}  # network address -> state of the path to it
        self.clock, self.sleep = time.monotonic, time.sleep  # tests replace these

    # ── network health and the serial fallback ──

    def mark_down(self, host: str, reason: str) -> None:
        health = self.health.setdefault(host, NetworkHealth())
        if health.down_since is None:
            health.down_since = time.time()
        health.reason, health.last_check = reason, self.clock()

    def mark_up(self, host: str, fb: Fallback | None = None) -> None:
        health = self.health.setdefault(host, NetworkHealth())
        if health.down_since is not None and fb is not None:
            fb.notes.append(f"network to {host} is back (down since {self.since(host)}); using it again")
        health.down_since, health.reason, health.last_check = None, "", self.clock()

    def since(self, host: str) -> str:
        down = self.health[host].down_since
        return time.strftime("%H:%M:%S", time.localtime(down)) if down else "now"

    def is_down(self, host: str) -> bool:
        return self.health.get(host, NetworkHealth()).down_since is not None

    def wait_for_network(self, host: str) -> float | None:
        """Seconds until the device's API answered, or None if it didn't within [fallback] wait_seconds."""
        began = self.clock()
        while True:
            if probe_network(self.cfg, host) is None:
                return self.clock() - began
            if self.clock() - began >= self.cfg.fallback_wait:
                return None
            self.sleep(PROBE_INTERVAL)

    def network_or_serial(self, host: str, attempt, serial_route=None, quick_retry: bool = False):
        """Run `attempt` over the network; if the network path fails, take the serial route.

        Returns (the result, Fallback). `attempt` raises DeviceUnreachable, or
        returns a StepsResult with network_error set, when the network failed.
        serial_route(port, fallback) does the job over serial instead (reset,
        then retry; or a serial upload). Without a usable fallback port the
        network failure is reported as before. While the path is marked down,
        jobs go straight to the serial route, and the network is checked again
        at most every [fallback] recheck_seconds.
        """
        fb = Fallback()
        port = self.cfg.fallback_port() if serial_route else None
        health = self.health.setdefault(host, NetworkHealth())
        if health.down_since is not None:
            if self.clock() - health.last_check >= self.cfg.fallback_recheck:
                why = probe_network(self.cfg, host)
                if why is None:
                    self.mark_up(host, fb)
                else:
                    self.mark_down(host, why)
            if health.down_since is not None and port:
                fb.notes.append(f"network to {host} down since {self.since(host)} ({health.reason}); went straight to serial")
                return serial_route(port, fb), fb
        tries = 2 if quick_retry else 1
        for attempt_number in range(tries):
            failed_log = ""
            try:
                value = attempt()
                why = getattr(value, "network_error", None)
                failed_log = getattr(value, "log", "") if why else ""
            except DeviceUnreachable as error:
                value, why, failed_log, failure = None, str(error), error.log, error
            if why is None:
                if health.down_since is not None:
                    self.mark_up(host, fb)
                return value, fb
            if attempt_number + 1 < tries:
                self.sleep(NETWORK_RETRY_DELAY)
        self.mark_down(host, why)
        if not port:
            if value is not None:
                return value, fb  # the job's own failure report stands
            raise failure
        fb.notes.append(f"network failed ({why})" + (" twice" if tries > 1 else ""))
        if failed_log:
            fb.log += f"=== over the network ({host}) ===\n{failed_log}\n\n"
        return serial_route(port, fb), fb

    def fallback_error(self, fb: Fallback) -> BridgeError:
        return BridgeError(fb.line(), log=mask(fb.log, self.secrets))

    def reset_and_retry(self, port: str, host: str, attempt, fb: Fallback):
        """Serial route for API actions and network logs: reset, wait for the API, retry once."""
        capture = SerialCapture(port, self.cfg.max_log_bytes, reset=True)
        capture.start()
        if not capture.reset_done:
            fb.notes.append(f"reset over {port} failed ({capture.error})")
            raise self.fallback_error(fb)
        fb.notes.append(f"reset over {port}")
        try:
            back = self.wait_for_network(host)
        finally:
            fb.log += f"=== serial {port} after the reset ===\n{capture.stop()}\n\n"
        if back is None:
            self.mark_down(host, f"no answer {self.cfg.fallback_wait}s after a serial reset")
            fb.notes.append(f"device not back on the network after {self.cfg.fallback_wait} s; not retried "
                            "(the serial boot log is attached)")
            raise self.fallback_error(fb)
        self.mark_up(host)
        fb.notes.append(f"device back after {back:.0f} s")
        try:
            value = attempt()
            why = getattr(value, "network_error", None)
            if why:
                fb.log += f"=== retry over the network ===\n{value.log}\n\n"
        except DeviceUnreachable as error:
            why = str(error)
            fb.log += error.log
        except BridgeError as error:
            fb.notes.append(f"retried: ❌ ({error})")
            raise self.fallback_error(fb) from error
        if why:
            self.mark_down(host, why)
            fb.notes.append(f"retried: ❌ ({why})")
            raise self.fallback_error(fb)
        fb.notes.append("retried: ✅" if getattr(value, "ok", True) else "retried: ❌")
        return value

    def serial_upload(self, yaml_path: Path, port: str, fb: Fallback) -> StepsResult:
        """Serial route for an OTA upload: flash the same build with `esphome upload --device <port>`.

        Same rules as `upload` itself: an allowed requester, `upload` enabled,
        and a port listed in [esphome].devices (Config.fallback_port checks it).
        """
        fb.notes.append(f"flashing the same build over {port} instead (serial flashing follows the `upload` rules)")
        steps = [("upload", esphome_argv(self.cfg, "upload", yaml_path, port), self.cfg.timeouts["upload"], None)]
        outcome = self.run_steps(steps, yaml_path.parent)
        outcome.device = port
        fb.notes.append("serial upload: ✅" if outcome.ok else "serial upload: ❌")
        return outcome

    def reset_device(self, req: Request, start: float) -> JobResult:
        """`reset`: pulse EN over the serial port, optionally read the boot log and wait for the network."""
        port, host = reset_target(req, self.cfg)
        if self.dry_run:
            return JobResult(True, f"🔄 The device on `{port}` would be reset (dry run).", "", 0.0)
        capture = SerialCapture(port, self.cfg.max_log_bytes, reset=True)
        capture.start()
        if not capture.reset_done:
            raise BridgeError(f"Could not reset: {capture.error}.")
        began = self.clock()
        back = None
        try:
            if host:
                back = self.wait_for_network(host)
            if req.seconds_given:
                self.sleep(max(0.0, req.seconds - (self.clock() - began)))
        finally:
            serial_log = capture.stop()
        fb = Fallback([f"reset over {port} (EN pulsed low, IO0 high: a normal boot)"])
        if host and back is None:
            self.mark_down(host, f"no answer {self.cfg.fallback_wait}s after a serial reset")
            fb.notes.append(f"{host} not back on the network after {self.cfg.fallback_wait} s")
        elif host:
            fb.notes.append(f"{host} back on the network after {back:.0f} s")
            self.mark_up(host, fb)
        log = mask(f"=== serial {port} ===\n{serial_log}", self.secrets) if req.seconds_given else ""
        summary = "🔄 " + "; ".join(fb.notes) + "." + (f" Serial boot log for {req.seconds}s attached." if log else "")
        panic = [line for line in serial_log.splitlines() if PANIC_LINE.search(line)]
        if panic and log:
            summary += "\nSerial console:\n```\n" + mask("\n".join(panic[:12]), self.secrets) + "\n```"
        decoded = decode_crash(self.cfg, log) if log else ""
        if decoded:
            log += f"\n\n=== crash decoded ===\n{decoded}"
        return JobResult(not host or back is not None, summary, log, time.monotonic() - start)

    def run_steps(self, steps: list[tuple[str, list[str], int, int | None]], cwd: Path) -> StepsResult:
        """Run a job's esphome commands in order; the first failure stops them."""
        log_parts: list[str] = []
        ok = True
        detail = ""
        for name, argv, timeout, stop_after in steps:
            log_parts.append(f"$ {' '.join(shlex.quote(a) for a in argv)}\n")
            if self.dry_run:
                log_parts.append("(dry run: not executed)\n")
                continue
            code, out, truncated = run_process(argv, cwd, timeout, self.cfg.max_log_bytes, stop_after)
            log_parts.append(out + ("\n[log truncated]\n" if truncated else ""))
            if name == "logs" and stop_after is not None and code is None:
                detail = f"captured {stop_after}s of logs"
                continue
            if code != 0:
                ok = False
                detail = f"`{name}` {'timed out' if code is None else f'exited {code}'}"
                break
        return StepsResult(ok, detail, "".join(log_parts))

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

    def edit_yaml(self, req: Request, yaml_path: Path, where: str, start: float) -> JobResult:
        """Replace one exact occurrence, keep a backup, and undo it if `esphome config` fails."""
        assert req.find is not None and req.replace is not None
        raw = b"" if self.dry_run else yaml_path.read_bytes()
        original = raw.decode("utf-8")
        # Requests arrive with \n; match files saved with \r\n too.
        crlf = "\r\n" in original
        find, replace = (req.find.replace("\r\n", "\n").replace("\n", "\r\n"), req.replace.replace("\r\n", "\n").replace("\n", "\r\n")) if crlf else (req.find, req.replace)
        count = original.count(find)
        if not self.dry_run and count != 1:
            raise BridgeError(f"`find` must occur exactly once in `{req.yaml}`; it occurs {count} times. Nothing was changed.")
        updated = original.replace(find, replace, 1)
        diff = "".join(difflib.unified_diff(original.replace("\r\n", "\n").splitlines(keepends=True),
                                            updated.replace("\r\n", "\n").splitlines(keepends=True),
                                            f"a/{req.yaml}", f"b/{req.yaml}", n=2))
        shown = code_block(mask(hide_inline_secrets(diff), self.secrets), "diff")
        if self.dry_run:
            return JobResult(True, f"✏️ `{req.yaml}` would be edited (dry run).", "", 0.0)
        backup = yaml_path.with_name(f"{yaml_path.name}.bak-{time.strftime('%Y%m%d-%H%M%S')}")
        backup.write_bytes(raw)
        yaml_path.write_bytes(updated.encode("utf-8"))
        code, out, _ = run_process(esphome_argv(self.cfg, "config", yaml_path, None), yaml_path.parent,
                                   self.cfg.timeouts["config"], self.cfg.max_log_bytes)
        took = time.monotonic() - start
        if code != 0:
            yaml_path.write_bytes(raw)
            return JobResult(False, f"❌ `esphome config` rejected the edit to `{req.yaml}`, so the original is back "
                                    f"(unchanged; backup `{backup.name}`).\n{shown}", mask(out, self.secrets), took)
        return JobResult(True, f"✏️ Edited `{req.yaml}` from {where}; `esphome config` passed. Backup: `{backup.name}`.\n{shown}",
                         "", took)

    def write_device_file(self, req: Request, start: float) -> JobResult:
        """Replace a small text file on the SD card, then read it back to confirm."""
        assert req.path is not None and req.content is not None
        if self.dry_run:
            return JobResult(True, f"✏️ `{req.path}` would be written on {self.cfg.api_host} (dry run).", "", 0.0)
        try:
            before = capture_file(self.cfg, req.path).decode("utf-8", errors="replace")
        except BridgeError:
            before = None  # a new file (or unreadable now; the loader keeps any old one as .bak)
        call_device_action(self.cfg, DEVICE_API_ACTIONS["writefile"], {"path": req.path, "data": req.content})
        try:
            after = capture_file(self.cfg, req.path).decode("utf-8", errors="replace")
        except BridgeError as error:
            after, readback = None, str(error)
        took = time.monotonic() - start
        if after != req.content:
            reason = "it reads back differently" if after is not None else f"reading it back failed: {readback}"
            return JobResult(False, f"❌ `{req.path}` was not written ({reason}). The loader refuses writes while an app "
                                    "is running (close it first) and only takes text files under /sd/.", "", took)
        diff = "".join(difflib.unified_diff((before or "").splitlines(keepends=True), after.splitlines(keepends=True),
                                            f"a{req.path}", f"b{req.path}", n=2))
        shown = code_block(mask(hide_inline_secrets(diff), self.secrets), "diff") if diff else "(no change)"
        kept = " The old file is kept as `.bak`." if before is not None else ""
        return JobResult(True, f"✏️ Wrote `{req.path}` ({len(req.content.encode('utf-8'))} bytes) on {self.cfg.api_host}; "
                               f"read back and verified.{kept}\n{shown}", "", took)

    def delete_device_file(self, req: Request, start: float) -> JobResult:
        """Remove one file from the SD card, then check that it is gone."""
        assert req.path is not None
        if self.dry_run:
            return JobResult(True, f"🗑️ `{req.path}` would be deleted on {self.cfg.api_host} (dry run).", "", 0.0)
        folder, name = req.path.rsplit("/", 1)
        folder += "/"

        def listed_size() -> str | None:
            """The file's size from its folder's listing (name<TAB>size lines), or None if absent."""
            for line in capture_file(self.cfg, folder).decode("utf-8", errors="replace").splitlines():
                entry, _, size = line.partition("\t")
                if entry.lower() == name.lower():  # FAT names ignore case
                    return size
            return None

        size = listed_size()
        if size is None:
            return JobResult(False, f"❌ Nothing deleted: `{req.path}` is not on the card.", "", time.monotonic() - start)
        call_device_action(self.cfg, DEVICE_API_ACTIONS["deletefile"], {"path": req.path})
        gone = listed_size() is None
        took = time.monotonic() - start
        if not gone:
            return JobResult(False, f"❌ `{req.path}` is still there. The loader refuses while an app is running (close it "
                                    "first), and only removes text and leftover files under /sd/.", "", took)
        return JobResult(True, f"🗑️ Deleted `{req.path}` ({size} bytes) on {self.cfg.api_host}; its folder no longer lists it.",
                         "", took)

    def execute(self, req: Request) -> JobResult:
        start = time.monotonic()
        if req.action == "status":
            enabled = ", ".join(self.cfg.enabled)
            devices = ", ".join(self.cfg.devices) or "none"
            text = f"Bridge `{self.cfg.handle}` is up. Actions: {enabled}. Devices: {devices}."
            port = self.cfg.fallback_port()
            text += f" Serial fallback: {'on, ' + port if port else 'off'}."
            for host, health in self.health.items():
                if health.down_since is not None:
                    text += f" Network to {host}: down since {self.since(host)} ({health.reason})."
            return JobResult(True, text, "", 0.0)
        base = validate(req, self.cfg)
        if req.action == "reset":
            return self.reset_device(req, start)
        if req.action == "screenshot":
            if self.dry_run:
                return JobResult(True, f"📸 Screenshot would be taken on {self.cfg.api_host} (dry run).", "", 0.0)
            host = self.cfg.api_host
            (width, height, raw), fb = self.network_or_serial(
                host, lambda: capture_screenshot(self.cfg), quick_retry=True,
                serial_route=lambda port, fb: self.reset_and_retry(port, host, lambda: capture_screenshot(self.cfg), fb))
            png = rgb565_to_png(raw, width, height)
            name = time.strftime("screenshot-%Y%m%d-%H%M%S.png")
            return JobResult(True, f"{fb.prefix()}📸 Screenshot of the running PAPP ({width}×{height}) from {host}.",
                             mask(fb.log, self.secrets), time.monotonic() - start, [(name, png, "image/png")])
        if req.action == "readfile":
            if self.dry_run:
                return JobResult(True, f"📄 `{req.path}` would be read from {self.cfg.api_host} (dry run).", "", 0.0)
            if not req.serial:
                content = capture_file(self.cfg, req.path)
                return JobResult(True, mask(describe_file(req.path, content), self.secrets), "", time.monotonic() - start)
            # With serial=, keep reading the console for `seconds` so a panic dump
            # (its assert text only goes to serial) is captured too.
            capture = SerialCapture(req.serial, self.cfg.max_log_bytes)
            capture.start()
            content, error = None, None
            try:
                content = capture_file(self.cfg, req.path)
            except BridgeError as failure:
                error = failure
            finally:
                time.sleep(req.seconds)
                serial_log = capture.stop()
            log = mask(f"=== serial {req.serial} ===\n{serial_log}", self.secrets)
            summary = mask(describe_file(req.path, content), self.secrets) if content is not None else f"❌ `readfile` failed: {error}"
            panic = [line for line in serial_log.splitlines() if PANIC_LINE.search(line)]
            if panic:
                summary += "\nSerial console:\n```\n" + mask("\n".join(panic[:12]), self.secrets) + "\n```"
            decoded = decode_crash(self.cfg, log)
            if decoded:
                log += f"\n\n=== crash decoded ===\n{decoded}"
            return JobResult(content is not None, summary, log, time.monotonic() - start)
        if req.action == "writefile":
            return self.write_device_file(req, start)
        if req.action == "deletefile":
            return self.delete_device_file(req, start)
        if req.action in DEVICE_API_ACTIONS:
            data = {"url": req.url or ""} if req.action == "launch" else {}
            served = ""
            if req.action == "launch" and req.url and req.url.startswith(PROXIED_PREFIXES) and not self.dry_run:
                data["url"] = proxy_url(self.cfg, req.url)
                served = f" (served from this machine as {data['url']})"
            log, fb = "", Fallback()
            if not self.dry_run:
                def attempt() -> str:
                    capture = SerialCapture(req.serial, self.cfg.max_log_bytes) if req.serial else None
                    if capture:
                        capture.start()
                    try:
                        device_log = call_device_action(self.cfg, DEVICE_API_ACTIONS[req.action], data,
                                                        log_seconds=req.seconds if req.seconds_given else 0)
                    finally:
                        if capture:
                            serial_log = capture.stop()
                    if capture:
                        device_log = f"=== device log (network) ===\n{device_log}\n\n=== serial {req.serial} ===\n{serial_log}"
                    return device_log

                host = self.cfg.api_host
                route = ((lambda port, fb: self.reset_and_retry(port, host, attempt, fb))
                         if req.action in FALLBACK_RESET_ACTIONS else None)
                log, fb = self.network_or_serial(host, attempt, route, quick_retry=True)
            what = f"`{req.action}`" + (f" `{req.url.rsplit('/', 1)[-1]}`" if req.url else "")
            logged = f" Device log for {req.seconds}s attached." if req.seconds_given and not self.dry_run else ""
            decoded = decode_crash(self.cfg, log, req.url) if log else ""
            if decoded:
                log += f"\n\n=== crash decoded ===\n{decoded}"
                logged += " 💥 The device crashed: the decoded backtrace is at the end of the log."
            if fb.log:
                log = f"{mask(fb.log, self.secrets)}=== final attempt ===\n{log}"
            return JobResult(True, f"{fb.prefix()}✅ {what} sent to {self.cfg.api_host}{served}"
                                   f"{' (dry run)' if self.dry_run else ''}.{logged}", log, time.monotonic() - start)
        assert base is not None and req.yaml is not None
        where = "local config"
        if req.source == "repo":
            sha = "dry-run" if self.dry_run else self.checkout(req.ref)
            where = f"`{req.ref}` @ `{sha[:10]}`"
        yaml_path = _inside(base, req.yaml)
        if not self.dry_run and not yaml_path.exists():
            raise BridgeError(f"`{req.yaml}` does not exist at {where}.")
        if req.action == "view":
            text = "" if self.dry_run else yaml_path.read_bytes().decode("utf-8", errors="replace")
            shown = mask(hide_inline_secrets(text), self.secrets)
            # Chat shows the start; a longer file also comes whole as an attachment.
            files = ([(f"{Path(req.yaml).name}.txt", shown.encode(), "text/plain")]
                     if len(shown) > READFILE_INLINE_CHARS else [])
            attached = " The whole file is attached." if files else ""
            return JobResult(True, f"📄 `{req.yaml}` from {where}, secret values hidden.{attached}\n{code_block(shown)}", "",
                             time.monotonic() - start, files)
        if req.action == "edit":
            return self.edit_yaml(req, yaml_path, where, start)
        steps: list[tuple[str, list[str], int, int | None]] = []
        if req.action in {"config", "compile", "upload", "run"}:
            timeout = self.cfg.timeouts["upload"] + self.cfg.timeouts["compile"] if req.action == "run" else self.cfg.timeouts.get(req.action, 900)
            steps.append((req.action, esphome_argv(self.cfg, req.action, yaml_path, req.device), timeout, None))
        if req.action in {"logs", "run"}:
            steps.append(("logs", esphome_argv(self.cfg, "logs", yaml_path, req.device), req.seconds + 30, req.seconds))
        fb = Fallback()
        if self.dry_run or req.action not in ("upload", "logs") or not req.device or is_serial_port(req.device):
            outcome = self.run_steps(steps, yaml_path.parent)
        else:
            # An OTA upload, or logs over the network: the serial fallback applies.
            def attempt() -> StepsResult:
                result = self.run_steps(steps, yaml_path.parent)
                result.network_error = network_failure(req.action, result.log, result.ok)
                return result

            if req.action == "upload":
                route = lambda port, fb: self.serial_upload(yaml_path, port, fb)  # noqa: E731
            else:
                route = lambda port, fb: self.reset_and_retry(port, req.device, attempt, fb)  # noqa: E731
            outcome, fb = self.network_or_serial(req.device, attempt, route)
        log = mask(f"{fb.log}=== final attempt ===\n{outcome.log}" if fb.log else outcome.log, self.secrets)
        ok, detail = outcome.ok, outcome.detail
        took = time.monotonic() - start
        device = outcome.device or req.device
        verb = f"{req.action} `{req.yaml}`" + (f" → `{device}`" if device else "")
        summary = f"{'✅' if ok else '❌'} {verb} from {where}: {'ok' if ok else 'failed'}" + (f" ({detail})" if detail else "") + f" in {took:.0f}s."
        return JobResult(ok, fb.prefix() + summary, log, took)


# ── hub (EhGI MCP over streamable HTTP) ─────────────────────────────────────


class HubAuthError(RuntimeError):
    """The hub rejected the token; retrying will not help."""


AUTH_HELP = ("The hub rejected the bridge's token (HTTP {code}). Put the token of the bridge's own agent "
             "(Add an agent in the project, e.g. {handle}) in {env}, then run `check` again. "
             "A token that was rotated or whose agent was removed also gives this.")


class Hub:
    """Minimal EhGI MCP client. Needs cfg.hub_url, token, token_env, handle and project_id
    (tools/device_bridge uses it too)."""

    CLIENT_NAME = "esp-bridge"

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
                raise HubAuthError(AUTH_HELP.format(code=error.code, env=self.cfg.token_env,
                                                    handle=getattr(self.cfg, "handle", "esp-bridge"))) from None
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
                    "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": self.CLIENT_NAME, "version": "1"}}})
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
    if req.action in ("logs", "run") or req.seconds_given:
        parts.append(f"seconds={req.seconds}")
    if req.url:
        parts.append(f"url={req.url}")
    if req.serial:
        parts.append(f"serial={req.serial}")
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
        if req is not None:
            req.requester = author
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
        result = JobResult(False, f"❌ `{req.action}` could not run: {error}", error.log, 0.0)
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
            if error.log:
                print(error.log, file=sys.stderr)
            return 2
        print(result.summary)
        print(result.log)
        return 0 if result.ok else 1
    serve(cfg, args.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
