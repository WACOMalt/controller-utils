"""Assemble the hook handshake from an existing Frame factory-calibration snapshot."""

import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", type=Path)
    dataset = parser.parse_args().dataset.resolve()

    def read(path):
        return json.loads((dataset / path).read_text().replace(": nan", ": null"))

    metadata = read("snapshots/BeginningSnapshot/cameraMetadata.json")
    print(
        json.dumps(
            {
                "version": 1,
                "device": read(dataset.stem + "_device_config.json"),
                "rig": read("sensorRigCalibration.json"),
                "metadata": {"cameras": metadata["cameras"]},
            },
            separators=(",", ":"),
            allow_nan=False,
        )
    )


if __name__ == "__main__":
    main()
