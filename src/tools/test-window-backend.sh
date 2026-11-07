#!/bin/bash
# A program of the base in a window of a private headless labwc
# (LOCACHY_BACKEND=wayland): its first configure and readiness, the
# compositor's frame, its picture on the output without the program's own top
# bar, keys and clicks from virtual devices, no DRM or input device of its
# own, a resize, closing as K_CLOSE, and the refusals of a window request that
# cannot be met. No uinput, physical input or display. Run in a disposable
# test root with labwc installed; the output directory must be new.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-window-backend.sh NEW_ABSOLUTE_OUTPUT_DIRECTORY}
[[ $out = /* && ! -e $out ]] || exit 2
mkdir -p "$out/gen" "$out/obj" "$out/run"
chmod 0700 "$out/run"
for p in xdg-shell fractional-scale-v1 viewporter primary-selection-unstable-v1 xdg-decoration-unstable-v1 \
         virtual-keyboard-unstable-v1 wlr-virtual-pointer-unstable-v1 wlr-screencopy-unstable-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/gen/$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/gen/$p-protocol.c"
done
cc=(gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/base" -I"$src/tools" -I"$out/gen")
read -r -a deps <<<"$(pkg-config --cflags freetype2 fontconfig libnm libinput) -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -DCONFIG_LIBINPUT"
read -r -a libs <<<"$(pkg-config --libs freetype2 fontconfig libnm libinput) -lm"
for f in "$src"/base/*.c; do
    n=$(basename "$f" .c)
    [ "$n" != ime ] || continue
    "${cc[@]}" "${deps[@]}" -DCONFIG_WAYLAND -c "$f" -o "$out/obj/win-$n.o"
    "${cc[@]}" "${deps[@]}" -c "$f" -o "$out/obj/direct-$n.o"
done
proto() { for p in "$@"; do printf '%s ' "$out/gen/$p-protocol.c"; done; }
read -r -a winproto <<<"$(proto xdg-shell fractional-scale-v1 viewporter primary-selection-unstable-v1 \
    xdg-decoration-unstable-v1)"
"${cc[@]}" "${deps[@]}" -DCONFIG_WAYLAND "$src/tools/test-window-backend.c" "$out"/obj/win-*.o "${winproto[@]}" \
    "${libs[@]}" -lwayland-client -lwayland-cursor -lxkbcommon -o "$out/window"
"${cc[@]}" "${deps[@]}" "$src/tools/test-window-backend.c" "$out"/obj/direct-*.o "${libs[@]}" -o "$out/direct"
"${cc[@]}" "$src/tools/test-keys.c" "$out/gen/virtual-keyboard-unstable-v1-protocol.c" \
    -lwayland-client -lxkbcommon -o "$out/keys"
"${cc[@]}" "$src/tools/test-click.c" "$out/gen/wlr-virtual-pointer-unstable-v1-protocol.c" \
    -lwayland-client -o "$out/click"
"${cc[@]}" "$src/tools/test-screenshot.c" "$out/gen/wlr-screencopy-unstable-v1-protocol.c" \
    -lwayland-client -o "$out/shot"
# The window in a screenshot: its colour's pixels and their box, the output's
# size, and how many of the box's first BAND rows are of the colour.
cat >"$out/box.py" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
colour = b'\x2a\x6f\xdb'
xs, ys, n = [], [], 0
for y in range(h):
    row = px[y * w * 3:(y + 1) * w * 3]
    for x in range(w):
        if row[x * 3:x * 3 + 3] == colour:
            n += 1
            xs.append(x)
            ys.append(y)
x0, y0 = min(xs, default=0), min(ys, default=0)
x1, y1 = max(xs, default=-1) + 1, max(ys, default=-1) + 1
band = 0
for y in range(y0, min(y0 + int(sys.argv[2]), y1)):
    row = px[y * w * 3:(y + 1) * w * 3]
    band += sum(1 for x in range(x0, x1) if row[x * 3:x * 3 + 3] == colour)
print(n, x0, y0, x1, y1, w, h, band)
PY

fails=0
pass() { printf 'PASS %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; fails=$((fails + 1)); }
# A line in the window's log matching the pattern, within 5 s.
logged() { for _ in $(seq 100); do grep -Eq "$1" "$out/window.log" && return 0; sleep .05; done; return 1; }
# The window's log has N lines matching the pattern, within 5 s.
counted() { for _ in $(seq 100); do [ "$(grep -Ec "$1" "$out/window.log" || true)" -ge "$2" ] && return 0; sleep .05; done; return 1; }

# The refusals need no compositor: they end before any connection.
# (Their reasons are said to /dev/kmsg where it can be written: the statuses tell them apart.)
status=0; timeout 10 env LOCACHY_BACKEND=wayland "$out/direct" >/dev/null 2>&1 || status=$?
if [ "$status" = 78 ]; then pass 'a direct-only build refuses LOCACHY_BACKEND=wayland (status 78), no DRM fallback'
else fail "direct-only build: status $status"; fi
status=0; timeout 10 env LOCACHY_BACKEND=framebuffer "$out/window" >/dev/null 2>&1 || status=$?
if [ "$status" = 78 ]; then pass 'an unknown LOCACHY_BACKEND value is refused (status 78)'
else fail "unknown backend: status $status"; fi
status=0
timeout 10 env XDG_RUNTIME_DIR="$out/run" WAYLAND_DISPLAY=no-such-display LOCACHY_BACKEND=wayland "$out/window" \
    >/dev/null 2>&1 || status=$?
if [ "$status" = 69 ]; then pass 'no compositor: the window request ends the program (status 69)'
else fail "no compositor: status $status"; fi

export XDG_RUNTIME_DIR=$out/run WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 XKB_DEFAULT_LAYOUT=us
labwc >"$out/labwc.log" 2>&1 &
lab=$!
client=
cleanup() {
    [ -z "$client" ] || kill "$client" 2>/dev/null || true
    kill "$lab" 2>/dev/null || true
    wait "$lab" 2>/dev/null || true
}
trap cleanup EXIT
for _ in $(seq 200); do [ -S "$out/run/wayland-0" ] && break; sleep .05; done
export WAYLAND_DISPLAY=wayland-0

mkfifo "$out/ready"
exec 4<>"$out/ready"
LOCACHY_BACKEND=wayland LOCACHY_WINDOW_TITLE='Window test' LOCACHY_WINDOW_APP_ID=locachy-test \
    LOCACHY_WINDOW_READY_FD=3 "$out/window" >"$out/window.log" 2>"$out/window.err" 3>"$out/ready" &
client=$!
byte=
if read -r -t 10 -n 1 -u 4 byte && [ "$byte" = 1 ]; then
    pass 'readiness byte after the first picture was shown'
else fail "no readiness byte (got '$byte')"; fi
cw=0 ch=0 top=0 gh=0 deco=0 bar=0
if logged '^SHOWN( [0-9]+){7}$'; then
    read -r _ cw ch cols rows top gh deco < <(grep -m1 '^SHOWN' "$out/window.log")
    bar=$((top + gh))
    pass "first configure: K_SHOWN with a ${cw}x${ch} canvas (${bar} rows of it the hidden bar's), ${cols}x${rows} cells"
else fail 'no K_SHOWN with a canvas'; fi
devices=$(for f in /proc/"$client"/fd/*; do readlink "$f"; done 2>/dev/null | grep -Ec '^/dev/(dri|input)/' || true)
if [ "$devices" = 0 ]; then pass 'the window opened no DRM or input device'
else fail "the window holds $devices DRM/input descriptors"; fi

sleep .3
# The compositor may configure again once it has placed the window and its
# frame: the size shown is the last K_SHOWN's.
read -r _ cw ch _ _ _ _ _ < <(grep '^SHOWN' "$out/window.log" | tail -n 1)
shown=$(grep -c '^SHOWN' "$out/window.log")
"$out/shot" "$out/screen" >/dev/null
read -r found x0 y0 x1 y1 sw sh band < <(python3 "$out/box.py" "$out"/screen-*.ppm "$gh")
vh=$((ch - bar))
if [ "$cw" -gt 0 ] && [ $((x1 - x0)) = "$cw" ] && [ $((y1 - y0)) = "$vh" ] && [ "$found" -ge $((cw * vh * 8 / 10)) ]; then
    pass "the window's picture is on the output: ${cw}x${vh} at ${x0},${y0}, ${found} pixels of its colour"
else fail "picture on the output: box ${x0},${y0}-${x1},${y1} (want ${cw}x${vh}), $found pixels"; fi
if [ "$bar" -gt 0 ] && [ "$band" = $((cw * gh)) ]; then
    pass "no top bar in the window: its first ${gh} rows are the program's (the canvas' ${bar} bar rows are not shown)"
else fail "top bar: ${band} of $((cw * gh)) pixels of the window's first ${gh} rows are the program's"; fi
if [ "$deco" = 2 ] && [ "$y0" -ge 8 ]; then
    pass "the compositor draws the frame: server-side decoration negotiated, the window starts at y=${y0} below its title bar"
else fail "frame: decoration mode $deco (2 is server side), window at y=${y0}"; fi

"$out/keys" a Shift+b Return BackSpace Ctrl+c Left F5 Escape
want=('^KEY 97 0$' '^KEY 66 1$' '^KEY 10 0$' '^KEY 8 0$' '^KEY 3 2$' '^KEY 258 0$' '^KEY 292 0$' '^KEY 27 0$')
ok=1
for w in "${want[@]}"; do logged "$w" || { ok=0; fail "key not delivered as $w"; }; done
[ "$ok" = 0 ] || pass 'keys from the seat: a, Shift+b, Return, BackSpace, Ctrl+c, Left, F5, Escape as the base codes'

cx=$(((x0 + x1) / 2)); cy=$(((y0 + y1) / 2))
"$out/click" "$cx" "$cy" "$sw" "$sh" left
if logged "^BUTTON 272 1 1 $((cx - x0)) $((cy - y0 + bar))$" && logged "^BUTTON 272 0 "; then
    pass "a click at the window's middle: press and release at $((cx - x0)),$((cy - y0 + bar)) of the canvas"
else fail "click: $(grep '^BUTTON' "$out/window.log" | head -2 | tr '\n' ' ')"; fi

# Maximized (labwc's default W-a): a new size is K_SHOWN, and the picture fills it.
"$out/keys" Super+a
counted '^SHOWN' $((shown + 1)) || true
read -r _ mw mh _ _ _ _ _ < <(grep '^SHOWN' "$out/window.log" | tail -n 1)
sleep .3
"$out/shot" "$out/maximized" >/dev/null
read -r found x0 y0 x1 y1 _ _ _ < <(python3 "$out/box.py" "$out"/maximized-*.ppm "$gh")
mvh=$((mh - bar))
if [ "$mw" -gt "$cw" ] && [ $((x1 - x0)) = "$mw" ] && [ $((y1 - y0)) = "$mvh" ] && [ "$found" -ge $((mw * mvh * 8 / 10)) ]; then
    pass "resized (maximized): K_SHOWN with ${mw}x${mh}, and the window shows ${mw}x${mvh} of it"
else fail "resize: ${mw}x${mh} after ${cw}x${ch}; box ${x0},${y0}-${x1},${y1}, $found pixels"; fi

# Closing: K_CLOSE, nothing ends by itself.
alive() { kill -0 "$client" 2>/dev/null; }
"$out/keys" k
logged '^KEY 107 0$' || true
"$out/keys" Alt+F4
counted '^KEPT$' 1 || true
"$out/keys" Alt+F4
if counted '^KEPT$' 2 && [ "$(grep -c '^CLOSE$' "$out/window.log")" = 2 ] && sleep .5 && alive; then
    pass 'a program that keeps its window (key_close_kept): two quick closes are two K_CLOSE, it keeps running'
else fail "kept: $(grep -c '^CLOSE$' "$out/window.log") K_CLOSE, $(grep -c '^KEPT$' "$out/window.log") kept"; fi
"$out/keys" u
logged '^KEY 117 0$' || true
"$out/keys" Alt+F4
if counted '^CLOSE$' 3 && sleep .5 && alive; then
    pass 'close: K_CLOSE delivered, and the program is still running'
else fail "close: $(grep -c '^CLOSE$' "$out/window.log") K_CLOSE, running: $(alive && echo yes || echo no)"; fi
sleep 3.3
"$out/keys" Alt+F4
if counted '^CLOSE$' 4 && sleep .3 && alive; then
    pass 'a close more than 3 s after an unanswered K_CLOSE is another K_CLOSE'
else fail "close after 3 s: $(grep -c '^CLOSE$' "$out/window.log") K_CLOSE, running: $(alive && echo yes || echo no)"; fi
"$out/keys" Alt+F4
for _ in $(seq 100); do alive || break; sleep .05; done
status=running
if ! alive; then status=0; wait "$client" || status=$?; client=; fi
if [ "$status" = 0 ]; then
    pass 'a second close within 3 s of an unanswered K_CLOSE ends the program (status 0)'
else fail "second close: status $status"; fi

if [ "$fails" = 0 ]; then printf 'PASS window backend: all checks\n'; else printf 'FAIL window backend: %d failed\n' "$fails"; exit 1; fi
