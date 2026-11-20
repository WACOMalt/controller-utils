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
python3 install.py --check
python3 install.py
```

The installer reuses saved camera calibration or reads factory calibration
from the headset. Use `--calibration /path/to/camera-calibration.json` to supply
a file. Tracking does not start a camera recording. Fresh setups may ask for
administrator access to install the Steam Controller udev rule and keep it
across SteamOS updates.

With automatic updates enabled, the published v0.2.0 release downloads the
signed transition on supported stable and beta builds. It installs v0.3.0
when SteamVR restarts and Controller Utils starts. The update retires the old
services and sudo rules without prompting for administrator access. Settings
and controller calibration stay in `app-data`.

If v0.2.0 rejects the update after a SteamOS change, run the installer above
over SSH and approve its one-time administrator prompt. This covers older
SteamOS builds whose XRService fingerprint differs from the migration feed.

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

Open **App** and select **Check for updates**. When a signed package is ready,
select **Install update**. With **Automatic updates** enabled, Controller Utils
checks in the background and installs when the Frame has been idle for 30
seconds. Installation briefly restarts Steam and SteamVR.

Each update matches the Frame’s current stable or beta channel. You can also
run the installer for your channel over SSH. A copy of the previous app and
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
