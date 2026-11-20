import array
import fcntl
import json
import os
import socket
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

try:
    import numpy as np
except ImportError:
    np = None


@unittest.skipUnless(np is not None, "Frame raw camera tests need NumPy")
class RawCameraTest(unittest.TestCase):
    def setUp(self):
        with patch.object(
            sys, "path", [str(Path(__file__).resolve().parents[1] / "tools"), *sys.path]
        ):
            import frame_raw_camera as raw
        self.raw = raw
        self.sender, self.receiver = socket.socketpair(type=socket.SOCK_SEQPACKET)
        self.addCleanup(self.sender.close)
        self.addCleanup(self.receiver.close)
        self.cameras = {"left": SimpleNamespace(shape=(2, 3))}
        self.fd = os.memfd_create("synthetic-camera", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
        self.addCleanup(os.close, self.fd)
        os.write(self.fd, bytes([1, 2, 3, 0, 4, 5, 6, 0]))

    def send(self, seq=1, stride=4):
        header = {"seq": seq, "time": 1.0, "exposure": "ir", "views": [["left", stride]]}
        self.sender.sendmsg(
            [json.dumps(header).encode()],
            [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [self.fd]))],
        )

    def test_borrow_then_release(self):
        fcntl.fcntl(self.fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_WRITE | fcntl.F_SEAL_SHRINK)
        self.send()

        def consume(kind, t, images):
            self.assertEqual((kind, t), (38, 1.0))
            self.assertFalse(images["left"].flags.writeable)
            np.testing.assert_array_equal(images["left"], [[1, 2, 3], [4, 5, 6]])
            return ["seen"]

        self.assertEqual(self.raw.frame(self.receiver, self.cameras, 0, consume), (1, ["seen"]))
        self.assertEqual(json.loads(self.sender.recv(100)), {"release": 1})
        self.send()
        with self.assertRaisesRegex(ValueError, "Invalid raw camera group"):
            self.raw.frame(self.receiver, self.cameras, 1, consume)

    def test_require_stable_memory_and_valid_shape(self):
        self.send()
        with self.assertRaisesRegex(ValueError, "Unsealed"):
            self.raw.frame(self.receiver, self.cameras, 0, None)
        self.send(stride=5)
        with self.assertRaisesRegex(ValueError, "too short"):
            self.raw.frame(self.receiver, self.cameras, 0, None)

    def test_wrong_headset(self):
        self.sender.send(
            json.dumps({"version": 1, "device": {"device_serial_number": "different"}}).encode()
        )
        with self.assertRaisesRegex(ValueError, "does not match"):
            self.raw.calibration(self.receiver, "HEADSET")

    def test_read_only_shared_backing_with_offset(self):
        offset = os.sysconf("SC_PAGE_SIZE")
        os.pwrite(self.fd, bytes([7, 8, 9, 0, 1, 2, 3, 0]), offset)
        fcntl.fcntl(self.fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_SHRINK)
        fd = os.open(f"/proc/self/fd/{self.fd}", os.O_RDONLY)
        self.addCleanup(os.close, fd)
        header = {
            "seq": 1,
            "time": 1.0,
            "exposure": "ir",
            "storage": "udmabuf",
            "views": [["left", 4, offset]],
        }

        def send(descriptor):
            self.sender.sendmsg(
                [json.dumps(header).encode()],
                [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [descriptor]))],
            )

        def consume(kind, t, images):
            self.assertFalse(images["left"].flags.writeable)
            np.testing.assert_array_equal(images["left"], [[7, 8, 9], [1, 2, 3]])

        send(fd)
        self.raw.frame(self.receiver, self.cameras, 0, consume)
        self.assertEqual(json.loads(self.sender.recv(100)), {"release": 1})
        send(self.fd)
        with self.assertRaisesRegex(ValueError, "read-only"):
            self.raw.frame(self.receiver, self.cameras, 0, consume)

    def test_illumination_handoff_does_not_update_dark_baseline(self):
        with patch.object(
            sys, "path", [str(Path(__file__).resolve().parents[1] / "tools"), *sys.path]
        ):
            import frame_camera_worker as worker

        with patch.object(worker, "PalmBootstrap", return_value=SimpleNamespace(head=None)):
            tracker = worker.CameraTracking(
                dict.fromkeys(("left", "right", "top", "bottom")), {}, None
            )

        tracker.lit_requested = True
        image = {"left": np.full((2, 3), 255, dtype=np.uint8)}
        with patch.object(tracker.led, "update", return_value=None) as update:
            self.assertEqual(tracker.exposure(38, 1.0, image, lambda _: True), [])
            tracker.enabled_at = 1.1
            self.assertEqual(tracker.exposure(38, 1.05, image, lambda _: True), [])
            update.assert_not_called()
            tracker.exposure(38, 1.2, image, lambda _: True)
            self.assertTrue(update.call_args.args[2])

    def test_calibration_requires_four_models(self):
        with self.assertRaises(KeyError):
            self.raw.parse_calibration(
                {
                    "version": 1,
                    "device": {"device_serial_number": "HEADSET"},
                    "rig": {"cameras": []},
                    "metadata": {"cameras": {}},
                },
                "HEADSET",
            )
