import json
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class NativeEntryTest(unittest.TestCase):
    def test_download_worker_dispatch_precedes_runtime_initialization(self):
        result = subprocess.run(
            [str(ROOT / "build/controller-utils"), "--update-download-worker"],
            input="{}",
            capture_output=True,
            text=True,
            timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        event = json.loads(result.stdout)
        self.assertEqual(event["kind"], "error")
        self.assertIn("error", event)
        self.assertNotIn("OpenVR", result.stderr)

    def test_invalid_arguments_do_not_open_runtime_or_controller_resources(self):
        for arguments in (["--unknown"], ["--show", "--background"]):
            result = subprocess.run(
                [str(ROOT / "build/controller-utils"), *arguments],
                capture_output=True,
                text=True,
                timeout=5,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("usage:", result.stderr)
            self.assertFalse(result.stdout)


if __name__ == "__main__":
    unittest.main()
