"""Tests for tools/device_bridge: request parsing, confinement, allowlists, masking,
and real runs in temporary git repositories (with a local bare remote).

Run: python3 -m unittest discover -s tests
"""

import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "device_bridge"))

import device_bridge as db  # noqa: E402

PY = Path(sys.executable).as_posix()
HANDLE = "deviceuse-bridge"
HAS_GIT = shutil.which("git") is not None

HELLO = """\
import os, sys
print("args:", " ".join(sys.argv[1:]))
print("hub token in env:", "EHGI_DEVICE_BRIDGE_TOKEN" in os.environ)
print("service token in env:", "MY_SERVICE_TOKEN" in os.environ)
if "--leak" in sys.argv:
    print(open("secrets.yaml").read())
    print(os.environ.get("MY_SERVICE_TOKEN", "(no service token)"))
if "--many" in sys.argv:
    for i in range(200):
        print("line", i)
"""


def write(path: Path, text: str) -> None:
    path.write_bytes(text.encode("utf-8"))  # exactly these bytes (no CRLF on Windows)


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def init_repo(path: Path):
    git(path, "init", "-q", "-b", "main")
    for key, value in [("user.name", "Test"), ("user.email", "test@example.invalid"), ("core.autocrlf", "false"),
                       ("commit.gpgsign", "false")]:
        git(path, "config", key, value)


def make_env(tmp: Path, *, extra_toml: str = "", enabled=None, subcommands=None, push_branches='["claude/*"]',
             run_timeout=60, max_output=1_000_000, git_repo=True):
    """A workspace `brew` (a git clone of a bare remote), a second workspace and an outside folder."""
    ws = tmp / "brew"
    (ws / "tools").mkdir(parents=True)
    write(ws / "README.md", "# Brew\nhello world\n")
    write(ws / "Makefile", "all:\n\techo hi\n")
    write(ws / "config.yaml", "wifi:\n  ssid: home\n  password: plain-wifi-pass\n")
    write(ws / "tools" / "hello.py", HELLO)
    write(ws / "tools" / "slow.py", "import time\nprint('tick', flush=True)\ntime.sleep(30)\n")
    write(ws / "secrets.yaml", "api_password: hunter2-long-value\n")
    (ws / "keys").mkdir()
    write(ws / "keys" / "api_token.txt", "tracked-token-content-XYZ\n")
    outside = tmp / "outside"
    outside.mkdir()
    write(outside / "data.txt", "outside data\n")
    other = tmp / "other-ws"
    other.mkdir()
    remote = tmp / "remote.git"
    if git_repo and HAS_GIT:
        remote.mkdir()
        git(remote, "init", "-q", "--bare", "-b", "main")
        init_repo(ws)
        write(ws / ".gitignore", "secrets.yaml\n")
        git(ws, "add", "README.md", "Makefile", "config.yaml", "tools", ".gitignore")
        git(ws, "add", "-f", "keys/api_token.txt")  # a tracked secret-looking file
        git(ws, "commit", "-q", "-m", "initial")
        git(ws, "remote", "add", "origin", str(remote))
        git(ws, "push", "-q", "-u", "origin", "main")
    toml = textwrap.dedent(f"""
        [hub]
        project_id = "p1"
        owner = "@Nona"
        default_workspace = "brew"
        [workspaces]
        other = '{other.as_posix()}'
        [workspaces.brew]
        path = '{ws.as_posix()}'
        readonly = ["Makefile"]
        commands = [['{PY}', "tools/hello.py"], ['{PY}', "tools/slow.py"], ["make"]]
        commands_exact = [['{PY}', "tools/hello.py", "--exact"], ["make", "listing"]]
        [actions]
        enabled = {enabled or '["status", "readfile", "writefile", "edit", "listdir", "git", "push", "run"]'}
        [git]
        subcommands = {subcommands or '["status", "diff", "log", "show", "add", "commit", "pull", "switch", "checkout", "fetch"]'}
        [push]
        branches = {push_branches}
        [run]
        timeout = {run_timeout}
        max_output_bytes = {max_output}
        [console]
        show_requests = false
    """) + extra_toml
    path = tmp / "bridge.toml"
    write(path, toml)
    cfg = db.Config.load(path, need_token=False)
    return cfg, ws, outside, remote


def req(text, requester="claude"):
    r = db.parse_request(f"@{HANDLE} {text}", None, HANDLE)
    r.requester = requester
    return r


def data_req(requester="claude", **fields):
    r = db.parse_request("", {"device_bridge": fields}, HANDLE)
    r.requester = requester
    return r


def file_req(action, path):
    """A readfile/writefile/listdir/edit request for `path` with whatever else that action needs."""
    extra = {"writefile": {"content": "x\n"}, "edit": {"find": "a", "replace": "b"}}.get(action, {})
    return data_req(action=action, path=path, **extra)


def symlink_or_skip(test, target: Path, link: Path, directory=False):
    try:
        os.symlink(target, link, target_is_directory=directory)
    except (OSError, NotImplementedError) as error:
        test.skipTest(f"symlinks not available here: {error}")


class FakeHub:
    def __init__(self):
        self.calls, self.posted, self.uploaded = [], [], []

    def call(self, tool, args, timeout=90):
        self.calls.append(tool)
        if tool == "post_message":
            self.posted.append(args)
        return {}

    def upload(self, name, content, content_type="text/plain"):
        self.uploaded.append((name, content, content_type))
        return f"att-{len(self.uploaded)}"


class Base(unittest.TestCase):
    git_repo = True

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        if self.git_repo and not HAS_GIT:
            self.skipTest("git is not installed")
        self.cfg, self.ws, self.outside, self.remote = make_env(self.tmp, git_repo=self.git_repo)
        self.runner = db.Runner(self.cfg)

    def tearDown(self):
        self._tmp.cleanup()

    def refused(self, r, cfg=None):
        with self.assertRaises(db.BridgeError) as caught:
            db.validate(r, cfg or self.cfg)
        return str(caught.exception)


