#!/bin/bash
# test-network-policy.sh [DIR]: locachy_network.py's fail-closed transitions
# of the full-system network layers (layer 1 Tor, layer 2 Clash), with the
# real nft, ip, tor-enforce, clash-client and mihomo, as any user: user,
# network, mount and PID namespaces of the test's own (unshare), a private
# /run with its locachy-net, every path of the helper, tor-enforce,
# tor-client and clash-client in DIR (LOCACHY_NETWORK_*, LOCACHY_TOR_*,
# LOCACHY_CLASH_*), never /etc/locachy or /var/lib/locachy. locachy-net's
# uplink is a dummy interface: nothing leaves the namespaces; what would
# leave is counted on it per protocol, destination and port by a table
# after every other (priority 300). Python listeners stand in for Tor's
# TransPort, DNSPort and SocksPort (tor itself is not needed). Subordinate
# ids (/etc/subuid, /etc/subgid) give the test nobody, a tor user and
# mihomo's group 2022; without them the checks that need those are skipped.
#   1. a request before locachy-net exists is committed (queued); reconcile,
#      as network-client runs it, applies it
#   2. every policy refused() allows applies from every allowed policy; a
#      refused request exits 2 and changes neither rc.conf, nor the status,
#      nor a rule; tor-enforce's entries reconcile, never a captured front;
#      a layer that is on needs its program (mihomo, tor, lyrebird for the
#      bridges), as settings offers it only then: without it the request
#      exits 2, and turning a layer off never needs one
#   3. what leaves under each policy: everything without Tor or Clash TUN;
#      under Tor only its front, the local network and DHCP, TCP and DNS at
#      the stand-ins; under Clash TUN only what clash-client's rules let out
#   4. lan: Tor's local networks refreshed in its table, which stays
#   5. a failure at each step (Tor's rules, Clash's, nft refusing the batch,
#      the rc.conf commit, removing the block, loading it): exit 1 (4 when
#      not even the block loads), only loopback and DHCP leave, the status
#      says failed; reconcile, tor-enforce and daemon claims keep the block
#      until a later set applies
#   6. inside a transition (off to Clash TUN held before its rules, Tor to
#      Clash held at the rc.conf commit): only loopback and DHCP leave
#   7. senders running through repeated toggles: nothing leaves under a
#      policy without a direct path, at any moment
#   8. requests serialize on policy.lock; concurrent ones end consistent
#   9. daemon generations with the real mihomo: a stale release exits 3 and
#      touches nothing; a killed mihomo (Clash TUN, Tor's front) leaves no
#      direct path; the next claim cleans what a killed generation left,
#      never a newer one's; a killed clash-client or tor-client takes its
#      daemon with it (pdeathsig), so the next generation starts
set -u
# The helpers import locachy_config from link/: no bytecode in the source tree.
export PYTHONDONTWRITEBYTECODE=1
src=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-/var/tmp/locachy-network-policy-test}
if [ "${LOCACHY_POLICY_TEST:-}" != inner ]; then
    mkdir -p "$out"
    out=$(cd "$out" && pwd)
    # The first block of 65535 subordinate ids: the inner ids 1..65535.
    sub() { awk -F: -v n="$(id -un)" -v u="$(id -u)" '($1 == n || $1 == u) && $3 >= 65535 { print $2; exit }' "$1" 2>/dev/null; }
    subuid=$(sub /etc/subuid) subgid=$(sub /etc/subgid)
    ids=()
    if [ -n "$subuid" ] && [ -n "$subgid" ] &&
        unshare --map-users="1:$subuid:65535" --map-groups="1:$subgid:65535" --map-root-user true 2>/dev/null; then
        ids=(--map-users="1:$subuid:65535" --map-groups="1:$subgid:65535")
    fi
    LOCACHY_POLICY_TEST=inner LOCACHY_POLICY_IDS=${#ids[@]} exec timeout 900 \
        unshare "${ids[@]}" --map-root-user --net --mount --pid --fork --mount-proc bash "$0" "$out"
fi
ids=$LOCACHY_POLICY_IDS
fail=0 passed=0
check() { if eval "$2"; then echo "PASS $1"; passed=$((passed + 1)); else echo "FAIL $1"; fail=$((fail + 1)); fi; }
skip() { echo "SKIP $1"; }
# A bounded wait for a condition the test cannot be told of.
until_true() { # TENTHS EXPR
    local i
    for ((i = 0; i < $1; i++)); do eval "$2" && return 0; sleep 0.1; done
    eval "$2"
}
find "$out" -mindepth 1 -delete 2>/dev/null
mkdir -p "$out/etc" "$out/state" "$out/tor-run" "$out/tor-state" "$out/clash" "$out/bin" "$out/layer"
mount -t tmpfs tmpfs /run
mkdir -p /run/netns
# What other users run and write (nobody, the tor user): world-readable.
T=/run/policy-test
mkdir -p "$T/heard" "$T/send"
chmod 755 "$T"
chmod 1777 "$T/heard" "$T/send"
# Stand-ins for the programs settings checks (tor, and lyrebird for bridges):
# present for every policy here; section 9 replaces the tor one with a listener.
printf '#!/bin/sh\nexit 0\n' >"$T/tor"
chmod 755 "$T/tor"
printf '#!/bin/sh\nexit 0\n' >"$out/bin/lyrebird"
chmod 755 "$out/bin/lyrebird"

# The test's tor user (tor-enforce and clash-client look it up by name).
cp /etc/passwd "$out/passwd"
cp /etc/group "$out/group"
if ! getent passwd tor >/dev/null; then
    echo 'tor:x:4343:4343:Tor (test):/:/usr/bin/nologin' >>"$out/passwd"
    echo 'tor:x:4343:' >>"$out/group"
fi
mount --bind "$out/passwd" /etc/passwd
mount --bind "$out/group" /etc/group

RC=$out/etc/rc.conf
helper=$src/link/locachy_network.py
NFT=$(command -v nft)
MIHOMO=${LOCACHY_TEST_MIHOMO:-/usr/bin/mihomo}
# The presence check of settings (mihomo): the real binary when there is one,
# else a stand-in (section 9 needs the real one, and is skipped without it).
PRESENT=$MIHOMO
if [ ! -x "$MIHOMO" ]; then
    printf '#!/bin/sh\nexit 0\n' >"$out/bin/mihomo"
    chmod 755 "$out/bin/mihomo"
    PRESENT=$out/bin/mihomo
fi
export LOCACHY_NETWORK_RC=$RC LOCACHY_NETWORK_STATE=$out/state LOCACHY_NETWORK_NETNS=locachy-net \
    LOCACHY_TOR_RUN=$out/tor-run LOCACHY_TOR_STATE=$out/tor-state LOCACHY_TOR_DATA=$out/tor-data \
    LOCACHY_TOR_RC=$RC LOCACHY_TOR_NETNS=locachy-net LOCACHY_TOR_USER=tor LOCACHY_TOR=$T/tor \
    LOCACHY_TOR_ENFORCE=$out/bin/tor-enforce LOCACHY_LYREBIRD=$out/bin/lyrebird LOCACHY_CLASH_CLIENT=$out/bin/clash-client \
    LOCACHY_CLASH_RC=$RC LOCACHY_CLASH_STATE=$out/clash LOCACHY_CLASH_NETNS=locachy-net LOCACHY_MIHOMO=$PRESENT \
    PATH=$out/bin:$PATH
unset LOCACHY_NETWORK_HELPER
pol() { python3 "$helper" "$@"; }

# Failures the test injects: tor-enforce's rules (DIR/layer/tor-fail),
# clash-client's (DIR/layer/clash-fail), and nft, which refuses or holds the
# batch DIR/layer/nft names ("fail|hold block|layers|unblock").
cat >"$out/bin/tor-enforce" <<EOF
#!/bin/bash
if [ "\${1:-}" = rules ] && [ -e "$out/layer/tor-fail" ]; then echo "tor-enforce: the test fails its rules" >&2; exit 1; fi
exec bash "$src/link/tor-enforce" "\$@"
EOF
cat >"$out/bin/clash-client" <<EOF
#!/bin/bash
if [ "\${1:-}" = --rules ] && [ -e "$out/layer/clash-fail" ]; then echo "clash-client: the test fails its rules" >&2; exit 1; fi
exec bash "$src/link/clash-client" "\$@"
EOF
cat >"$out/bin/nft" <<EOF
#!/bin/bash
mode=\$(cat "$out/layer/nft" 2>/dev/null)
if [ -n "\$mode" ] && [ "\${1:-}" = -f ] && [ "\${2:-}" = - ]; then
    batch=\$(cat)
    case \$batch in
    *"priority -250"*) kind=block ;;
    *locachy-tor* | *locachy-clash*) kind=layers ;;
    *locachy-transition*) kind=unblock ;;
    *) kind=other ;;
    esac
    case \$mode in
    "fail \$kind") echo "nft: the test refuses this batch (\$kind)" >&2; exit 1 ;;
    "hold \$kind")
        : >"$out/layer/held"
        for ((i = 0; i < 600; i++)); do [ -e "$out/layer/go" ] && break; sleep 0.1; done ;;
    esac
    printf '%s\n' "\$batch" | exec "$NFT" -f -
