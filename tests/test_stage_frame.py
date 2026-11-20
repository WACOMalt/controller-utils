import ast
import contextlib
import io
import json
import runpy
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import stage_frame  # noqa: E402


def arm64_file(path):
    header = b"\x7fELF\x02\x01" + bytes(12) + b"\xb7\x00"
    return header + path.name.encode()


class FramePackageTest(unittest.TestCase):
    def test_preflight_accepts_a_new_build_on_the_same_channel(self):
        main = runpy.run_path(str(ROOT / "packaging/frame_install.py"))["main"]
        output = io.StringIO()

        def command(*args, **kwargs):
            return SimpleNamespace(
                stdout={"tracked-branch": "stable", "tracked-variant": "vr"}.get(args[-1], "")
            )

        with (
            patch.dict(
                main.__globals__,
                {
                    "verify_package": lambda _: ("stable", "20261022.6150929"),
                    "command": command,
                    "calibration": lambda _: "{}",
                },
            ),
            patch.object(sys, "argv", ["install.py", "--check"]),
            patch("os.geteuid", return_value=1000),
            patch("platform.machine", return_value="aarch64"),
            patch.object(Path, "read_text", return_value="BUILD_ID=20261029.6150930\n"),
            patch("subprocess.run") as palm,
            contextlib.redirect_stdout(output),
        ):
            main()
        palm.assert_called_once()
        self.assertIn("Running 20261029.6150930", output.getvalue())
        self.assertIn("Frame stable package preflight passed", output.getvalue())

    def test_installer_and_worker_support_stable_python(self):
        scripts = [
            ROOT / "packaging/frame_install.py",
            *(ROOT / "tools" / name for name in stage_frame.OPTICAL_FILES if name.endswith(".py")),
        ]
        for script in scripts:
            ast.parse(script.read_text(), filename=str(script), feature_version=(3, 12))

    def test_channel_packages_are_independent_and_contain_only_the_shared_camera_backend(self):
        verify = runpy.run_path(str(ROOT / "packaging/frame_install.py"))["verify_package"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stable, beta = root / "stable", root / "beta"
            with (
                patch.object(stage_frame, "read_source", side_effect=arm64_file),
                patch.object(stage_frame, "check_stable_abi") as check_abi,
            ):
                stage_frame.stage(stable, "stable")
                stable_checks = check_abi.call_count
                stage_frame.stage(beta, "beta")
            self.assertGreater(stable_checks, 0)
            self.assertEqual(check_abi.call_count, stable_checks)
            self.assertEqual(verify(stable), ("stable", stage_frame.STEAMOS_BUILDS["stable"]))
            self.assertEqual(verify(beta), ("beta", stage_frame.STEAMOS_BUILDS["beta"]))
            manifest = json.loads((stable / "frame-package.json").read_bytes())
            self.assertIn("app/optical/frame_camera_hook.so", manifest["files"])
            self.assertIn(
                "app/native-driver/bin/linuxarm64/driver_controllerutils_native.so",
                manifest["files"],
            )
            self.assertFalse(
                any(
                    "native-tracker" in name or "controller-utils-helper" in name
                    for name in manifest["files"]
                )
            )
            (beta / "app/controller-utils").write_bytes(b"changed")
            self.assertEqual(verify(stable), ("stable", stage_frame.STEAMOS_BUILDS["stable"]))
            with self.assertRaisesRegex(ValueError, "checksum"):
                verify(beta)

    def test_desktop_artifacts_cannot_be_staged_for_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(stage_frame, "read_source", return_value=b"desktop"):
                with self.assertRaisesRegex(ValueError, "ARM64"):
                    stage_frame.stage(Path(directory) / "package", "stable")

    def test_stable_package_rejects_a_beta_libc_requirement(self):
        with patch.object(stage_frame.subprocess, "check_output", return_value="Name: GLIBC_2.43"):
            with self.assertRaisesRegex(ValueError, "newer than SteamOS stable"):
                stage_frame.check_stable_abi(Path("controller-utils"))

    def test_installer_restores_the_previous_app_if_a_hook_fails_to_start(self):
        source = ROOT / "packaging/frame_install.py"
        install = runpy.run_path(str(source))["install"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base, package = root / "installed", root / "package"
            app = base / "app"
            (app / "optical").mkdir(parents=True)
            (app / "controller-utils").write_bytes(b"previous app")
            (package / "app/optical").mkdir(parents=True)
            (package / "app/controller-utils").write_bytes(b"new app")
            (package / "controller-utils").write_bytes(b"new launcher")
            rule = root / "70-controller-utils.rules"
            rule.write_bytes(b"udev rules")
            (package / rule.name).write_bytes(rule.read_bytes())
            config = base / "app-data/camera-calibration.json"
            config.parent.mkdir()
            config.write_text("previous calibration")

            def fail():
                raise RuntimeError("beam hook failed")

            with patch.dict(
                install.__globals__,
                {
                    "BASE": base,
                    "UNITS": root / "systemd",
                    "RULE": rule,
                    "numpy_environment": lambda _: root / "numpy",
                    "systemctl": lambda *args, **kwargs: SimpleNamespace(returncode=1),
                    "command": lambda *args, **kwargs: None,
                    "ready": fail,
                },
            ):
                with self.assertRaisesRegex(RuntimeError, "beam hook failed"):
                    install(package, "new calibration")
            self.assertEqual((app / "controller-utils").read_bytes(), b"previous app")
            self.assertEqual(config.read_text(), "previous calibration")
            self.assertFalse((base / "native-driver").is_symlink())
