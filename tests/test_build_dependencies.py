import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class BuildDependenciesTest(unittest.TestCase):
    def test_installer_headers_and_shared_fixture_invalidate_binaries(self):
        for target, dependency in (
            ("build/controller-utils-updater", "src/update/installer.h"),
            ("build/controller-utils-updater", "src/update/setup.h"),
            ("build/controller-utils-updater", "assets/update-public.pem"),
            ("build/update_installer_test", "tests/update_fixture.h"),
            ("build/update_setup_test", "tests/update_fixture.h"),
        ):
            with self.subTest(target=target, dependency=dependency):
                current = subprocess.run(["make", "-q", target], cwd=ROOT, capture_output=True)
                self.assertEqual(current.returncode, 0, current.stderr.decode())
                changed = subprocess.run(
                    ["make", "-n", "-W", dependency, target],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    check=True,
                )
                self.assertIn("-o " + target, changed.stdout)
