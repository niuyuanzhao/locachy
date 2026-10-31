#!/bin/sh
# test-app-init.sh OUT: the container's process 1 (progs/app-init.c) in a
# PID namespace of a user namespace: a stop (TERM) reaches the main
# application alone, and its helpers (one in a session of its own) only once
# it has exited; the main application's status is app-init's. No root.
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-app-init.sh OUT}
rm -rf "$out"
mkdir -p "$out"
cc -O2 -Wall -Wextra -Werror -o "$out/app-init" "$src/progs/app-init.c" || { echo "FAIL app-init does not build"; exit 1; }
log=$out/log
cat >"$out/app" <<'EOF'
#!/bin/sh
setsid sh -c 'trap "echo helper-term >>$LOG; exit 0" TERM; while :; do sleep 0.05; done' &
trap 'echo main-term >>$LOG; sleep 0.5; echo main-exit >>$LOG; exit 7' TERM
echo ready >>$LOG
while :; do sleep 0.05; done
EOF
chmod 755 "$out/app"
fail=0
LOG=$log unshare --map-root-user --pid --fork --mount-proc "$out/app-init" "$out/app" &
outer=$!
for _ in $(seq 100); do
    grep -q ready "$log" 2>/dev/null && break
    sleep 0.05
done
sleep 0.2 # the helper's trap is set
init=$(pgrep -P "$outer" | head -n 1)
[ -n "$init" ] && kill -TERM "$init"
status=0
wait "$outer" || status=$?
order=$(tr '\n' ' ' <"$log")
if [ "$order" = "ready main-term main-exit helper-term " ]; then
    echo "PASS a stop reaches the main application first, its helpers once it has exited"
else
    echo "FAIL the order was: $order"
    fail=1
fi
if [ "$status" = 7 ]; then
    echo "PASS app-init exits with the main application's status"
else
    echo "FAIL app-init exited $status, not 7"
    fail=1
fi
[ "$fail" = 0 ] && echo "ALL PASSED"
exit "$fail"