# ── parsing ─────────────────────────────────────────────────────────────────


class ParseTests(unittest.TestCase):
    def test_file_requests(self):
        r = req("readfile ws=brew path=README.md")
        self.assertEqual((r.action, r.workspace, r.paths), ("readfile", "brew", ["README.md"]))
        self.assertEqual(req("readfile tools/hello.py").paths, ["tools/hello.py"])
        self.assertEqual(req("listdir").paths, [])
        r = db.parse_request(f"please:\n@{HANDLE} writefile path=notes.txt\n```text\nline 1\nline 2\n```", None, HANDLE)
        self.assertEqual((r.action, r.path, r.content), ("writefile", "notes.txt", "line 1\nline 2\n"))

    def test_edit_find_and_replace(self):
        r = req('edit path=a.yaml "find=refresh: 1d" "replace=refresh: 0s"')
        self.assertEqual((r.find, r.replace), ("refresh: 1d", "refresh: 0s"))
        r = db.parse_request(f"@{HANDLE} edit path=a.yaml\n```\nold\n```\n```\nnew\n```", None, HANDLE)
        self.assertEqual((r.find, r.replace), ("old\n", "new\n"))

    def test_git_requests_accept_familiar_flags(self):
        r = req('git commit -m "Fix the listing" -m "Second paragraph"')
        self.assertEqual((r.subcommand, r.message), ("commit", "Fix the listing\n\nSecond paragraph"))
        r = req("git switch -c claude/x")
        self.assertEqual((r.subcommand, r.create, r.branch), ("switch", True, "claude/x"))
        r = req("git checkout -b claude/y main")
        self.assertEqual((r.create, r.branch, r.start), (True, "claude/y", "main"))
        self.assertTrue(req("git diff --cached path=a.txt").staged)
        self.assertEqual(req("git log -n 5").count, 5)
        self.assertEqual(req("git log -3 --oneline").count, 3)
        self.assertEqual(req("git show HEAD~1 --stat").rev, "HEAD~1")
        self.assertTrue(req("git fetch --prune origin").prune)
        self.assertEqual(req("git pull --ff-only").subcommand, "pull")
        r = req("push origin claude/x -u")
        self.assertEqual((r.remote, r.branch, r.upstream), ("origin", "claude/x", True))
        r = req("push claude/x")
        self.assertEqual((r.remote, r.branch), (None, "claude/x"))

    def test_run_argv(self):
        r = req("run ws=brew -- python3 tools/make_listing.py --out=listing.json")
        self.assertEqual((r.workspace, r.argv), ("brew", ["python3", "tools/make_listing.py", "--out=listing.json"]))
        r = req("run ws=brew make V=1 -j2")
        self.assertEqual((r.workspace, r.argv), ("brew", ["make", "V=1", "-j2"]))
        with self.assertRaises(db.BridgeError):
            req("run make -- x")  # bare words before `--`

    def test_structured_data(self):
        r = data_req(action="git", subcommand="add", ws="brew", paths=["a.txt", "b.txt"])
        self.assertEqual((r.name, r.paths), ("git add", ["a.txt", "b.txt"]))
        r = data_req(action="run", argv=["make", "listing"])
        self.assertEqual(r.argv, ["make", "listing"])
        r = data_req(action="writefile", path="a.txt", content="x\n")
        self.assertEqual(r.content, "x\n")
        with self.assertRaises(db.BridgeError):
            data_req(action="push", branch="claude/x", force=True)
        with self.assertRaises(db.BridgeError):
            data_req(action="readfile", path="a", colour="blue")

    def test_messages_without_a_request_are_ignored(self):
        for text in ["no mention", f"hey @{HANDLE} can you look?", f"@{HANDLE}-2 status", "@esp-bridge status",
                     f"It said `@{HANDLE} status` earlier"]:
            with self.subTest(text=text):
                self.assertIsNone(db.parse_request(text, None, HANDLE))

    def test_bad_requests_are_refused_while_parsing(self):
        for text in ["rm -rf /", "shell ls", "git", "git reset --hard", "push origin claude/x --force",
                     "push origin claude/x -f", "push claude/x --force-with-lease", "push --delete claude/x",
                     "git switch --discard-changes main", "readfile path=a force=1", "readfile bogus=1",
                     "git log -n lots", "git commit fix things", "git status --porcelain"]:
            with self.subTest(text=text), self.assertRaises(db.BridgeError):
                req(text)


# ── path confinement ────────────────────────────────────────────────────────


