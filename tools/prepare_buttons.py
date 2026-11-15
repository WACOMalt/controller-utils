#!/usr/bin/env python3
import hashlib
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# IDs match ButtonAnimation::masks, travel is in metres, not a geometry scale.
BUTTONS = [
    ("A", 1, 0.0008),
    ("B", 2, 0.0008),
    ("X", 4, 0.0008),
    ("Y", 8, 0.0008),
    ("steam", 0x10000, 0.00045),
    ("view", 0x40, 0.00045),
    ("menu", 0x4000, 0.00045),
    ("quick", 0x10, 0.00045),
    ("bumper_left", 0x80000, 0.0007),
    ("bumper_right", 0x200, 0.0007),
    ("rear_left_upper", 0x20000, 0.00065),
    ("rear_right_upper", 0x80, 0.00065),
    ("rear_left_lower", 0x40000, 0.00065),
    ("rear_right_lower", 0x100, 0.00065),
]


def pad_mapping(triangles, selected):
    points = sorted(
        {(triangles[i][3 + c * 3], triangles[i][5 + c * 3]) for i in selected for c in range(3)}
    )

    def half_hull(values):
        hull = []
        for point in values:
            while len(hull) >= 2:
                a, b = hull[-2:]
                cross = (b[0] - a[0]) * (point[1] - a[1]) - (b[1] - a[1]) * (point[0] - a[0])
                if cross > 0:
                    break
                hull.pop()
            hull.append(point)
        return hull

    hull = half_hull(points)[:-1] + half_hull(reversed(points))[:-1]
    if len(hull) < 3:
        raise ValueError("Touchpad surface has no area")
    best = None
    for a, b in zip(hull, hull[1:] + hull[:1]):
        angle = (math.atan2(b[1] - a[1], b[0] - a[0]) + math.pi / 4) % (math.pi / 2) - math.pi / 4
        right, up = (math.cos(angle), math.sin(angle)), (math.sin(angle), -math.cos(angle))
        horizontal = [p[0] * right[0] + p[1] * right[1] for p in hull]
        vertical = [p[0] * up[0] + p[1] * up[1] for p in hull]
        cx, cy = (max(horizontal) + min(horizontal)) / 2, (max(vertical) + min(vertical)) / 2
        rx, ry = (max(horizontal) - min(horizontal)) / 2, (max(vertical) - min(vertical)) / 2
        area = rx * ry
        if best is None or area < best[0]:
            best = (
                area,
                [
                    cx * right[0] + cy * up[0],
                    cx * right[1] + cy * up[1],
                    rx * right[0],
                    rx * right[1],
                    ry * up[0],
                    ry * up[1],
                ],
            )
    return best[1]


