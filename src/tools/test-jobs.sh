#!/bin/bash
# test-jobs.sh NEW_OUTPUT: locachy-jobs, the system jobs' queue
# (tools/JOBS.md), without a device: as root of a user namespace with a
# private /run, its cgroups and the peers' /proc/PID/cgroup as fixture files,
# and test-jobs-tool.sh in place of the programs the jobs run. The client is
# test-jobs.c through base/jobs.c, the programs' helper. Checked: the kinds
# and their arguments, the steps' umask, the queue (one at a time, in
# order), the peers, a job
# outliving the program that asked, cancelling (queued; running only when
# confirmed, SIGINT then kill), the log and state files, the service ended or
# killed and started again. Once with the service as built, once with
# AddressSanitizer and UndefinedBehaviorSanitizer.
set -u
[ "$#" = 1 ] || { echo "usage: test-jobs.sh NEW_OUTPUT" >&2; exit 2; }
out=$(realpath -m "$1")
[ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
mkdir -p "$out"
src=$(cd "$(dirname "$0")/.." && pwd)
run=/run/test-jobs
t=$run/t
defs=(-DJOBS_DIR="\"$t/jobs\"" -DJOBS_CGROUP="\"$t/cgroup\"" -DJOBS_PROC="\"$t/proc\"" -DJOBS_BIN="\"$t/bin\""
      -DJOBS_UNTRUSTED_KEYS="\"$t/untrusted-keys\"" -DJOBS_STOP_GRACE=2)
cc=(gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/tools" "${defs[@]}")
"${cc[@]}" "$src/progs/locachy-jobs.c" -o "$out/locachy-jobs" || exit 1
"${cc[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer "$src/progs/locachy-jobs.c" \
    -o "$out/locachy-jobs-san" || exit 1
"${cc[@]}" "$src/tools/test-jobs.c" "$src/base/jobs.c" -o "$out/test-jobs" || exit 1

inner='
set -u
src=$1 run=$2 t=$3 server=$4 client=$5
mount -t tmpfs -o mode=0755 tmpfs /run || exit 9
mkdir -p "$t/cgroup/service" "$t/proc" "$t/bin" "$t/files"
touch "$t/cgroup/service/cgroup.procs"
for i in $(seq 1 60); do
    mkdir "$t/cgroup/job-$i"
    touch "$t/cgroup/job-$i/cgroup.procs" "$t/cgroup/job-$i/cgroup.kill"
    echo "populated 0" >"$t/cgroup/job-$i/cgroup.events"
done
install -m 755 "$src/tools/test-jobs-tool.sh" "$run/tool"
for p in locachy-pkg locachy-repos locachy-boot locachy-app locachy-firmware; do ln -s "$run/tool" "$t/bin/$p"; done
install -m 755 "$server" "$run/locachy-jobs"
install -m 755 "$client" "$run/test-jobs"
export TEST_JOBS=$t
pass=0 fail=0
check() { local name=$1; shift; if "$@"; then echo "PASS $name"; pass=$((pass + 1)); else echo "FAIL $name"; fail=$((fail + 1)); fi; }
J() { "$run/test-jobs" "$@"; }
svc=
start_service() {
    "$run/locachy-jobs" 2>>"$t/service.log" &
    svc=$!
    for _ in $(seq 100); do [ -S "$t/jobs/control" ] && return 0; sleep 0.05; done
    return 1
}
field() { J status "$1" | cut -f"$2"; }
wait_state() { # ID STATE...: up to 15 s
    local id=$1; shift
    for _ in $(seq 300); do
        local s; s=$(field "$id" 3)
        for want in "$@"; do [ "$s" = "$want" ] && return 0; done
        sleep 0.05
    done
    echo "job $id: $(J status "$id")" >&2
    return 1
}
idle() { for _ in $(seq 300); do [ "$(J pending)" = "0 0" ] && return 0; sleep 0.05; done; return 1; }
id_of() { set -- $1; [ "$1" = ok ] && echo "$2"; }
lines() { grep -c "$1" "$t/calls" || true; }
started() { for _ in $(seq 100); do grep -q "^start $1 " "$t/calls" && return 0; sleep 0.05; done; return 1; }

check "the service starts and listens" start_service
check "the control socket is 0600, its folder 0700" \
    test "$(stat -c %a "$t/jobs/control") $(stat -c %a "$t/jobs")" = "600 700"

# The kinds and their arguments.
touch "$t/files/fw.cab" "$t/files/my fw.cab"
head -c 4096 /dev/zero >"$t/files/rom.bin"
rom=$t/files/rom.bin
refuse() { local r; r=$(J submit "$@"); case $r in error*) return 0;; esac; echo "accepted: $* -> $r" >&2; return 1; }
accept() { local r; r=$(J submit "$@"); case $r in ok*) return 0;; esac; echo "refused: $* -> $r" >&2; return 1; }
check "an unknown kind is refused" refuse nope
check "a package name that is an option is refused" refuse pkg-install -rf
check "a package name with ; is refused" refuse pkg-install "a;b"
check "a package name with a space is refused" refuse pkg-install "a b"
check "no package is refused" refuse pkg-install
check "33 packages are refused" refuse pkg-install $(seq -f p%g 33)
check "a swap needs two packages" refuse pkg-swap only
check "a kernel that is not linux* is refused" refuse kernel-select bash
check "headers take on or off only" refuse kernel-headers maybe
check "a repository section that is not settings'"'"' is refused" refuse repos-set core=maybe
check "a kind without arguments takes none" refuse pkg-sync extra
check "the boot image takes no arguments" refuse boot --now
check "a container name with a slash is refused" refuse app-build ../x
check "a container removal takes --data only" refuse app-remove alpha --force
check "a firmware archive that is not there is refused" refuse fw-local-install /nonexistent.cab
check "a relative firmware archive is refused" refuse fw-local-install files/fw.cab
check "a release from ftp is refused" refuse fw-install-release dev ftp://example.org/x.cab
check "a release with a space is refused" refuse fw-install-release dev "https://example.org/x y.cab"
check "an SPI region list with an empty region is refused" refuse spi-write internal "$rom" --regions "fd,,bios"
check "an SPI probe takes no regions" refuse spi-probe internal --regions fd
check "an SPI backup goes into a folder" refuse spi-backup internal "$rom"
check "an SPI option given twice is refused" refuse spi-verify internal "$rom" --chip A --chip B
check "an SPI option without its value is refused" refuse spi-verify internal "$rom" --chip
check "65 arguments are refused" refuse pkg-apply $(seq -f c%g 65)
check "a package install is taken" accept pkg-install foo lib32-mesa python-dlib noto-fonts-cjk
check "a swap is taken" accept pkg-swap python-dlib python-dlib-cuda
check "a kernel is taken" accept kernel-select linux-cachyos-bore
check "headers are taken" accept kernel-headers on
check "a repository section is taken" accept repos-set extra=off
check "a container removal with its data is taken" accept app-remove alpha.1 --data
check "a firmware archive with a space in its path is taken" accept fw-local-install "$t/files/my fw.cab"
check "a release by its address is taken" accept fw-install-release a45df35ac0e9 https://example.org/x.cab
check "an SPI write with every option is taken" accept spi-write ch341a_spi "$rom" --chip W25Q64.V \
    --regions fd,bios --expected-size 4096
