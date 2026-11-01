#!/bin/sh
# Test double used only by test-app-registry.c.
set -eu
root=/run/locachy-registry-test
name=$2
cg="$root/cgroup/app_$name"
run() {
    [ "$name" != broken ] || exit 23
    printf 'populated 1\nfrozen 0\n' >"$cg/cgroup.events"
    printf '1048576\n' >"$cg/memory.current"
    printf '0\n' >"$cg/memory.swap.current"
    printf '%s\n' "$$" >"$root/runtime/$name.pid"
    touch "$root/runtime/$name.ready"
    exec sleep 20
}
case "$1" in
start) run ;;
open)
    # What it was given; a running container takes it and this ends.
    printf '%s\n' "$3" >>"$root/runtime/$name.opened"
    grep -q 'populated 1' "$cg/cgroup.events" || run
    ;;
freeze) printf 'populated 1\nfrozen 1\n' >"$cg/cgroup.events" ;;
resume) printf 'populated 1\nfrozen 0\n' >"$cg/cgroup.events" ;;
stop)
    printf 'populated 0\nfrozen 0\n' >"$cg/cgroup.events"
    rm -f "$root/runtime/$name.ready"
    kill "$(cat "$root/runtime/$name.pid")"
    ;;
*) exit 2 ;;
esac
