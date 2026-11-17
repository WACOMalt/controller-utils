#!/bin/sh
set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
umask 077

fail() { printf '%s\n' "$*" >&2; exit 1; }
[ "$(id -un)" = steamos ] && [ "$(id -u)" = 1000 ] || fail 'Run this installer as steamos, without sudo.'
[ "$(uname -m)" = aarch64 ] || fail 'Controller Utils requires Steam Frame ARM64.'
worker=false
if [ "${1:-}" = --worker ]; then
    worker=true
    shift
fi
remove=false
if [ "${1:-}" = --uninstall ]; then
    remove=true
    shift
fi
recover=false
if [ "$#" = 2 ] && [ "$1" = --cleanup ]; then
    recover=true
    shift
fi
if "$remove"; then
    [ "$#" = 0 ] && ! "$recover" || fail 'Usage: ./uninstall.sh'
else
    [ "$#" = 1 ] || fail 'Usage: ./install.sh [--cleanup] DIRECTORY_WITH_RELEASE_FILES'
    feed=$(realpath -e -- "$1")
fi
source_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
base=$HOME/.local/share/controller-utils
service=$HOME/.local/share/systemd/user/controller-utils-update.service
if "$remove"; then
    [ -d "$base/app" ] && [ ! -L "$base/app" ] || fail 'Controller Utils is not installed.'
    [ "$(readlink "$base/native-driver")" = app/native-driver ] || fail 'The installed driver path needs review before removal.'
else
    [ ! -e "$base/app" ] && [ ! -L "$base/app" ] || fail 'Controller Utils is already installed. Use its updates or uninstall it first.'
    [ ! -e "$base/controller-utils" ] && [ ! -L "$base/controller-utils" ] || fail 'An existing Controller Utils launcher needs review before installation.'
    [ ! -e "$base/native-driver" ] && [ ! -L "$base/native-driver" ] || fail 'An existing Controller Utils driver needs review before installation.'
    legacy=$HOME/.local/share/controllertracking
    for path in "$legacy/app" "$legacy/controller-utils" "$legacy/native-driver" /var/lib/controllertracking
    do
        [ ! -e "$path" ] && [ ! -L "$path" ] || fail "Uninstall the previous version first: $legacy/app/uninstall.sh"
    done
    [ ! -e "$service" ] && [ ! -L "$service" ] || fail 'An existing update service needs review before installation.'
