#!/bin/sh
set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
umask 077

fail() { printf '%s\n' "$*" >&2; exit 1; }
[ "$(id -un)" = steamos ] && [ "$(id -u)" = 1000 ] || fail 'Run this uninstaller as steamos, without sudo.'
[ "$#" = 0 ] || fail 'Usage: ./uninstall.sh'
base=$HOME/.local/share/controllertracking
[ -d "$base/app" ] && [ ! -L "$base/app" ] || fail 'Controller Utils is not installed.'
[ "$(readlink "$base/native-driver")" = app/native-driver ] || fail 'The installed driver path needs review before removal.'
[ -x /opt/steamvr/bin/linuxarm64/vrpathreg ] || fail 'The SteamVR driver registration tool is missing.'
sudo -v || fail 'Authentication is required to uninstall Controller Utils.'
systemctl --user is-active --quiet steamvr.service || fail 'Start SteamVR before uninstalling so its app registration can be removed.'
processes=$(pgrep -u 1000 -f '^/home/steamos/.local/share/controllertracking/app/controller-utils( |$)' || true)
if [ -n "$processes" ]
then
    kill -TERM $processes
    attempts=0
    while pgrep -u 1000 -f '^/home/steamos/.local/share/controllertracking/app/controller-utils( |$)' >/dev/null
    do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 50 ] || fail 'Controller Utils is still closing. Try again shortly.'
        sleep 0.1
    done
fi
"$base/app/controller-utils" --unregister
resume=false
cleanup() {
    status=$?
    trap - EXIT
    if "$resume"; then systemctl --user start steamvr.service || status=1; fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
resume=true
systemctl --user stop steamvr.service
exec 9<> "$base/app-data/app.lock"
flock -n 9 || fail 'Close Controller Utils before uninstalling.'
sudo /var/lib/controllertracking/controller-utils-updater --uninstall
/opt/steamvr/bin/linuxarm64/vrpathreg removedriver "$base/native-driver"
systemctl --user disable controller-utils-update.service
rm -- "$HOME/.local/share/systemd/user/controller-utils-update.service"
systemctl --user daemon-reload
if [ -d "$base/app-data" ] && [ ! -L "$base/app-data" ]; then
    rm -f -- "$base/app-data/startup-recovery"
fi
rm -- "$base/native-driver" "$base/controller-utils"
rm -rf -- "$base/app"
printf '%s\n' 'Controller Utils was removed.' "Settings and calibration were kept in $base/app-data."
