#!/usr/bin/env python3
"""Device bridge: lets the owner and trusted agents on the EhGI hub work in a few
named folders on this machine (read, edit, git, push, allowlisted commands),
without giving anyone a shell.

It joins the project as its own agent (its own token), waits for requests that
@mention it, checks them against bridge.toml, runs them without a shell and
replies in the request's thread with the result (long output as an attachment).

Deny by default:
  * only folders listed under [workspaces], by name; every path is resolved
    (symlinks too) and must stay inside its workspace; `.git` internals and
    files matching [files].secret_patterns are never read or written;
  * only the actions in [actions].enabled and the git subcommands in
    [git].subcommands, with arguments the bridge builds itself (no free flags);
  * `push` only to [push].remotes and branch patterns in [push].branches, never
    forced, never a delete;
  * `run` only for argv prefixes listed per workspace (argv, no shell, timeout);
  * only requests from handles in [hub].allowed_requesters (default: the owner
    and claude); secret values (env, token, secrets files) are masked in output.

Request format (mention the bridge at the start of a line):
    @deviceuse-bridge status
    @deviceuse-bridge readfile ws=esphomebrew path=README.md
    @deviceuse-bridge listdir ws=esphomebrew path=tools
    @deviceuse-bridge writefile ws=esphomebrew path=notes.txt      # + a ``` code block (the whole file)
    @deviceuse-bridge edit ws=esphomebrew path=a.yaml "find=old text" "replace=new text"
    @deviceuse-bridge git status ws=esphomebrew
    @deviceuse-bridge git diff ws=esphomebrew --cached
    @deviceuse-bridge git log ws=esphomebrew -n 5
    @deviceuse-bridge git show ws=esphomebrew rev=HEAD~1 --stat
    @deviceuse-bridge git add ws=esphomebrew path=a.yaml path=b.yaml
    @deviceuse-bridge git commit ws=esphomebrew -m "Fix the listing"
    @deviceuse-bridge git switch ws=esphomebrew -c claude/fix-listing
    @deviceuse-bridge git pull ws=esphomebrew
    @deviceuse-bridge git fetch ws=esphomebrew --prune
    @deviceuse-bridge push ws=esphomebrew origin claude/fix-listing
    @deviceuse-bridge run ws=esphomebrew -- python3 tools/make_listing.py --check
`ws=` can be left out when only one workspace is configured (or [hub].default_workspace
is set). Agents may send the same fields as message data: {"device_bridge": {...}}.

Usage:
    python device_bridge.py --config bridge.toml check      # hub connectivity
    python device_bridge.py --config bridge.toml serve      # run the bridge
    python device_bridge.py --config bridge.toml --dry-run serve   # validate only; never writes or runs
    python device_bridge.py --config bridge.toml local "git status ws=esphomebrew"
"""

from __future__ import annotations

import argparse
import dataclasses
import difflib
import fnmatch
import hashlib
import os
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

# The hub client, request-line parsing, chat formatting and masking are shared
# with the ESP bridge (tools/esp_bridge/esp_bridge.py).
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "esp_bridge"))

import esp_bridge as eb  # noqa: E402

BridgeError = eb.BridgeError
HubAuthError = eb.HubAuthError
JobResult = eb.JobResult
code_block = eb.code_block
console = eb.console
INLINE_CHARS = eb.READFILE_INLINE_CHARS

ACTIONS = ("status", "readfile", "writefile", "edit", "listdir", "git", "push", "run")
GIT_SUBCOMMANDS = ("status", "diff", "log", "show", "add", "commit", "pull", "switch", "checkout", "fetch")
DATA_KEY = "device_bridge"  # message data: {"device_bridge": {"action": ..., ...}}

# Files that are never read, written, staged or shown (matched against every
# path component, case-insensitively, also with a trailing .bak/.orig/~ removed).
DEFAULT_SECRET_PATTERNS = [
    "secrets.yaml", "secrets.yml", "secrets.json", "secrets.toml", ".env", ".env.*", "*.env",
    "*token*", "*secret*", "*password*", "*credential*", "id_*", "*.pem", "*.key", "*.p12", "*.pfx",
    "*.keystore", "*.jks", ".netrc", ".git-credentials", ".npmrc", ".pypirc", ".ssh", ".gnupg",
]
# Paths that writefile/edit never change: hook scripts git runs, submodule URLs,
# and CI workflows (a pushed workflow runs with the repository's secrets).
DEFAULT_READONLY = [".github/workflows/*", ".githooks/*", ".husky/*", ".gitmodules"]
BACKUP_SUFFIX = re.compile(r"(\.(bak|orig|old|save|swp|tmp)|~)+$")
# Files shown with inline secret values (password:, key:, token: ...) hidden.
CONFIG_SUFFIXES = (".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf", ".properties")

NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")          # workspaces and remotes
BRANCH_RE = re.compile(r"^(?![-/.])(?!.*//)(?!.*\.\.)(?!.*/\.)(?!.*@\{)(?!.*\.lock$)(?!.*[/.]$)[A-Za-z0-9._/-]{1,120}$")
REV_RE = re.compile(r"^(?![-.])[A-Za-z0-9._/~^@{}-]{1,120}$")          # no `:` (rev:path would read any blob)
SECRET_ENV_NAME = re.compile(r"(^|_)(TOKEN|SECRET|PASSWORD|PASSWD|PASS|PASSPHRASE|CREDENTIALS?|API_?KEY|KEY|"
                             r"PRIVATE_KEY|COOKIE)(_|$)")
# Well-known token formats, masked even when their value is not known here.
TOKEN_SHAPES = re.compile(r"(?<![A-Za-z0-9_-])(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}"
                          r"|glpat-[A-Za-z0-9_-]{20,}|xox[abprs]-[A-Za-z0-9-]{10,}|AKIA[0-9A-Z]{16}"
                          r"|sk-(?:ant-|proj-)?[A-Za-z0-9_-]{32,})"
                          r"|-----BEGIN [A-Z ]*PRIVATE KEY-----.*?-----END [A-Z ]*PRIVATE KEY-----", re.S)
URL_CREDENTIALS = re.compile(r"(?i)\b([a-z][a-z0-9+.-]*://)[^/\s:@]+(?::[^/\s@]*)?@")
FORCE_FLAGS = {"-f", "--force", "--force-with-lease", "--force-if-includes", "--delete", "-d", "-D", "--mirror",
               "--prune-tags", "--discard-changes", "--hard"}
SLOW = {"push", "run"}  # also git pull/fetch: these post a "started" message first
GIT_BASE = ["git", "-c", "protocol.ext.allow=never", "-c", "core.quotepath=false", "-c", "color.ui=false", "--no-pager"]
TAIL_LINES = 60


# ── configuration ───────────────────────────────────────────────────────────


def expand(path: str) -> Path:
    return Path(os.path.expanduser(path)).resolve()


@dataclass
class Workspace:
    name: str
    root: Path
    readonly: list[str] = field(default_factory=list)       # globs writefile/edit refuse
    commands: list[list[str]] = field(default_factory=list)  # argv prefixes `run` accepts (more args may follow)
    commands_exact: list[list[str]] = field(default_factory=list)  # argv `run` accepts only exactly
    push_remotes: list[str] = field(default_factory=lambda: ["origin"])
    push_branches: list[str] = field(default_factory=lambda: ["claude/*"])


