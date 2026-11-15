import hashlib
import importlib.util
import io
import json
import subprocess
import tarfile
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("bootstrap", ROOT / "packaging/bootstrap.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class BootstrapTest(unittest.TestCase):
    def setUp(self):
        self.temporary = TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.feed = self.root / "feed"
        self.feed.mkdir()
        self.key = self.root / "test-key.pem"
        subprocess.run(
            ["openssl", "genpkey", "-algorithm", "Ed25519", "-out", str(self.key)],
            check=True,
            capture_output=True,
        )
        self.public = subprocess.check_output(
            ["openssl", "pkey", "-in", str(self.key), "-pubout"],
            text=True,
        )
        self.artifact = self.feed / "controller-utils-0.1.0-5-linuxarm64.tar.gz"
        self.make_archive()

    def sign(self, contents):
        (self.feed / "latest.json").write_bytes(contents)
        subprocess.run(
            [
                "openssl",
                "pkeyutl",
                "-sign",
                "-rawin",
                "-inkey",
                str(self.key),
                "-in",
                str(self.feed / "latest.json"),
                "-out",
                str(self.feed / "latest.sig"),
            ],
            check=True,
            capture_output=True,
        )

    def make_archive(self, name="install.sh", link=False):
        with tarfile.open(self.artifact, "w:gz") as archive:
            data = b"#!/bin/sh\nexit 0\n"
            member = tarfile.TarInfo(name)
            member.mode = 0o755
            member.size = len(data)
            if link:
                member.type = tarfile.SYMTYPE
                member.linkname = "../escape"
                member.size = 0
            archive.addfile(member, None if link else io.BytesIO(data))
        self.metadata = {
            "schema": 5,
            "platform": "linuxarm64",
            "expires": 0,
            "artifact": self.artifact.name,
            "size": self.artifact.stat().st_size,
            "sha256": hashlib.sha256(self.artifact.read_bytes()).hexdigest(),
        }
        self.sign(json.dumps(self.metadata).encode())

    def verify(self):
        return bootstrap.verify_package(self.feed, self.root / "verified", self.public)

    def test_authenticated_extraction_and_independent_key(self):
        self.assertEqual(bootstrap.PUBLIC_KEY, (ROOT / "assets/update-public.pem").read_text())
        path = self.verify()
        self.assertEqual((path / "install.sh").read_bytes(), b"#!/bin/sh\nexit 0\n")
        self.assertTrue((path / "install.sh").stat().st_mode & 0o100)

    def test_substituted_archive_is_not_extracted(self):
        self.artifact.write_bytes(self.artifact.read_bytes() + b"substitution")
        with self.assertRaises(ValueError):
            self.verify()
        self.assertFalse((self.root / "verified/installer").exists())

    def test_metadata_tampering_and_wrong_signature_are_rejected(self):
        (self.feed / "latest.sig").write_bytes(b"x" * 64)
        with self.assertRaises(subprocess.CalledProcessError):
            self.verify()
        self.assertFalse((self.root / "verified/installer").exists())

    def test_duplicate_signed_fields_are_rejected(self):
        self.sign(b'{"schema":5,' + json.dumps(self.metadata).encode()[1:])
        with self.assertRaises(ValueError):
            self.verify()

    def test_signed_paths_and_links_are_still_checked(self):
        for name, link in (("../escape", False), ("link", True)):
            with self.subTest(name=name):
                self.make_archive(name, link)
                with self.assertRaises(ValueError):
                    bootstrap.verify_package(
                        self.feed, self.root / ("bad-" + str(link)), self.public
                    )
        self.assertFalse((self.root / "escape").exists())
