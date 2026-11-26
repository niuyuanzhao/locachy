#!/usr/bin/env bash
# Rootless test for /etc/ssh/ssh_config.d/50-locachy-foot-term.conf:
# verifies OpenSSH Match exec + SetEnv TERM=xterm-256color behavior both via
# ssh -G and end-to-end against a real unprivileged sshd (no AcceptEnv).
set -euo pipefail

src="$(cd "$(dirname "$0")/.." && pwd)"
conf="$src/link/50-locachy-foot-term.conf"

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL: $1"; }

[ -f "$conf" ] || { echo "FAIL: missing $conf"; exit 1; }

tmp="$(mktemp -d)"
sshd_pid=""
cleanup() {
    [ -n "$sshd_pid" ] && kill "$sshd_pid" 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT

# 1. Config syntax + first-match precedence via ssh -G
# OpenSSH parses ~/.ssh/config as a separate file before /etc/ssh/ssh_config
# (whose top-level Include is unconditional). In a single -F test file, place
# Host * before Include so the preceding Host block does not make Include
# conditional.
cat >"$tmp/combined.conf" <<EOF
Host arch-with-foot
    HostName 127.0.0.1
    SetEnv TERM=foot

Host *
Include $conf
EOF

check_G() {
    local desc="$1" term="$2" host="$3" expect="$4"
    local got
    got="$(TERM="$term" ssh -G -F "$tmp/combined.conf" "$host" 2>/dev/null | awk 'tolower($1)=="setenv" {print $2; exit}')"
    if [ "$got" = "$expect" ]; then
        pass "$desc"
    else
        fail "$desc (expected '$expect', got '$got')"
    fi
}

check_G "ssh -G: TERM=foot falls back to TERM=xterm-256color" "foot" "example.com" "TERM=xterm-256color"
check_G "ssh -G: TERM=foot-direct falls back to TERM=xterm-256color" "foot-direct" "example.com" "TERM=xterm-256color"
check_G "ssh -G: TERM=foot-extra falls back to TERM=xterm-256color" "foot-extra" "example.com" "TERM=xterm-256color"
check_G "ssh -G: TERM=linux is untouched" "linux" "example.com" ""
check_G "ssh -G: TERM=xterm-256color is untouched" "xterm-256color" "example.com" ""
check_G "ssh -G: user Host override keeps TERM=foot" "foot" "arch-with-foot" "TERM=foot"

# 2. End-to-end pty-req negotiation against a real sshd with NO AcceptEnv
if command -v sshd >/dev/null 2>&1 && command -v ssh-keygen >/dev/null 2>&1; then
    e2e_log="$tmp/e2e.log"
    python3 - "$tmp" "$conf" >"$e2e_log" 2>&1 <<'PY' && e2e_rc=0 || e2e_rc=$?
import os, pty, socket, subprocess, sys, time

td, conf = sys.argv[1], sys.argv[2]
hk = os.path.join(td, "host_key")
uk = os.path.join(td, "user_key")
ak = os.path.join(td, "authorized_keys")
subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", hk], check=True)
subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", uk], check=True)
with open(ak, "w") as f:
    f.write(open(uk + ".pub").read())
os.chmod(ak, 0o600)

s = socket.socket()
s.bind(("127.0.0.1", 0))
port = s.getsockname()[1]
s.close()

sshd_cfg = os.path.join(td, "sshd_config")
with open(sshd_cfg, "w") as f:
    f.write(
        f"Port {port}\n"
        "ListenAddress 127.0.0.1\n"
        f"HostKey {hk}\n"
        f"AuthorizedKeysFile {ak}\n"
        f"PidFile {td}/sshd.pid\n"
        "UsePAM no\n"
        "PasswordAuthentication no\n"
        "PubkeyAuthentication yes\n"
        "StrictModes no\n"
    )

client_cfg = os.path.join(td, "client_config")
with open(client_cfg, "w") as f:
    f.write(
        "Host keepfoot\n"
        "    HostName 127.0.0.1\n"
        "    SetEnv TERM=foot\n\n"
        "Host *\n"
        f"Include {conf}\n"
    )

sshd = subprocess.Popen(["/usr/bin/sshd", "-D", "-f", sshd_cfg], stderr=subprocess.PIPE)
try:
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                break
        except OSError:
            time.sleep(0.05)

    failures = 0
    cases = [
        ("127.0.0.1", "foot", "xterm-256color"),
        ("127.0.0.1", "foot-direct", "xterm-256color"),
        ("127.0.0.1", "linux", "linux"),
        ("keepfoot", "foot", "foot"),
    ]
    for target, local_term, expected in cases:
        master, slave = pty.openpty()
        env = dict(os.environ, TERM=local_term)
        p = subprocess.Popen(
            [
                "ssh", "-tt", "-p", str(port), "-i", uk, "-F", client_cfg,
                "-o", "StrictHostKeyChecking=no",
                "-o", "UserKnownHostsFile=/dev/null",
                "-o", "BatchMode=yes",
                target,
                'sh -c \'printf "REMOTE_TERM=%s\\n" "$TERM"\'',
            ],
            stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
        )
        os.close(slave)
        out, err = p.communicate(timeout=5)
        os.close(master)
        got = ""
        for line in out.decode(errors="replace").splitlines():
            line = line.strip()
            if line.startswith("REMOTE_TERM="):
                got = line.split("=", 1)[1]
        if p.returncode == 0 and got == expected:
            print(f"PASS: real sshd pty-req target={target} local={local_term} -> {got}")
        else:
            print(f"FAIL: real sshd pty-req target={target} local={local_term} rc={p.returncode} expected={expected!r} got={got!r} err={err.decode(errors='replace').strip()!r}")
            failures += 1
    sys.exit(1 if failures else 0)
finally:
    sshd.terminate()
    try:
        sshd.wait(timeout=3)
    except subprocess.TimeoutExpired:
        sshd.kill()
PY
    n_pass=$(grep -c '^PASS: ' "$e2e_log" || true)
    n_fail=$(grep -c '^FAIL: ' "$e2e_log" || true)
    PASS=$((PASS + n_pass))
    FAIL=$((FAIL + n_fail))
    if [ "$e2e_rc" -ne 0 ] && [ "$n_fail" -eq 0 ]; then
        FAIL=$((FAIL + 1))
        echo "FAIL: real sshd harness exited $e2e_rc"
    fi
    cat "$e2e_log"
else
    fail "sshd/ssh-keygen not available to run end-to-end pty-req check"
fi

echo "SUMMARY: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
