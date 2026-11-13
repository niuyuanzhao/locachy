#!/bin/sh
# The locachy-app double for test-apps-lifecycle.c: process 1's application
# operations (init/apps.c) run it as they run locachy-app ACTION NAME [TARGET].
# It says what it does on the descriptor $TEST_APPS_EVFD, one line an event
# ("EVENT NAME PID [EXTRA]"), and waits at $TEST_APPS/gate-NAME-STEP, a FIFO,
# while the test has made one (until the test writes a line to it). The
# fixture cgroup $TEST_APPS/cgroup/app_NAME lists in cgroup.procs who has
# entered it; the test derives cgroup.events from whether they live, as the
# kernel would.
#   start, open   a launcher: a helper in its process group (as one waiting
#                 for the app's lock behind a build), gate enter, then into
#                 app_NAME with the application (which ends 0 once a line is
#                 written to app-exit-NAME), gate ready, NAME.ready, waits for
#                 the application, leaves app_NAME, gate cleanup, ends with
#                 the application's status
#   open (runs)   hands its target on: gate open
#   stop          gate stop, TERM to the application, then KILL to what is
#                 left in app_NAME after 1 s (as request_term and cgroup.kill)
#   freeze        gate freeze, cgroup.freeze 1; resume: gate resume, 0
fx=${TEST_APPS:?}
ev=${TEST_APPS_EVFD:?}
run=${TEST_APPS_RUN:?}
action=$1 name=$2
cg=$fx/cgroup/app_$name
note() { echo "$1 $name $$ ${2:-}" >&"$ev"; }
gate() {
    g=$fx/gate-$name-$1
    [ -p "$g" ] || return 0
    note "gate-$1"
    read -r _ <"$g"
}
alive() {
    [ -r "/proc/$1/stat" ] || return 1
    read -r line <"/proc/$1/stat" 2>/dev/null || return 1
    s=${line##*) }
    [ "${s%% *}" != Z ]
}
running() {
    for p in $(cat "$cg/cgroup.procs" 2>/dev/null); do
        alive "$p" && return 0
    done
    return 1
}
settle() { # up to 1 s for app_NAME to empty
    i=0
    while running && [ "$i" -lt 50 ]; do sleep 0.02; i=$((i + 1)); done
}

case $action in
start | open)
    if [ "$action" = open ] && running; then
        note open-running
        gate open
        exit 0
    fi
    note start
    sleep 1000 &
    helper=$!
    note helper "$helper"
    gate enter
    kill "$helper" 2>/dev/null
    if running; then
        note already
        exit 0
    fi
    echo "$$" >>"$cg/cgroup.procs"
    sh -c 'read -r _ <"$1"; exit 0' sh "$fx/app-exit-$name" &
    app=$!
    echo "$app" >>"$cg/cgroup.procs"
    echo "$app" >"$fx/app-pid-$name"
    note app "$app"
    gate ready
    touch "$run/$name.ready"
    note ready
    status=0
    wait "$app" || status=$?
    sed -i "/^$$\$/d" "$cg/cgroup.procs" # out of app_NAME, as locachy-app moves to its caller's cgroup
    gate cleanup
    rm -f "$run/$name.ready"
    note cleanup-end "$status"
    exit "$status"
    ;;
stop)
    note stop
    gate stop
    [ ! -r "$fx/app-pid-$name" ] || kill -TERM "$(cat "$fx/app-pid-$name")" 2>/dev/null
    settle
    for p in $(cat "$cg/cgroup.procs" 2>/dev/null); do kill -KILL "$p" 2>/dev/null; done
    settle
    rm -f "$run/$name.ready"
    echo 0 >"$cg/cgroup.freeze"
    note stop-end
    ;;
freeze)
    note freeze
    gate freeze
    echo 1 >"$cg/cgroup.freeze"
    note freeze-end
    ;;
resume)
    note resume
    gate resume
    echo 0 >"$cg/cgroup.freeze"
    note resume-end
    ;;
*)
    note unknown "$action"
    exit 2
    ;;
esac
exit 0
