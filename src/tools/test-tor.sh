#!/bin/bash
# test-tor.sh [MIHOMO]: full-system Tor's firewall (tor-enforce), its service
# (tor-client) and mihomo beside it (clash-client), as root on a built system
# with tor, nftables and python3, in a throwaway namespace (a dummy interface
# its default route and local network, not locachy-net). Offline: nothing it
# sends reaches anything; what leaves is counted on the dummy interface (a
# table of its own, after the firewall), and what Tor's ports would get is
# taken by listeners of the test where tor is not running.
#   1. the rules load for each front and go with the layer; the service loads
#      them first, keeps them while Tor is on, removes them when it is off
#   2. a program (nobody, root) connecting to 192.0.2.1 (TEST-NET-1) comes to
#      the TransPort with that address; the local network is reached; other
#      UDP and ICMP are refused; DNS, anywhere, comes to the DNSPort (TCP is
#      refused); IPv6 goes nowhere but loopback; a socket bound to the
#      interface leaves nothing; the tor user goes out (bridges) or only to
#      mihomo (via mihomo), whose marked connections go out, and whose ports
#      nobody else reaches
#   3. tor itself (lyrebird for a bridge, when installed): TransPort and
#      DNSPort taken, tor's own connections out; killed, the kill switch:
#      a new connection is refused, nothing leaves, the status says so
#   4. with MIHOMO (and /dev/net/tun): its configuration for each Tor
#      arrangement (mihomo -t takes them); with bridges, its TUN leaves the
#      tor user alone and its own connections come to the TransPort; as
#      Tor's front, Tor's port sends Tor through a proxy, out, and the rest
#      is Tor's
set -u
mihomo=${1:-}
src=$(cd "$(dirname "$0")/.." && pwd)
out=/var/tmp/locachy-tor-test
ns=locachy-tor-test
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
skip() { echo "SKIP $1"; }
rm -rf "$out"
mkdir -p "$out/state" "$out/run" "$out/data" "$out/clash"
chmod 755 "$out"
export LOCACHY_TOR_NETNS=$ns LOCACHY_TOR_STATE=$out/state LOCACHY_TOR_RUN=$out/run LOCACHY_TOR_DATA=$out/data \
    LOCACHY_TOR_RC=$out/rc.conf LOCACHY_TOR_ENFORCE=$src/link/tor-enforce
enforce() { bash "$src/link/tor-enforce" "$@"; }
tc() { bash "$src/link/tor-client" "$@"; }
in_ns() { ip netns exec "$ns" "$@"; }
as() { local who=$1; shift; in_ns setpriv --reuid="$who" --regid="$(id -g "$who")" --clear-groups "$@"; }
tor_uid=$(id -u tor 2>/dev/null) || { echo "FAIL the tor user (install tor)"; exit 1; }

# The test's programs: clients and the listeners in Tor's place.
cat >"$out/net.py" <<'EOF'
import socket, struct, sys
cmd = sys.argv[1]
def done(text):
    print(text, flush=True)
    sys.exit(0)
if cmd in ("tcp", "tcp6"):
    host, port = sys.argv[2], int(sys.argv[3])
    s = socket.socket(socket.AF_INET6 if cmd == "tcp6" else socket.AF_INET)
    s.settimeout(float(sys.argv[5]) if len(sys.argv) > 5 else 2)
    if len(sys.argv) > 4 and sys.argv[4] not in ("", "-"):
        opt, value = sys.argv[4].split("=")
        if opt == "dev":
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, value.encode())
        elif opt == "src":
            s.bind((value, 0))
        else:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_MARK, int(value))
    try:
        s.connect((host, port))
        done("connected")
    except ConnectionRefusedError:
        done("refused")
    except socket.timeout:
        done("timeout")
    except OSError as e:
        done("error " + e.strerror)
