#!/bin/sh
set -eu
[ "$#" = 0 ] || { printf '%s\n' 'Usage: ./uninstall.sh' >&2; exit 1; }
source_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
exec /bin/sh "$source_dir/install.sh" --uninstall
