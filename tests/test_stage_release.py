import json
import os
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import stage_release as stage


class StageReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temporary = TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "source"
        self.root.mkdir()
        self.override = patch.object(stage, "ROOT", self.root)
        self.override.start()
        self.addCleanup(self.override.stop)

    def fixture(self):
        names = {
            "build/controller-utils",
            "build/controller-utils-launcher",
            "release.json",
            "LICENSE",
            "third_party/openvr/LICENSE",
            "assets/update-public.pem",
            "driver/runtime.json",
            *stage.PACKAGE_FILES,
            *("assets/" + name for name in stage.ASSETS),
            *("build/native-driver/" + name for name in stage.DRIVER_FILES),
            *("build/" + name for name in stage.HELPER_FILES),
            "packaging/install.sh",
            "packaging/uninstall.sh",
        }
        for name in names:
            file = self.root / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(b"regular source")
        (self.root / "driver/runtime.json").write_text(json.dumps({"backend": "native-tracker"}))

    def test_staging_regular_sources_preserves_checksums(self):
        self.fixture()
        output = self.base / "stage"
        requirements = stage.stage(output)
        self.assertEqual((output / "controller-utils").read_bytes(), b"regular source")
        self.assertEqual(len(requirements["driver"]), len(stage.DRIVER_FILES))
        self.assertEqual(len(requirements["helper"]), len(stage.HELPER_FILES))

    def test_allowlisted_file_symlink_is_rejected(self):
        self.fixture()
        file = self.root / "build/controller-utils"
        outside = self.base / "outside"
        outside.write_bytes(b"external contents")
        file.unlink()
        file.symlink_to(outside)
        with self.assertRaises(OSError):
            stage.stage(self.base / "stage")
        self.assertEqual(outside.read_bytes(), b"external contents")

    def test_allowlisted_ancestor_symlink_is_rejected(self):
        self.fixture()
        (self.root / "build").rename(self.base / "outside-build")
        (self.root / "build").symlink_to(self.base / "outside-build", target_is_directory=True)
        with self.assertRaises(OSError):
            stage.stage(self.base / "stage")

    def test_non_regular_oversized_and_parent_escape_are_rejected(self):
        source = self.root / "input"
        source.write_bytes(b"12345")
        with self.assertRaises(ValueError):
            stage.read_source(source, 4)
        source.unlink()
        os.mkfifo(source)
        with self.assertRaises(ValueError):
            stage.read_source(source)
        with self.assertRaises(ValueError):
            stage.read_source(self.root / "../escape")
        with self.assertRaises(ValueError):
            stage.read_source(self.base / "outside")
