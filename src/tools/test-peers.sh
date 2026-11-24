#!/bin/sh
# Tests for the shared once-forever peer store (locachy-pair / locachy_peers.py),
# extdisplay-broker persistent peer PSK authentication, and Thunderbolt
# shared-link decoupling (extdisplay + compute concurrent over one link).
# Linux only; runs inside unprivileged user/net/mount namespaces; nothing installed.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
if [ "$(uname -s)" != Linux ]; then
    echo "SKIP: test-peers.sh requires Linux"
    exit 77
fi
if ! pkg-config --exists openssl avahi-core; then
    echo "SKIP: OpenSSL and Avahi development files are not installed"
    exit 77
fi
if [ $# -ge 1 ]; then
    mkdir -p "$1"
    out=$(CDPATH= cd -- "$1" && pwd)
else
    out=$(mktemp -d "${TMPDIR:-/tmp}/locachy-peers.XXXXXX")
    trap 'rm -rf "$out"' EXIT
fi

fails=0
note() {
    echo "$1: $2"
    [ "$1" != FAIL ] || fails=$((fails + 1))
}

# 1. Peer store CLI & permission invariants
peers_dir="$out/peers"
client_dir="$out/client-mac"
client2_dir="$out/client-mac2"
export LOCACHY_PEERS_DIR="$peers_dir"

status_init=$(python3 "$src/link/locachy-pair" status --json)
if echo "$status_init" | grep -q '"initialized": false' && echo "$status_init" | grep -q '"peer_count": 0'; then
    note PASS "peers: default state is uninitialized with 0 paired peers"
else
    note FAIL "peers: unexpected default status: $status_init"
fi

token=$(python3 "$src/link/locachy-pair" add --yes --name "MacBook Pro" --scope extdisplay,compute --token)
if echo "$token" | grep -q '^locachy-pair-v1:'; then
    note PASS "peers: locachy-pair add generates one-time pairing token"
else
    note FAIL "peers: token missing prefix: $token"
fi

python3 "$src/link/locachy-pair" export-client --token "$token" --out-dir "$client_dir" >/dev/null
dir_mode=$(stat -c '%a' "$peers_dir")
host_mode=$(stat -c '%a' "$peers_dir/host.json")
key_mode=$(stat -c '%a' "$peers_dir/host-key.pem")
peer_file=$(ls "$peers_dir/peers/"*.json | head -n 1)
peer_mode=$(stat -c '%a' "$peer_file")
if [ "$dir_mode" = 700 ] && [ "$host_mode" = 600 ] && [ "$key_mode" = 600 ] && [ "$peer_mode" = 600 ]; then
    note PASS "peers: store permissions enforced (0700 dir, 0600 files)"
else
    note FAIL "peers: unexpected modes dir=$dir_mode host=$host_mode key=$key_mode peer=$peer_mode"
fi

# Host peer file must NEVER retain the client's private key.
if grep -q 'PRIVATE KEY' "$peer_file"; then
    note FAIL "peers: host peer record leaked client private key"
else
    note PASS "peers: host peer record does not store client private key"
fi

# Client dir must contain host-cert.pem, client-cert.pem, client-key.pem, peer.json all 0600.
if [ "$(stat -c '%a' "$client_dir/client-key.pem")" = 600 ] && [ -s "$client_dir/host-cert.pem" ] && [ -s "$client_dir/peer.json" ]; then
    note PASS "peers: export-client writes 0600 client bundle"
else
    note FAIL "peers: export-client bundle missing or wrong mode"
fi

# List must not print secrets.
psk_hex=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["extdisplay_psk_hex"])' "$client_dir/peer.json")
list_out=$(python3 "$src/link/locachy-pair" list --json)
if echo "$list_out" | grep -q '"MacBook Pro"' && ! echo "$list_out" | grep -q "$psk_hex"; then
    note PASS "peers: list shows peer metadata without leaking PSK"
else
    note FAIL "peers: list leaked secret or missed peer"
fi

# Add a second peer, then revoke it and confirm removal.
python3 "$src/link/locachy-pair" add --name "Refused Mac" --scope compute >/dev/null 2>&1 </dev/null && note FAIL "peers: pairing without confirmation was not refused" || note PASS "peers: pairing without --yes or a terminal is refused"
python3 "$src/link/locachy-pair" add --yes --name "Second Mac" --scope extdisplay --no-client-cert --out-dir "$client2_dir" >/dev/null
python3 "$src/link/locachy-pair" revoke "Second Mac" >/dev/null
if ! python3 "$src/link/locachy-pair" list --json | grep -q '"Second Mac"'; then
    note PASS "peers: revoke removes peer immediately"