@dataclass
class Config:
    hub_url: str
    project_id: str
    token: str
    token_env: str
    handle: str
    owner: str
    allowed_requesters: list[str]
    workspaces: dict[str, Workspace]
    default_workspace: str | None = None
    enabled: list[str] = field(default_factory=lambda: ["status", "readfile", "listdir", "git"])
    git_subcommands: list[str] = field(default_factory=lambda: ["status", "diff", "log", "show"])
    git_remotes: list[str] = field(default_factory=lambda: ["origin"])
    secret_patterns: list[str] = field(default_factory=lambda: list(DEFAULT_SECRET_PATTERNS))
    max_read_bytes: int = 1_000_000
    max_write_bytes: int = 256 * 1024
    max_list_entries: int = 500
    max_output_bytes: int = 1_000_000
    max_log_count: int = 100
    git_timeout: int = 300
    run_timeout: int = 600
    pass_env: list[str] = field(default_factory=list)   # secret-looking env names `run` still gets
    secrets_files: list[Path] = field(default_factory=list)
    mask_env: list[str] = field(default_factory=list)   # more env names whose values are masked
    show_requests: bool = True

    @staticmethod
    def load(path: Path, *, need_token: bool = True) -> "Config":
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
        hub, files, git, push = raw.get("hub", {}), raw.get("files", {}), raw.get("git", {}), raw.get("push", {})
        run, masking = raw.get("run", {}), raw.get("masking", {})
        token_env = hub.get("token_env", "EHGI_DEVICE_BRIDGE_TOKEN")
        token = eb.read_agent_token(token_env, need_token)
        owner = str(hub.get("owner", "")).lstrip("@").lower()
        requesters = hub.get("allowed_requesters")
        if requesters is None:
            requesters = [owner, "claude"]
        push_remotes = [str(r) for r in push.get("remotes", ["origin"])]
        push_branches = [str(b) for b in push.get("branches", ["claude/*"])]
        readonly = [str(g) for g in files.get("readonly", DEFAULT_READONLY)]
        workspaces: dict[str, Workspace] = {}
        for name, spec in raw.get("workspaces", {}).items():
            if not NAME_RE.match(name):
                raise SystemExit(f"Workspace name `{name}` must be letters, digits, `.`, `_` or `-`.")
            spec = {"path": spec} if isinstance(spec, str) else dict(spec)
            if "path" not in spec:
                raise SystemExit(f"[workspaces.{name}] needs a `path`.")
            workspaces[name] = Workspace(
                name=name, root=expand(spec["path"]),
                readonly=readonly + [str(g) for g in spec.get("readonly", [])],
                commands=_argv_list(spec.get("commands", []), name),
                commands_exact=_argv_list(spec.get("commands_exact", []), name),
                push_remotes=[str(r) for r in spec.get("push_remotes", push_remotes)],
                push_branches=[str(b) for b in spec.get("push_branches", push_branches)],
            )
        if not workspaces:
            raise SystemExit("bridge.toml has no [workspaces]; list at least one folder the bridge may use.")
        default_ws = hub.get("default_workspace") or (next(iter(workspaces)) if len(workspaces) == 1 else None)
        if default_ws is not None and default_ws not in workspaces:
            raise SystemExit(f"[hub].default_workspace `{default_ws}` is not in [workspaces].")
        enabled = [a for a in raw.get("actions", {}).get("enabled", ["status", "readfile", "listdir", "git"]) if a in ACTIONS]
        return Config(
            hub_url=hub.get("url", "https://ehgi.ai/api/mcp"),
            project_id=hub["project_id"],
            token=token,
            token_env=token_env,
            handle=hub.get("handle", "deviceuse-bridge").lstrip("@"),
            owner=owner,
            allowed_requesters=[h.lstrip("@").lower() for h in requesters if h],
            workspaces=workspaces,
            default_workspace=default_ws,
            enabled=enabled,
            git_subcommands=[s for s in git.get("subcommands", ["status", "diff", "log", "show"]) if s in GIT_SUBCOMMANDS],
            git_remotes=[str(r) for r in git.get("remotes", ["origin"])],
            secret_patterns=[str(p) for p in files.get("secret_patterns", DEFAULT_SECRET_PATTERNS)],
            max_read_bytes=int(files.get("max_read_bytes", 1_000_000)),
            max_write_bytes=int(files.get("max_write_bytes", 256 * 1024)),
            max_list_entries=int(files.get("max_list_entries", 500)),
            max_output_bytes=int(run.get("max_output_bytes", 1_000_000)),
            max_log_count=int(git.get("max_log", 100)),
            git_timeout=int(git.get("timeout", 300)),
            run_timeout=int(run.get("timeout", 600)),
            pass_env=[str(n) for n in run.get("pass_env", [])],
            secrets_files=[expand(p) for p in masking.get("secrets_files", [])],
            mask_env=[str(n) for n in masking.get("env", [])],
            show_requests=bool(raw.get("console", {}).get("show_requests", True)),
        )


def _argv_list(value, workspace: str) -> list[list[str]]:
    if not isinstance(value, list) or not all(isinstance(c, list) and c and all(isinstance(a, str) for a in c)
                                              for c in value):
        raise SystemExit(f"[workspaces.{workspace}] commands must be lists of strings, e.g. [[\"make\", \"listing\"]].")
    return [list(c) for c in value]


# ── requests ────────────────────────────────────────────────────────────────


@dataclass
class Request:
    action: str
    subcommand: str | None = None      # git
    workspace: str | None = None
    paths: list[str] = field(default_factory=list)
    content: str | None = None         # writefile: the whole new file
    find: str | None = None            # edit
    replace: str | None = None
    message: str | None = None         # git commit
    rev: str | None = None             # git diff/log/show
    count: int | None = None           # git log
    staged: bool = False               # git diff --cached
    stat: bool = False                 # git diff/log/show --stat
    prune: bool = False                # git fetch --prune
    create: bool = False               # git switch -c / checkout -b
    upstream: bool = False             # push --set-upstream
    remote: str | None = None          # git pull/fetch, push
    branch: str | None = None          # git pull/switch, push
    start: str | None = None           # git switch -c <branch> <start>
    source: str | None = None          # push: local branch (default: the same name as `branch`)
    argv: list[str] = field(default_factory=list)  # run
    requester: str = ""

    @property
    def name(self) -> str:
        return f"git {self.subcommand}" if self.action == "git" else self.action

    @property
    def path(self) -> str | None:
        return self.paths[0] if self.paths else None


# Which request fields each action takes; anything else is refused.
FIELDS = {
    "status": {"workspace"}, "readfile": {"workspace", "paths"}, "listdir": {"workspace", "paths"},
    "writefile": {"workspace", "paths", "content"}, "edit": {"workspace", "paths", "find", "replace"},
    "git status": {"workspace"}, "git diff": {"workspace", "paths", "staged", "stat", "rev"},
    "git log": {"workspace", "paths", "count", "stat", "rev"}, "git show": {"workspace", "rev", "stat"},
    "git add": {"workspace", "paths"}, "git commit": {"workspace", "message"},
    "git pull": {"workspace", "remote", "branch"}, "git fetch": {"workspace", "remote", "prune"},
    "git switch": {"workspace", "branch", "create", "start"}, "git checkout": {"workspace", "branch", "create", "start"},
    "push": {"workspace", "remote", "branch", "source", "upstream"}, "run": {"workspace", "argv"},
}
TEXT_KEYS = {"ws": "workspace", "workspace": "workspace", "path": "paths", "find": "find", "replace": "replace",
             "message": "message", "rev": "rev", "n": "count", "count": "count", "staged": "staged", "stat": "stat",
             "prune": "prune", "create": "create", "upstream": "upstream", "remote": "remote", "branch": "branch",
             "start": "start", "from": "source", "source": "source"}
