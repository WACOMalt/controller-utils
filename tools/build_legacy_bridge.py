"""Create the v0.2.0-compatible, signed transition to the Frame updater."""

import argparse
import hashlib
import io
import json
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEGACY_ARTIFACT = "controller-utils-0.2.0-14-linuxarm64.tar.gz"
XR_SERVICE = {
    "stable": "9e1c6f57a7e3613c69dbf7f61c62d0cfd4b8a664889791907772b03a62f9f6d8",
    "beta": "2118dc52823a7c504e531eb582b26acfccb97c738befd950ad7b6bbae3e4ebc3",
}


def digest(contents):
    return hashlib.sha256(contents).hexdigest()


def signature(path, key, *, verify=False):
    if verify:
        command = [
            "openssl",
            "pkeyutl",
            "-verify",
            "-pubin",
            "-inkey",
            key,
            "-rawin",
            "-in",
            path,
            "-sigfile",
            path.with_suffix(".sig"),
        ]
    else:
        command = [
            "openssl",
            "pkeyutl",
            "-sign",
            "-inkey",
            key,
            "-rawin",
            "-in",
            path,
            "-out",
            path.with_suffix(".sig"),
        ]
    subprocess.run(command, check=True)


def arm_program(path):
    binary = path.read_bytes()
    if binary[:6] != b"\x7fELF\x02\x01" or binary[18:20] != b"\xb7\x00":
        raise ValueError("The bridge program must be Linux ARM64")
    return binary


def build(reference, destination, key):
    trust = ROOT / "assets/update-public.pem"
    manifest = reference / "latest.json"
    signature(manifest, trust, verify=True)
    prior = json.loads(manifest.read_bytes())
    source = reference / LEGACY_ARTIFACT
    payload = source.read_bytes()
    if (
        prior["schema"] != 5
        or prior["version"] != "0.2.0"
        or prior["sequence"] != 14
        or prior["artifact"] != source.name
        or prior["size"] != len(payload)
        or prior["sha256"] != digest(payload)
    ):
        raise ValueError("The transition requires the published v0.2.0 release")
    release = json.loads((ROOT / "release.json").read_bytes())
    if release != {"version": "0.3.0", "sequence": 15, "platform": "linuxarm64"}:
        raise ValueError("The legacy bridge is only for the transition to v0.3.0")
    installed_helper = "compatibility/controller-utils-updater"
    cleanup = arm_program(ROOT / "build/frame-legacy-cleanup")
    replacements = {
        "controller-utils": arm_program(ROOT / "build/frame-legacy-launcher"),
        installed_helper: cleanup,
        "release.json": json.dumps(release, separators=(",", ":")).encode(),
    }
    artifact = destination / "controller-utils-legacy-0.3.0-linuxarm64.tar.gz"
    with (
        tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as old,
        tarfile.open(artifact, mode="x:gz") as bridge,
    ):
        seen = set()
        for member in old:
            if not member.isfile() or member.name in seen:
                raise ValueError("Unexpected entry in the verified v0.2.0 package")
            seen.add(member.name)
            with old.extractfile(member) as file:
                content = file.read()
            if (
                member.name == installed_helper
                and digest(content) != prior["requirements"]["helper"]["controller-utils-updater"]
            ):
                raise ValueError("The v0.2.0 updater does not match its release")
            content = replacements.get(member.name, content)
            member.size = len(content)
            bridge.addfile(member, io.BytesIO(content))
        if not replacements.keys() <= seen:
            raise ValueError("The v0.2.0 package is incomplete")
    requirements = prior["requirements"]
    requirements["helper"]["controller-utils-updater"] = digest(cleanup)
    value = dict(
        schema=5,
        **release,
        expires=0,
        artifact=artifact.name,
        size=artifact.stat().st_size,
        sha256=digest(artifact.read_bytes()),
        requirements=requirements,
        notes="Installs shared-camera tracking for the active SteamOS channel.",
    )
    for channel, filename in (("stable", "latest-stable.json"), ("beta", "latest.json")):
        requirements["runtime"]["XRService"] = XR_SERVICE[channel]
        path = destination / filename
        path.write_text(json.dumps(value, separators=(",", ":")) + "\n")
        signature(path, key)
        signature(path, trust, verify=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--key", type=Path, required=True)
    args = parser.parse_args()
    build(args.reference, args.output, args.key)