check "an SPI backup into a folder is taken" accept spi-backup "linux_spi:dev=/dev/spidev0.0" "$t/files"
check "the boot image is taken" accept boot
check "the jobs above ran" idle
check "the programs ran as their kinds say, an argument each, no shell" grep -q \
    "locachy-pkg install foo lib32-mesa python-dlib noto-fonts-cjk \[5\]" "$t/calls"
check "a swap removes, then installs" sh -c "grep -A2 \"locachy-pkg remove python-dlib \[2\]\" \"$t/calls\" | grep -q \"locachy-pkg install python-dlib-cuda \[2\]\""
check "a kernel: locachy-pkg kernel select" grep -q "locachy-pkg kernel select linux-cachyos-bore \[3\]" "$t/calls"
check "a section: locachy-repos apply --set" grep -q "locachy-repos apply --set extra=off \[3\]" "$t/calls"
check "a path with a space stays one argument" grep -q "fwupd-action local-install $t/files/my fw.cab \[3\]" "$t/calls"
check "an SPI write: its options as given" grep -q \
    "spi-write ch341a_spi $t/files/rom.bin --chip W25Q64.V --regions fd,bios --expected-size 4096 \[9\]" "$t/calls"
check "the boot image: locachy-boot alone" grep -q "locachy-boot  \[0\]" "$t/calls"
# The service keeps its own files to root (umask 077); the files a step
# writes (mirror lists, pacman.conf, rc.conf) are read by others too.
check "every step runs with umask 022, not the umask of the service" \
    sh -c "[ -s \"$t/umasks\" ] && ! grep -qv \" 0022\$\" \"$t/umasks\""
