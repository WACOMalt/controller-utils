import itertools
import unittest

try:
    import numpy as np
except ImportError:
    np = None


@unittest.skipIf(np is None, "Frame optical tests need NumPy")
class FrameLedTrackingTest(unittest.TestCase):
    def setUp(self):
        from tools.frame_led_tracking import TRACKING_CAMERAS, LedTracker, calibrated_cameras

        cameras = []
        snapshot = {"cameras": {}}
        for name, (x, y) in zip(TRACKING_CAMERAS, ((-40, 0), (40, 0), (-65, 35), (65, 35))):
            cameras.append(
                {
                    "sourceCamera": name,
                    "cameraModel": "kb",
                    "width": 240,
                    "height": 200,
                    "extrinsics": {
                        "__units": "mm",
                        "position": [x, y, 0],
                        "plus_x": [1, 0, 0],
                        "plus_z": [0, 0, 1],
                    },
                    "intrinsics": {
                        "fx": 190,
                        "fy": 190,
                        "cx": 120,
                        "cy": 100,
                        "k1": 0.05,
                        "k2": -0.004,
                        "k3": 0,
                        "k4": 0,
                    },
                }
            )
            snapshot["cameras"][name] = {"serialNumber": name}
        cameras[2]["parent"] = "Cam1"
        cameras[3]["parent"] = "Cam0"
        basis = {"position": [0, 0, 0], "plus_x": [1, 0, 0], "plus_z": [0, 0, 1]}
        self.cams, _ = calibrated_cameras(
            {"cameras": cameras}, snapshot, {"cv": {"cad_from_cal": basis}, "head": basis}
        )
        self.make = lambda: LedTracker(self.cams)
        self.p = np.array([0.025, 0.033, 0.48])
        self.pixels = {k: [c.project(self.p)] for k, c in self.cams.items()}

    def track(self, views, tracker=None):
        tracker = tracker or self.make()
        for i in range(5):
            tracker.update(1 + i * 0.03, {k: [] for k in self.cams}, False)
        fix = None
        for i in range(3):
            fix = tracker.update(2 + i * 0.03, views, True)
        return fix

    def test_all_pairs_and_all_four(self):
        for keys in [*itertools.combinations(self.cams, 2), tuple(self.cams)]:
            with self.subTest(keys=keys):
                p, support = self.track({k: self.pixels[k] for k in keys})
                np.testing.assert_allclose(p, self.p, atol=1e-6)
                self.assertEqual(set(keys), set(support))

    def test_one_outlier_and_loss(self):
        t = self.make()
        bad = dict(self.pixels, upper_right=[[5, 12]])
        p, views = self.track(bad, t)
        self.assertEqual(len(views), 3)
        np.testing.assert_allclose(p, self.p, atol=1e-6)
        self.assertIsNone(t.update(2.09, {"upper_left": self.pixels["upper_left"]}, True))
        self.assertIsNone(t.update(2.3, {}, True))
        self.assertIsNone(t.update(2.33, self.pixels, True))
        self.assertIsNone(t.update(2.36, self.pixels, True))
        self.assertIsNotNone(t.update(2.39, self.pixels, True))

    def test_dark_background(self):
        t = self.make()
        for i in range(5):
            t.update(i * 0.03, self.pixels, False)
        for i in range(5):
            self.assertIsNone(t.update(1 + i * 0.03, self.pixels, True))

    def test_centroids(self):
        from tools.frame_led_tracking import centroids

        im = np.zeros((220, 230), dtype=np.uint8)
        im[200, 215:217] = [180, 240]
        (center,) = centroids(im)
        np.testing.assert_allclose(center, [215 + 240 / 420, 200])

    def test_dim_led_after_camera_pair_changes(self):
        tracker = self.make()
        self.assertIsNotNone(self.track(self.pixels, tracker))
        # A head turn moves the projection farther than the previous rig-space fix expects.
        point = self.p + [0.15, -0.02, 0.0]
        prior = point + [0.005, 0.0, 0.0]
        keys = ("slam_right", "upper_left")
        images = {k: np.full(self.cams[k].shape, 25, dtype=np.uint8) for k in keys}
        for k, image in images.items():
            x, y = np.rint(self.cams[k].project(point)).astype(int)
            image[y, x] = 83
            image[20, 20] = 100
        # A dim point is usable with a pose prior. One view only constrains the bearing.
        first = keys[0]
        self.assertIsNone(tracker.update(2.09, {first: []}, True, prior, images))
        origin, ray = tracker.bearing
        np.testing.assert_allclose(origin, self.cams[first].origin)
        self.assertLess(np.linalg.norm(np.cross(point - origin, ray)), 0.003)
        fixed, support = tracker.update(2.12, dict.fromkeys(keys, []), True, prior, images)
        np.testing.assert_allclose(fixed, point, atol=0.007)
        self.assertEqual(set(support), set(keys))
        self.assertIsNone(tracker.bearing)
        self.assertIsNone(tracker.update(2.15, dict.fromkeys(keys, []), True))

    def test_resume_finds_a_dim_led_and_rejects_existing_dim_background(self):
        keys = ("slam_right", "upper_left")
        detections = dict.fromkeys(keys, [])

        def images(point, brightness):
            result = {k: np.full(self.cams[k].shape, 25, dtype=np.uint8) for k in keys}
            for key, image in result.items():
                x, y = np.rint(self.cams[key].project(point)).astype(int)
                image[y, x] = brightness
            return result

        before = self.p - [0.08, 0, 0]
        illuminated = images(self.p, 83)
        for background in (25, 83):
            with self.subTest(background=background):
                tracker = self.make()
                dark = images(before, background)
                for i in range(5):
                    tracker.update(i * 0.03, detections, False, before, dark, resume=True)
                fix = None
                for i in range(4):
                    fix = tracker.update(
                        1 + i * 0.03, detections, True, self.p, illuminated, resume=True
                    )
                if background == 25:
                    position, support = fix
                    np.testing.assert_allclose(position, self.p, atol=0.008)
                    self.assertEqual(set(support), set(keys))
                else:
                    self.assertIsNone(fix)
