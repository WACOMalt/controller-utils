import hashlib
import json
import runpy
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
UPDATER = runpy.run_path(str(ROOT / "packaging/frame_update.py"))


class FrameUpdateTest(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.work = Path(work.name)
        self.root = self.work / "app"
        self.data = self.work / "app-data"
        self.feed = self.work / "feed"
        self.data.mkdir()
        self.feed.mkdir()
        assets = self.root / "assets"
        assets.mkdir(parents=True)
        self.key = self.work / "key.pem"
        subprocess.run(
            ["openssl", "genpkey", "-algorithm", "Ed25519", "-out", self.key], check=True
        )
        subprocess.run(
            ["openssl", "pkey", "-in", self.key, "-pubout", "-out", assets / "update-public.pem"],
            check=True,
        )
        self.marker = self.data / "installed"
        self.release = {"version": "0.3.0", "sequence": 15, "platform": "linuxarm64"}
        self.current = {"version": "0.2.0", "sequence": 14, "platform": "linuxarm64"}
        self.data.joinpath("calibration").write_text("preserve this")

    def sign(self, channel):
        filename = UPDATER["artifact"](channel, self.release["version"])
        package = self.work / filename.removesuffix("-linuxarm64.tar.gz")
        (package / "app").mkdir(parents=True)
        (package / "app/release.json").write_text(json.dumps(self.release))
        (package / "install.py").write_text(
            "import sys\nfrom pathlib import Path\n"
            "assert sys.argv[1:] == ['--unattended']\n"
            f"Path({str(self.marker)!r}).touch()\n"
        )
        archive = self.feed / filename
        with tarfile.open(archive, "w:gz") as contents:
            contents.add(package, arcname=package.name)
        metadata, signature = (self.feed / name for name in UPDATER["paths"](channel))
        metadata.write_text(
            json.dumps(
                dict(
                    schema=1,
                    channel=channel,
                    **self.release,
                    artifact=filename,
                    size=archive.stat().st_size,
                    sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                )
            )
        )
        subprocess.run(
            [
                "openssl",
                "pkeyutl",
                "-sign",
                "-inkey",
                self.key,
                "-rawin",
                "-in",
                metadata,
                "-out",
                signature,
            ],
            check=True,
        )
        return archive

    def test_signed_update_stages_and_applies_for_its_channel(self):
        self.sign("beta")
        self.sign("stable")

        def download(url, path, maximum):
            if "api.github.com" in url:
                path.write_text(
                    json.dumps(
                        [
                            {
                                "tag_name": "v0.4.0",
                                "draft": False,
                                "assets": [{"name": name} for name in UPDATER["paths"]("stable")],
                            },
                            {
                                "tag_name": "v0.3.0",
                                "draft": False,
                                "assets": [{"name": name} for name in UPDATER["paths"]("beta")],
                            },
                        ]
                    )
                )
            else:
                self.assertIn("/download/v0.3.0/", url)
                contents = (self.feed / path.name).read_bytes()
                self.assertLessEqual(len(contents), maximum)
                path.write_bytes(contents)

        with patch.dict(UPDATER["check"].__globals__, download=download):
            UPDATER["check"](self.root, self.data, "beta", self.current)
        self.assertFalse(self.marker.exists())
        self.assertEqual(
            json.loads((self.data / "frame-update.json").read_text())["state"], "ready"
        )
        UPDATER["apply"](self.root, self.data, "beta", self.current)
        self.assertTrue(self.marker.exists())
        self.assertEqual((self.data / "calibration").read_text(), "preserve this")

    def test_signature_and_archive_are_checked_before_install(self):
        archive = self.sign("stable")
        metadata = self.feed / UPDATER["paths"]("stable")[0]
        value = UPDATER["manifest"](self.feed, self.root / "assets/update-public.pem", "stable")
        original = metadata.read_bytes()
        metadata.write_bytes(original + b" ")
        with self.assertRaises(subprocess.CalledProcessError):
            UPDATER["manifest"](self.feed, self.root / "assets/update-public.pem", "stable")
        archive.write_bytes(b"x" * value["size"])
        with self.assertRaisesRegex(ValueError, "checksum"):
            UPDATER["verify_archive"](archive, value)
        self.assertFalse(self.marker.exists())

    def test_archive_cannot_write_outside_its_directory(self):
        archive = self.work / "invalid.tar.gz"
        with tarfile.open(archive, "w:gz") as contents:
            contents.add(self.key, arcname="../escape")
        with self.assertRaisesRegex(ValueError, "contents"):
            UPDATER["extract"](archive, self.feed)
        self.assertFalse((self.work / "escape").exists())