else
    note FAIL "peers: revoked peer still present"
fi

# 2. Broker persistent PSK identity + shared-netns Thunderbolt decoupling test
if ! python3 -I -c 'import ssl, sys; sys.exit(0 if hasattr(ssl.SSLContext, "set_psk_client_callback") else 1)' 2>/dev/null; then
    echo "SKIP: broker persistent-peer test requires Python 3.13+ TLS PSK support"
elif ! unshare --user --map-auto --map-root-user --net --mount true >/dev/null 2>&1; then
    echo "SKIP: broker persistent-peer test requires unprivileged user/net/mount namespaces"
else
    cflags="-O2 -Wall -Wextra -Werror -D_GNU_SOURCE"
    broker_src="$src/progs/extdisplay-broker.c $src/progs/extdisplay-broker-net.c $src/progs/extdisplay-broker-pump.c $src/progs/extdisplay-broker-tls.c $src/progs/extdisplay-discovery.c"
    lw="$out/broker-shared"
    mkdir -p "$lw"
    rc_file="$lw/rc.conf"
    cat >"$rc_file" <<'EOF'
extdisplay_tb_shared="1"
EOF
    cc $cflags $(pkg-config --cflags avahi-core) -o "$lw/extdisplay-advertise" \
        "$src/progs/extdisplay-advertise.c" $(pkg-config --libs avahi-core)
    cc $cflags $(pkg-config --cflags openssl) \
        -DEXTDISPLAY_DISCOVERY_PATH=\"$lw/extdisplay-advertise\" \
        -DEXTDISPLAY_BROKER_TEST_DRIVER=\"veth\" \
        -DEXTDISPLAY_BROKER_TEST_HOME_NETNS=\"$lw/home\" \
        -DEXTDISPLAY_BROKER_TEST_STATE_DIR=\"$lw/state\" \
        -DEXTDISPLAY_BROKER_TEST_PEERS_DIR=\"$peers_dir\" \
        -DEXTDISPLAY_BROKER_TEST_RC_CONF=\"$rc_file\" \
        -DEXTDISPLAY_BROKER_TEST_ATTEMPT_SECONDS=12 \
        -o "$lw/extdisplay-broker" $broker_src $(pkg-config --libs openssl)
    cc $cflags -o "$lw/test-extdisplay-broker-ctl" "$src/tools/test-extdisplay-broker-ctl.c"

    cat >"$lw/shared-driver.py" <<'PYEOF'
import json
import os
import queue
import socket
import ssl
import subprocess
import sys
import threading
import time
from pathlib import Path

W, SRC, PEERS_DIR, CLIENT_DIR = sys.argv[1:5]
CTL = W + "/test-extdisplay-broker-ctl"
BROKER = W + "/extdisplay-broker"
COMPUTE = SRC + "/link/locachy-compute"
PAIR = SRC + "/link/locachy-pair"
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS" if ok else "FAIL") + ": " + name + ("" if ok or not detail else " -- " + detail), flush=True)
    if not ok:
        fails += 1


def sh(*argv):
    r = subprocess.run(list(argv), capture_output=True, text=True, timeout=30)
    return r.returncode, r.stdout + r.stderr


def reader(stream, q):
    for line in stream:
        q.put(line.rstrip("\n"))
    q.put(None)


