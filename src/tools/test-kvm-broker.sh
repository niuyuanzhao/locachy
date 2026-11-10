#!/bin/sh
# The extended-display broker's tests (tools/KVM.md), Linux only:
#   build    the broker with the production flags;
#   code     pairing-code encoding and canonical decoding;
#   tls      the cable TLS profile on loopback, through the broker's own
#            server context and pump, against Python's ssl as the peer;
#   lease    the broker itself through its control socket, in a user and
#            network namespace of the test's own, with a veth pair as the
#            cable and compile-time fixtures (driver name, namespace path,
#            state directory, attempt timeout, pump probes): lease, session
#            namespace contents, pairing, refusal, cancel, timeout, carrier
#            loss, control EOF, protocol errors, a killed broker, a device
#            that took the name meanwhile, and the pump's sandbox.
# usage: test-kvm-broker.sh [WORK_DIRECTORY]
# Without a directory a temporary one is used and removed. Nothing is
# installed, and nothing outside the test's own namespaces is changed.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
if [ $# -ge 1 ]; then
    mkdir -p "$1"
    out=$(CDPATH= cd -- "$1" && pwd)
else
    out=$(mktemp -d "${TMPDIR:-/tmp}/locachy-kvm-broker.XXXXXX")
    trap 'rm -rf "$out"' EXIT
fi
if [ "$(uname -s)" != Linux ]; then
    echo "SKIP: kvm-broker needs Linux (rtnetlink, namespaces, Landlock, seccomp)"
    exit 77
fi
if ! pkg-config --exists openssl; then
    echo "SKIP: OpenSSL development files are not installed"
    exit 77
fi
cflags="-O2 -Wall -Wextra -Werror -D_GNU_SOURCE"
broker_src="$src/progs/kvm-broker.c $src/progs/kvm-broker-net.c $src/progs/kvm-broker-pump.c $src/progs/kvm-broker-tls.c"
fails=0
note() { # PASS|FAIL text
    echo "$1: $2"
    [ "$1" != FAIL ] || fails=$((fails + 1))
}

# build
if cc $cflags $(pkg-config --cflags openssl) -o "$out/kvm-broker" $broker_src $(pkg-config --libs openssl) \
    2>"$out/build.log"; then
    note PASS "build: kvm-broker builds with the production flags"
else
    note FAIL "build: kvm-broker does not build: $(head -c 400 "$out/build.log" | tr '\n' ' ')"
fi
cc $cflags $(pkg-config --cflags openssl) -o "$out/test-kvm-broker-tls" "$src/tools/test-kvm-broker-tls.c" \
    "$src/progs/kvm-broker-tls.c" $(pkg-config --libs openssl)
cc $cflags -o "$out/test-kvm-broker-ctl" "$src/tools/test-kvm-broker-ctl.c"

# code
"$out/test-kvm-broker-tls" code >"$out/code.log" 2>&1 || :
grep -E '^(PASS|FAIL): ' "$out/code.log" || :
fails=$((fails + $(grep -c '^FAIL: ' "$out/code.log" || :)))
grep -q '^code: 0 failure' "$out/code.log" || note FAIL "code: the code cases did not all run"

# tls
if ! python3 -I -c 'import ssl, sys; sys.exit(0 if hasattr(ssl.SSLContext, "set_psk_client_callback") else 1)' \
    2>/dev/null; then
    echo "SKIP: tls and lease: python3 has no TLS PSK client support (Python 3.13 or newer is needed)"
    echo "kvm-broker tests: $fails failed"
    [ "$fails" -eq 0 ]
    exit
fi
cat >"$out/tls-client.py" <<'EOF'
# Loopback TLS peer. argv: PORT KEY_HEX IDENTITY ALPN CIPHER VERSION MODE
# ALPN or CIPHER "-": not set. MODE: hs (handshake), echo, twice (resumption).
import socket
import ssl
import sys

port, key_hex, identity, alpn, cipher, version, mode = sys.argv[1:8]
key = bytes.fromhex(key_hex)


def context():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    tls = ssl.TLSVersion.TLSv1_2 if version == "1.2" else ssl.TLSVersion.TLSv1_3
    ctx.minimum_version = ctx.maximum_version = tls
    if cipher != "-":
        ctx.set_ciphers(cipher)
    ctx.set_psk_client_callback(lambda hint: (identity.encode(), key))
    if alpn != "-":
        ctx.set_alpn_protocols([alpn])
    return ctx


def connect(ctx, session=None):
    raw = socket.create_connection(("127.0.0.1", int(port)), timeout=10)
    return ctx.wrap_socket(raw, session=session)


ctx = context()
try:
    if mode == "twice":
        first = connect(ctx)
        session = first.session
        first.close()
        second = connect(ctx, session)
        print("CLIENT second reused=" + str(second.session_reused))
        second.close()
    else:
        conn = connect(ctx)
        print("CLIENT handshake " + conn.version() + " " + conn.cipher()[0])
        if mode == "echo":
            conn.sendall(b"ping-kvm-1\n")
            got = b""
            while len(got) < 11:
                chunk = conn.recv(64)
                if not chunk:
                    break
                got += chunk
            print("CLIENT echo " + ("ok" if got == b"ping-kvm-1\n" else "mismatch"))
            conn.unwrap()
        else:
            conn.close()
except (ssl.SSLError, OSError) as error:
    print("CLIENT error " + type(error).__name__ + ": " + str(error))
EOF
key=00112233445566778899aabbccddeeff
wrong=ffeeddccbbaa99887766554433221100
id=locachy-kvm-v1
profile="PROFILE version=TLSv1.2 cipher=PSK-AES128-GCM-SHA256 alpn=locachy-kvm-v1 identity=locachy-kvm-v1 reused=0"
port=$((30000 + $$ % 20000))
tls_case() { # NAME COUNT EXPECT(refused|ok|echo|twice) TEXT CLIENT-ARGS...
    name=$1 count=$2 expect=$3 text=$4
    shift 4
    port=$((port + 1))
    log="$out/tls-$name.server"
    "$out/test-kvm-broker-tls" serve "$port" "$key" "$count" >"$log" 2>&1 &
    server=$!
    i=0
    while [ $i -lt 50 ] && ! grep -q READY "$log"; do
        sleep 0.1
        i=$((i + 1))
    done
    client=$(timeout 30 python3 -I "$out/tls-client.py" "$port" "$@" 2>&1 || :)
    wait "$server" || :
    ok=0
    case $expect in
    refused)
        grep -q '^HANDSHAKE refused' "$log" && ! grep -q '^HANDSHAKE ok' "$log" &&
            echo "$client" | grep -q '^CLIENT error' && ok=1 ;;
    ok)
        grep -qx "$profile" "$log" && echo "$client" | grep -q '^CLIENT handshake TLSv1.2 PSK-AES128-GCM-SHA256$' &&
            ok=1 ;;
    echo)
        grep -qx "$profile" "$log" && grep -qx 'PUMP bytes=11 end=orderly' "$log" &&
            echo "$client" | grep -qx 'CLIENT echo ok' && ok=1 ;;
    twice)
        [ "$(grep -cx "$profile" "$log")" -eq 2 ] && echo "$client" | grep -qx 'CLIENT second reused=False' && ok=1 ;;
    esac
    if [ $ok -eq 1 ]; then
        note PASS "tls: $text"
    else
        note FAIL "tls: $text -- server: $(tr '\n' ' ' <"$log") client: $(echo "$client" | tr '\n' ' ')"
    fi
}
tls_case tls13 1 refused "a TLS 1.3 peer is refused" "$key" $id $id - 1.3 hs
tls_case nonpsk 1 refused "a peer offering only a certificate suite is refused" \
    "$key" $id $id ECDHE-RSA-AES128-GCM-SHA256 1.2 hs
