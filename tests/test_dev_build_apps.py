"""Tests for tools/dev_build_apps.py: which apps a branch's dev build builds.

Run: python3 -m unittest discover -s tests
"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import dev_build_apps as dba  # noqa: E402

OPENLARA = {"publish": True, "dev_build": True,
            "patches": ["local:ports/openlara/patches/*.patch"],
            "linker_script": "local:ports/redalert/papp_cpp.ld"}
TULIP = {"publish": True, "dev_build": True, "patches": ["local:ports/tulip/patches/*.patch"]}
NETSURF = {"publish": False, "source": {"repo": "https://example.invalid/netsurf"}}
DOOM = {"publish": True}


class SelectTest(unittest.TestCase):
    apps = {"openlara": OPENLARA, "tulip": TULIP, "netsurf": NETSURF}

    def test_only_the_apps_a_branch_touches(self):
        self.assertEqual(dba.select(self.apps, ["ports/openlara/patches/0009-x.patch"]), ["openlara"])
        self.assertEqual(dba.select(self.apps, ["apps/tulip/papp.json", "docs/building.md"]), ["tulip"])

    def test_a_shared_port_folder_rebuilds_every_app_using_it(self):
        self.assertEqual(dba.select(self.apps, ["ports/redalert/papp_cpp.ld"]), ["openlara"])

    def test_build_changes_or_no_base_rebuild_all(self):
        everything = ["netsurf", "openlara", "tulip"]
        self.assertEqual(dba.select(self.apps, ["tools/build_papp.py"]), everything)
        self.assertEqual(dba.select(self.apps, None), everything)

    def test_nothing_for_unrelated_changes(self):
        self.assertEqual(dba.select(self.apps, ["esphome/components/usb_midi/usb_midi.cpp"]), [])
        self.assertEqual(dba.select(self.apps, []), [])

    def test_folder_prefixes_do_not_match_longer_names(self):
        self.assertEqual(dba.select({"tulip": TULIP}, ["ports/tulipx/a.c", "apps/tulip2/papp.json"]), [])


class WipAppsTest(unittest.TestCase):
    def test_publish_false_and_dev_build_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name, manifest in {"tulip": TULIP, "netsurf": NETSURF, "doom": DOOM}.items():
                (Path(tmp) / name).mkdir()
                (Path(tmp) / name / "papp.json").write_text(json.dumps(manifest), encoding="utf-8")
            self.assertEqual(sorted(dba.wip_apps(Path(tmp))), ["netsurf", "tulip"])


if __name__ == "__main__":
    unittest.main()
