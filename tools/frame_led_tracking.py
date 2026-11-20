"""Four-camera IR association in the factory calibration frame (metres)."""

from itertools import combinations

import numpy as np

try:
    from .frame_palm_bootstrap import Camera
except ImportError:
    from frame_palm_bootstrap import Camera

TRACKING_CAMERAS = ("slam_left", "slam_right", "upper_left", "upper_right")


def calibrated_cameras(rig, metadata, device):
    """The factory exports all four extrinsics in Cam0, including parent-labelled cameras."""
    cameras = {}
    entries = {c["sourceCamera"]: c for c in rig["cameras"]}
    for name in TRACKING_CAMERAS:
        config = dict(entries[name])
        parent = config.pop("parent", None)
        if parent not in (None, {"upper_left": "Cam1", "upper_right": "Cam0"}.get(name)):
            raise ValueError("Unknown camera calibration layout")
        serial = metadata["cameras"][name]["serialNumber"]
        if not serial or serial in cameras:
            raise ValueError("Missing or repeated camera serial")
        cameras[serial] = Camera(config)
    cad = device["cv"]["cad_from_cal"]
    head = device["head"]

    def axes(entry):
        x, z = np.array(entry["plus_x"]), np.array(entry["plus_z"])
        return np.column_stack([x, np.cross(z, x), z])

    head_from_cal = np.column_stack(
        [axes(head).T @ axes(cad), axes(head).T @ (np.array(cad["position"]) - head["position"])]
    )
    return cameras, {"head_from_cal": head_from_cal, "cameras": entries}


def centroids(image, threshold=160, max_area=100):
    """Find small, isolated bright components in the short exposure."""
    if image.ndim != 2 or image.dtype != np.uint8:
        raise ValueError("Expected grayscale IR image")
    flat = np.flatnonzero((image >= threshold).ravel())
    if len(flat) > 4096:
        return []
    y, x = np.divmod(flat, image.shape[1])
    remaining = set(zip(y.tolist(), x.tolist()))
    centers = []
    while remaining:
        pending = [remaining.pop()]
        pixels = []
        while pending:
            row, col = pending.pop()
            pixels.append((row, col))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    neighbor = (row + dy, col + dx)
                    if neighbor in remaining:
                        remaining.remove(neighbor)
                        pending.append(neighbor)
        if len(pixels) <= max_area:
            rows, cols = np.transpose(pixels)
            weights = image[rows, cols]
            centers.append(np.array([cols @ weights, rows @ weights]) / weights.sum())
    return centers if len(centers) <= 128 else []


def dim_centroid(image, expected):
    """Look for a compact IR point against the local background near the IMU projection."""
    x, y = np.rint(expected).astype(int)
    h, w = image.shape
    if not (-6 <= x < w + 6 and -6 <= y < h + 6):
        return None
    left, top = max(0, x - 14), max(0, y - 14)
    crop = image[top : y + 15, left : x + 15]
    median, high = np.percentile(crop, [50, 90])
    threshold = max(55, high + 18, median + 3 * (high - median))
    if threshold >= 160:
        return None
    peaks = [p + [left, top] for p in centroids(crop, threshold, max_area=32)]
    close = [p for p in peaks if np.linalg.norm(p - expected) < 12]
    return close[0] if len(close) == 1 else None


def directions(camera, pixels):
    xy = (pixels - camera.center) / camera.focal
    radius = np.linalg.norm(xy, axis=1)
    lo, hi = np.zeros_like(radius), np.full_like(radius, np.pi / 2)
    a, b, c, d = camera.distortion
    for _ in range(24):
        angle = (lo + hi) / 2
        t = angle * angle
        estimate = angle * (1 + t * (a + t * (b + t * (c + t * d))))
        lo = np.where(estimate < radius, angle, lo)
        hi = np.where(estimate < radius, hi, angle)
    angle = (lo + hi) / 2
    rays = np.column_stack(
        [xy * (np.sin(angle) / np.maximum(radius, 1e-15))[:, None], np.cos(angle)]
    )
    rays[angle >= np.pi / 2 - 1e-6] = np.nan
    return rays @ camera.axes