fi
[ -x /opt/steamvr/bin/linuxarm64/vrpathreg ] || fail 'The SteamVR driver registration tool is missing.'
XDG_RUNTIME_DIR=/run/user/$(id -u)
export XDG_RUNTIME_DIR
unset DBUS_SESSION_BUS_ADDRESS
systemctl --user show-environment >/dev/null 2>&1 || fail 'Cannot connect to the SteamOS user session. Open Konsole on Frame and try again.'
if ! "$worker"; then
    [ -x /usr/bin/tmux ] && [ -x /usr/bin/systemd-run ] || fail 'SteamOS setup session tools are missing.'
    if [ -t 0 ]; then
        exec 3<&0
    elif [ -t 1 ]; then
        exec 3<&1
    elif [ -t 2 ]; then
        exec 3<&2
    else
        fail 'Open a terminal on Frame or connect using ssh -t before running setup.'
    fi
    terminal=$(tty <&3) || fail 'Could not locate the setup terminal.'
    exec 3<&-
    printf '%s' 'Setup will close the desktop and restart SteamVR. Continue? [y/N] ' >"$terminal"
    IFS= read -r answer <"$terminal" || answer=
    case "$answer" in
        y|Y|yes|YES|Yes) ;;
        *) printf '%s\n' 'Setup canceled.'; exit 0 ;;
    esac
    mkdir -p "$base/app-data"
    job=$(mktemp -d "$base/app-data/.setup.XXXXXXXX")
    mkdir "$job/source"
    cp -- "$source_dir/install.sh" "$job/source/install.sh"
    if "$remove"; then
        set -- "$job/source/install.sh" --worker --uninstall
    else
        mkdir "$job/source/compatibility" "$job/feed"
        cp -- "$source_dir/compatibility/controller-utils-updater" "$job/source/compatibility/"
        cp -- "$feed/latest.json" "$feed/latest.sig" "$feed"/controller-utils-*-linuxarm64.tar.gz "$job/feed/"
        if "$recover"; then
            set -- "$job/source/install.sh" --worker --cleanup "$job/feed"
        else
            set -- "$job/source/install.sh" --worker "$job/feed"
        fi
    fi
    printf 'Setup will continue if the desktop closes. Log: %s\n' "$base/app-data/setup.log"
    unit=controller-utils-setup-${job##*.}
    systemd-run --user --scope --collect --quiet --expand-environment=no --unit="$unit" \
        /usr/bin/env -u TMUX /usr/bin/tmux -f /dev/null -S "$job/socket" new-session -d -s setup \
        /bin/bash -c '
            job=$1
            shift
            exec 8<>"$job/../setup.lock"
            if ! flock -n 8; then
                printf "%s\n" "Another Controller Utils setup is running." >"$job/output.log"
                printf "1\n" >"$job/status.next"
                mv -- "$job/status.next" "$job/status"
                exit 1
            fi
            /bin/sh "$@" 2>&1 | tee "$job/output.log" "$job/../setup.log"
            status=${PIPESTATUS[0]}
            printf "%s\n" "$status" >"$job/status.next"
            mv -- "$job/status.next" "$job/status"
            rm -rf -- "$job/source" "$job/feed"
            exit "$status"
        ' setup "$job" "$@" || fail "Could not start setup. Check $job/output.log."
    /usr/bin/env -u TMUX /usr/bin/tmux -S "$job/socket" attach-session -t setup <>"$terminal" >"$terminal" 2>&1 || true
    if [ -f "$job/status" ]; then
        status=$(cat "$job/status")
        [ "$status" = 0 ] || cat "$job/output.log" >&2
        rm -rf -- "$job"
        exit "$status"
    fi
    systemctl --user is-active --quiet "$unit.scope" || fail "Setup stopped before finishing. Check $base/app-data/setup.log."
    printf 'Setup is still running. Log: %s\n' "$base/app-data/setup.log"
    exit 0
fi
if "$remove"; then
    sudo -v || fail 'Authentication is required to uninstall Controller Utils.'
    systemctl --user is-active --quiet steamvr.service || fail 'Start SteamVR before uninstalling so its app registration can be removed.'
    load_state=$(systemctl --user show steamvr.service -p LoadState --value) || fail 'Cannot inspect the SteamVR service. Removal was stopped.'
    [ "$load_state" != masked ] || fail 'SteamVR is masked. Unmask it before uninstalling.'
    processes=$(pgrep -u 1000 -f '^/home/steamos/.local/share/controller-utils/app/controller-utils( |$)' || true)
    if [ -n "$processes" ]; then
        kill -TERM $processes
        attempts=0
        while pgrep -u 1000 -f '^/home/steamos/.local/share/controller-utils/app/controller-utils( |$)' >/dev/null; do
            attempts=$((attempts + 1))
            [ "$attempts" -lt 50 ] || fail 'Controller Utils is still closing. Try again shortly.'
            sleep 0.1
        done
    fi
    "$base/app/controller-utils" --unregister
    complete=false
    masked=false
    locked=false
    cleanup() {
        status=$?
        trap - EXIT
        if "$locked"; then flock -u 9 || status=1; fi
        if "$masked"; then systemctl --user unmask --runtime steamvr.service || status=1; fi
        if ! systemctl --user start steamvr.service && ! systemctl --user is-active --quiet steamvr.service; then
            printf '%s\n' 'SteamVR did not finish restarting. Restart Frame if VR does not return.' >&2
        fi
        if ! "$complete"; then
            "$base/app/controller-utils" --prepare-registration || status=1
        fi
        exit "$status"
    }
    trap cleanup EXIT
    trap 'exit 1' HUP INT TERM
    masked=true
    systemctl --user mask --runtime steamvr.service
    systemctl --user stop steamvr.service
    exec 9<> "$base/app-data/app.lock"
    flock -n 9 || fail 'Close Controller Utils before uninstalling.'
    locked=true
    sudo /var/lib/controller-utils/controller-utils-updater --uninstall
    /opt/steamvr/bin/linuxarm64/vrpathreg removedriver "$base/native-driver"
    systemctl --user disable controller-utils-update.service
    rm -- "$service"
    systemctl --user daemon-reload
    if [ -d "$base/app-data" ] && [ ! -L "$base/app-data" ]; then
        rm -f -- "$base/app-data/startup-recovery"
    fi
    rm -- "$base/native-driver" "$base/controller-utils"
    rm -rf -- "$base/app"
    complete=true
    printf '%s\n' 'Controller Utils was removed.' "Settings and calibration were kept in $base/app-data."
    exit 0
fi
if [ -e "$legacy/app-data" ] || [ -L "$legacy/app-data" ]
then
    [ -d "$legacy" ] && [ ! -L "$legacy" ] && [ -d "$legacy/app-data" ] && [ ! -L "$legacy/app-data" ] || fail 'The previous settings directory needs review before installation.'
    [ ! -e "$base/app-data" ] && [ ! -L "$base/app-data" ] || fail 'Both old and new settings directories exist. Preserve and review them before installing.'
    mkdir -p "$base"
    mv -T -- "$legacy/app-data" "$base/app-data"
fi
mkdir -p "$base/app-data"
work=$(mktemp -d "$base/.install.XXXXXXXX")
resume=false
masked=false
complete=false
app_installed=false
launcher_installed=false
service_installed=false
driver_added=false
native_installed=false
native_preexisting=false
[ ! -x /var/lib/controller-utils/controller-utils-updater ] || native_preexisting=true
cleanup() {
    status=$?
    trap - EXIT
    preserve=false
    if ! "$complete"
    then
        if "$native_installed" && ! "$native_preexisting"; then
            if ! sudo -n "$work/setup-updater" --uninstall; then
                preserve=true
                printf '%s\n' 'Native cleanup failed. Run the install command with --cleanup before retrying.' >&2
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
    if "$masked"; then systemctl --user unmask --runtime steamvr.service || status=1; fi
    if "$resume" && ! systemctl --user start steamvr.service &&
       ! systemctl --user is-active --quiet steamvr.service
    then
        printf '%s\n' 'SteamVR did not finish restarting. Restart Frame if VR does not return.' >&2
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
"$source_dir/compatibility/controller-utils-updater" --stage "$feed" "$work/package"
install -m 0755 "$work/package/compatibility/controller-utils-updater" "$work/setup-updater"
sudo -v || fail 'Authentication is required to install Controller Utils.'
if systemctl --user is-active --quiet steamvr.service
then
    resume=true
fi
load_state=$(systemctl --user show steamvr.service -p LoadState --value) || fail 'Cannot inspect the SteamVR service. Installation was stopped.'
[ "$load_state" != masked ] || fail 'SteamVR is masked. Unmask it before installing.'
masked=true
systemctl --user mask --runtime steamvr.service
systemctl --user stop steamvr.service
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
rm -rf -- "$base/app-data/updates"
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
printf '%s\n' "Controller Utils is installed. Open it from Frame's app drawer (+)."
