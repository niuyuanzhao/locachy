#!/bin/bash
# locachy-app wait_ready: the launcher's "ready" line on a FIFO held open both
# ways, as locachy-app holds it. A line split across a read timeout is put
# together; a launcher that ends first is told apart from one that never
# answers; a line written just before the launcher ended still counts.
set -euo pipefail
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-app-ready.sh NEW_OUTPUT_DIRECTORY}
[[ "$out" = /* && ! -e "$out" ]] || { echo 'Output must be a new absolute directory' >&2; exit 2; }
mkdir -p "$out"
# The function itself, from the script.
sed -n '/^wait_ready() { # FD PID$/,/^}$/p' "$src/link/locachy-app" >"$out/wait_ready.sh"
grep -q 'ready_why=ended' "$out/wait_ready.sh" || { echo 'FAIL wait_ready not found in locachy-app'; exit 1; }
source "$out/wait_ready.sh"

fifo() { # a fresh FIFO open both ways in FD
    rm -f "$out/ready"
    mkfifo "$out/ready"
    exec {FD}<>"$out/ready"
}
check() { # WANT_STATUS WANT_WHY MAX_SECONDS LABEL
    local status=0 start=$SECONDS
    wait_ready "$FD" "$launcher" || status=$?
    exec {FD}>&-
    if [ "$status" != "$1" ] || { [ "$1" != 0 ] && [ "$ready_why" != "$2" ]; } || ((SECONDS - start > $3)); then
        printf 'FAIL %s: status %s, %s, %s s\n' "$4" "$status" "$ready_why" "$((SECONDS - start))"
        exit 1
    fi
}

# Split by the timer: "rea", then "dy" after more than one 50 ms read.
fifo
( printf rea >&"$FD"; sleep 0.12; printf 'dy\n' >&"$FD"; sleep 2 ) & launcher=$!
check 0 - 1 'a line split across a read timeout'
kill "$launcher"; wait "$launcher" 2>/dev/null || true

# Ended before writing anything: told at once, not as six seconds.
fifo
( sleep 0.2; exit 3 ) & launcher=$!
check 1 ended 1 'a launcher that ended first'
status=0; wait "$launcher" || status=$?
[ "$status" = 3 ] || { echo "FAIL the ended launcher's status: $status"; exit 1; }

# Ready, then gone at once: still ready.
fifo
( printf 'ready\n' >&"$FD" ) & launcher=$!
wait "$launcher"
check 0 - 1 'ready written just before the launcher ended'

# Something else written.
fifo
( printf 'nope\n' >&"$FD"; sleep 2 ) & launcher=$!
check 1 line 1 'a malformed line'
kill "$launcher"; wait "$launcher" 2>/dev/null || true

# Alive and silent: the full six seconds.
fifo
sleep 8 & launcher=$!
start=$SECONDS
check 1 timeout 8 'a silent launcher'
((SECONDS - start >= 5)) || { echo 'FAIL a silent launcher was given up early'; exit 1; }
kill "$launcher"; wait "$launcher" 2>/dev/null || true
rm -f "$out/ready"
echo 'PASS wait_ready: a line split by its read timeout is joined; a launcher that ends first is told with its status, not as a timeout; ready just before the end counts; malformed and silent are refused'