class LedTracker:
    def __init__(self, cameras):
        if len(cameras) != 4:
            raise ValueError("Expected four calibrated tracking cameras")
        self.cameras = cameras
        self.dark = {k: [] for k in cameras}
        self.baselines = dict.fromkeys(cameras, 0)
        self.last = None
        self.streak = 0
        self.time = -np.inf
        self.bearing = None

    def update(self, t, detections, illuminated, prior=None, images=None):
        """One synchronized exposure group, with absent cameras omitted."""
        if not np.isfinite(t) or t <= self.time or not detections.keys() <= self.cameras.keys():
            raise ValueError("Invalid IR exposure time or camera")
        self.time = t
        self.bearing = None
        rays, origins, bearings = {}, {}, []
        predicted = self.last is not None and t - self.last[0] < 0.15
        for key, points in detections.items():
            camera = self.cameras[key]
            pixels = np.asarray(points, dtype=float).reshape(-1, 2)
            if len(pixels) > 128 or not np.isfinite(pixels).all():
                raise ValueError("Invalid IR centroids")
            if not illuminated:
                self.baselines[key] += 1
                for p in pixels:
                    if not any(np.linalg.norm(p - q) < 8 for q in self.dark[key]):
                        if len(self.dark[key]) >= 512:
                            raise ValueError("Too much movement during LED blink")
                        self.dark[key].append(p)
                continue
            if self.baselines[key] < 5:
                continue
            expected = None
            if prior is not None and (prior - camera.origin) @ camera.axes[2] > 0:
                expected = camera.project(prior)
                if (
                    images is not None
                    and len(pixels) < 128
                    and not np.any(np.linalg.norm(pixels - expected, axis=1) < 12)
                    and (weak := dim_centroid(images[key], expected)) is not None
                ):
                    pixels = np.vstack([pixels, weak])
            if not len(pixels):
                continue
            # The clustered LEDs on the stock VR controllers are a frequent distractor.
            near = np.linalg.norm(pixels[:, None] - pixels, axis=2) < 65
            isolated = near.sum(axis=1) <= 2
            if self.dark[key]:
                isolated &= (
                    np.linalg.norm(pixels[:, None] - self.dark[key], axis=2).min(axis=1) > 12
                )
            if expected is not None:
                near_prior = np.linalg.norm(pixels - expected, axis=1) < 12
                guided = directions(camera, pixels[isolated & near_prior])
                bearings.extend((camera.origin, r) for r in guided if np.isfinite(r).all())
                # Allow a stereo consensus to recover the LED in a cluttered camera.
                isolated |= near_prior
            if predicted:
                isolated |= np.linalg.norm(pixels - camera.project(self.last[1]), axis=1) < 10
            if not 0 < isolated.sum() <= 8:
                continue
            r = directions(camera, pixels[isolated])
            rays[key] = r[np.isfinite(r).all(axis=1)]
            origins[key] = camera.origin

        estimate = self.last[1] if predicted else None
        if not predicted or not illuminated:
            self.streak = 0
            self.last = None
        solutions = []
        for a, b in combinations(rays, 2):
            baseline = origins[a] - origins[b]
            for u in rays[a]:
                for v in rays[b]:
                    uv = u @ v
                    denom = 1 - uv**2
                    if denom < 0.0025:
                        continue
                    du, dv = u @ baseline, v @ baseline
                    s, z = (uv * dv - du) / denom, (dv - uv * du) / denom
                    if not 0.07 < min(s, z) or max(s, z) > 1.5:
                        continue
                    pa, pb = origins[a] + s * u, origins[b] + z * v
                    if np.linalg.norm(pa - pb) > 0.005:
                        continue
                    point = (pa + pb) / 2
                    support = {}
                    for key, candidates in rays.items():
                        if not len(candidates):
                            continue
                        offset = point - origins[key]
                        along = candidates @ offset
                        errors = np.linalg.norm(offset - along[:, None] * candidates, axis=1)
                        i = np.argmin(errors)
                        if along[i] > 0 and errors[i] <= 0.005:
                            support[key] = candidates[i]
                    if len(support) < 2:
                        continue
                    matrices = [np.eye(3) - np.outer(r, r) for r in support.values()]
                    A = np.sum(matrices, axis=0)
                    if np.linalg.cond(A) > 1600:
                        continue
                    fitted = np.linalg.solve(
                        A, sum(m @ origins[k] for m, k in zip(matrices, support))
                    )
                    if predicted:
                        limit = 0.025 + 3 * (t - self.last[0])
                        if np.linalg.norm(fitted - estimate) > limit and (
                            prior is None or np.linalg.norm(fitted - prior) > limit
                        ):
                            continue
                    if not any(
                        support.keys() == old.keys() and np.linalg.norm(fitted - p) < 0.002
                        for p, old in solutions
                    ):
                        solutions.append((fitted, support))
        if not solutions:
            if len(bearings) == 1:
                self.bearing = bearings[0]
            if self.streak < 3:
                self.streak, self.last = 0, None
            return None
        if prior is not None and any(np.linalg.norm(p - prior) < 0.10 for p, _ in solutions):
            estimate = prior
        solutions.sort(
            key=lambda item: (
                -len(item[1]),
                np.linalg.norm(item[0] - estimate) if estimate is not None else 0,
            )
        )
        position, support = solutions[0]
        for p, other in solutions[1:]:
            if len(other) == len(support) and np.linalg.norm(p - position) > 0.012:
                if (
                    estimate is None
                    or np.linalg.norm(p - estimate) - np.linalg.norm(position - estimate) < 0.015
                ):
                    if self.streak < 3:
                        self.streak, self.last = 0, None
                    return None
        self.last = t, position
        self.streak += 1
        return (position, tuple(support)) if self.streak >= 3 else None
