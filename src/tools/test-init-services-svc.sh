#!/bin/sh
# A service for test-init-services.c: test-init-services-svc.sh DIR TAG. It
# says what it does on the descriptor $SVC_EVFD ("EVENT TAG PID"), each
# signal disposition set before it says start (child-ready). On TERM it
# cleans up until a line is written to DIR/gate-TAG (a FIFO; at once without
# one).
#   TAG one    the cleanup is its own (TERM trapped), then it ends
#   TAG tree   it ends at once on TERM; a child of its session cleans up
d=$1 tag=$2
ev=${SVC_EVFD:?}
if [ "$tag" = tree ]; then
    sh -c 'trap "echo child-term $1 \$\$ >&$3; { read -r _ <\"$2\"; } 2>/dev/null; echo child-end $1 \$\$ >&$3; exit 0" TERM
        echo "child-ready $1 $$" >&"$3"
        while :; do sleep 1 & wait $!; done' sh "$tag" "$d/gate-$tag" "$ev" &
    trap 'exit 0' TERM
else
    trap 'echo "term $tag $$" >&"$ev"; { read -r _ <"$d/gate-$tag"; } 2>/dev/null; echo "end $tag $$" >&"$ev"; exit 0' TERM
fi
echo "start $tag $$" >&"$ev"
while :; do
    sleep 1 &
    wait $!
done