elif cmd in ("udp", "dns", "udp6"):
    host, port = sys.argv[2], int(sys.argv[3])
    s = socket.socket(socket.AF_INET6 if cmd == "udp6" else socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.5)
    query = struct.pack(">HHHHHH", 0x4c4f, 0x0100, 1, 0, 0, 0) + b"\x07example\x03com\x00\x00\x01\x00\x01"
    try:
        s.sendto(query if cmd == "dns" else b"hello", (host, port))
        data, peer = s.recvfrom(512)
        done("reply" if data[:2] == query[:2] or cmd != "dns" else "other")
    except socket.timeout:
        done("timeout")
    except OSError as e:
        done("error " + e.strerror)
elif cmd == "socks":
    port, host, dport = int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    s.sendall(b"\x05\x01\x00")
    if s.recv(2) != b"\x05\x00":
        done("no socks")
    s.sendall(b"\x05\x01\x00\x01" + socket.inet_aton(host) + struct.pack(">H", dport))
    try:
        done("socks reply %d" % s.recv(10)[1])
    except socket.timeout:
        done("socks pending")
elif cmd == "listen-tcp":
    host, port, log = sys.argv[2], int(sys.argv[3]), sys.argv[4]
    s = socket.socket(socket.AF_INET6 if ":" in host else socket.AF_INET)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    s.listen(16)
    open(log + ".ready", "w").close()
    while True:
        c, peer = s.accept()
        where = "-"
        try:
            raw = c.getsockopt(socket.SOL_IP, 80, 16)  # SO_ORIGINAL_DST
            where = "%s:%d" % (socket.inet_ntoa(raw[4:8]), struct.unpack(">H", raw[2:4])[0])
        except OSError:
            pass
        with open(log, "a") as f:
            f.write(where + "\n")
        c.close()
elif cmd == "listen-dns":
    port, log = int(sys.argv[2]), sys.argv[3]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    open(log + ".ready", "w").close()
    while True:
        data, peer = s.recvfrom(512)
        with open(log, "a") as f:
            f.write("query\n")
        s.sendto(data[:2] + b"\x81\x80" + data[4:6] + b"\x00\x00\x00\x00\x00\x00" + data[12:], peer)
EOF
net() { python3 -I "$out/net.py" "$@"; }
# A listener in the namespace as USER (tor, in place of Tor's ports; root,
# of mihomo's), until stop_listeners: what it takes in $out/heard/NAME. Not
# through in_ns: $! must be python (ip netns exec and setpriv become it).
listen() { # NAME USER ARGS...
    local name=$1 who=$2
    shift 2
    mkdir -p "$out/heard"
    chmod 777 "$out/heard"
    ip netns exec "$ns" setpriv --reuid="$who" --regid="$(id -g "$who")" --clear-groups \
        python3 -I "$out/net.py" "$@" "$out/heard/$name" &
    listeners+=($!)
    for _ in $(seq 50); do [ -e "$out/heard/$name.ready" ] && return; sleep 0.1; done
}
listeners=()
stop_listeners() { kill "${listeners[@]}" 2>/dev/null; wait "${listeners[@]}" 2>/dev/null; listeners=(); rm -rf "$out/heard"; }

