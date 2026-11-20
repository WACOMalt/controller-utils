#!/usr/bin/env python3
"""Install the pinned ONNX Runtime headers used by the Frame palm worker."""

import hashlib
import io
import sys
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

VERSION = "1.24.1"
DIGEST = "5fca4421974b450711520f17a27e527fec60a78c2b6f9db3b05c159d9b5a346d"
PACKAGE = "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/"


def install(destination):
    url = f"{PACKAGE}{VERSION}/microsoft.ml.onnxruntime.{VERSION}.nupkg"
    with urllib.request.urlopen(url, timeout=60) as source:
        data = source.read(128 * 1024 * 1024)
    if hashlib.sha256(data).hexdigest() != DIGEST:
        raise ValueError("ONNX Runtime SDK package checksum mismatch")
    destination.mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for entry in archive.infolist():
            path = PurePosixPath(entry.filename)
            if path.parent == PurePosixPath("build/native/include") and path.suffix == ".h":
                (destination / path.name).write_bytes(archive.read(entry))


if __name__ == "__main__":
    install(Path(sys.argv[1]))
