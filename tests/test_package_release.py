import copy
import errno
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import package_release as packager
from test_native_update_client import DRIVER_FILES, RUNTIME_FILES

ROOT = Path(__file__).resolve().parents[1]


class NativeReleaseTest(unittest.TestCase):
    def setUp(self):
        temporary = TemporaryDirectory(prefix="package-test-", dir=ROOT / "build")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.stage, self.feed = self.base / "stage", self.base / "feed"
        (self.stage / "assets").mkdir(parents=True)
        for name in packager.ASSETS:
            (self.stage / "assets" / name).write_bytes(b"public fixture")
        (self.stage / "third_party/openvr").mkdir(parents=True)
        (self.stage / "third_party/openvr/LICENSE").write_text("OpenVR test notice")
        (self.stage / "LICENSE").write_text("MIT test notice")
        self.version = {"version": "0.1.0", "sequence": 1, "platform": "linuxarm64"}
        (self.stage / "release.json").write_text(json.dumps(self.version))
        header = struct.pack(
            "<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01\x01", 3, 183, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0
        )
        (self.stage / "controller-utils").write_bytes(header + bytes(56))
        for name in packager.PACKAGE_FILES:
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(
                header + bytes(56) if name == "controller-utils-launcher" else b"package fixture"
            )
        self.key = self.base / "signing.pem"
        subprocess.run(
            ["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(self.key)],
            check=True,
            capture_output=True,
            timeout=5,
        )
        self.key.chmod(0o600)
        self.requirements = {
            "backend": "native-tracker",
            "driver": dict.fromkeys(DRIVER_FILES, "b" * 64),
            "runtime": dict.fromkeys(RUNTIME_FILES, "b" * 64),
            "helper": dict.fromkeys(
                [
                    "controller-utils-helper",
                    "controller-utils-updater",
                    *[
                        "native-tracker/" + name
                        for name in (
                            "adapter.json",
                            "loader.bin",
                            "libnative_tracker.so",
                            "manifest.json",
                        )
                    ],
                ],
                "b" * 64,
            ),
        }

        (self.stage / "assets/update-public.pem").write_bytes(
            subprocess.check_output(["openssl", "pkey", "-in", str(self.key), "-pubout"])
        )
        for field, folder in (("driver", "native-driver"), ("helper", "compatibility")):
            for name in self.requirements[field]:
                path = self.stage / folder / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(header + bytes(56))
                self.requirements[field][name] = hashlib.sha256(path.read_bytes()).hexdigest()

    def build(self, **kwargs):
        values = {
            "root": self.stage,
            "requirements": self.requirements,
            "key": self.key,
            "output": self.feed,
            "notes": "Native release",
            **kwargs,
        }
        return packager.build(**values)

    def check(self, public, destination):
        return subprocess.run(
            [str(ROOT / "build/release_check"), str(self.feed), str(public), str(destination)],
            capture_output=True,
            timeout=5,
        )

    def test_signed_native_package_and_tamper_rejection(self):
        (self.stage / "assets/tracking-calibration.json").write_text("private calibration")
        (self.stage / "tools").mkdir()
        (self.stage / "tools/unlisted-script.py").write_text("unlisted development tool")
        shutil.copyfile(self.key, self.stage / "private.pem")
        result = self.build()
        metadata = json.loads((self.feed / "latest.json").read_text())
        self.assertEqual(metadata["schema"], 5)
        self.assertEqual(metadata["requirements"], self.requirements)
        with tarfile.open(result["archive"]) as archive:
            names = archive.getnames()
            self.assertEqual(archive.extractfile("LICENSE").read(), b"MIT test notice")
            self.assertEqual(archive.getmember("controller-utils").mode, 0o755)
            self.assertFalse(any(name.startswith(("tools/", "build/")) for name in names))
            self.assertNotIn("assets/tracking-calibration.json", names)
            self.assertNotIn("private.pem", names)
            public = self.base / "public.pem"
            public.write_bytes(archive.extractfile("assets/update-public.pem").read())
        self.assertEqual(len(names), result["files"])
        self.assertEqual(self.check(public, self.base / "check").returncode, 0)
        manifest = self.feed / "latest.json"
        manifest.write_text(manifest.read_text() + " ")
        self.assertNotEqual(self.check(public, self.base / "bad").returncode, 0)
        self.assertFalse((self.base / "bad").exists())

    def test_invalid_cpu_requirements_and_input_files_never_publish(self):
        program = self.stage / "controller-utils"
        original = program.read_bytes()
        for data in (b"#!/bin/sh\nexit 0\n", original[:18] + b"\x3e\x00" + original[20:]):
            with self.subTest(program=data[:20]):
                program.write_bytes(data)
                with self.assertRaises(subprocess.CalledProcessError):
                    self.build()
                self.assertEqual(list(self.feed.iterdir()), [])
        program.write_bytes(original)
        requirements = copy.deepcopy(self.requirements)
        requirements["helper"]["unexpected-helper"] = "b" * 64
        with self.assertRaises(ValueError):
            self.build(requirements=requirements)
        self.assertEqual(list(self.feed.iterdir()), [])
        source = self.stage / "assets/controller.stl"
        source.unlink()
        source.symlink_to(self.key)
        with self.assertRaises(ValueError):
            self.build()

    def test_invalid_release_strings_cannot_escape_the_output(self):
        for version in ("../../escape", "0.2", "0.1.0-pre", "1.2.9999999"):
            (self.stage / "release.json").write_text(
                json.dumps({**self.version, "version": version})
            )
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.build()
            self.assertFalse(self.feed.exists())

    def test_signing_key_boundaries(self):
        self.key.chmod(0o644)
        with self.assertRaises(ValueError):
            self.build()
        self.key.chmod(0o600)
        alias = self.base / "alias.pem"
        alias.symlink_to(self.key)
        with self.assertRaises(ValueError):
            self.build(key=alias)
        alias.unlink()
        os.link(self.key, alias)
        with self.assertRaises(ValueError):
            self.build()
        self.assertFalse(self.feed.exists())

    def test_published_files_are_preserved_and_failed_copies_are_removed(self):
        original_open = Path.open

        def fail(path, *args, **kwargs):
            if path == self.feed / "latest.json" and args == ("xb",):
                raise OSError(errno.ENOSPC, "injected full disk")
            return original_open(path, *args, **kwargs)

        with patch.object(Path, "open", fail), self.assertRaises(OSError):
            self.build()
        self.assertEqual(list(self.feed.iterdir()), [])
        self.build()
        before = {path.name: path.read_bytes() for path in self.feed.iterdir()}
        with self.assertRaises(ValueError):
            self.build()
        self.assertEqual({path.name: path.read_bytes() for path in self.feed.iterdir()}, before)