class PathTests(Base):
    git_repo = False

    def test_allowed_paths(self):
        for text in ["readfile README.md", "readfile ./tools/hello.py", "listdir", "listdir tools",
                     "readfile Makefile", "writefile new/dir/file.txt\n```\nx\n```"]:
            with self.subTest(text=text):
                job = db.validate(db.parse_request(f"@{HANDLE} {text}", None, HANDLE), self.cfg)
                self.assertTrue(job.targets[0].is_relative_to(self.ws.resolve()))

    def test_dotdot_and_absolute_paths_are_refused(self):
        for path in ["../outside/data.txt", "tools/../../outside/data.txt", "tools/../README.md", "..",
                     "/etc/passwd", "~/.ssh/id_rsa", "~nona/x", "C:/Windows/win.ini", "C:\\x", "\\\\server\\share\\x",
                     "tools\\hello.py", self.outside.as_posix() + "/data.txt"]:
            for action in ("readfile", "writefile", "edit", "listdir"):
                with self.subTest(path=path, action=action):
                    self.refused(file_req(action, path))

    def test_secret_files_are_refused_for_read_and_write(self):
        for path in ["secrets.yaml", "sub/SECRETS.YAML", ".env", ".env.local", "prod.env", "keys/api_token.txt",
                     "my-secret-notes.md", "id_rsa", "id_ed25519.pub", "server.pem", "tls.key", "secrets.yaml.bak",
                     "tls.key~", ".ssh/config", ".netrc", "db_password.txt", "tokens/x.txt"]:
            for action in ("readfile", "writefile", "edit"):
                with self.subTest(path=path, action=action):
                    self.assertIn("secret pattern", self.refused(file_req(action, path)))

    def test_git_internals_are_refused(self):
        for path in [".git/config", ".git", "sub/.git/hooks/pre-commit", ".GIT/config"]:
            for action in ("readfile", "writefile", "edit", "listdir"):
                with self.subTest(path=path, action=action):
                    self.assertIn(".git", self.refused(file_req(action, path)))

    def test_symlinks_out_of_the_workspace_are_refused(self):
        symlink_or_skip(self, self.outside, self.ws / "link_out", directory=True)
        symlink_or_skip(self, self.outside / "data.txt", self.ws / "notes.txt")
        for path in ["link_out/data.txt", "link_out", "notes.txt"]:
            for action in ("readfile", "writefile", "edit", "listdir"):
                with self.subTest(path=path, action=action):
                    self.assertIn("outside", self.refused(file_req(action, path)))

    def test_symlinks_to_protected_files_inside_are_refused(self):
        (self.ws / ".git").mkdir()
        write(self.ws / ".git" / "config", "[core]\n")
        symlink_or_skip(self, self.ws / ".git" / "config", self.ws / "gitcfg.txt")
        symlink_or_skip(self, self.ws / "secrets.yaml", self.ws / "innocent.txt")
        self.assertIn(".git", self.refused(data_req(action="readfile", path="gitcfg.txt")))
        self.assertIn("secret pattern", self.refused(data_req(action="readfile", path="innocent.txt")))
        symlink_or_skip(self, self.ws / "README.md", self.ws / "readme-link.md")
        job = db.validate(data_req(action="readfile", path="readme-link.md"), self.cfg)
        self.assertEqual(job.targets[0], (self.ws / "README.md").resolve())

    def test_readonly_files_can_be_read_but_not_written(self):
        db.validate(data_req(action="readfile", path="Makefile"), self.cfg)
        for path in ["Makefile", ".github/workflows/ci.yml", ".githooks/pre-commit", ".gitmodules"]:
            with self.subTest(path=path):
                self.assertIn("read-only", self.refused(file_req("writefile", path)))
                self.assertIn("read-only", self.refused(file_req("edit", path)))

    def test_workspaces_are_by_name(self):
        self.assertIn("No workspace", self.refused(req("readfile ws=elsewhere README.md")))
        db.validate(req("listdir ws=other"), self.cfg)
        self.cfg.default_workspace = None
        self.assertIn("ws=", self.refused(req("readfile README.md")))
        self.cfg.workspaces["other"].root = self.tmp / "gone"
        self.assertIn("does not exist", self.refused(req("listdir ws=other")))

    def test_write_limits(self):
        self.refused(data_req(action="writefile", path="a.txt"))  # no content
        self.refused(data_req(action="writefile", path="a.txt", content="x" * (self.cfg.max_write_bytes + 1)))
        self.refused(data_req(action="writefile", path="a.txt", content="password: ***\n"))
        self.refused(data_req(action="writefile", path="a.txt", content="bad\0byte"))
        self.refused(data_req(action="edit", path="a.txt", replace="b"))
        self.refused(data_req(action="readfile", paths=["a.txt", "b.txt"]))

    def test_disabled_actions_are_refused(self):
        cfg, *_ = make_env(self.tmp / "t2", enabled='["status", "readfile"]', git_repo=False)
        self.assertIn("not enabled", self.refused(data_req(action="writefile", path="a.txt", content="x"), cfg))
        self.assertIn("not enabled", self.refused(req("run -- make"), cfg))
        db.validate(req("readfile README.md"), cfg)
        cfg.enabled = ["readfile"]
        with self.assertRaises(db.BridgeError):
            db.Runner(cfg).execute(req("status"))


# ── git, push and run allowlists ────────────────────────────────────────────


