#!/usr/bin/env python3
"""Verify a release before executing any code from its archive."""

import argparse
import hashlib
import json
import os
import re
import signal
import stat
import subprocess
import tarfile
import tempfile
import time
from pathlib import Path, PurePosixPath

PUBLIC_KEY = "-----BEGIN PUBLIC KEY-----\nMCowBQYDK2VwAyEAgOeA07dV3lAIKraJnLObYbJJTd7F/n6YTLvCmnq4Xfo=\n-----END PUBLIC KEY-----\n"
RELEASES = "https://github.com/kallupx/controller-utils/releases/"
MAX_ARCHIVE = 128 * 1024 * 1024
MAX_CONTENTS = 256 * 1024 * 1024


def regular(path, limit):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    stream = os.fdopen(descriptor, "rb")
    info = os.fstat(descriptor)
    if not stat.S_ISREG(info.st_mode) or not 0 <= info.st_size <= limit:
        stream.close()
        raise ValueError("Expected a bounded regular release file")
    return stream


def unique_object(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError("Duplicate release metadata field")
        result[key] = value
    return result


def verify_metadata(feed, work, public_key=PUBLIC_KEY):
    work.mkdir(mode=0o700, parents=True, exist_ok=False)
    for name, limit in (("latest.json", 65536), ("latest.sig", 64)):
        with regular(feed / name, limit) as source:
            data = source.read(limit + 1)
        if len(data) > limit:
            raise ValueError("Release metadata exceeded its limit")
        (work / name).write_bytes(data)
    (work / "public.pem").write_text(public_key)
    subprocess.run(
        [
            "/usr/bin/openssl",
            "pkeyutl",
            "-verify",
            "-rawin",
            "-pubin",
            "-inkey",
            str(work / "public.pem"),
            "-in",
            str(work / "latest.json"),
            "-sigfile",
            str(work / "latest.sig"),
        ],
        check=True,
        timeout=5,
        capture_output=True,
    )
    metadata = json.loads((work / "latest.json").read_bytes(), object_pairs_hook=unique_object)
    artifact, size, digest = metadata["artifact"], metadata["size"], metadata["sha256"]
    expiry = metadata["expires"]
    if (
        metadata["schema"] != 5
        or metadata["platform"] != "linuxarm64"
        or not isinstance(artifact, str)
        or not re.fullmatch(r"controller-utils-[A-Za-z0-9_.-]+-linuxarm64\.tar\.gz", artifact)
        or type(size) is not int
        or not 0 < size <= MAX_ARCHIVE
        or not isinstance(digest, str)
        or not re.fullmatch(r"[a-f0-9]{64}", digest)
        or type(expiry) is not int
        or expiry < 0
        or (expiry and expiry <= time.time())
    ):
        raise ValueError("Invalid or expired release metadata")
    return metadata


def verify_package(feed, work, public_key=PUBLIC_KEY):
    metadata = verify_metadata(feed, work, public_key)
    artifact, size, digest = metadata["artifact"], metadata["size"], metadata["sha256"]
    hashed = hashlib.sha256()
    copied = 0
    with regular(feed / artifact, MAX_ARCHIVE) as source, (work / artifact).open("xb") as output:
        while chunk := source.read(1024 * 1024):
            copied += len(chunk)
            if copied > size:
                raise ValueError("Release archive exceeded its signed size")
            output.write(chunk)
            hashed.update(chunk)
    if copied != size or hashed.hexdigest() != digest:
        raise ValueError("Release archive checksum mismatch")
    destination = work / "installer"
    destination.mkdir(mode=0o700)
    seen, total = set(), 0
    with tarfile.open(work / artifact, "r:gz") as archive:
        for member in archive:
            name = PurePosixPath(member.name)
            total += member.size
            if (
                not member.isfile()
                or name.is_absolute()
                or ".." in name.parts
                or str(name) != member.name
                or member.name in seen
                or len(seen) >= 1024
                or total > MAX_CONTENTS
            ):
                raise ValueError("Invalid release archive member")
            seen.add(member.name)
            path = destination / member.name
            path.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as source, path.open("xb") as output:
                while chunk := source.read(1024 * 1024):
                    output.write(chunk)
            path.chmod(0o755 if member.mode & 0o111 else 0o644)
    if not (destination / "install.sh").is_file():
        raise ValueError("Release installer is missing")
    return destination


def download(url, destination, limit):
    subprocess.run(
        [
            "/usr/bin/curl",
            "--fail",
            "--silent",
            "--show-error",
            "--location",
            "--proto",
            "=https",
            "--proto-redir",
            "=https",
            "--connect-timeout",
            "10",
            "--max-time",
            "180",
            "--max-filesize",
            str(limit),
            "--output",
            str(destination),
            url,
        ],
        check=True,
        timeout=190,
    )


def steam_os_channel():
    values = []
    for option in ("tracked-variant", "tracked-branch"):
        result = subprocess.run(
            ["/usr/bin/atomupd-manager", option],
            check=True,
            capture_output=True,
            text=True,
            timeout=3,
        )
        values.append(result.stdout.strip())
    variant, branch = values
    if variant != "vr" or branch not in ("stable", "beta"):
        raise ValueError("Controller Utils supports Steam Frame's stable and beta OS channels.")
    return branch


def download_release(feed, public_key=PUBLIC_KEY):
    stable = steam_os_channel() == "stable"
    manifest = "latest-stable.json" if stable else "latest.json"
    signature = "latest-stable.sig" if stable else "latest.sig"
    feed.mkdir(mode=0o700)
    print("Downloading the latest Controller Utils release...", flush=True)
    download(RELEASES + "latest/download/" + manifest, feed / "latest.json", 65536)
    with regular(feed / "latest.json", 65536) as source:
        version = json.load(source, object_pairs_hook=unique_object)["version"]
    if not isinstance(version, str) or not re.fullmatch(
        r"[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}", version
    ):
        raise ValueError("Invalid release version")
    release_url = RELEASES + "download/v" + version + "/"
    download(release_url + signature, feed / "latest.sig", 64)
    metadata = verify_metadata(feed, feed / "authenticated", public_key)
    download(release_url + metadata["artifact"], feed / metadata["artifact"], metadata["size"])
    return feed


def exit_on_signal(signum, _frame):
    raise SystemExit(128 + signum)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cleanup", action="store_true")
    parser.add_argument("release_directory", type=Path, nargs="?")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="controller-utils-bootstrap-") as temporary:
        feed = (
            args.release_directory.resolve()
            if args.release_directory is not None
            else download_release(Path(temporary) / "release")
        )
        work = Path(temporary) / "verified"
        installer = verify_package(feed, work)
        print("Release verified. Starting installation...", flush=True)
        command = ["/bin/sh", str(installer / "install.sh")]
        if args.cleanup:
            command.append("--cleanup")
        command.append(str(work))
        return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    signal.signal(signal.SIGHUP, exit_on_signal)
    signal.signal(signal.SIGTERM, exit_on_signal)
    raise SystemExit(main())
