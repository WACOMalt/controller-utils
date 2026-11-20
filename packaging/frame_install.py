#!/usr/bin/env python3
"""Install a shared-camera package for the active Steam Frame channel."""

import argparse
import fcntl
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path, PurePosixPath

VR = Path("/opt/steamvr/bin/linuxarm64")
PALM = Path(
    "/opt/steamvr/drivers/cv/resources/minControllerFeatureNNModel/K2V3/"
    "2024-12-13_16-02_StereoSQRT2-half-WithLongSleevesSynthetic_ch199_partial_AU_WU.onnx"
)
BASE = Path.home() / ".local/share/controller-utils"
UNITS = Path.home() / ".local/share/systemd/user"
RULE = Path("/etc/udev/rules.d/70-controller-utils.rules")
NUMPY = "2.5.5"
NUMPY_WHEELS = (
    "a0750f2156d53446caca85663d0b65d67214e1664b9050454a731ea1418d176a",
    "ae2b8285d0b4eed24739c3ca9ec0fa424a0247489c01e1d5fe2815a599e11d33",
    "cc6eb7502facce90349b4097a26821af224aaa0ab36e78304711cdb544352882",
    "e99bf8a3d54693f2063fa6c7d13955d460a9f543d9dbc80e69a92da288259b28",
)


def command(*args, capture=False, check=True):
    return subprocess.run(args, check=check, text=True, capture_output=capture, timeout=180)


def systemctl(*args, check=True):
    return command("systemctl", "--user", *args, check=check)


def verify_package(package):
    record = json.loads((package / "frame-package.json").read_bytes())
    if (
        record["schema"] != 1
        or record["channel"] not in ("stable", "beta")
        or not re.fullmatch(r"[0-9]{8}\.[0-9]+", record["steamos_build"])
    ):
        raise ValueError("Invalid Frame package")
    files = record["files"]
    actual = set()
    for file in package.rglob("*"):
        name = file.relative_to(package).as_posix()
        if file.is_symlink() or not (file.is_file() or file.is_dir()):
            raise ValueError(f"Unexpected package entry {name}")
        if file.is_file() and name != "frame-package.json":
            actual.add(name)
    if actual != set(files):
        raise ValueError("Files do not match the package manifest")
    for name, digest in files.items():
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts:
            raise ValueError("Invalid package path")
        with (package / name).open("rb") as source:
            if hashlib.file_digest(source, "sha256").hexdigest() != digest:
                raise ValueError(f"Package checksum failed for {name}")
    return record["channel"], record["steamos_build"]


def calibration(source):
    choices = (
        BASE / "app-data/camera-calibration.json",
        BASE / "app/optical/camera-calibration.json",
    )
    if source is None:
        source = next((path for path in choices if path.is_file()), None)
    if source is None:
        raise ValueError("Supply this headset's camera calibration with --calibration")
    contents = source.read_bytes()
    if not 0 < len(contents) <= 131072:
        raise ValueError("Invalid calibration size")
    value = json.loads(contents)
    if value["version"] != 1 or not isinstance(value["rig"]["cameras"], list):
        raise ValueError("Invalid Frame camera calibration")
    for name in ("slam_left", "slam_right", "upper_left", "upper_right"):
        serial = value["metadata"]["cameras"][name]["serialNumber"]
        if not serial or not serial.isalnum() or not serial.isupper() and not serial.isdigit():
            raise ValueError(f"Invalid camera serial for {name}")
    return json.dumps(value, separators=(",", ":"), allow_nan=False) + "\n"


def start_services():
    systemctl("start", "steamvr.service")
    systemctl("reset-failed", "steam.service", check=False)
    systemctl("start", "steam.service")


def stop_services(*, check=True):
    # Steam wants SteamVR and can queue a competing start during shutdown.
    systemctl(
        "--job-mode=replace-irreversibly", "stop", "steam.service", "steamvr.service", check=check
    )


def ready():
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
    expected = {
        f"@controller-utils.{name}.v1.{os.getuid()}" for name in ("optical", "input", "beam")
    } | {str(runtime / "controller-utils-camera.sock")}
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        entries = (
            line.split(maxsplit=7) for line in Path("/proc/net/unix").read_text().splitlines()
        )
        listening = {parts[7] for parts in entries if len(parts) == 8 and parts[3] == "00010000"}
        if expected <= listening:
            return
        time.sleep(0.2)
    raise RuntimeError("SteamVR did not start all Controller Utils connections")


def atomic_text(path, content, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, mode="w", delete=False) as out:
        temporary = Path(out.name)
        try:
            out.write(content)
            out.flush()
            os.fchmod(out.fileno(), mode)
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


def numpy_environment(data):
    target = data / f"numpy-{NUMPY}-{sys.implementation.cache_tag}"
    python = target / "bin/python"
    imports = f"import numpy; assert numpy.__version__ == '{NUMPY}'"
    if python.is_file() and command(python, "-I", "-c", imports, check=False).returncode == 0:
        return target
    if target.exists():
        raise RuntimeError(f"Move the incomplete Python environment aside at {target}")
    with tempfile.TemporaryDirectory(prefix="numpy-", dir=data) as directory:
        directory = Path(directory)
        trial = directory / "venv"
        requirement = directory / "requirements.txt"
        hashes = " ".join(f"--hash=sha256:{digest}" for digest in NUMPY_WHEELS)
        requirement.write_text(f"numpy=={NUMPY} {hashes}\n")
        command(sys.executable, "-m", "venv", trial)
        command(
            trial / "bin/python",
            "-m",
            "pip",
            "--isolated",
            "install",
            "--index-url",
            "https://pypi.org/simple",
            "--only-binary=:all:",
            "--no-deps",
            "--require-hashes",
            "-r",
            requirement,
        )
        command(trial / "bin/python", "-I", "-c", imports)
        trial.rename(target)
    return target