class GitValidationTests(Base):
    git_repo = False

    def argv(self, text):
        job = db.validate(req(text), self.cfg)
        return db.shown_argv(job.argv), job.argv

    def test_git_commands_are_built_by_the_bridge(self):
        cases = {
            "git status": "git status --short --branch --untracked-files=normal",
            "git diff --cached path=README.md": "git diff --no-color --no-ext-diff --no-textconv --cached -- README.md",
            "git diff": "git diff --no-color --no-ext-diff --no-textconv -- .",
            "git log -n 5": "git log --no-color --max-count=5 --date=short '--format=%h %ad %an%d: %s'",
            "git show rev=HEAD~1 --stat": "git show --no-color --no-ext-diff --no-textconv --stat 'HEAD~1^{commit}' -- .",
            "git log rev=main..claude/x": "git log --no-color --max-count=20 --date=short '--format=%h %ad %an%d: %s' "
                                          "'main^{commit}..claude/x^{commit}'",
            "git diff rev=release/1.0": "git diff --no-color --no-ext-diff --no-textconv 'release/1.0^{commit}' -- .",
            "git add path=README.md path=tools": "git add -- README.md tools",
            'git commit -m "Fix it"': "git commit '--message=Fix it'",
            "git pull": "git pull --ff-only --no-rebase --no-recurse-submodules",
            "git pull main": "git pull --ff-only --no-rebase --no-recurse-submodules origin main",
            "git fetch --prune origin": "git fetch --no-recurse-submodules --prune origin",
            "git switch -c claude/x main": "git switch --no-recurse-submodules -c claude/x main",
            "git checkout -b claude/y": "git switch --no-recurse-submodules -c claude/y",
            "git switch main": "git switch --no-recurse-submodules main",
        }
        for text, expected in cases.items():
            with self.subTest(text=text):
                self.assertEqual(self.argv(text)[0], expected)
        _, argv = self.argv("git add path=tools")
        self.assertEqual(argv[:len(db.GIT_BASE)], db.GIT_BASE)
        self.assertIn(":(literal)tools", argv)
        self.assertIn(":(exclude,glob,icase)**/secrets.yaml", argv)  # secrets never staged, even from a folder
        self.assertIn(":(exclude,glob)**/*.bak", argv)  # nor the bridge's backups
        self.assertIn(":(exclude,glob,icase)**/*.pem", self.argv("git diff")[1])
        self.assertIn(":(exclude,glob,icase)**/.env", self.argv("git show")[1])

    def test_disallowed_git_subcommands(self):
        for text in ["git reset", "git clean", "git config user.name x", "git remote add evil x", "git rebase main",
                     "git merge main", "git stash", "git gc", "git submodule update", "git am", "git apply",
                     "git worktree add x", "git filter-branch", "git update-ref", "git -c core.pager=sh status"]:
            with self.subTest(text=text), self.assertRaises(db.BridgeError):
                db.validate(req(text), self.cfg)
        self.assertIn("`push` action", self.refused(req("git push origin claude/x")))

    def test_disabled_git_subcommands(self):
        cfg, *_ = make_env(self.tmp / "t2", subcommands='["status", "log"]', git_repo=False)
        db.validate(req("git status"), cfg)
        self.assertIn("not enabled", self.refused(req('git commit -m "x"'), cfg))

    def test_bad_git_arguments(self):
        for text in ["git show HEAD:secrets.yaml", "git show rev=--output=x", "git diff rev=-p", "git log -n 0",
                     "git log -n 1000", "git status path=README.md", "git commit", "git add",
                     "git diff path=../outside", "git add path=secrets.yaml", "git add path=.git/config",
                     "git pull https://evil.example/repo.git main", "git fetch evil", "git fetch ../remote.git",
                     "git switch -c ../x", "git switch -c -x", "git switch main HEAD~1", "git switch -c a..b"]:
            with self.subTest(text=text), self.assertRaises(db.BridgeError):
                db.validate(req(text), self.cfg)

    def test_paths_are_literal_pathspecs(self):
        _, argv = self.argv("git add path=[ab].txt")
        self.assertIn(":(literal)[ab].txt", argv)  # a file named `[ab].txt`, not a.txt and b.txt


class PushValidationTests(Base):
    git_repo = False

    def test_allowed_push(self):
        job = db.validate(req("push origin claude/fix"), self.cfg)
        self.assertEqual(db.shown_argv(job.argv), "git push --no-follow-tags origin refs/heads/claude/fix:refs/heads/claude/fix")
        job = db.validate(req("push claude/fix from=work -u"), self.cfg)
        self.assertEqual(job.argv[-3:], ["--set-upstream", "origin", "refs/heads/work:refs/heads/claude/fix"])

    def test_push_to_disallowed_branches_and_remotes(self):
        for text in ["push main", "push origin main", "push feature/x", "push claude", "push HEAD",
                     "push refs/heads/claude/x", "push upstream claude/x", "push https://github.com/x/y.git claude/x",
                     "push ../remote.git claude/x", "push claude/x from=..", "push claude/../main", "push"]:
            with self.subTest(text=text), self.assertRaises(db.BridgeError):
                db.validate(req(text), self.cfg)

    def test_force_and_delete_are_never_accepted(self):
        for text in ["push claude/x --force", "push claude/x -f", "push claude/x --force-with-lease",
                     "push claude/x --force-if-includes", "push --delete claude/x", "push -d claude/x",
                     "push --mirror", "push -uf claude/x"]:
            with self.subTest(text=text), self.assertRaises(db.BridgeError) as caught:
                req(text)
            self.assertIn("never", str(caught.exception))
        for branch in ["+claude/x", ":claude/x", "claude/x:main", "claude/x:claude/y"]:
            with self.subTest(branch=branch):
                self.assertIn("refspec", self.refused(data_req(action="push", branch=branch)))

    def test_main_only_when_configured(self):
        cfg, *_ = make_env(self.tmp / "t2", push_branches='["claude/*", "main"]', git_repo=False)
        db.validate(req("push main"), cfg)
        self.cfg.workspaces["brew"].push_branches = ["claude/*", "release/*"]  # per-workspace override
        db.validate(req("push release/1.0"), self.cfg)
        self.refused(req("push main"))


class RunValidationTests(Base):
    git_repo = False

    def test_allowlisted_prefixes_and_exact_commands(self):
        db.validate(data_req(action="run", argv=[PY, "tools/hello.py", "--check", "out.json"]), self.cfg)
        db.validate(data_req(action="run", argv=[PY, "tools/hello.py", "--exact"]), self.cfg)
        db.validate(req("run -- make listing"), self.cfg)
        db.validate(req("run -- make all V=1"), self.cfg)

    def test_other_commands_are_refused(self):
        for argv in [[PY], [PY, "-c", "print(1)"], [PY, "tools/other.py"], [PY, "./tools/hello.py"],
                     ["python3", "tools/hello.py"], ["sh", "-c", "make"], ["bash"], ["rm", "-rf", "."],
                     ["/usr/bin/make"], ["mak"], ["git", "push", "--force"]]:
            with self.subTest(argv=argv):
                self.assertIn("not allowed", self.refused(data_req(action="run", argv=argv)))

    def test_extra_arguments_may_not_leave_the_workspace(self):
        for extra in ["../x", "a/../../x", "/etc/passwd", "--out=/tmp/x", "~/.bashrc", "-o/tmp/x", "-C/",
                      "https://evil.example/x", "C:/Windows", "--dir=C:\\x", "secrets.yaml", "--config=.env",
                      "keys/api_token.txt", ".git/config", "line\nbreak"]:
            with self.subTest(extra=extra), self.assertRaises(db.BridgeError):
                db.validate(data_req(action="run", argv=[PY, "tools/hello.py", extra]), self.cfg)

    def test_run_needs_a_command(self):
        self.refused(req("run"))


