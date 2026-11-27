#!/bin/sh
# The extended-display broker's tests (tools/EXTDISPLAY.md), Linux only:
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
# usage: test-extdisplay-broker.sh [WORK_DIRECTORY]
# Without a directory a temporary one is used and removed. Nothing is
# installed, and nothing outside the test's own namespaces is changed.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
if [ $# -ge 1 ]; then
    mkdir -p "$1"
    out=$(CDPATH= cd -- "$1" && pwd)
else
    out=$(mktemp -d "${TMPDIR:-/tmp}/locachy-extdisplay-broker.XXXXXX")
    trap 'rm -rf "$out"' EXIT
fi
if [ "$(uname -s)" != Linux ]; then
    echo "SKIP: extdisplay-broker needs Linux (rtnetlink, namespaces, Landlock, seccomp)"
    exit 77
fi
if ! pkg-config --exists openssl avahi-core; then
    echo "SKIP: OpenSSL and Avahi development files are not installed"
    exit 77
fi
cflags="-O2 -Wall -Wextra -Werror -D_GNU_SOURCE"
broker_src="$src/progs/extdisplay-broker.c $src/progs/extdisplay-broker-net.c $src/progs/extdisplay-broker-pump.c $src/progs/extdisplay-broker-tls.c $src/progs/extdisplay-discovery.c"
fails=0
note() { # PASS|FAIL text
    echo "$1: $2"
    [ "$1" != FAIL ] || fails=$((fails + 1))
}

# build
if cc $cflags $(pkg-config --cflags openssl) -o "$out/extdisplay-broker" $broker_src $(pkg-config --libs openssl) \
    2>"$out/build.log"; then
    note PASS "build: extdisplay-broker builds with the production flags"
else
    note FAIL "build: extdisplay-broker does not build: $(head -c 400 "$out/build.log" | tr '\n' ' ')"
fi
cc $cflags $(pkg-config --cflags openssl) -o "$out/test-extdisplay-broker-tls" "$src/tools/test-extdisplay-broker-tls.c" \
    "$src/progs/extdisplay-broker-tls.c" $(pkg-config --libs openssl)
cc $cflags $(pkg-config --cflags avahi-core) -o "$out/extdisplay-advertise-test" \
    "$src/progs/extdisplay-advertise.c" $(pkg-config --libs avahi-core)
cc $cflags -o "$out/test-extdisplay-broker-ctl" "$src/tools/test-extdisplay-broker-ctl.c"

# code
"$out/test-extdisplay-broker-tls" code >"$out/code.log" 2>&1 || :
grep -E '^(PASS|FAIL): ' "$out/code.log" || :
fails=$((fails + $(grep -c '^FAIL: ' "$out/code.log" || :)))
grep -q '^code: 0 failure' "$out/code.log" || note FAIL "code: the code cases did not all run"

# tls
if ! python3 -I -c 'import ssl, sys; sys.exit(0 if hasattr(ssl.SSLContext, "set_psk_client_callback") else 1)' \
    2>/dev/null; then
    echo "SKIP: tls and lease: python3 has no TLS PSK client support (Python 3.13 or newer is needed)"
    echo "extdisplay-broker tests: $fails failed"
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
            conn.sendall(b"ping-cable\n")
            got = b""
            while len(got) < 11:
                chunk = conn.recv(64)
                if not chunk:
                    break
                got += chunk
            print("CLIENT echo " + ("ok" if got == b"ping-cable\n" else "mismatch"))
            conn.unwrap()
        else:
            conn.close()
except (ssl.SSLError, OSError) as error:
    print("CLIENT error " + type(error).__name__ + ": " + str(error))
EOF
key=00112233445566778899aabbccddeeff
wrong=ffeeddccbbaa99887766554433221100
id=locachy-extdisplay-v1
profile="PROFILE version=TLSv1.2 cipher=PSK-AES128-GCM-SHA256 alpn=locachy-extdisplay-v1 identity=locachy-extdisplay-v1 reused=0"
port=$((30000 + $$ % 20000))
tls_case() { # NAME COUNT EXPECT(refused|ok|echo|twice) TEXT CLIENT-ARGS...
    name=$1 count=$2 expect=$3 text=$4
    shift 4
    port=$((port + 1))
    log="$out/tls-$name.server"
    "$out/test-extdisplay-broker-tls" serve "$port" "$key" "$count" >"$log" 2>&1 &
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

# lease: the broker itself, through its control socket (test-extdisplay-broker-ctl),
# inside a user and network namespace of the test's own. The cable is a veth
# pair: extdisplay0 is the leased end, extdisplaypeer the sender's end, which stays in the
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
CTL = W + "/test-extdisplay-broker-ctl"
BROKER = W + "/extdisplay-broker-test"
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
    """The discovery child is also a direct child; select the TLS pump by its
    /proc comm rather than assuming it was forked first."""
    rc, out = sh("pgrep", "-P", str(pid))
    if rc != 0:
        return None
    try:
        expected = open("/proc/" + str(pid) + "/comm").read().strip()
    except OSError:
        return None
    for child in out.split():
        try:
            comm = open("/proc/" + child + "/comm").read().strip()
        except OSError:
            continue
        if comm == expected:
            return child
    return None


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
    check("scan lists the fixture devices only", names == ["extdisplay0", "extdisplaypeer"], str(listing))
    b.send("pair dum0")
    reply = b.expect("< ")
    check("non-fixture driver refused", reply == "< error not a Thunderbolt/USB4 network interface", reply)
    check("refused pair leaves the device alone", home_has("dum0"))
    b.send("pair extdisplay0")
    lease = b.expect("< lease ")
    code = b.expect("< code ")
    parts = lease.split()
    check("lease line names device, link-local and port",
          len(parts) == 5 and parts[2] == "extdisplay0" and parts[3].startswith("fe80:") and parts[4] == "27183", lease)
    check("code has 26 symbols in groups", len(code[7:].replace("-", "")) == 26 and code.count("-") == 6, code)
    check("device left the home namespace", not home_has("extdisplay0"))
    pump = pump_of(b.pid)
    check("pump process found", pump is not None)
    if pump:
        rc, addrs = in_session(pump, "ip", "-o", "addr", "show", "dev", "extdisplay0")
        check("session namespace: only link-local addresses", rc == 0 and addrs and
              all("inet6 fe80:" in x for x in addrs.splitlines()), addrs)
        rc, routes = in_session(pump, "ip", "-6", "route", "show", "table", "all")
        rc4, routes4 = in_session(pump, "ip", "-4", "route", "show", "table", "all")
        lines = [x for x in (routes + routes4).splitlines() if x.strip()]
        kernel = [x for x in lines if x.startswith(("local ", "multicast ", "fe80::/64 "))]
        check("session namespace: no default route, only kernel link-local routes",
              rc == 0 and rc4 == 0 and len(kernel) == len(lines) and
              not any(x.startswith(("default", "::/0")) for x in lines), routes + routes4)
        rc, ra = in_session(pump, "cat", "/proc/sys/net/ipv6/conf/extdisplay0/accept_ra")
        check("session namespace: accept_ra is 0", rc == 0 and ra.strip() == "0", ra)
        rc, lo = in_session(pump, "ip", "-o", "link", "show")
        check("session namespace: no other devices", "extdisplaypeer" not in lo and "dum0" not in lo, lo)
    b.send("cancel")
    check("cancel answers ended cancelled", b.expect("< ended ") == "< ended cancelled")
    check("device returned to the home namespace", home_has("extdisplay0"))
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


def client(pump, addr, port, key_hex, identity="locachy-extdisplay-v1", mode="pair"):
    # The sender's end of the cable stays in this namespace; pump is unused.
    return subprocess.Popen(["python3", "-I", W + "/ll-client.py", addr, "extdisplaypeer", port, key_hex, identity,
                             "locachy-extdisplay-v1", mode],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def lease_and_code(b):
    b.send("pair extdisplay0")
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
          "CLIENT handshake TLSv1.2 PSK-AES128-GCM-SHA256 alpn=locachy-extdisplay-v1" in out, out)
    check("plaintext from the device reaches the peer", "CLIENT received" in out and "to-mac" in out, out)
    check("peer close ends the attempt: ended error", b.expect("< ended ") == "< ended error")
    check("device returned to the home namespace", home_has("extdisplay0"))
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
    check("the device is back in the home namespace", home_has("extdisplay0"))
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
    check("device returned after the cancel", home_has("extdisplay0"))
    finish(b)


def scenario_timeout():
    b = Broker()
    lease_and_code(b)
    check("no peer: the attempt times out", b.expect("< ended ", timeout=25) == "< ended timeout")
    check("device returned after the timeout", home_has("extdisplay0"))
    finish(b)


def scenario_carrier():
    b = Broker()
    addr, port, code = lease_and_code(b)
    pump = pump_of(b.pid)
    c = client(None, addr, port, decode_code(code).hex())
    check("paired before the carrier drops", b.expect("< paired") == "< paired +fd")
    b.expect("plain< ")
    rc, text = in_session(pump, "ip", "link", "set", "extdisplay0", "down")
    check("the link goes down inside the session namespace", rc == 0, text)
    check("link down ends the session: ended carrier-lost",
          b.expect("< ended ", timeout=15) == "< ended carrier-lost")
    check("device returned after carrier loss", home_has("extdisplay0"))
    c.communicate(timeout=20)
    finish(b)


def scenario_eof():
    b = Broker()
    lease_and_code(b)
    b.p.stdin.close()
    check("control EOF: the broker exits 0", b.expect("exit ", timeout=15) == "exit 0")
    check("control EOF: the device is back in the home namespace", home_has("extdisplay0"))


def scenario_protocol():
    b = Broker()
    b.send("cancel")
    b.send("scan")
    listing = b.collect("< end")
    check("cancel while idle gets no reply", listing[0].startswith("< iface "), str(listing))
    b.send("bogus")
    check("unknown command is refused", b.expect("< ") == "< error unknown command")
    lease_and_code(b)
    b.send("pair extdisplaypeer")
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
    check("the leased device is gone with its namespace", not home_has("extdisplay0"))
    check("the record stays after an unclean end", os.path.exists(record))
    b2 = Broker()
    b2.send("scan")
    b2.collect("< end")
    time.sleep(0.3)
    b2.stderr_drain()
    check("a new broker reports the unfinished lease",
          any("did not end cleanly" in x and "not back in locachy-net yet" in x for x in b2.stderr_lines),
          " | ".join(b2.stderr_lines))
    rc, text = sh("ip", "link", "add", "extdisplay0", "address", mac, "type", "veth", "peer", "name", "extdisplaypeer")
    check("the device comes back with the same address", rc == 0, text)
    sh("ip", "link", "set", "extdisplay0", "up")
    sh("ip", "link", "set", "extdisplaypeer", "up")
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


def scenario_watch():
    # The carrier-loss case deliberately returned an administratively down link.
    sh("ip", "link", "set", "dev", "extdisplay0", "up")
    sh("ip", "link", "set", "dev", "extdisplaypeer", "up")
    b = Broker()
    b.send("watch")
    listing = b.collect("< end")
    lines = [x.split() for x in listing if x.startswith("< iface ")]
    check("watch snapshots include kernel index and hardware identity", len(lines) == 2 and
          all(len(x) == 7 and int(x[5]) > 0 and ":" in x[6] for x in lines), str(listing))
    b.send("pair extdisplay0 2147483647 02:00:00:00:00:ff")
    check("stale identity cannot lease a same-name interface", b.expect("< error ").startswith("< error the selected interface changed"))
    check("stale identity leaves the interface at home", home_has("extdisplay0"))
    sh("ip", "link", "set", "extdisplaypeer", "down")
    check("carrier change is reported without manual scan", b.expect("< changed") == "< changed")
    b.send("scan")
    listing = b.collect("< end")
    fields = next(x.split() for x in listing if x.startswith("< iface extdisplay0 "))
    check("snapshot sees the carrier change", fields[4] == "down", str(fields))
    b.send("pair " + fields[2] + " " + fields[5] + " " + fields[6])
    progress = b.expect("< preparing ")
    check("carrier wait is explicit", progress == "< preparing extdisplay0 carrier", progress)
    started = time.monotonic()
    b.send("cancel")
    check("carrier preparation is cancellable without the old ten-second wait", b.expect("< ended ") == "< ended cancelled" and
          time.monotonic() - started < 2)
    check("cancel returns the waiting interface", home_has("extdisplay0"))
    try:
        extra = b.line(timeout=0.3)
    except TimeoutError:
        extra = None
    check("own lease return does not trigger a connection notification", extra is None, str(extra))
    # Moving a veth back resets administrative state; a real returned device
    # is brought up by NetworkManager. Model that policy before the new cable.
    sh("ip", "link", "set", "dev", "extdisplay0", "up")
    sh("ip", "link", "set", "dev", "extdisplaypeer", "up")
    check("new physical-link event is noticed after cancel", b.expect("< changed") == "< changed")
    finish(b)


def scenario_rename():
    b = Broker()
    mac = mac_of("extdisplay0")
    addr, port, code = lease_and_code(b)
    c = client(None, addr, port, decode_code(code).hex())
    check("paired before the return", b.expect("< paired") == "< paired +fd")
    b.expect("plain< ")
    rc, text = sh("ip", "link", "add", "extdisplay0", "address", "02:00:00:00:00:99", "type", "dummy")
    check("another device takes the name in the home namespace", rc == 0, text)
    b.send("cancel")
    check("cancel returns the leased device: ended cancelled", b.expect("< ended ") == "< ended cancelled")
    c.communicate(timeout=20)
    check("the device that took the name is untouched", mac_of("extdisplay0") == "02:00:00:00:00:99", str(mac_of("extdisplay0")))
    lines = [x for x in sh("ip", "-o", "link")[1].splitlines() if mac in x]
    name = lines[0].split(":")[1].strip().split("@")[0] if len(lines) == 1 else ""
    check("the leased device came back under a free name", len(lines) == 1 and name not in ("", "extdisplay0"), str(lines))
    check("its record is gone", not [f for f in os.listdir(W + "/state") if f.startswith("lease-")])
    sh("ip", "link", "del", "extdisplay0")
    if name:
        sh("ip", "link", "set", "dev", name, "down")
        sh("ip", "link", "set", "dev", name, "name", "extdisplay0")
        sh("ip", "link", "set", "dev", "extdisplay0", "up")
    finish(b)


def scenario_hotplug():
    # Cable unplugged while leased (interface destroyed inside session netns),
    # then replugged with the same MAC at a different sysfs device path and
    # interface name/ifindex. Must settle any record, announce "changed", and
    # pair again on the same broker without restarting or rebooting.
    sh("ip", "link", "set", "dev", "extdisplay0", "up")
    sh("ip", "link", "set", "dev", "extdisplaypeer", "up")
    mac = mac_of("extdisplay0")
    b = Broker()
    b.send("watch")
    listing = b.collect("< end")
    fields = next(x.split() for x in listing if x.startswith("< iface extdisplay0 "))
    b.send("pair " + fields[2] + " " + fields[5] + " " + fields[6])
    lease = b.expect("< lease ").split()
    code = b.expect("< code ")[7:]
    pump = pump_of(b.pid)
    c = client(None, lease[3], lease[4], decode_code(code).hex())
    check("hotplug: paired before cable pull", b.expect("< paired") == "< paired +fd")
    b.expect("plain< ")
    records = [f for f in os.listdir(W + "/state") if f.startswith("lease-") and not f.endswith(".new")]
    check("hotplug: lease record written", len(records) == 1, str(records))
    record = W + "/state/" + records[0]
    # Simulate a physical USB4/Thunderbolt device with a different sysfs device
    # path before replug (e.g. a different port or dock).
    if os.path.exists(record):
        with open(record, "r+") as rf:
            body = rf.read().replace("device -", "device /sys/devices/pci0000:00/0000:00:0d.2/domain0/0-0/0-1")
            rf.seek(0)
            rf.write(body)
            rf.truncate()
    # Cable pull destroys the thunderbolt-net interface inside the session netns.
    rc, text = in_session(pump, "ip", "link", "del", "extdisplay0")
    check("hotplug: leased device destroyed on cable pull", rc == 0, text)
    check("hotplug: session ends with carrier-lost", b.expect("< ended ", timeout=15) == "< ended carrier-lost")
    c.communicate(timeout=20)
    b.send("scan")
    empty = b.collect("< end")
    check("hotplug: no extdisplay0 after unplug",
          not any(x.startswith("< iface extdisplay0 ") for x in empty), str(empty))
    # Replug: same MAC returns under a new interface name/ifindex and different
    # sysfs device path ("-" vs the earlier PCI path in the record if any stayed).
    rc, text = sh("ip", "link", "add", "extdisplay1", "address", mac, "type", "veth", "peer", "name", "extdisplaypeer")
    check("hotplug: device returns with same MAC and new name", rc == 0, text)
    sh("ip", "link", "set", "extdisplay1", "up")
    sh("ip", "link", "set", "extdisplaypeer", "up")
    try:
        ev = b.expect("< changed", timeout=5)
    except TimeoutError:
        ev = "timeout"
    check("hotplug: broker announces changed on replug", ev == "< changed", ev)
    b.send("scan")
    listing2 = b.collect("< end")
    f2 = next((x.split() for x in listing2 if x.startswith("< iface extdisplay1 ")), None)
    check("hotplug: returning interface visible with carrier up",
          f2 is not None and f2[4] == "up" and f2[6] == mac, str(listing2))
    check("hotplug: no stale lease record blocks replug", not os.path.exists(record))
    # Re-pair on the same broker without restarting it.
    def wait_lease_or_error(br, timeout=10):
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                return "timeout"
            line = br.expect("< ", timeout=left)
            if line.startswith("< preparing "):
                continue
            return line

    if f2 is not None:
        b.send("pair " + f2[2] + " " + f2[5] + " " + f2[6])
        reply2 = wait_lease_or_error(b, timeout=10)
        if reply2.startswith("< lease "):
            lease2 = reply2.split()
            code2 = b.expect("< code ", timeout=10)[7:]
            pair_ok = len(lease2) == 5 and lease2[2] == "extdisplay1" and lease2[3].startswith("fe80:")
        else:
            lease2, code2, pair_ok = [reply2], "", False
        check("hotplug: second lease succeeds without broker restart", pair_ok, str(lease2))
        if pair_ok:
            c2 = client(None, lease2[3], lease2[4], decode_code(code2).hex())
            check("hotplug: second pair succeeds after replug", b.expect("< paired", timeout=15) == "< paired +fd")
            b.expect("plain< ")
            b.plain("to-mac")
            out2, _ = c2.communicate(timeout=20)
            check("hotplug: plaintext flows after replug", "CLIENT received" in out2 and "to-mac" in out2, out2)
            check("hotplug: second session closes cleanly", b.expect("< ended ", timeout=15) == "< ended error")
    # Also verify kill-recovery when the device returns at a different sysfs path.
    if home_has("extdisplay1"):
        sh("ip", "link", "set", "extdisplay1", "up")
        sh("ip", "link", "set", "extdisplaypeer", "up")
    b.send("scan")
    listing3 = b.collect("< end")
    f3 = next((x.split() for x in listing3 if x.startswith("< iface extdisplay1 ")), None)
    if f3 is not None and not os.path.exists(record):
        b.send("pair " + f3[2] + " " + f3[5] + " " + f3[6])
        b.expect("< lease ")
        b.expect("< code ")
        records = [f for f in os.listdir(W + "/state") if f.startswith("lease-") and not f.endswith(".new")]
        record = W + "/state/" + records[0] if records else ""
        if record and os.path.exists(record):
            with open(record, "r+") as rf:
                body = rf.read().replace("device -", "device /sys/devices/pci0000:00/0000:00:0d.3/domain1/1-0/1-1")
                rf.seek(0)
                rf.write(body)
                rf.truncate()
        os.kill(b.pid, 9)
        b.expect("signal ")
        b.p.stdin.close()
        time.sleep(0.5)
        check("hotplug: record stays after unclean broker kill", bool(record) and os.path.exists(record))
        b3 = Broker()
        b3.send("watch")
        b3.collect("< end")
        check("hotplug: record stays while device is still absent", os.path.exists(record))
        rc, text = sh("ip", "link", "add", "extdisplay0", "address", mac, "type", "veth", "peer", "name", "extdisplaypeer")
        check("hotplug: device returns at a different sysfs path after kill", rc == 0, text)
        sh("ip", "link", "set", "extdisplay0", "up")
        sh("ip", "link", "set", "extdisplaypeer", "up")
        try:
            ev3 = b3.expect("< changed", timeout=5)
        except TimeoutError:
            ev3 = "timeout"
        check("hotplug: broker announces returning device after kill", ev3 == "< changed", ev3)
        # Note: returning link event settles stale record even before scan.
        time.sleep(0.1)
        check("hotplug: stale record settled on return before rescan", not os.path.exists(record))
        b3.send("scan")
        l4 = b3.collect("< end")
        f4 = next((x.split() for x in l4 if x.startswith("< iface extdisplay0 ")), None)
        if f4 is not None:
            b3.send("pair " + f4[2] + " " + f4[5] + " " + f4[6])
            reply4 = wait_lease_or_error(b3, timeout=10)
            l_ok = reply4.startswith("< lease extdisplay0 ")
            if l_ok:
                b3.expect("< code ", timeout=10)
            check("hotplug: pair succeeds after different-path kill recovery", l_ok, reply4)
            if l_ok:
                b3.send("cancel")
                b3.expect("< ended ")
        finish(b3)
    else:
        finish(b)


def scenario_carrier_kick():
    # Stalled carrier (peer down, link administratively UP without IFF_LOWER_UP):
    # the broker must bounce the leased link DOWN -> UP to re-trigger tbnet_open,
    # then progress automatically once carrier comes up.
    sh("ip", "link", "set", "dev", "extdisplay0", "up")
    sh("ip", "link", "set", "dev", "extdisplaypeer", "down")
    b = Broker()
    b.send("watch")
    listing = b.collect("< end")
    fields = next(x.split() for x in listing if x.startswith("< iface extdisplay0 "))
    check("carrier_kick: starts with carrier down", fields[4] == "down", str(fields))
    b.send("pair " + fields[2] + " " + fields[5] + " " + fields[6])
    check("carrier_kick: enters carrier wait", b.expect("< preparing ") == "< preparing extdisplay0 carrier")
    # Three bounces at 600 ms spacing (+150 ms each), then none while the peer stays silent.
    time.sleep(3.0)
    b.stderr_drain()
    bounces = [x for x in b.stderr_lines if "bouncing extdisplay0 to restart ThunderboltIP login" in x]
    check("carrier_kick: broker bounced the stalled link to restart ThunderboltIP login",
          any("(attempt 1)" in x for x in bounces), " | ".join(b.stderr_lines))
    check("carrier_kick: bounces stop after three per lease",
          len(bounces) == 3 and not any("(attempt 4)" in x for x in bounces), " | ".join(bounces))
    # Bring peer up; the next carrier event must advance cleanly to lease + code.
    sh("ip", "link", "set", "dev", "extdisplaypeer", "up")
    got_lease = False
    deadline = time.time() + 10
    while time.time() < deadline:
        line = b.expect("< ", timeout=max(0.5, deadline - time.time()))
        if line.startswith("< lease extdisplay0 "):
            got_lease = True
            break
    check("carrier_kick: recovers to lease once carrier returns", got_lease)
    if got_lease:
        code = b.expect("< code ", timeout=10)
        check("carrier_kick: produces pairing code after recovery", code.startswith("< code "), code)
    b.send("cancel")
    check("carrier_kick: cancel ends cleanly", b.expect("< ended ") == "< ended cancelled")
    check("carrier_kick: device returns to home namespace", home_has("extdisplay0"))
    sh("ip", "link", "set", "dev", "extdisplay0", "up")
    sh("ip", "link", "set", "dev", "extdisplaypeer", "up")
    finish(b)


def scenario_tb_pm():
    import shutil
    import signal
    pci_dir = os.path.join(W, "tb-pci")
    bus_dir = os.path.join(W, "tb-bus")
    dev_dir = os.path.join(W, "tb-dev")
    for d in (pci_dir, bus_dir, dev_dir):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d, exist_ok=True)

    def mkctrl(base, rel, val):
        p = os.path.join(base, rel, "power")
        os.makedirs(p, exist_ok=True)
        f = os.path.join(p, "control")
        with open(f, "w") as fp:
            fp.write(val + "\n")
        return f

    def rd(path):
        return open(path).read().strip()

    # PCI NHI devices (real sysfs layout: symlinks into devices/)
    nhi_auto = mkctrl(dev_dir, "0000:07:00.0", "auto")
    open(os.path.join(dev_dir, "0000:07:00.0", "config"), "wb").write(b"\x86\x80\xd2\x15")
    os.symlink(os.path.join(dev_dir, "0000:07:00.0"), os.path.join(pci_dir, "0000:07:00.0"))

    nhi_on = mkctrl(dev_dir, "0000:08:00.0", "on")
    open(os.path.join(dev_dir, "0000:08:00.0", "config"), "wb").write(b"\x86\x80\xd2\x15")
    os.symlink(os.path.join(dev_dir, "0000:08:00.0"), os.path.join(pci_dir, "0000:08:00.0"))

    # Driver-dir non-device entries must be ignored.
    open(os.path.join(pci_dir, "bind"), "w").write("")
    mod_ctrl = mkctrl(pci_dir, "module", "auto")

    # Thunderbolt bus devices: host router 0-0 held; downstream 0-1 and domain0 untouched.
    host_ctrl = mkctrl(dev_dir, "0-0", "auto")
    os.symlink(os.path.join(dev_dir, "0-0"), os.path.join(bus_dir, "0-0"))
    peer_ctrl = mkctrl(dev_dir, "0-1", "auto")
    os.symlink(os.path.join(dev_dir, "0-1"), os.path.join(bus_dir, "0-1"))
    dom_ctrl = mkctrl(dev_dir, "domain0", "auto")
    os.symlink(os.path.join(dev_dir, "domain0"), os.path.join(bus_dir, "domain0"))

    try:
        b = Broker()
        # Synchronize past startup (tb_pm_hold + enter_home).
        b.send("scan")
        b.collect("< end")
        check("tb_pm: PCI NHI in auto is held on while broker runs", rd(nhi_auto) == "on", rd(nhi_auto))
        check("tb_pm: host router 0-0 in auto is held on while broker runs", rd(host_ctrl) == "on", rd(host_ctrl))
        check("tb_pm: PCI NHI already on stays on", rd(nhi_on) == "on", rd(nhi_on))
        check("tb_pm: driver non-device entry (module) untouched", rd(mod_ctrl) == "auto", rd(mod_ctrl))
        check("tb_pm: downstream peer router 0-1 untouched", rd(peer_ctrl) == "auto", rd(peer_ctrl))
        check("tb_pm: domain0 entry untouched", rd(dom_ctrl) == "auto", rd(dom_ctrl))

        # Forking and cancelling a pump/discovery attempt must not release the hold early.
        b.send("pair extdisplay0")
        b.expect("< lease extdisplay0 ")
        b.expect("< code ")
        b.send("cancel")
        b.expect("< ended ")
        check("tb_pm: hold survives pump/discovery child exit",
              rd(nhi_auto) == "on" and rd(host_ctrl) == "on",
              f"nhi={rd(nhi_auto)} host={rd(host_ctrl)}")

        # Clean release restores auto for held devices and preserves user-configured on.
        finish(b)
        b.wait_exit(timeout=10)
        check("tb_pm: release restores PCI NHI to auto", rd(nhi_auto) == "auto", rd(nhi_auto))
        check("tb_pm: release restores host router 0-0 to auto", rd(host_ctrl) == "auto", rd(host_ctrl))
        check("tb_pm: release preserves pre-existing on", rd(nhi_on) == "on", rd(nhi_on))

        # Signal termination (SIGTERM) also restores auto cleanly.
        b2 = Broker()
        b2.send("scan")
        b2.collect("< end")
        check("tb_pm: second run holds on again",
              rd(nhi_auto) == "on" and rd(host_ctrl) == "on",
              f"nhi={rd(nhi_auto)} host={rd(host_ctrl)}")
        os.kill(b2.pid, signal.SIGTERM)
        ex = b2.expect("exit ", timeout=10)
        b2.p.stdin.close()
        b2.wait_exit(timeout=10)
        check("tb_pm: SIGTERM exits cleanly", ex == "exit 0", ex)
        check("tb_pm: SIGTERM restores PCI NHI to auto", rd(nhi_auto) == "auto", rd(nhi_auto))
        check("tb_pm: SIGTERM restores host router 0-0 to auto", rd(host_ctrl) == "auto", rd(host_ctrl))
        check("tb_pm: SIGTERM preserves pre-existing on", rd(nhi_on) == "on", rd(nhi_on))
    finally:
        for d in (pci_dir, bus_dir, dev_dir):
            shutil.rmtree(d, ignore_errors=True)


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
ip link add extdisplay0 type veth peer name extdisplaypeer
ip link set extdisplay0 up
ip link set extdisplaypeer up
ip link add dum0 type dummy
ip link set dum0 up
rm -rf "$W/state"
exec python3 -I "$W/driver.py" "$W" "$@"
SHEOF
    cp "$out/extdisplay-advertise-test" "$lw/extdisplay-advertise-test"
    cc $cflags $(pkg-config --cflags openssl) \
        -DEXTDISPLAY_DISCOVERY_PATH=\"$lw/extdisplay-advertise-test\" \
        -DEXTDISPLAY_BROKER_TEST_DRIVER=\"veth\" -DEXTDISPLAY_BROKER_TEST_HOME_NETNS=\"$lw/home\" \
        -DEXTDISPLAY_BROKER_TEST_STATE_DIR=\"$lw/state\" -DEXTDISPLAY_BROKER_TEST_ATTEMPT_SECONDS=8 \
        -DEXTDISPLAY_BROKER_TEST_CARRIER_KICK_MS=600 -DEXTDISPLAY_BROKER_TEST_CARRIER_BOUNCE_MS=150 \
        -DEXTDISPLAY_BROKER_TEST_TB_PCI_DIR=\"$lw/tb-pci\" -DEXTDISPLAY_BROKER_TEST_TB_BUS_DIR=\"$lw/tb-bus\" \
        -DEXTDISPLAY_BROKER_TEST_PROBES -o "$lw/extdisplay-broker-test" $broker_src $(pkg-config --libs openssl)
    cp "$out/test-extdisplay-broker-ctl" "$lw/test-extdisplay-broker-ctl"
    scenarios="lease pair refuse cancel_paired timeout carrier eof protocol watch kill rename hotplug carrier_kick tb_pm"
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
echo "extdisplay-broker tests: $fails failed"
[ "$fails" -eq 0 ]