# The namespace: dummy0 its local network 10.99.0.0/24 and default route,
# 2001:db8::/64 for IPv6; what leaves it is counted after the firewall.
ip netns del "$ns" 2>/dev/null
ip netns add "$ns"
ip -n "$ns" link set lo up
ip -n "$ns" link add dummy0 type dummy
ip -n "$ns" addr add 10.99.0.2/24 dev dummy0
ip -n "$ns" addr add 2001:db8::2/64 dev dummy0 nodad
ip -n "$ns" link set dummy0 up
ip -n "$ns" route add default via 10.99.0.1
ip -n "$ns" -6 route add default via 2001:db8::1
in_ns nft -f - <<'EOF'
table inet probe {
    counter out-all {}
    counter out-doc1 {}
    counter out-doc10 {}
    counter out-lan {}
    counter out-v6 {}
    counter out-loop {}
    chain leaving {
        type filter hook postrouting priority 300; policy accept;
        oifname != "dummy0" return
        counter name out-all
        ip daddr 192.0.2.1 counter name out-doc1
        ip daddr 192.0.2.10 counter name out-doc10
        ip daddr 10.99.0.0/24 counter name out-lan
        ip daddr 127.0.0.0/8 counter name out-loop
        meta nfproto ipv6 counter name out-v6
    }
}
EOF
packets() { # TABLE COUNTER
    in_ns nft list counter inet "$1" "$2" 2>/dev/null | sed -n 's/.*packets \([0-9]*\).*/\1/p'
}
left() { packets probe "out-$1"; } # numbers: compared with -eq, -gt (an empty one fails)
rc() { printf '%s\n' "$@" >"$out/rc.conf"; }

# 1. The rules, with the layer.
for f in bridges mihomo none; do
    check "the rules for front $f load" "enforce load $f && enforce loaded"
done
enforce remove
check "removed: no rules" "! enforce loaded && ! in_ns nft list table inet locachy-tor >/dev/null 2>&1"
rc 'service_tor="tor-client"' 'tor_front="mihomo"'
LOCACHY_TOR=/usr/bin/tor bash "$src/link/tor-client" >"$out/service.log" 2>&1 &
svc=$!
for _ in $(seq 50); do grep -q waiting "$out/run/state" 2>/dev/null && break; sleep 0.1; done
check "the service loads them first, waiting for its front" "enforce loaded && grep -q 'waiting for Clash' $out/run/state"
check "its status: waiting, nothing goes out" "tc --status | grep -q 'waiting for Clash, its front: nothing goes out'"
kill -TERM $svc
wait $svc
check "ended while Tor is on: the rules stay" "enforce loaded"
LOCACHY_TOR=/usr/bin/tor bash "$src/link/tor-client" >"$out/service.log" 2>&1 &
svc=$!
sleep 1
rc 'tor_front="mihomo"'
for _ in $(seq 60); do kill -0 $svc 2>/dev/null || break; sleep 0.1; done
check "Tor off: the service removes them and ends" "! kill -0 $svc 2>/dev/null && ! enforce loaded"
check "its status: off, direct" "tc --status | grep -q '^off: the network goes direct'"
wait $svc 2>/dev/null

# 2. What the rules do with each kind of traffic (front: bridges, then mihomo).
enforce load bridges
listen trans tor listen-tcp 127.0.0.1 9040
listen dns tor listen-dns 5353
r0=$(packets locachy-tor redirected) d1=$(left doc1)
check "nobody to 192.0.2.1:80: to the TransPort, nothing out" \
    "[ \"\$(as nobody python3 -I $out/net.py tcp 192.0.2.1 80)\" = connected ] && grep -qx 192.0.2.1:80 $out/heard/trans && [ \$(left doc1) -eq $d1 ]"
check "root to 192.0.2.1:443: to the TransPort too" \
    "[ \"\$(in_ns python3 -I $out/net.py tcp 192.0.2.1 443)\" = connected ] && grep -qx 192.0.2.1:443 $out/heard/trans && [ \$(left doc1) -eq $d1 ]"
check "counted as redirected" "[ \$(packets locachy-tor redirected) -gt $r0 ]"
in_ns ip addr add 198.51.100.7/32 dev lo
check "from an address of the machine not on the local network (as mihomo's TUN's): to the TransPort, answered" \
    "[ \"\$(as nobody python3 -I $out/net.py tcp 192.0.2.1 81 src=198.51.100.7)\" = connected ] && grep -qx 192.0.2.1:81 $out/heard/trans && [ \$(left doc1) -eq $d1 ]"
l0=$(left lan)
check "nobody to the local network (10.99.0.7:80): out, not to Tor" \
    "[ \"\$(as nobody python3 -I $out/net.py tcp 10.99.0.7 80 - 1)\" = timeout ] && [ \$(left lan) -gt $l0 ] && ! grep -q 10.99.0.7 $out/heard/trans"
