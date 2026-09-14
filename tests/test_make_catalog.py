"""Tests for tools/make_catalog.py: catalog page, store.json and app data lists.

Run: python3 -m unittest discover -s tests
"""

import base64
import hashlib
import io
import json
import subprocess
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import make_catalog as mc  # noqa: E402

REPO = "NonaSuomy/papp-conversions"
WAD = b"IWAD" + b"\x00" * 60
PAK = b"PACK" + b"\x01" * 100


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def app(name="psram_doom", data=True) -> dict:
    a = {"name": name, "title": "Doom", "version": "0.1.0", "description": "d", "file": f"{name}.papp",
         "size": 40, "sha256": "ab" * 32, "abi": 1, "source": {"repo": "r", "ref": "f" * 40, "path": "p"}}
    if data:
        a["data"] = {
            "repo": "https://github.com/NonaSuomy/RetroESP32-P4", "ref": "a" * 40, "license": "shareware",
            "files": [
                {"path": "SDcard/roms/doom/doom1.wad", "target": "roms/doom/doom1.wad", "size": len(WAD), "sha256": sha(WAD)},
                {"path": "SDcard/roms/quake/id1/pak0.pak", "target": "roms/quake/id1/pak0.pak", "size": len(PAK), "sha256": sha(PAK)},
            ],
        }
    return a


class FakeOpener:
    def __init__(self, files: dict[str, bytes]):
        self.files, self.calls = files, []

    def __call__(self, url):
        self.calls.append(url)
        return io.BytesIO(self.files[url])


def raw(path: str) -> str:
    return f"https://raw.githubusercontent.com/NonaSuomy/RetroESP32-P4/{'a' * 40}/{path}"


class DataTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name) / "site"

    def tearDown(self):
        self.tmp.cleanup()

    def test_data_list_and_files(self):
        opener = FakeOpener({raw("SDcard/roms/doom/doom1.wad"): WAD, raw("SDcard/roms/quake/id1/pak0.pak"): PAK})
        text = mc.publish_data(REPO, app(), self.out, None, opener)
        lines = text.splitlines()
        self.assertEqual(lines[0], "# papp-data 1")
        size, digest, target, url = lines[2].split(" ")
        self.assertEqual((int(size), digest, target), (len(WAD), sha(WAD), "roms/doom/doom1.wad"))
        self.assertEqual(url, "https://nonasuomy.github.io/papp-conversions/data/psram_doom/roms/doom/doom1.wad")
        self.assertEqual((self.out / "data/psram_doom/roms/quake/id1/pak0.pak").read_bytes(), PAK)
        self.assertEqual(mc.data_list_name(app()), "psram_doom-0.1.0.files")

    def test_wrong_bytes_are_refused(self):
        opener = FakeOpener({raw("SDcard/roms/doom/doom1.wad"): WAD[:-1] + b"X", raw("SDcard/roms/quake/id1/pak0.pak"): PAK})
        with self.assertRaises(ValueError):
            mc.publish_data(REPO, app(), self.out, None, opener)
        self.assertFalse((self.out / "data/psram_doom/roms/doom/doom1.wad").exists())
        self.assertFalse(list(self.out.rglob("*.part")))

    def test_oversized_download_is_refused(self):
        opener = FakeOpener({raw("SDcard/roms/doom/doom1.wad"): WAD + b"extra", raw("SDcard/roms/quake/id1/pak0.pak"): PAK})
        with self.assertRaises(ValueError):
            mc.publish_data(REPO, app(), self.out, None, opener)

    def test_cache_is_used_and_filled(self):
        cache = Path(self.tmp.name) / "cache"
        cache.mkdir()
        (cache / sha(WAD)).write_bytes(WAD)
        opener = FakeOpener({raw("SDcard/roms/quake/id1/pak0.pak"): PAK})
        mc.publish_data(REPO, app(), self.out, cache, opener)
        self.assertEqual(opener.calls, [raw("SDcard/roms/quake/id1/pak0.pak")])
        self.assertEqual((cache / sha(PAK)).read_bytes(), PAK)

    def test_catalog_run_writes_lists_only_for_apps_with_data(self):
        dist = Path(self.tmp.name) / "dist"
        dist.mkdir()
        apps = [app(), app("psram_lvgl", data=False)]
        for a in apps:
            (dist / a["file"]).write_bytes(b"P" * 40)
        (dist / "build.json").write_text(json.dumps({"apps": apps}))
        cache = Path(self.tmp.name) / "cache"
        cache.mkdir()
        (cache / sha(WAD)).write_bytes(WAD)
        (cache / sha(PAK)).write_bytes(PAK)
        script = Path(mc.__file__)
        subprocess.run([sys.executable, str(script), "--repo", REPO, "--dist", str(dist), "--out", str(self.out),
                        "--data-cache", str(cache)], check=True, capture_output=True)
        self.assertTrue((self.out / "psram_doom-0.1.0.files").exists())
        self.assertFalse((self.out / "psram_lvgl-0.1.0.files").exists())
        store = {a["name"]: a for a in json.loads((self.out / "store.json").read_text())["apps"]}
        self.assertEqual(store["psram_doom"]["data"]["list_url"], "https://nonasuomy.github.io/papp-conversions/psram_doom-0.1.0.files")
        self.assertNotIn("data", store["psram_lvgl"])
        page = (self.out / "index.html").read_text()
        self.assertNotIn(".files", page)  # the device only follows .papp links

    def test_listing_sidecar_and_icon(self):
        dist = Path(self.tmp.name) / "dist"
        dist.mkdir()
        icon = mc_png(48, 48)
        rich = app("psram_lvgl", data=False)
        rich.update({"author": "giltal", "category": "Demo", "about": "Longer text.", "controls": ["Touch: everything"],
                     "license": "MIT", "changelog": "0.1.0: first release", "upstream": {"project": "LVGL", "version": "9.2"},
                     "canvas": ["1024x600", "800x480"],
                     "screenshots": [{"type": "image/png", "width": 800, "height": 480, "base64": base64.b64encode(mc_png(800, 480)).decode()}],
                     "icon": {"type": "image/png", "width": 48, "height": 48,
                              "base64": base64.b64encode(icon).decode()}})
        plain = app()
        for a in (rich, plain):
            (dist / a["file"]).write_bytes(b"P" * 40)
        (dist / "build.json").write_text(json.dumps({"apps": [rich, plain]}))
        cache = Path(self.tmp.name) / "cache"
        cache.mkdir()
        (cache / sha(WAD)).write_bytes(WAD)
        (cache / sha(PAK)).write_bytes(PAK)
        subprocess.run([sys.executable, str(Path(mc.__file__)), "--repo", REPO, "--dist", str(dist), "--out", str(self.out),
                        "--data-cache", str(cache)], check=True, capture_output=True)
        store = {a["name"]: a for a in json.loads((self.out / "store.json").read_text())["apps"]}
        entry = store["psram_lvgl"]
        self.assertEqual((entry["author"], entry["category"], entry["controls"]), ("giltal", "Demo", ["Touch: everything"]))
        self.assertEqual(entry["icon"]["url"], "https://nonasuomy.github.io/papp-conversions/psram_lvgl-0.1.0.png")
        self.assertEqual(entry["info_url"], "https://nonasuomy.github.io/papp-conversions/psram_lvgl-0.1.0.json")
        self.assertEqual((self.out / "psram_lvgl-0.1.0.png").read_bytes(), icon)
        self.assertEqual((entry["license"], entry["changelog"]), ("MIT", "0.1.0: first release"))
        self.assertEqual(entry["upstream"], {"project": "LVGL", "version": "9.2"})
        self.assertEqual(entry["canvas"], ["1024x600", "800x480"])  # the store's Screen setting
        self.assertNotIn("canvas", store["psram_doom"])
        self.assertEqual(entry["screenshots"], [{"width": 800, "height": 480,
                                                 "url": "https://nonasuomy.github.io/papp-conversions/psram_lvgl-0.1.0-screen1.png"}])
        self.assertEqual((self.out / "psram_lvgl-0.1.0-screen1.png").read_bytes(), mc_png(800, 480))
        self.assertEqual(json.loads((self.out / "psram_lvgl-0.1.0.json").read_text()), entry)
        # Sizes for "PAPP X KB + data Y MB", also for apps without listing extras.
        self.assertEqual((store["psram_doom"]["size"], store["psram_doom"]["data_size"]), (40, len(WAD) + len(PAK)))
        self.assertNotIn("icon", store["psram_doom"])
        self.assertTrue((self.out / "psram_doom-0.1.0.json").exists())
        page = (self.out / "index.html").read_text()
        self.assertIn('<img src="psram_lvgl-0.1.0.png"', page)
        self.assertNotIn("base64", page)  # icons stay out of the 64 KB page


def mc_png(width: int, height: int) -> bytes:
    """A minimal PNG header (signature + IHDR) of the given size, enough for the checks."""
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    chunk = b"IHDR" + ihdr
    return (b"\x89PNG\r\n\x1a\n" + struct.pack(">I", len(ihdr)) + chunk
            + struct.pack(">I", zlib.crc32(chunk) & 0xFFFFFFFF) + b"\x00\x00\x00\x00IEND\xaeB`\x82")


if __name__ == "__main__":
    unittest.main()