fi
exec "$NFT" "\$@"
EOF
chmod 755 "$out/bin/tor-enforce" "$out/bin/clash-client" "$out/bin/nft"
inject_clear() {
    rm -f "$out/layer/tor-fail" "$out/layer/clash-fail" "$out/layer/nft" "$out/layer/held" "$out/layer/go"
    [ ! -L "$RC.lock" ] || rm -f "$RC.lock" # the commit's failure (a lock locachy_config refuses)
}

# The test's programs: probes, senders, listeners, the probe's counts.
net=$T/net.py
cat >"$net" <<'EOF'
import errno, fcntl, json, os, select, socket, struct, sys, time
cmd = sys.argv[1]
QUERY = struct.pack(">HHHHHH", 0x4c4f, 0x0100, 1, 0, 0, 0) + b"\x07example\x03com\x00\x00\x01\x00\x01"

def say(text):
    print(text, flush=True)
    sys.exit(0)

def connect(s, address, wait):
    s.setblocking(False)
    try:
        s.connect(address)
        return "connected"
    except BlockingIOError:
        pass
    except OSError as e:
        return "refused" if e.errno == errno.ECONNREFUSED else "error " + e.strerror
    if not select.select([], [s], [], wait)[1]:
        return "timeout"
    err = s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
    return "connected" if err == 0 else "refused" if err == errno.ECONNREFUSED else "error " + os.strerror(err)

def icmp_echo():
    body = struct.pack(">BBHHH", 8, 0, 0, 0x4c4f, 1) + b"probe!"
    total = sum(struct.unpack(">%dH" % (len(body) // 2), body))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return body[:2] + struct.pack(">H", ~total & 0xffff) + body[4:]

def send(spec, keep):
    """kind,host,port[,mark]: one packet (a TCP SYN), never waited for."""
    kind, host, port = spec.split(",")[:3]
    mark = spec.split(",")[3] if spec.count(",") == 3 else None
    family = socket.AF_INET6 if kind.endswith("6") else socket.AF_INET
    if kind == "icmp":
        s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)
    else:
        s = socket.socket(family, socket.SOCK_STREAM if kind.startswith("tcp") else socket.SOCK_DGRAM)
    if mark:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_MARK, int(mark))
    if kind == "dhcp":
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", 68))
    s.setblocking(False)
    try:
        if kind.startswith("tcp"):
            s.connect((host, int(port)))
        elif kind == "icmp":
            s.sendto(icmp_echo(), (host, 0))
        else:
            s.sendto(b"probe", (host, int(port)))
    except OSError:
        pass
    keep.append(s)

def counts(text):
    """The probe's sets: proto:address[:port] -> packets."""
    found = {}
    for item in json.loads(text)["nftables"]:
        s = item.get("set")
        for e in (s or {}).get("elem", []):
            e = e["elem"]
            parts = [str(p) for p in e["val"]["concat"]]
            address = "[%s]" % parts[0] if s["name"].startswith("v6") else parts[0]
            found[":".join([parts[1], address] + parts[2:])] = e["counter"]["packets"]
    return found

if cmd in ("tcp", "tcp6"):  # HOST PORT [WAIT]
    s = socket.socket(socket.AF_INET6 if cmd == "tcp6" else socket.AF_INET)
    say(connect(s, (sys.argv[2], int(sys.argv[3])), float(sys.argv[4]) if len(sys.argv) > 4 else 0.5))
elif cmd == "dns":  # HOST PORT
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.7)
    try:
        s.sendto(QUERY, (sys.argv[2], int(sys.argv[3])))
        say("reply" if s.recv(512)[:2] == QUERY[:2] else "other")
    except socket.timeout:
        say("timeout")
    except OSError as e:
        say("error " + e.strerror)
elif cmd == "battery":  # SPEC...: each once
    keep = []
    for spec in sys.argv[2:]:
        send(spec, keep)
    time.sleep(0.05)
