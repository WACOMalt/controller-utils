import json
import os
import subprocess
import textwrap
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]


class ReleaseUploadTest(unittest.TestCase):
    def test_sequence_check_verifies_previous_metadata_independently_of_driver_layout(self):
        contents = (ROOT / ".github/workflows/release.yml").read_text()
        step = contents.split("      - name: Require a newer release sequence\n", 1)[1]
        script = textwrap.dedent(
            step.split("        run: |\n", 1)[1].split("      - name: Sign complete release\n", 1)[
                0
            ]
        )
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            for directory in ("build", "assets", "bin"):
                (root / directory).mkdir()
            key = root / "key.pem"
            subprocess.run(
                ["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(key)],
                check=True,
                capture_output=True,
            )
            subprocess.run(
                [
                    "openssl",
                    "pkey",
                    "-in",
                    str(key),
                    "-pubout",
                    "-out",
                    str(root / "assets/update-public.pem"),
                ],
                check=True,
                capture_output=True,
            )
            curl = root / "bin/curl"
            curl.write_text(
                "#!/usr/bin/python3\nimport sys\nfrom pathlib import Path\n"
                "args=sys.argv[1:]\noutput=Path(args[args.index('--output')+1])\n"
                "source='signed.sig' if output.suffix=='.sig' else 'signed.json'\n"
                "output.write_bytes(Path(source).read_bytes())\n"
                "if '--write-out' in args: print('200',end='')\n"
            )
            curl.chmod(0o755)
            previous = {
                "version": "0.1.3",
                "sequence": 9,
                "requirements": {
                    "driver": {"bin/linuxarm64/driver_controllertracking_native.so": "a" * 64}
                },
            }
            for sequence, version, tampered, accepted in (
                (10, "0.1.4", False, True),
                (10, "0.1.4", True, False),
                (9, "0.1.4", False, False),
                (10, "0.1.3", False, False),
            ):
                with self.subTest(sequence=sequence, version=version, tampered=tampered):
                    (root / "release.json").write_text(
                        json.dumps({"version": version, "sequence": sequence})
                    )
                    (root / "signed.json").write_text(json.dumps(previous))
                    subprocess.run(
                        [
                            "openssl",
                            "pkeyutl",
                            "-sign",
                            "-rawin",
                            "-inkey",
                            str(key),
                            "-in",
                            str(root / "signed.json"),
                            "-out",
                            str(root / "signed.sig"),
                        ],
                        check=True,
                        capture_output=True,
                    )
                    if tampered:
                        with (root / "signed.json").open("a") as output:
                            output.write(" ")
                    result = subprocess.run(
                        ["bash", "-eu", "-c", script],
                        cwd=root,
                        env={**os.environ, "PATH": str(root / "bin") + ":" + os.environ["PATH"]},
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )
                    self.assertEqual(
                        result.returncode == 0, accepted, result.stdout + result.stderr
                    )

    def test_published_releases_are_skipped_and_api_failures_are_reported(self):
        contents = (ROOT / ".github/workflows/release.yml").read_text()
        step = contents.split("      - name: Check whether the release is already published\n", 1)[
            1
        ]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1].split("\n  release:\n", 1)[0])
        for response, status, expected in (
            ('{"draft":false}', 0, "needed=false"),
            ('{"draft":true}', 0, "needed=true"),
            ("gh: Not Found (HTTP 404)", 1, "needed=true"),
            ("gh: Service Unavailable (HTTP 503)", 1, None),
        ):
            with self.subTest(response=response), TemporaryDirectory() as temporary:
                root = Path(temporary)
                gh = root / "gh"
                gh.write_text(
                    "#!/usr/bin/python3\nimport os,sys\n"
                    "status=int(os.environ['TEST_STATUS'])\n"
                    "print(os.environ['TEST_RESPONSE'],file=sys.stderr if status else sys.stdout)\n"
                    "sys.exit(status)\n"
                )
                gh.chmod(0o755)
                output = root / "output"
                result = subprocess.run(
                    ["bash", "-eu", "-c", script],
                    cwd=root,
                    env={
                        **os.environ,
                        "PATH": str(root) + ":" + os.environ["PATH"],
                        "GITHUB_OUTPUT": str(output),
                        "GH_REPO": "kallupx/controller-utils",
                        "RELEASE_TAG": "v0.1.1",
                        "TEST_RESPONSE": response,
                        "TEST_STATUS": str(status),
                    },
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                if expected:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(output.read_text().strip(), expected)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(output.exists())

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
            for name in (
                artifact,
                "latest.sig",
                "latest-stable.json",
                "latest-stable.sig",
                "SHA256SUMS",
                "unrelated-secret.txt",
            ):
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
                    "dist/latest-stable.json",
                    "dist/latest-stable.sig",
                    "dist/SHA256SUMS",
                    "--clobber",
                ],
            )
