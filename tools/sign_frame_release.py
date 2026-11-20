"""Sign a staged Frame package using the existing release signing key."""

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sign(bundle, key, channel):
    release = json.loads((ROOT / "release.json").read_bytes())
    version = release["version"]
    expected = f"controller-utils-frame-{channel}-{version}-linuxarm64.tar.gz"
    if bundle.name != expected or not re.fullmatch(r"[0-9]{1,6}(\.[0-9]{1,6}){2}", version):
        raise ValueError("Frame archive does not match release.json")
    path = bundle.parent / f"frame-update-{channel}.json"
    signature = path.with_suffix(".sig")
    with bundle.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    path.write_text(
        json.dumps(
            dict(
                schema=1,
                **release,
                channel=channel,
                artifact=bundle.name,
                size=bundle.stat().st_size,
                sha256=digest,
            ),
            separators=(",", ":"),
        )
        + "\n"
    )
    subprocess.run(
        ["openssl", "pkeyutl", "-sign", "-inkey", key, "-rawin", "-in", path, "-out", signature],
        check=True,
    )
    subprocess.run(
        [
            "openssl",
            "pkeyutl",
            "-verify",
            "-pubin",
            "-inkey",
            ROOT / "assets/update-public.pem",
            "-rawin",
            "-in",
            path,
            "-sigfile",
            signature,
        ],
        check=True,
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", choices=("stable", "beta"), required=True)
    parser.add_argument("--key", type=Path, required=True)
    parser.add_argument("bundle", type=Path)
    args = parser.parse_args()
    sign(args.bundle, args.key, args.channel)
