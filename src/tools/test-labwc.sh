#!/bin/bash
# test-labwc.sh: the shared compositor's host clients on a headless labwc, as
# root on a built system, without touching the screen, the real keyboards,
# /run/user/0, the containers or the Rime user data: everything runs in
# private mount and PID namespaces (whatever is left is killed with them,
# however the test ends), typing comes from a uinput test keyboard. Only the
# results go to standard output; the programs' output goes to files in $out.
#   1. locachy-0 (labwc-control) hides the privileged interfaces
#   2. "show NAME" raises a minimized window of container NAME; "focused"
#      names the container of the focused window
#   3. monitors.conf: fractional scales and placement, applied at start, on
#      request, and by itself when the file changes
#   4. locachy-bar: Ctrl+Space, "nihao" + 1 types its first candidate into a plain
#      wl_keyboard client without resizing it; back in direct mode nothing is
#      grabbed; released (home on screen) it ignores keys; key_home while
#      grabbed asks locachy-seat for home; each screen has the top bar, two
#      rows of text in its 27 pixels (one and a half rows of 18), the window
#      in its window list; a click
#      there minimizes the focused window, another brings it back; swaybg
#      draws the wallpaper under the windows
#      (screenshots: $out/shot-OUTPUT.ppm, clicks: a virtual pointer)
#   5. locachy-seat, as process 1 starts it (a stand-in at its control
#      socket), starts labwc and swaybg with rc.conf's wallpaper: on every
#      screen, changed while released (as settings and files write it) at
#      the next take, started again if it exited, ended with no wallpaper;
#      the touchpad's swipes (labwc-control --action) reach process 1
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
out=/tmp/locachy-labwc-test
rm -rf "$out"
mkdir -p "$out"
cd "$out"
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }

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
    fractional-scale-v1-protocol.c viewporter-protocol.c relative-pointer-unstable-v1-protocol.c $libs -lrime \
    -lxkbcommon -lwayland-cursor || exit 1
$cc -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -o locachy-seat "$src/progs/seat.c" $base $libs || exit 1
$cc -o test-window "$src/tools/test-window.c" xdg-shell-protocol.c -lwayland-client || exit 1
$cc -o test-ime-client "$src/tools/test-ime-client.c" xdg-shell-protocol.c -lwayland-client -lxkbcommon || exit 1
$cc -o wayland-check "$src/tools/wayland-check.c" -lwayland-client || exit 1
$cc -o test-uinput "$src/tools/test-uinput.c" || exit 1
$cc -o test-screenshot "$src/tools/test-screenshot.c" wlr-screencopy-unstable-v1-protocol.c -lwayland-client || exit 1
$cc -o test-click "$src/tools/test-click.c" wlr-virtual-pointer-unstable-v1-protocol.c -lwayland-client || exit 1
# pixel.py FILE X Y RRGGBB: is the pixel of the screenshot that colour?
cat >pixel.py <<'EOF'
import sys
d = open(sys.argv[1], "rb").read().split(b"\n", 3)
w, h = map(int, d[1].split())
i = (int(sys.argv[3]) * w + int(sys.argv[2])) * 3
got = d[3][i:i + 3]
if got != bytes.fromhex(sys.argv[4]):
    print("%s %s,%s: %s, not %s" % (sys.argv[1], sys.argv[2], sys.argv[3], got.hex(), sys.argv[4]), file=sys.stderr)
