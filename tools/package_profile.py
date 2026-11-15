#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path

from package_release import ROOT, public_key, read, sign


def build(source, sequence, expires, key, trust, output):
    public = public_key(key)
    if public != read(trust, 16384):
        raise ValueError("Signing key does not match the shipped update trust key")
    profile = json.loads(read(source, 16384))
    if not isinstance(profile, dict) or set(profile) != {
        "schema",
        "platform",
        "validator",
        "backend",
        "queue",
    }:
        raise ValueError("Invalid compatibility profile source")
    profile.update(sequence=sequence, expires=expires)
    document = json.dumps(profile, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    output.mkdir(parents=True, exist_ok=True)
    names = (f"compatibility-profile-{sequence}.bin",)
    if any(os.path.lexists(output / name) for name in names):
        raise ValueError("Use a new output directory")
    with tempfile.TemporaryDirectory(prefix="profile-", dir=ROOT / "build") as temporary:
        directory = Path(temporary)
        manifest, signature = directory / "profile.json", directory / "profile.sig"
        manifest.write_bytes(document)
        sign(manifest, signature, key)
        result = subprocess.check_output(
            [
                str(ROOT / "build/release_check"),
                "--profile",
                str(manifest),
                str(signature),
                str(trust),
            ],
            timeout=10,
        )
        (directory / names[0]).write_bytes(signature.read_bytes() + document)
        created = []
        try:
            for name in names:
                with (output / name).open("xb") as destination:
                    created.append(output / name)
                    destination.write((directory / name).read_bytes())
                    destination.flush()
                    os.fsync(destination.fileno())
        except BaseException:
            for path in reversed(created):
                path.unlink()
            raise
    return json.loads(result)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Package a reviewed signed compatibility profile")
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--sequence", type=int, required=True)
    parser.add_argument("--expires", type=int, required=True)
    parser.add_argument("--key", type=Path, required=True)
    parser.add_argument("--trust", type=Path, default=ROOT / "assets/update-public.pem")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(
        json.dumps(
            build(args.profile, args.sequence, args.expires, args.key, args.trust, args.output)
        )
    )
