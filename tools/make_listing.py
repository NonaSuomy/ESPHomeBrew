#!/usr/bin/env python3
"""Write store listings for a folder of .papp files, such as a LAN server or an SD card folder.

The ESPHOMEBREW store view (papp_loader `library_style: grid`) reads an app's
listing from the .json next to its .papp, and its data list from the .files
next to it (psram_doom-0.1.1.papp -> psram_doom-0.1.1.json, .files). The
GitHub store publishes both; this writes them for any other folder:

    python3 tools/make_listing.py /srv/papp --base-url http://10.20.30.158:8000/

For every .papp in the folder:

- The size and sha256 always come from the file itself, so Install can check
  the download.
- When the app is also in the store (same name, with or without the psram_
  prefix, e.g. doom.papp and psram_doom), its title, icon, about, controls,
  licence, upstream project and data list are copied from the store listing.
- A <stem>.png next to the .papp (at most 256x256, 64 KB) becomes the icon.
- Fields already in an existing <stem>.json are kept, so it can be edited by
  hand and the tool run again. --force starts from scratch.
- Data files listed by the store are fetched from GitHub Pages by default.
  With --mirror-data they are downloaded into <folder>/data/<name>/, checked
  against their sha256, and listed at --base-url instead, so the device gets
  its game data from the LAN too.

The .papp files themselves are never changed.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import struct
import sys
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_catalog as mc  # noqa: E402

DEFAULT_STORE = "https://nonasuomy.github.io/papp-conversions/"
# What describes the app rather than one build of it: the store build's source
# commit, changelog and version are left out, since this .papp may differ.
COPIED_FIELDS = ("title", "description", "author", "category", "license", "about", "controls", "upstream")
MAX_ICON_BYTES = 64 * 1024
MAX_ICON_SIDE = 256
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
VERSIONED = re.compile(r"^(?P<name>.+?)-(?P<version>\d+(?:\.\d+)*)$")


def split_stem(stem: str) -> tuple[str, str]:
    """psram_doom-0.1.1 -> (psram_doom, 0.1.1); doom -> (doom, '')."""
    match = VERSIONED.match(stem)
    return (match["name"], match["version"]) if match else (stem, "")


def default_title(name: str) -> str:
    words = name.removeprefix("psram_").replace("_", " ").replace("-", " ").split()
    return " ".join(w if w.isupper() else w.capitalize() for w in words) or name


def load_store(where: str, opener=urllib.request.urlopen) -> dict[str, dict]:
    """store.json by app name, from a store URL, a store.json URL or a local file."""
    if not where:
        return {}
    if where.startswith(("http://", "https://")):
        url = where if where.endswith(".json") else where.rstrip("/") + "/store.json"
        with opener(url) as response:
            store = json.loads(response.read())
    else:
        path = Path(where)
        store = json.loads((path / "store.json" if path.is_dir() else path).read_text())
    return {a["name"]: a for a in store.get("apps", [])}


def store_match(name: str, store: dict[str, dict]) -> dict | None:
    for candidate in (name, "psram_" + name, name.removeprefix("psram_")):
        if candidate in store:
            return store[candidate]
    return None


def local_icon(folder: Path, stem: str, name: str) -> dict | None:
    for candidate in (folder / f"{stem}.png", folder / f"{name}.png"):
        if not candidate.is_file():
            continue
        png = candidate.read_bytes()
        if len(png) > MAX_ICON_BYTES or png[:8] != PNG_SIGNATURE or png[12:16] != b"IHDR":
            raise ValueError(f"{candidate}: an icon must be a PNG of at most {MAX_ICON_BYTES // 1024} KB")
        width, height = struct.unpack(">II", png[16:24])
        if not (0 < width <= MAX_ICON_SIDE and 0 < height <= MAX_ICON_SIDE):
            raise ValueError(f"{candidate}: the icon is {width}x{height}; at most {MAX_ICON_SIDE}x{MAX_ICON_SIDE}")
        return {"type": "image/png", "width": width, "height": height, "base64": base64.b64encode(png).decode("ascii")}
    return None


def data_list(name: str, version: str, files: list[dict], license_text: str) -> str:
    lines = [mc.DATA_LIST_HEADER, f"# {name} {version}".rstrip() + (f": {license_text}" if license_text else "")]
    lines += [f"{f['size']} {f['sha256']} {f['target']} {f['url']}" for f in files]
    return "\n".join(lines) + "\n"


def build_listing(papp: Path, store: dict[str, dict], base_url: str, mirror: bool, force: bool,
                  opener=urllib.request.urlopen) -> tuple[dict, str | None]:
    """The listing for one .papp, and its data list text (None when it needs no data)."""
    folder, stem = papp.parent, papp.stem
    name, version = split_stem(stem)
    existing_path = folder / f"{stem}.json"
    existing = {} if force or not existing_path.is_file() else json.loads(existing_path.read_text())
    match = store_match(name, store)

    listing: dict = {"name": name, "title": default_title(name), "version": version}
    if match:
        listing.update({k: match[k] for k in COPIED_FIELDS if k in match})
        if match.get("icon"):
            listing["icon"] = {k: v for k, v in match["icon"].items() if k != "url"}
    listing.update({k: v for k, v in existing.items() if k not in ("size", "sha256", "file", "url", "data_size")})
    icon = local_icon(folder, stem, name)
    if icon:
        listing["icon"] = icon

    listing["file"] = papp.name
    if base_url:
        listing["url"] = urllib.parse.urljoin(base_url, urllib.parse.quote(papp.name))
    listing["size"] = papp.stat().st_size
    listing["sha256"] = mc.sha256_file(papp)

    files = (match or {}).get("data", {}).get("files", [])
    listing["data_size"] = sum(f["size"] for f in files)
    if not files:
        return listing, None
    listed = []
    for f in files:
        url = f["url"]
        if mirror:
            site_path = f"data/{name}/{f['target']}"
            dest = folder / site_path
            if not (dest.is_file() and dest.stat().st_size == f["size"] and mc.sha256_file(dest) == f["sha256"]):
                mc.fetch_verified(url, f["size"], f["sha256"], dest, opener)
            url = urllib.parse.urljoin(base_url, urllib.parse.quote(site_path))
        listed.append({**f, "url": url})
    return listing, data_list(name, listing["version"], listed, match["data"].get("license", ""))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder", type=Path, help="folder with the .papp files")
    parser.add_argument("--base-url", default="", help="the folder's URL on the network, e.g. http://10.20.30.158:8000/")
    parser.add_argument("--store", default=DEFAULT_STORE,
                        help="store to copy listings and data lists from (URL or store.json); '' for none")
    parser.add_argument("--mirror-data", action="store_true", help="download app data into <folder>/data/ and list it at --base-url")
    parser.add_argument("--force", action="store_true", help="ignore existing .json listings")
    args = parser.parse_args()

    if args.mirror_data and not args.base_url.startswith(("http://", "https://")):
        parser.error("--mirror-data needs --base-url, the folder's http(s) address")
    if args.base_url and not args.base_url.endswith("/"):
        args.base_url += "/"
    papps = sorted(args.folder.glob("*.papp"))
    if not papps:
        print(f"no .papp files in {args.folder}", file=sys.stderr)
        return 1
    store = load_store(args.store)
    for papp in papps:
        listing, files = build_listing(papp, store, args.base_url, args.mirror_data, args.force)
        (papp.parent / f"{papp.stem}.json").write_text(json.dumps(listing, indent=2) + "\n")
        note = ""
        if files is not None:
            (papp.parent / f"{papp.stem}.files").write_text(files)
            note = f", data {listing['data_size']:,} bytes" + (" (mirrored)" if args.mirror_data else "")
        source = "store listing" if store_match(listing["name"], store) else "basic listing"
        print(f"{papp.name}: {source}{note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