DATA_KEYS = {**TEXT_KEYS, "paths": "paths", "content": "content", "argv": "argv", "action": "action",
             "subcommand": "subcommand", "git": "subcommand"}
NEVER_KEYS = {"force", "force_with_lease", "delete", "mirror", "shell"}


def _bool(key: str, value) -> bool:
    if isinstance(value, bool):
        return value
    text = str(value).strip().lower()
    if text in ("1", "true", "yes", "on"):
        return True
    if text in ("0", "false", "no", "off"):
        return False
    raise BridgeError(f"`{key}` must be yes or no.")


def set_field(req: Request, key: str, value) -> None:
    """Set one request option; unknown and forbidden keys are refused."""
    lower = key.lower()
    if lower in NEVER_KEYS:
        raise BridgeError(f"`{key}` is never allowed: the bridge does not force, mirror or delete, and has no shell.")
    name = DATA_KEYS.get(lower)
    if name is None or name in ("action", "subcommand"):
        raise BridgeError(f"Unknown option `{key}`. Options: {', '.join(sorted(TEXT_KEYS))}.")
    if name == "paths":
        req.paths.extend([str(v) for v in value] if isinstance(value, list) else [str(value)])
    elif name == "argv":
        req.argv = [str(v) for v in value] if isinstance(value, list) else shlex.split(str(value))
    elif name == "count":
        try:
            req.count = int(value)
        except (TypeError, ValueError):
            raise BridgeError("`n` must be a whole number.") from None
    elif name in ("staged", "stat", "prune", "create", "upstream"):
        setattr(req, name, _bool(key, value))
    else:
        setattr(req, name, str(value))


def apply_flag(req: Request, word: str, following: str | None) -> int:
    """The few git-style flags people type out of habit. Returns how many extra words it used."""
    sub = req.subcommand if req.action == "git" else req.action
    if word in FORCE_FLAGS or word.startswith("--force") or (word.startswith("-") and not word.startswith("--")
                                                             and "f" in word[1:] and sub in ("push", "switch", "checkout")):
        raise BridgeError(f"`{word}` is never used: the bridge does not force, delete or discard anything.")
    value = word.split("=", 1)[1] if "=" in word else None
    if sub == "diff" and word in ("--cached", "--staged"):
        req.staged = True
    elif sub in ("diff", "log", "show") and word == "--stat":
        req.stat = True
    elif sub == "log" and word == "--oneline":
        pass  # the bridge's log format is one line per commit already
    elif sub == "log" and word == "-n" and following is not None:
        set_field(req, "n", following)
        return 1
    elif sub == "log" and re.fullmatch(r"-n?\d+", word):
        set_field(req, "n", word.lstrip("-n"))
    elif sub == "log" and word.startswith("--max-count="):
        set_field(req, "n", value)
    elif sub == "commit" and word == "-m" and following is not None:
        req.message = following if req.message is None else f"{req.message}\n\n{following}"
        return 1
    elif sub == "commit" and word.startswith("--message="):
        req.message = value if req.message is None else f"{req.message}\n\n{value}"
    elif sub == "pull" and word == "--ff-only":
        pass  # pull is always --ff-only
    elif sub == "fetch" and word in ("--prune", "-p"):
        req.prune = True
    elif (sub == "switch" and word in ("-c", "--create")) or (sub == "checkout" and word == "-b"):
        req.create = True
        if following is not None and not following.startswith("-") and "=" not in following:
            req.branch = following
            return 1
    elif sub == "push" and word in ("-u", "--set-upstream"):
        req.upstream = True
    else:
        raise BridgeError(f"`{word}` is not accepted for `{sub}`; the bridge builds the command itself.")
    return 0


def assign_positionals(req: Request, words: list[str]) -> None:
    """Bare words: paths for file actions, remote/branch for push/pull, a rev for show."""
    if not words:
        return
    sub = req.name
    if req.action == "git" and req.subcommand not in GIT_SUBCOMMANDS:
        req.paths.extend(words)  # validation refuses the subcommand with a better message
    elif sub in ("readfile", "writefile", "edit", "listdir", "git diff", "git add", "git log"):
        req.paths.extend(words)
    elif sub == "git show" and req.rev is None and len(words) == 1:
        req.rev = words[0]
    elif sub in ("git switch", "git checkout") and len(words) <= 2:
        slots = ["branch", "start"] if req.branch is None else ["start"]
        if len(words) > len(slots):
            raise BridgeError(f"Too many words for `{sub}`: {' '.join(words)}.")
        for slot, word in zip(slots, words):
            setattr(req, slot, word)
    elif sub in ("push", "git pull", "git fetch") and len(words) <= (1 if sub == "git fetch" else 2):
        # `push claude/x` names a branch; `push origin claude/x` a remote and a branch; `fetch origin` a remote.
        slots = ["branch"] if len(words) == 1 and sub != "git fetch" else ["remote", "branch"]
        for slot, word in zip(slots, words):
            if getattr(req, slot) is not None:
                raise BridgeError(f"`{slot}` is given twice.")
            setattr(req, slot, word)
    elif sub == "git commit":
        raise BridgeError('Give the commit message with -m "…" (or message="…").')
    else:
        raise BridgeError(f"`{sub}` does not take `{' '.join(words)}`.")


def parse_request(text: str, data: dict | None, handle: str) -> Request | None:
    """Return the request addressed to this bridge, or None if the message has none."""
    if isinstance(data, dict) and isinstance(data.get(DATA_KEY), dict):
        return request_from_data(data[DATA_KEY])
    words = eb.request_words(text, handle, ACTIONS)
    if words is None:
        return None
    action = words[0]
    if action not in ACTIONS:
        raise BridgeError(f"Unknown action `{action}`. Use one of: {', '.join(ACTIONS)}.")
    req = Request(action=action)
    rest = words[1:]
    if action == "git":
        if not rest:
            raise BridgeError(f"`git` needs a subcommand: {', '.join(GIT_SUBCOMMANDS)} (push is its own action).")
        req.subcommand, rest = rest[0].strip("`.,:;!?").lower(), rest[1:]
    if action == "run":
        # Options (ws=...) come first; the command follows `--`, or is the bare words.
        if "--" in rest:
            cut = rest.index("--")
            rest, req.argv = rest[:cut], rest[cut + 1:]
            for word in rest:
                if "=" not in word:
                    raise BridgeError("Put the whole command after `--`.")
                set_field(req, *word.split("=", 1))
        else:
            for word in rest:
                if "=" in word and not req.argv and not word.startswith("-"):
                    set_field(req, *word.split("=", 1))
                else:
                    req.argv.append(word)
        return req
    positionals: list[str] = []
    i = 0
    while i < len(rest):
        word = rest[i]
        if word.startswith("-") and len(word) > 1:
            i += 1 + apply_flag(req, word, rest[i + 1] if i + 1 < len(rest) else None)
            continue
        if "=" in word and not word.startswith("="):
            set_field(req, *word.split("=", 1))
        else:
            positionals.append(word)
        i += 1
    assign_positionals(req, positionals)
    blocks = eb.CODE_FENCE.findall(text)
    if action == "writefile" and blocks:
        req.content = blocks[0]
    if action == "edit" and req.find is None and req.replace is None and len(blocks) == 2:
        req.find, req.replace = blocks  # first block: the text to find; second: its replacement
    return req


