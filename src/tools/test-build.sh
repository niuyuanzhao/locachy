#!/bin/bash
# test-build.sh: a container built from its configuration (locachy-app build),
# as root on a built system with the network: a throwaway container, hello,
# with a repository package (sl) and an AUR one (pipes.sh, built by pikaur, an
# extra; locachy-pkg info tells where each is, locachy-pkg commands what it runs);
# built again with other packages (pipes.sh, cowsay), the old build kept;
# rolled back and forward. It is removed at the end (its configuration in
# /etc/locachy/containers, its state in /var/lib/locachy/containers).
set -u
C=/etc/locachy/containers/hello.conf
S=/var/lib/locachy/containers/hello
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
cleanup() { rm -f $C; rm -rf $S; rmdir /sys/fs/cgroup/app_hello 2>/dev/null; true; }
[ ! -e $C ] && [ ! -e $S ] || { echo "a container hello is there already"; exit 1; }
# What the container has, as its own root sees it: "sl=VERSION pipes.sh=... cowsay=...".
has() {
    unshare --mount --propagation private bash -c "
        mount -t overlay overlay -o lowerdir=/,upperdir=$S/upper,workdir=$S/work $S/merged
        for p in sl pipes.sh cowsay; do printf \"%s=%s \" \$p \"\$(chroot $S/merged pacman -Q \$p 2>/dev/null | cut -d\" \" -f2)\"; done
    "
}
v() { has | tr " " "\n" | sed -n "s/^$1=//p"; }

info=$(locachy-pkg info sl pipes.sh no-such-package-locachy)
kind() { printf '%s\n' "$info" | awk -F '\t' -v p="$1" '$1 == p { print $2 }'; }
check "info: sl from a repository, pipes.sh from the AUR, the third nowhere ($(kind sl) $(kind pipes.sh) $(kind no-such-package-locachy))" \
    "[ \"$(kind sl)\" = repo ] && [ \"$(kind pipes.sh)\" = aur ] && [ \"$(kind no-such-package-locachy)\" = none ]"
printf 'package="sl"\nextras="pipes.sh"\ncommand="/usr/bin/sl"\n' >$C
locachy-app build hello </dev/null >/var/tmp/test-build.1 2>&1
check "built: sl from the repositories, pipes.sh from the AUR ($(has))" "[ -n \"$(v sl)\" ] && [ -n \"$(v pipes.sh)\" ] && [ -z \"$(v cowsay)\" ]"
check "its build is recorded" "grep -qx 'packages=\"sl pipes.sh\"' $S/built"
check "what it can run: sl, pipes.sh ($(locachy-pkg commands hello | cut -f1 | tr '\n' ' '))" \
    "locachy-pkg commands hello | cut -f1 | grep -qx /usr/bin/sl && locachy-pkg commands hello | cut -f1 | grep -qx /usr/bin/pipes.sh"
check "no previous build yet" "[ ! -d $S/upper.prev ]"

printf 'package="pipes.sh"\nextras="cowsay"\ncommand="/usr/bin/pipes.sh"\n' >$C
locachy-app build hello </dev/null >/var/tmp/test-build.2 2>&1
check "built again: pipes.sh and cowsay, no sl ($(has))" "[ -z \"$(v sl)\" ] && [ -n \"$(v pipes.sh)\" ] && [ -n \"$(v cowsay)\" ]"
check "the previous build kept, with its record" "[ -d $S/upper.prev ] && grep -q sl $S/built.prev"

locachy-app rollback hello </dev/null >/dev/null 2>&1
check "rolled back: sl again ($(has))" "[ -n \"$(v sl)\" ] && [ -z \"$(v cowsay)\" ] && grep -q sl $S/built"
locachy-app rollback hello </dev/null >/dev/null 2>&1
check "rolled forward: cowsay again ($(has))" "[ -z \"$(v sl)\" ] && [ -n \"$(v cowsay)\" ] && grep -q cowsay $S/built"

cleanup
[ $fail = 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; tail -5 /var/tmp/test-build.1 /var/tmp/test-build.2; }
exit $fail
