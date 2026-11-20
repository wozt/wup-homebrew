#!/bin/sh
# netfs-server: serve a folder to the Wii U GamePad's /net mount.
#   ./run.sh [ROOT] [--token SECRET] [--host 0.0.0.0] [--port 8734]
# Default: serve the current directory on this machine only. For the pad on the LAN,
# pass --host 0.0.0.0 and the token the menu is configured with. NOT encrypted: LAN only.
set -e
cd "$(dirname "$0")"
ROOT="${1:-.}"; [ "${1#--}" != "$1" ] && ROOT=.
exec python3 netfs_server.py --root "$ROOT" "$@"