dr0=$(packets locachy-tor dropped)
check "nobody, UDP to 192.0.2.1:9999: refused, nothing out" \
    "as nobody python3 -I $out/net.py udp 192.0.2.1 9999 | grep -q -e error -e timeout && [ \$(left doc1) -eq $d1 ]"
check "root, ICMP to 192.0.2.1 (ping): refused, nothing out" \
    "! in_ns ping -c 1 -W 1 192.0.2.1 >/dev/null 2>&1 && [ \$(left doc1) -eq $d1 ] && [ \$(packets locachy-tor dropped) -gt $dr0 ]"
a0=$(left all)
check "nobody, DNS to 192.0.2.53: the DNSPort answers" \
    "[ \"\$(as nobody python3 -I $out/net.py dns 192.0.2.53 53)\" = reply ] && grep -q query $out/heard/dns"
check "nobody, DNS to the local network's resolver (10.99.0.1): the DNSPort too" \
    "[ \"\$(as nobody python3 -I $out/net.py dns 10.99.0.1 53)\" = reply ] && [ \$(grep -c query $out/heard/dns) = 2 ]"
check "nobody, DNS over TCP: refused at the DNSPort (UDP only), nothing out" \
    "[ \"\$(as nobody python3 -I $out/net.py tcp 192.0.2.53 53)\" = refused ] && [ \$(left all) -eq $a0 ]"
v0=$(left v6) i0=$(packets locachy-tor ipv6)
check "IPv6, nobody and root to [2001:db8::99]: refused, nothing out" \
    "as nobody python3 -I $out/net.py tcp6 2001:db8::99 80 | grep -q -e refused -e error && in_ns python3 -I $out/net.py udp6 2001:db8::99 53 | grep -q -e timeout -e error && [ \$(left v6) -eq $v0 ] && [ \$(packets locachy-tor ipv6) -gt $i0 ]"
listen loop6 root listen-tcp ::1 8080
check "IPv6 loopback (::1) stays usable" "[ \"\$(as nobody python3 -I $out/net.py tcp6 ::1 8080)\" = connected ]"
check "a socket bound to dummy0 (redirected, it would leave as 127.0.0.1): nothing out" \
    "[ \"\$(in_ns python3 -I $out/net.py tcp 192.0.2.1 80 dev=dummy0 1)\" = timeout ] && [ \$(left loop) -eq 0 ] && [ \$(left doc1) -eq $d1 ] && [ \$(packets locachy-tor leaving) -gt 0 ]"
t0=$(packets locachy-tor tor)
check "the tor user, TCP to 192.0.2.1:443: out (bridges)" \
    "[ \"\$(as tor python3 -I $out/net.py tcp 192.0.2.1 443 - 1)\" = timeout ] && [ \$(left doc1) -gt $d1 ] && [ \$(packets locachy-tor tor) -gt $t0 ]"
l0=$(left lan)
check "the tor user, DNS to the local resolver: out (WebTunnel finds its host)" \
    "as tor python3 -I $out/net.py dns 10.99.0.1 53 | grep -q timeout && [ \$(left lan) -gt $l0 ]"
d1=$(left doc1)
check "the tor user, other UDP and IPv6: refused, nothing out" \
    "as tor python3 -I $out/net.py udp 192.0.2.1 9999 | grep -q -e error -e timeout && as tor python3 -I $out/net.py tcp6 2001:db8::99 443 | grep -q -e refused -e error && [ \$(left doc1) -eq $d1 ] && [ \$(left v6) -eq $v0 ]"

enforce load mihomo
listen front root listen-tcp 127.0.0.1 7891
listen mixed root listen-tcp 127.0.0.1 7890
d1=$(left doc1)
check "via mihomo: the tor user to 192.0.2.1:443 is refused, nothing out" \
    "as tor python3 -I $out/net.py tcp 192.0.2.1 443 | grep -q -e refused -e error && [ \$(left doc1) -eq $d1 ]"
