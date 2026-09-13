"""Tests for tools/make_listing.py: listings for a folder of .papp files.

Run: python3 -m unittest discover -s tests
"""

import base64
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import make_listing as ml  # noqa: E402

WAD = b"IWAD" + b"\x00" * 60
PAGES = "https://nonasuomy.github.io/papp-conversions/"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def png(w: int, h: int) -> bytes:
    return ml.PNG_SIGNATURE + struct.pack(">I", 13) + b"IHDR" + struct.pack(">II", w, h) + b"\x00" * 5


STORE = {"apps": [{
    "name": "psram_doom", "title": "Doom", "version": "0.1.1", "author": "PrBoom team", "about": "Shooter.",
    "controls": ["A: fire"], "upstream": {"project": "PrBoom", "version": "2.5.0"}, "changelog": "store build only",
    "source": {"repo": "r", "ref": "f" * 40}, "size": 1, "sha256": "0" * 64,
    "icon": {"type": "image/png", "width": 8, "height": 8, "base64": "AAAA", "url": PAGES + "psram_doom-0.1.1.png"},
    "data": {"license": "shareware", "files": [
        {"target": "roms/doom/doom1.wad", "size": len(WAD), "sha256": sha(WAD), "url": PAGES + "data/psram_doom/roms/doom/doom1.wad"}]},
}]}


class FakeOpener:
    def __init__(self, files):
        self.files, self.calls = files, []

    def __call__(self, url):
        self.calls.append(url)
        return io.BytesIO(self.files[url])


class ListingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.folder = Path(self.tmp.name)
        self.store = {a["name"]: a for a in STORE["apps"]}

    def tearDown(self):
        self.tmp.cleanup()

    def test_names_and_titles(self):
        self.assertEqual(ml.split_stem("psram_doom-0.1.1"), ("psram_doom", "0.1.1"))
        self.assertEqual(ml.split_stem("ESP32_P4_PAPP"), ("ESP32_P4_PAPP", ""))
        self.assertEqual(ml.default_title("psram_atari800"), "Atari800")
        self.assertEqual(ml.default_title("ESP32_P4_PAPP"), "ESP32 P4 PAPP")
        self.assertEqual(ml.store_match("doom", self.store)["name"], "psram_doom")
        self.assertIsNone(ml.store_match("azip", self.store))

    def test_store_app_gets_its_listing_but_this_files_size_and_hash(self):
        papp = self.folder / "doom.papp"
        papp.write_bytes(b"P" * 50)
        listing, files = ml.build_listing(papp, self.store, "http://lan:8000/", False, False)
        self.assertEqual((listing["name"], listing["title"], listing["version"]), ("doom", "Doom", ""))
        self.assertEqual((listing["size"], listing["sha256"]), (50, sha(b"P" * 50)))
        self.assertEqual(listing["url"], "http://lan:8000/doom.papp")
        self.assertEqual((listing["controls"], listing["upstream"]["project"]), (["A: fire"], "PrBoom"))
        self.assertNotIn("changelog", listing)
        self.assertNotIn("source", listing)
        self.assertNotIn("url", listing["icon"])
        self.assertEqual(listing["data_size"], len(WAD))
        self.assertIn(f"{len(WAD)} {sha(WAD)} roms/doom/doom1.wad {PAGES}data/psram_doom/roms/doom/doom1.wad", files)
        self.assertTrue(files.startswith("# papp-data 1\n# doom: shareware\n"))

    def test_other_apps_get_a_basic_listing_and_their_own_icon(self):
        papp = self.folder / "azip-1.2.papp"
        papp.write_bytes(b"Z" * 10)
        (self.folder / "azip-1.2.png").write_bytes(png(64, 64))
        listing, files = ml.build_listing(papp, self.store, "", False, False)
        self.assertIsNone(files)
        self.assertEqual((listing["name"], listing["title"], listing["version"], listing["data_size"]),
                         ("azip", "Azip", "1.2", 0))
        self.assertEqual(listing["icon"]["width"], 64)
        self.assertEqual(base64.b64decode(listing["icon"]["base64"]), png(64, 64))
        self.assertNotIn("url", listing)
        (self.folder / "azip-1.2.png").write_bytes(png(512, 64))  # launcher art, too big: skipped
        self.assertNotIn("icon", ml.build_listing(papp, self.store, "", False, False)[0])
        icons = self.folder / "icons"
        icons.mkdir()
        (icons / "azip.png").write_bytes(png(32, 32))
        self.assertEqual(ml.build_listing(papp, self.store, "", False, False, icons=icons)[0]["icon"]["width"], 32)

    def test_hand_edits_survive_a_rerun_unless_forced(self):
        papp = self.folder / "azip.papp"
        papp.write_bytes(b"Z" * 10)
        (self.folder / "azip.json").write_text(json.dumps({"title": "A-Zip", "about": "Mine.", "size": 1}))
        listing, _ = ml.build_listing(papp, self.store, "", False, False)
        self.assertEqual((listing["title"], listing["about"], listing["size"]), ("A-Zip", "Mine.", 10))
        listing, _ = ml.build_listing(papp, self.store, "", False, True)
        self.assertEqual(listing["title"], "Azip")
        self.assertNotIn("about", listing)

    def test_mirrored_data_is_downloaded_once_and_listed_on_the_lan(self):
        papp = self.folder / "doom.papp"
        papp.write_bytes(b"P")
        opener = FakeOpener({PAGES + "data/psram_doom/roms/doom/doom1.wad": WAD})
        _, files = ml.build_listing(papp, self.store, "http://lan:8000/", True, False, opener)
        self.assertEqual((self.folder / "data/doom/roms/doom/doom1.wad").read_bytes(), WAD)
        self.assertIn("http://lan:8000/data/doom/roms/doom/doom1.wad", files)
        ml.build_listing(papp, self.store, "http://lan:8000/", True, False, opener)
        self.assertEqual(len(opener.calls), 1)

    def test_store_from_a_local_file(self):
        (self.folder / "store.json").write_text(json.dumps(STORE))
        self.assertIn("psram_doom", ml.load_store(str(self.folder)))
        self.assertEqual(ml.load_store(""), {})


if __name__ == "__main__":
    unittest.main()
