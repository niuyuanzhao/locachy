#!/bin/bash
# test-overview.sh: locachy-overview (and locachy-bar's top left corner) on a
# headless labwc with two screens (1 and 1.5), as root on a built system. A
# virtual keyboard and pointer, no uinput and an empty /dev/input: it can run
# while the shared display is in use. Private mount and PID namespaces, as
# test-bar.sh; the candidate overview is put over /usr/local/bin's (an overlay)
# where labwc's Super+W and the bar run it.
#   1. Super+W: on each screen its windows (three, one), as large as each
#      other and as on the screen (160 wide, 240 at 1.5), and the search field
#   2. the pointer over a window: framed
#   3. typing: only the windows that match; none: none; Esc clears, Esc ends
#   4. Super+W again: it ends
#   5. End: the last selected, Enter: activated, and the overview ends
#   6. a click: activated (a container's window, foot's, with labwc's
#      environment naming a control channel it does not have); a middle
#      click, a click on the close button (red under the pointer): asked to
#      close
#   7. a minimized window: listed, and brought back
#   8. the top left corner (the bar's): a 40 ms push nothing, 150 ms the
#      overview, which the rest of that push does not close; pushed again, it
#      closes, and the rest of that push does not show it again; one long
#      push (400 ms): shown once
#   9. a window that changes (foot, printing the time): its picture in the
#      overview changes too
#  10. touch (from standard input: built with OVERVIEW_TEST, as wlroots has
#      no virtual touch screen): touched, every window shows its close button;
#      a tap activates; a window dragged most of the way down is closed and
#      the overview stays, dragged a little it comes back; a tap on nothing
#      ends it
#  11. --show and --hide (the gestures'): only show, only close
# OVERVIEW_RENDERER=gles2 runs labwc on the GPU (a render node) instead of
# pixman: its captures come in XBGR8888 only, as on the X230's screen.
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
out=/var/tmp/locachy-overview-test
rm -rf "$out"
mkdir -p "$out"
cd "$out"

for p in wlr-foreign-toplevel-management-unstable-v1 wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 \
         xdg-shell wlr-screencopy-unstable-v1 wlr-virtual-pointer-unstable-v1 fractional-scale-v1 viewporter \
         relative-pointer-unstable-v1 ext-foreign-toplevel-list-v1 ext-image-capture-source-v1 \
         ext-image-copy-capture-v1 security-context-v1 wlr-output-management-unstable-v1 ext-idle-notify-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$p-protocol.c"
