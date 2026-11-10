#!/bin/sh
# Loopback interoperability for the frozen cable profile: TLS 1.2 only,
# TLS_PSK_WITH_AES_128_GCM_SHA256, PSK identity and ALPN "locachy-kvm-v1",
# 16-byte per-connection PSK typed as base32. The Swift client uses production
# tlsOptions() and PairingCode.read(). The listener (tls-listener.py, Python's
# OpenSSL binding) enforces the expected identity and key in its PSK callback
# and records what it negotiated. Codes, logs and binaries live only in
# temporary/output directories. This is protocol evidence, not a cable run.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-/tmp/locachy-macos-companion-build}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/locachy-tls.XXXXXX")
SERVER=
cleanup() {
    [ -z "$SERVER" ] || kill "$SERVER" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

mkdir -p "$OUT/tests"
SDK=$(xcrun --sdk macosx --show-sdk-path)
xcrun swiftc -swift-version 5 -parse-as-library -target "$(uname -m)-apple-macos13.0" -sdk "$SDK" \
    -framework Network -framework Security \
    "$HERE/sources/Wire.swift" "$HERE/sources/PairingCode.swift" "$HERE/sources/SecureTransport.swift" \
    "$HERE/tests/TLSLoopback.swift" -o "$OUT/tests/tls-loopback"

# One fresh 16-byte secret: "<base32 code grouped 5-5-4-4-4-4> <hex>".
new_secret() {
    python3 -I -c '
import base64, os
raw = os.urandom(16)
code = base64.b32encode(raw).decode().rstrip("=")
groups, at = [], 0
for size in (5, 5, 4, 4, 4, 4):
    groups.append(code[at:at + size]); at += size
print("-".join(groups), raw.hex())'
}
set -- $(new_secret); CODE=$1; HEX=$2
set -- $(new_secret); WRONG_CODE=$1
[ "${#HEX}" -eq 32 ] && [ "${#CODE}" -eq 31 ] || { printf '%s\n' 'FAIL: could not create a test secret' >&2; exit 1; }

port=$((21000 + $$ % 20000))

# start_listener LOG IDENTITY ALPN CIPHERS VERSION
start_listener() {
    port=$((port + 1))
    python3 -I "$HERE/tests/tls-listener.py" "$port" "$HEX" "$2" "$3" "$4" "$5" "$1" 2>>"$1" &
    SERVER=$!
    i=0
    until grep -q LISTENING "$1" 2>/dev/null; do
        i=$((i + 1))
        [ "$i" -lt 100 ] || { printf '%s\n' 'FAIL: listener did not start' >&2; exit 1; }
        sleep 0.1
    done
}
stop_listener() {
    kill "$SERVER" 2>/dev/null || true
    wait "$SERVER" 2>/dev/null || true
    SERVER=
}
failed=0
run_case() {
    # run_case NAME CODE EXPECT
    if printf '%s\n' "$2" | "$OUT/tests/tls-loopback" --port "$port" --expect "$3"; then
        :
    else
        printf 'FAIL: case %s\n' "$1"
        failed=1
    fi
}
expect_log() {
    # expect_log LOG TEXT DESCRIPTION
    if grep -qF -- "$2" "$1"; then
        printf 'PASS: listener %s\n' "$3"
    else
        printf 'FAIL: listener did not record %s\n' "$3"
        failed=1
    fi
}

start_listener "$WORK/ready.log" locachy-kvm-v1 locachy-kvm-v1 PSK-AES128-GCM-SHA256 1.2
run_case 'matching profile and code' "$CODE" ready
sleep 0.5
stop_listener
expect_log "$WORK/ready.log" "IDENTITY 'locachy-kvm-v1'" "the PSK identity locachy-kvm-v1"
expect_log "$WORK/ready.log" "CIPHER 'PSK-AES128-GCM-SHA256' VERSION TLSv1.2" "TLS 1.2 with PSK-AES128-GCM-SHA256"
expect_log "$WORK/ready.log" "ALPN 'locachy-kvm-v1'" "the ALPN locachy-kvm-v1"
expect_log "$WORK/ready.log" "FRAME-PING True" "the framed control message"

start_listener "$WORK/wrong-code.log" locachy-kvm-v1 locachy-kvm-v1 PSK-AES128-GCM-SHA256 1.2
run_case 'wrong pairing code' "$WRONG_CODE" reject-tls
stop_listener

start_listener "$WORK/wrong-identity.log" other-identity locachy-kvm-v1 PSK-AES128-GCM-SHA256 1.2
run_case 'listener expecting another PSK identity' "$CODE" reject-tls
stop_listener

start_listener "$WORK/wrong-suite.log" locachy-kvm-v1 locachy-kvm-v1 PSK-AES256-GCM-SHA384 1.2
run_case 'listener offering only another PSK suite' "$CODE" refuse
stop_listener

start_listener "$WORK/wrong-alpn.log" locachy-kvm-v1 other-protocol PSK-AES128-GCM-SHA256 1.2
run_case 'listener without the locachy ALPN' "$CODE" refuse
stop_listener

start_listener "$WORK/wrong-version.log" locachy-kvm-v1 locachy-kvm-v1 PSK-AES128-GCM-SHA256 1.3
run_case 'TLS 1.3-only listener' "$CODE" reject-tls
stop_listener

if [ "$failed" -eq 0 ]; then
    printf '%s\n' 'PASS: loopback TLS 1.2 PSK interoperability (identity-enforcing listener; not a cable run)'
    exit 0
fi
printf '%s\n' 'FAIL: loopback TLS 1.2 PSK interoperability' >&2
for log in "$WORK"/*.log; do printf '%s\n' "== ${log##*/}" >&2; sed -n '1,12p' "$log" >&2; done
exit 1
