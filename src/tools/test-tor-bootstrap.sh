#!/bin/bash
# test-tor-bootstrap.sh BRIDGES: full-system Tor through real bridges, on the
# internet, as root on a built system with tor, lyrebird, nftables, curl and
# pasta (passt): in a throwaway namespace that pasta connects to the build
# machine's network, tor-client (its firewall first) must bring tor to
# Bootstrapped 100% through the obfs4 or WebTunnel lines of the file BRIDGES,
# and a program (nobody) must reach check.torproject.org through the DNSPort
# and the TransPort, which says it came from Tor. Meanwhile nothing but the
# tor user's own connections (and the local network) leaves the namespace,
# counted after the firewall.
set -u
bridges=${1:?usage: test-tor-bootstrap.sh BRIDGES}
src=$(cd "$(dirname "$0")/.." && pwd)
out=/var/tmp/locachy-tor-bootstrap
ns=locachy-tor-boot
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
rm -rf "$out"
mkdir -p "$out/state" "$out/run" "$out/data"
chmod 755 "$out"
cp "$bridges" "$out/state/bridges"
export LOCACHY_TOR_NETNS=$ns LOCACHY_TOR_STATE=$out/state LOCACHY_TOR_RUN=$out/run LOCACHY_TOR_DATA=$out/data \
    LOCACHY_TOR_RC=$out/rc.conf LOCACHY_TOR_ENFORCE=$src/link/tor-enforce LOCACHY_TOR=/usr/bin/tor
in_ns() { ip netns exec "$ns" "$@"; }
packets() { in_ns nft list counter inet "$1" "$2" 2>/dev/null | sed -n 's/.*packets \([0-9]*\).*/\1/p'; }
tor_uid=$(id -u tor) || exit 1

ip netns del "$ns" 2>/dev/null
ip netns add "$ns"
ip -n "$ns" link set lo up
pasta --quiet --config-net --netns "/run/netns/$ns" -P "$out/pasta.pid" || { echo "FAIL pasta"; exit 1; }
dev=$(ip -n "$ns" -o link show | awk -F': ' '$2 != "lo" {print $2; exit}')
check "pasta's interface ($dev) and a default route" "[ -n '$dev' ] && ip -n $ns route show default | grep -q ."
# What leaves the interface, after the firewall: the tor user's, and the rest.
in_ns nft -f - <<EOF
table inet probe {
    counter tor {}
    counter others {}
    chain leaving {
        type filter hook postrouting priority 300; policy accept;
        oifname != "$dev" return
        meta skuid $tor_uid counter name tor return
        fib daddr type local return
        counter name others
    }
}
EOF
printf '%s\n' 'service_tor="tor-client"' 'tor_front="bridges"' >"$out/rc.conf"
bash "$src/link/tor-client" >"$out/service.log" 2>&1 &
svc=$!
for _ in $(seq 300); do
    grep -q 'Bootstrapped 100%' "$out/run/notices.log" 2>/dev/null && break
    kill -0 $svc 2>/dev/null || break
    sleep 1
done
grep 'Bootstrapped' "$out/run/notices.log" 2>/dev/null | tail -3
check "Bootstrapped 100% through the bridges" "grep -q 'Bootstrapped 100%' $out/run/notices.log"
check "its status: connected" "bash $src/link/tor-client --status | grep -q '^connected through bridges'"
# Through an exit relay: a circuit can fail, so up to three tries.
for _ in 1 2 3; do
    in_ns setpriv --reuid=nobody --regid=nobody --clear-groups \
        curl -sS --max-time 90 https://check.torproject.org/api/ip >"$out/check.json" 2>>"$out/curl.log" && break
done
cat "$out/check.json"; echo
check "nobody reaches check.torproject.org (DNSPort, TransPort): it came from Tor" \
    "grep -q '\"IsTor\":true' $out/check.json"
check "what left the namespace: the tor user's ($(packets probe tor) packets), nothing else ($(packets probe others))" \
    "[ \$(packets probe tor) -gt 0 ] && [ \$(packets probe others) -eq 0 ]"
printf '%s\n' 'tor_front="bridges"' >"$out/rc.conf"
for _ in $(seq 100); do kill -0 $svc 2>/dev/null || break; sleep 0.1; done
check "Tor off: the rules gone" "! bash $src/link/tor-enforce loaded"
kill "$(cat "$out/pasta.pid")" 2>/dev/null
ip netns del "$ns"
[ $fail = 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; tail -5 "$out/run/notices.log"; tail -3 "$out/curl.log"; }
exit $fail