check "via mihomo: the tor user reaches mihomo's port for Tor (7891)" \
    "[ \"\$(as tor python3 -I $out/net.py tcp 127.0.0.1 7891)\" = connected ] && [ -s $out/heard/front ]"
check "via mihomo: nobody else reaches mihomo's ports (7890, 7891)" \
    "[ \"\$(as nobody python3 -I $out/net.py tcp 127.0.0.1 7891)\" = refused ] && [ \"\$(in_ns python3 -I $out/net.py tcp 127.0.0.1 7890)\" = refused ] && [ ! -s $out/heard/mixed ]"
f0=$(packets locachy-tor front)
check "via mihomo: mihomo's marked connections (2023) go out" \
    "[ \"\$(in_ns python3 -I $out/net.py tcp 192.0.2.1 443 mark=2023 1)\" = timeout ] && [ \$(left doc1) -gt $d1 ] && [ \$(packets locachy-tor front) -gt $f0 ]"
d1=$(left doc1)
check "via mihomo: everyone else still to the TransPort" \
    "[ \"\$(as nobody python3 -I $out/net.py tcp 192.0.2.1 8443)\" = connected ] && grep -qx 192.0.2.1:8443 $out/heard/trans && [ \$(left doc1) -eq $d1 ]"
stop_listeners

# 3. tor itself, through a bridge (lyrebird) or, without lyrebird, through
# mihomo's port (nobody there); then killed.
if [ -x /usr/bin/lyrebird ]; then
    front=bridges
    echo 'obfs4 192.0.2.10:443 0123456789ABCDEF0123456789ABCDEF01234567 cert=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA iat-mode=0' \
        >"$out/state/bridges"
    rc 'service_tor="tor-client"' 'tor_front="bridges"'
else
    front=mihomo
    skip "tor's own connections through a bridge (lyrebird is not installed)"
    rc 'service_tor="tor-client"' 'tor_front="mihomo"' 'service_clash="clash-client"'
fi
check "its configuration: TransPort, DNSPort, no SocksPort, the front" \
    "tc --torrc $front | grep -qx 'TransPort 127.0.0.1:9040 IsolateDestAddr IsolateDestPort' && tc --torrc $front | grep -qx 'SocksPort 0' && tc --torrc $front | grep -q -e '^Bridge obfs4 192.0.2.10:443' -e '^Socks5Proxy 127.0.0.1:7891'"
d10=$(left doc10)
LOCACHY_TOR=/usr/bin/tor bash "$src/link/tor-client" >"$out/service.log" 2>&1 &
svc=$!
for _ in $(seq 100); do
    pid=$(cat "$out/run/tor.pid" 2>/dev/null)
    # A well-formed query: Arch's tor dies of a malformed one (bugs.archlinux.org/task/65313).
    [ -n "$pid" ] && [ "$(in_ns python3 -I "$out/net.py" dns 127.0.0.1 5353)" != error ] && break
    sleep 0.2
done
sleep 2
check "tor runs as the tor user" "[ -n \"\$pid\" ] && [ \"\$(stat -c %U /proc/\$pid)\" = tor ]"
check "its status: through $front" "tc --status | grep -q -e '^starting, through' -e '^connecting through'"
check "nobody to 192.0.2.1:80: tor's TransPort takes it" \
    "[ \"\$(as nobody python3 -I $out/net.py tcp 192.0.2.1 80)\" = connected ]"
if [ $front = bridges ]; then
    for _ in $(seq 30); do [ "$(left doc10)" -gt "$d10" ] && break; sleep 0.5; done
    check "tor's own connections go out, to its bridge (192.0.2.10)" "[ \$(left doc10) -gt $d10 ]"
