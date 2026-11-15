#!/usr/bin/env python3
"""Stage the complete ARM64 application and its native components."""

import argparse
import hashlib
import json
import os
import stat
from pathlib import Path

from package_release import ASSETS, DRIVER_FILES, HELPER_FILES, MAX_CONTENTS, PACKAGE_FILES

ROOT = Path(__file__).resolve().parents[1]


def read_source(path, limit=MAX_CONTENTS):
    parts = path.relative_to(ROOT).parts
    if not parts or any(part in (".", "..") for part in parts):
        raise ValueError("Invalid release source path")
    directory = os.open(ROOT, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        for part in parts[:-1]:
            child = os.open(
                part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=directory
            )
            os.close(directory)
            directory = child
        descriptor = os.open(
            parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=directory
        )
        with os.fdopen(descriptor, "rb") as source:
            before = os.fstat(source.fileno())
            if not stat.S_ISREG(before.st_mode) or not 0 <= before.st_size <= limit:
                raise ValueError("Invalid or oversized release source")
            contents = source.read(limit + 1)
            after = os.fstat(source.fileno())
            if (
                len(contents) != before.st_size
                or before.st_size != after.st_size
                or before.st_mtime_ns != after.st_mtime_ns
                or before.st_ctime_ns != after.st_ctime_ns
            ):
                raise ValueError("Release source changed while staging")
            return contents
    finally:
        os.close(directory)


def stage(destination):
    destination.mkdir(parents=True, exist_ok=False)
    files = {
        "controller-utils": ROOT / "build/controller-utils",
        "release.json": ROOT / "release.json",
        "LICENSE": ROOT / "LICENSE",
        "third_party/openvr/LICENSE": ROOT / "third_party/openvr/LICENSE",
        **{"assets/" + name: ROOT / "assets" / name for name in ASSETS},
        "assets/update-public.pem": ROOT / "assets/update-public.pem",
        **{"native-driver/" + name: ROOT / "build/native-driver" / name for name in DRIVER_FILES},
        **{"compatibility/" + name: ROOT / "build" / name for name in HELPER_FILES},
    }
    files.update({name: ROOT / name for name in PACKAGE_FILES})
    files["controller-utils-launcher"] = ROOT / "build/controller-utils-launcher"
    for name in ("install.sh", "uninstall.sh"):
        files[name] = ROOT / "packaging" / name
    for name, source in files.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as output:
            output.write(read_source(source))
    layout = json.loads(read_source(ROOT / "driver/native_layout.json", 65536))
    requirements = {
        "backend": "native-tracker",
        "runtime": {"XRService": layout["runtimes"]["beta"]["xr_sha256"]},
    }
    for field, directory, names in (
        ("driver", "native-driver", DRIVER_FILES),
        ("helper", "compatibility", HELPER_FILES),
    ):
        requirements[field] = {
            name: hashlib.sha256((destination / directory / name).read_bytes()).hexdigest()
            for name in names
        }
    return requirements


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--requirements", type=Path, required=True)
    args = parser.parse_args()
    args.requirements.write_text(json.dumps(stage(args.output), indent=2) + "\n")