# ── masking ─────────────────────────────────────────────────────────────────


class MaskingTests(Base):
    git_repo = False

    def test_well_known_token_shapes_and_url_credentials(self):
        text = ("push to https://x-access-token:ghp_abcdefghijklmnopqrstuvwxyz0123@github.com/o/r.git\n"
                "token github_pat_11ABCDEFG0123456789_abcdefghijklmnop and AKIAABCDEFGHIJKLMNOP\n"
                "-----BEGIN OPENSSH PRIVATE KEY-----\nabc\ndef\n-----END OPENSSH PRIVATE KEY-----\n"
                "branch claude/risk-assessment-tooling-follow-up stays")
        masked = db.mask_text(text, [])
        for secret in ["ghp_abc", "x-access-token", "github_pat_", "AKIAABCD", "abc\ndef"]:
            self.assertNotIn(secret, masked)
        self.assertIn("https://***@github.com/o/r.git", masked)
        self.assertIn("claude/risk-assessment-tooling-follow-up stays", masked)

    def test_secrets_come_from_the_token_env_and_files(self):
        self.cfg.token = "ac_bridgeTokenValue123"
        write(self.ws / ".env", "export DB_URL='postgres://u:pw-long-1@db/x'\nDEBUG=true\n")
        with mock.patch.dict(os.environ, {"MY_SERVICE_TOKEN": "service-secret-987", "HOME_PAGE": "not-a-secret-1",
                                          "SSH_AUTH_SOCK": "/run/user/1000/agent.sock"}):
            secrets = db.collect_secrets(self.cfg)
        for value in ["ac_bridgeTokenValue123", "service-secret-987", "hunter2-long-value", "postgres://u:pw-long-1@db/x"]:
            self.assertIn(value, secrets)
        for value in ["not-a-secret-1", "/run/user/1000/agent.sock", "true"]:
            self.assertNotIn(value, secrets)

    def test_run_environment_drops_the_hub_token_and_secret_variables(self):
        env = {"EHGI_DEVICE_BRIDGE_TOKEN": "ac_x", "GITHUB_TOKEN": "t", "AWS_SECRET_ACCESS_KEY": "k", "PATH": "/bin",
               "SSH_AUTH_SOCK": "/s", "KEEP_API_KEY": "kept"}
        self.cfg.pass_env = ["KEEP_API_KEY"]
        with mock.patch.dict(os.environ, env, clear=True):
            run, gitenv = db.run_env(self.cfg), db.git_env(self.cfg)
        self.assertEqual(sorted(k for k in run if k in env), ["KEEP_API_KEY", "PATH", "SSH_AUTH_SOCK"])
        self.assertNotIn("EHGI_DEVICE_BRIDGE_TOKEN", gitenv)
        self.assertIn("GITHUB_TOKEN", gitenv)  # git pushes with this machine's own credentials
        self.assertEqual(gitenv["GIT_TERMINAL_PROMPT"], "0")


# ── requesters and the hub ──────────────────────────────────────────────────


class EventTests(Base):
    git_repo = False

    def event(self, text, author="claude", kind="agent", **extra):
        hub = FakeHub()
        db.handle_event({"from": author, "from_kind": kind, "channel": "general", "id": "m1", "text": text, **extra},
                        self.cfg, hub, self.runner)
        return hub

    def test_requesters_default_to_the_owner_and_claude(self):
        self.assertEqual(self.cfg.allowed_requesters, ["nona", "claude"])

    def test_other_handles_are_refused_and_nothing_runs(self):
        before = (self.ws / "README.md").read_text()
        hub = self.event(f"@{HANDLE} writefile README.md\n```\npwned\n```", author="mallory", kind="human")
        self.assertEqual(len(hub.posted), 1)
        self.assertIn("🚫", hub.posted[0]["text"])
        self.assertIn("not allowed", hub.posted[0]["text"])
        self.assertNotIn("set_status", hub.calls)
        self.assertEqual((self.ws / "README.md").read_text(), before)

    def test_system_and_github_messages_are_ignored(self):
        for kind in ("system", "github"):
            self.assertEqual(self.event(f"@{HANDLE} status", author=kind, kind=kind).calls, [])

    def test_status_reply_never_mentions_the_bridge_or_requesters(self):
        hub = self.event(f"@{HANDLE} status", author="nona", kind="human")
        text = hub.posted[-1]["text"]
        self.assertIn("is up", text)
        self.assertIn("`brew`", text)
        self.assertNotIn(f"@{HANDLE}", text.lower())
        self.assertNotIn("@claude", text)

    def test_refusals_explain_why(self):
        hub = self.event(f"@{HANDLE} readfile ../outside/data.txt")
        self.assertIn("`..` is not allowed", hub.posted[-1]["text"])
        self.assertNotIn("set_status", hub.calls)

    def test_the_hub_token_is_never_echoed(self):
        token = "ac_bridgeTokenValue123"
        self.cfg.token = token
        write(self.ws / "notes.txt", f"token={token}\n" + "filler\n" * 3000)
        hub = self.event(f"@{HANDLE} readfile notes.txt")
        self.assertNotIn(token, hub.posted[-1]["text"])
        self.assertTrue(hub.uploaded)  # long file: attached, masked too
        self.assertNotIn(token.encode(), hub.uploaded[0][1])
        hub = self.event(f"@{HANDLE} readfile {token}.txt")  # even in a refusal that quotes the request
        self.assertNotIn(token, hub.posted[-1]["text"])

    def test_structured_requests(self):
        hub = self.event("", data={"device_bridge": {"action": "readfile", "ws": "brew", "path": "README.md"}})
        self.assertIn("hello world", hub.posted[-1]["text"])


