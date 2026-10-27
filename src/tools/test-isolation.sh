#!/bin/bash
# test-isolation.sh: what a program in a container sees, as root on a built
# system with the user (uid 1000) made. A throwaway container (in $out, not
# among the real ones) runs a check script on a headless labwc, in private
# mount and PID namespaces; the screen, /run/user/0 and the real containers
# are not touched. It writes one file in the shared Downloads and removes it,
# and is given a directory of the home of its own (directories=), made and
# removed here; two it is refused.
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
out=/var/tmp/locachy-isolation-test # not /tmp: a container has a /tmp of its own
rm -rf "$out"
mkdir -p "$out/containers/isotest"
IFS=: read -r user _ uid gid _ uhome _ <<<"$(getent passwd 1000)"
[ -n "${uid:-}" ] || { echo "no user with uid 1000"; exit 1; }
sha256sum /etc/passwd /etc/group >"$out/host-ids.before"

cat >"$out/check.sh" <<'EOF'
#!/bin/sh
# Runs in the container as the user.
r() { printf '%s=%s\n' "$1" "$2"; }
r uid "$(id -u)"
r user "$(id -un)"
r home "$HOME"
r groups "$(id -G)"
r var_lib_locachy "$(ls -A /var/lib/locachy 2>&1 | tr '\n' ' ')"
r other_container "$(ls /var/lib/locachy/containers/edge/data 2>&1 | head -1)"
r runtime "$(cd /run/user/$(id -u) && find . -mindepth 1 -not -path "./dbus-1*" | sort | tr '\n' ' ')"
r bus "${DBUS_SESSION_BUS_ADDRESS:-none}"
r bus_socket "$([ -S /run/locachy-bus/bus ] && echo yes || echo no)"
r host_buses "$(ls /run/dbus /run/user/0/bus 2>&1 | tr '\n' ' ')"
r snd "$(ls /dev/snd 2>&1 | head -1)"
r dri "$(ls /dev/dri 2>&1 | tr '\n' ' ')"
r user_dirs "$(grep -c '^XDG_.*_DIR="\$HOME/' "$HOME/.config/user-dirs.dirs" 2>&1)"
r shared "$(for d in Documents Downloads Music Pictures Videos; do [ -w "$HOME/$d" ] && printf '%s ' "$d"; done)"
echo isolation-test >"$HOME/Downloads/.locachy-isolation-test"
r wrote "$?"
r dir_read "$(cat "$HOME/locachy-isolation-dir/from-host" 2>&1)"
echo container >"$HOME/locachy-isolation-dir/from-container"
r dir_wrote "$?"
r shadow "$(head -c1 /etc/shadow 2>&1 | head -1)"
r root_home "$(ls /root 2>&1 | head -1)"
EOF
chmod 755 "$out/check.sh"

export out src uid uhome
exec unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -c '
set -u
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
mount -t tmpfs -o mode=0755 tmpfs /run/user
mkdir -m 0700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
mount -t tmpfs -o mode=0755 tmpfs /run/locachy
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
    labwc -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >$out/labwc.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/wayland-0 ] && break; sleep 0.1; done
WAYLAND_DISPLAY=wayland-0 /usr/local/lib/locachy/labwc-control >$out/control.log 2>&1 &
for i in $(seq 50); do [ -S /run/user/0/locachy-0 ] && break; sleep 0.1; done
# The check script, on the root the container sees (a host path through the overlay).
app() { LOCACHY_CONT_BASE=$out/containers LOCACHY_CONT_CONF=$out/conf LOCACHY_APP_RUNTIME=/run/locachy/apps \
    bash $src/link/locachy-app "$@" </dev/null; }
dir=$uhome/locachy-isolation-dir
mkdir -p $out/conf $dir
echo host >$dir/from-host
chown -R $uid $dir
printf "directories=\"%s\"\n" "$dir" >$out/conf/isotest.conf
app start isotest $out/check.sh >$out/result 2>$out/app.log
echo "launcher exit $?"
v() { sed -n "s/^$1=//p" $out/result; }
check "runs as the user, uid 1000 ($(v user))" "[ \"$(v uid)\" = $uid ]"
check "no supplementary groups" "[ \"$(v groups)\" = \"$(id -g $uid)\" ]"
check "home is the user home path ($(v home))" "[ \"$(v home)\" = $uhome ]"
check "/var/lib/locachy is empty inside" "[ \"$(v var_lib_locachy | wc -w)\" = 0 ]"
check "another container data is out of reach" "v other_container | grep -q \"No such file\""
check "runtime: display and sound only ($(v runtime))" "[ \"$(v runtime)\" = \"./locachy-0 ./pipewire-0 ./pulse ./pulse/native \" ] || [ \"$(v runtime)\" = \"./locachy-0 \" ]"
check "its own session bus ($(v bus))" "[ \"$(v bus)\" = unix:path=/run/locachy-bus/bus ] && [ \"$(v bus_socket)\" = yes ]"
check "no bus of the host" "v host_buses | grep -q \"No such file\" && ! v host_buses | grep -q system_bus_socket"
check "no sound devices" "v snd | grep -q \"No such file\""
check "GPU render node only ($(v dri))" "v dri | grep -q renderD && ! v dri | grep -q card"
check "user-dirs.dirs names the 5 shared directories" "[ \"$(v user_dirs)\" = 5 ]"
check "the 5 shared directories are writable" "[ \"$(v shared)\" = \"Documents Downloads Music Pictures Videos \" ]"
check "a file written in Downloads is on the host, owned by the user" \
    "[ -f $uhome/Downloads/.locachy-isolation-test ] && [ \"$(stat -c %u $uhome/Downloads/.locachy-isolation-test 2>/dev/null)\" = $uid ]"
rm -f $uhome/Downloads/.locachy-isolation-test
check "its own directory is there ($(v dir_read)), and what it writes in it is on the host, owned by the user" \
    "[ \"$(v dir_read)\" = host ] && [ \"$(stat -c %u $dir/from-container 2>/dev/null)\" = $uid ]"
rm -rf $dir
for refused in /etc $uhome/Downloads; do
    printf "directories=\"%s\"\n" "$refused" >$out/conf/isotest.conf
    app start isotest /bin/true >/dev/null 2>$out/refused.log
    status=$?
    check "$refused is refused ($(cat $out/refused.log))" "[ $status != 0 ] && grep -q \"cannot have $refused\" $out/refused.log"
done
check "/etc/shadow and /root stay closed" "v shadow | grep -q denied && v root_home | grep -q denied"
check "the host passwd and group are unchanged" "sha256sum -c --quiet $out/host-ids.before"
[ $fail = 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; cat $out/result $out/app.log | head -40; }
rmdir /sys/fs/cgroup/app_isotest 2>/dev/null || true
exit $fail
'