tls_case psk256 1 refused "a peer offering only another PSK suite is refused" "$key" $id $id PSK-AES256-GCM-SHA384 1.2 hs
tls_case apple4 1 ok "a peer offering four PSK suites gets PSK-AES128-GCM-SHA256" "$key" $id $id \
    PSK-AES256-GCM-SHA384:PSK-AES128-CBC-SHA256:PSK-AES256-CBC-SHA384:PSK-AES128-GCM-SHA256 1.2 hs
tls_case wrongid 1 refused "a wrong PSK identity is refused" "$key" other-id $id PSK-AES128-GCM-SHA256 1.2 hs
tls_case wrongkey 1 refused "a wrong key is refused" "$wrong" $id $id PSK-AES128-GCM-SHA256 1.2 hs
tls_case alpnwrong 1 refused "a wrong ALPN is refused" "$key" $id other-v1 PSK-AES128-GCM-SHA256 1.2 hs
tls_case alpnnone 1 refused "a peer without ALPN is refused" "$key" $id - PSK-AES128-GCM-SHA256 1.2 hs
tls_case good 1 echo "the cable profile pairs; plaintext flows both ways; orderly close_notify" \
    "$key" $id $id PSK-AES128-GCM-SHA256 1.2 echo
tls_case twice 2 twice "a second connection offering the first session is not resumed" \
    "$key" $id $id PSK-AES128-GCM-SHA256 1.2 twice

