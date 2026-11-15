import json
import re
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ReleaseConfigurationTest(unittest.TestCase):
    def test_runtime_profile_matches_native_pins_and_audited_layout(self):
        runtime = json.loads((ROOT / "driver/runtime.json").read_text())
        layout = json.loads((ROOT / "driver/native_layout.json").read_text())
        pins = (ROOT / "src/steamvr/compatibility.h").read_text()
        for digest in runtime["runtime"].values():
            self.assertRegex(digest, r"^[a-f0-9]{64}$")
            self.assertIn('"' + digest + '"', pins)
        self.assertEqual(layout["xr_sha256"], runtime["runtime"]["XRService"])
        self.assertEqual(len(layout["sites"]), 13)
        for original in layout["sites"].values():
            self.assertEqual(len(bytes.fromhex(original)), 16)
        self.assertGreater(len(bytes.fromhex(layout["cave_original"])), 512)
        self.assertEqual(runtime["backend"], "native-tracker")

    def test_github_channel_and_installer_syntax(self):
        channel = json.loads((ROOT / "assets/update-channel.json").read_text())
        self.assertEqual(
            channel,
            {"url": "https://github.com/kallupx/controller-utils/releases/latest/download/"},
        )
        for script in ("install.sh", "uninstall.sh"):
            subprocess.run(["sh", "-n", str(ROOT / "packaging" / script)], check=True)
        subprocess.run(
            ["openssl", "pkey", "-pubin", "-in", str(ROOT / "assets/update-public.pem"), "-noout"],
            check=True,
        )

    def test_privileged_commands_are_complete_and_narrow(self):
        policy = ROOT / "packaging/controller-utils-updater.sudoers"
        contents = policy.read_text()
        commands = []
        for line in contents.splitlines():
            prefix, values = line.split(": ", 1)
            self.assertEqual(prefix, "steamos ALL=(root) NOPASSWD")
            commands.extend(values.split(", "))
        self.assertEqual(
            set(commands),
            {
                "/var/lib/controllertracking/controller-utils-helper --private",
                "/var/lib/controllertracking/controller-utils-updater --apply",
                "/var/lib/controllertracking/controller-utils-updater --confirm",
                "/var/lib/controllertracking/controller-utils-updater --repair",
                "/var/lib/controllertracking/controller-utils-updater --repair-status",
            },
        )
        if shutil.which("visudo"):
            subprocess.run(["visudo", "-cf", str(policy)], check=True, capture_output=True)

    @unittest.skipUnless(shutil.which("systemd-analyze"), "systemd-analyze is unavailable")
    def test_startup_service_is_valid(self):
        result = subprocess.run(
            [
                "systemd-analyze",
                "--user",
                "verify",
                str(ROOT / "packaging/controller-utils-update.service"),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        service = (ROOT / "packaging/controller-utils-update.service").read_text()
        self.assertIn("Before=steamvr.service", service)
        self.assertIn("WantedBy=steamvr.service", service)
        self.assertRegex(
            service,
            re.escape(
                "ExecStart=/usr/bin/sudo -n /var/lib/controllertracking/controller-utils-updater --apply"
            ),
        )