def request_from_data(fields: dict) -> Request:
    action = str(fields.get("action", "")).lower()
    if action not in ACTIONS:
        raise BridgeError(f"Unknown action `{action}`. Use one of: {', '.join(ACTIONS)}.")
    req = Request(action=action)
    sub = fields.get("subcommand", fields.get("git"))
    if action == "git":
        if not sub:
            raise BridgeError(f"`git` needs a subcommand: {', '.join(GIT_SUBCOMMANDS)}.")
        req.subcommand = str(sub).lower()
    for key, value in fields.items():
        if key.lower() in ("action", "subcommand", "git"):
            continue
        set_field(req, key, value)
    return req


def check_fields(req: Request) -> None:
    allowed = FIELDS.get(req.name)
    if allowed is None:
        return  # an unknown git subcommand; validate_git explains
    default = Request(action=req.action)
    used = {f.name for f in dataclasses.fields(Request)
            if f.name not in ("action", "subcommand", "requester") and getattr(req, f.name) != getattr(default, f.name)}
    extra = used - allowed
    if extra:
        raise BridgeError(f"`{req.name}` does not take {', '.join(sorted(extra))}.")


# ── validation ──────────────────────────────────────────────────────────────


@dataclass
class Job:
    """A validated request: its workspace, resolved paths and (for git/push/run) the exact argv."""
    workspace: Workspace
    targets: list[Path] = field(default_factory=list)
    argv: list[str] | None = None


def secret_reason(parts, cfg: Config) -> str | None:
    """Why a path with these components is off limits, or None."""
    for part in parts:
        low = part.lower()
        if low == ".git":
            return "git internals (`.git`) are off limits"
        stripped = BACKUP_SUFFIX.sub("", low)
        for pattern in cfg.secret_patterns:
            pat = pattern.lower()
            if fnmatch.fnmatchcase(low, pat) or (stripped and fnmatch.fnmatchcase(stripped, pat)):
                return f"`{part}` matches the secret pattern `{pattern}`"
    return None


def pick_workspace(req: Request, cfg: Config) -> Workspace:
    name = req.workspace or cfg.default_workspace
    if not name:
        raise BridgeError(f"Say which workspace with `ws=` ({', '.join(cfg.workspaces)}).")
    ws = cfg.workspaces.get(name)
    if ws is None:
        raise BridgeError(f"No workspace `{name}` on this bridge (workspaces: {', '.join(cfg.workspaces)}).")
    return ws


def confine(cfg: Config, ws: Workspace, rel: str, *, write: bool = False) -> Path:
    """Resolve a workspace-relative path; refuse escapes, `.git`, secrets and (for writes) readonly files."""
    if not rel or any(c in rel for c in "\0\r\n") or len(rel) > 1000:
        raise BridgeError("Give a path relative to the workspace.")
    if rel.startswith(("/", "\\", "~")) or re.match(r"^[A-Za-z]:", rel) or "\\" in rel:
        raise BridgeError(f"`{rel}`: paths are relative to the workspace (no absolute paths, `~` or backslashes).")
    parts = [p for p in rel.split("/") if p not in ("", ".")]
    if ".." in parts:
        raise BridgeError(f"`{rel}`: `..` is not allowed in paths.")
    reason = secret_reason(parts, cfg)
    if reason:
        raise BridgeError(f"`{rel}` is refused: {reason}.")
    root = ws.root.resolve()
    if not root.is_dir():
        raise BridgeError(f"Workspace `{ws.name}` ({root}) does not exist on this machine.")
    target = root.joinpath(*parts).resolve()
    if not target.is_relative_to(root):
        raise BridgeError(f"`{rel}` resolves outside workspace `{ws.name}` (through a symlink); refused.")
    resolved = target.relative_to(root)
    reason = secret_reason(resolved.parts, cfg)
    if reason:
        raise BridgeError(f"`{rel}` is refused: it resolves to `{resolved.as_posix()}`, and {reason}.")
    if write:
        for name in {"/".join(parts), resolved.as_posix()}:
            for pattern in ws.readonly:
                if fnmatch.fnmatch(name, pattern):  # case-insensitive where the file system is
                    raise BridgeError(f"`{rel}` is read-only on this bridge (matches `{pattern}`).")
    return target


def check_ref(kind: str, value: str | None, regex: re.Pattern = BRANCH_RE) -> str:
    if not value:
        raise BridgeError(f"Give a {kind}.")
    branch = regex is BRANCH_RE
    if branch and (value.startswith("+") or ":" in value):
        raise BridgeError(f"`{value}`: name a {kind}, not a refspec (the bridge never force-pushes (`+`) or deletes (`:`)).")
    if branch and (value == "HEAD" or value.startswith("refs/")) or not regex.match(value):
        raise BridgeError(f"`{value}` is not a valid {kind} name" + ("" if branch else " (`rev:path` is not accepted)") + ".")
    return value


def check_remote(value: str, allowed: list[str], what: str) -> str:
    if not NAME_RE.match(value):
        raise BridgeError(f"`{value}`: name a configured remote (like `origin`), not a URL or path.")
    if value not in allowed:
        raise BridgeError(f"Remote `{value}` is not allowed for {what} (allowed: {', '.join(allowed) or 'none'}).")
    return value


def secret_excludes(cfg: Config) -> list[str]:
    """Pathspecs that keep secret files out of diffs, shows and `git add`."""
    out = []
    for pattern in cfg.secret_patterns:
        out += [f":(exclude,glob,icase)**/{pattern}", f":(exclude,glob,icase)**/{pattern}/**"]
    return out


def validate(req: Request, cfg: Config) -> Job | None:
    """Check a request against bridge.toml. Returns the job to run (None for status)."""
    if req.action not in cfg.enabled:
        raise BridgeError(f"`{req.action}` is not enabled on this bridge (enabled: {', '.join(cfg.enabled)}).")
    check_fields(req)
    if req.action == "status":
        return None
    ws = pick_workspace(req, cfg)
    if req.action in ("readfile", "writefile", "edit", "listdir"):
        if len(req.paths) > 1:
            raise BridgeError(f"`{req.action}` takes one path.")
        if not req.paths and req.action != "listdir":
            raise BridgeError(f"`{req.action}` needs `path=`.")
        write = req.action in ("writefile", "edit")
        target = confine(cfg, ws, req.path or ".", write=write)
        if req.action == "writefile":
            if req.content is None:
                raise BridgeError("`writefile` takes the whole new file from a ``` code block in the same message.")
            if "\0" in req.content or len(req.content.encode("utf-8")) > cfg.max_write_bytes:
                raise BridgeError(f"`writefile` content must be text of at most {cfg.max_write_bytes} bytes.")
            if eb.MASKED_VALUE.search(req.content):
                raise BridgeError("The content has a hidden value (`***`); write the real value, or leave that line out.")
        if req.action == "edit":
            if not req.find or req.replace is None:
                raise BridgeError("`edit` needs `find` (text that occurs exactly once) and `replace`.")
            if len(req.find) > cfg.max_write_bytes or len(req.replace) > cfg.max_write_bytes or "\0" in req.replace:
                raise BridgeError(f"`find` and `replace` are limited to {cfg.max_write_bytes} characters of text.")
            if eb.MASKED_VALUE.search(req.replace):
                raise BridgeError("`replace` has a hidden value (`***`); write the real value.")
        return Job(ws, [target])
    if req.action == "git":
        return validate_git(req, cfg, ws)
    if req.action == "push":
        return validate_push(req, cfg, ws)
    return validate_run(req, cfg, ws)


