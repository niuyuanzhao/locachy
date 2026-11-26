#!/usr/bin/env bash
# Open MPI 5 CPU/TCP emulation only: one rank per isolated veth peer.
# This is not a thunderbolt-net, LOCachy firewall, or accelerator test.
set -euo pipefail

if [[ $# == 0 ]]; then
    for tool in mpicc mpirun ip ss unshare nsenter timeout; do
        command -v "$tool" >/dev/null || { printf 'Required tool missing: %s\n' "$tool" >&2; exit 1; }
    done
    exec timeout --kill-after=5s 45s unshare --user --map-root-user --net --uts \
        --pid --fork --mount-proc --kill-child -- "$BASH" "$0" --isolated
fi
[[ $# == 1 && $1 == --isolated && $$ == 1 && $EUID == 0 ]]
# Refuse to touch networking if this invocation was not isolated.
[[ $(ip -o link show | wc -l) == 1 && $(ip -o link show) == *': lo:'* ]]

work=$(mktemp -d "${HOME}/.locachy-tb-mpi.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
# Keep MPI/PMIx state off the real user's profile and avoid a long PMIx socket path.
export HOME=$work
hostname locachy-tb-a
ip link set lo up
ip link add tbtest type veth peer name tb-peer
unshare --net --uts -- sh -c 'hostname locachy-tb-b; exec sleep 50' &
peer=$!
for ((i = 0; i < 100; i++)); do
    [[ $(nsenter --target "$peer" --uts -- hostname) != locachy-tb-b ]] || break
    sleep 0.02
done
[[ $(nsenter --target "$peer" --uts -- hostname) == locachy-tb-b ]]
ip link set tb-peer netns "$peer"
there() { nsenter --target "$peer" --net --uts -- "$@"; }
there ip link set lo up
there ip link set tb-peer name tbtest
there ip addr add 198.18.0.2/30 dev tbtest
there ip link set tbtest up
ip addr add 198.18.0.1/30 dev tbtest
ip link set tbtest up
export LOCACHY_TB_TEST_PEER=$peer

# PRRTE's documented launch-agent hook replaces SSH only in this fixture.
# It puts the remote daemon in the other namespace; MPI traffic uses the veth.
cat >"$work/launch-peer" <<'EOF'
#!/bin/sh
set -eu
[ "$1" = 198.18.0.2 ]
shift
exec nsenter --target "$LOCACHY_TB_TEST_PEER" --net --uts -- /bin/sh -c "$*"
EOF
chmod +x "$work/launch-peer"

cat >"$work/check.c" <<'EOF'
#include <inttypes.h>
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint64_t square_sum(uint64_t n) {
    return n * (n + 1) * (2 * n + 1) / 6;
}

int main(int argc, char **argv) {
    const uint64_t chunk = 100000;
    int rank, peers;
    char host[64], netns[64];
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &peers);
    int n = (int)readlink("/proc/self/ns/net", netns, sizeof(netns) - 1);
    if (argc != 2 || peers != 2 || n < 0 || gethostname(host, sizeof(host)) ||
        strcmp(host, rank == 0 ? "locachy-tb-a" : "locachy-tb-b")) {
        fprintf(stderr, "Unexpected placement for rank %d\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    netns[n] = '\0';
    uint64_t begin = rank * chunk, end = begin + chunk;
    uint64_t local = 0, remote = 0, total = 0;
    for (uint64_t i = begin + 1; i <= end; i++)
        local += i * i;
    int other = 1 - rank;
    MPI_Sendrecv(&local, 1, MPI_UINT64_T, other, 0, &remote, 1, MPI_UINT64_T,
                 other, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Allreduce(&local, &total, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    if (local != square_sum(end) - square_sum(begin) ||
        remote != square_sum((other + 1) * chunk) - square_sum(other * chunk) ||
        total != square_sum(2 * chunk)) {
        fprintf(stderr, "Incorrect result for rank %d\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    printf("rank=%d host=%s namespace=%s range=%" PRIu64 "..%" PRIu64
           " local=%" PRIu64 " remote=%" PRIu64 " total=%" PRIu64 "\n",
           rank, host, netns, begin + 1, end, local, remote, total);
    fflush(stdout);
    /* Keep data sockets open until the harness observes the peer endpoints. */
    while (access(argv[1], F_OK) == -1)
        usleep(20000);
    MPI_Finalize();
    return 0;
}
EOF
mpicc -Wall -Wextra -Werror -O2 -o "$work/check" "$work/check.c"
mpirun --version
mpirun --allow-run-as-root --bind-to none --host locachy-tb-a,198.18.0.2 \
    --map-by ppr:1:node --prtemca plm_ssh_agent "$work/launch-peer" \
    --prtemca oob_tcp_if_include tbtest --mca pml ob1 --mca btl self,tcp \
    --mca btl_tcp_if_include tbtest --mca btl_tcp_port_min_v4 41000 \
    --mca btl_tcp_port_range_v4 32 -n 2 "$work/check" "$work/done" >"$work/result" 2>&1 &
mpi=$!
for ((i = 0; i < 200; i++)); do
    if grep -q '^rank=0 ' "$work/result" && grep -q '^rank=1 ' "$work/result"; then break; fi
    if ! kill -0 "$mpi" 2>/dev/null; then break; fi
    sleep 0.05
done
cat "$work/result"
grep -q "^rank=0 host=locachy-tb-a namespace=$(readlink /proc/self/ns/net | sed 's/\[/\\[/;s/\]/\\]/') " "$work/result"
peer_net=$(there readlink /proc/self/ns/net)
[[ $peer_net != "$(readlink /proc/self/ns/net)" ]]
grep -Fq "rank=1 host=locachy-tb-b namespace=$peer_net " "$work/result"
# Only the BTL uses this port range; a PRRTE control connection is not evidence.
port='410(0[0-9]|[12][0-9]|3[01])'
for side in here there; do
    if [[ $side == here ]]; then
        sockets=$(ss -H4tn state established)
        local_ip='198\.18\.0\.1' remote_ip='198\.18\.0\.2'
    else
        sockets=$(there ss -H4tn state established)
        local_ip='198\.18\.0\.2' remote_ip='198\.18\.0\.1'
    fi
    grep -E "${local_ip}:${port}[[:space:]]+${remote_ip}:[0-9]+|${local_ip}:[0-9]+[[:space:]]+${remote_ip}:${port}" <<<"$sockets"
done
touch "$work/done"
wait "$mpi"
printf 'PASS: two isolated CPU peers validated the split sum using Open MPI over veth/TCP.\n'