def install(package, camera_config):
    data, app = BASE / "app-data", BASE / "app"
    data.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (data / "frame-install.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        python = numpy_environment(data)
        if RULE.is_symlink() or (
            RULE.exists() and RULE.read_bytes() != (package / RULE.name).read_bytes()
        ):
            raise ValueError("Existing Controller Utils udev rule differs from the package")
        if not RULE.exists():
            command("sudo", "install", "-m", "0644", package / RULE.name, RULE)
            command("sudo", "udevadm", "control", "--reload-rules")
            command("sudo", "udevadm", "trigger", "--subsystem-match=hidraw")
        driver = BASE / "native-driver"
        if (driver.exists() or driver.is_symlink()) and (
            not driver.is_symlink() or os.readlink(driver) != "app/native-driver"
        ):
            raise ValueError("Existing driver path belongs to another installation")
        if app.is_symlink():
            raise ValueError("Existing app directory is a symlink")
        backup = Path(tempfile.mkdtemp(prefix="previous-frame-", dir=BASE))
        stage = backup / "replacement"
        shutil.copytree(package / "app", stage)
        (stage / "optical/venv").symlink_to(python, target_is_directory=True)
        launcher = BASE / "controller-utils"
        config = data / "camera-calibration.json"
        camera_env = data / "camera-hook.env"
        input_env = data / "input-hook.env"
        camera_drop = UNITS / "steamvr.service.d/90-controller-utils-camera.conf"
        input_drop = UNITS / "steam.service.d/90-controller-utils-input.conf"
        drop_ins = {
            camera_drop: f"[Service]\nEnvironmentFile=-{camera_env}\n",
            input_drop: f"[Service]\nEnvironmentFile=-{input_env}\n",
        }
        managed = {launcher, config, camera_env, input_env, *drop_ins}
        old = {}
        for item in managed:
            if item.is_symlink() or (item.exists() and not item.is_file()):
                raise ValueError(f"Unsafe setup path {item}")
            if item.is_file():
                saved = backup / item.name
                shutil.copy2(item, saved)
                old[item] = saved
        updater = "controller-utils-update.service"
        legacy_enabled = systemctl("is-enabled", "--quiet", updater, check=False).returncode == 0
        moved = installed = driver_created = False
        try:
            if legacy_enabled:
                systemctl("disable", updater)
            stop_services()
            if app.exists():
                app.rename(backup / "app")
                moved = True
            stage.rename(app)
            installed = True
            shutil.copy2(package / "controller-utils", backup / "launcher-next")
            os.replace(backup / "launcher-next", launcher)
            atomic_text(config, camera_config, 0o600)
            optical = app / "optical"
            atomic_text(
                camera_env,
                f"LD_PRELOAD={optical}/frame_camera_hook.so:{optical}/frame_input_hook.so\n"
                f"CONTROLLER_UTILS_CAMERA_CALIBRATION={config}\n",
                0o600,
            )
            atomic_text(input_env, f"LD_PRELOAD={optical}/frame_input_hook.so\n", 0o600)
            for path, contents in drop_ins.items():
                atomic_text(path, contents, 0o644)
            if not driver.is_symlink():
                driver.symlink_to("app/native-driver", target_is_directory=True)
                driver_created = True
            command(VR / "vrpathreg", "adddriver", driver)
            command(app / "controller-utils", "--prepare-registration")
            systemctl("daemon-reload")
            start_services()
            ready()
            if not moved:
                command(app / "controller-utils", "--register")
        except BaseException:
            stop_services(check=False)
            if installed:
                app.rename(backup / "failed")
            if moved:
                (backup / "app").rename(app)
            if driver_created:
                command(VR / "vrpathreg", "removedriver", driver, check=False)
                driver.unlink()
            for item in managed:
                if item in old:
                    os.replace(old[item], item)
                else:
                    item.unlink(missing_ok=True)
            if legacy_enabled:
                systemctl("enable", updater, check=False)
            systemctl("daemon-reload")
            start_services()
            raise
    print(f"Controller Utils installed. Previous files are in {backup}.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--calibration", type=Path)
    parser.add_argument("--check", action="store_true", help="Check the Frame without installing")
    args = parser.parse_args()
    if os.geteuid() == 0:
        raise ValueError("Run this installer as the Steam Frame user")
    package = Path(__file__).resolve().parent
    channel, build_id = verify_package(package)
    branch = command("atomupd-manager", "tracked-branch", capture=True).stdout.strip()
    variant = command("atomupd-manager", "tracked-variant", capture=True).stdout.strip()
    if platform.machine() != "aarch64" or variant != "vr" or branch != channel:
        raise ValueError(f"This package requires Steam Frame's {channel} channel")
    installed = {
        key: value.strip('"')
        for key, _, value in (
            line.partition("=") for line in Path("/etc/os-release").read_text().splitlines()
        )
    }
    if installed.get("BUILD_ID") != build_id:
        print(
            f"Package tested on SteamOS {build_id}. Running {installed.get('BUILD_ID', 'unknown')}."
        )
    camera_config = calibration(args.calibration)
    command(package / "frame-compatibility")
    subprocess.run(
        [package / "app/optical/frame_palm_inference", PALM],
        stdin=subprocess.DEVNULL,
        check=True,
        timeout=30,
    )
    if args.check:
        print(f"Frame {channel} package preflight passed")
    else:
        install(package, camera_config)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
