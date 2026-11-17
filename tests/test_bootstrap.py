import hashlib
import importlib.util
import io
import json
import signal
import subprocess
import tarfile
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

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
            "version": "0.1.0",
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

    def download_release(self, tamper_signature=False, channel="beta"):
        calls = []

        def download(url, destination, limit):
            calls.append((url, limit))
            name = url.rsplit("/", 1)[1].replace("latest-stable", "latest")
            data = (self.feed / name).read_bytes()
            if tamper_signature and name == "latest.sig":
                data = b"x" * 64
            self.assertLessEqual(len(data), limit)
            destination.write_bytes(data)

        with (
            patch.object(bootstrap, "download", side_effect=download),
            patch.object(bootstrap, "steam_os_channel", return_value=channel),
        ):
            try:
                feed = bootstrap.download_release(self.root / "download", self.public)
            finally:
                self.downloads = calls
        return feed

    def test_latest_release_pins_signature_and_archive_to_its_version(self):
        feed = self.download_release()
        self.assertEqual(
            self.downloads,
            [
                (bootstrap.RELEASES + "latest/download/latest.json", 65536),
                (bootstrap.RELEASES + "download/v0.1.0/latest.sig", 64),
                (
                    bootstrap.RELEASES + "download/v0.1.0/" + self.artifact.name,
                    self.metadata["size"],
                ),
            ],
        )
        installer = bootstrap.verify_package(feed, self.root / "verified", self.public)
        self.assertTrue((installer / "install.sh").is_file())

    def test_stable_release_uses_stable_metadata_and_version_pinned_signature(self):
        feed = self.download_release(channel="stable")
        self.assertEqual(
            self.downloads,
            [
                (bootstrap.RELEASES + "latest/download/latest-stable.json", 65536),
                (bootstrap.RELEASES + "download/v0.1.0/latest-stable.sig", 64),
                (
                    bootstrap.RELEASES + "download/v0.1.0/" + self.artifact.name,
                    self.metadata["size"],
                ),
            ],
        )
        bootstrap.verify_package(feed, self.root / "verified-stable", self.public)

    def test_only_supported_frame_channels_are_accepted(self):
        for branch in ("stable", "beta", "preview", "main", "rc", "", "rel"):
            with self.subTest(branch=branch):
                with patch.object(
                    bootstrap.subprocess,
                    "run",
                    side_effect=[
                        subprocess.CompletedProcess([], 0, "vr\n"),
                        subprocess.CompletedProcess([], 0, branch + "\n"),
                    ],
                ) as run:
                    if branch in ("stable", "beta"):
                        self.assertEqual(bootstrap.steam_os_channel(), branch)
                    else:
                        with self.assertRaises(ValueError):
                            bootstrap.steam_os_channel()
                    self.assertEqual(
                        run.call_args.args[0], ["/usr/bin/atomupd-manager", "tracked-branch"]
                    )
                    self.assertEqual(run.call_args.kwargs["timeout"], 3)
        with patch.object(
            bootstrap.subprocess,
            "run",
            side_effect=[
                subprocess.CompletedProcess([], 0, "deck\n"),
                subprocess.CompletedProcess([], 0, "stable\n"),
            ],
        ):
            with self.assertRaises(ValueError):
                bootstrap.steam_os_channel()

    def test_channel_detection_failure_prevents_downloads(self):
        for error in (
            subprocess.TimeoutExpired("atomupd-manager", 3),
            subprocess.CalledProcessError(1, "atomupd-manager"),
            FileNotFoundError("atomupd-manager"),
        ):
            with (
                self.subTest(error=error),
                patch.object(bootstrap.subprocess, "run", side_effect=error),
                patch.object(bootstrap, "download") as download,
            ):
                with self.assertRaises(type(error)):
                    bootstrap.download_release(self.root / "undetected", self.public)
                download.assert_not_called()

    def test_invalid_signature_prevents_archive_download(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.download_release(tamper_signature=True)
        self.assertEqual(len(self.downloads), 2)
        self.assertFalse((self.root / "download" / self.artifact.name).exists())

    def test_invalid_online_versions_never_form_a_download_url(self):
        for index, version in enumerate(("../escape", "0.1.0/path", 1)):
            with self.subTest(version=version):
                metadata = {**self.metadata, "version": version}

                def download(url, destination, limit):
                    self.assertEqual(url, bootstrap.RELEASES + "latest/download/latest.json")
                    destination.write_text(json.dumps(metadata))

                with (
                    patch.object(bootstrap, "download", side_effect=download) as request,
                    patch.object(bootstrap, "steam_os_channel", return_value="beta"),
                ):
                    with self.assertRaises(ValueError):
                        bootstrap.download_release(
                            self.root / ("invalid-" + str(index)), self.public
                        )
                self.assertEqual(request.call_count, 1)

    def test_download_requires_https_and_enforces_the_size_limit(self):
        with patch.object(bootstrap.subprocess, "run") as run:
            bootstrap.download("https://example.test/release", self.root / "download", 64)
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("--proto") + 1], "=https")
        self.assertEqual(command[command.index("--proto-redir") + 1], "=https")
        self.assertEqual(command[command.index("--max-filesize") + 1], "64")
        self.assertTrue(run.call_args.kwargs["check"])

    def test_downloads_and_propagates_verified_installer_exit_status(self):
        installer = self.root / "verified-installer"
        for status in (0, 1, 23):
            with (
                self.subTest(status=status),
                patch("sys.argv", ["bootstrap.py"]),
                patch.object(bootstrap, "download_release", return_value=self.feed) as fetch,
                patch.object(bootstrap, "verify_package", return_value=installer) as verify,
                patch.object(
                    bootstrap.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], status),
                ) as run,
            ):
                self.assertEqual(bootstrap.main(), status)
                fetch.assert_called_once()
                self.assertEqual(verify.call_args.args[0], self.feed)
                self.assertEqual(
                    run.call_args.args[0][:2], ["/bin/sh", str(installer / "install.sh")]
                )
                self.assertFalse(run.call_args.kwargs["check"])

    def test_interruption_cleans_bootstrap_files_and_preserves_installer_status(self):
        for signum in (signal.SIGHUP, signal.SIGTERM):
            with (
                self.subTest(signum=signum),
                patch("sys.argv", ["bootstrap.py", str(self.feed)]),
                patch.object(bootstrap, "verify_package", return_value=self.root) as verify,
                patch.object(
                    bootstrap.subprocess,
                    "run",
                    side_effect=lambda *_args, **_kwargs: bootstrap.exit_on_signal(signum, None),
                ),
                self.assertRaises(SystemExit) as exited,
            ):
                bootstrap.main()
            self.assertEqual(exited.exception.code, 128 + signum)
            self.assertFalse(verify.call_args.args[1].parent.exists())

    def test_uninstall_runs_the_verified_installer_without_a_feed_argument(self):
        installer = self.root / "verified-installer"
        with (
            patch("sys.argv", ["bootstrap.py", "--uninstall", str(self.feed)]),
            patch.object(bootstrap, "verify_package", return_value=installer) as verify,
            patch.object(
                bootstrap.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
            ) as run,
        ):
            self.assertEqual(bootstrap.main(), 0)
        self.assertEqual(verify.call_args.args[0], self.feed)
        self.assertEqual(
            run.call_args.args[0], ["/bin/sh", str(installer / "install.sh"), "--uninstall"]
        )

    def test_cleanup_and_uninstall_cannot_be_combined(self):
        with (
            patch("sys.argv", ["bootstrap.py", "--cleanup", "--uninstall"]),
            patch.object(bootstrap, "download_release") as download,
            patch.object(bootstrap, "verify_package") as verify,
            patch("sys.stderr", new_callable=io.StringIO),
            self.assertRaises(SystemExit) as exited,
        ):
            bootstrap.main()
        self.assertEqual(exited.exception.code, 2)
        download.assert_not_called()
        verify.assert_not_called()