# lease: the broker itself, through its control socket (test-kvm-broker-ctl),
# inside a user and network namespace of the test's own. The cable is a veth
# pair: kvm0 is the leased end, kvmpeer the sender's end, which stays in the
# test's namespace. dum0 is a device of another driver. Fixtures are
# compile-time: the driver name, the home namespace path, the state directory,
# the attempt timeout, and the pump's sandbox probes.
if ! unshare --user --map-auto --map-root-user --net --mount true >/dev/null 2>&1; then
    echo "SKIP: lease: unprivileged user and network namespaces with --map-auto are not available"
else
    lw="$out/lease"
    mkdir -p "$lw"
    cat >"$lw/driver.py" <<'PYEOF'
# Scratch driver for the lease scenarios (not part of the repository).
# Runs inside `unshare --user --map-auto --map-root-user --net --mount`.
# argv: WORK (the directory holding home, nmcli, state, the broker and ctl)
import os
import queue
import subprocess
import sys
import threading
import time

W = sys.argv[1]
CTL = W + "/test-kvm-broker-ctl"
BROKER = W + "/kvm-broker-test"
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(("PASS" if ok else "FAIL") + ": " + name + ("" if ok or not detail else " -- " + detail))
    sys.stdout.flush()


def sh(*argv):
    """Run a host-side command in this (test) namespace; returns (rc, stdout)."""
    done = subprocess.run(list(argv), capture_output=True, text=True, timeout=30)
    return done.returncode, done.stdout + done.stderr


def pump_of(pid):
    rc, out = sh("pgrep", "-P", str(pid))
    return out.split()[0] if rc == 0 and out.split() else None


def in_session(pump, *argv):
    return sh("nsenter", "-t", pump, "-n", "--", *argv)


def reader(stream, q):
    for line in stream:
        q.put(line.rstrip("\n"))
    q.put(None)