class Broker:
    def __init__(self, extra_args=None):
        # Run test-extdisplay-broker-ctl or spawn broker directly with extra flags.
        self.p = subprocess.Popen([CTL, BROKER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.out, self.err = queue.Queue(), queue.Queue()
        threading.Thread(target=reader, args=(self.p.stdout, self.out), daemon=True).start()
        threading.Thread(target=reader, args=(self.p.stderr, self.err), daemon=True).start()
        self.pid = int(self.expect("pid ")[4:])

    def send(self, text):
        self.p.stdin.write("> " + text + "\n")
        self.p.stdin.flush()

    def plain(self, text):
        self.p.stdin.write("plain " + text + "\n")
        self.p.stdin.flush()

    def expect(self, prefix, timeout=15):
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError("waiting for " + prefix)
            line = self.out.get(timeout=left)
            if line is None:
                raise EOFError("broker closed while waiting for " + prefix)
            if line.startswith(prefix):
                return line


def tls_connect(addr, ifname, port, key_hex, identity):
    scope = socket.if_nametoindex(ifname)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.set_ciphers("PSK-AES128-GCM-SHA256")
    ctx.set_psk_client_callback(lambda hint: (identity.encode(), bytes.fromhex(key_hex)))
    ctx.set_alpn_protocols(["locachy-extdisplay-v1"])
    raw = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    raw.settimeout(10)
    raw.connect((addr, int(port), 0, scope))
    return ctx.wrap_socket(raw)


meta = json.loads((Path(CLIENT_DIR) / "peer.json").read_text())

# Start compute daemon bound to the SAME TB veth interface (extdisplay0) in home netns!
env_comp = dict(os.environ)
env_comp["LOCACHY_PEERS_DIR"] = PEERS_DIR
env_comp["LOCACHY_COMPUTE_BASE"] = W + "/compute-base"
env_comp["LOCACHY_COMPUTE_RUN"] = W + "/compute-run"
env_comp["LOCACHY_COMPUTE_ALLOW_NO_CGROUP"] = "1"
ready_file = W + "/compute-ready.json"
# Wait until extdisplay0 has a non-tentative fe80:: address
tb_ll = ""
for _ in range(50):
    rc, out = sh("ip", "-j", "-6", "addr", "show", "dev", "extdisplay0")
    if rc == 0:
        for item in json.loads(out):
            for ai in item.get("addr_info", []):
                if ai.get("local", "").startswith("fe80:") and not ai.get("tentative"):
                    tb_ll = ai["local"]
                    break
    if tb_ll:
        break
    time.sleep(0.1)
check("shared-tb: veth link-local ready", bool(tb_ll), tb_ll)

comp_proc = subprocess.Popen(
    ["python3", COMPUTE, "serve", "--port", "27184", "--bind-tb", "--tb-iface", "extdisplay0", "--ready-file", ready_file],
    env=env_comp,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)
try:
    for _ in range(60):
        if os.path.exists(ready_file):
            break
        time.sleep(0.1)
    check("shared-tb: compute daemon bound on TB fe80::", os.path.exists(ready_file))

    # Now start extdisplay-broker in shared-netns mode on the SAME interface!
    b = Broker()
    b.send("pair extdisplay0")
    lease = b.expect("< lease ").split()
    code = b.expect("< code ")[7:]
    addr, port = lease[3], int(lease[4])

    # Crucial: extdisplay0 MUST STILL BE in the home netns (NOT ripped away!)
    check("shared-tb: extdisplay0 remains in home netns during active lease", sh("ip", "-o", "link", "show", "dev", "extdisplay0")[0] == 0)

    # Connect to extdisplay-broker using the PERSISTENT paired peer identity & PSK (NO ephemeral code needed!)
    conn = tls_connect(addr, "extdisplaypeer", port, meta["extdisplay_psk_hex"], meta["extdisplay_identity"])
    paired = b.expect("< paired")
    check("shared-tb: persistent peer PSK authenticates without typing ephemeral code", paired == "< paired +fd", paired)
    conn.sendall(b"persistent-hello")
    check("shared-tb: persistent extdisplay plaintext reaches broker", b.expect("plain< ") == "plain< persistent-hello")

    # WHILE extdisplay TLS session is live over extdisplay0, run a compute mTLS health check over the SAME link!
    r = subprocess.run(
        ["python3", COMPUTE, "remote", "--creds-dir", CLIENT_DIR, "--host", f"{tb_ll}%extdisplaypeer", "--port", "27184", "health"],
        capture_output=True, text=True, timeout=15,
    )
    check("shared-tb: compute mTLS succeeds concurrently with active extdisplay session on same TB link",
          r.returncode == 0 and meta["peer_id"] in r.stdout, r.stdout + r.stderr)

    # The shipped locachy-filter: a port outside the TB allow-list is dropped on the
    # TB link (a timeout, not a refusal), and a non-TB link never reaches 27183.
    def dropped(addr_ll, peer_if, port, scope_if):
        s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        s.settimeout(2)
        try:
            s.connect((addr_ll, port, 0, socket.if_nametoindex(scope_if)))
            return False
        except TimeoutError:
            return True
        except OSError:
            return False
        finally:
            s.close()

    check("filter: TB link port outside the allow-list is dropped", dropped(tb_ll, "extdisplaypeer", 2222, "extdisplaypeer"))
    xnet_ll = ""
    rc, out = sh("ip", "-j", "-6", "addr", "show", "dev", "xnet0")
    for item in json.loads(out) if rc == 0 else []:
        for ai in item.get("addr_info", []):
            if ai.get("local", "").startswith("fe80:") and not ai.get("tentative"):
                xnet_ll = ai["local"]
    check("filter: non-TB link has link-local address for the test", bool(xnet_ll), xnet_ll)
    check("filter: non-TB link never reaches the TB ports (27183)", dropped(xnet_ll, "xnetpeer", 27183, "xnetpeer"))

    b.plain("persistent-ack")
    got = conn.recv(64)
    check("shared-tb: extdisplay stream remains healthy after concurrent compute call", got == b"persistent-ack", repr(got))
    conn.close()
    b.expect("< ended ")
    b.send("release")
    b.expect("< ended ")
    b.expect("exit ")
    b.p.stdin.close()

    # Paired-only mode: ephemeral code MUST be refused while paired peer succeeds.
    Path(W + "/rc.conf").write_text('extdisplay_tb_shared="1"\nextdisplay_paired_only="1"\n')
    b2 = Broker()
    b2.send("pair extdisplay0")
    lease2 = b2.expect("< lease ").split()
    _ = b2.expect("< code ")
    addr2, port2 = lease2[3], int(lease2[4])
    refused = False
    try:
        bad = tls_connect(addr2, "extdisplaypeer", port2, "00" * 16, "locachy-extdisplay-v1")
        bad.close()
    except (ssl.SSLError, OSError):
        refused = True
    check("paired-only: ephemeral identity refused when paired_only=1", refused)
    b2.expect("< ended ")
    b2.send("release")
    b2.expect("< ended ")
    b2.expect("exit ")
    b2.p.stdin.close()
finally:
    comp_proc.terminate()
    try:
        comp_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        comp_proc.kill()

sys.exit(0 if fails == 0 else 1)
PYEOF

    cat >"$lw/inner.sh" <<'SHEOF'
#!/bin/sh
set -eu
W=$1; SRC=$2; PEERS=$3; CLIENT=$4
touch "$W/home"
mount --bind /proc/self/ns/net "$W/home"
ip link add extdisplay0 type veth peer name extdisplaypeer
ip link set extdisplay0 up
ip link set extdisplaypeer up
# A non-Thunderbolt link (stands in for Ethernet or Wi-Fi): never allow-listed.
ip link add xnet0 type veth peer name xnetpeer
ip link set xnet0 up
ip link set xnetpeer up
# The shipped inbound filter, verbatim from network-client, with the sets that
# sync_tb_filter fills for a verified thunderbolt-net interface.
awk '/^ip netns exec "\$NS" nft -f - <<.EOF.$/{f=1;next} /^EOF$/{if(f)exit} f' "$SRC/link/network-client" >"$W/locachy-filter.nft"
test -s "$W/locachy-filter.nft"
nft -f "$W/locachy-filter.nft"
nft add element inet locachy-filter tb_ifaces '{ "extdisplay0" }'
nft add element inet locachy-filter tb_tcp_ports '{ 27183, 27184 }'
nft add element inet locachy-filter tb_udp_ports '{ 5353 }'
rm -rf "$W/state"
exec python3 "$W/shared-driver.py" "$W" "$SRC" "$PEERS" "$CLIENT"
SHEOF

    unshare --user --map-auto --map-root-user --net --mount --propagation private \
        sh "$lw/inner.sh" "$lw" "$src" "$peers_dir" "$client_dir" >"$lw/run.log" 2>&1 || :
    grep -E '^(PASS|FAIL): ' "$lw/run.log" || cat "$lw/run.log"
    sub_fails=$(grep -c '^FAIL: ' "$lw/run.log" || :)
    fails=$((fails + sub_fails))
    if ! grep -q '^PASS: shared-tb: compute mTLS succeeds concurrently' "$lw/run.log"; then
        note FAIL "shared-tb: concurrent driver did not complete: $(tail -n 10 "$lw/run.log" | tr '\n' ' ')"
    fi
fi

echo "peers tests: $fails failed"
[ "$fails" -eq 0 ]