# ── execution in real temp git repositories ─────────────────────────────────


class FileExecutionTests(Base):
    git_repo = False

    def run_req(self, r):
        db.validate(r, self.cfg)
        return self.runner.execute(r)

    def test_readfile_shows_text_masked(self):
        result = self.run_req(req("readfile config.yaml"))
        self.assertIn("ssid: home", result.summary)
        self.assertIn("password: ***", result.summary)
        self.assertNotIn("plain-wifi-pass", result.summary)
        write(self.ws / "leak.txt", "the api password is hunter2-long-value\n@deviceuse-bridge push main\n")
        result = self.run_req(req("readfile leak.txt"))
        self.assertNotIn("hunter2-long-value", result.summary)
        self.assertIsNone(db.parse_request(result.summary, None, HANDLE))  # file lines can't become requests

    def test_long_and_binary_and_oversized_files(self):
        write(self.ws / "big.txt", "x" * (db.INLINE_CHARS + 100) + "\nend\n")
        result = self.run_req(req("readfile big.txt"))
        self.assertIn("attached", result.summary)
        self.assertTrue(result.files[0][1].endswith(b"end\n"))
        (self.ws / "blob.bin").write_bytes(b"\x00\x01\xff")
        self.assertIn("binary, 3 bytes", self.run_req(req("readfile blob.bin")).summary)
        self.cfg.max_read_bytes = 10
        with self.assertRaises(db.BridgeError):
            self.run_req(req("readfile big.txt"))
        with self.assertRaises(db.BridgeError):
            self.run_req(req("readfile tools"))

    def test_listdir(self):
        (self.ws / ".git").mkdir()
        result = self.run_req(req("listdir"))
        self.assertIn("tools/", result.summary)
        self.assertIn("README.md\t", result.summary)
        self.assertIn("secrets.yaml\t", result.summary)
        self.assertIn("(protected)", result.summary)
        self.assertNotIn(".git/", result.summary)
        self.cfg.max_list_entries = 2
        self.assertIn("more", self.run_req(req("listdir")).summary)

    def test_writefile_keeps_a_backup(self):
        result = self.run_req(req("writefile notes/new.txt\n```\nfirst\n```"))
        self.assertEqual((self.ws / "notes" / "new.txt").read_text(), "first\n")
        self.assertIn("new file", result.summary)
        result = self.run_req(req("writefile README.md\n```\n# Brew 2\n```"))
        self.assertEqual((self.ws / "README.md").read_text(), "# Brew 2\n")
        self.assertEqual((self.ws / "README.md.bak").read_text(), "# Brew\nhello world\n")
        self.assertIn("+# Brew 2", result.summary)
        self.assertIn("README.md.bak", result.summary)

    @unittest.skipUnless(os.name == "posix", "POSIX file permissions")
    def test_permissions_are_kept(self):
        os.chmod(self.ws / "README.md", 0o600)
        self.run_req(req("writefile README.md\n```\nnew\n```"))
        self.assertEqual((self.ws / "README.md").stat().st_mode & 0o777, 0o600)
        self.assertEqual((self.ws / "README.md.bak").stat().st_mode & 0o777, 0o600)
        old = os.umask(0o022)
        try:
            self.run_req(req("writefile fresh.txt\n```\nx\n```"))
        finally:
            os.umask(old)
        self.assertEqual((self.ws / "fresh.txt").stat().st_mode & 0o777, 0o644)

    def test_writefile_keeps_crlf_line_endings(self):
        (self.ws / "win.txt").write_bytes(b"a\r\nb\r\n")
        self.run_req(req("writefile win.txt\n```\na\nc\n```"))
        self.assertEqual((self.ws / "win.txt").read_bytes(), b"a\r\nc\r\n")

    def test_backup_symlink_is_replaced_not_followed(self):
        symlink_or_skip(self, self.outside / "data.txt", self.ws / "README.md.bak")
        self.run_req(req("writefile README.md\n```\nnew\n```"))
        self.assertEqual((self.outside / "data.txt").read_text(), "outside data\n")
        self.assertFalse((self.ws / "README.md.bak").is_symlink())

    def test_edit_replaces_exactly_once(self):
        result = self.run_req(req('edit README.md "find=hello world" "replace=hello brew"'))
        self.assertEqual((self.ws / "README.md").read_text(), "# Brew\nhello brew\n")
        self.assertIn("-hello world", result.summary)
        self.assertIn("+hello brew", result.summary)
        self.assertTrue((self.ws / "README.md.bak").exists())
        write(self.ws / "twice.txt", "a a\n")
        for text in ['edit twice.txt "find=a" "replace=b"', 'edit twice.txt "find=zzz" "replace=b"']:
            with self.subTest(text=text), self.assertRaises(db.BridgeError) as caught:
                self.run_req(req(text))
            self.assertIn("exactly once", str(caught.exception))
        self.assertEqual((self.ws / "twice.txt").read_text(), "a a\n")

    def test_edit_matches_crlf_files(self):
        (self.ws / "crlf.yaml").write_bytes(b"a: 1\r\nb: 2\r\n")
        self.run_req(data_req(action="edit", path="crlf.yaml", find="a: 1\nb: 2", replace="a: 1\nb: 3"))
        self.assertEqual((self.ws / "crlf.yaml").read_bytes(), b"a: 1\r\nb: 3\r\n")

    def test_dry_run_reads_and_writes_nothing(self):
        runner = db.Runner(self.cfg, dry_run=True)
        result = runner.execute(req("writefile README.md\n```\nchanged\n```"))
        self.assertIn("dry run", result.summary)
        self.assertEqual((self.ws / "README.md").read_text(), "# Brew\nhello world\n")
        self.assertNotIn("hello", runner.execute(req("readfile README.md")).summary)
        with self.assertRaises(db.BridgeError):  # still validated
            runner.execute(req("readfile secrets.yaml"))