def main():
    raw = (ROOT / "assets/controller.stl").read_bytes()
    metadata = json.loads((ROOT / "assets/controller.surface.json").read_text())
    digest = hashlib.sha256(raw).hexdigest()
    if digest != metadata["geometry_sha256"]:
        raise ValueError("Button regions require the textured CAD geometry")
    triangles = list(struct.iter_unpack("<12fH", raw[84:]))
    (count,) = struct.unpack_from("<I", raw, 80)
    if len(triangles) != count:
        raise ValueError("Invalid CAD triangle count")
    parents, edges = list(range(count)), {}

    def root(i):
        while parents[i] != i:
            parents[i] = parents[parents[i]]
            i = parents[i]
        return i

    # Match the appearance metadata's 20-degree sharp-edge regions.
    threshold = math.cos(math.radians(20))
    for i, triangle in enumerate(triangles):
        vertices = [triangle[3 + j * 3 : 6 + j * 3] for j in range(3)]
        for a, b in ((0, 1), (1, 2), (2, 0)):
            edge = tuple(sorted((vertices[a], vertices[b])))
            other = edges.get(edge)
            if other is None:
                edges[edge] = i
            elif sum(triangle[k] * triangles[other][k] for k in range(3)) > threshold:
                parents[root(i)] = root(other)
    groups = {}
    for i in range(count):
        groups.setdefault(root(i), []).append(i)

    def region(name):
        source = metadata["parts"][name]
        selected = groups[root(source["seed_face"])]
        if len(selected) != source["triangles"]:
            raise ValueError("CAD control region changed: " + name)
        return selected

    tags, records, descriptions = bytearray(count), bytearray(), []
    for part, (name, mask, travel) in enumerate(BUTTONS + [("dpad", 0x3C00, 0)], 1):
        selected = region(name)
        if any(tags[i] for i in selected):
            raise ValueError("CAD button regions overlap: " + name)
        normal = [0.0, 0.0, 0.0]
        for i in selected:
            tags[i] = part
            t = triangles[i]
            ab, ac = (
                [t[6 + k] - t[3 + k] for k in range(3)],
                [t[9 + k] - t[3 + k] for k in range(3)],
            )
            cross = [
                ab[1] * ac[2] - ab[2] * ac[1],
                ab[2] * ac[0] - ab[0] * ac[2],
                ab[0] * ac[1] - ab[1] * ac[0],
            ]
            area = math.sqrt(sum(v * v for v in cross))
            for k in range(3):
                normal[k] += t[k] * area
        length = math.sqrt(sum(v * v for v in normal))
        if not math.isfinite(length) or length < 1e-8:
            raise ValueError("Button has no unambiguous outward surface: " + name)
        delta = [-travel * v / length for v in normal]
        if name != "dpad":
            records.extend(struct.pack("<I3f", mask, *delta))
        descriptions.append(
            {"name": name, "mask": mask, "triangles": len(selected), "press_translation": delta}
        )
    bounds = metadata["parts"]["dpad"]["bounds"]
    pivot = [(bounds[0][0] + bounds[1][0]) / 2, bounds[0][1], (bounds[0][2] + bounds[1][2]) / 2]
    pivots = []
    for part, name in enumerate(("trigger_left", "trigger_right", "stick_left", "stick_right"), 16):
        selected = set(region(name))
        low, high = metadata["parts"][name]["bounds"]
        center = [(a + b) / 2 for a, b in zip(low, high)]
        if name.startswith("stick"):
            selected.update(region(name.replace("stick_", "stick_center_")))
            # Include the rim and shaft regions from the appearance metadata.
            for i, t in enumerate(triangles):
                x, y, z = [
                    sum(t[3 + corner * 3 + axis] for corner in range(3)) / 3 for axis in range(3)
                ]
                radius = math.hypot(x - center[0], z - center[2])
                if (y > 0.0037 and radius < 0.0088) or (-0.0001 < y <= 0.0037 and radius < 0.0066):
                    selected.add(i)
            control_pivot = [center[0], -0.005, center[2]]
        else:
            control_pivot = [center[0], high[1], low[2]]
        if any(tags[i] for i in selected):
            raise ValueError("CAD analog regions overlap another control: " + name)
        for i in selected:
            tags[i] = part
        pivots.extend(control_pivot)
        descriptions.append(
            {"name": name, "part": part, "triangles": len(selected), "pivot": control_pivot}
        )
    pad_frames = []
    for part, name in enumerate(("pad_left", "pad_right"), 20):
        selected = region(name)
        if any(tags[i] for i in selected):
            raise ValueError("CAD touchpad regions overlap another control: " + name)
        for i in selected:
            tags[i] = part
        mapping = pad_mapping(triangles, selected)
        pad_frames.extend(mapping)
        descriptions.append(
            {"name": name, "part": part, "triangles": len(selected), "mapping_xz": mapping}
        )
    fingerprint = 14695981039346656037
    for t in triangles:
        for vertex in range(3):
            for byte in struct.pack("<6f", *t[3 + vertex * 3 : 6 + vertex * 3], *t[:3]):
                fingerprint = ((fingerprint ^ byte) * 1099511628211) & ((1 << 64) - 1)
    payload = (
        struct.pack("<8sIQ3f", b"CTBT0003", count, fingerprint, *pivot)
        + records
        + struct.pack("<12f", *pivots)
        + struct.pack("<12f", *pad_frames)
        + tags
    )
    destination = ROOT / "assets/controller.buttons"
    temporary = destination.with_suffix(".buttons.tmp")
    temporary.write_bytes(payload)
    temporary.replace(destination)
    details = {
        "geometry_sha256": digest,
        "geometry_unchanged": True,
        "textures_unchanged": True,
        "sha256": hashlib.sha256(payload).hexdigest(),
        "bytes": len(payload),
        "dpad_pivot": pivot,
        "parts": descriptions,
        "motion": (
            "Buttons move inward along their surface normals. "
            "The D-pad rocks 0.045 rad, triggers rotate 0.30 rad, "
            "sticks tilt 0.35 rad and click 0.65 mm. "
            "Pivots and travel are estimated from CAD."
        ),
    }
    destination.with_suffix(".buttons.json").write_text(json.dumps(details, indent=2) + "\n")
    print(
        json.dumps(
            {"built": str(destination), "bytes": len(payload), "controls": len(descriptions)}
        )
    )


if __name__ == "__main__":
    main()