check "a malformed request (a trailing NUL) is refused" sh -c "\"$run/test-jobs\" raw submit pkg-sync \"\" | grep -q \"malformed\""
check "an unknown request is refused" sh -c "\"$run/test-jobs\" raw frobnicate | grep -q \"unknown request\""
check "an unknown job is refused" sh -c "\"$run/test-jobs\" cancel 99999 | grep -q \"no such job\""

# One at a time, in order.
a=$(id_of "$(J submit pkg-install slow)")
b=$(id_of "$(J submit pkg-remove fail)")
c=$(id_of "$(J submit repos-rank)")
check "the first runs at once, the others wait in order" \
    test "$(field "$a" 3) $(field "$a" 5) $(field "$b" 3) $(field "$b" 5) $(field "$c" 5)" = "running 0 queued 1 2"
check "the same job asked again while queued is that one" test "$(J submit pkg-remove fail)" = "ok $b 1"
touch "$t/release-$a"
check "the queue runs out" wait_state "$c" done
check "each ended as its program did" test "$(field "$a" 3)/$(field "$a" 4) $(field "$b" 3)/$(field "$b" 4)" = "done/0 failed/4"
check "never two at a time" test "$(lines OVERLAP)" = 0
check "in the order asked" sh -c "grep -E \"^(start|end) ($a|$b|$c) \" \"$t/calls\" | cut -d\" \" -f1,2 | tr \"\n\" \" \" | grep -qx \"start $a end $a start $b end $b start $c end $c \""
check "a row says who asked" test "$(field "$a" 6)" = test-jobs
check "a row has the last line of the output" test "$(field "$a" 8)" = "locachy-pkg: last line of job $a"

# A build stopped for source signing keys: not a failure.
k1=$(id_of "$(J submit pkg-apply host keys)")
check "an apply that stopped for keys (untrusted-keys, status 1): waiting for keys" wait_state "$k1" keys
rm -f "$t/untrusted-keys"
k2=$(id_of "$(J submit app-build keys3)")
check "a build that exits 3: waiting for keys" wait_state "$k2" keys
k3=$(id_of "$(J submit fw-verify keys3)")
check "status 3 of a program that builds nothing: failed" wait_state "$k3" failed
echo "old KEY" >"$t/untrusted-keys"; touch -d "2000-01-01" "$t/untrusted-keys"
k4=$(id_of "$(J submit pkg-install fail)")
check "keys listed before the job: failed" wait_state "$k4" failed
rm -f "$t/untrusted-keys"
check "waiting for keys is an end: the queue went on" idle

# The log and the state.
check "the state file is root'"'"'s alone" test "$(stat -c %a "$t/jobs/$a.state")" = 600
check "the log is root'"'"'s alone" test "$(stat -c %a "$t/jobs/$a.log")" = 600
check "the state file says how it ended" grep -qx "state=failed" "$t/jobs/$b.state"
check "log ID: its output" sh -c "\"$run/test-jobs\" log $a | grep -q \"working on install slow\""

# A job outlives the program that asked.
"$run/test-jobs" hold pkg-install slow >"$t/hold.out" &
holder=$!
for _ in $(seq 100); do [ -s "$t/hold.out" ] && break; sleep 0.05; done
h=$(id_of "$(cat "$t/hold.out")")
kill -KILL "$holder"; wait "$holder" 2>/dev/null
check "the program that asked has gone; its job runs" test "$(field "$h" 3)" = running
touch "$t/release-$h"
check "and ends as it would have" wait_state "$h" done

# Cancelling.
x=$(id_of "$(J submit pkg-install slow)")
y=$(id_of "$(J submit pkg-sync)")
check "a queued job: removed" test "$(J cancel "$y")" = "ok removed"
check "removed, it is cancelled" test "$(field "$y" 3)" = cancelled
check "a running job: not without confirm" sh -c "\"$run/test-jobs\" cancel $x | grep -q \"^confirm .*half-changed\""
check "not confirmed, it runs on" test "$(field "$x" 3)" = running
touch "$t/release-$x"
check "it ends as it would have" wait_state "$x" done
check "the removed job never ran" test "$(grep -c "^start $y " "$t/calls")" = 0
check "an ended job: nothing to cancel" test "$(J cancel "$x")" = "ok ended"
z=$(id_of "$(J submit pkg-install child)")
for _ in $(seq 100); do grep -q "^start $z " "$t/calls" && [ "$(wc -l <"$t/cgroup/job-$z/cgroup.procs")" -ge 2 ] && break; sleep 0.05; done
check "confirmed: stopping" test "$(J cancel "$z" confirm)" = "ok stopping"
check "what the step ran stopped on SIGINT; the job is cancelled" wait_state "$z" cancelled
check "SIGINT went to what the step runs, not to the step" test "$(lines "TOOL-SIGINT $z")" = 0
check "the step saw it end" sh -c "\"$run/test-jobs\" log $z | grep -q \"child ended with 130\""
d=$(id_of "$(J submit pkg-install deaf)")
started "$d"
J cancel "$d" confirm >/dev/null
check "a job deaf to SIGINT is killed after the grace" wait_state "$d" cancelled
check "killed: status 137" test "$(field "$d" 4)" = 137
rm -f "$t/running" # it was killed before it could say it ended

