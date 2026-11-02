#!/bin/bash
# locachy-app camera_nodes: what a container with camera="1" gets is what
# locachy-face-camera ordinary names, and only character devices called
# /dev/videoN or /dev/mediaN (no link, no other path). In a user and mount
# namespace of its own, /dev is a directory in which /dev/null is bound under
# camera names: no camera and no root are needed.
set -euo pipefail
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-camera-nodes.sh NEW_OUTPUT_DIRECTORY}
[[ "$out" = /* && ! -e "$out" ]] || { echo 'Output must be a new absolute directory' >&2; exit 2; }
mkdir -p "$out/dev"
# The function itself, from the script.
sed -n '/^camera_nodes() {$/,/^}$/p' "$src/link/locachy-app" >"$out/camera_nodes.sh"
grep -q 'ordinary' "$out/camera_nodes.sh" || { echo 'FAIL camera_nodes not found in locachy-app'; exit 1; }
cat >"$out/face-camera" <<'EOF'
#!/bin/sh
# What a tool could print, the good with the bad.
printf '%s\n' /dev/video0 /dev/media1 /dev/video3 /dev/video9 /dev/null /dev/../dev/video0 \
    '/dev/video0 1 3' /dev/v4l/by-path/x /dev/video2x
exit "${FACE_EXIT:-0}"
EOF
chmod +x "$out/face-camera"
for f in video0 media1 null video3; do touch "$out/dev/$f"; done
ln -s video0 "$out/dev/video9"
ln -s /proc/self/fd "$out/dev/fd" # bash process substitution
unshare --map-root-user --mount bash -euc '
    out=$1
    for f in video0 media1 null; do mount --bind /dev/null "$out/dev/$f"; done
    mount --rbind "$out/dev" /dev
    source "$out/camera_nodes.sh"
    FACE_CAMERA=$out/face-camera
    got=$(camera_nodes)
    want=$(printf "video0 1 3\nmedia1 1 3")
    [ "$got" = "$want" ] || { printf "FAIL camera_nodes gave:\n%s\n" "$got"; exit 1; }
    got=$(FACE_EXIT=1 camera_nodes) # a failing tool: what it named, still checked
    [ "$got" = "$want" ] || { printf "FAIL camera_nodes with a failing tool gave:\n%s\n" "$got"; exit 1; }
    FACE_CAMERA=$out/missing
    got=$(camera_nodes)
    [ -z "$got" ] || { printf "FAIL camera_nodes without the tool gave:\n%s\n" "$got"; exit 1; }
' bash "$out"
echo 'PASS camera_nodes: only character devices /dev/videoN, /dev/mediaN named by locachy-face-camera ordinary (no links, no other paths); none without the tool'