class GitExecutionTests(Base):
    def run_req(self, r, runner=None):
        db.validate(r, self.cfg)
        return (runner or self.runner).execute(r)

    def remote_ref(self, ref):
        result = subprocess.run(["git", "rev-parse", "--verify", "--quiet", ref], cwd=self.remote, capture_output=True, text=True)
        return result.stdout.strip() or None

    def test_status_diff_log_show(self):
        write(self.ws / "README.md", "# Brew\nhello there\n")
        write(self.ws / "keys" / "api_token.txt", "changed-token-content-ABC\n")
        status = self.run_req(req("git status"))
        self.assertTrue(status.ok, status.summary)
        self.assertIn("## main", status.summary)
        diff = self.run_req(req("git diff"))
        self.assertIn("+hello there", diff.summary)
        self.assertNotIn("api_token", diff.summary)  # tracked secret files stay out of diffs
        self.assertNotIn("changed-token-content", diff.summary)
        log = self.run_req(req("git log -n 3"))
        self.assertIn("initial", log.summary)
        show = self.run_req(req("git show"))
        self.assertIn("hello world", show.summary)
        self.assertNotIn("tracked-token-content", show.summary)
        self.assertFalse(self.run_req(req("git show rev=nosuchrev")).ok)
        # A secret file's blob or tree id can't be shown directly: revisions must be commits.
        for ref in ("HEAD:keys/api_token.txt", "HEAD:keys", "HEAD^{tree}"):
            sha = git(self.ws, "rev-parse", ref).strip()
            for text in (f"git show rev={sha}", f"git diff rev={sha}..HEAD", f"git log rev={sha}"):
                with self.subTest(text=text):
                    result = self.run_req(req(text))
                    self.assertFalse(result.ok, result.summary)
                    self.assertNotIn("tracked-token-content", result.summary)

    def test_add_commit_push_to_a_local_bare_remote(self):
        self.run_req(req("git switch -c claude/test"))
        self.run_req(req("writefile README.md\n```\n# Brew\nhello bridge\n```"))
        write(self.ws / "tools" / "secrets.yaml", "x: y\n")
        write(self.ws / "tools" / "id_rsa", "key\n")
        added = self.run_req(req("git add path=README.md path=tools"))
        self.assertTrue(added.ok, added.summary)
        staged = git(self.ws, "diff", "--cached", "--name-only").split()
        self.assertEqual(staged, ["README.md"])  # no secrets, no README.md.bak
        committed = self.run_req(req('git commit -m "Say hello to the bridge"'))
        self.assertTrue(committed.ok, committed.summary)
        pushed = self.run_req(req("push origin claude/test"))
        self.assertTrue(pushed.ok, pushed.summary)
        self.assertEqual(self.remote_ref("refs/heads/claude/test"), git(self.ws, "rev-parse", "HEAD").strip())
        self.assertEqual(git(self.remote, "log", "-1", "--format=%s", "claude/test").strip(), "Say hello to the bridge")
        with self.assertRaises(db.BridgeError):
            self.run_req(req("push origin main"))

    def test_rejected_push_is_not_forced(self):
        self.run_req(req("git switch -c claude/nf"))
        write(self.ws / "a.txt", "one\n")
        self.run_req(req("git add path=a.txt"))
        self.run_req(req('git commit -m one'))
        self.assertTrue(self.run_req(req("push claude/nf")).ok)
        first = self.remote_ref("refs/heads/claude/nf")
        git(self.ws, "reset", "-q", "--hard", "HEAD~1")  # rewrite history behind the bridge's back
        write(self.ws / "b.txt", "two\n")
        self.run_req(req("git add path=b.txt"))
        self.run_req(req("git commit -m two"))
        result = self.run_req(req("push claude/nf"))
        self.assertFalse(result.ok, result.summary)
        self.assertEqual(self.remote_ref("refs/heads/claude/nf"), first)

    def test_pull_fetch_and_switch(self):
        other = self.tmp / "clone"
        git(self.tmp, "-c", "core.autocrlf=false", "clone", "-q", str(self.remote), str(other))
        init_repo_identity(other)
        write(other / "new.txt", "from elsewhere\n")
        git(other, "add", "new.txt")
        git(other, "commit", "-q", "-m", "elsewhere")
        git(other, "push", "-q", "origin", "main", "main:claude/remote-branch")
        fetched = self.run_req(req("git fetch origin"))
        self.assertTrue(fetched.ok, fetched.summary)
        pulled = self.run_req(req("git pull"))
        self.assertTrue(pulled.ok, pulled.summary)
        self.assertEqual((self.ws / "new.txt").read_text(), "from elsewhere\n")
        switched = self.run_req(req("git switch claude/remote-branch"))
        self.assertTrue(switched.ok, switched.summary)
        self.assertEqual(git(self.ws, "branch", "--show-current").strip(), "claude/remote-branch")
        self.assertIn("claude/remote-branch", self.runner.execute(db.Request(action="status")).summary)

    def test_dry_run_never_runs_git(self):
        runner = db.Runner(self.cfg, dry_run=True)
        result = self.run_req(req("push origin claude/dry"), runner)
        self.assertIn("dry run", result.summary)
        self.assertIn("refs/heads/claude/dry", result.summary)
        self.assertIsNone(self.remote_ref("refs/heads/claude/dry"))


