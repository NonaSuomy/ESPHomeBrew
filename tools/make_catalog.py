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

Apps that need files on the card (a game's data) list them in papp.json under
"data", pinned by commit, size and sha256. Those files are downloaded, checked
and published under data/<app>/, and each such app gets a plain-text file list
next to its .papp (psram_doom-0.1.0.papp -> psram_doom-0.1.0.files). The
loader reads that list before launching and fetches whatever the card is
missing. One line per file:

    <size> <sha256> <target path under the data root> <https URL>

    python3 tools/make_catalog.py --repo OWNER/REPO --dist dist --out site [--data-cache DIR]
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import html
import json
import shutil
import sys
import urllib.parse
import urllib.request
from pathlib import Path

MAX_CATALOG_BYTES = 64 * 1024
DATA_LIST_HEADER = "# papp-data 1"


def release_tag(app: dict) -> str:
    return f"{app['name']}-v{app['version']}"


def asset_name(app: dict) -> str:
    return f"{app['name']}-{app['version']}.papp"


def asset_url(repo: str, app: dict) -> str:
    """Archived copy on the GitHub Release."""
    return f"https://github.com/{repo}/releases/download/{release_tag(app)}/{asset_name(app)}"


def pages_base(repo: str) -> str:
    owner, name = repo.split("/", 1)
    return f"https://{owner.lower()}.github.io/{name}/"


def pages_url(repo: str, app: dict) -> str:
    """Where devices download it: the copy on GitHub Pages next to index.html."""
    return pages_base(repo) + asset_name(app)


def data_list_name(app: dict) -> str:
    """The app's data file list; the loader finds it by swapping .papp for .files."""
    return f"{app['name']}-{app['version']}.files"


def data_site_path(app: dict, target: str) -> str:
    return f"data/{app['name']}/{target}"


def raw_url(data: dict, path: str) -> str:
    """The pinned file in its source repository."""
    repo = data["repo"].removeprefix("https://github.com/").rstrip("/")
    return f"https://raw.githubusercontent.com/{repo}/{data['ref']}/{urllib.parse.quote(path)}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch_verified(url: str, size: int, sha256: str, dest: Path, opener=urllib.request.urlopen) -> None:
    """Download url to dest, refusing anything but the pinned size and sha256."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    digest, got = hashlib.sha256(), 0
    with opener(url) as response, part.open("wb") as out:
        while chunk := response.read(1 << 20):
            got += len(chunk)
            if got > size:
                break
            digest.update(chunk)
            out.write(chunk)
    if got != size or digest.hexdigest() != sha256:
        part.unlink(missing_ok=True)
        raise ValueError(f"{url}: expected {size} bytes sha256 {sha256}, got {got} bytes sha256 {digest.hexdigest()}")
    part.replace(dest)


def publish_data(repo: str, app: dict, out: Path, cache: Path | None, opener=urllib.request.urlopen) -> str:
    """Put the app's pinned data files into the site and return its file list."""
    data = app["data"]
    lines = [DATA_LIST_HEADER, f"# {app['name']} {app['version']}: {data['license']}"]
    for f in data["files"]:
        dest = out / data_site_path(app, f["target"])
        cached = cache / f["sha256"] if cache else None
        if cached and cached.is_file() and cached.stat().st_size == f["size"] and sha256_file(cached) == f["sha256"]:
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(cached, dest)
        else:
            fetch_verified(raw_url(data, f["path"]), f["size"], f["sha256"], dest, opener)
            if cached:
                cached.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(dest, cached)
        url = pages_base(repo) + urllib.parse.quote(data_site_path(app, f["target"]))
        lines.append(f"{f['size']} {f['sha256']} {f['target']} {url}")
    return "\n".join(lines) + "\n"


def data_size(app: dict) -> int:
    return sum(f["size"] for f in app.get("data", {}).get("files", []))


def release_page(repo: str, app: dict) -> str:
    return f"https://github.com/{repo}/releases/tag/{release_tag(app)}"


def sidecar_name(app: dict) -> str:
    """The app's listing as JSON; the loader finds it by swapping .papp for .json."""
    return f"{app['name']}-{app['version']}.json"


def icon_name(app: dict) -> str:
    return f"{app['name']}-{app['version']}.png"


def screenshot_name(app: dict, index: int) -> str:
    return f"{app['name']}-{app['version']}-screen{index + 1}.png"


LISTING_FIELDS = ("author", "category", "license", "about", "changelog", "controls", "upstream", "canvas")


def store_entry(repo: str, a: dict) -> dict:
    """One app in store.json and in its sidecar: what a store UI shows before downloading."""
    return {
        "name": a["name"],
        "title": a["title"],
        "version": a["version"],
        "description": a.get("description", ""),
        **{key: a[key] for key in LISTING_FIELDS if key in a},
        **({"icon": {**a["icon"], "url": pages_base(repo) + icon_name(a)}} if a.get("icon") else {}),
        # Screenshots are big, so only their URLs; the icon stays inline.
        **({"screenshots": [{"width": s["width"], "height": s["height"], "url": pages_base(repo) + screenshot_name(a, i)}
                            for i, s in enumerate(a["screenshots"])]} if a.get("screenshots") else {}),
        "file": asset_name(a),
        "url": pages_url(repo, a),
        "info_url": pages_base(repo) + sidecar_name(a),
        "release_url": asset_url(repo, a),
        "size": a["size"],
        "data_size": data_size(a),
        "sha256": a["sha256"],
        "abi": a["abi"],
        "source": a["source"],
        **({"data": {
            "list_url": pages_base(repo) + data_list_name(a),
            "license": a["data"]["license"],
            "source": {"repo": a["data"]["repo"], "ref": a["data"]["ref"]},
            "files": [{"target": f["target"], "size": f["size"], "sha256": f["sha256"],
                       "url": pages_base(repo) + urllib.parse.quote(data_site_path(a, f["target"]))}
                      for f in a["data"]["files"]],
        }} if a.get("data") else {}),
    }


def render(repo: str, apps: list[dict]) -> str:
    rows = []
    for app in sorted(apps, key=lambda a: a["name"]):
        # Icons are separate files: inline base64 would push the page past the
        # 64 KB the device reads.
        icon = (f'<img src="{html.escape(icon_name(app))}" width="32" height="32" alt=""> '
                if app.get("icon") else "")
        rows.append(
            f'<li>{icon}<a href="{html.escape(asset_name(app))}">{html.escape(asset_name(app))}</a>'
            f" {html.escape(app['title'])} &middot; {app['size'] // 1024} KB"
            + (f" + {data_size(app) / 1e6:.1f} MB data" if app.get("data") else "")
            + f" &middot; <code>{app['sha256'][:12]}</code>"
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
    parser.add_argument("--data-cache", type=Path, help="keep downloaded data files here (by sha256) between runs")
    args = parser.parse_args()

    # Apps marked "publish": false are work in progress: built, but not in the store.
    apps = [a for a in json.loads((args.dist / "build.json").read_text())["apps"] if a.get("publish", True)]
    page = render(args.repo, apps)
    if len(page.encode()) > MAX_CATALOG_BYTES:
        print(f"catalog is {len(page.encode())} bytes; the device reads at most {MAX_CATALOG_BYTES}", file=sys.stderr)
        return 1

    store = {"apps": [store_entry(args.repo, a) for a in sorted(apps, key=lambda a: a["name"])]}
    args.out.mkdir(parents=True, exist_ok=True)
    for a, entry in zip(sorted(apps, key=lambda a: a["name"]), store["apps"]):
        shutil.copyfile(args.dist / a["file"], args.out / asset_name(a))
        # The app's full listing next to its .papp (swap .papp for .json), so a
        # device reading the HTML catalog, or a folder of .papp files, can show it.
        (args.out / sidecar_name(a)).write_text(json.dumps(entry, indent=2) + "\n")
        if a.get("icon"):
            (args.out / icon_name(a)).write_bytes(base64.b64decode(a["icon"]["base64"]))
        for i, shot in enumerate(a.get("screenshots", [])):
            (args.out / screenshot_name(a, i)).write_bytes(base64.b64decode(shot["base64"]))
        if a.get("data"):
            (args.out / data_list_name(a)).write_text(publish_data(args.repo, a, args.out, args.data_cache))
            print(f"data: {a['name']}: {len(a['data']['files'])} file(s), {data_size(a):,} bytes")
    (args.out / "index.html").write_text(page)
    (args.out / "store.json").write_text(json.dumps(store, indent=2) + "\n")
    print(f"catalog: {len(apps)} app(s), {len(page.encode())} bytes -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
