# Controller Utils

Tracks the 2026 Steam Controller on Steam Frame over Bluetooth. Controller Utils
reads the shared tracking-camera buffers, triangulates the controller's IR LED,
and uses a palm model to initialize heading. The controller's IMU keeps rotation
and short movements responsive between camera fixes.

The app displays a 3D controller model in VR. You can navigate Steam's UI with a
laser pointer or touchpad cursor, and type with both touchpads. Steam Controller
tracking also works while Frame's own controllers are being tracked.

![Controller Utils running on Steam Frame](docs/controller-utils.png)

The shared-camera build has separate packages for SteamOS stable and beta.
The installer checks the active channel and the SteamVR interfaces used by the
pointer. Build or update beta independently of the last working stable package.

## Install

Unpack the ARM64 package for your SteamOS channel. Run the installer as the
`steamos` user over SSH so it can finish while Steam and SteamVR restart.

```sh
python3 install.py --check --calibration /path/to/camera-calibration.json
python3 install.py --calibration /path/to/camera-calibration.json
```

The installer reuses this headset's saved camera calibration when available.
For a new profile, run `python3 app/optical/frame_camera_calibration.py` with an
existing factory-calibration snapshot directory and save its output to a JSON
file. Tracking does not start a camera recording. Fresh setups may ask for
administrator access to install the Steam Controller udev rule and keep it
across SteamOS updates.

Before upgrading from the Valve IR release, use that release's uninstaller to
remove its privileged helper and updater. Keep `app-data` and copy any camera
calibration stored inside the old app directory to a separate file first.

Pair the controller with Frame, then open Controller Utils from the app drawer (+).
The app can start with SteamVR after its first launch. Change this in App settings.

## Controls

| Input | Action |
| --- | --- |
| R4 | Toggle UI control, configurable in Controls |
| Right pad | Move the cursor with the beam hidden. Lift your thumb to restore it |
| Right trigger or right pad click | Click |
| Left trigger | Right-click |
| Right stick or left pad | Scroll |
| View + Menu, hold 1 second then release | Realign heading |
| View + Menu, hold 5 seconds | Restart tracking |
| Steam + X | Open the keyboard |
| Steam + R1 | Take a screenshot |
| Steam + L1 | Switch between the Frame and Arcturus passthrough cameras |
| Steam, hold 1 second then release | Recenter the view |
| Steam, double press | Toggle passthrough |

Steam shortcuts are enabled by default. Change this in Controls.
Camera switching requires an attached Arcturus Vision Camera and Camera Control
running from the Arcturus Vision Camera app. Choose a different UI control
binding if you use L1 for it.

With UI control on, look at the keyboard and touch either pad to type. Both pads
control the typing cursors. Typing stays active while you look away. Release both
pads to aim again.

## Updates

Install a newer package for your current channel using the same installer.
Settings and controller calibration live in
`~/.local/share/controller-utils/app-data`. A copy of the previous app and
configuration stays on the headset.

To remove Controller Utils, choose **Exit app**, then run
`python3 install.py --uninstall` from the package you installed. This removes
the app and Steam integration and keeps your settings and calibration.

## Build

The [CI workflow](.github/workflows/checks.yml) lists the compiler and library
dependencies. For ARM64 packages, use an Ubuntu 24.04 ARM64 build environment.
Build dependencies from a recent beta SteamOS install can produce a binary that
requires a newer libc than stable provides.

```sh
python3 tools/frame_sdk.py build/frame-sdk
make -j4 frame-optical CPPFLAGS=-Ibuild/frame-sdk
python3 tools/stage_frame.py --channel stable \
  --output build/controller-utils-frame-stable \
  --archive build/controller-utils-frame-stable-linuxarm64.tar.gz
```

Use `--channel beta` and a different output path to build the beta package.
The [Frame packages workflow](.github/workflows/release.yml) builds and uploads
only the selected channel's archive. App, driver and hook binaries use the same
ARM64 build baseline. Each archive records its channel, SteamOS build ID and
file checksums. The build ID records the tested SteamOS version. Use
`--build-id` to record a different tested build.

`make check` runs the desktop tests, including the shared-camera and input
regressions.

## License

Code is [MIT licensed](LICENSE). The bundled Valve controller model is
CC BY-NC-SA 4.0, so the complete bundle has noncommercial artwork restrictions.
Textures are CC BY 4.0 and the Kenney icon is CC0. See [artwork credits](assets/README.md)
and the [OpenVR license](third_party/openvr/LICENSE).

This project is not affiliated with Valve.
