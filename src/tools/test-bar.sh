#!/bin/bash
# test-bar.sh: locachy-bar's window list and overlays on a headless labwc with
# two screens (1 and 1.5), as root on a built system. No keyboard at all (an
# empty /dev/input for the bar, no uinput): it can run while the shared
# display is in use. Private mount and PID namespaces, as test-labwc.sh.
#   1. the window list as on Windows XP: one button at most 20 characters
#      wide; more, all as wide; too many, 6 characters wide and scrolled by
#      < and >, which stays scrolled (the widths seen are printed)
#   2. the question (start again?) is on the screen of the focused window at
#      its scale, and goes to another screen with the focus
#   3. notifications from a container (locachy-app start, its own bus): at the
#      top right of the focused screen, the container marked in the window
#      list; one goes by itself, a click on the other runs its default action,
#      brings the container's window and clears the mark. It writes files
#      named .locachy-bar-test-notify* in the shared Documents and removes them.
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
out=/var/tmp/locachy-bar-test
rm -rf "$out"
mkdir -p "$out"
cd "$out"
IFS=: read -r _ _ uid _ _ uhome _ <<<"$(getent passwd 1000)"
[ -n "${uid:-}" ] || { echo "no user with uid 1000"; exit 1; }
f=$uhome/Documents/.locachy-bar-test-notify # a container writes only shared folders

for p in security-context-v1 wlr-foreign-toplevel-management-unstable-v1 \
         wlr-output-management-unstable-v1 wlr-layer-shell-unstable-v1 \
         virtual-keyboard-unstable-v1 xdg-shell wlr-screencopy-unstable-v1 \
         wlr-virtual-pointer-unstable-v1 fractional-scale-v1 viewporter ext-idle-notify-v1 \
         relative-pointer-unstable-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$p-protocol.c"