def init_repo_identity(path):
    for key, value in [("user.name", "Other"), ("user.email", "other@example.invalid"), ("commit.gpgsign", "false")]:
        git(path, "config", key, value)


class RunExecutionTests(Base):
    git_repo = False

    def run_argv(self, *extra, runner=None):
        r = data_req(action="run", argv=[PY, "tools/hello.py", *extra])
        db.validate(r, self.cfg)
        return (runner or self.runner).execute(r)

    def test_allowlisted_command_runs_without_the_token_and_masked(self):
        self.cfg.token = "ac_bridgeTokenValue123"
        env = {"EHGI_DEVICE_BRIDGE_TOKEN": self.cfg.token, "MY_SERVICE_TOKEN": "service-secret-987"}
        with mock.patch.dict(os.environ, env):
            result = self.run_argv("--leak", "out.json")
        self.assertTrue(result.ok, result.summary)
        self.assertIn("args: --leak out.json", result.summary)
        self.assertIn("hub token in env: False", result.summary)
        self.assertIn("service token in env: False", result.summary)
        self.assertNotIn("hunter2-long-value", result.summary)
        self.assertIn("api_password: ***", result.summary)
        self.assertIn("(no service token)", result.summary)

    def test_long_output_is_attached_and_the_tail_shown(self):
        result = self.run_argv("--many")
        self.assertIn("line 199", result.summary)
        self.assertNotIn("line 5\n", result.summary)
        self.assertIn("attached", result.summary)
        self.assertIn(b"line 0\n", result.files[0][1])

    def test_timeout_and_output_limit(self):
        self.cfg.run_timeout = 2
        r = data_req(action="run", argv=[PY, "tools/slow.py"])
        result = self.runner.execute(r)
        self.assertFalse(result.ok)
        self.assertIn("timed out", result.summary)
        self.assertIn("tick", result.summary)
        self.cfg.max_output_bytes = 50
        result = self.run_argv("--many")
        self.assertTrue(result.ok)
        self.assertIn("output truncated at 50 bytes", result.summary)
        self.assertNotIn("line 199", result.summary)

    def test_dry_run_does_not_run(self):
        result = self.run_argv("x", runner=db.Runner(self.cfg, dry_run=True))
        self.assertIn("dry run", result.summary)
        self.assertNotIn("args:", result.summary)

    def test_missing_program_is_reported(self):
        self.cfg.workspaces["brew"].commands.append(["no-such-program-xyz"])
        r = data_req(action="run", argv=["no-such-program-xyz"])
        with self.assertRaises(db.BridgeError) as caught:
            self.runner.execute(r)
        self.assertIn("Could not start", str(caught.exception))


# ── configuration ───────────────────────────────────────────────────────────


class ConfigTests(unittest.TestCase):
    def test_example_config_loads(self):
        cfg = db.Config.load(ROOT / "tools" / "device_bridge" / "config.example.toml", need_token=False)
        self.assertEqual(cfg.handle, "deviceuse-bridge")
        self.assertIn("esphomebrew", cfg.workspaces)
        self.assertIn("claude", cfg.allowed_requesters)
        self.assertNotIn("main", cfg.workspaces["esphomebrew"].push_branches)
        self.assertTrue(set(cfg.enabled) <= set(db.ACTIONS))
        self.assertTrue(cfg.secret_patterns)

    def config_file(self, tmp, text):
        path = Path(tmp) / "bridge.toml"
        write(path, textwrap.dedent(text))
        return path

    def test_defaults_are_conservative(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = db.Config.load(self.config_file(tmp, """
                [hub]
                project_id = "p1"
                owner = "nona"
                [workspaces]
                brew = "~/code/brew"
            """), need_token=False)
        self.assertEqual(cfg.handle, "deviceuse-bridge")
        self.assertEqual(cfg.allowed_requesters, ["nona", "claude"])
        self.assertEqual(cfg.enabled, ["status", "readfile", "listdir", "git"])
        self.assertEqual(cfg.git_subcommands, ["status", "diff", "log", "show"])
        self.assertEqual(cfg.workspaces["brew"].push_branches, ["claude/*"])
        self.assertEqual(cfg.workspaces["brew"].root, Path(os.path.expanduser("~/code/brew")).resolve())
        self.assertEqual(cfg.default_workspace, "brew")
        self.assertIn(".github/workflows/*", cfg.workspaces["brew"].readonly)

    def test_bad_configs_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            for body in ['[hub]\nproject_id = "p"\n',
                         '[hub]\nproject_id = "p"\n[workspaces]\n"bad name" = "/x"\n',
                         '[hub]\nproject_id = "p"\n[workspaces.a]\npath = "/x"\ncommands = ["make"]\n']:
                with self.subTest(body=body), self.assertRaises(SystemExit):
                    db.Config.load(self.config_file(tmp, body), need_token=False)

    def test_token_must_look_like_an_agent_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = self.config_file(tmp, '[hub]\nproject_id = "p"\n[workspaces]\na = "/x"\n')
            for token in ["", "ac_PASTE_TOKEN", "hello"]:
                with self.subTest(token=token), mock.patch.dict(os.environ, {"EHGI_DEVICE_BRIDGE_TOKEN": token}), \
                        self.assertRaises(SystemExit):
                    db.Config.load(path)
            with mock.patch.dict(os.environ, {"EHGI_DEVICE_BRIDGE_TOKEN": "ac_realLooking123"}):
                self.assertEqual(db.Config.load(path).token, "ac_realLooking123")


if __name__ == "__main__":
    unittest.main()
