#!/bin/bash
# test-builder.sh: the build root (locachy-build), as root on a built system
# with the network (a few minutes: it builds mihomo, written in Go, and
# pipes.sh). What a build sees:
#   - it runs as locachy-builder, its HOME its home
#   - nothing of the host's own state: /home, /root, /var/lib/locachy but its
#     home and the hand-over; root's cache read-only; /run, /tmp its own; of
#     /dev, no disks, no screen, no input
# What a build leaves:
#   - Go's caches in the build user's home, nothing set for them
#   - nothing new on the host's root outside /var, /home, /tmp (not in /,
#     /root, /etc, /usr, /opt); the host's packages unchanged
#   - root's cache root's, which the build user cannot write; no root of the
#     run left; no package twice in a build root's database
set -u
b=/usr/local/bin/locachy-build
user=locachy-builder
home=/var/lib/locachy/builder/home
cache=/var/cache/locachy-pkgs
log=/var/tmp/test-builder
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
v() { sed -n "s/^$1=//p" $log.seen; }

$b --run sh -c 'echo user=$(id -un); echo home=$HOME; echo homes=$(ls -A /home | wc -l)
    echo root=$(ls -A /root 2>/dev/null | wc -l); echo state=$(cd /var/lib/locachy && find . -maxdepth 2 | sort | tr "\n" " ")
    touch /var/cache/locachy-pkgs/x 2>/dev/null && echo cache=written || echo cache=read-only
    echo run=$(ls -A /run | tr "\n" " "); echo tmp=$(ls -A /tmp | wc -l); echo dev=$(ls /dev | tr "\n" " ")
    echo conf=$(ls /etc/makepkg.conf.d 2>/dev/null | tr "\n" " ")' >$log.seen 2>&1
check "it runs as $user ($(v user)), HOME its home ($(v home))" "[ \"$(v user)\" = $user ] && [ \"$(v home)\" = $home ]"
check "/home and /root show nothing ($(v homes), $(v root))" "[ \"$(v homes)\" = 0 ] && [ \"$(v root)\" = 0 ]"
check "/var/lib/locachy: its home and the hand-over only ($(v state))" \
    "[ \"$(v state)\" = '. ./builder ./builder/home ./builder/out' ]"
check "root's cache read-only to it" "[ \"$(v cache)\" = read-only ] && [ ! -e $cache/x ]"
check "/run and /tmp its own (run: $(v run))" "! v run | grep -q -e locachy -e user -e dbus && [ \"$(v tmp)\" = 0 ]"
check "/dev: no disks, screen or input" "! v dev | grep -q -w -e sda -e dri -e input -e kmsg -e fb0"
check "no makepkg settings of ours, nothing set for Go (there: $(v conf))" "! v conf | grep -q locachy"

marker=$log.marker
touch $marker
sleep 1
packages=$(pacman -Q | sha256sum)
$b -r mihomo >$log.mihomo 2>&1
status=$?
check "mihomo (Go) built" "[ $status = 0 ] && ls $cache/mihomo-[0-9]*.pkg.tar.zst >/dev/null 2>&1"
check "Go's cache in the build user's home, the build user's" \
    "[ -d $home/.cache/go-build ] && [ \"$(stat -c %U $home/.cache/go-build 2>/dev/null)\" = $user ]"
$b pipes.sh >$log.pipes 2>&1
check "pipes.sh built after it" "[ $? = 0 ]"
check "no package twice in a build root's database" "! grep -q 'duplicated database entry' $log.mihomo $log.pipes"
new=$(find / -xdev \( -path /var -o -path /home -o -path /tmp \) -prune -o -newer $marker -print 2>/dev/null | head -5)
check "nothing new on the host's root but in /var, /home, /tmp ($new)" "[ -z \"$new\" ]"
check "the host's packages unchanged" "[ \"$(pacman -Q | sha256sum)\" = \"$packages\" ]"
check "root's cache all root's" "[ -z \"$(find $cache \( ! -user root -o ! -group root \) | head -1)\" ]"
check "the build user cannot write in it" \
    "! setpriv --reuid=$user --regid=$user --init-groups touch $cache/x 2>/dev/null && [ ! -e $cache/x ]"
check "no root of a run left; the builder has its home and its lock only ($(ls /var/lib/locachy/builder | tr '\n' ' '))" \
    "[ \"$(ls /var/lib/locachy/builder | tr '\n' ' ')\" = 'home lock ' ]"

rm -f $marker
[ $fail = 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; tail -n 5 $log.mihomo $log.pipes; }
exit $fail
