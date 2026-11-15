#!/bin/sh
set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
umask 077

fail() { printf '%s\n' "$*" >&2; exit 1; }
[ "$(id -un)" = steamos ] && [ "$(id -u)" = 1000 ] || fail 'Run this installer as steamos, without sudo.'
[ "$(uname -m)" = aarch64 ] || fail 'Controller Utils requires Steam Frame ARM64.'
recover=false
if [ "$#" = 2 ] && [ "$1" = --cleanup ]; then
    recover=true
    shift
fi
[ "$#" = 1 ] || fail 'Usage: ./install.sh [--cleanup] DIRECTORY_WITH_RELEASE_FILES'
feed=$(realpath -e -- "$1")
source_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
base=$HOME/.local/share/controllertracking
[ ! -e "$base/app" ] && [ ! -L "$base/app" ] || fail 'Controller Utils is already installed. Use its updates or uninstall it first.'
[ ! -e "$base/controller-utils" ] && [ ! -L "$base/controller-utils" ] || fail 'An existing Controller Utils launcher needs review before installation.'
[ ! -e "$base/native-driver" ] && [ ! -L "$base/native-driver" ] || fail 'An existing Controller Utils driver needs review before installation.'
[ -x /opt/steamvr/bin/linuxarm64/vrpathreg ] || fail 'The SteamVR driver registration tool is missing.'
mkdir -p "$base/app-data"
service=$HOME/.local/share/systemd/user/controller-utils-update.service
[ ! -e "$service" ] && [ ! -L "$service" ] || fail 'An existing update service needs review before installation.'
work=$(mktemp -d "$base/.install.XXXXXXXX")
resume=false
complete=false
app_installed=false
launcher_installed=false
service_installed=false
driver_added=false
native_installed=false
native_preexisting=false
[ ! -x /var/lib/controllertracking/controller-utils-updater ] || native_preexisting=true
cleanup() {
    status=$?
    trap - EXIT
    preserve=false
    if ! "$complete"
    then
        if "$native_installed" && ! "$native_preexisting"; then
            if ! sudo -n "$work/setup-updater" --uninstall; then
                preserve=true
                printf '%s\n' 'Native cleanup failed. Run the verified installer with --cleanup and the release directory.' >&2
            fi
        fi
        if "$driver_added"; then /opt/steamvr/bin/linuxarm64/vrpathreg removedriver "$base/native-driver" || true; fi
        if "$service_installed"
        then
            systemctl --user disable controller-utils-update.service || true
            rm -f -- "$service"
            systemctl --user daemon-reload || true
        fi
        if "$app_installed"; then rm -rf -- "$base/app"; rm -f -- "$base/native-driver"; fi
        if "$launcher_installed"; then rm -f -- "$base/controller-utils"; fi
        printf '%s\n' 'Installation did not finish. Resolve the reported error and rerun the same installer.' >&2
    fi
    if "$preserve"; then
        printf 'Verified recovery files kept in %s\n' "$work" >&2
    else
        rm -rf -- "$work"
    fi
    if "$resume"; then systemctl --user start steamvr.service || status=1; fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
"$source_dir/compatibility/controller-utils-updater" --stage "$feed" "$work/package"
install -m 0755 "$work/package/compatibility/controller-utils-updater" "$work/setup-updater"
if systemctl --user is-active --quiet steamvr.service
then
    resume=true
    systemctl --user stop steamvr.service
fi
exec 9<> "$base/app-data/app.lock"
flock -n 9 || fail 'Close Controller Utils before installing.'
if "$recover"; then
    sudo "$work/setup-updater" --uninstall
    complete=true
    printf '%s\n' 'Incomplete native setup was removed. You can install a new release.'
    exit 0
fi
sudo "$work/setup-updater" --install "$feed"
native_installed=true
rm -rf -- "$work/package/compatibility"
launcher_installed=true
install -m 0755 "$work/package/controller-utils-launcher" "$base/controller-utils"
mv -- "$work/package" "$base/app"
app_installed=true
ln -s app/native-driver "$base/native-driver"
mkdir -p "$HOME/.local/share/systemd/user"
service_installed=true
install -m 0644 "$base/app/packaging/controller-utils-update.service" "$service"
systemctl --user daemon-reload
systemctl --user enable controller-utils-update.service
driver_added=true
/opt/steamvr/bin/linuxarm64/vrpathreg adddriver "$base/native-driver"
flock -u 9
"$base/app/controller-utils" --prepare-registration
complete=true
printf '%s\n' 'Controller Utils is installed. Open it from the desktop app launcher with SteamVR running.'
