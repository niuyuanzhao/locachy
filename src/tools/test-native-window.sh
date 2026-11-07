#!/bin/sh
# Test double for a host program shown as a managed window, used only by
# test-app-registry.c (test-app-registry.sh): installed as view, files,
# settings and term. It records what process 1 gave it, says it has drawn its
# window on LOCACHY_WINDOW_READY_FD and keeps the requests from
# LOCACHY_REQUEST_FD. Its cgroup's files are the test's (no kernel).
set -eu
root=/run/locachy-registry-test
name=$LOCACHY_WINDOW_APP_ID
cg=$root/cgroup/app_$name
{
    printf 'program=%s\n' "${0##*/}"
    printf 'args=%s\n' "$*"
    printf 'backend=%s\n' "${LOCACHY_BACKEND:-}"
    printf 'display=%s/%s\n' "${XDG_RUNTIME_DIR:-}" "${WAYLAND_DISPLAY:-}"
    printf 'title=%s\n' "${LOCACHY_WINDOW_TITLE:-}"
    printf 'fds=%s %s\n' "${LOCACHY_WINDOW_READY_FD:-}" "${LOCACHY_REQUEST_FD:-}"
    printf 'control=%s\n' "${LOCACHY_CONTROL:-none}"
} >"$root/runtime/$name.env"
case "${1:-}" in
*fail-doc) echo "view: cannot open $1" >&2; exit 9 ;;
*silent-doc) exec 3>&-; sleep 1; exit 0 ;;
esac
printf 'populated 1\nfrozen 0\n' >"$cg/cgroup.events"
printf '%s\n' "$$" >"$cg/cgroup.procs"
quit() {
    printf 'populated 0\nfrozen 0\n' >"$cg/cgroup.events"
    exit 0
}
trap quit TERM
( exec 3>&-; while IFS= read -r line; do printf '%s\n' "$line" >>"$root/runtime/$name.requests"; done ) <&4 &
exec 4<&-
printf r >&3
exec 3>&-
while :; do
    sleep 1 &
    wait $! || true
done
