#!/usr/bin/env python3
"""Write the store catalog (index.html + store.json) from dist/build.json.

The ESPHome papp_loader reads `catalog_url` as HTML: every href ending in
.papp becomes a button labelled with the link's file name, and relative links
resolve against the catalog URL. Each app's .papp is copied into the site
itself (e.g. psram_lvgl-0.1.0.papp) and linked relatively, so devices download
from GitHub Pages. Pages uses Let's Encrypt (ISRG Root X1), which the ESP-IDF
certificate bundle verifies; github.com release downloads chain to Sectigo's
newer ECC root and fail verification on-device (seen on ESP-IDF 6.1). The
GitHub Release stays the versioned archive and is linked by its tag page, which
is not a .papp link. The loader downloads at most 64 KB of catalog, so the page
stays small and holds no other .papp links.

    python3 tools/make_catalog.py --repo OWNER/REPO --dist dist --out site
"""

from __future__ import annotations

import argparse
import html
import json
import shutil
import sys
from pathlib import Path

MAX_CATALOG_BYTES = 64 * 1024


def release_tag(app: dict) -> str:
    return f"{app['name']}-v{app['version']}"


def asset_name(app: dict) -> str:
    return f"{app['name']}-{app['version']}.papp"


def asset_url(repo: str, app: dict) -> str:
    """Archived copy on the GitHub Release."""
    return f"https://github.com/{repo}/releases/download/{release_tag(app)}/{asset_name(app)}"


def pages_url(repo: str, app: dict) -> str:
    """Where devices download it: the copy on GitHub Pages next to index.html."""
    owner, name = repo.split("/", 1)
    return f"https://{owner.lower()}.github.io/{name}/{asset_name(app)}"


def release_page(repo: str, app: dict) -> str:
    return f"https://github.com/{repo}/releases/tag/{release_tag(app)}"


def render(repo: str, apps: list[dict]) -> str:
    rows = []
    for app in sorted(apps, key=lambda a: a["name"]):
        rows.append(
            f'<li><a href="{html.escape(asset_name(app))}">{html.escape(asset_name(app))}</a>'
            f" {html.escape(app['title'])} &middot; {app['size'] // 1024} KB"
            f" &middot; <code>{app['sha256'][:12]}</code>"
            f' &middot; <a href="{html.escape(release_page(repo, app))}">release</a>'
            + (f"<br><small>{html.escape(app['description'])}</small>" if app.get("description") else "")
            + "</li>"
        )
    return (
        "<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>PAPP Store</title>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"></head>\n"
        f"<body><h1>PAPP Store</h1><p>Apps for the ESP32-P4 from "
        f"<a href=\"https://github.com/{html.escape(repo)}\">{html.escape(repo)}</a>. "
        "Point the ESPHome <code>papp_loader</code> <code>catalog_url</code> at this page.</p>\n"
        "<ul>\n" + "\n".join(rows) + "\n</ul>\n"
        "<p><a href=\"store.json\">store.json</a></p></body></html>\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", required=True, help="OWNER/REPO that hosts the releases")
    parser.add_argument("--dist", type=Path, default=Path("dist"))
    parser.add_argument("--out", type=Path, default=Path("site"))
    args = parser.parse_args()

    apps = json.loads((args.dist / "build.json").read_text())["apps"]
    page = render(args.repo, apps)
    if len(page.encode()) > MAX_CATALOG_BYTES:
        print(f"catalog is {len(page.encode())} bytes; the device reads at most {MAX_CATALOG_BYTES}", file=sys.stderr)
        return 1

    store = {
        "apps": [
            {
                "name": a["name"],
                "title": a["title"],
                "version": a["version"],
                "description": a.get("description", ""),
                "file": asset_name(a),
                "url": pages_url(args.repo, a),
                "release_url": asset_url(args.repo, a),
                "size": a["size"],
                "sha256": a["sha256"],
                "abi": a["abi"],
                "source": a["source"],
            }
            for a in sorted(apps, key=lambda a: a["name"])
        ]
    }
    args.out.mkdir(parents=True, exist_ok=True)
    for a in apps:
        shutil.copyfile(args.dist / a["file"], args.out / asset_name(a))
    (args.out / "index.html").write_text(page)
    (args.out / "store.json").write_text(json.dumps(store, indent=2) + "\n")
    print(f"catalog: {len(apps)} app(s), {len(page.encode())} bytes -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
