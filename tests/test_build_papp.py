"""Tests for tools/build_papp.py: manifests, custom recipes and the PAPP header.

Run: python3 -m unittest discover -s tests
"""

import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import build_papp as bp  # noqa: E402

APPS = Path(__file__).resolve().parent.parent / "apps"


class ManifestTests(unittest.TestCase):
    def test_every_manifest_is_valid(self):
        manifests = sorted(APPS.glob("*/papp.json"))
        self.assertTrue(manifests)
        for path in manifests:
            with self.subTest(app=path.parent.name):
                m = json.loads(path.read_text())
                self.assertEqual(m["name"], path.parent.name)
                self.assertIn(m["build"], ("lvgl", "plain", "custom"))
                self.assertRegex(m["source"]["ref"], r"^[0-9a-f]{40}$")
                self.assertRegex(m["version"], r"^\d+\.\d+\.\d+$")
                if m["build"] == "lvgl":
                    self.assertRegex(m["lvgl"]["ref"], r"^[0-9a-f]{40}$")
                if m["build"] == "custom":
                    self.assertTrue(m["groups"])
                bp.check_data(m["name"], m.get("data"))
                bp.check_submodules(m["name"], m)
                bp.store_info(m["name"], m, path.parent)

    def test_store_listing_is_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / "psram_x"
            folder.mkdir()

            def png(w, h, pad=0):
                return b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + struct.pack(">II", w, h) + b"\x00" * (5 + pad)

            (folder / "icon.png").write_bytes(png(96, 96))
            (folder / "shot.png").write_bytes(png(1024, 600))
            info = bp.store_info("psram_x", {"author": " giltal ", "controls": ["A: fire"], "icon": "icon.png",
                                             "license": "MIT", "screenshots": ["shot.png"],
                                             "upstream": {"project": "PrBoom", "version": "2.5.0",
                                                          "url": "https://example.com/prboom"}}, folder)
            self.assertEqual((info["license"], info["screenshots"][0]["width"]), ("MIT", 1024))
            self.assertEqual((info["author"], info["controls"]), ("giltal", ["A: fire"]))
            self.assertEqual(info["upstream"], {"project": "PrBoom", "version": "2.5.0", "url": "https://example.com/prboom"})
            self.assertEqual((info["icon"]["width"], info["icon"]["height"], info["icon"]["type"]), (96, 96, "image/png"))
            self.assertEqual(bp.store_info("psram_x", {}, folder), {})
            (folder / "big.png").write_bytes(png(512, 64))
            (folder / "wide.png").write_bytes(png(1280, 600))
            (folder / "heavy.png").write_bytes(png(64, 64, pad=bp.MAX_ICON_BYTES))
            (folder / "fake.png").write_bytes(b"GIF89a" + b"\x00" * 30)
            (Path(tmp) / "outside.png").write_bytes(png(8, 8))
            bad = [{"icon": "big.png"}, {"icon": "heavy.png"}, {"icon": "fake.png"}, {"icon": "../outside.png"},
                   {"icon": "missing.png"}, {"author": ""}, {"about": "x" * 2001}, {"controls": []},
                   {"controls": ["x" * 61]}, {"controls": "A: fire"},
                   {"screenshots": ["wide.png"]}, {"screenshots": []}, {"screenshots": ["shot.png"] * 4},
                   {"changelog": "x" * 4001}, {"upstream": "PrBoom"}, {"upstream": {"version": "1"}},
                   {"upstream": {"project": "Q", "url": "http://x"}}, {"upstream": {"project": "Q", "tag": "v1"}}]
            for manifest in bad:
                with self.subTest(manifest=str(manifest)[:40]), self.assertRaises(ValueError):
                    bp.store_info("psram_x", manifest, folder)

    def test_data_blocks_are_checked(self):
        good = {"repo": "https://github.com/o/r", "ref": "a" * 40, "license": "shareware",
                "files": [{"path": "SDcard/roms/doom/doom1.wad", "target": "roms/doom/doom1.wad",
                           "size": 4, "sha256": "b" * 64}]}
        self.assertEqual(bp.check_data("x", good)["files"][0]["target"], "roms/doom/doom1.wad")
        self.assertIsNone(bp.check_data("x", None))
        bad_files = [
            {"target": "../etc/passwd"}, {"target": "/abs/path"}, {"target": "roms/a b.wad"},
            {"target": "roms\\doom.wad"}, {"size": 0}, {"size": "4"}, {"sha256": "xyz"}, {"path": "../x"},
        ]
        for change in bad_files:
            with self.subTest(change=change), self.assertRaises(ValueError):
                bp.check_data("x", {**good, "files": [{**good["files"][0], **change}]})
        for change in ({"ref": "main"}, {"repo": "http://example.com/r"}, {"license": ""}, {"files": []},
                       {"files": good["files"] * 2}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                bp.check_data("x", {**good, **change})


class CustomRecipeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "src"
        for rel in ["engine/a.c", "engine/b.c", "app/main.c", "app/mp3.cpp", "app/inc/x.h"]:
            (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.root / rel).write_text("")
        self.build = Path(self.tmp.name) / "build"

    def tearDown(self):
        self.tmp.cleanup()

    def manifest(self, **extra):
        m = {
            "includes": ["app/inc"],
            "cflags": ["-std=gnu99"],
            "cxxflags": ["-fno-rtti"],
            "groups": [
                {"dir": "engine", "files": ["a.c", "b.c"]},
                {"dir": "app", "files": ["main.c", "mp3.cpp"], "prefix": "app_", "includes": ["app"]},
            ],
            "ldflags": ["-Wl,--allow-multiple-definition"],
            "newlib": True,
        }
        m.update(extra)
        return m

    def test_units_use_the_right_compiler_and_flags(self):
        units, ldflags = bp.custom_units(self.manifest(), self.root, self.build)
        by_name = {u.obj.name: u for u in units}
        self.assertEqual(sorted(by_name), ["a.o", "app_main.o", "app_mp3.o", "b.o"])
        self.assertEqual(by_name["app_mp3.o"].compiler, bp.CXX)
        self.assertIn("-fno-rtti", by_name["app_mp3.o"].flags)
        self.assertNotIn("-std=gnu99", by_name["app_mp3.o"].flags)
        self.assertEqual(by_name["a.o"].compiler, bp.CC)
        self.assertIn("-std=gnu99", by_name["a.o"].flags)
        self.assertIn("-mcmodel=medany", by_name["a.o"].flags)
        self.assertIn(f"-I{(self.root / 'app/inc').resolve()}", by_name["a.o"].flags)
        self.assertIn(f"-I{(self.root / 'app').resolve()}", by_name["app_main.o"].flags)
        self.assertNotIn(f"-I{(self.root / 'app').resolve()}", by_name["a.o"].flags)
        self.assertEqual(ldflags[0], "-Wl,--allow-multiple-definition")
        self.assertIn("-Wl,--wrap=malloc", ldflags)
        self.assertIn("-Wl,--wrap=__retarget_lock_release_recursive", ldflags)
        self.assertEqual(ldflags[-3:], ["-lc", "-lgcc", "-lm"])

    def test_without_newlib_nothing_is_wrapped(self):
        _, ldflags = bp.custom_units(self.manifest(newlib=False, ldflags=[]), self.root, self.build)
        self.assertEqual(ldflags, [])

    def test_paths_must_stay_inside_the_checkout(self):
        for bad in ({"groups": [{"dir": "..", "files": ["etc.c"]}]},
                    {"groups": [{"dir": "engine", "files": ["../../x.c"]}]},
                    {"includes": ["/usr/include"]}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                bp.custom_units(self.manifest(**bad), self.root, self.build)

    def test_object_name_collisions_are_refused(self):
        groups = [{"dir": "engine", "files": ["a.c"]}, {"dir": "engine", "files": ["a.c"]}]
        with self.assertRaises(ValueError):
            bp.custom_units(self.manifest(groups=groups), self.root, self.build)

    def test_only_c_and_cpp_sources(self):
        with self.assertRaises(ValueError):
            bp.custom_units(self.manifest(groups=[{"dir": "app/inc", "files": ["x.h"]}]), self.root, self.build)


class PortRecipeTests(unittest.TestCase):
    """Globs, local: paths, patches and packing, used by ports such as Red Alert."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "src"
        for rel in ["common/a.cpp", "common/b.cpp", "common/win.cpp", "common/x.h"]:
            (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.root / rel).write_text("int x;\n")

    def tearDown(self):
        self.tmp.cleanup()

    def test_globs_expand_and_exclude(self):
        group = {"dir": "common", "files": ["*.cpp", "a.cpp"], "exclude": ["win.cpp"]}
        self.assertEqual(bp.group_files(self.root, group), ["a.cpp", "b.cpp"])
        with self.assertRaises(ValueError):
            bp.group_files(self.root, {"dir": "common", "files": ["*.cpp"], "exclude": ["missing.cpp"]})

    def test_recursive_globs_keep_subfolders_in_object_names(self):
        for rel in ["lib/core/obj.c", "lib/draw/obj.c", "lib/top.c", "lib/notes.txt"]:
            (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.root / rel).write_text("")
        group = {"dir": "lib", "files": ["**/*.c"], "exclude": ["top.c"], "prefix": "lv_"}
        self.assertEqual(bp.group_files(self.root, group), ["core/obj.c", "draw/obj.c"])
        units, _ = bp.custom_units({"groups": [group]}, self.root, Path(self.tmp.name) / "build")
        self.assertEqual(sorted(u.obj.name for u in units), ["lv_core_obj.o", "lv_draw_obj.o"])

    def test_local_paths_resolve_inside_this_repository(self):
        self.assertEqual(bp.source_file(self.root, "local:tools/build_papp.py"), (bp.ROOT / "tools/build_papp.py").resolve())
        with self.assertRaises(ValueError):
            bp.source_file(self.root, "local:../outside")
        with self.assertRaises(ValueError):
            bp.source_file(self.root, "../outside")

    def test_pack_matches_the_upstream_format(self):
        packed = bp.pack_papp(b"\x01\x02\x03\x04", 64)
        self.assertEqual(bp.parse_header(packed)["bss_size"], 64)
        self.assertEqual(bp.parse_header(packed)["text_size"], 4)
        self.assertEqual(packed[:8], struct.pack("<II", bp.PAPP_MAGIC, 1))
        self.assertEqual(len(packed), 36)

    def test_patches_apply_to_a_clean_checkout_every_time(self):
        import subprocess
        repo = Path(self.tmp.name) / "up"
        repo.mkdir()
        run = lambda *a: subprocess.run(["git", *a], cwd=repo, check=True, capture_output=True)  # noqa: E731
        run("init", "-q")
        run("-c", "user.email=t@t", "-c", "user.name=t", "commit", "-q", "--allow-empty", "-m", "base")
        (repo / "f.txt").write_text("one\n")
        run("add", "f.txt")
        run("-c", "user.email=t@t", "-c", "user.name=t", "commit", "-q", "-m", "f")
        local = Path(self.tmp.name) / "repo"
        (local / "ports/demo/patches").mkdir(parents=True)
        (local / "ports/demo/patches/0001-two.patch").write_text(
            "--- a/f.txt\n+++ b/f.txt\n@@ -1 +1 @@\n-one\n+two\n")
        pattern = "local:ports/demo/patches/*.patch"
        saved, bp.ROOT = bp.ROOT, local
        try:
            self.assertEqual(bp.apply_patches(repo, [pattern]), ["0001-two.patch"])
            self.assertEqual((repo / "f.txt").read_text(), "two\n")
            # A second build starts from the pinned files again instead of failing.
            self.assertEqual(bp.apply_patches(repo, [pattern]), ["0001-two.patch"])
            self.assertEqual((repo / "f.txt").read_text(), "two\n")
            with self.assertRaises(ValueError):
                bp.apply_patches(repo, ["local:ports/demo/patches/none-*.patch"])
        finally:
            bp.ROOT = saved


def git(cwd, *args):
    import subprocess
    return subprocess.run(["git", "-c", "user.email=t@t", "-c", "user.name=t", "-c", "protocol.file.allow=always",
                           *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


class SubmoduleAndPrebuildTests(unittest.TestCase):
    """Extra pinned repositories inside the source tree, and code generators run before compiling."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def make_repo(self, name, files):
        repo = self.base / name
        repo.mkdir()
        git(repo, "init", "-q")
        git(repo, "config", "uploadpack.allowFilter", "true")
        git(repo, "config", "uploadpack.allowAnySHA1InWant", "true")
        for rel, text in files.items():
            (repo / rel).parent.mkdir(parents=True, exist_ok=True)
            (repo / rel).write_text(text)
        git(repo, "add", ".")
        git(repo, "commit", "-q", "-m", "files")
        return repo, git(repo, "rev-parse", "HEAD")

    def test_submodules_are_validated(self):
        good = {"path": "lib/mp", "repo": "https://github.com/o/r", "ref": "a" * 40}
        self.assertEqual(bp.check_submodules("x", {"submodules": [good]}), [good])
        self.assertEqual(bp.check_submodules("x", {}), [])
        for bad in ({**good, "ref": "main"}, {**good, "path": "../up"}, {**good, "path": "/abs"},
                    {**good, "repo": "git@github.com:o/r"}, {**good, "path": "a/./b"}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                bp.check_submodules("x", {"submodules": [bad]})
        with self.assertRaises(ValueError):
            bp.check_submodules("x", {"submodules": [good, {**good, "path": "lib/mp/inner"}]})

    def test_paths_are_shared_out_between_the_checkout_and_its_submodules(self):
        subs = [{"path": "micropython"}, {"path": "amy"}, {"path": "lib/whole"}]
        main, per_sub = bp.split_sparse(["tulip/shared", "micropython", "micropython/py", "micropython/extmod",
                                         "amy/src", "lib/whole"], subs)
        self.assertEqual(main, ["tulip/shared"])
        self.assertEqual(per_sub, {"micropython": ["/py", "/extmod"], "amy": ["/src"], "lib/whole": None})
        paths = bp.manifest_paths({"paths": ["tulip/fs"], "includes": ["local:x", ".", "a/inc"],
                                   "groups": [{"dir": "a", "files": [], "includes": ["b"]}]})
        self.assertEqual(paths, ["tulip/fs", "a/inc", "a", "b"])

    def test_submodules_are_fetched_at_the_pinned_commit(self):
        sub, sub_ref = self.make_repo("sub", {"py/a.c": "int a;\n", "docs/big.txt": "x\n"})
        top, top_ref = self.make_repo("top", {"main.c": "int m;\n"})
        # Record the submodule in the upstream tree, as `git submodule add` would.
        git(top, "update-index", "--add", "--cacheinfo", f"160000,{sub_ref},mp")
        git(top, "commit", "-q", "-m", "gitlink")
        top_ref = git(top, "rev-parse", "HEAD")
        src = bp.fetch(top.as_uri(), top_ref, self.base / "src", None)
        entry = {"path": "mp", "repo": sub.as_uri(), "ref": sub_ref}
        bp.fetch_submodules(src, [entry], {"mp": ["/py"]})
        self.assertEqual((src / "mp/py/a.c").read_text(), "int a;\n")
        self.assertFalse((src / "mp/docs/big.txt").exists())  # sparse: only what the recipe uses
        with self.assertRaises(ValueError):  # upstream pins a different commit
            bp.fetch_submodules(src, [{**entry, "ref": "b" * 40}], {"mp": None})
        # Patches may touch submodule files; the next build starts from the pinned files again.
        (src / "mp/py/a.c").write_text("changed\n")
        (src / bp.GEN_DIR).mkdir()
        self.assertEqual(bp.apply_patches(src, [], ["mp"]), [])
        self.assertEqual((src / "mp/py/a.c").read_text(), "int a;\n")
        self.assertTrue((src / ".papp-ref").exists())  # cache stamp survives the reset
        self.assertFalse((src / bp.GEN_DIR).exists())

    def test_prebuild_runs_generators_with_the_compile_units(self):
        src = self.base / "src"
        (src / "app").mkdir(parents=True)
        (src / "app/main.c").write_text("")
        script = self.base / "gen.py"
        script.write_text(
            "import json, sys, pathlib\n"
            "units = json.load(open(sys.argv[2]))\n"
            "gen = pathlib.Path(sys.argv[1])\n"
            "(gen / 'made.c').write_text('/* ' + ' '.join(u['obj'] for u in units) + ' */\\n')\n")
        manifest = {"cflags": ["-DX=1"], "groups": [{"dir": "app", "files": ["main.c"]},
                                                     {"dir": bp.GEN_DIR, "files": ["made.c"], "prefix": "gen_"}],
                    "prebuild": [["{python}", str(script), "{gen}", "{units}"]]}
        (src / bp.GEN_DIR).mkdir()
        (src / bp.GEN_DIR / "stale.c").write_text("")
        bp.run_prebuild(manifest, src, self.base / "build", self.base, dict(os.environ))
        self.assertEqual((src / bp.GEN_DIR / "made.c").read_text(), "/* main.o gen_made.o */\n")
        self.assertFalse((src / bp.GEN_DIR / "stale.c").exists())
        units = json.loads((src / bp.GEN_DIR / "units.json").read_text())
        self.assertIn("-DX=1", units[0]["flags"])
        self.assertEqual(bp.prebuild_args(["-o{gen}/x", "{jobs}"], {"gen": "/g", "jobs": "4"}), ["-o/g/x", "4"])
        with self.assertRaises(ValueError):
            bp.run_prebuild({**manifest, "prebuild": ["echo hi"]}, src, self.base / "build", self.base, {})


class HeaderTests(unittest.TestCase):
    def test_parse_header(self):
        body = b"\x00" * 24
        data = bp.PAPP_HEADER.pack(bp.PAPP_MAGIC, 1, 0, 20, 4, 100, 0, 0) + body
        self.assertEqual(bp.parse_header(data)["bss_size"], 100)
        with self.assertRaises(ValueError):
            bp.parse_header(data[:-1])
        with self.assertRaises(ValueError):
            bp.parse_header(struct.pack("<I", 0) + data[4:])


if __name__ == "__main__":
    unittest.main()