elif cmd == "socks":  # PORT HOST DPORT: a SOCKS5 CONNECT, its answer not waited for
    try:
        s = socket.create_connection(("127.0.0.1", int(sys.argv[2])), timeout=2)
        s.sendall(b"\x05\x01\x00")
        if s.recv(2) != b"\x05\x00":
            say("no socks")
        s.sendall(b"\x05\x01\x00\x01" + socket.inet_aton(sys.argv[3]) + struct.pack(">H", int(sys.argv[4])))
        say("socks sent")
    except ConnectionRefusedError:
        say("refused")
    except OSError as e:
        say("error " + str(e))
elif cmd == "listen-tcp":  # HOST PORT LOG: each connection's original destination
    s = socket.socket(socket.AF_INET)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((sys.argv[2], int(sys.argv[3])))
    s.listen(1024)
    log = open(sys.argv[4], "a", buffering=1)
    open(sys.argv[4] + ".ready", "w").close()
    while True:
        c, _ = s.accept()
        where = "-"
        try:
            raw = c.getsockopt(socket.SOL_IP, 80, 16)  # SO_ORIGINAL_DST
            where = "%s:%d" % (socket.inet_ntoa(raw[4:8]), struct.unpack(">H", raw[2:4])[0])
        except OSError:
            pass
        log.write(where + "\n")
        c.close()
elif cmd == "listen-dns":  # PORT LOG
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", int(sys.argv[2])))
    log = open(sys.argv[3], "a", buffering=1)
    open(sys.argv[3] + ".ready", "w").close()
    while True:
        data, peer = s.recvfrom(512)
        log.write("query\n")
        s.sendto(data[:2] + b"\x81\x80" + data[4:6] + b"\x00\x00\x00\x00\x00\x00" + data[12:], peer)
elif cmd == "sender":  # STOP PROGRESS SPEC...: the specs again and again until STOP exists
    stop, progress, specs = sys.argv[2], sys.argv[3], sys.argv[4:]
    ring, n = [], 0
    while not os.path.exists(stop):
        for spec in specs:
            send(spec, ring)
        while len(ring) > 64:  # held a moment, closed (a SYN_SENT close sends nothing)
            ring.pop(0).close()
        n += 1
        if n % 5 == 0:
            with open(progress + ".new", "w") as f:
                f.write("%d\n" % n)
            os.replace(progress + ".new", progress)
        time.sleep(0.003)
elif cmd == "counts":  # the probe table (nft -j) on stdin
    for key, packets in sorted(counts(sys.stdin.read()).items()):
        print(key, packets)
elif cmd == "delta":  # BEFORE (counts' output): what grew since, sorted
    before = {}
    with open(sys.argv[2]) as f:
        for line in f:
            key, packets = line.split()
            before[key] = int(packets)
    now = counts(sys.stdin.read())
    print(" ".join(sorted(k for k, v in now.items() if v > before.get(k, 0))))
elif cmd == "field":  # KEY...: from status JSON on stdin (a.b for a nested key)
    status = json.load(sys.stdin)
    values = []
    for path in sys.argv[2:]:
        value = status
        for key in path.split("."):
            value = value.get(key) if isinstance(value, dict) else None
        if isinstance(value, dict) and set(value) == {"tor", "clash"}:
            value = "%s/%s" % (value["tor"], value["clash"])
        values.append("none" if value is None else json.dumps(value) if isinstance(value, bool)
                      else "-" if value == "" else str(value))
    print(" ".join(values))
elif cmd == "token":  # STATUS LAYER: the daemon token in status.json (status hides it)
    print(json.load(open(sys.argv[2]))["daemons"].get(sys.argv[3], {}).get("token", ""))
elif cmd == "hold":  # LOCK READY GO: hold LOCK (flock) until GO exists, at most a minute
    fd = os.open(sys.argv[2], os.O_RDWR | os.O_CREAT, 0o600)
    fcntl.flock(fd, fcntl.LOCK_EX)
    open(sys.argv[3], "w").close()
    for _ in range(600):
        if os.path.exists(sys.argv[4]):
            break
        time.sleep(0.1)
EOF
chmod 644 "$net"
py() { python3 -I "$net" "$@"; }
in_ns() { ip netns exec locachy-net "$@"; }
as_user() { # USER CMD...: in locachy-net as USER (root: as it is)
    local who=$1
    shift
    if [ "$who" = root ]; then in_ns "$@"; else in_ns setpriv --reuid="$who" --regid="$(id -g "$who")" --clear-groups "$@"; fi
}
who=root
[ "$ids" = 0 ] || who=nobody
field() { pol status | py field "$@"; }
set_to() { pol set --tor "${1%/*}" --clash "${1#*/}"; } # TOR/CLASH
ALLOWED="off/off off/tun off/proxy bridges/off bridges/tun bridges/proxy mihomo/tun mihomo/proxy"

