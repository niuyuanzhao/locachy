#!/bin/bash
# test-open.sh: xdg-open from one container to another through locachy-bar,
# as root on a built system with the user (uid 1000) made. On a headless
# labwc in private mount and PID namespaces, with throwaway containers (the
# real ones, /etc/locachy and /run/locachy are hidden behind copies), typing
# from a uinput keyboard. It writes files named .locachy-open-test* in the
# shared Documents and removes them.
#   1. one container can open a type: it gets the file, without asking
#   2. two can: the chooser; 2 + Enter opens in the second, once
#   3. A in the chooser makes it the rule; the next open does not ask
#   4. a URL goes to the container listing its scheme
#   5. a folder goes to home: the files request, and home is asked for
#   6. a file outside the shared folders is refused
#   7. --with program:term (an executable in a shared folder) is refused:
#      only a program whose .desktop file opens the type, and home is not asked
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
out=/var/tmp/locachy-open-test # not /tmp: a container has a /tmp of its own
rm -rf "$out"
mkdir -p "$out"
IFS=: read -r user _ uid gid _ uhome _ <<<"$(getent passwd 1000)"
[ -n "${uid:-}" ] || { echo "no user with uid 1000"; exit 1; }
docs=$uhome/Documents
cd "$out"

for p in wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 xdg-shell \
         wlr-foreign-toplevel-management-unstable-v1 fractional-scale-v1 viewporter; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$p-protocol.c"
