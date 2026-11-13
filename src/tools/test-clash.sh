#!/bin/bash
# test-clash.sh MIHOMO [DIR]: clash-client's configuration and its routes,
# with the mihomo program MIHOMO (it need not be installed), in network,
# mount and PID namespaces of the test's own with a private /run (as any
# user: a user namespace too, with the subordinate ids of /etc/subuid and
# /etc/subgid). Offline: the test's network namespace has no way out. The
# network policy (locachy_network.py, which clash-client claims, releases
# and reconciles with) keeps its rc.conf and state in DIR, never
# /etc/locachy or /var/lib/locachy.
#   1. a subscription that gives a Clash configuration: its proxies, groups
#      and rules are kept, LOCachy's keys take the place of its own (ports,
#      controller, secret, dns, tun), FIRST_RULES go before its rules;
#      mihomo -t accepts it, in both modes
#   2. one that gives links: a provider of them, everything through it
#   3. in a throwaway namespace (a dummy interface its default route, not
#      locachy-net): the service makes mihomo's interface and rules, and
#      takes them away when ended; those of an mihomo that was killed go when
#      it starts again
set -u
mihomo=${1:?usage: test-clash.sh MIHOMO [DIR]}
src=$(cd "$(dirname "$0")/.." && pwd)
out=${2:-/var/tmp/locachy-clash-test}
ns=locachy-clash-test
if [ "${LOCACHY_CLASH_TEST:-}" != inner ]; then
    mkdir -p "$out"
    out=$(cd "$out" && pwd)
    user=()
    if [ "$(id -u)" != 0 ]; then
        sub() { awk -F: -v n="$(id -un)" -v u="$(id -u)" '($1 == n || $1 == u) && $3 >= 65535 { print $2; exit }' "$1" 2>/dev/null; }
        subuid=$(sub /etc/subuid) subgid=$(sub /etc/subgid)
        [ -n "$subuid" ] && [ -n "$subgid" ] || { echo "FAIL subordinate ids for $(id -un) (/etc/subuid, /etc/subgid)"; exit 1; }
        user=(--map-users="1:$subuid:65535" --map-groups="1:$subgid:65535" --map-root-user)
    fi
    LOCACHY_CLASH_TEST=inner exec timeout 600 unshare "${user[@]}" --net --mount --pid --fork --mount-proc \
        bash "$0" "$mihomo" "$out"
fi
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
find "$out" -mindepth 1 -delete
mkdir -p "$out/state" "$out/network"
mount -t tmpfs tmpfs /run
mkdir -p /run/netns
# Clash on, TUN (rc.conf), the policy's files in DIR.
printf 'service_clash="/usr/local/lib/locachy/clash-client"\nclash_mode="tun"\n' >"$out/rc.conf"
export LOCACHY_CLASH_RC=$out/rc.conf LOCACHY_NETWORK_RC=$out/rc.conf LOCACHY_NETWORK_STATE=$out/network \
    LOCACHY_NETWORK_NETNS=$ns
unset LOCACHY_NETWORK_HELPER LOCACHY_CLASH_CLIENT LOCACHY_TOR_ENFORCE
cc() { LOCACHY_CLASH_STATE=$out/state LOCACHY_CLASH_NETNS=$ns LOCACHY_MIHOMO=$mihomo bash "$src/link/clash-client" "$@"; }
# The service, in the background: exec, so that $! is clash-client itself,
# not a shell around it (which a signal would end, leaving it running).
service() { LOCACHY_CLASH_STATE=$out/state LOCACHY_CLASH_NETNS=$ns LOCACHY_MIHOMO=$mihomo exec bash "$src/link/clash-client"; }

cat >"$out/clash.yaml" <<'EOF'
port: 7891
socks-port: 7892
allow-lan: true
mode: rule
external-controller: 0.0.0.0:9091
secret: 'theirs'
dns:
  enable: false
  nameserver:
    - 1.2.3.4
tun:
  enable: false
hosts:
  example.com: 6.6.6.6
rule-providers:
  evil:
    type: http
    path: ./pwn.yaml
proxies:
  - { name: node-a, type: socks5, server: 127.0.0.1, port: 1 }
proxy-groups:
  - name: Choose
    type: select
    proxies: [node-a, DIRECT]
rules:
- DOMAIN-SUFFIX,example.com,Choose
- MATCH,DIRECT
EOF
echo 'https://sub.example.com/link?token=abc' >"$out/state/subscription"
cc --config "$out/clash.yaml" tun 0123secret >"$out/tun.yaml"
cc --config "$out/clash.yaml" proxy 0123secret >"$out/proxy.yaml"
top() { grep -c "^$1:" "$2"; }
check "its proxies and group kept" "grep -q 'name: node-a' $out/tun.yaml && grep -q 'name: Choose' $out/tun.yaml"
check "each own key once: mixed-port, allow-lan, dns, tun, secret, controller" \
    "[ \$(top mixed-port $out/tun.yaml)\$(top allow-lan $out/tun.yaml)\$(top dns $out/tun.yaml)\$(top tun $out/tun.yaml)\$(top secret $out/tun.yaml)\$(top external-controller $out/tun.yaml) = 111111 ]"