sys.exit(0 if got == bytes.fromhex(sys.argv[4]) else 1)
EOF
# A screenshot of one screen, H (27) pixels of bar: is the bar there (its
# colour over the top H rows, not below), text in both of its rows right of
# LOCachy, and the first button of the window list: "BUTTON X on|off" (its
# middle, and whether its colour is the focused one).
cat >bar-check.py <<'EOF'
import sys
BG, ON, OFF = bytes((0x20, 0x30, 0x48)), bytes((0x46, 0x66, 0x9A)), bytes((0x2A, 0x3C, 0x5C))
head = open(sys.argv[1], "rb").read().split(b"\n", 3)
H = int(sys.argv[2]) if len(sys.argv) > 2 else 27
w, h = map(int, head[1].split())
px = head[3]
at = lambda x, y: px[(y * w + x) * 3:(y * w + x) * 3 + 3]
bar = sum(at(x, y) in (BG, ON, OFF) for y in range(H) for x in range(0, w, 2)) > w // 2 * (H - 6)
below = sum(at(x, H) == BG for x in range(w)) < w // 2
lit = lambda ys: sum(at(x, y)[0] > 0x60 for y in ys for x in range(150, w))
top, bottom = lit(range(0, H // 2)), lit(range(H // 2, H))
xs = [x for x in range(w) if at(x, H // 2) in (ON, OFF)]
button = "BUTTON %d %s" % ((xs[0] + 20), "on" if at(xs[0], H // 2) == ON else "off") if xs else "no window button"
print("bar %s, text %d/%d, %s" % ("yes" if bar and below else "no", top, bottom, button))
sys.exit(0 if bar and below and top > 200 and bottom > 200 else 1)
EOF
# p1.py SEAT ARG...: process 1 for locachy-seat, its control socket at fd 3:
# "take" (with a descriptor, as the screen comes) and "release" from standard
# input go to it, "master" is answered "mastered", what it sends is printed
# (fields separated by spaces).
cat >p1.py <<'EOF'
import os, selectors, socket, sys
a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
os.environ["LOCACHY_CONTROL"] = "3"
if os.fork() == 0:
    os.dup2(b.fileno(), 3)
    os.execv(sys.argv[1], sys.argv[1:])
b.close()
null = os.open("/dev/null", os.O_RDONLY)
sel = selectors.DefaultSelector()
sel.register(a, selectors.EVENT_READ)
sel.register(sys.stdin, selectors.EVENT_READ)
while True:
    for key, _ in sel.select():
        if key.fileobj is a:
            msg = a.recv(4096)
            if not msg:
                sys.exit(0)
            print(msg.decode().replace("\0", " "), flush=True)
            if msg == b"master":
                a.send(b"mastered")
        else:
            cmd = sys.stdin.readline().strip()
            if not cmd:
                sys.exit(0)
            if cmd == "take":
                socket.send_fds(a, [b"take"], [null])
            else:
                a.send(cmd.encode())
EOF
# Two wallpapers, PNG files of one colour each.
python3 - <<'EOF'
import struct, zlib
def png(path, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * 64 for _ in range(48))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 48, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
png("wall-a.png", (0x30, 0xa0, 0x50))
png("wall-b.png", (0x20, 0x40, 0xc0))
EOF
modprobe uinput 2>/dev/null || true

export out
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -c '
set -u
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
mount -t tmpfs -o mode=0755 tmpfs /run/user
mkdir -m 0700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
mount -t tmpfs tmpfs /var/lib/locachy/containers
mkdir -p /var/lib/locachy/containers/demo/upper/usr/share/applications
printf "[Desktop Entry]\nName=Demo\nStartupWMClass=locachy-test-window\n" \
    >/var/lib/locachy/containers/demo/upper/usr/share/applications/demo.desktop
mkdir -p $out/etc $out/rime-upper $out/rime-work
cp -a /etc/locachy/. $out/etc/
rm -rf $out/etc/containers && mkdir $out/etc/containers
echo "command=\"true\"" >$out/etc/containers/demo.conf
printf "HEADLESS-1=origin,1280x720,1.25\nHEADLESS-2=right,preferred,1.5\n" >$out/etc/monitors.conf
mount --bind $out/etc /etc/locachy
mount -t overlay overlay -o lowerdir=/var/lib/locachy/rime,upperdir=$out/rime-upper,workdir=$out/rime-work \
    /var/lib/locachy/rime

WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=2 \
    labwc -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >$out/labwc.log 2>&1 &
labwc=$!
for i in $(seq 50); do [ -S /run/user/0/wayland-0 ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=wayland-0
run=$(date +%s%N)
mark() { echo "<6>test-labwc: mark $run $1" >/dev/kmsg; }
since() { dmesg | sed -n "/test-labwc: mark $run $1\$/,\$p"; }
mark start
$out/labwc-control >$out/control.log 2>&1 &
control=$!
for i in $(seq 50); do [ -S /run/user/0/locachy-0 ] && break; sleep 0.1; done

echo "== 1. the containers display"
check "locachy-0 is a socket anyone may open" "[ \"\$(stat -c %a /run/user/0/locachy-0)\" = 666 ]"
check "the control socket is root-only" "[ \"\$(stat -c %a /run/user/0/locachy-control.sock)\" = 600 ]"
WAYLAND_DISPLAY=wayland-0 $out/wayland-check >$out/host.globals
WAYLAND_DISPLAY=locachy-0 $out/wayland-check >$out/container.globals
for g in zwlr_screencopy_manager_v1 zwp_virtual_keyboard_manager_v1 zwlr_foreign_toplevel_manager_v1 \
         zwlr_output_manager_v1 zwlr_layer_shell_v1 zwp_input_method_manager_v2 zwlr_data_control_manager_v1 \
         wp_security_context_manager_v1; do
    check "$g: host yes, container no" "grep -q \"^$g \" $out/host.globals && ! grep -q \"^$g \" $out/container.globals"
done
check "containers keep xdg_wm_base and fractional scale" \
    "grep -q \"^xdg_wm_base \" $out/container.globals && grep -q \"^wp_fractional_scale_manager_v1 \" $out/container.globals"

request() { python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect(\"/run/user/0/locachy-control.sock\")
s.send(sys.argv[1].encode())
print(s.recv(256).decode())" "$1"; }

echo "== 2. show"
WAYLAND_DISPLAY=locachy-0 $out/test-window --minimize >$out/window.out 2>&1 &
window=$!
sleep 0.5
: >$out/window.out
reply=$(request "show demo")
sleep 0.3
check "show demo -> ok shown 1 ($reply)" "[ \"$reply\" = \"ok shown 1\" ]"
check "the minimized window was activated" "grep -q activated $out/window.out"
reply=$(request "show other")
check "a container without .desktop files is an error ($reply)" "[ \"\${reply%% *}\" = error ]"
reply=$(request focused)
check "focused -> ok demo ($reply)" "[ \"$reply\" = \"ok demo\" ]"
kill $window
sleep 0.3
reply=$(request focused)
check "no window: focused -> ok ($reply)" "[ \"$reply\" = ok ]"

echo "== 3. screens"
sleep 0.2
check "monitors.conf applied at start" "since start | grep -q \"labwc-control: HEADLESS-1 1280x720 scale 1.25 at 0,0\" && since start | grep -q \"HEADLESS-2 1280x720 scale 1.50 at 1024,0\" && since start | grep -q \"screens configured\""
printf "HEADLESS-1=origin,1280x720,1\nHEADLESS-2=left,preferred,1.75\n" >/etc/locachy/monitors.conf
mark reload
reply=$(request reload-monitors)
sleep 0.3
check "reload-monitors -> ok ($reply)" "[ \"$reply\" = \"ok monitors\" ]"
check "new placement applied (left, 1.75)" "since reload | grep -q \"HEADLESS-2 1280x720 scale 1.75 at 0,0\" && since reload | grep -q \"HEADLESS-1 1280x720 scale 1.00 at 731,0\" && since reload | grep -q \"screens configured\""
# As settings writes it (while the shared display is frozen): no request.
mark watch
printf "HEADLESS-1=origin,1280x720,1\nHEADLESS-2=above,preferred,1\n" >/etc/locachy/monitors.conf.tmp
mv /etc/locachy/monitors.conf.tmp /etc/locachy/monitors.conf
sleep 1
check "a changed monitors.conf is applied by itself (above)" "since watch | grep -q \"HEADLESS-2 1280x720 scale 1.00 at 0,0\" && since watch | grep -q \"HEADLESS-1 1280x720 scale 1.00 at 0,720\" && since watch | grep -q \"screens configured\""

echo "== 4. locachy-bar"
printf "HEADLESS-1=origin,1280x720,1\nHEADLESS-2=right,preferred,1\n" >/etc/locachy/monitors.conf
request reload-monitors >/dev/null
coproc KEYS { $out/test-uinput 2>$out/uinput.log; }
read -r dev <&${KEYS[0]}
mount -t tmpfs -o mode=0755 tmpfs /dev/input
mknod /dev/input/event0 c ${dev%%:*} ${dev##*:}
key() { for k in "$@"; do echo "$k" >&${KEYS[1]}; done; sleep 0.4; }
# The bar first: a headless labwc has no keyboard but the virtual one of the bar.
( trap "echo home >>$out/home" USR1; $out/locachy-bar & bar=$!; echo $bar >$out/bar.pid
  while kill -0 $bar 2>/dev/null; do wait $bar; done ) >$out/bar.log 2>&1 &
sleep 2
bar=$(cat $out/bar.pid)
WAYLAND_DISPLAY=wayland-0 $out/test-ime-client $out/typed >$out/client.log 2>&1 &
client=$!
sleep 0.5
check "the bar and a plain wl_keyboard client are running" "kill -0 $bar && kill -0 $client"
swaybg -c "#c03020" >$out/swaybg.log 2>&1 &
sleep 2
$out/test-screenshot $out/shot 2>>$out/client.log
wallpaper() { for i in $(seq 10); do $out/test-screenshot $out/shot 2>>$out/client.log
    python3 $out/pixel.py $out/shot-HEADLESS-1.ppm 10 700 c03020 2>>$out/pixel.log && return 0; sleep 0.5; done
    return 1; }
check "the wallpaper (swaybg) is under the windows" wallpaper
for o in HEADLESS-1 HEADLESS-2; do
    check "$o: the top bar, two rows of text in 27 pixels ($(python3 $out/bar-check.py $out/shot-$o.ppm))" \
        "python3 $out/bar-check.py $out/shot-$o.ppm >/dev/null"
done
: >$out/typed
key +29 57 -29                 # Ctrl+Space: direct -> Chinese
key 49 23 35 30 24 2           # n i h a o, candidate 1
check "Chinese mode types 你好 into a plain wl_keyboard client" "grep -qx 你好 $out/typed"
# Chromium drops a character typed on Esc (1); Backspace, Tab, Enter have
# meanings of their own too.
check "the text came on plain character keys, not Esc/Backspace/Tab/Enter" \
    "[ -s $out/typed.keys ] && ! grep -qE \"^(1|14|15|28) \" $out/typed.keys"
check "the candidate overlay did not resize the window" "! grep -q reconfigured $out/typed"
key +125 1 -125                # Super+Esc (key_home) while grabbed
sleep 0.3
check "key_home while grabbed asks locachy-seat for home" "grep -q home $out/home 2>/dev/null"
key +29 57 -29 +29 57 -29      # Ctrl+Space twice: Chinese -> English -> direct
: >$out/typed
key 44                         # z: direct mode, labwc (headless: no keyboard) gets it
check "direct mode leaves the keyboard to labwc (nothing typed by the bar)" "[ ! -s $out/typed ]"
kill -USR2 $bar; sleep 0.2     # locachy-seat released the seat: home is on the screen
key +29 57 -29 49 23 2
check "released, it ignores the keyboard (no mode switch, nothing typed)" "[ ! -s $out/typed ]"
kill -USR1 $bar; sleep 0.2
key +29 57 -29 49 23 35 30 24 2
# The client writes all the text it has received: the first word and this one.
check "back on the screen, it works again" "grep -qx 你好你好 $out/typed"
check "the client is still running" "kill -0 $client"
button() { $out/test-screenshot $out/shot 2>>$out/client.log; python3 $out/bar-check.py $out/shot-HEADLESS-1.ppm | grep -o "BUTTON.*"; }
b=$(button)
check "the window list shows the client, focused ($b)" "[ \"${b##* }\" = on ]"
x=$(echo "$b" | cut -d" " -f2)
$out/test-click $x 18 2560 720 2>>$out/client.log; sleep 0.5
b=$(button)
check "a click on the focused window minimizes it ($b)" "[ \"${b##* }\" = off ]"
$out/test-click $x 18 2560 720 2>>$out/client.log; sleep 0.5
b=$(button)
check "a click on the minimized one brings it back, focused ($b)" "[ \"${b##* }\" = on ]"
printf "HEADLESS-1=origin,1280x720,1\nHEADLESS-2=right,preferred,1.5\n" >/etc/locachy/monitors.conf
sleep 1.5
$out/test-screenshot $out/shot 2>>$out/client.log
check "at scale 1.5 the bar is 42 pixels (one and a half rows of 28), drawn at that size ($(python3 $out/bar-check.py $out/shot-HEADLESS-2.ppm 42))" \
    "python3 $out/bar-check.py $out/shot-HEADLESS-2.ppm 42 >/dev/null"

kill $bar $client $control 2>/dev/null
sleep 0.3
check "labwc-control removed its sockets" "[ ! -e /run/user/0/locachy-0 ] && [ ! -e /run/user/0/locachy-control.sock ]"
kill $labwc 2>/dev/null
wait $labwc 2>/dev/null

echo "== 5. the wallpaper (locachy-seat)"
# locachy-seat starts labwc, labwc-control (this one) and swaybg itself;
# udevadm, settings --apply, the bar and the gestures are left out: they
# would act on the real devices.
mount -t tmpfs tmpfs /run/udev && : >/run/udev/control
mount --bind /usr/bin/true /usr/bin/udevadm
[ ! -e /usr/local/bin/settings ] || mount --bind /usr/bin/true /usr/local/bin/settings
mount --bind $out/labwc-control /usr/local/lib/locachy/labwc-control
[ ! -e /usr/local/bin/locachy-bar ] || mount --bind /dev/null /usr/local/bin/locachy-bar
[ ! -e /usr/local/lib/locachy/gestures ] || mount --bind /dev/null /usr/local/lib/locachy/gestures
# As settings and files write it.
wallpaper() { grep -v "^wallpaper=" /etc/locachy/rc.conf >/etc/locachy/rc.conf.tmp
    [ -z "$1" ] || echo "wallpaper=\"$1\"" >>/etc/locachy/rc.conf.tmp
    mv /etc/locachy/rc.conf.tmp /etc/locachy/rc.conf; }
said() { for i in $(seq 100); do [ "$(grep -cx "$1" $out/p1.out)" -ge "$2" ] && return 0; sleep 0.2; done; return 1; }
shows() { for i in $(seq 10); do $out/test-screenshot $out/wall 2>>$out/client.log
    python3 $out/pixel.py $out/wall-HEADLESS-1.ppm 10 700 $1 2>>$out/pixel.log &&
    python3 $out/pixel.py $out/wall-HEADLESS-2.ppm 10 700 $1 2>>$out/pixel.log && return 0; sleep 0.5; done
    return 1; }
wallpaper $out/wall-a.png
mkfifo $out/p1.in
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=2 \
    python3 $out/p1.py $out/locachy-seat /usr/bin/labwc <$out/p1.in >$out/p1.out 2>$out/seat.log &
exec 8>$out/p1.in
echo take >&8
said display-ready 1 && echo release >&8 # as process 1 does: home stays on the screen
check "display-ready, then released" "said released 1"
check "rc.conf wallpaper is drawn by its swaybg on both screens" "shows 30a050"
wallpaper $out/wall-b.png
echo take >&8
check "one chosen while released is drawn at the next take, on both screens" "said master 2 && shows 2040c0"
# As link/libinput-gestures.conf runs it for three-finger swipes.
for a in home lock next prev; do $out/labwc-control --action $a >>$out/client.log 2>&1 || echo "action $a: $?" >>$out/client.log; done
check "swipes: labwc-control --action home, lock, next, prev reach process 1" \
    "said home 1 && said lock 1 && said \"switch next\" 1 && said \"switch prev\" 1"
pkill -f "^swaybg -m fill"
sleep 0.3
echo release >&8; said released 2; echo take >&8
check "a swaybg that exited is started again at the next take" "said master 3 && shows 2040c0"
wallpaper ""
echo release >&8; said released 3; echo take >&8
said master 4; sleep 0.5
check "no wallpaper: its swaybg is ended" "! pgrep -f \"^swaybg -m fill\" >/dev/null"
exec 8>&-
pkill -x labwc
eval "exec ${KEYS[1]}>&-"
wait 2>/dev/null
[ $fail = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $fail
'
