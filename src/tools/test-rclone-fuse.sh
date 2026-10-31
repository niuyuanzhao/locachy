#!/bin/sh
# test-rclone-fuse.sh RCLONE NEW_SCRATCH: test-rclone-fuse.py as root of a
# disposable user and mount namespace. Real rclone and FUSE mounts; nothing
# is mounted, installed or changed on the host.
set -eu
[ "$#" = 2 ] || { echo "usage: test-rclone-fuse.sh RCLONE NEW_SCRATCH" >&2; exit 2; }
rclone=$(realpath "$1")
scratch=$(realpath -m "$2")
[ -x "$rclone" ] || { echo "no rclone at $1" >&2; exit 2; }
[ -c /dev/fuse ] || { echo "no /dev/fuse" >&2; exit 2; }
[ ! -e "$scratch" ] || { echo "$scratch exists" >&2; exit 2; }
mkdir -p "$scratch"
src=$(cd "$(dirname "$0")/.." && pwd)
RCLONE=$rclone SCRATCH=$scratch exec unshare --map-auto --map-root-user --mount --propagation private \
    timeout 300 python3 -B "$src/tools/test-rclone-fuse.py"