check "theirs gone: port 7891, socks 7892, 0.0.0.0:9091, secret theirs, nameserver 1.2.3.4, hosts, rule-providers" \
    "! grep -q -e 7891 -e 7892 -e 9091 -e theirs -e 1.2.3.4 -e 6.6.6.6 -e rule-providers -e pwn.yaml $out/tun.yaml"
check "the first rules before theirs, at their indentation" \
    "grep -A4 '^rules:' $out/tun.yaml | tr '\n' '|' | grep -q '^rules:|- PROCESS-NAME,tailscaled,DIRECT|- DOMAIN,ipinfo.io,DIRECT|- DST-PORT,123,DIRECT|- DOMAIN-SUFFIX,example.com,Choose|'"
check "tun: auto-route, Tailscale's addresses left out" \
    "grep -q 'auto-route: true' $out/tun.yaml && grep -q '100.64.0.0/10' $out/tun.yaml"
check "proxy mode: no tun" "[ \$(top tun $out/proxy.yaml) = 0 ] && grep -q '^mixed-port: 7890' $out/proxy.yaml"
for m in tun proxy; do
    check "mihomo -t accepts the $m configuration" \
        "timeout 120 $mihomo -t -d $out/state -f $out/$m.yaml >$out/t-$m.log 2>&1 || { tail -3 $out/t-$m.log; false; }"
done

echo 'c3M6Ly9ZV1Z6TFRFeU9DMW5ZMjA2Y0dGemMzZHZjbVE9QDEyNy4wLjAuMToxI25vZGU=' >"$out/links.txt"
cc --config "$out/links.txt" tun 0123secret >"$out/links.yaml"
check "links: a provider of the subscription, everything through it" \
    "grep -q \"url: 'https://sub.example.com/link?token=abc'\" $out/links.yaml && grep -q 'MATCH,Proxy' $out/links.yaml && grep -q '^tun:' $out/links.yaml"

# 3. The service, in a namespace of its own with a dummy default route.
ip netns del "$ns" 2>/dev/null
ip netns add "$ns"
ip -n "$ns" link set lo up
ip -n "$ns" link add dummy0 type dummy
ip -n "$ns" addr add 10.99.0.2/24 dev dummy0
ip -n "$ns" link set dummy0 up
ip -n "$ns" route add default via 10.99.0.1
echo "file://$out/clash.yaml" >"$out/state/subscription"
rules() { ip -n "$ns" rule show | grep -c -E '^9[0-9]{3}:'; }
up() { ip -n "$ns" link show mihomo >/dev/null 2>&1; }
before=$(ip -n "$ns" rule show | wc -l)
service >"$out/service.log" 2>&1 &
svc=$!
for i in $(seq 60); do up && [ "$(rules)" -gt 0 ] && break; sleep 0.5; done
check "running: its interface and rules ($(rules) rules)" "up && [ \$(rules) -gt 0 ]"
kill -TERM $svc
wait $svc
check "ended: interface and rules gone" "! up && [ \$(rules) = 0 ] && [ \$(ip -n $ns rule show | wc -l) = $before ]"
service >"$out/service.log" 2>&1 &
svc=$!
for i in $(seq 60); do up && [ "$(rules)" -gt 0 ] && break; sleep 0.5; done
kill -KILL $svc
pkill -KILL -f "$mihomo -d $out/state"
for i in $(seq 50); do up || break; sleep 0.1; done
check "killed: its interface and rules are left ($(rules) rules)" "[ \$(rules) -gt 0 ]"
service >"$out/service.log" 2>&1 &
svc=$!
for i in $(seq 60); do up && [ "$(rules)" -gt 0 ] && break; sleep 0.5; done
kill -TERM $svc
wait $svc
check "started again and ended: nothing left" "! up && [ \$(rules) = 0 ]"

# 4. Fail-closed TUN firewall when service_clash is enabled in rc.conf.
printf 'service_clash="/usr/local/lib/locachy/clash-client"\nclash_mode="tun"\n' >"$out/rc.conf"
cc --enforce
check "TUN firewall loaded: unprivileged direct TCP is refused while mihomo is down" \
    "ip netns exec $ns nft list table inet locachy-clash >/dev/null 2>&1 && ! ip netns exec $ns setpriv --reuid=1000 --regid=1000 --clear-groups -- python3 -c 'import socket; s=socket.socket(); s.settimeout(1); s.connect((\"10.99.0.7\", 80))' 2>/dev/null"
: >"$out/rc.conf"
cc --unenforce
check "Clash off: TUN firewall removed" "! ip netns exec $ns nft list table inet locachy-clash >/dev/null 2>&1"
ip netns del "$ns"

[ $fail = 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; tail -5 "$out/service.log"; }
exit $fail