class Broker:
    def __init__(self):
        self.p = subprocess.Popen([CTL, BROKER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.out, self.err = queue.Queue(), queue.Queue()
        self.stderr_lines = []
        threading.Thread(target=reader, args=(self.p.stdout, self.out), daemon=True).start()
        threading.Thread(target=reader, args=(self.p.stderr, self.err), daemon=True).start()
        self.pid = int(self.expect("pid ")[4:])

    def send(self, text):
        self.p.stdin.write("> " + text + "\n")
        self.p.stdin.flush()

    def plain(self, text):
        self.p.stdin.write("plain " + text + "\n")
        self.p.stdin.flush()

    def line(self, timeout=10):
        try:
            item = self.out.get(timeout=timeout)
        except queue.Empty:
            raise TimeoutError("no line")
        if item is None:
            raise EOFError("control stream closed")
        return item

    def expect(self, prefix, timeout=10):
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError("waiting for " + prefix)
            try:
                line = self.out.get(timeout=left)
            except queue.Empty:
                raise TimeoutError("waiting for " + prefix)
            if line is None:
                raise EOFError("closed while waiting for " + prefix)
            if line.startswith(prefix):
                return line

    def collect(self, end, timeout=10):
        lines = []
        while True:
            line = self.expect("", timeout)
            lines.append(line)
            if line.startswith(end):
                return lines

    def stderr_drain(self):
        while True:
            try:
                item = self.err.get_nowait()
            except queue.Empty:
                return
            if item is not None:
                self.stderr_lines.append(item)

    def wait_exit(self, timeout=10):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.p.poll() is not None:
                return self.p.returncode
            time.sleep(0.05)
        return None


def home_has(name):
    return sh("ip", "-o", "link", "show", "dev", name)[0] == 0


def nm_state(name):
    try:
        return open(W + "/nm/" + name).read().strip()
    except OSError:
        return "absent"


def scenario_lease():
    b = Broker()
    b.send("scan")
    listing = b.collect("< end")
    names = [x.split()[2] for x in listing if x.startswith("< iface ")]
    check("scan lists the fixture devices only", names == ["kvm0", "kvmpeer"], str(listing))
    b.send("pair dum0")
    reply = b.expect("< ")
    check("non-fixture driver refused", reply == "< error not a Thunderbolt/USB4 network interface", reply)
    check("refused pair leaves the device alone", home_has("dum0"))
    b.send("pair kvm0")
    lease = b.expect("< lease ")
    code = b.expect("< code ")
    parts = lease.split()
    check("lease line names device, link-local and port",
          len(parts) == 5 and parts[2] == "kvm0" and parts[3].startswith("fe80:") and parts[4] == "27183", lease)
    check("code has 26 symbols in groups", len(code[7:].replace("-", "")) == 26 and code.count("-") == 6, code)
    check("device left the home namespace", not home_has("kvm0"))
    pump = pump_of(b.pid)
    check("pump process found", pump is not None)
    if pump:
        rc, addrs = in_session(pump, "ip", "-o", "addr", "show", "dev", "kvm0")
        check("session namespace: only link-local addresses", rc == 0 and addrs and
              all("inet6 fe80:" in x for x in addrs.splitlines()), addrs)
        rc, routes = in_session(pump, "ip", "-6", "route", "show", "table", "all")
        rc4, routes4 = in_session(pump, "ip", "-4", "route", "show", "table", "all")
        lines = [x for x in (routes + routes4).splitlines() if x.strip()]
        kernel = [x for x in lines if x.startswith(("local ", "multicast ", "fe80::/64 "))]
        check("session namespace: no default route, only kernel link-local routes",
              rc == 0 and rc4 == 0 and len(kernel) == len(lines) and
              not any(x.startswith(("default", "::/0")) for x in lines), routes + routes4)
        rc, ra = in_session(pump, "cat", "/proc/sys/net/ipv6/conf/kvm0/accept_ra")
        check("session namespace: accept_ra is 0", rc == 0 and ra.strip() == "0", ra)
        rc, lo = in_session(pump, "ip", "-o", "link", "show")
        check("session namespace: no other devices", "kvmpeer" not in lo and "dum0" not in lo, lo)
    b.send("cancel")
    check("cancel answers ended cancelled", b.expect("< ended ") == "< ended cancelled")
    check("device returned to the home namespace", home_has("kvm0"))
    b.send("release")
    check("release answers ended released", b.expect("< ended ") == "< ended released")
    check("broker exits after release", b.expect("exit ") == "exit 0")
    time.sleep(0.3)
    b.stderr_drain()
    probe = [x for x in b.stderr_lines if "pump probe" in x]
    want = ["socket=denied", "open-netns=denied", "open-file=denied", "fork=denied", "setuid0=denied",
            "unshare=denied", "caps=none", "uid=65534"]
    check("pump probes: no socket, netns, file, fork, root or unshare; no capabilities; uid nobody",
          len(probe) == 1 and all(w in probe[0] for w in want), " | ".join(probe))
    b.p.stdin.close()
    try:
        b.p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        b.p.kill()
    print("   broker log: " + " | ".join(b.stderr_lines[-6:]))


ALPH = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"


def decode_code(code):
    """Independent decoder for the displayed code (26 symbols, '-' ignored)."""
    bits, n, out = 0, 0, bytearray()
    for ch in code.replace("-", ""):
        bits = (bits << 5) | ALPH.index(ch)
        n += 5
        if n >= 8:
            n -= 8
            out.append((bits >> n) & 0xFF)
            bits &= (1 << n) - 1
    return bytes(out)


def client(pump, addr, port, key_hex, identity="locachy-kvm-v1", mode="pair"):
    # The sender's end of the cable stays in this namespace; pump is unused.
    return subprocess.Popen(["python3", "-I", W + "/ll-client.py", addr, "kvmpeer", port, key_hex, identity,
                             "locachy-kvm-v1", mode],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def lease_and_code(b):
    b.send("pair kvm0")
    lease = b.expect("< lease ").split()
    code = b.expect("< code ")[7:]
    return lease[3], lease[4], code


def scenario_pair():
    b = Broker()
    addr, port, code = lease_and_code(b)
    pump = pump_of(b.pid)
    c = client(pump, addr, port, decode_code(code).hex())
    paired = b.expect("< paired")
    check("paired arrives with one descriptor", paired == "< paired +fd", paired)
    check("plaintext from the peer arrives", b.expect("plain< ") == "plain< from-mac")
    b.plain("to-mac")
    out, _ = c.communicate(timeout=20)
    check("peer completes the cable profile handshake",
          "CLIENT handshake TLSv1.2 PSK-AES128-GCM-SHA256 alpn=locachy-kvm-v1" in out, out)
    check("plaintext from the device reaches the peer", "CLIENT received" in out and "to-mac" in out, out)
    check("peer close ends the attempt: ended error", b.expect("< ended ") == "< ended error")
    check("device returned to the home namespace", home_has("kvm0"))
    b.send("release")
    check("release answers ended released", b.expect("< ended ") == "< ended released")
    check("broker exits after release", b.expect("exit ") == "exit 0")
    b.p.stdin.close()


def finish(b):
    b.send("release")
    check("release answers ended released", b.expect("< ended ") == "< ended released")
    check("broker exits after release", b.expect("exit ") == "exit 0")
    b.p.stdin.close()


def scenario_refuse():
    b = Broker()
    addr, port, code = lease_and_code(b)
    wrong = bytes(x ^ 0x5A for x in decode_code(code))
    out, _ = client(None, addr, port, wrong.hex()).communicate(timeout=20)
    check("a wrong code fails the peer's handshake", "CLIENT error" in out, out)
    check("a failed handshake ends the attempt: ended handshake-failed",
          b.expect("< ended ") == "< ended handshake-failed")
    check("the device is back in the home namespace", home_has("kvm0"))
    _, _, code2 = lease_and_code(b)
    check("the next pairing shows a new code", code2 != code)
    b.send("cancel")
    check("cancel of an open attempt: ended cancelled", b.expect("< ended ") == "< ended cancelled")
    finish(b)


def scenario_cancel_paired():
    b = Broker()
    addr, port, code = lease_and_code(b)
    c = client(None, addr, port, decode_code(code).hex())
    check("paired before the cancel", b.expect("< paired") == "< paired +fd")
    b.expect("plain< ")
    b.send("cancel")
    check("cancel while paired: ended cancelled", b.expect("< ended ") == "< ended cancelled")
    out, _ = c.communicate(timeout=20)
    check("the peer sees the stream end", "CLIENT" in out, out)
    check("device returned after the cancel", home_has("kvm0"))
    finish(b)


def scenario_timeout():
    b = Broker()
    lease_and_code(b)
    check("no peer: the attempt times out", b.expect("< ended ", timeout=25) == "< ended timeout")
    check("device returned after the timeout", home_has("kvm0"))
    finish(b)


def scenario_carrier():
    b = Broker()
    addr, port, code = lease_and_code(b)
    pump = pump_of(b.pid)
    c = client(None, addr, port, decode_code(code).hex())
    check("paired before the carrier drops", b.expect("< paired") == "< paired +fd")
    b.expect("plain< ")
    rc, text = in_session(pump, "ip", "link", "set", "kvm0", "down")
    check("the link goes down inside the session namespace", rc == 0, text)
    check("link down ends the session: ended carrier-lost",
          b.expect("< ended ", timeout=15) == "< ended carrier-lost")
    check("device returned after carrier loss", home_has("kvm0"))
    c.communicate(timeout=20)
    finish(b)


def scenario_eof():
    b = Broker()
    lease_and_code(b)
    b.p.stdin.close()
    check("control EOF: the broker exits 0", b.expect("exit ", timeout=15) == "exit 0")
    check("control EOF: the device is back in the home namespace", home_has("kvm0"))


def scenario_protocol():
    b = Broker()
    b.send("cancel")
    b.send("scan")
    listing = b.collect("< end")
    check("cancel while idle gets no reply", listing[0].startswith("< iface "), str(listing))
    b.send("bogus")
    check("unknown command is refused", b.expect("< ") == "< error unknown command")
    lease_and_code(b)
    b.send("pair kvmpeer")
    check("second pair while leased: error busy", b.expect("< ") == "< error busy")
    b.send("scan")
    check("scan while leased: error busy", b.expect("< ") == "< error busy")
    b.send("x" * 600)
    check("an overlong line is refused", b.expect("< ") == "< error line too long")
    b.send("cancel")
    check("cancel: ended cancelled", b.expect("< ended ") == "< ended cancelled")
    finish(b)


def mac_of(name):
    rc, text = sh("ip", "-o", "link", "show", "dev", name)
    words = text.split()
    return words[words.index("link/ether") + 1] if rc == 0 and "link/ether" in words else None


def scenario_kill():
    b = Broker()
    addr, port, code = lease_and_code(b)
    pump = pump_of(b.pid)
    c = client(None, addr, port, decode_code(code).hex())
    check("paired before the kill", b.expect("< paired") == "< paired +fd")
    b.expect("plain< ")
    records = [f for f in os.listdir(W + "/state") if f.startswith("lease-") and not f.endswith(".new")]
    check("one lease record exists while leased", len(records) == 1, str(records))
    record = W + "/state/" + records[0]
    mac = [x.split()[1] for x in open(record).read().splitlines() if x.startswith("address ")][0]
    os.kill(b.pid, 9)
    check("the broker is killed", b.expect("signal ") == "signal 9")
    b.p.stdin.close()
    c.communicate(timeout=20)
    time.sleep(0.5)
    check("the pump dies with the broker", sh("kill", "-0", pump)[0] != 0)
    check("the leased device is gone with its namespace", not home_has("kvm0"))
    check("the record stays after an unclean end", os.path.exists(record))
    b2 = Broker()
    b2.send("scan")
    b2.collect("< end")
    time.sleep(0.3)
    b2.stderr_drain()
    check("a new broker reports the unfinished lease",
          any("did not end cleanly" in x and "not back in locachy-net yet" in x for x in b2.stderr_lines),
          " | ".join(b2.stderr_lines))
    rc, text = sh("ip", "link", "add", "kvm0", "address", mac, "type", "veth", "peer", "name", "kvmpeer")
    check("the device comes back with the same address", rc == 0, text)
    sh("ip", "link", "set", "kvm0", "up")
    sh("ip", "link", "set", "kvmpeer", "up")
    b2.send("scan")
    b2.collect("< end")
    time.sleep(0.3)
    b2.stderr_drain()
    check("the returned device settles the record",
          not os.path.exists(record) and any("back in locachy-net: settled" in x or "back in locachy-net" in x
                                             for x in b2.stderr_lines), " | ".join(b2.stderr_lines))
    addr2, port2, code2 = lease_and_code(b2)
    check("the device can be leased again", addr2.startswith("fe80:"), addr2)
    b2.send("cancel")
    check("cancel after the new lease", b2.expect("< ended ") == "< ended cancelled")
    finish(b2)


def scenario_rename():
    b = Broker()
    mac = mac_of("kvm0")
    addr, port, code = lease_and_code(b)
    c = client(None, addr, port, decode_code(code).hex())
    check("paired before the return", b.expect("< paired") == "< paired +fd")
    b.expect("plain< ")
    rc, text = sh("ip", "link", "add", "kvm0", "address", "02:00:00:00:00:99", "type", "dummy")
    check("another device takes the name in the home namespace", rc == 0, text)
    b.send("cancel")
    check("cancel returns the leased device: ended cancelled", b.expect("< ended ") == "< ended cancelled")
    c.communicate(timeout=20)
    check("the device that took the name is untouched", mac_of("kvm0") == "02:00:00:00:00:99", str(mac_of("kvm0")))
    lines = [x for x in sh("ip", "-o", "link")[1].splitlines() if mac in x]
    name = lines[0].split(":")[1].strip().split("@")[0] if len(lines) == 1 else ""
    check("the leased device came back under a free name", len(lines) == 1 and name not in ("", "kvm0"), str(lines))
    check("its record is gone", not [f for f in os.listdir(W + "/state") if f.startswith("lease-")])
    sh("ip", "link", "del", "kvm0")
    finish(b)


if __name__ == "__main__":
    names = sys.argv[2:] or ["lease"]
    for name in names:
        print("== " + name)
        globals()["scenario_" + name]()
    print("SUMMARY: %d PASS, %d FAIL" % (sum(results), len(results) - sum(results)))
PYEOF
    cat >"$lw/ll-client.py" <<'PYEOF'
# Scratch link-local TLS client for the lease scenarios (not part of the repository).
# Runs on the peer end of the veth cable. argv: ADDRESS IFNAME PORT KEY_HEX IDENTITY ALPN MODE
import socket
import ssl
import sys

addr, ifname, port, key_hex, identity, alpn, mode = sys.argv[1:8]
key = bytes.fromhex(key_hex)
scope = socket.if_nametoindex(ifname)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
ctx.maximum_version = ssl.TLSVersion.TLSv1_2
ctx.set_ciphers("PSK-AES128-GCM-SHA256")
ctx.set_psk_client_callback(lambda hint: (identity.encode(), key))
ctx.set_alpn_protocols([alpn])
try:
    raw = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    raw.settimeout(10)
    raw.connect((addr, int(port), 0, scope))
    conn = ctx.wrap_socket(raw)
    print("CLIENT handshake " + conn.version() + " " + conn.cipher()[0] + " alpn=" + str(conn.selected_alpn_protocol()))
    if mode == "pair":
        conn.sendall(b"from-mac")
        got = b""
        while len(got) < 6:
            chunk = conn.recv(64)
            if not chunk:
                break
            got += chunk
        print("CLIENT received " + repr(got))
        conn.unwrap()
    else:
        conn.recv(64)
except (ssl.SSLError, OSError) as error:
    print("CLIENT error " + type(error).__name__ + ": " + str(error))
PYEOF
    cat >"$lw/inner.sh" <<'SHEOF'
#!/bin/sh
# Inside the namespace: the home namespace file, the cable, another device,
# then the scenario driver.
set -eu
W=$1
shift
touch "$W/home"
mount --bind /proc/self/ns/net "$W/home"
ip link add kvm0 type veth peer name kvmpeer
ip link set kvm0 up
ip link set kvmpeer up
ip link add dum0 type dummy
ip link set dum0 up
rm -rf "$W/state"
exec python3 -I "$W/driver.py" "$W" "$@"
SHEOF
    cc $cflags $(pkg-config --cflags openssl) \
        -DKVM_BROKER_TEST_DRIVER=\"veth\" -DKVM_BROKER_TEST_HOME_NETNS=\"$lw/home\" \
        -DKVM_BROKER_TEST_STATE_DIR=\"$lw/state\" -DKVM_BROKER_TEST_ATTEMPT_SECONDS=8 \
        -DKVM_BROKER_TEST_PROBES -o "$lw/kvm-broker-test" $broker_src $(pkg-config --libs openssl)
    cp "$out/test-kvm-broker-ctl" "$lw/test-kvm-broker-ctl"
    scenarios="lease pair refuse cancel_paired timeout carrier eof protocol kill rename"
    unshare --user --map-auto --map-root-user --net --mount --propagation private \
        sh "$lw/inner.sh" "$lw" $scenarios >"$lw/driver.log" 2>&1 || :
    grep -E '^(== |PASS: |FAIL: |SUMMARY: )' "$lw/driver.log" || :
    if grep -q '^SUMMARY: ' "$lw/driver.log"; then
        lease_fails=$(grep -c '^FAIL: ' "$lw/driver.log" || :)
        fails=$((fails + lease_fails))
    else
        note FAIL "lease: the scenario driver did not finish: $(tail -n 8 "$lw/driver.log" | tr '\n' ' ')"
    fi
fi
echo "kvm-broker tests: $fails failed"
[ "$fails" -eq 0 ]