done
cc="gcc -O2 -Wall -Wextra -D_GNU_SOURCE -I$src/base/include -I$src/base -I$out $(pkg-config --cflags freetype2 libnm)"
libs="$(pkg-config --libs freetype2 libnm) -lwayland-client -lm"
base=$(ls "$src"/base/*.c | grep -v '/ime.c$')
$cc -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -o labwc-control "$src/progs/labwc-control.c" $base \
    security-context-v1-protocol.c wlr-foreign-toplevel-management-unstable-v1-protocol.c \
    wlr-output-management-unstable-v1-protocol.c ext-idle-notify-v1-protocol.c $libs || exit 1
$cc -DCONFIG_FREETYPE -DOVERVIEW_TEST -o locachy-overview "$src/progs/overview.c" $base \
    wlr-layer-shell-unstable-v1-protocol.c wlr-foreign-toplevel-management-unstable-v1-protocol.c \
    ext-foreign-toplevel-list-v1-protocol.c ext-image-capture-source-v1-protocol.c \
    ext-image-copy-capture-v1-protocol.c fractional-scale-v1-protocol.c viewporter-protocol.c xdg-shell-protocol.c \
    $libs -lwayland-cursor -lxkbcommon || exit 1
$cc -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -DCONFIG_IME -o locachy-bar "$src/progs/bar.c" \
    $base "$src/base/ime.c" wlr-layer-shell-unstable-v1-protocol.c virtual-keyboard-unstable-v1-protocol.c \
    xdg-shell-protocol.c wlr-foreign-toplevel-management-unstable-v1-protocol.c \
    fractional-scale-v1-protocol.c viewporter-protocol.c relative-pointer-unstable-v1-protocol.c \
    $libs -lrime -lxkbcommon -lwayland-cursor || exit 1
$cc -o test-window "$src/tools/test-window.c" xdg-shell-protocol.c -lwayland-client || exit 1
$cc -o test-screenshot "$src/tools/test-screenshot.c" wlr-screencopy-unstable-v1-protocol.c -lwayland-client || exit 1
$cc -o test-click "$src/tools/test-click.c" wlr-virtual-pointer-unstable-v1-protocol.c -lwayland-client || exit 1
$cc -o test-keys "$src/tools/test-keys.c" virtual-keyboard-unstable-v1-protocol.c -lwayland-client -lxkbcommon || exit 1
# blobs.py SHOT RRGGBB: the areas of that colour (100 pixels or more), left
# to right: "N: WxH+X+Y ...".
cat >blobs.py <<'EOF'
import sys
d = open(sys.argv[1], "rb").read().split(b"\n", 3)
w, h = map(int, d[1].split())
px, c = d[3], bytes.fromhex(sys.argv[2])
parent, boxes, prev = {}, {}, []
def find(a):
    while parent[a] != a:
        parent[a] = parent[parent[a]]
        a = parent[a]
    return a
n = 0
for y in range(h):
    row, runs, x = px[y * w * 3:(y + 1) * w * 3], [], 0
    i = row.find(c)
    while i >= 0:
        if i % 3:
            i = row.find(c, i + 1)
            continue
        s = i // 3
        e = s
        while e < w and row[e * 3:e * 3 + 3] == c:
            e += 1
        parent[n] = n
        boxes[n] = [s, y, e - 1, y, e - s]
        for ps, pe, pid in prev:
            if ps <= e - 1 and s <= pe:
                parent[find(pid)] = find(n)
        runs.append((s, e - 1, n))
        n += 1
        i = row.find(c, e * 3)
    prev = runs
merged = {}
for k, b in boxes.items():
    r = find(k)
    m = merged.setdefault(r, [b[0], b[1], b[2], b[3], 0])
    m[0], m[1], m[2], m[3], m[4] = min(m[0], b[0]), min(m[1], b[1]), max(m[2], b[2]), max(m[3], b[3]), m[4] + b[4]
found = sorted(m for m in merged.values() if m[4] >= 100)
print("%d: %s" % (len(found), " ".join("%dx%d+%d+%d" % (m[2] - m[0] + 1, m[3] - m[1] + 1, m[0], m[1]) for m in found)))
EOF
# at.py SHOT X Y: the colour there, RRGGBB.
cat >at.py <<'EOF'
import sys
d = open(sys.argv[1], "rb").read().split(b"\n", 3)
w = int(d[1].split()[0])
i = (int(sys.argv[3]) * w + int(sys.argv[2])) * 3
print(d[3][i:i + 3].hex())
EOF
mkdir -p etc
cp -a /etc/locachy/. etc/
rm -rf etc/containers && mkdir etc/containers
# First: one is foot's (a host program, its container's name foot).
printf 'host="1"\npackage="foot"\ndesktop="foot.desktop"\n' >etc/containers/foot.conf
printf "HEADLESS-1=origin,1280x720,1\nHEADLESS-2=right,preferred,1.5\n" >etc/monitors.conf
python3 - "$src/link/labwc-rc.xml" etc/labwc/rc.xml <<'EOF'
import sys
s = open(sys.argv[1]).read().replace("TERMINAL-KEY", "C-A-t").replace("HOME-KEY", "W-Escape")
s = "".join(line for line in s.replace("LOCK-KEY", "W-l").splitlines(True) if "IME-KEY" not in line)
rules = """<windowRules>
  <windowRule title="First:*"><action name="MoveToOutput" output="HEADLESS-1"/></windowRule>
  <windowRule title="Second:*"><action name="MoveToOutput" output="HEADLESS-2"/></windowRule>
  <windowRule title="Live:*"><action name="MoveToOutput" output="HEADLESS-2"/></windowRule>
</windowRules>
"""
open(sys.argv[2], "w").write(s.replace("</openbox_config>", rules + "</openbox_config>"))
EOF

export out src OVERVIEW_RENDERER="${OVERVIEW_RENDERER:-pixman}"
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -c '
set -u
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
mount -t tmpfs -o mode=0755 tmpfs /run/user
mkdir -m 0700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
mount -t tmpfs -o mode=0755 tmpfs /run/locachy
mount -t tmpfs -o mode=0755 tmpfs /dev/input
mount --bind $out/etc /etc/locachy
mkdir -p $out/rime-upper $out/rime-work $out/bin-upper $out/bin-work
mount -t overlay overlay -o lowerdir=/var/lib/locachy/rime,upperdir=$out/rime-upper,workdir=$out/rime-work \
    /var/lib/locachy/rime
mount -t overlay overlay -o lowerdir=/usr/local/bin,upperdir=$out/bin-upper,workdir=$out/bin-work /usr/local/bin
cp $out/locachy-overview /usr/local/bin/locachy-overview

# As locachy-seat once left it to labwc and its clients: their descriptor 3
# is not process 1 (the click on a container window wrote there).
LOCACHY_CONTROL=3 WLR_BACKENDS=headless WLR_RENDERER=$OVERVIEW_RENDERER WLR_HEADLESS_OUTPUTS=2 \
    labwc -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >$out/labwc.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/wayland-0 ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=wayland-0
$out/labwc-control >$out/control.log 2>&1 & # the screens of monitors.conf
for i in $(seq 50); do [ -S /run/user/0/locachy-0 ] && break; sleep 0.1; done
$out/locachy-bar >$out/bar.log 2>&1 &
sleep 1.5
L="2133 720" # the layout: 1280 of the first screen, 1280 / 1.5 of the second
shot() { sleep ${1:-1}; rm -f $out/shot-*.ppm; $out/test-screenshot $out/shot 2>>$out/client.log; }
blobs() { python3 $out/blobs.py $out/shot-HEADLESS-$1.ppm 4488cc; }
count() { blobs $1 | cut -d: -f1; }
box() { blobs $1 | cut -d" " -f$(($2 + 1)); } # box SCREEN N: WxH+X+Y of the Nth
at() { python3 $out/at.py $out/shot-HEADLESS-$1.ppm $2 $3; }
up() { pgrep -x locachy-overvie >/dev/null; }
keys() { $out/test-keys "$@" 2>>$out/client.log; }
click() { $out/test-click "$@" 2>>$out/client.log; }
activations() { grep -c activated $out/win-$1; }
# A window framed: the colour just outside its picture (the frame is drawn
# twice its line away, a line wide: at 1, x - 3).
framed() { local b=$(box $1 $2) x y w h; IFS="x+" read -r w h x y <<<"$b"; [ "$(at $1 $((x - 3)) $((y + h / 2)))" = 3daee9 ]; }
centre() { local b=$(box 1 $1) x y w h; IFS="x+" read -r w h x y <<<"$b"; echo "$((x + w / 2)) $((y + h / 2))"; }

for t in one two three; do
    $out/test-window --title "First: $t" $([ $t = one ] && echo foot) >$out/win-$t &
    sleep 0.3
done
$out/test-window --title "Second: four" >$out/win-four &
sleep 1

echo "== 1. Super+W"
keys Super+w
shot 1.2
b1=$(blobs 1); b2=$(blobs 2)
w1=$(echo "$b1" | cut -d" " -f2- | tr " " "\n" | cut -dx -f1 | sort -u) # widths: First: one has its container name (foot) under it
check "up, the first screen with its three windows as large as each other ($b1)" \
    "up && [ \"\$(count 1)\" = 3 ] && [ \$(echo \"$w1\" | wc -l) = 1 ]"
check "the second with its one, 240 wide at 1.5 ($b2)" "[ \"\$(count 2)\" = 1 ] && [ \"\$(box 2 1 | cut -dx -f1)\" = 240 ]"
check "the search field on both (Breeze view background)" \
    "[ \"\$(python3 $out/blobs.py $out/shot-HEADLESS-1.ppm 141618 | cut -d: -f1)\" -ge 1 ] &&
     [ \"\$(python3 $out/blobs.py $out/shot-HEADLESS-2.ppm 141618 | cut -d: -f1)\" -ge 1 ]"

echo "== 2. hover"
read -r cx cy <<<"$(centre 2)"
click $cx $cy $L move
shot 0.5
check "the pointer over the second window: it is framed, the others not" "framed 1 2 && ! framed 1 1 && ! framed 1 3"

echo "== 3. search"
keys t w o
shot 0.5
check "typing two: only First: two ($(blobs 1); second screen: $(blobs 2))" "[ \"\$(count 1)\" = 1 ] && [ \"\$(count 2)\" = 0 ]"
keys x
shot 0.5
check "twox: no window left ($(blobs 1))" "[ \"\$(count 1)\" = 0 ]"
keys Escape
shot 0.5
check "Esc clears the search: all three again, still up" "up && [ \"\$(count 1)\" = 3 ]"
keys Escape
sleep 0.5
check "Esc again: it ends" "! up"

echo "== 4. Super+W again"
keys Super+w
sleep 1
up && keys Super+w
sleep 0.8
check "Super+W, then Super+W: it ends" "! up"

echo "== 5. keys"
before=$(activations three)
keys Super+w
sleep 1
click 200 650 $L move # on the first screen: the keys go there (the focus was on the second)
keys End
shot 0.5
check "End selects the last" "framed 1 3 && ! framed 1 1"
keys Home
shot 0.5
check "Home the first" "framed 1 1 && ! framed 1 3"
keys Right
shot 0.5
check "Right the one beside it" "framed 1 2 && ! framed 1 1"
keys End Return
sleep 0.8
check "End, Enter: First: three activated ($before then $(activations three)), and it ends" \
    "[ \$(activations three) -gt $before ] && ! up"

echo "== 6. clicks"
before=$(activations one)
keys Super+w
shot 1.2
read -r cx cy <<<"$(centre 1)"
click $cx $cy $L
sleep 0.8
check "a click on First: one (foot): activated ($before then $(activations one)), and it ends" \
    "[ \$(activations one) -gt $before ] && ! up"
keys Super+w
shot 1.2
read -r cx cy <<<"$(centre 2)"
click $cx $cy $L middle
sleep 0.5
check "a middle click on First: two: asked to close, still up" "grep -q close $out/win-two && up"
IFS="x+" read -r w h x y <<<"$(box 1 3)"
click $((x + w - 16)) $((y + 16)) $L move # the close button: 18 pixel text, its radius 12, a quarter row in
shot 0.5
check "the pointer on First: three s close button: red ($(at 1 $((x + w - 16)) $((y + 16 - 9))))" \
    "[ \"\$(at 1 $((x + w - 16)) $((y + 16 - 9)))\" = da4453 ]"
click $((x + w - 16)) $((y + 16)) $L
sleep 0.5
check "a click on it: asked to close" "grep -q close $out/win-three && up"
keys Escape
sleep 0.5

echo "== 7. minimized"
$out/test-window --minimize >$out/win-mini &
sleep 1
keys Super+w
sleep 1.2
keys l i f e c y c l e
shot 0.5
echo "   found: first screen $(blobs 1), second $(blobs 2)"
before=$(activations mini)
keys Return
sleep 0.8
check "Enter on the only one found: brought back, activated ($before then $(activations mini))" \
    "[ \$(activations mini) -gt $before ] && ! up"
pkill -f "test-window --minimize" # the first screen as it was: three windows

echo "== 8. the top left corner"
click 0 0 $L push 40
sleep 0.8
check "pushed for 40 ms: nothing" "! up"
sleep 0.5
click 0 0 $L push 150
sleep 1
check "pushed for 150 ms: the overview, still up after the push" "up"
click 0 0 $L push 150
sleep 1
check "pushed again: it ends, and stays closed" "! up"
click 0 0 $L push 400
sleep 1
check "one push of 400 ms: shown once, still up" "up"
keys Escape
sleep 0.5
grep "corner" $out/bar.log | tail -2

echo "== 9. live"
foot --title "Live: clock" sh -c "while :; do date +%s.%N; sleep 0.2; done" >/dev/null 2>&1 &
sleep 1.5
keys Super+w
shot 1.5
cp $out/shot-HEADLESS-2.ppm $out/live-1.ppm
shot 1
check "the picture of the window that changes changes too" "! cmp -s $out/live-1.ppm $out/shot-HEADLESS-2.ppm"
keys Escape
sleep 0.5
check "Esc: it ends" "! up"
pkill -x foot

echo "== 10. touch"
mkfifo $out/touch
touch_on() { /usr/local/bin/locachy-overview <$out/touch >>$out/overview.log 2>&1 & exec 7>$out/touch; sleep 1.2; }
touch_off() { exec 7>&-; sleep 0.3; }
grey() { local x y w h; IFS="x+" read -r w h x y <<<"$(box 1 $1)"; [ "$(at 1 $((x + w - 16)) $((y + 16 - 9)))" = 202326 ]; }
touch_on
read -r cx cy <<<"$(centre 2)"
echo "down 1280x720 $cx $cy" >&7
shot 0.5
check "touched: every window shows its close button" "grey 1 && grey 3"
before=$(activations two)
echo up >&7
sleep 0.8
check "a tap on First: two: activated ($before then $(activations two)), and it ends" \
    "[ \$(activations two) -gt $before ] && ! up"
touch_off
touch_on
read -r cx cy <<<"$(centre 1)"
echo "down 1280x720 $cx $cy" >&7
for d in 20 60 120 200 260; do echo "motion $cx $((cy + d))" >&7; sleep 0.05; done
shot 0.5
check "dragged down: First: one follows the finger, fading (left: $(blobs 1))" "[ \"\$(count 1)\" = 2 ]"
echo up >&7
sleep 0.5
check "let go 0.6 of the way down: First: one asked to close, the overview stays" "grep -q close $out/win-one && up"
shot 0.3 # all three in their places again (First: one stays, as test-window does when asked to close)
read -r cx cy <<<"$(centre 3)"
closes=$(grep -c close $out/win-three)
echo "down 1280x720 $cx $cy" >&7
for d in 20 40 60; do echo "motion $cx $((cy + d))" >&7; sleep 0.05; done
echo up >&7
shot 0.5
check "dragged a little: not closed, back in its place ($(blobs 1))" \
    "[ \"\$(count 1)\" = 3 ] && [ \$(grep -c close $out/win-three) = $closes ]"
echo "down 1280x720 100 650" >&7
echo up >&7
sleep 0.5
check "a tap on nothing: it ends" "! up"
touch_off

echo "== 11. --show, --hide"
/usr/local/bin/locachy-overview --hide; sleep 0.3
check "--hide with none up: nothing" "! up"
/usr/local/bin/locachy-overview --show </dev/null >>$out/overview.log 2>&1 & sleep 1.2
check "--show: up" "up"
/usr/local/bin/locachy-overview --show; sleep 0.5
check "--show again: still up (not closed)" "up"
/usr/local/bin/locachy-overview --hide; sleep 0.5
check "--hide: it ends" "! up"

pkill -f "$out/test-window"
pkill -x foot
[ $fail = 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; tail -5 $out/labwc.log; }
exit $fail
'