def validate_git(req: Request, cfg: Config, ws: Workspace) -> Job:
    sub = req.subcommand or ""
    if sub == "push":
        raise BridgeError("Push with the `push` action, e.g. `push origin claude/my-branch`.")
    if sub not in GIT_SUBCOMMANDS:
        raise BridgeError(f"`git {sub}` is not allowed. Allowed: {', '.join(GIT_SUBCOMMANDS)} (and the `push` action).")
    if sub not in cfg.git_subcommands:
        raise BridgeError(f"`git {sub}` is not enabled on this bridge (enabled: {', '.join(cfg.git_subcommands)}).")
    targets = [confine(cfg, ws, p) for p in req.paths]
    # :(literal) keeps wildcards and pathspec magic (`:(top)`, `:!x`) in a path from meaning anything.
    literal = [":(literal)" + ("/".join(q for q in p.split("/") if q not in ("", ".")) or ".") for p in req.paths]
    rev = None
    if req.rev is not None:
        # Each side of a range must be a commit: a blob or tree id (`git show <sha>`) would print
        # a file's content past the secret-file excludes.
        rev = "".join(p if p in ("", "..", "...") else p + "^{commit}"
                      for p in re.split(r"(\.{2,3})", check_ref("revision", req.rev, REV_RE)))
    if sub == "status":
        argv = ["status", "--short", "--branch", "--untracked-files=normal"]
    elif sub == "diff":
        argv = ["diff", "--no-color", "--no-ext-diff", "--no-textconv", *(["--cached"] if req.staged else []),
                *(["--stat"] if req.stat else []), *([rev] if rev else []),
                "--", *(literal or ["."]), *secret_excludes(cfg)]
    elif sub == "log":
        count = 20 if req.count is None else req.count
        if not 1 <= count <= cfg.max_log_count:
            raise BridgeError(f"`n` must be between 1 and {cfg.max_log_count}.")
        argv = ["log", "--no-color", f"--max-count={count}", "--date=short", "--format=%h %ad %an%d: %s",
                *(["--stat"] if req.stat else []), *([rev] if rev else []), *(["--", *literal] if literal else [])]
    elif sub == "show":
        argv = ["show", "--no-color", "--no-ext-diff", "--no-textconv", *(["--stat"] if req.stat else []),
                rev or "HEAD", "--", ".", *secret_excludes(cfg)]
    elif sub == "add":
        if not literal:
            raise BridgeError("`git add` needs the paths to stage (`path=…`).")
        # Secret files and the bridge's own .bak backups are never staged, even from a folder.
        argv = ["add", "--", *literal, *secret_excludes(cfg), ":(exclude,glob)**/*.bak"]
    elif sub == "commit":
        message = (req.message or "").strip()
        if not message or "\0" in message or len(message) > 10_000:
            raise BridgeError('`git commit` needs a message: -m "…" (at most 10,000 characters).')
        argv = ["commit", f"--message={message}"]
    elif sub == "pull":
        remote_name = req.remote or (cfg.git_remotes[0] if req.branch and cfg.git_remotes else None)
        if req.branch and not remote_name:
            raise BridgeError("Name the remote too: `git pull origin <branch>`.")
        remote = check_remote(remote_name, cfg.git_remotes, "pull") if remote_name else None
        branch = check_ref("branch", req.branch) if req.branch else None
        argv = ["pull", "--ff-only", "--no-rebase", "--no-recurse-submodules", *filter(None, [remote, branch])]
    elif sub == "fetch":
        remote = check_remote(req.remote, cfg.git_remotes, "fetch") if req.remote else None
        argv = ["fetch", "--no-recurse-submodules", *(["--prune"] if req.prune else []), *([remote] if remote else [])]
    else:  # switch / checkout: to a branch, or -c/-b a new one
        branch = check_ref("branch", req.branch)
        if req.start and not req.create:
            raise BridgeError("A start point only goes with creating a branch (`-c`).")
        start = check_ref("start point", req.start, REV_RE) if req.start else None
        argv = ["switch", "--no-recurse-submodules", *(["-c"] if req.create else []), branch, *([start] if start else [])]
    return Job(ws, targets, GIT_BASE + argv)


def validate_push(req: Request, cfg: Config, ws: Workspace) -> Job:
    remote = check_remote(req.remote or (ws.push_remotes[0] if ws.push_remotes else "origin"), ws.push_remotes, "push")
    branch = check_ref("branch", req.branch)
    source = check_ref("local branch", req.source or branch)
    if not any(fnmatch.fnmatchcase(branch, pattern) for pattern in ws.push_branches):
        raise BridgeError(f"Pushing to `{branch}` is not allowed on `{ws.name}` (allowed: {', '.join(ws.push_branches)}).")
    argv = ["push", "--no-follow-tags", *(["--set-upstream"] if req.upstream else []), remote,
            f"refs/heads/{source}:refs/heads/{branch}"]
    return Job(ws, [], GIT_BASE + argv)


def check_run_arg(arg: str, cfg: Config) -> None:
    """Extra arguments after an allowed prefix may not point outside the workspace or at secrets."""
    if any(c in arg for c in "\0\r\n") or len(arg) > 4096:
        raise BridgeError("Command arguments must be single-line text.")
    if (re.search(r"(^|[=:,])[/~\\]", arg) or re.match(r"^-[A-Za-z][/~]", arg) or re.search(r"(^|[=:,])[A-Za-z]:[\\/]", arg)
            or re.search(r"(^|[/=:,\\])\.\.([/\\]|$)", arg)):
        raise BridgeError(f"Argument `{arg}` points outside the workspace (absolute path, `~`, `..` or a URL); refused.")
    value = arg.split("=", 1)[1] if arg.startswith("-") and "=" in arg else arg
    reason = secret_reason([p for p in value.split("/") if p], cfg)
    if reason:
        raise BridgeError(f"Argument `{arg}` is refused: {reason}.")


def validate_run(req: Request, cfg: Config, ws: Workspace) -> Job:
    argv = req.argv
    if not argv:
        raise BridgeError("`run` needs a command, e.g. `run -- make listing`.")
    if len(argv) > 100:
        raise BridgeError("That command has too many arguments.")
    if argv in ws.commands_exact:
        extra: list[str] = []
    else:
        prefix = next((p for p in ws.commands if argv[:len(p)] == p), None)
        if prefix is None:
            allowed = [shlex.join(p) + " …" for p in ws.commands] + [shlex.join(p) for p in ws.commands_exact]
            raise BridgeError(f"`{shlex.join(argv)}` is not allowed in `{ws.name}` "
                              f"(allowed: {', '.join(allowed) or 'nothing'}).")
        extra = argv[len(prefix):]
    for arg in argv:
        if any(c in arg for c in "\0\r\n"):
            raise BridgeError("Command arguments must be single-line text.")
    for arg in extra:
        check_run_arg(arg, cfg)
    if not ws.root.is_dir():
        raise BridgeError(f"Workspace `{ws.name}` ({ws.root}) does not exist on this machine.")
    return Job(ws, [], list(argv))


