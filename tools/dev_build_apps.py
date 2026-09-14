#!/usr/bin/env python3
"""Which work-in-progress apps a branch's dev build should build.

Work-in-progress apps are those with "publish": false or "dev_build": true
in apps/<name>/papp.json. Every branch uploads its dev builds to the same
"dev-builds" release, so a branch that rebuilt all of them would replace
another branch's test build of an app it never touched. A branch therefore
builds only the apps whose files it changes: apps/<name>/ and the folders
the manifest points at with "local:" paths (ports/<port>/...). Changes to
the build itself (tools/build_papp.py, the workflow) rebuild them all, and
so does a run with no main branch to compare against.

    python3 tools/dev_build_apps.py [--base origin/main]   # prints the names
    python3 tools/dev_build_apps.py --all                  # every one (a manual run)
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

# Files every app build depends on: a change here rebuilds all of them.
SHARED = ("tools/build_papp.py", ".github/workflows/dev-builds.yml", "tools/dev_build_apps.py")


def wip_apps(apps_dir: Path) -> dict[str, dict]:
    """Work-in-progress apps by name, with their manifests."""
    found = {}
    for manifest in sorted(apps_dir.glob("*/papp.json")):
        data = json.loads(manifest.read_text(encoding="utf-8"))
        if data.get("publish", True) is False or data.get("dev_build"):
            found[manifest.parent.name] = data
    return found


def app_roots(name: str, manifest: dict) -> set[str]:
    """Folder prefixes whose changes affect this app's build."""
    roots = {f"apps/{name}/"}

    def walk(value) -> None:
        if isinstance(value, str) and value.startswith("local:"):
            parts = value[len("local:"):].split("/")
            if len(parts) >= 2:
                roots.add("/".join(parts[:2]) + "/")
        elif isinstance(value, list):
            for item in value:
                walk(item)
        elif isinstance(value, dict):
            for item in value.values():
                walk(item)

    walk(manifest)
    return roots


def select(apps: dict[str, dict], changed: list[str] | None) -> list[str]:
    """The apps to build for a branch that changed these files (None: unknown)."""
    if changed is None or any(path in SHARED for path in changed):
        return sorted(apps)
    return sorted(
        name for name, manifest in apps.items()
        if any(path.startswith(root) for path in changed for root in app_roots(name, manifest))
    )


def changed_files(base: str) -> list[str] | None:
    """Files this branch changed since it left base, or None if git cannot tell."""
    result = subprocess.run(["git", "diff", "--name-only", f"{base}...HEAD"],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        return None
    return [line for line in result.stdout.splitlines() if line]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default="origin/main", help="branch to compare against (default: origin/main)")
    parser.add_argument("--apps", default="apps", type=Path, help="apps folder (default: apps)")
    parser.add_argument("--all", action="store_true", help="every work-in-progress app, changed or not")
    args = parser.parse_args(argv)
    changed = None if args.all else changed_files(args.base)
    print(" ".join(select(wip_apps(args.apps), changed)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