done
cc="gcc -O2 -Wall -Wextra -D_GNU_SOURCE -I$src/base/include -I$src/base -I$out $(pkg-config --cflags freetype2 libnm)"
libs="$(pkg-config --libs freetype2 libnm) -lwayland-client -lm"
base=$(ls "$src"/base/*.c | grep -v '/ime.c$')
$cc -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -o labwc-control "$src/progs/labwc-control.c" $base \
    security-context-v1-protocol.c wlr-foreign-toplevel-management-unstable-v1-protocol.c \
    wlr-output-management-unstable-v1-protocol.c ext-idle-notify-v1-protocol.c $libs || exit 1
$cc -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -DCONFIG_IME -o locachy-bar "$src/progs/bar.c" $base \
    "$src/base/ime.c" wlr-layer-shell-unstable-v1-protocol.c virtual-keyboard-unstable-v1-protocol.c \
    xdg-shell-protocol.c wlr-foreign-toplevel-management-unstable-v1-protocol.c \
    fractional-scale-v1-protocol.c viewporter-protocol.c relative-pointer-unstable-v1-protocol.c $libs $(pkg-config --cflags --libs gio-2.0) -lrime \
    -lxkbcommon -lwayland-cursor || exit 1
$cc -o test-window "$src/tools/test-window.c" xdg-shell-protocol.c -lwayland-client || exit 1
$cc -o test-screenshot "$src/tools/test-screenshot.c" wlr-screencopy-unstable-v1-protocol.c -lwayland-client || exit 1
$cc -o test-click "$src/tools/test-click.c" wlr-virtual-pointer-unstable-v1-protocol.c -lwayland-client || exit 1
$cc $(pkg-config --cflags gio-2.0) -o test-notify "$src/tools/test-notify.c" $(pkg-config --libs gio-2.0) || exit 1
# In the demo container: its window, a notification that goes by itself after
# 1.5 s, and one that stays until clicked.
cat >notifier <<EOF
#!/bin/sh
$out/test-window --title "First: notifier" &
sleep 1.5
$out/test-notify $f.1 1500 "Goes by itself" &
sleep 6
exec $out/test-notify $f.2 0 "Stays until clicked"
EOF
chmod 755 notifier
# buttons.py SHOT: the window list's buttons in the top bar (27 pixels at 1:
# one and a half rows of 18, its text 6 pixels wide, bar.c's pad 2): runs
# of a button's colours, apart by the bar's: "WIDTH:on|off|attn ...".
cat >buttons.py <<'EOF'
import sys
BAR, ON, OFF = bytes((0x20, 0x30, 0x48)), bytes((0x46, 0x66, 0x9A)), bytes((0x2A, 0x3C, 0x5C))
ATTN = bytes((0xB0, 0x70, 0x2A))
d = open(sys.argv[1], "rb").read().split(b"\n", 3)
w = int(d[1].split()[0])
px = d[3]
y = 2  # above the text of the first row
row = [px[(y * w + x) * 3:(y * w + x) * 3 + 3] for x in range(w)]
runs, x = [], 0
while x < w:
    if row[x] in (ON, OFF, ATTN):
        s = x
        while x < w and row[x] != BAR:
            x += 1
        part = row[s:x]
        kind = max(("on", ON), ("off", OFF), ("attn", ATTN), key=lambda k: part.count(k[1]))[0]
        runs.append("%d:%s" % (x - s, kind))
    else:
        x += 1
print(" ".join(runs))
EOF
# box.py SHOT RRGGBB: the box of that colour: "WIDTHxHEIGHT at X,Y", or "none".
cat >box.py <<'EOF'
import sys
d = open(sys.argv[1], "rb").read().split(b"\n", 3)
w, h = map(int, d[1].split())
px, c = d[3], bytes.fromhex(sys.argv[2])
xs, ys = [], []
for y in range(0, h):
    row = px[y * w * 3:(y + 1) * w * 3]
    i = row.find(c)
    while i >= 0 and i % 3:
        i = row.find(c, i + 1)
    if i >= 0:
        j = row.rfind(c)
        while j % 3:
            j = row.rfind(c, 0, j)
        xs += [i // 3, j // 3]
        ys.append(y)
print("%dx%d at %d,%d" % (max(xs) - min(xs) + 1, max(ys) - min(ys) + 1, min(xs), min(ys)) if xs else "none")
EOF
# The configuration, with windows put on a screen by their titles.
mkdir -p etc
cp -a /etc/locachy/. etc/
rm -rf etc/containers && mkdir etc/containers # no real container is among them
printf "HEADLESS-1=origin,1280x720,1\nHEADLESS-2=right,preferred,1.5\n" >etc/monitors.conf
python3 - etc/labwc/rc.xml <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
rules = """<windowRules>
  <windowRule title="First:*"><action name="MoveToOutput" output="HEADLESS-1"/></windowRule>
  <windowRule title="Second:*"><action name="MoveToOutput" output="HEADLESS-2"/></windowRule>
</windowRules>
"""
open(p, "w").write(s.replace("</openbox_config>", rules + "</openbox_config>"))
EOF

export out src f
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -c '
set -u
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
mount -t tmpfs -o mode=0755 tmpfs /run/user
mkdir -m 0700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
mount -t tmpfs -o mode=0755 tmpfs /run/locachy
mount -t tmpfs -o mode=0755 tmpfs /dev/input # the bar finds no keyboard to read or grab
mount -t tmpfs tmpfs /var/lib/locachy/containers
C=/var/lib/locachy/containers
for c in demo plain; do mkdir -p $C/$c/upper/usr/share/applications; echo "command=\"true\"" >$out/etc/containers/$c.conf; done
printf "[Desktop Entry]\nName=Demo\nStartupWMClass=locachy-test-window\n" >$C/demo/upper/usr/share/applications/demo.desktop
printf "[Desktop Entry]\nName=Plain\nExec=plain\n" >$C/plain/upper/usr/share/applications/plain.desktop
mkdir -p $out/rime-upper $out/rime-work
mount --bind $out/etc /etc/locachy
mount -t overlay overlay -o lowerdir=/var/lib/locachy/rime,upperdir=$out/rime-upper,workdir=$out/rime-work \
    /var/lib/locachy/rime

WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=2 \
    labwc -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >$out/labwc.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/wayland-0 ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=wayland-0
$out/labwc-control >$out/control.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/locachy-0 ] && break; sleep 0.1; done
$out/locachy-bar >$out/bar.log 2>&1 &
bar=$!
for i in $(seq 50); do [ -S /run/user/0/locachy-bar.sock ] && break; sleep 0.1; done
sleep 1
L="2133 720" # the layout: 1280 of the first screen, 1280 / 1.5 of the second
shot() { sleep ${1:-1}; rm -f $out/shot-*.ppm; $out/test-screenshot $out/shot 2>>$out/client.log; }
buttons() { python3 $out/buttons.py $out/shot-HEADLESS-1.ppm; }
widths() { buttons | tr " " "\n" | cut -d: -f1 | sort -n | uniq | tr "\n" " "; }
count() { buttons | wc -w; }
window() { $out/test-window --title "$1" >/dev/null 2>&1 & }
seen() { echo "$1 window(s): $(buttons)" >>$out/widths; }

echo "== 1. the window list"
window "First: a very long title, as Edge gives its windows: Microsoft Edge and a page with a long name"
shot 1.5
b=$(buttons); seen 1
check "one window: one button, focused, at most 20 characters wide ($b)" \
    "[ \"\${b##*:}\" = on ] && [ \$(count) = 1 ] && [ \"\${b%%:*}\" -le 116 ]"
for i in 2 3; do window "First: window $i, with a title too long for its button"; done
shot 1.5
seen 3
check "3 windows: 3 buttons, all as wide, at most 20 characters ($(widths))" \
    "[ \$(count) = 3 ] && [ \$(widths | wc -w) = 1 ] && [ \$(widths) -le 116 ]"
for i in 4 5 6; do window "First: window $i"; done
shot 1.5
seen 6
for i in $(seq 7 40); do window "First: window $i"; done
shot 3
b=$(buttons); seen 40
first=${b%% *}; last=${b##* }
check "40 windows: scrolled, 6 characters wide between < and > ($(widths))" \
    "[ \"\${first%%:*}\" = 10 ] && [ \"\${last%%:*}\" = 10 ] && [ \"\$(widths)\" = \"10 32 \" ]"
check "the focused window is in sight" "echo \"$b\" | grep -q \"32:on\""
lx=$(python3 -c "
import sys
d = open(\"$out/shot-HEADLESS-1.ppm\", \"rb\").read().split(b\"\n\", 3)
w = int(d[1].split()[0]); px = d[3]; y = 2
ON, OFF = bytes((0x46, 0x66, 0x9A)), bytes((0x2A, 0x3C, 0x5C))
print([x for x in range(w) if px[(y * w + x) * 3:(y * w + x) * 3 + 3] in (ON, OFF)][0] + 8)")
for i in $(seq 1 15); do $out/test-click $lx 18 $L 2>>$out/client.log; done
shot
check "< scrolls the list back: the focused window out of sight" "! buttons | grep -q \"32:on\""
shot 2.5
check "and it stays scrolled (redrawn each second)" "! buttons | grep -q \"32:on\""
pkill -f "$out/test-window"
sleep 0.5

echo "== 2. the question, on the focused window s screen"
window "First: the focus"
sleep 1
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect(\"/run/user/0/locachy-bar.sock\")
s.send(b\"show plain\")
print(s.recv(256).decode())" >$out/show.reply
shot 1.5
b1=$(python3 $out/box.py $out/shot-HEADLESS-1.ppm 182848); b2=$(python3 $out/box.py $out/shot-HEADLESS-2.ppm 182848)
check "on the first screen at scale 1: 72 by 7 cells of 10x18, 720x126 ($b1; second: $b2; $(cat $out/show.reply))" \
    "[ \"\${b1%% *}\" = 720x126 ] && [ \"$b2\" = none ]"
window "Second: the focus"
shot 2
b1=$(python3 $out/box.py $out/shot-HEADLESS-1.ppm 182848); b2=$(python3 $out/box.py $out/shot-HEADLESS-2.ppm 182848)
check "with the focus on the second screen at 1.5: there, cells of 14x28, 1008x196 drawn at that size ($b2; first: $b1)" \
    "[ \"\${b2%% *}\" = 1008x196 ] && [ \"$b1\" = none ]"
kill -USR2 $bar # released (home on the screen): the question goes
shot
check "released, the question is gone" "[ \"\$(python3 $out/box.py $out/shot-HEADLESS-2.ppm 182848)\" = none ]"

echo "== 3. notifications"
pkill -f "$out/test-window"
kill -USR1 $bar # the shared display on the screen again
rm -f $f.1 $f.2
echo "command=\"$out/notifier\"" >/etc/locachy/containers/demo.conf
LOCACHY_WAYLAND_DISPLAY=locachy-0 locachy-app start demo >$out/demo.log 2>&1 &
sleep 0.8
$out/test-window --title "First: holds the focus" locachy-other >/dev/null 2>&1 &
for i in $(seq 50); do grep -q "^id" $f.1 2>/dev/null && break; sleep 0.1; done
shot 0.5
b1=$(python3 $out/box.py $out/shot-HEADLESS-1.ppm 182848); b2=$(python3 $out/box.py $out/shot-HEADLESS-2.ppm 182848)
check "the server answers ($(head -2 $f.1 2>/dev/null | tr "\n" " "))" "grep -qx \"caps body,actions\" $f.1 && grep -q ^id $f.1"
check "at the top right of the focused screen, 8 under the bar: 40 by 4 cells, 400x72 ($b1; second: $b2)" \
    "[ \"$b1\" = \"400x72 at 872,35\" ] && [ \"$b2\" = none ]"
check "the container is marked in the window list ($(buttons))" "buttons | grep -q attn"
id1=$(sed -n "s/^id //p" $f.1)
for i in $(seq 40); do grep -q "^closed" $f.1 2>/dev/null && break; sleep 0.1; done
shot 0.3
check "after 1.5 s it goes by itself (closed $id1 1)" \
    "grep -qx \"closed $id1 1\" $f.1 && [ \"\$(python3 $out/box.py $out/shot-HEADLESS-1.ppm 182848)\" = none ]"
for i in $(seq 80); do grep -q "^id" $f.2 2>/dev/null && break; sleep 0.1; done
id2=$(sed -n "s/^id //p" $f.2)
shot 0.5
check "the one that stays is shown ($(python3 $out/box.py $out/shot-HEADLESS-1.ppm 182848))" \
    "[ \"\$(python3 $out/box.py $out/shot-HEADLESS-1.ppm 182848)\" = \"400x72 at 872,35\" ]"
$out/test-click 1032 92 $L 2>>$out/client.log
sleep 0.5
focused=$(python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect(\"/run/user/0/locachy-control.sock\")
s.send(b\"focused\")
print(s.recv(256).decode())")
check "a click: its default action, then closed ($(tail -2 $f.2 | tr "\n" " "))" \
    "grep -qx \"action $id2 default\" $f.2 && grep -qx \"closed $id2 2\" $f.2"
shot 1.2
check "the container s window has the focus ($focused), the mark is gone ($(buttons))" \
    "[ \"$focused\" = \"ok demo\" ] && ! buttons | grep -q attn && [ \"\$(python3 $out/box.py $out/shot-HEADLESS-1.ppm 182848)\" = none ]"

pkill -f "$out/test-window"
LOCACHY_WAYLAND_DISPLAY=locachy-0 locachy-app stop demo >/dev/null 2>&1
rmdir /sys/fs/cgroup/app_demo 2>/dev/null
rm -f $f.1 $f.2
kill $bar 2>/dev/null
cat $out/widths
[ $fail = 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; tail -5 $out/bar.log; }
exit $fail
'
