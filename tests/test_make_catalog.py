"""Tests for tools/make_catalog.py: catalog page, store.json and app data lists.

Run: python3 -m unittest discover -s tests
"""

import hashlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
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


if __name__ == "__main__":
    unittest.main()
