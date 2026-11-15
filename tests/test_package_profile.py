import json
import subprocess
import sys
import time
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import package_profile


class PackageProfileTest(unittest.TestCase):
    def test_signed_profile_and_rejection_boundaries(self):
        with TemporaryDirectory(dir=package_profile.ROOT / "build") as temporary:
            root = Path(temporary)
            key, trust = root / "private.pem", root / "public.pem"
            subprocess.run(
                ["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(key)],
                check=True,
                capture_output=True,
                timeout=5,
            )
            key.chmod(0o600)
            trust.write_bytes(package_profile.public_key(key))
            source = package_profile.ROOT / "compatibility/queue-v2.json"
            expires = int(time.time()) + 3600
            result = package_profile.build(source, 1, expires, key, trust, root / "valid")
            self.assertEqual(result["sequence"], 1)
            record = (root / "valid/compatibility-profile-1.bin").read_bytes()
            self.assertNotIn("module_sha256", json.loads(record[64:])["queue"])
            manifest, signature = root / "profile.json", root / "profile.sig"
            manifest.write_bytes(record[64:])
            signature.write_bytes(record[:64])
            self.assertEqual(signature.stat().st_size, 64)
            manifest.write_bytes(manifest.read_bytes() + b" ")
            rejected = subprocess.run(
                [
                    str(package_profile.ROOT / "build/release_check"),
                    "--profile",
                    str(manifest),
                    str(signature),
                    str(trust),
                ],
                capture_output=True,
                timeout=5,
            )
            self.assertNotEqual(rejected.returncode, 0)
            with self.assertRaises(ValueError):
                package_profile.build(source, 1, expires, key, trust, root / "valid")
            for sequence, expiry in ((0, expires), (1, 1)):
                with self.assertRaises(subprocess.CalledProcessError):
                    package_profile.build(
                        source, sequence, expiry, key, trust, root / f"bad-{sequence}-{expiry}"
                    )
            key.chmod(0o644)
            with self.assertRaises(ValueError):
                package_profile.build(source, 2, expires, key, trust, root / "bad-key")