# Peers.
quiet() { "$@" >/dev/null 2>&1; }
not() { ! "$@"; }
check "a container'"'"'s process is refused" not quiet J --cgroup 0::/app_firefox list
check "a container'"'"'s process cannot submit" not quiet J --cgroup 0::/app_firefox submit pkg-sync
check "a native window (root, app_window-*) is a peer" quiet J --cgroup 0::/app_window-settings-1 list
check "a direct program (pN) is a peer" quiet J --cgroup 0::/p7 list
check "a peer whose cgroup cannot be told is refused" not quiet J --cgroup "" list
check "the user (uid 1000) cannot reach the socket" sh -c "\"$run/test-jobs\" --uid 1000 list | grep -q \"Permission denied\""
check "the refusals are in the service'"'"'s log" test "$(grep -c "refused" "$t/service.log")" -ge 2

# The service ended (SIGTERM, as process 1 ends a service) and started again.
r=$(id_of "$(J submit pkg-install slow)")
q=$(id_of "$(J submit pkg-sync)")
started "$r"
kill -TERM "$svc"; wait "$svc"; st=$?
check "SIGTERM ends the service cleanly" test "$st" = 0
check "its running job goes on" test "$(cat "$t/running" 2>/dev/null)" = "$r"
check "the state files keep the queue" sh -c "grep -qx state=running \"$t/jobs/$r.state\" && grep -qx state=queued \"$t/jobs/$q.state\""
check "without the service a client says so" sh -c "\"$run/test-jobs\" list | grep -q \"does not run\""
echo "populated 1" >"$t/cgroup/job-$r/cgroup.events" # what the kernel says while it runs
check "without the service, the running job is still seen (its cgroup)" test "$(J pending)" = "1 $r"
echo "populated 0" >"$t/cgroup/job-$r/cgroup.events"
check "started again" start_service
check "it finds the running job and the queue" test "$(field "$r" 3) $(field "$q" 3) $(field "$q" 5)" = "running queued 1"
touch "$t/release-$r"
check "the job it found ends as it would have" wait_state "$r" done
check "then the queue goes on" wait_state "$q" done
check "still never two at a time" test "$(lines OVERLAP)" = 0
s=$(id_of "$(J submit pkg-install slow)")
started "$s"
kill -KILL "$svc"; wait "$svc" 2>/dev/null
check "killed, the service leaves its job running" test "$(cat "$t/running" 2>/dev/null)" = "$s"
touch "$t/release-$s"
for _ in $(seq 200); do [ -e "$t/running" ] || break; sleep 0.05; done
check "started again after its job ended" start_service
check "the job that ended meanwhile is done" wait_state "$s" done
u=$(id_of "$(J submit pkg-install fail)")
check "and the queue runs again" wait_state "$u" failed
kill -TERM "$svc"; wait "$svc"
sed "s/^/service: /" "$t/service.log"
sed "s/^/calls: /" "$t/calls"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ] && echo "ALL PASSED"
'

status=0
for server in locachy-jobs locachy-jobs-san; do
    timeout 300 unshare --map-auto --map-root-user --mount bash -c "$inner" sh "$src" "$run" "$t" \
        "$out/$server" "$out/test-jobs" >"$out/$server.log" 2>&1 || status=1
    if grep -qx 'ALL PASSED' "$out/$server.log" && ! grep -qE 'Sanitizer|runtime error' "$out/$server.log"; then
        grep '^PASS' "$out/$server.log" | sed "s/^PASS/PASS ($server)/"
    else
        grep '^FAIL' "$out/$server.log" | sed "s/^FAIL/FAIL ($server)/" || true
        echo "FAIL locachy-jobs ($server): $out/$server.log"
        status=1
    fi
done
exit "$status"