done
cc="gcc -O2 -Wall -Wextra -D_GNU_SOURCE -I$src/base/include -I$src/base -I$out $(pkg-config --cflags freetype2 libnm)"
base=$(ls "$src"/base/*.c | grep -v '/ime.c$')
$cc -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -DCONFIG_IME -o locachy-bar "$src/progs/bar.c" $base \
    "$src/base/ime.c" wlr-layer-shell-unstable-v1-protocol.c virtual-keyboard-unstable-v1-protocol.c \
    xdg-shell-protocol.c wlr-foreign-toplevel-management-unstable-v1-protocol.c \
    fractional-scale-v1-protocol.c viewporter-protocol.c $(pkg-config --libs freetype2 libnm) -lwayland-client -lwayland-cursor -lrime -lxkbcommon || exit 1
musl-gcc -static -O2 -o xdg-open "$src/progs/xdg-open.c" || exit 1
$cc -o test-uinput "$src/tools/test-uinput.c" || exit 1
modprobe uinput 2>/dev/null || true

# A program for the test containers: it writes what it was given.
cat >"$out/record" <<EOF
#!/bin/sh
name=\$1; shift
printf '%s\n' "\$*" >>$docs/.locachy-open-test.\$name
sleep 4 # still running for the next open: that one goes into it (nsenter)
EOF
cat >"$out/ask" <<'EOF'
#!/bin/sh
# The asking container: xdg-open each argument (with=NAME=TARGET: --with
# NAME TARGET), its exit status recorded.
out=$1; shift
for t in "$@"; do
    case $t in
    with=*) w=${t#with=}; /usr/local/bin/xdg-open --with "${w%%=*}" "${w#*=}" >>"$out.err" 2>&1 ;;
    *) /usr/local/bin/xdg-open "$t" >>"$out.err" 2>&1 ;;
    esac
    echo "$? $t" >>"$out"
done
sleep 60
EOF
chmod 755 "$out/record" "$out/ask"

export out src uid uhome docs
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -c '
set -u
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
mount -t tmpfs -o mode=0755 tmpfs /run/user
mkdir -m 0700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
mount -t tmpfs -o mode=0755 tmpfs /run/locachy
mkdir -p $out/etc && cp -a /etc/locachy/. $out/etc/ && rm -f $out/etc/open.conf
rm -rf $out/etc/containers && mkdir $out/etc/containers # no real container is among them
mount --bind $out/etc /etc/locachy
mount -t tmpfs tmpfs /var/lib/locachy/containers
C=/var/lib/locachy/containers
mk() { # NAME MIMETYPES COMMAND...
    local name=$1 mime=$2; shift 2
    mkdir -p $C/$name/upper/usr/share/applications
    printf "command=\"%s\"\n" "$*" >/etc/locachy/containers/$name.conf
    printf "[Desktop Entry]\nType=Application\nName=Test %s\nExec=%s %%U\nMimeType=%s\n" \
        "$name" "$*" "$mime" >$C/$name/upper/usr/share/applications/$name.desktop
}
f=$docs/.locachy-open-test
mk asker "x-none/x-none;" $out/ask $f.asked $f.only $f.txt $f.txt $f.txt https://example.org/x $docs /etc/os-release \
    with=program:term=$f.sh
mk one "image/png;" $out/record one
mk two "text/plain;" $out/record two
mk three "text/plain;" $out/record three
mk web "x-scheme-handler/https;" $out/record web
# The bar mounts its socket in the containers: /usr/local/lib/locachy/xdg-open must be this one.
mount --bind $out/xdg-open /usr/local/lib/locachy/xdg-open

WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
    labwc -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >$out/labwc.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/wayland-0 ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=wayland-0
/usr/local/lib/locachy/labwc-control >$out/control.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/locachy-0 ] && break; sleep 0.1; done
coproc KEYS { $out/test-uinput 2>$out/uinput.log; }
read -r dev <&${KEYS[0]}
mount -t tmpfs -o mode=0755 tmpfs /dev/input
mknod /dev/input/event0 c ${dev%%:*} ${dev##*:}
key() { for k in "$@"; do echo "$k" >&${KEYS[1]}; done; sleep 0.4; }
( trap "echo home >>$out/home" USR1; $out/locachy-bar & bar=$!; echo $bar >$out/bar.pid
  while kill -0 $bar 2>/dev/null; do wait $bar; done ) >$out/bar.log 2>&1 &
for i in $(seq 50); do [ -S /run/locachy/open/socket ] && break; sleep 0.1; done
check "the bar listens on /run/locachy/open/socket" "[ -S /run/locachy/open/socket ]"

cp /usr/share/pixmaps/archlinux-logo.png $f.only # image/png to file(1)
echo two >$f.txt
printf "#!/bin/sh\ntouch /run/locachy/open-test-ran\n" >$f.sh
chmod 755 $f.sh
: >$f.asked
chown $uid $f.only $f.txt $f.sh $f.asked
# The asker opens, in turn: the only-one type, a text file (chooser), the
# text file again (chooser: always), the text file (no chooser), a URL, a
# folder, a file outside the shared folders.
LOCACHY_WAYLAND_DISPLAY=locachy-0 locachy-app start asker >$out/asker.log 2>&1 &
waitfor() { for i in $(seq 60); do [ "$(wc -l <$f.asked)" -ge $1 ] && return 0; sleep 0.25; done; return 1; }
# A container that starts first has its bus and the tray watcher: a moment more.
arrived() { for i in $(seq 20); do grep -qx "$1" "$2" 2>/dev/null && return 0; sleep 0.25; done; return 1; }

echo "== 1. one candidate"
waitfor 1
sleep 1
check "xdg-open succeeded" "grep -qx \"0 $f.only\" $f.asked"
check "container one got the file" "arrived $f.only $f.one"
echo "== 2. the chooser, once"
sleep 1
key 3 +28 -28                   # candidates sorted by name: 1 three, 2 two; 2 + Enter
sleep 1
waitfor 2
check "chooser: the second candidate (two) got the file" "arrived $f.txt $f.two"
check "no rule was written" "[ ! -s /etc/locachy/open.conf ]"
echo "== 3. A: always"
sleep 1
key 2 30                        # first candidate (three), A
waitfor 3
check "chooser A: three got it" "arrived $f.txt $f.three"
check "the rule text/plain=three was written" "grep -qx text/plain=three /etc/locachy/open.conf"
waitfor 4
sleep 1
check "next time no chooser: three again, into the running container" "[ \"$(grep -c . $f.three)\" = 2 ]"
echo "== 4. a URL"
waitfor 5
sleep 1
check "the https URL went to web" "arrived https://example.org/x $f.web"
echo "== 5. a folder"
waitfor 6
check "a folder becomes a files request for home" "grep -qx $docs /run/locachy/files-request && grep -q home $out/home"
echo "== 6. outside the shared folders"
waitfor 7
check "a file outside the shared folders is refused" "grep -q \"^4 /etc/os-release\" $f.asked && grep -q \"not in a shared folder\" $f.asked.err"
echo "== 7. --with program:term"
waitfor 8
check "program:term for an executable is refused" "grep -q \"^4 with=program:term=\" $f.asked && grep -q \"does not open files of this type\" $f.asked.err"
check "home was not asked to run it" "[ ! -e /run/locachy/open-request ] && [ ! -e /run/locachy/open-test-ran ]"
grep -h "locachy-bar: open" <(dmesg) | tail -6
rm -f $f.*
[ $fail = 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; cat $f.asked $f.asked.err $out/asker.log $out/bar.log 2>/dev/null | tail -30; }
eval "exec ${KEYS[1]}>&-"
for c in asker one two three web; do LOCACHY_WAYLAND_DISPLAY=locachy-0 locachy-app stop $c >/dev/null 2>&1; rmdir /sys/fs/cgroup/app_$c 2>/dev/null; done
exit $fail
'