# ── secrets masking ─────────────────────────────────────────────────────────


def load_secret_file_values(files: list[Path]) -> list[str]:
    """Values in secrets.yaml-style (`key: value`) and .env-style (`KEY=value`) files."""
    values = []
    for path in files:
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line in lines:
            m = re.match(r"^\s*(?:export\s+)?[A-Za-z0-9_.-]+\s*[:=]\s*(.+?)\s*$", line)
            if m and not m.group(1).startswith("#"):
                value = m.group(1).strip().strip("'\"")
                if len(value) >= 5 and value.lower() not in ("true", "false", "null", "none"):
                    values.append(value)
    return values


def collect_secrets(cfg: Config) -> list[str]:
    values = [cfg.token] if cfg.token else []
    names = set(cfg.mask_env) | {cfg.token_env}
    values += [v.strip() for k, v in os.environ.items()
               if (k in names or SECRET_ENV_NAME.search(k.upper())) and len(v.strip()) >= 6]
    files = list(cfg.secrets_files)
    for ws in cfg.workspaces.values():
        files += [ws.root / n for n in ("secrets.yaml", "secrets.yml", ".env")]
    values += load_secret_file_values(files)
    return sorted({v for v in values if v}, key=len, reverse=True)


def mask_text(text: str, secrets: list[str]) -> str:
    text = eb.mask(text, secrets)
    text = TOKEN_SHAPES.sub("***", text)
    return URL_CREDENTIALS.sub(r"\1***@", text)


# ── running ─────────────────────────────────────────────────────────────────


def run_process(argv: list[str], cwd: Path, timeout: int, max_bytes: int, env: dict | None = None) -> tuple[int | None, str, bool]:
    """Run argv (no shell) in its own process group. Returns (exit code or None on timeout, output, truncated)."""
    extra = {"start_new_session": True} if os.name == "posix" else {}
    try:
        proc = subprocess.Popen(argv, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, **extra)
    except OSError as error:
        raise BridgeError(f"Could not start `{argv[0]}`: {error.strerror or error}.") from error
    chunks: list[bytes] = []
    size, truncated = 0, False

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
    try:
        code: int | None = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        _stop(proc)
        code = None
    thread.join(timeout=5)
    if proc.stdout is not None:
        proc.stdout.close()
    return code, b"".join(chunks).decode("utf-8", "replace").replace("\r\n", "\n"), truncated


def _stop(proc: subprocess.Popen) -> None:
    """Stop a timed-out command and everything it started."""
    if os.name == "posix":
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.killpg(proc.pid, sig)
            except ProcessLookupError:
                return
            try:
                proc.wait(timeout=5)
                return
            except subprocess.TimeoutExpired:
                continue
    else:
        proc.kill()
        proc.wait(timeout=10)


def git_env(cfg: Config) -> dict:
    """The bridge's environment for git (the machine's own credentials), minus the hub token; never prompts."""
    env = {k: v for k, v in os.environ.items() if k != cfg.token_env}
    env.update(GIT_TERMINAL_PROMPT="0", GIT_EDITOR="true", GIT_SEQUENCE_EDITOR="true", GIT_PAGER="cat", PAGER="cat")
    return env


def run_env(cfg: Config) -> dict:
    """`run` gets no hub token and no secret-looking variables unless [run].pass_env lists them."""
    env = {k: v for k, v in os.environ.items()
           if k != cfg.token_env and (k in cfg.pass_env or not SECRET_ENV_NAME.search(k.upper()))}
    env.update(GIT_TERMINAL_PROMPT="0", PAGER="cat")
    return env


