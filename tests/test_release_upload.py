import os
import subprocess
import textwrap
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]


class ReleaseUploadTest(unittest.TestCase):
    def test_each_build_stages_only_the_selected_channel(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text()
        build, upload = workflow.split("      - name: Upload the selected channel package\n")
        script = textwrap.dedent(build.rsplit("        run: |\n", 1)[1])
        for channel in ("stable", "beta"):
            with self.subTest(channel=channel), TemporaryDirectory() as directory:
                work = Path(directory)
                (work / "build").mkdir()
                for name in ("optical_session_test", "tracked_input_test"):
                    binary = work / "build" / name
                    binary.write_text("#!/bin/sh\nexit 0\n")
                    binary.chmod(0o755)
                python = work / "python3"
                python.write_text(
                    "#!/bin/sh\n"
                    'if [ "$1" = tools/stage_frame.py ]; then printf \'%s\\n\' "$@" > args; fi\n'
                )
                python.chmod(0o755)
                make = work / "make"
                make.write_text("#!/bin/sh\nexit 0\n")
                make.chmod(0o755)
                subprocess.run(
                    ["bash", "-eu", "-c", script],
                    cwd=work,
                    env={
                        **os.environ,
                        "PATH": str(work) + ":" + os.environ["PATH"],
                        "FRAME_CHANNEL": channel,
                    },
                    check=True,
                    timeout=10,
                )
                self.assertEqual(
                    (work / "args").read_text().splitlines(),
                    [
                        "tools/stage_frame.py",
                        "--channel",
                        channel,
                        "--output",
                        f"build/controller-utils-frame-{channel}",
                        "--archive",
                        f"build/controller-utils-frame-{channel}-linuxarm64.tar.gz",
                    ],
                )
        self.assertIn("controller-utils-frame-${{ inputs.channel }}-${{ github.sha }}", upload)
        self.assertIn(
            "build/controller-utils-frame-${{ inputs.channel }}-linuxarm64.tar.gz", upload
        )
        self.assertNotIn("gh release", workflow)
