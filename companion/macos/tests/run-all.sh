#!/bin/sh
# Build the companion and run every check that needs no display, permission or
# cable: protocol/input/pairing tests and loopback TLS interoperability.
# Each step rebuilds its binary first; any compile or test failure stops here.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-/tmp/locachy-macos-companion-build}
mkdir -p "$OUT/tests"

"$HERE/build.sh" "$OUT"

rm -f "$OUT/tests/protocol-tests"
xcrun swiftc -swift-version 5 -parse-as-library -target "$(uname -m)-apple-macos13.0" \
    -sdk "$(xcrun --sdk macosx --show-sdk-path)" -framework AppKit -framework Carbon \
    "$HERE/sources/Wire.swift" "$HERE/sources/PairingCode.swift" "$HERE/sources/Input.swift" \
    "$HERE/tests/ProtocolTests.swift" -o "$OUT/tests/protocol-tests"
"$OUT/tests/protocol-tests"

"$HERE/tests/tls-loopback.sh" "$OUT"

"$OUT/locachy-macos-sender" --check
printf '%s\n' 'PASS: all companion checks (build, protocol, loopback TLS); no cable or Mac session was exercised'