def atomic_write(target: Path, data: bytes, mode: int | None = None) -> None:
    """Write via a temp file and rename, so a symlink at `target` is replaced, never followed.

    The file keeps its permissions (or gets `mode`); a new one gets the usual umask ones."""
    if mode is None:
        if target.is_file() and not target.is_symlink():
            mode = target.stat().st_mode
        else:
            umask = os.umask(0o022)
            os.umask(umask)
            mode = 0o666 & ~umask
    fd, tmp = tempfile.mkstemp(dir=target.parent, prefix=f".{target.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.chmod(tmp, mode & 0o7777)
        os.replace(tmp, target)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def shown_argv(argv: list[str]) -> str:
    """The command as people would type it: without the bridge's fixed git options and secret excludes."""
    if argv[:len(GIT_BASE)] == GIT_BASE:
        rest = [a for a in argv[len(GIT_BASE):] if not a.startswith(":(exclude")]
        rest = [a[len(":(literal)"):] if a.startswith(":(literal)") else a for a in rest]
        return "git " + shlex.join(rest)
    return shlex.join(argv)


def display_path(path: Path) -> str:
    home = Path.home()
    try:
        return "~/" + path.relative_to(home).as_posix()
    except ValueError:
        return str(path)


class Runner:
    def __init__(self, cfg: Config, dry_run: bool = False):
        self.cfg, self.dry_run = cfg, dry_run
        self.secrets = collect_secrets(cfg)

    def mask(self, text: str) -> str:
        return mask_text(text, self.secrets)

    def result(self, ok: bool, summary: str, output: str, start: float, name: str, *, tail: bool = False) -> JobResult:
        """Summary plus output in a code block; output longer than the chat shows is attached whole."""
        output = self.mask(output.rstrip()) or "(no output)"
        files = []
        if len(output) > INLINE_CHARS or (tail and output.count("\n") >= TAIL_LINES):
            files = [(f"{name}-{time.strftime('%Y%m%d-%H%M%S')}.log", output.encode("utf-8"), "text/plain")]
            summary += " Full output attached."
        if tail:
            lines = output.splitlines()
            shown = "\n".join(lines[-TAIL_LINES:])[-INLINE_CHARS:]
            if len(lines) > TAIL_LINES:
                shown = f"(last {TAIL_LINES} of {len(lines)} lines)\n{shown}"
        else:
            shown = output
        return JobResult(ok, f"{summary}\n{code_block(shown)}", "", time.monotonic() - start, files)

    def execute(self, req: Request) -> JobResult:
        start = time.monotonic()
        self.secrets = collect_secrets(self.cfg)  # secrets files may have changed
        job = validate(req, self.cfg)
        if job is None:
            return self.status()
        where = f"`{job.workspace.name}:{req.path}`" if req.path else f"`{job.workspace.name}`"
        if req.action in ("git", "push", "run"):
            return self.run_job(req, job, start)
        if self.dry_run:
            return JobResult(True, f"{req.action} {where}: allowed (dry run; nothing read or written).", "", 0.0)
        return getattr(self, f"do_{req.action}")(req, job.targets[0], where, start)

    def status(self) -> JobResult:
        cfg = self.cfg
        lines = [f"Bridge `{cfg.handle}` is up{' (dry run)' if self.dry_run else ''}. Actions: {', '.join(cfg.enabled)}.",
                 f"Requesters: {', '.join(cfg.allowed_requesters) or 'none'}."]
        if "git" in cfg.enabled:
            lines.append(f"git: {', '.join(cfg.git_subcommands)}.")
        for ws in cfg.workspaces.values():
            line = f"- `{ws.name}`: {display_path(ws.root)}"
            if not ws.root.is_dir():
                line += " (missing)"
            elif not self.dry_run:
                try:
                    code, out, _ = run_process([*GIT_BASE, "branch", "--show-current"], ws.root, 10, 1000, git_env(cfg))
                except BridgeError:
                    code, out = None, ""
                if code == 0 and out.strip():
                    line += f", branch `{out.strip()}`"
            if "push" in cfg.enabled:
                line += f"; push to {', '.join(ws.push_remotes)}: {', '.join(ws.push_branches)}"
            if "run" in cfg.enabled:
                cmds = [f"`{shlex.join(c)} …`" for c in ws.commands] + [f"`{shlex.join(c)}`" for c in ws.commands_exact]
                line += f"; run: {', '.join(cmds) or 'nothing'}"
            lines.append(line)
        return JobResult(True, self.mask("\n".join(lines)), "", 0.0)

    # ── files ──

    def do_readfile(self, req: Request, target: Path, where: str, start: float) -> JobResult:
        if not target.exists():
            raise BridgeError(f"{where} does not exist.")
        if target.is_dir():
            raise BridgeError(f"{where} is a folder; use `listdir`.")
        size = target.stat().st_size
        if size > self.cfg.max_read_bytes:
            raise BridgeError(f"{where} is {size} bytes; `readfile` is limited to {self.cfg.max_read_bytes}.")
        data = target.read_bytes()
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = None
        if text is None or "\0" in text:
            digest = hashlib.sha256(data).hexdigest()
            return JobResult(True, f"📄 {where}: binary, {size} bytes, sha256 `{digest[:16]}…` (not shown).", "",
                             time.monotonic() - start)
        if target.suffix.lower() in CONFIG_SUFFIXES:
            text = eb.hide_inline_secrets(text)
        shown = self.mask(text)
        files = [(f"{target.name}.txt", shown.encode("utf-8"), "text/plain")] if len(shown) > INLINE_CHARS else []
        attached = " The whole file is attached." if files else ""
        return JobResult(True, f"📄 {where} ({size} bytes).{attached}\n{code_block(shown.rstrip(chr(10)))}", "",
                         time.monotonic() - start, files)

    def do_listdir(self, req: Request, target: Path, where: str, start: float) -> JobResult:
        if not target.is_dir():
            raise BridgeError(f"{where} is not a folder.")
        entries = sorted((p for p in target.iterdir() if p.name.lower() != ".git"),
                         key=lambda p: (not p.is_dir(), p.name.lower()))
        rows = []
        for p in entries[: self.cfg.max_list_entries]:
            mark = " (protected)" if secret_reason([p.name], self.cfg) else ""
            if p.is_symlink():
                rows.append(f"{p.name}@{mark}")
            elif p.is_dir():
                rows.append(f"{p.name}/{mark}")
            else:
                rows.append(f"{p.name}\t{p.stat().st_size}{mark}")
        more = len(entries) - len(rows)
        if more > 0:
            rows.append(f"(… {more} more; listdir shows at most {self.cfg.max_list_entries})")
        count = f"{len(entries)} entr{'y' if len(entries) == 1 else 'ies'}"
        return JobResult(True, f"📁 {where}: {count}.\n{code_block(self.mask(chr(10).join(rows)) or '(empty)')}",
                         "", time.monotonic() - start)

    def _diff(self, rel: str, before: str, after: str) -> str:
        diff = "".join(difflib.unified_diff(before.replace("\r\n", "\n").splitlines(keepends=True),
                                            after.replace("\r\n", "\n").splitlines(keepends=True),
                                            f"a/{rel}", f"b/{rel}", n=2))
        return code_block(self.mask(diff.rstrip("\n")), "diff") if diff else "(no change)"

    def _backup(self, target: Path, old: bytes) -> str:
        backup = target.with_name(f"{target.name}.bak")
        atomic_write(backup, old, target.stat().st_mode)  # as private as the original
        return backup.name

    def do_writefile(self, req: Request, target: Path, where: str, start: float) -> JobResult:
        assert req.content is not None
        if target.is_dir():
            raise BridgeError(f"{where} is a folder.")
        old = target.read_bytes() if target.exists() else None
        content = req.content
        if old is not None and b"\r\n" in old and "\r" not in content:
            content = content.replace("\n", "\r\n")  # keep the file's line endings
        before = old.decode("utf-8", "replace") if old is not None else ""
        target.parent.mkdir(parents=True, exist_ok=True)
        kept = f" Backup: `{self._backup(target, old)}`." if old is not None else " (new file)"
        atomic_write(target, content.encode("utf-8"))
        return JobResult(True, f"✏️ Wrote {where} ({len(content.encode('utf-8'))} bytes).{kept}\n"
                               f"{self._diff(req.path or '', before, content)}", "", time.monotonic() - start)

    def do_edit(self, req: Request, target: Path, where: str, start: float) -> JobResult:
        assert req.find is not None and req.replace is not None
        if not target.is_file():
            raise BridgeError(f"{where} is not a file.")
        if target.stat().st_size > self.cfg.max_read_bytes:
            raise BridgeError(f"{where} is larger than {self.cfg.max_read_bytes} bytes; too large to edit here.")
        raw = target.read_bytes()
        try:
            original = raw.decode("utf-8")
        except UnicodeDecodeError:
            raise BridgeError(f"{where} is not UTF-8 text.") from None
        find, replace = req.find, req.replace
        if "\r\n" in original:  # requests arrive with \n; match files saved with \r\n too
            find = find.replace("\r\n", "\n").replace("\n", "\r\n")
            replace = replace.replace("\r\n", "\n").replace("\n", "\r\n")
        count = original.count(find)
        if count != 1:
            raise BridgeError(f"`find` must occur exactly once in {where}; it occurs {count} times. Nothing was changed.")
        updated = original.replace(find, replace, 1)
        if len(updated.encode("utf-8")) > self.cfg.max_read_bytes:
            raise BridgeError("The edited file would be too large.")
        backup = self._backup(target, raw)
        atomic_write(target, updated.encode("utf-8"))
        return JobResult(True, f"✏️ Edited {where}. Backup: `{backup}`.\n{self._diff(req.path or '', original, updated)}",
                         "", time.monotonic() - start)

    # ── git, push, run ──

    def run_job(self, req: Request, job: Job, start: float) -> JobResult:
        assert job.argv is not None
        ws = job.workspace
        command = shown_argv(job.argv)
        if self.dry_run:
            return JobResult(True, f"`{command}` in `{ws.name}`: allowed (dry run; not executed).", "", 0.0)
        is_run = req.action == "run"
        timeout = self.cfg.run_timeout if is_run else self.cfg.git_timeout
        env = run_env(self.cfg) if is_run else git_env(self.cfg)
        code, out, truncated = run_process(job.argv, ws.root, timeout, self.cfg.max_output_bytes, env)
        if truncated:
            out += f"\n[output truncated at {self.cfg.max_output_bytes} bytes]"
        if req.name == "git add" and code == 0:
            _, status, _ = run_process([*GIT_BASE, "status", "--short", "--branch"], ws.root, 30, 100_000, env)
            out = (out + "\n" if out.strip() else "") + status
        took = time.monotonic() - start
        ok = code == 0
        state = "ok" if ok else ("timed out" if code is None else f"exited {code}")
        summary = f"{'✅' if ok else '❌'} `{command}` in `{ws.name}`: {state} in {took:.0f}s."
        return self.result(ok, summary, out, start, req.action, tail=is_run)


# ── hub ─────────────────────────────────────────────────────────────────────


class Hub(eb.Hub):
    CLIENT_NAME = "device-bridge"


def describe(req: Request) -> str:
    """The request as one line, for the bridge's terminal (file contents are summarised)."""
    parts = [req.name]
    if req.workspace:
        parts.append(f"ws={req.workspace}")
    parts += [f"path={p}" for p in req.paths]
    for name in ("rev", "remote", "branch", "start", "source", "count"):
        if getattr(req, name) is not None:
            parts.append(f"{name}={getattr(req, name)}")
    parts += [f"--{n}" for n in ("staged", "stat", "prune", "create", "upstream") if getattr(req, n)]
    if req.message is not None:
        parts.append(f"message={req.message.splitlines()[0][:60]!r}")
    if req.content is not None:
        parts.append(f"(content: {len(req.content)} chars)")
    if req.find is not None:
        parts.append(f"(find/replace: {len(req.find)}/{len(req.replace or '')} chars)")
    if req.argv:
        parts.append("-- " + shlex.join(req.argv))
    return " ".join(parts)


def handle_event(event: dict, cfg: Config, hub, runner: Runner) -> None:
    author = str(event.get("from", "")).lower()
    if author == cfg.handle.lower():
        return
    # Only people and agents send jobs; system and GitHub feed messages never do.
    if event.get("from_kind", "human") not in ("human", "agent"):
        return
    thread = event.get("thread_id") or event.get("id")
    channel = event.get("channel")
    runner.secrets = collect_secrets(cfg)  # refusals quote requests, so mask those too
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
        refusal = f"@{author} is not allowed to use this bridge."
    # The hub rejects a message that mentions its own author, so never write @<own handle>.
    own = re.compile(re.escape(f"@{cfg.handle}"), re.IGNORECASE)

    def reply(text: str, **extra) -> None:
        hub.call("post_message", {"channel": channel, "thread_id": thread,
                                  "text": own.sub(cfg.handle, runner.mask(text)), **extra})

    where = f"#{channel}" if channel else "hub"
    if refusal:
        console(cfg, f"@{author} in {where}: refused: {runner.mask(refusal)}")
        reply(f"🚫 {refusal}")
        return
    assert req is not None
    console(cfg, f"@{author} in {where}: {runner.mask(describe(req))}")
    try:
        validate(req, cfg)
    except BridgeError as error:
        console(cfg, f"  refused: {runner.mask(str(error))}")
        reply(f"🚫 {error}")
        return
    if req.action in SLOW or req.name in ("git pull", "git fetch"):
        reply(f"⏳ @{author} `{req.name}` started.", mentions=[author])
    hub.call("set_status", {"state": "working", "note": req.name})
    try:
        result = runner.execute(req)
    except BridgeError as error:
        result = JobResult(False, f"❌ `{req.name}` could not run: {error}", "", 0.0)
    except Exception as error:  # noqa: BLE001 - report anything unexpected instead of dying
        result = JobResult(False, f"❌ `{req.name}` crashed: {type(error).__name__}: {error}", "", 0.0)
    finally:
        hub.call("set_status", {"state": "online", "note": ready_note(cfg, runner.dry_run)})
    console(cfg, f"  {'ok' if result.ok else 'failed'} in {result.seconds:.1f}s: {runner.mask(result.summary.splitlines()[0])}")
    attachments = [hub.upload(name, runner.mask(content.decode("utf-8", "replace")).encode("utf-8"), kind)
                   for name, content, kind in result.files]
    attachments = [a for a in attachments if a]
    text = f"@{author} {result.summary}"
    if result.files and not attachments:
        text += "\n(The full output could not be attached.)"
    reply(text, mentions=[author], **({"attachment_ids": attachments[:6]} if attachments else {}))


def ready_note(cfg: Config, dry_run: bool) -> str:
    return f"Device bridge ready ({', '.join(cfg.workspaces)})" + (" [dry run]" if dry_run else "")


def serve(cfg: Config, dry_run: bool) -> None:
    hub, runner = Hub(cfg), Runner(cfg, dry_run)
    briefing = hub.call("get_briefing", {})
    since = briefing.get("envelope", {}).get("latest_seq", 0)
    hub.call("set_status", {"state": "online", "note": ready_note(cfg, dry_run)})
    print(f"Listening as {cfg.handle} from seq {since} (dry run: {dry_run})", flush=True)
    failures = 0
    while True:
        try:
            result = hub.call("wait_for_activity", {"since_seq": since, "max_wait_seconds": 45, "only_for_me": True}, timeout=75)
            failures = 0
        except HubAuthError:
            raise
        except RuntimeError as error:
            failures += 1
            print(runner.mask(str(error)), file=sys.stderr, flush=True)
            time.sleep(min(60, 2 ** failures))
            continue
        if result.get("envelope", {}).get("stop_requested"):
            print("Stop requested from the hub; exiting.", flush=True)
            hub.call("set_status", {"state": "offline", "note": "Device bridge stopped"})
            return
        for event in result.get("events", []):
            try:
                handle_event(event, cfg, hub, runner)
            except RuntimeError as error:
                print(f"event {event.get('seq')}: {runner.mask(str(error))}", file=sys.stderr, flush=True)
        since = max(since, result.get("next_seq", since))


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="replace")  # consoles without emoji support
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--dry-run", action="store_true", help="validate and report, but never read, write or run anything")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("check", help="connect to the hub and print who this bridge is")
    sub.add_parser("serve", help="run the bridge")
    local = sub.add_parser("local", help="run one request here, without the hub (as the owner)")
    local.add_argument("request", help='e.g. "git status ws=esphomebrew"')
    args = parser.parse_args(argv)
    cfg = Config.load(args.config, need_token=args.command != "local")
    try:
        if args.command == "check":
            me = Hub(cfg).call("get_briefing", {}).get("me", {})
            print(f"Connected as @{me.get('handle')} ({me.get('agent_id') or me.get('id')}); configured handle @{cfg.handle}.")
            if me.get("handle") != cfg.handle:
                print("Warning: [hub].handle does not match the token's agent handle.", file=sys.stderr)
            return 0
        if args.command == "local":
            runner = Runner(cfg, args.dry_run)
            try:
                req = parse_request(f"@{cfg.handle} {args.request}", None, cfg.handle)
                if req is None:
                    raise BridgeError("Empty request.")
                req.requester = cfg.owner
                result = runner.execute(req)
            except BridgeError as error:
                print(f"refused: {runner.mask(str(error))}", file=sys.stderr)
                return 2
            print(result.summary)
            for name, _, _ in result.files:
                print(f"(attachment: {name})")
            return 0 if result.ok else 1
        serve(cfg, args.dry_run)
        return 0
    except HubAuthError as error:
        print(error, file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
