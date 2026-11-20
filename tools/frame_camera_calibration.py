"""Assemble the hook handshake from an existing Frame factory-calibration snapshot."""

import argparse
import json
from pathlib import Path


def read(path):
    return json.loads(path.read_text().replace(": nan", ": null"))


def factory(persist):
    device = read(persist / "device_config.json")
    rig = read(persist / "xrservice.json")
    if rig["metadata"]["device_serial"] != device["device_serial_number"]:
        raise ValueError("Factory camera rig and device configuration differ")
    if rig["metadata"]["pixel_coordinates_convention"] != "integer":
        raise ValueError("Unknown factory camera pixel convention")
    rig["metadata"]["pixel_coordinates_convention"] = "float"
    cameras = {}
    for camera in rig["cameras"]:
        name = camera["sourceCamera"]
        if name not in ("slam_left", "slam_right", "upper_left", "upper_right"):
            continue
        models = [model for model in camera["intrinsics"] if model["cameraModel"] == "kb"]
        if len(models) != 1:
            raise ValueError(f"Missing factory camera model for {name}")
        intrinsics = models[0].copy()
        camera["cameraModel"] = intrinsics.pop("cameraModel")
        intrinsics["cx"] += 0.5
        intrinsics["cy"] += 0.5
        camera["intrinsics"] = intrinsics
        cameras[name] = {"serialNumber": camera["serial"]}
    return dict(version=1, device=device, rig=rig, metadata={"cameras": cameras})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sources = parser.add_mutually_exclusive_group(required=True)
    sources.add_argument("dataset", type=Path, nargs="?")
    sources.add_argument("--factory", type=Path)
    args = parser.parse_args()
    if args.factory:
        value = factory(args.factory)
    else:
        dataset = args.dataset.resolve()
        metadata = read(dataset / "snapshots/BeginningSnapshot/cameraMetadata.json")
        value = {
            "version": 1,
            "device": read(dataset / f"{dataset.stem}_device_config.json"),
            "rig": read(dataset / "sensorRigCalibration.json"),
            "metadata": {"cameras": metadata["cameras"]},
        }
    print(json.dumps(value, separators=(",", ":"), allow_nan=False))


if __name__ == "__main__":
    main()
