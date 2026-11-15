import json
import os
import subprocess
import textwrap
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]


class ReleaseUploadTest(unittest.TestCase):
    def test_only_signed_release_files_are_uploaded(self):
        contents = (ROOT / ".github/workflows/release.yml").read_text()
        step = contents.split("      - name: Publish complete GitHub release\n", 1)[1]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "build").mkdir()
            (root / "dist").mkdir()
            (root / "bin").mkdir()
            artifact = "controller-utils-0.1.0-5-linuxarm64.tar.gz"
            (root / "dist/latest.json").write_text(json.dumps({"artifact": artifact}))
            for name in (artifact, "latest.sig", "SHA256SUMS", "unrelated-secret.txt"):
                (root / "dist" / name).write_text("fixture")
            gh = root / "bin/gh"
            gh.write_text(
                "#!/usr/bin/python3\nimport json, os, sys\n"
                "with open(os.environ['UPLOAD_LOG'], 'a') as output:\n"
                "    output.write(json.dumps(sys.argv[1:]) + '\\n')\n"
                "if sys.argv[1:3] == ['release', 'view']: print('true')\n"
            )
            gh.chmod(0o755)
            log = root / "upload.jsonl"
            subprocess.run(
                ["bash", "-eu", "-c", script],
                cwd=root,
                check=True,
                env={
                    **os.environ,
                    "PATH": str(root / "bin") + ":" + os.environ["PATH"],
                    "RELEASE_TAG": "v0.1.0",
                    "UPLOAD_LOG": str(log),
                },
                capture_output=True,
                text=True,
                timeout=10,
            )
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            upload = next(call for call in calls if call[:2] == ["release", "upload"])
            self.assertEqual(
                upload,
                [
                    "release",
                    "upload",
                    "v0.1.0",
                    "dist/" + artifact,
                    "dist/latest.json",
                    "dist/latest.sig",
                    "dist/SHA256SUMS",
                    "--clobber",
                ],
            )
