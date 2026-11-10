#!/bin/sh
# The programs locachy-jobs runs (locachy-pkg, locachy-repos, locachy-boot,
# locachy-app, locachy-firmware), for tools/test-jobs.sh: each records its
# start and end in $TEST_JOBS/calls (OVERLAP when another runs), its umask
# in $TEST_JOBS/umasks, and lists its processes in its job's fixture cgroup,
# as the kernel would. Its arguments say how it behaves:
#   slow      waits for $TEST_JOBS/release-ID
#   fail      exits 4
#   keys      lists a key in $TEST_JOBS/untrusted-keys (as locachy-build does)
#             and exits 1 (as locachy-pkg does then)
#   keys3     exits 3 (as locachy-build does for keys)
#   child     runs a child that stops on SIGINT (as pacman does) and waits
#             for it; a SIGINT to this one is recorded (TOOL-SIGINT)
#   deaf      ignores SIGINT, waits for $TEST_JOBS/release-ID
t=${TEST_JOBS:?}
name=${0##*/}
log=$(readlink "/proc/$$/fd/1")
id=${log##*/}
id=${id%.log}
procs=$t/cgroup/job-$id/cgroup.procs
echo "$$" >>"$procs"
# The disposition a SIGINT meets is set before the start is seen: the test that
# cancels a job as soon as it has started must not beat the trap.
case " $* " in
*" deaf "*) trap '' INT ;;
*" child "*) trap 'echo "TOOL-SIGINT $id" >>"$t/calls"' INT ;;
esac
[ ! -e "$t/running" ] || echo "OVERLAP $id $name $*" >>"$t/calls"
echo "$id" >"$t/running"
echo "start $id $name $* [$#]" >>"$t/calls"
# A step of a package-domain job inherits the domain's lock (JOBS_PKG_LOCK):
# its job id in $TEST_JOBS/domains.
for f in /proc/$$/fd/*; do
    [ "$(readlink "$f" 2>/dev/null)" = "$t/pkg.lock" ] && echo "$id" >>"$t/domains"
done
# A step of a domain job: the descriptor it inherits is the one held (flock on
# it succeeds, the same open file), and a fresh open of the lock is refused.
for f in /proc/$$/fd/*; do
    [ "$(readlink "$f" 2>/dev/null)" = "$t/pkg.lock" ] || continue
    n=${f##*/}
    if flock -n "$n" true 2>/dev/null; then inherited=held; else inherited=not-held; fi
    if flock -n "$t/pkg.lock" true 2>/dev/null; then fresh=open; else fresh=refused; fi
    echo "$id $inherited $fresh" >>"$t/lockuse"
done
# The descriptors this step has, as it starts.
echo "$id $(ls /proc/$$/fd | sort -n | tr '\n' ' ')" >>"$t/fds"
# As locachy-pkg does inside a job (locachy_transaction.hold_domain with
# inside_job): the domain its runner passed down, or a refusal without one.
if [ "$name" = locachy-app ] && [ "${1:-}" = build ] && [ -n "${TEST_SRC:-}" ]; then
    if python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import locachy_transaction as t; t.hold_domain(sys.argv[2], inside_job=True)' \
        "$TEST_SRC/link" "$t/pkg.lock" 2>/dev/null; then
        echo "$id held" >>"$t/pkguse"
    else
        echo "$id refused" >>"$t/pkguse"
    fi
fi
# What the service leaves of a lock path override in the environment (none).
printf '%s %s\n' "$id" "${LOCACHY_PKG_LOCK:-}" >>"$t/lockenv"
# A copy the service made at acceptance: its digest, as the tool sees it.
for a in "$@"; do
    case $a in
    *.input) echo "$id $(sha256sum "$a" | cut -d" " -f1) $a" >>"$t/images" ;;
    esac
done
echo "$id $(umask)" >>"$t/umasks"
echo "$name: working on $*"
status=0
if [ "$name" = locachy-tpm ] && [ "${1:-}" = enroll ] && [ "${2:-}" = --passphrase-stdin ]; then
    sha256sum >"$t/input-$id.sha256"
fi
case " $* " in
*" slow "*)
    until [ -e "$t/release-$id" ]; do sleep 0.05; done
    ;;
*" deaf "*)
    trap '' INT
    until [ -e "$t/release-$id" ]; do sleep 0.05; done
    ;;
*" child "*)
    trap 'echo "TOOL-SIGINT $id" >>"$t/calls"' INT
    # SIGINT as a program started by the step has it (sh ignores it in what
    # it starts with &).
    env --default-signal=INT sh -c 'trap "echo child interrupted; exit 130" INT; echo $$ >>"$1"
        while :; do sleep 0.05; done' sh "$procs" &
    child=$!
    while :; do
        wait "$child"
        status=$?
        kill -0 "$child" 2>/dev/null || break
    done
    echo "child ended with $status"
    status=1
    ;;
*" fail "*)
    status=4
    ;;
*" keys "*)
    echo "lyrebird-proxy 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C" >"$t/untrusted-keys"
    status=1
    ;;
*" keys3 "*)
    status=3
    ;;
esac
rm -f "$t/running"
echo "end $id $name $status" >>"$t/calls"
echo "$name: last line of job $id"
exit "$status"
