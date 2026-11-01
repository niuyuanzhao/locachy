#!/bin/bash
# Exercise the actual freeze/stop functions with fake cgroup files and a
# clipboard double. No real cgroup, display, input or clipboard is touched.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d /var/tmp/locachy-clipboard-freeze.XXXXXX)
trap 'rm -rf "$out"' EXIT
# Only definitions; never execute the launch/control dispatch of locachy-app.
sed '/^operation=start/,$d' "$src/link/locachy-app" >"$out/functions.sh"
cat >"$out/clipboard" <<'EOF'
#!/bin/sh
set -eu
case "$1" in
freeze)
    test "$2" = "$TEST_CG"
    test "$(cat "$TEST_CG/cgroup.freeze")" = 0
    test "$TEST_MODE" != fail-sync || exit 1
    echo 1 >"$TEST_CG/cgroup.freeze"
    if [ "$TEST_MODE" = fail-check ]; then echo 0 >"$TEST_CG/cgroup.freeze"; exit 1; fi
    ;;
sync)
    test "$TEST_MODE" != fail-sync || exit 1
    ;;
esac
EOF
chmod 755 "$out/clipboard"
mkdir -p "$out/cg" "$out/runtime/demo"
export TEST_CG=$out/cg
export TEST_MODE=ok
export LOCACHY_CLIPBOARD_TOOL=$out/clipboard
export LOCACHY_APP_RUNTIME=$out/runtime
# shellcheck source=/dev/null
source "$out/functions.sh"
cg_path() { printf '%s\n' "$TEST_CG"; }
is_running() { return 0; }
is_frozen() { [ "$(cat "$TEST_CG/cgroup.freeze")" = 1 ]; }
wait_event() { [ "$(cat "$TEST_CG/cgroup.freeze")" = "$3" ]; }
echo 0 >"$TEST_CG/cgroup.freeze"
echo 0 >"$TEST_CG/memory.current"
do_freeze demo
test "$(cat "$TEST_CG/cgroup.freeze")" = 1
echo 'PASS container freeze delegates the handover to the compositor-gated helper'
for mode in fail-sync fail-check; do
    echo 0 >"$TEST_CG/cgroup.freeze"
    export TEST_MODE=$mode
    if (do_freeze demo) >"$out/$mode.log" 2>&1; then exit 1; fi
    test "$(cat "$TEST_CG/cgroup.freeze")" = 0
    echo "PASS $mode leaves the source unfrozen"
done
export TEST_MODE=fail-sync
echo 1 >"$TEST_CG/cgroup.kill"
request_term() { echo terminated >"$out/terminated"; }
wait_event() { return 0; }
do_stop demo >"$out/stop.log" 2>&1
test -f "$out/terminated"
grep -q 'clipboard was not saved' "$out/stop.log"
echo 'PASS a failed clipboard transfer warns but does not prevent stopping'
echo 'ALL PASSED'
