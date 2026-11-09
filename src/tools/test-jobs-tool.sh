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
[ ! -e "$t/running" ] || echo "OVERLAP $id $name $*" >>"$t/calls"
echo "$id" >"$t/running"
echo "start $id $name $* [$#]" >>"$t/calls"
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
