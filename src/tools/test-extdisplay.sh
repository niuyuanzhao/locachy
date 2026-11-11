#!/bin/sh
# The extended-display receiver's focused tests (tools/EXTDISPLAY.md): protocol
# cases, then real FFmpeg decoding of H.264 and HEVC streams made by the
# installed ffmpeg's encoders, through the receiver's demux and decoder.
# On Linux it also builds the worker and the client (sandbox, memfd, IPC).
# Nothing is installed; no VM, no display, no network.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/locachy-extdisplay.XXXXXX")
trap 'rm -rf "$out"' EXIT
pkgs="json-c libavcodec libavutil libswscale"
if ! pkg-config --exists $pkgs; then
    echo "SKIP: $pkgs are not all installed" >&2
    exit 77
fi
cflags="-std=c11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I$src/progs"
extra=
if [ "$(uname -s)" = Linux ]; then
    extra="$src/progs/extdisplay-client.c"
fi
cc $cflags -DCONFIG_EXTDISPLAY_FFMPEG $(pkg-config --cflags $pkgs) "$src/tools/test-extdisplay.c" \
    "$src/progs/extdisplay-protocol.c" "$src/progs/extdisplay-decode.c" $extra $(pkg-config --libs $pkgs) -lm -o "$out/test-extdisplay"
"$out/test-extdisplay"
if [ "$(uname -s)" = Linux ]; then
    cc $cflags -DCONFIG_EXTDISPLAY_FFMPEG $(pkg-config --cflags $pkgs) "$src/progs/extdisplay-worker.c" \
        "$src/progs/extdisplay-protocol.c" "$src/progs/extdisplay-decode.c" $(pkg-config --libs $pkgs) -lm -o "$out/extdisplay-worker"
    cc $cflags -c "$src/progs/extdisplay-client.c" -o "$out/extdisplay-client.o"
    echo "PASS: extdisplay-worker and extdisplay-client build"
    # The pairing client against a stub broker: a real child process, fd 3 only.
    cc $cflags -I"$src/base/include" -I"$src/base" "$src/tools/test-extdisplay-setup.c" "$src/progs/extdisplay-setup.c" \
        -o "$out/test-extdisplay-setup"
    "$out/test-extdisplay-setup"
    # Without its two channels the worker must refuse to run (exit 78), never decode.
    rc=0
    "$out/extdisplay-worker" 3<&- 4<&- </dev/null >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 78 ]; then
        echo "PASS: extdisplay-worker refuses to run without its channels"
    else
        echo "FAIL: extdisplay-worker exited $rc without its channels (expected 78)"
        exit 1
    fi
else
    echo "SKIP: extdisplay-worker, extdisplay-client and the IPC cases need Linux (Landlock, seccomp, memfd seals)"
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "SKIP: no ffmpeg to make the test streams" >&2
    exit 0
fi
lavfi="testsrc2=size=320x240:rate=30"
if ffmpeg -hide_banner -loglevel error -f lavfi -i "$lavfi" -frames:v 12 -an -c:v libx264 -bf 0 -g 6 \
    -pix_fmt yuv420p -f h264 "$out/test.h264"; then
    "$out/test-extdisplay" h264 320 240 "$out/test.h264"
else
    echo "SKIP: ffmpeg has no libx264"
fi
if ffmpeg -hide_banner -loglevel error -f lavfi -i "$lavfi" -frames:v 12 -an -c:v libx265 \
    -x265-params log-level=error:bframes=0:keyint=6 -pix_fmt yuv420p -f hevc "$out/test.hevc"; then
    "$out/test-extdisplay" hevc 320 240 "$out/test.hevc"
else
    echo "SKIP: ffmpeg has no libx265"
fi
