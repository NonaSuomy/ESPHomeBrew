# Device bridge: let trusted agents work in a few folders on your machine

`tools/device_bridge/device_bridge.py` runs on your own computer (written for an Arch Linux box). It joins an EhGI project **as its own agent**, by default `@deviceuse-bridge`. When you or `@claude` ask it to, it reads and edits files in the folders you list, runs a fixed set of git commands, pushes to branches you allow, and runs commands you allowlist. It replies in the same thread, and attaches long output as a file.

It is the sibling of the [ESP bridge](esp-bridge.md), which drives an ESP32 device. It shares that bridge's hub client, request format and masking. It is **not** a shell: every request is checked against `bridge.toml`, and anything not listed there is refused with a reason.

```
you / @claude in EhGI ──"@deviceuse-bridge git status"──▶ EhGI hub ◀── bridge (your machine) ──▶ git / files in ~/code/esphomebrew
                      ◀── result (+ output file) ─────────────────┘
```

The bridge only connects **out** to the hub, the same way coding agents do. Nothing on your machine accepts incoming connections.

## Security model

Read this before you enable writes or `run`.

- **Who.** Only the handles in `[hub].allowed_requesters` may use it (default: `owner` and `claude`). Anyone else gets a refusal and nothing runs. System and GitHub feed messages are never treated as requests.
- **Where.** Only the folders under `[workspaces]`, by name. Every path is relative to its workspace. The bridge refuses `..`, absolute paths, `~` and backslashes, then resolves the path, symlinks included, and refuses it if it ends up outside the workspace.
- **Never touched.** `.git` internals, and files whose name (or any folder in the path) matches `[files].secret_patterns`: `secrets.yaml`, `.env`, `*token*`, `*secret*`, `*password*`, `id_*`, `*.pem`, `*.key`, `.ssh`, `.netrc` and others (see `config.example.toml`). These can't be read, written, edited, staged with `git add`, or shown in `git diff`/`git show`. A symlink that points at one of them is refused too.
- **Read-only files.** `writefile` and `edit` never change CI workflows (`.github/workflows/*`, which run with the repository's secrets when pushed), git hook folders (`.githooks/*`, `.husky/*`) or `.gitmodules`. Each workspace can add its own `readonly` globs.
- **Git.** Only `status`, `diff`, `log` (bounded), `show`, `add`, `commit -m`, `pull --ff-only`, `switch`/`checkout -b` and `fetch`, each enabled separately in `[git].subcommands`. The bridge builds every git command line itself. Only a few familiar flags are understood (`--cached`, `--stat`, `-n`, `-m`, `-c`/`-b`, `--prune`, `-u`), and anything else is refused. Paths are passed as literal pathspecs, so `*` or `:(top)` in a path means nothing special. Revisions must name commits, so `rev:path`, blob ids and tree ids are refused. `pull` and `fetch` only name remotes from `[git].remotes`, never URLs. Git never prompts (`GIT_TERMINAL_PROMPT=0`) and never gets the hub token.
- **Push.** Push is a separate action. It only goes to `[push].remotes` and to branch patterns in `[push].branches` (default `claude/*`). `main` is allowed only if you add it. There is never a `--force`, `+refspec`, `--delete`, `:branch` or `--mirror`, and tags don't follow. A non-fast-forward push simply fails.
- **Run.** Only commands whose argv starts with a prefix listed for that workspace (`commands`), or matches one exactly (`commands_exact`). The command runs as argv with no shell, with the workspace as its working directory. It has a timeout (its child processes are stopped too), and its output is capped. Arguments after a prefix can't be absolute paths, `~`, `..`, URLs or secret files. `run` gets your environment minus the hub token and every secret-looking variable (`…_TOKEN`, `…_KEY`, `…PASSWORD…`), unless you list it in `[run].pass_env`.
- **Output.** Before anything is posted, the bridge replaces these with `***`: the hub token, the values of secret-looking environment variables, values in each workspace's `secrets.yaml`/`.env` and in `[masking].secrets_files`, well-known token formats (`ghp_…`, `github_pat_…`, `glpat-…`, AWS keys, private key blocks), and credentials inside URLs. Config files shown with `readfile` also hide `password:`/`key:`/`token:` values. Lines in posted files can't mention anyone or turn into bridge requests.
- **No network of its own.** Apart from the hub connection, the bridge only reaches the network through `git pull`/`fetch`/`push`, using your local git and your machine's own credentials (ssh agent or credential helper). It makes no GitHub API calls.

**The trust boundary is the requester list.** `writefile`/`edit` together with `run` means requesters can change what a command does. With `run` allowing `make`, a changed `Makefile` runs anything. With `python3 tools/x.py`, a new `tools/json.py` next to it shadows a standard module. `pull`/`switch` can bring in changed scripts too. So:

- List only people and agents you'd trust with a shell in those folders. The default is you and `claude`.
- Make everything `run` executes `readonly` (for example `["Makefile", "tools/*"]`).
- Prefer `commands_exact` for tools whose flags can run code (`make --eval=…`, `python -c`), and never allow an interpreter by itself as a prefix.
- Start with the read-only actions and add `writefile`, `edit`, `push` and `run` once you're comfortable.
- For more isolation, run the bridge as a separate user that owns only the workspaces and has its own push credentials.

## Setup on Arch Linux (once)

You need `git` and Python 3.11 or newer (`pacman -S --needed git python`). The bridge uses only the standard library.

1. **Give the bridge a seat.** On ehgi.ai, open the project and click **+ Add agent** (Agents page, or the Agents list in the chat's team panel).
   - **Handle:** `deviceuse-bridge`. **Client:** **Other**, because the bridge isn't a coding agent.
   - Click **Reserve seat and get token**, then **Copy token**. Skip the install snippets.
   - Don't paste the token into chat. The agent's page can issue a new one.
2. **Get the tool.** It imports `tools/esp_bridge/esp_bridge.py`, so keep the whole checkout:
   ```sh
   git clone https://github.com/NonaSuomy/papp-conversions.git ~/code/papp-bridge/tool
   mkdir -p ~/.config/device-bridge
   cp ~/code/papp-bridge/tool/tools/device_bridge/config.example.toml ~/.config/device-bridge/bridge.toml
   chmod 600 ~/.config/device-bridge/bridge.toml
   ```
3. **Edit `bridge.toml`:**
   - `[hub]`: the `project_id`, your `owner` handle, and `allowed_requesters`.
   - `[workspaces.<name>]`: each folder's `path`, its `readonly` files and its `run` commands.
   - `[push]`: which branches the bridge may push.
   - `[actions]`: which actions are enabled.
4. **Store the token privately.** This reads it without echoing it or saving it in your shell history. Paste the `ac_…` token, then press Enter:
   ```sh
   read -rs T && printf 'EHGI_DEVICE_BRIDGE_TOKEN=%s\n' "$T" > ~/.config/device-bridge/env && chmod 600 ~/.config/device-bridge/env && unset T
   ```
5. **Check, then try it without doing anything:**
   ```sh
   set -a; . ~/.config/device-bridge/env; set +a
   B=~/code/papp-bridge/tool/tools/device_bridge/device_bridge.py
   python $B --config ~/.config/device-bridge/bridge.toml local "status"          # no hub needed
   python $B --config ~/.config/device-bridge/bridge.toml check                   # hub connectivity
   python $B --config ~/.config/device-bridge/bridge.toml --dry-run serve         # validate only
   ```
   Post `@deviceuse-bridge status` in the project and it answers. In dry-run mode every request is validated and reported, but nothing is read, written or run. Stop it with Ctrl+C, then run `serve` without `--dry-run`.

### Keep it running (systemd user service)

`~/.config/systemd/user/device-bridge.service`:

```ini
[Unit]
Description=Device bridge for EhGI
After=network-online.target

[Service]
EnvironmentFile=%h/.config/device-bridge/env
# For `push` over ssh, point git at your agent's socket (pick yours):
# Environment=SSH_AUTH_SOCK=%t/ssh-agent.socket
# Environment=SSH_AUTH_SOCK=%t/gcr/ssh
ExecStart=/usr/bin/python %h/code/papp-bridge/tool/tools/device_bridge/device_bridge.py --config %h/.config/device-bridge/bridge.toml serve
Restart=on-failure
RestartSec=10
NoNewPrivileges=yes
UMask=0022

[Install]
WantedBy=default.target
```

```sh
systemctl --user daemon-reload
systemctl --user enable --now device-bridge
journalctl --user -u device-bridge -f      # every request and result is logged here
```

To stop it, use the agent's **Stop** button in EhGI, or `systemctl --user stop device-bridge`. It stops by itself if the hub rejects its token (`check` explains why). To keep it running while you're logged out, run `loginctl enable-linger $USER`.

**Git credentials under systemd:** the service has no terminal, so git can't ask for a password or passphrase.
- For ssh remotes, use an ssh agent that has your key loaded, and set `SSH_AUTH_SOCK` as above.
- For https remotes, use a credential helper, for example `git config --global credential.helper libsecret` or `gh auth setup-git`.
- If you sign commits, the signing key must be usable without a prompt (a cached gpg-agent, or ssh signing through the agent). Otherwise turn off `commit.gpgsign` for that repository.

## Sending it requests

Mention the bridge at the **start of a line**: an action, then `key=value` options. `ws=` names the workspace; leave it out when there is only one, or set `[hub].default_workspace`.

| Request | Does |
|---|---|
| `@deviceuse-bridge status` | Shows workspaces (and their branch), enabled actions, push rules and `run` commands |
| `@deviceuse-bridge listdir ws=esphomebrew path=tools` | Lists one folder, not recursive. Secret-looking names are marked `(protected)` |
| `@deviceuse-bridge readfile ws=esphomebrew path=README.md` | Posts a text file (up to `max_read_bytes`). Long files are attached whole, binary files only by size and hash |
| `@deviceuse-bridge writefile ws=esphomebrew path=docs/notes.md` + a code block | Replaces the whole file with the code block (or creates it). Keeps the old one as `notes.md.bak` and shows a diff |
| `@deviceuse-bridge edit ws=esphomebrew path=listing.yaml "find=refresh: 1d" "replace=refresh: 0s"` | Replaces text that occurs **exactly once**, keeps a `.bak`, and shows a diff |
| `@deviceuse-bridge git status ws=esphomebrew` | `git status --short --branch` |
| `@deviceuse-bridge git diff ws=esphomebrew --cached path=listing.yaml` | Diff of staged changes (`--stat` and `rev=` work too) |
| `@deviceuse-bridge git log ws=esphomebrew -n 10` | The last commits, one line each (at most `[git].max_log`) |
| `@deviceuse-bridge git show ws=esphomebrew rev=HEAD~1 --stat` | One commit |
| `@deviceuse-bridge git add ws=esphomebrew path=listing.yaml path=tools/make_listing.py` | Stages those paths. Secret files and `*.bak` are never staged, even from a folder |
| `@deviceuse-bridge git commit ws=esphomebrew -m "Refresh the listing"` | Commits what is staged |
| `@deviceuse-bridge git switch ws=esphomebrew -c claude/refresh-listing` | Creates a branch and switches to it (`git checkout -b …` works too; `git switch main` changes branch) |
| `@deviceuse-bridge git pull ws=esphomebrew` | `git pull --ff-only` from the upstream (or `git pull origin main`) |
| `@deviceuse-bridge git fetch ws=esphomebrew --prune` | `git fetch` |
| `@deviceuse-bridge push ws=esphomebrew claude/refresh-listing` | Pushes that local branch to `origin` (`push origin claude/x`, `from=<local branch>` and `-u` also work) |
| `@deviceuse-bridge run ws=esphomebrew -- python3 tools/make_listing.py --check` | Runs an allowlisted command. The reply shows the last 60 lines and attaches the full output |

A typical agent loop: `git switch -c claude/x`, `edit`/`writefile`, `run` the checks, `git diff`, `git add`, `git commit -m`, `push claude/x`. Then open the pull request from your own GitHub account; the bridge never talks to the GitHub API.

**Multi-line edits.** Put the text to find in the first code block and its replacement in the second:

````
@deviceuse-bridge edit ws=esphomebrew path=listing.yaml
```
refresh: 1d
timeout: 10s
```
```
refresh: 0s
timeout: 30s
```
````

**Agents** can send the same fields as message data. This avoids quoting problems:
`post_message { text: "@deviceuse-bridge edit", data: { device_bridge: { action: "edit", ws: "esphomebrew", path: "listing.yaml", find: "…", replace: "…" } } }`.
For git, add `subcommand` (`{"action": "git", "subcommand": "commit", "message": "…"}`). For `run`, pass `argv` as a list, and for `git add`, pass `paths` as a list.

**Refusals** start with 🚫 and say why. For example, ``🚫 `../x`: `..` is not allowed in paths.``, ``🚫 Pushing to `main` is not allowed on `esphomebrew` (allowed: claude/*).`` or ``🚫 `--force` is never used: the bridge does not force, delete or discard anything.``

**In the terminal or journal**, every request is printed with who sent it and the result. Set `[console] show_requests = false` to turn that off.

## Tests

`python3 -m unittest discover -s tests` runs `tests/test_device_bridge.py`. It covers:
- request parsing;
- path confinement: `..`, absolute paths, symlinks out of a workspace and onto `.git` or secret files;
- secret and read-only files;
- the exact git command lines, and disallowed subcommands and flags;
- push rules: branch patterns, remotes, force and delete;
- `run` prefix and exact matching, and argument checks;
- requester refusals, masking, and the environment `run` and git get;
- real runs in temporary git repositories: `writefile`, `edit`, `git add`/`commit`, and `push` to a local bare remote, including a rejected non-fast-forward push.

CI runs it on every change to `tools/device_bridge/`.
