"""Download and install signed Controller Utils packages for the active Frame channel."""

import fcntl
import hashlib
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath

REPO = "kallupx/controller-utils"
RELEASES = f"https://github.com/{REPO}/releases"
API = f"https://api.github.com/repos/{REPO}/releases?per_page=30"
LIMIT = 128 * 1024 * 1024
VERSION = r"[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}"


def download(url, path, maximum):
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
            "8",
            "--max-time",
            "90",
            "--max-filesize",
            str(maximum),
            "--output",
            path,
            url,
        ],
        check=True,
        stdin=subprocess.DEVNULL,
        timeout=95,
    )
    if path.stat().st_size > maximum:
        raise ValueError("Update download is too large")


def paths(channel):
    metadata = f"frame-update-{channel}"
    return metadata + ".json", metadata + ".sig"


def artifact(channel, version):
    return f"controller-utils-frame-{channel}-{version}-linuxarm64.tar.gz"


def manifest(directory, key, channel):
    metadata, signature = (directory / name for name in paths(channel))
    if not 0 < metadata.stat().st_size <= 65536 or signature.stat().st_size != 64:
        raise ValueError("Invalid update signature or metadata size")
    subprocess.run(
        [
            "/usr/bin/openssl",
            "pkeyutl",
            "-verify",
            "-pubin",
            "-inkey",
            key,
            "-rawin",
            "-in",
            metadata,
            "-sigfile",
            signature,
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        timeout=10,
    )
    value = json.loads(metadata.read_bytes())
    if (
        value.get("schema") != 1
        or value.get("channel") != channel
        or value.get("platform") != "linuxarm64"
        or not re.fullmatch(VERSION, value["version"])
        or type(value["sequence"]) is not int
        or not 0 < value["sequence"] < 2**63
        or type(value["size"]) is not int
        or not 0 < value["size"] <= LIMIT
        or value["artifact"] != artifact(channel, value["version"])
        or not re.fullmatch(r"[0-9a-f]{64}", value["sha256"])
    ):
        raise ValueError("Invalid update metadata")
    return value


def verify_archive(path, value):
    with path.open("rb") as source:
        if os.fstat(source.fileno()).st_size != value["size"]:
            raise ValueError("Update package size mismatch")
        if hashlib.file_digest(source, "sha256").hexdigest() != value["sha256"]:
            raise ValueError("Update package checksum mismatch")


def newer(value, current):
    version = tuple(map(int, value["version"].split(".")))
    installed = tuple(map(int, current["version"].split(".")))
    return value["sequence"] > current["sequence"] and version > installed


def extract(path, directory):
    entries = set()
    size = 0
    with tarfile.open(path, "r:gz") as archive:
        for member in archive:
            parts = PurePosixPath(member.name)
            if (
                parts.is_absolute()
                or ".." in parts.parts
                or not parts.parts
                or parts in entries
                or len(entries) >= 1024
                or not (member.isfile() or member.isdir())
            ):
                raise ValueError("Invalid update package contents")
            entries.add(parts)
            size += member.size
            if size > 256 * 1024 * 1024:
                raise ValueError("Update package expands beyond its size limit")
            archive.extract(member, directory, filter="data")


def choose_release(directory, channel):
    index = directory / "releases.json"
    download(API, index, 2 * 1024 * 1024)
    names = set(paths(channel))
    tags = [
        release["tag_name"]
        for release in json.loads(index.read_bytes())
        if not release["draft"]
        and re.fullmatch("v" + VERSION, release["tag_name"])
        and names <= {asset["name"] for asset in release["assets"]}
    ]
    return max(tags, key=lambda tag: tuple(map(int, tag[1:].split("."))), default=None)


def save_status(data, channel, state, message):
    with tempfile.NamedTemporaryFile(mode="w", dir=data, delete=False) as out:
        path = Path(out.name)
        try:
            json.dump(dict(channel=channel, state=state, message=message), out)
            out.flush()
            os.replace(path, data / "frame-update.json")
        finally:
            path.unlink(missing_ok=True)


def fetch_manifest(work, key, channel, tag):
    base = f"{RELEASES}/download/{tag}"
    for name, maximum in zip(paths(channel), (65536, 64)):
        download(f"{base}/{name}", work / name, maximum)
    value = manifest(work, key, channel)
    if tag != "v" + value["version"]:
        raise ValueError("Update metadata does not match its release")
    return value


def fetch_bundle(work, value):
    bundle = work / value["artifact"]
    download(f"{RELEASES}/download/v{value['version']}/{bundle.name}", bundle, value["size"])
    verify_archive(bundle, value)
    return bundle


def check(root, data, channel, current):
    pending = data / f"frame-update-{channel}"
    key = root / "assets/update-public.pem"
    save_status(data, channel, "busy", "Checking for updates…")
    with tempfile.TemporaryDirectory(prefix="frame-download-", dir=data) as temp:
        work = Path(temp)
        tag = choose_release(work, channel)
        if tag is None:
            save_status(data, channel, "idle", "No updates have been published for this channel.")
            return
        value = fetch_manifest(work, key, channel, tag)
        if not newer(value, current):
            save_status(data, channel, "idle", "You’re up to date.")
            return
        bundle = fetch_bundle(work, value)
        pending.mkdir(mode=0o700, exist_ok=True)
        for name in (bundle.name, *paths(channel)):
            os.replace(work / name, pending / name)
    save_status(data, channel, "ready", f"Version {value['version']} is ready to install.")


def install_bundle(data, channel, value, path, mode):
    verify_archive(path, value)
    save_status(data, channel, "busy", "Installing update…")
    with tempfile.TemporaryDirectory(prefix="frame-update-", dir=data) as temp:
        directory = Path(temp)
        extract(path, directory)
        package = directory / path.name.removesuffix("-linuxarm64.tar.gz")
        if json.loads((package / "app/release.json").read_bytes()) != {
            field: value[field] for field in ("version", "sequence", "platform")
        }:
            raise ValueError("Update package does not match the signed release")
        subprocess.run(
            [sys.executable, "-B", package / "install.py", mode],
            check=True,
            stdin=subprocess.DEVNULL,
        )
    path.unlink()
    save_status(data, channel, "idle", f"Installed version {value['version']}.")


def apply(root, data, channel, current):
    pending = data / f"frame-update-{channel}"
    if not (pending / paths(channel)[0]).is_file():
        check(root, data, channel, current)
        return
    value = manifest(pending, root / "assets/update-public.pem", channel)
    if not newer(value, current):
        save_status(data, channel, "idle", "You’re up to date.")
        return
    install_bundle(data, channel, value, pending / value["artifact"], "--unattended")


def migrate(root, data, channel, current):
    version = current["version"]
    if version != "0.3.0" or current["sequence"] != 15:
        raise ValueError("Unexpected legacy migration release")
    save_status(data, channel, "busy", f"Downloading version {version}…")
    with tempfile.TemporaryDirectory(prefix="frame-migration-", dir=data) as temp:
        work = Path(temp)
        value = fetch_manifest(work, root / "assets/update-public.pem", channel, f"v{version}")
        if value["sequence"] != current["sequence"]:
            raise ValueError("The migration package does not match the installed release")
        install_bundle(data, channel, value, fetch_bundle(work, value), "--legacy-migration")


def main():
    mode = sys.argv[1]
    root = Path(sys.argv[2]).resolve() if mode == "migrate" else Path(__file__).resolve().parent
    data = root.parent / "app-data"
    channel = ""
    try:
        channel = subprocess.check_output(
            ["/usr/bin/atomupd-manager", "tracked-branch"], text=True, timeout=10
        ).strip()
        if channel not in ("stable", "beta"):
            raise ValueError("Updates require SteamOS stable or beta")
        current = json.loads((root / "release.json").read_bytes())
        operation = {"check": check, "apply": apply, "migrate": migrate}[mode]
        with (data / "frame-update.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            operation(root, data, channel, current)
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        tarfile.TarError,
        subprocess.SubprocessError,
    ) as error:
        print(f"Controller Utils update: {error}", file=sys.stderr)
        message = "Couldn’t install the update. Check for updates to retry."
        if sys.argv[1] == "check":
            message = "Couldn’t check for updates. Try again later."
        save_status(data, channel, "error", message)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