# The tables there are, as one word: block, Tor's (and its front), Clash's.
sig() {
    local tor=none clash=0 block=0 table
    in_ns "$NFT" list table inet locachy-transition >/dev/null 2>&1 && block=1
    if table=$(in_ns "$NFT" list table inet locachy-tor 2>/dev/null); then
        tor=front-none
        grep -q 'meta mark 0x000007e7 counter name "front" accept' <<<"$table" && tor=mihomo
        grep -q 'oifname "mihomo" accept' <<<"$table" && tor=bridges
    fi
    in_ns "$NFT" list table inet locachy-clash >/dev/null 2>&1 && clash=1
    echo "block=$block tor=$tor clash=$clash"
}
want_sig() { # POLICY
    local tor=${1%/*} clash=0
    [ "$tor" != off ] || tor=none
    [ "$1" != off/tun ] || clash=1
    echo "block=0 tor=$tor clash=$clash"
}
# Applied: rc.conf, the status and the tables all say POLICY.
applied_ok() {
    [ "$(field configured applied desired phase guard)" = "$1 $1 $1 applied false" ] && [ "$(sig)" = "$(want_sig "$1")" ]
}
# Failed and blocked: the block in, the status failed.
blocked_ok() {
    [ "$(field phase guard)" = "failed true" ] && [ "$(sig | cut -d' ' -f1)" = block=1 ] && [ "$(measure)" = "$(want block)" ]
}

# What leaves: batteries of one packet each (a TCP SYN), by whom.
B_any=(tcp,192.0.2.1,80 udp,192.0.2.1,9999 udp6,2001:db8::99,9999 tcp6,2001:db8::99,80 tcp,10.99.0.7,80)
B_root=(icmp,192.0.2.1,0 dhcp,10.99.0.1,67 udp,10.99.0.1,53 tcp,192.0.2.1,8443,2023)
B_tor=(tcp,192.0.2.1,443)
B_gid=(tcp,192.0.2.1,7000)
K_any="tcp:192.0.2.1:80 udp:192.0.2.1:9999 udp:[2001:db8::99]:9999 tcp:[2001:db8::99]:80 tcp:10.99.0.7:80"
K_root="icmp:192.0.2.1 udp:10.99.0.1:67 udp:10.99.0.1:53 tcp:192.0.2.1:8443"
K_tor="tcp:192.0.2.1:443"
K_gid="tcp:192.0.2.1:7000"
snap() { in_ns "$NFT" -j list table inet probe | py counts >"$out/snap"; }
grew() { in_ns "$NFT" -j list table inet probe | py delta "$out/snap"; }
batteries() {
    as_user "$who" python3 -I "$net" battery "${B_any[@]}"
    in_ns python3 -I "$net" battery "${B_root[@]}"
    if [ "$ids" != 0 ]; then
        as_user tor python3 -I "$net" battery "${B_tor[@]}"
        in_ns setpriv --regid=2022 --clear-groups python3 -I "$net" battery "${B_gid[@]}"
    fi
}
measure() { snap; batteries; grew; }
# The keys a policy lets out (sorted, as delta prints them); block: the transition's.
want() {
    local keys
    case $1 in
    off/off | off/proxy) keys="$K_any $K_root $K_tor $K_gid" ;;
    off/tun) keys="udp:10.99.0.1:67 udp:10.99.0.1:53 $K_gid" ;;
    bridges/*) keys="udp:10.99.0.1:67 tcp:10.99.0.7:80 $K_tor" ;;
    mihomo/*) keys="udp:10.99.0.1:67 tcp:10.99.0.7:80 tcp:192.0.2.1:8443" ;;
    block) keys="udp:10.99.0.1:67" ;;
    esac
    [ "$ids" != 0 ] || keys=$(tr ' ' '\n' <<<"$keys" | grep -vxF -e "$K_tor" -e "$K_gid" | tr '\n' ' ')
    tr ' ' '\n' <<<"$keys" | grep . | python3 -c 'import sys; print(" ".join(sorted(l.strip() for l in sys.stdin)))'
}
[ "$ids" != 0 ] || skip "nobody, the tor user and group 2022 (no subordinate ids): their batteries and section 9"

# 1. A request before locachy-net exists: committed, applied when it comes.
: >"$RC"
pol set --clash tun
r=$?
check "no locachy-net: set exits 0, rc.conf committed, the status queued (exit $r)" \
    "[ $r = 0 ] && grep -qx 'service_clash=\"/usr/local/lib/locachy/clash-client\"' $RC && grep -qx 'clash_mode=\"tun\"' $RC && [ \"\$(field phase applied desired)\" = 'queued none off/tun' ]"

# locachy-net: dummy0 its uplink (10.99.0.0/24, 2001:db8::/64, the default
# routes), what leaves on it counted after every other table.
ip netns add locachy-net
in_ns ip link set lo up
in_ns ip link add dummy0 type dummy
in_ns ip addr add 10.99.0.2/24 dev dummy0
in_ns ip addr add 2001:db8::2/64 dev dummy0 nodad
in_ns ip link set dummy0 up
in_ns ip route add default via 10.99.0.1
in_ns ip -6 route add default via 2001:db8::1
in_ns "$NFT" -f - <<'EOF'
table inet probe {
    set v4 { type ipv4_addr . inet_proto . inet_service; flags dynamic; counter; size 65535; }
    set v4other { type ipv4_addr . inet_proto; flags dynamic; counter; size 65535; }
    set v6 { type ipv6_addr . inet_proto . inet_service; flags dynamic; counter; size 65535; }
    chain leaving {
        type filter hook postrouting priority 300; policy accept;
        oifname != "dummy0" return
        meta nfproto ipv4 meta l4proto { tcp, udp } add @v4 { ip daddr . meta l4proto . th dport }
        meta nfproto ipv4 meta l4proto != { tcp, udp } add @v4other { ip daddr . meta l4proto }
        ip6 daddr 2001:db8::/32 meta l4proto { tcp, udp } add @v6 { ip6 daddr . meta l4proto . th dport }
    }
}
EOF
# Tor's ports, stood in for (as root: who listens does not matter to the rules).
listeners=()
listen() { # NAME ARGS...
    local name=$1
    shift
    ip netns exec locachy-net python3 -I "$net" "$@" "$T/heard/$name" &
    listeners+=($!)
    until_true 50 "[ -e $T/heard/$name.ready ]"
}
listen trans listen-tcp 127.0.0.1 9040
listen dns listen-dns 5353
listen socks listen-tcp 127.0.0.1 9050

pol reconcile
r=$?
check "locachy-net made: reconcile (network-client's) applies the queued request (exit $r)" "[ $r = 0 ] && applied_ok off/tun"

# 2. Every allowed policy from every allowed policy; the refused ones.
for s in $ALLOWED; do
    bad=
    for t in $ALLOWED; do
        set_to "$s" 2>>"$out/set.log" || bad+=" back-to-$s"
        set_to "$t" 2>>"$out/set.log"
        r=$?
        [ $r = 0 ] && applied_ok "$t" || bad+=" $t(exit $r: $(pol status --text))"
    done
    check "from $s: each allowed policy applies${bad:+, not:$bad}" "[ -z \"\$bad\" ]"
done
for s in $ALLOWED; do
    set_to "$s" 2>>"$out/set.log"
    asks=("--tor mihomo --clash off")
    [ "${s#*/}" != off ] || asks+=("--tor mihomo")
    [ "${s%/*}" != mihomo ] || asks+=("--clash off")
    bad=
    for ask in "${asks[@]}"; do
        cp "$RC" "$out/rc.before"
        cp "$out/state/status.json" "$out/status.before"
        in_ns "$NFT" -s list ruleset >"$out/ruleset.before"
        # shellcheck disable=SC2086
        pol set $ask 2>"$out/refused.log"
        r=$?
        in_ns "$NFT" -s list ruleset >"$out/ruleset.after"
        [ $r = 2 ] && grep -q '^network policy: refused: ' "$out/refused.log" && cmp -s "$RC" "$out/rc.before" &&
            cmp -s "$out/state/status.json" "$out/status.before" && cmp -s "$out/ruleset.before" "$out/ruleset.after" ||
            bad+=" [$ask: exit $r]"
    done
    check "from $s: refused (${asks[*]}): exit 2, rc.conf, status and rules unchanged${bad:+, not:$bad}" "[ -z \"\$bad\" ]"
done
printf '%s\n' 'service_tor="/usr/local/lib/locachy/tor-client"' >"$RC"
pol reconcile 2>/dev/null
cp "$RC" "$out/rc.before"
pol set --clash tun 2>"$out/refused.log"
r=$?
check "Tor on without a front (rc.conf): a request keeping it is refused (exit $r), rc.conf unchanged" \
    "[ $r = 2 ] && grep -q 'Tor needs a way out' $out/refused.log && cmp -s $RC $out/rc.before"
pol set --tor bogus 2>/dev/null
r=$?
check "a malformed request: usage, exit 2 ($r)" "[ $r = 2 ]"
# A layer that is on needs its program, as settings offers a layer only then;
# turning one off needs none. A refused request changes nothing.
absent=$out/absent
for m in tun proxy; do
    set_to off/off 2>>"$out/set.log"
    cp "$RC" "$out/rc.before"
    cp "$out/state/status.json" "$out/status.before"
    in_ns "$NFT" -s list ruleset >"$out/ruleset.before"
    env LOCACHY_MIHOMO=$absent python3 "$helper" set --clash $m 2>"$out/refused.log"
    r=$?
    check "mihomo absent, off to Clash $m: exit 2, refused, rc.conf, status and rules unchanged ($r)" \
        "[ $r = 2 ] && grep -q '^network policy: refused: Clash is on, but mihomo is not installed' $out/refused.log && cmp -s $RC $out/rc.before && cmp -s $out/state/status.json $out/status.before && in_ns $NFT -s list ruleset | cmp -s - $out/ruleset.before"
done
set_to off/tun 2>>"$out/set.log"
env LOCACHY_MIHOMO=$absent python3 "$helper" set --clash off 2>>"$out/set.log"
r=$?
check "mihomo absent, Clash TUN to off: allowed, exit 0 ($r), applied" "[ $r = 0 ] && applied_ok off/off"
set_to off/tun 2>>"$out/set.log"
env LOCACHY_MIHOMO=$absent python3 "$helper" set --tor bridges 2>"$out/refused.log"
r=$?
check "mihomo absent, Clash TUN kept, Tor to bridges asked: exit 2 for Clash, applied unchanged ($r)" \
    "[ $r = 2 ] && grep -q '^network policy: refused: Clash is on, but mihomo is not installed' $out/refused.log && applied_ok off/tun"
set_to off/off 2>>"$out/set.log"
env LOCACHY_TOR=$absent python3 "$helper" set --tor mihomo --clash tun 2>"$out/refused.log"
r=$?
check "tor absent, off to Tor via mihomo: exit 2, refused, nothing changed ($r)" \
    "[ $r = 2 ] && grep -q '^network policy: refused: Tor is on, but tor is not installed' $out/refused.log && applied_ok off/off"
set_to mihomo/tun 2>>"$out/set.log"
env LOCACHY_TOR=$absent python3 "$helper" set --tor off 2>>"$out/set.log"
r=$?
check "tor absent, Tor off from mihomo/tun: allowed, exit 0 ($r), applied" "[ $r = 0 ] && applied_ok off/tun"
set_to off/off 2>>"$out/set.log"
env LOCACHY_LYREBIRD=$absent python3 "$helper" set --tor bridges 2>"$out/refused.log"
r=$?
check "lyrebird absent, Tor via bridges: exit 2, refused, nothing changed ($r)" \
    "[ $r = 2 ] && grep -q '^network policy: refused: the bridges need lyrebird' $out/refused.log && applied_ok off/off"
set_to off/off 2>>"$out/set.log"
bash "$src/link/tor-enforce" load bridges 2>>"$out/set.log"
r=$?
check "tor-enforce load bridges with Tor off in rc.conf: reconciles, loads no Tor rules (exit $r)" "[ $r = 0 ] && applied_ok off/off"
set_to bridges/off 2>>"$out/set.log"
bash "$src/link/tor-enforce" remove 2>>"$out/set.log"
r=$?
check "tor-enforce remove (an old daemon's, delayed) with Tor on: Tor's rules stay (exit $r)" "[ $r = 0 ] && applied_ok bridges/off"

# 3. What leaves under each policy.
for p in $ALLOWED; do
    set_to "$p" 2>>"$out/set.log"
    got=$(measure)
    check "$p: what leaves is what it allows: ${got:-nothing}" "[ \"\$got\" = \"\$(want $p)\" ]"
done
for p in bridges/off mihomo/tun; do
    set_to "$p" 2>>"$out/set.log"
    : >"$T/heard/trans"
    : >"$T/heard/socks"
    : >"$T/heard/dns"
    check "$p: TCP to 192.0.2.1:80 at the TransPort, DNS at the DNSPort, the SocksPort reached as it is" \
        "[ \"\$(as_user $who python3 -I $net tcp 192.0.2.1 80)\" = connected ] && grep -qx 192.0.2.1:80 $T/heard/trans && [ \"\$(as_user $who python3 -I $net dns 192.0.2.53 53)\" = reply ] && grep -q query $T/heard/dns && [ \"\$(as_user $who python3 -I $net tcp 127.0.0.1 9050)\" = connected ] && [ -s $T/heard/socks ] && ! grep -q :9050 $T/heard/trans"
done

# 4. lan: Tor's local networks again, in its table (never replaced).
set_to bridges/off 2>>"$out/set.log"
handle() { in_ns "$NFT" -a list table inet locachy-tor 2>/dev/null | sed -n '1s/.*# handle \([0-9]*\).*/\1/p'; }
h=$(handle)
in_ns ip addr add 10.88.0.2/24 dev dummy0
pol lan 2>"$out/lan.log"
r=$?
check "lan, Tor on, a new local network (10.88.0.0/24): in Tor's set, the table kept (exit $r)" \
    "[ $r = 0 ] && in_ns $NFT list set inet locachy-tor lan | grep -q 10.88.0.0/24 && [ \"\$(handle)\" = $h ] && applied_ok bridges/off || { cat $out/lan.log; false; }"
snap
as_user "$who" python3 -I "$net" battery tcp,10.88.0.7,80 tcp,10.77.0.7,80
got=$(grew)
check "lan: the new local network is reached, the others not ($got)" "[ \"\$got\" = tcp:10.88.0.7:80 ]"
in_ns ip addr add 10.77.0.2/24 dev dummy0
bash "$src/link/tor-enforce" lan 2>>"$out/lan.log"
r=$?
check "tor-enforce lan (tor-client's): the set has 10.77.0.0/24 too (exit $r)" \
    "[ $r = 0 ] && in_ns $NFT list set inet locachy-tor lan | grep -q 10.77.0.0/24 && applied_ok bridges/off"
in_ns ip addr del 10.88.0.2/24 dev dummy0
in_ns ip addr del 10.77.0.2/24 dev dummy0
pol lan 2>>"$out/lan.log"
set_to off/off 2>>"$out/set.log"
pol lan 2>>"$out/lan.log"
r=$?
check "lan, Tor off: nothing to do, no table (exit $r)" "[ $r = 0 ] && applied_ok off/off"

# 5. A failure at each step, from a policy with a direct path (off/off).
fail_case() { # NAME TARGET COMMITTED(0|1): the failure injected already
    local name=$1 target=$2 committed=$3 r token
    cp "$RC" "$out/rc.before"
    set_to "$target" 2>"$out/fail.log"
    r=$?
    check "$name: exit 1, the block stays, only DHCP leaves, status failed (exit $r: $(tail -n 1 "$out/fail.log"))" \
        "[ $r = 1 ] && blocked_ok && [ \"\$(field error)\" != - ] && pol status --text | grep -q '^failed: .*the network is blocked'"
    if [ "$committed" = 0 ]; then
        check "$name: rc.conf not changed" "cmp -s $RC $out/rc.before"
    else
        check "$name: rc.conf committed" "[ \"\$(field configured)\" = $target ]"
    fi
    inject_clear
    if [ "$committed" = 0 ]; then
        pol reconcile 2>/dev/null
        r=$?
        bash "$src/link/tor-enforce" load bridges 2>/dev/null
        local r2=$?
        token=$(pol daemon claim clash 2>/dev/null)
        local r3=$?
        pol daemon release clash "$token" 2>/dev/null
        local r4=$?
        check "$name: reconcile, tor-enforce load and a daemon claim and release keep the block (exits $r $r2 $r3 $r4)" \
            "[ '$r $r2 $r3 $r4' = '1 1 0 0' ] && [ -n '$token' ] && blocked_ok"
    else
        pol reconcile 2>/dev/null
        r=$?
        check "$name: committed, reconcile retries and applies (exit $r)" "[ $r = 0 ] && applied_ok $target"
    fi
    set_to "$target" 2>>"$out/set.log"
    r=$?
    check "$name: set again applies (exit $r)" "[ $r = 0 ] && applied_ok $target"
    set_to off/off 2>>"$out/set.log"
}
touch "$out/layer/tor-fail"
fail_case "Tor's rules fail (off to Tor bridges)" bridges/off 0
touch "$out/layer/clash-fail"
fail_case "Clash's rules fail (off to Clash TUN)" off/tun 0
echo "fail layers" >"$out/layer/nft"
fail_case "nft refuses the layers' batch (off to Tor via mihomo, Clash TUN)" mihomo/tun 0
rm -f "$RC.lock"
ln -s /nonexistent "$RC.lock"
fail_case "the rc.conf commit fails (off to Clash TUN)" off/tun 0
echo "fail unblock" >"$out/layer/nft"
fail_case "removing the block fails (off to Tor bridges, Clash TUN)" bridges/tun 1
# Not even the block: exit 4, the old restriction (or none) as it was.
for from in off/off bridges/off; do
    set_to "$from" 2>>"$out/set.log"
    to=off/tun
    [ "$from" = off/off ] || to=off/off
    cp "$RC" "$out/rc.before"
    echo "fail block" >"$out/layer/nft"
    set_to "$to" 2>"$out/fail.log"
    r=$?
    got=$(measure)
    check "from $from, the block cannot load: exit 4, failed, rc.conf and $from's rules as they were (exit $r; out: ${got:-nothing})" \
        "[ $r = 4 ] && [ \"\$(field phase guard)\" = 'failed false' ] && cmp -s $RC $out/rc.before && [ \"\$(sig)\" = \"\$(want_sig $from)\" ] && [ \"\$got\" = \"\$(want $from)\" ]"
    inject_clear
    pol reconcile 2>/dev/null
    r=$?
    check "from $from: then reconcile blocks, the failed request not dropped (exit $r)" "[ $r = 1 ] && blocked_ok"
    set_to "$to" 2>>"$out/set.log"
    r=$?
    check "from $from: set again applies $to (exit $r)" "[ $r = 0 ] && applied_ok $to"
done
set_to off/off 2>>"$out/set.log"

# 6. Inside a transition.
echo "hold layers" >"$out/layer/nft"
set_to off/tun 2>>"$out/set.log" &
setter=$!
until_true 100 "[ -e $out/layer/held ]"
got=$(measure)
check "off to Clash TUN, held before its rules: the block alone, only DHCP leaves (${got:-nothing})" \
    "[ -e $out/layer/held ] && [ \"\$(sig)\" = 'block=1 tor=none clash=0' ] && [ \"\$got\" = \"\$(want block)\" ]"
touch "$out/layer/go"
wait "$setter"
r=$?
inject_clear
check "and then it applies (exit $r)" "[ $r = 0 ] && applied_ok off/tun"
set_to bridges/off 2>>"$out/set.log"
rm -f "$out/held" "$out/go"
py hold "$RC.lock" "$out/held" "$out/go" &
holder=$!
until_true 50 "[ -e $out/held ]"
set_to off/tun 2>>"$out/set.log" &
setter=$!
until_true 100 "[ \"\$(sig)\" = 'block=1 tor=none clash=1' ]"
got=$(measure)
check "Tor to Clash TUN, held at the rc.conf commit: Clash's rules in one batch with Tor's gone, the block, only DHCP leaves (${got:-nothing})" \
    "[ \"\$(sig)\" = 'block=1 tor=none clash=1' ] && [ \"\$(py field phase <$out/state/status.json)\" = applying ] && grep -q '^service_tor=' $RC && [ \"\$got\" = \"\$(want block)\" ]"
touch "$out/go"
wait "$setter"
r=$?
wait "$holder"
check "and then it applies (exit $r)" "[ $r = 0 ] && applied_ok off/tun"

# 7. Senders running through repeated toggles.
rm -f "$T/send/"*
in_ns python3 -I "$net" sender "$T/send/stop" "$T/send/root" tcp,192.0.2.1,81 udp,192.0.2.1,9998 udp6,2001:db8::99,9998 &
s1=$!
as_user "$who" python3 -I "$net" sender "$T/send/stop" "$T/send/user" tcp,192.0.2.1,82 udp,192.0.2.1,9997 udp6,2001:db8::99,9997 &
s2=$!
progress() { echo $(($(cat "$T/send/root" 2>/dev/null || echo 0) + $(cat "$T/send/user" 2>/dev/null || echo 0))); }
sent_keys() { grep -E ':(192\.0\.2\.1|\[2001:db8::99\]):(8[12]|999[78])$' | tr '\n' ' '; }
settle() { local n; n=$(progress); until_true 100 "[ \$(progress) -ge $((n + 40)) ]"; }
until_true 100 "[ \$(progress) -ge 20 ]"
NODIRECT="off/tun bridges/off bridges/tun bridges/proxy mihomo/tun mihomo/proxy"
set_to off/tun 2>>"$out/set.log"
snap
p0=$(progress)
bad=
for round in 1 2 3 4; do
    for p in $NODIRECT; do
        set_to "$p" 2>>"$out/set.log" || bad+=" $p(exit $?)"
    done
done
settle
leaked=$(grew | tr ' ' '\n' | sent_keys)
check "24 toggles among the policies without a direct path, senders running ($(($(progress) - p0)) rounds): nothing of theirs left${leaked:+: $leaked}${bad:+; failed:$bad}" \
    "[ -z \"\$leaked\" ] && [ -z \"\$bad\" ] && [ \$(progress) -gt $((p0 + 40)) ]"
bad=
for round in 1 2; do
    for p in $NODIRECT; do
        set_to off/off 2>>"$out/set.log"
        snap
        settle
        [ -n "$(grew | tr ' ' '\n' | sent_keys)" ] || bad+=" off/off(nothing left)"
        set_to "$p" 2>>"$out/set.log"
        snap
        settle
        leaked=$(grew | tr ' ' '\n' | sent_keys)
        [ -z "$leaked" ] || bad+=" $p($leaked)"
    done
done
check "12 toggles direct and back: the senders' packets leave under off/off, none once each other policy is in${bad:+:$bad}" \
    "[ -z \"\$bad\" ]"
touch "$T/send/stop"
wait "$s1" "$s2"

# 8. Requests serialize on policy.lock; concurrent ones end consistent.
set_to off/off 2>>"$out/set.log"
rm -f "$out/held" "$out/go"
py hold "$out/state/policy.lock" "$out/held" "$out/go" &
holder=$!
until_true 50 "[ -e $out/held ]"
cp "$RC" "$out/rc.before"
cp "$out/state/status.json" "$out/status.before"
python3 "$helper" set --clash tun 2>>"$out/set.log" &
setter=$!
until_true 100 "grep -qE '^[0-9]+: -> FLOCK +ADVISORY +WRITE +$setter ' /proc/locks"
check "policy.lock held: a set waits on it, rc.conf and the status untouched" \
    "grep -qE '^[0-9]+: -> FLOCK +ADVISORY +WRITE +$setter ' /proc/locks && kill -0 $setter && cmp -s $RC $out/rc.before && cmp -s $out/state/status.json $out/status.before && [ \"\$(sig)\" = \"\$(want_sig off/off)\" ]"
touch "$out/go"
wait "$holder"
wait "$setter"
r=$?
check "released: the set applies (exit $r)" "[ $r = 0 ] && applied_ok off/tun"
pids=() asked=()
i=0
for p in $ALLOWED $ALLOWED mihomo/off; do
    asked+=("$p")
    pol set --tor "${p%/*}" --clash "${p#*/}" 2>>"$out/set.log" &
    pids+=($!)
done
codes=
for k in "${!pids[@]}"; do
    wait "${pids[$k]}"
    codes+="${asked[$k]}=$? "
done
final=$(field configured)
check "17 sets at once: 16 applied, the refused one 2, the end one policy everywhere ($final)" \
    "[ \"\$(tr ' ' '\n' <<<\"\$codes\" | grep -c '=0$')\" = 16 ] && grep -q 'mihomo/off=2' <<<\"\$codes\" && applied_ok $final && [[ \" $ALLOWED \" = *\" $final \"* ]] || { echo \"exits: \$codes\"; false; }"

# 9. Daemon generations: clash-client and the real mihomo, tor-client.
if [ "$ids" = 0 ] || [ ! -x "$MIHOMO" ] || [ ! -c /dev/net/tun ]; then
    skip "daemon generations (needs subordinate ids, $MIHOMO and /dev/net/tun)"
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
    start_clash() {
        bash "$src/link/clash-client" >>"$out/clash.log" 2>&1 &
        cpid=$!
    }
    up() { in_ns ip link show mihomo >/dev/null 2>&1; }
    rules9() { in_ns ip rule show | grep -cE '^(9[0-9]{3}|8999):'; }
    routes9() { in_ns ip route show table 2022 2>/dev/null | grep -c .; }
    mpid() { pgrep -x mihomo; }
    token() { py token "$out/state/status.json" "$1"; }
    running() { up && [ "$(rules9)" -gt 0 ] && [ "$(routes9)" -gt 0 ] && [ -n "$(mpid)" ]; }
    set_to off/tun 2>>"$out/set.log"
    start_clash
    until_true 600 running
    rules_n=$(rules9)
    tok1=$(token clash)
    check "Clash TUN, clash-client: mihomo's TUN, $rules_n rules and its routes; the status: running" \
        "running && [ -n '$tok1' ] && [ \"\$(field daemons.clash.state)\" = running ]"
    snap
    r=$(as_user nobody python3 -I "$net" tcp 192.0.2.1 80)
    until_true 50 "grew | grep -q 'tcp:192.0.2.10:1080'"
    got=$(grew)
    check "nobody to 192.0.2.1:80 ($r): into mihomo's TUN, which dials its proxy 192.0.2.10:1080; nothing to 192.0.2.1 leaves ($got)" \
        "[ '$r' = connected ] && grep -q 'tcp:192.0.2.10:1080' <<<'$got' && ! grep -q '192.0.2.1:' <<<'$got'"
    cp "$out/state/status.json" "$out/status.before"
    pol daemon release clash 0123456789abcdef0123456789abcdef
    r=$?
    check "a stale token's release: exit 3 ($r), mihomo's TUN, rules and the status untouched" \
        "[ $r = 3 ] && running && [ \$(rules9) = $rules_n ] && cmp -s $out/state/status.json $out/status.before"
    kill -KILL "$(mpid)"
    until_true 100 "! kill -0 $cpid 2>/dev/null"
    wait "$cpid"
    r=$?
    got=$(measure)
    check "mihomo killed: clash-client releases, failed ($r); its TUN, rules and routes gone; Clash's rules stay; no direct path ($got)" \
        "[ $r = 1 ] && ! up && [ \$(rules9) = 0 ] && [ \$(routes9) = 0 ] && [ \"\$(field daemons.clash.state)\" = failed ] && applied_ok off/tun && [ \"\$got\" = \"\$(want off/tun)\" ]"
    start_clash
    until_true 600 running
    tok2=$(token clash)
    m=$(mpid)
    kill -KILL "$cpid" "$m"
    wait "$cpid" 2>/dev/null
    until_true 50 "! kill -0 $m 2>/dev/null"
    got=$(measure)
    check "clash-client and mihomo killed at once: their rules left ($(rules9)), no direct path ($got)" \
        "[ \$(rules9) -gt 0 ] && ! up && [ \"\$got\" = \"\$(want off/tun)\" ]"
    start_clash
    until_true 600 "running && [ \"\$(token clash)\" != '$tok2' ]"
    tok3=$(token clash)
    check "the next generation's claim cleaned them: its own $rules_n rules, no more" \
        "running && [ \$(rules9) = $rules_n ] && [ -n '$tok3' ] && [ '$tok3' != '$tok2' ]"
    pol daemon release clash "$tok2"
    r=$?
    check "the killed generation's release: exit 3 ($r), the newer one's TUN, rules and routes kept" \
        "[ $r = 3 ] && running && [ \$(rules9) = $rules_n ] && [ \"\$(token clash)\" = '$tok3' ]"
    m=$(mpid)
    kill -KILL "$cpid"
    wait "$cpid" 2>/dev/null
    until_true 50 "! kill -0 $m 2>/dev/null"
    check "clash-client killed alone: its mihomo dies with it" "! kill -0 $m 2>/dev/null"
    start_clash
    until_true 300 "running && [ \"\$(token clash)\" != '$tok3' ]"
    check "and the next generation starts (no orphan holds its lifecycle lock)" \
        "running && [ \"\$(token clash)\" != '$tok3' ]"
    kill -KILL "$m" 2>/dev/null # an orphan, if one was left
    until_true 300 "running && [ \"\$(token clash)\" != '$tok3' ]"
    kill -TERM "$cpid"
    wait "$cpid"
    r=$?
    check "clash-client ended (TERM, exit $r): its TUN, rules and routes gone, stopped; Clash's rules stay" \
        "[ $r = 0 ] && ! up && [ \$(rules9) = 0 ] && [ \$(routes9) = 0 ] && [ \"\$(field daemons.clash.state)\" = stopped ] && applied_ok off/tun"

    # Tor via mihomo: mihomo's port for Tor; killed, nothing goes direct.
    set_to mihomo/tun 2>>"$out/set.log"
    start_clash
    until_true 600 "running && [ \"\$(as_user tor python3 -I $net tcp 127.0.0.1 7891)\" = connected ]"
    snap
    r=$(as_user tor python3 -I "$net" socks 7891 192.0.2.1 443)
    until_true 50 "grew | grep -q 'tcp:192.0.2.10:1080'"
    got=$(grew)
    check "Tor via mihomo: the tor user through mihomo's port ($r): out to its proxy, marked; nothing to 192.0.2.1 ($got)" \
        "grep -q 'tcp:192.0.2.10:1080' <<<'$got' && ! grep -q '192.0.2.1:' <<<'$got' && [ \$(in_ns $NFT list counter inet locachy-tor front | sed -n 's/.*packets \([0-9]*\).*/\1/p') -gt 0 ]"
    : >"$T/heard/trans"
    check "Tor via mihomo: nobody to 192.0.2.1:80 at the TransPort, not into mihomo" \
        "[ \"\$(as_user nobody python3 -I $net tcp 192.0.2.1 80)\" = connected ] && grep -qx 192.0.2.1:80 $T/heard/trans"
    kill -KILL "$(mpid)"
    until_true 100 "! kill -0 $cpid 2>/dev/null"
    wait "$cpid"
    got=$(measure)
    check "Tor via mihomo, mihomo killed: its port refuses, its TUN and rules gone, no direct path ($got)" \
        "[ \"\$(as_user tor python3 -I $net tcp 127.0.0.1 7891)\" = refused ] && ! up && [ \$(rules9) = 0 ] && applied_ok mihomo/tun && [ \"\$got\" = \"\$(want mihomo/tun)\" ]"

    # tor-client, with a stand-in for tor (a listener as the tor user).
    printf '#!/bin/sh\n[ "${1:-}" = --verify-config ] && exit 0\nexec python3 -I %s listen-tcp 127.0.0.1 9049 %s/heard/tor\n' \
        "$net" "$T" >"$T/tor"
    chmod 755 "$T/tor"
    tpid=
    start_tor() {
        rm -f "$T/heard/tor.ready"
        bash "$src/link/tor-client" >>"$out/tor.log" 2>&1 &
        tpid=$!
    }
    tor_pid() { pgrep -f "listen-tcp 127.0.0.1 9049"; }
    start_tor
    until_true 300 "[ -e $T/heard/tor.ready ] && [ -n \"\$(tor_pid)\" ]"
    t=$(tor_pid)
    ttok=$(token tor)
    check "tor-client (Tor via mihomo): claimed, its tor running as the tor user" \
        "[ -n '$t' ] && [ \"\$(stat -c %U /proc/$t)\" = tor ] && [ -n '$ttok' ] && [ \"\$(field daemons.tor.state)\" = running ]"
    kill -KILL "$tpid"
    wait "$tpid" 2>/dev/null
    until_true 50 "! kill -0 $t 2>/dev/null"
    check "tor-client killed alone: its tor dies with it" "! kill -0 $t 2>/dev/null"
    start_tor
    until_true 300 "[ -e $T/heard/tor.ready ] && [ \"\$(token tor)\" != '$ttok' ]"
    check "and the next tor-client starts (no orphan holds its lifecycle lock); Tor's rules stayed throughout" \
        "[ -e $T/heard/tor.ready ] && [ \"\$(token tor)\" != '$ttok' ] && applied_ok mihomo/tun"
    kill -KILL "$t" 2>/dev/null # an orphan, if one was left
    until_true 300 "[ -e $T/heard/tor.ready ] && [ \"\$(token tor)\" != '$ttok' ]"
    t=$(tor_pid)
    kill -TERM "$tpid"
    wait "$tpid"
    r=$?
    check "tor-client ended (TERM, exit $r): its tor gone, stopped; Tor's rules stay while it is on" \
        "[ $r = 0 ] && ! kill -0 $t 2>/dev/null && [ \"\$(field daemons.tor.state)\" = stopped ] && applied_ok mihomo/tun"
    set_to off/off 2>>"$out/set.log"
fi

kill "${listeners[@]}" 2>/dev/null
wait "${listeners[@]}" 2>/dev/null
rm -rf "$out/tor-run" "$out/tor-data" # the tor user's: removable from outside too
echo "network policy: $passed passed, $fail failed"
[ "$fail" = 0 ] && echo "PASS network policy: fail-closed transitions, by packets and rules" ||
    echo "FAIL network policy (files in $out)"
[ "$fail" = 0 ]