fi
kill -KILL "$pid"
for _ in $(seq 60); do kill -0 $svc 2>/dev/null || break; sleep 0.1; done
d1=$(left doc1)
check "tor killed: the rules stay" "enforce loaded"
check "the kill switch: nobody to 192.0.2.1:80 is refused, nothing out" \
    "[ \"\$(as nobody python3 -I $out/net.py tcp 192.0.2.1 80)\" = refused ] && [ \$(left doc1) -eq $d1 ]"
check "its status says so" "tc --status | grep -q 'kill switch holds the network, nothing goes out'"
wait $svc 2>/dev/null
enforce remove
rm -f "$out/state/bridges"

# 4. mihomo beside Tor.
tun=0
if [ -n "$mihomo" ] && [ -c /dev/net/tun ]; then
    tun=1
fi
if [ -z "$mihomo" ]; then
    skip "mihomo beside Tor (no MIHOMO given)"
else
    cat >"$out/clash.yaml" <<'EOF'
mode: rule
proxies:
  - { name: node-a, type: socks5, server: 192.0.2.10, port: 1080 }
proxy-groups:
  - name: Choose
    type: select
    proxies: [node-a, DIRECT]
rules:
- MATCH,Choose
EOF
    echo "file://$out/clash.yaml" >"$out/clash/subscription"
    cc() { LOCACHY_CLASH_STATE=$out/clash LOCACHY_CLASH_NETNS=$ns LOCACHY_MIHOMO=$mihomo bash "$src/link/clash-client" "$@"; }
    for t in none mihomo bridges; do
        for m in tun proxy; do
            cc --config "$out/clash.yaml" $m secret $t >"$out/c-$t-$m.yaml"
            check "mihomo -t takes the $m configuration with Tor $t" \
                "timeout 120 $mihomo -t -d $out/clash -f $out/c-$t-$m.yaml >$out/t.log 2>&1 || { tail -3 $out/t.log; false; }"
        done
    done
    check "Tor off: as before (no mark, no port for Tor)" \
        "! grep -q -e routing-mark -e exclude-uid -e listeners -e LOCachy-Tor-front $out/c-none-tun.yaml && grep -q 'auto-detect-interface: true' $out/c-none-tun.yaml"
    check "front: the port for Tor through a group of all proxies, never DIRECT, first; its mark" \
        "grep -q 'port: 7891.*proxy: LOCachy-Tor-front' $out/c-mihomo-tun.yaml && grep -A1 '^proxy-groups:' $out/c-mihomo-tun.yaml | grep -q 'name: LOCachy-Tor-front, type: url-test, include-all: true, exclude-type: direct' && grep -qx 'routing-mark: 2023' $out/c-mihomo-tun.yaml"
    check "bridges: its own mark, no interface of its own; the tor user out of its TUN" \
        "grep -qx 'routing-mark: 2024' $out/c-bridges-tun.yaml && grep -q 'auto-detect-interface: false' $out/c-bridges-tun.yaml && grep -qx '  exclude-uid: \[$tor_uid\]' $out/c-bridges-tun.yaml"
    if [ $tun = 0 ]; then
        skip "mihomo's TUN beside Tor (no /dev/net/tun)"
    else
        clash() { # the service, in the background (exec: $! is clash-client itself)
            LOCACHY_CLASH_STATE=$out/clash LOCACHY_CLASH_NETNS=$ns LOCACHY_MIHOMO=$mihomo LOCACHY_CLASH_RC=$out/rc.conf \
                exec bash "$src/link/clash-client"
        }
        up() { ip -n "$ns" link show mihomo >/dev/null 2>&1; }
        controller() { in_ns curl -s --max-time 2 --unix-socket "$out/clash/controller.sock" -H "Authorization: Bearer $(cat "$out/clash/secret")" "http://localhost/$1"; }
        listen trans tor listen-tcp 127.0.0.1 9040
        listen dns tor listen-dns 5353
        rc 'service_tor="tor-client"' 'tor_front="bridges"' 'service_clash="clash-client"' 'clash_mode="tun"'
        enforce load bridges
        clash >"$out/clash.log" 2>&1 &
        svc=$!
        for _ in $(seq 120); do up && controller version | grep -q version && break; sleep 0.5; done
        check "bridges: mihomo's TUN up" "up"
        check "bridges: its TUN leaves the tor user's connections to the main routes" \
            "ip -n $ns rule show | grep -q 'uidrange $tor_uid-$tor_uid goto'"
        check "bridges: its marked connections take the main routes (rule 8999)" \
            "ip -n $ns rule show | grep -q '^8999:.*fwmark 0x7e8 lookup main'"
        d1=$(left doc1) d10=$(left doc10)
        check "bridges: the tor user to 192.0.2.1:443 goes out, not into mihomo" \
            "[ \"\$(as tor python3 -I $out/net.py tcp 192.0.2.1 443 - 1)\" = timeout ] && [ \$(left doc1) -gt $d1 ] && ! grep -q '192.0.2.1:443' $out/clash.log"
        d1=$(left doc1)
        as nobody python3 -I "$out/net.py" tcp 192.0.2.1 80 - 3 >/dev/null
        for _ in $(seq 20); do grep -q 192.0.2.10:1080 "$out/heard/trans" 2>/dev/null && break; sleep 0.2; done
        check "bridges: nobody to 192.0.2.1:80 goes into mihomo, whose connection to its proxy comes to the TransPort" \
            "grep -qx 192.0.2.10:1080 $out/heard/trans && [ \$(left doc1) -eq $d1 ] && [ \$(left doc10) -eq $d10 ]"
        rc 'service_tor="tor-client"' 'tor_front="mihomo"' 'service_clash="clash-client"' 'clash_mode="tun"'
        enforce load mihomo
        for _ in $(seq 100); do kill -0 $svc 2>/dev/null || break; sleep 0.1; done
        check "Tor's arrangement changed: clash-client ends (process 1 starts it again)" \
            "! kill -0 $svc 2>/dev/null && grep -q \"Tor's arrangement changed\" $out/clash.log && ! ip -n $ns rule show | grep -q '^8999:'"
        wait $svc 2>/dev/null
        clash >"$out/clash.log" 2>&1 &
        svc=$!
        for _ in $(seq 120); do up && controller version | grep -q version && break; sleep 0.5; done
        : >"$out/heard/trans"
        d10=$(left doc10) f0=$(packets locachy-tor front)
        check "front: Tor (the tor user) through mihomo's port: out to its proxy, marked" \
            "as tor python3 -I $out/net.py socks 7891 192.0.2.1 443 >/dev/null; for _ in \$(seq 20); do [ \$(left doc10) -gt $d10 ] && break; sleep 0.2; done; [ \$(left doc10) -gt $d10 ] && [ \$(packets locachy-tor front) -gt $f0 ]"
        check "front: nobody reaches mihomo's ports" \
            "[ \"\$(as nobody python3 -I $out/net.py tcp 127.0.0.1 7890)\" = refused ] && [ \"\$(as nobody python3 -I $out/net.py tcp 127.0.0.1 7891)\" = refused ]"
        d1=$(left doc1)
        check "front: nobody to 192.0.2.1:80 comes to the TransPort, not into mihomo" \
            "[ \"\$(as nobody python3 -I $out/net.py tcp 192.0.2.1 80)\" = connected ] && grep -qx 192.0.2.1:80 $out/heard/trans && [ \$(left doc1) -eq $d1 ]"
        kill -TERM $svc
        wait $svc 2>/dev/null
        check "clash-client ended: its TUN and rules gone" "! up && ! ip -n $ns rule show | grep -q -e '^9[0-9][0-9][0-9]:' -e '^8999:'"
        stop_listeners
    fi
fi

enforce remove
ip netns del "$ns"
[ $fail = 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; tail -5 "$out/service.log"; }
exit $fail
