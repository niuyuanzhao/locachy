#!/bin/sh
# Build the LOCachy Mac extended-display sender without installing or launching it.
# The upstream source is fetched by immutable commit and verified before use.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:-/tmp/locachy-macos-companion-build}
UPSTREAM_COMMIT=0f44853adc5d44646a9e4d74e4516db460efb8bb
UPSTREAM_SHA256=dba0ece10f0d0d810b63f64269d62dd3eaea52058f7566ea1159fb17be0f1a15
URL="https://codeload.github.com/peetzweg/opendisplay/tar.gz/$UPSTREAM_COMMIT"
ARCHIVE="$OUT/opendisplay-$UPSTREAM_COMMIT.tar.gz"
SOURCE="$OUT/opendisplay-$UPSTREAM_COMMIT"
GENERATED="$OUT/generated"
PRODUCT="$OUT/locachy-macos-sender"

case "$(uname -s)" in Darwin) ;; *) printf '%s\n' 'Mac companion must be built on macOS with xcrun.' >&2; exit 2 ;; esac
command -v xcrun >/dev/null || { printf '%s\n' 'xcrun is required.' >&2; exit 2; }
command -v curl >/dev/null || { printf '%s\n' 'curl is required.' >&2; exit 2; }
command -v shasum >/dev/null || { printf '%s\n' 'shasum is required.' >&2; exit 2; }
rm -rf -- "$SOURCE" "$GENERATED"
mkdir -p -- "$OUT" "$GENERATED"
if [ ! -f "$ARCHIVE" ]; then
    curl --fail --location --retry 2 --connect-timeout 15 --max-time 180 "$URL" -o "$ARCHIVE"
fi
actual=$(shasum -a 256 "$ARCHIVE" | cut -d ' ' -f 1)
[ "$actual" = "$UPSTREAM_SHA256" ] || { printf 'OpenDisplay archive digest mismatch: %s\n' "$actual" >&2; exit 1; }
tar -xzf "$ARCHIVE" -C "$OUT"
# GitHub's archive directory is opendisplay-<commit>.
[ -d "$SOURCE" ] || { printf 'Missing extracted upstream tree: %s\n' "$SOURCE" >&2; exit 1; }
for file in Mac/VirtualDisplay.swift Mac/OriginRestore.swift Mac/VirtualCanvasSizing.swift Mac/CGVirtualDisplayPrivate.h; do
    mkdir -p "$GENERATED/$(dirname "$file")"
    cp "$SOURCE/$file" "$GENERATED/$file"
done
patch --batch --forward --fuzz=0 -d "$GENERATED" -p1 < "$ROOT/patches/session-display.patch"

SDK=$(xcrun --sdk macosx --show-sdk-path)
SWIFTC=$(xcrun --find swiftc)
# Use the host architecture so this command is safe on both Apple silicon and
# Intel build Macs. A release builder can invoke this recipe once per arch and
# lipo the two resulting binaries outside this repository.
TARGET="$(uname -m)-apple-macos13.0"
"$SWIFTC" -swift-version 5 -parse-as-library -target "$TARGET" -sdk "$SDK" -O \
    -import-objc-header "$GENERATED/Mac/CGVirtualDisplayPrivate.h" \
    -framework AppKit -framework Carbon -framework CoreGraphics -framework CoreMedia \
    -framework CoreServices -framework CoreVideo -framework Network \
    -framework ScreenCaptureKit -framework Security -framework VideoToolbox \
    "$GENERATED/Mac/OriginRestore.swift" \
    "$GENERATED/Mac/VirtualCanvasSizing.swift" \
    "$GENERATED/Mac/VirtualDisplay.swift" \
    "$ROOT/sources/UpstreamLog.swift" \
    "$ROOT/sources/Wire.swift" \
    "$ROOT/sources/PairingCode.swift" \
    "$ROOT/sources/SecureTransport.swift" \
    "$ROOT/sources/Input.swift" \
    "$ROOT/sources/Capture.swift" \
    "$ROOT/sources/CompanionMain.swift" \
    -o "$PRODUCT"

printf 'PASS: built %s\n' "$PRODUCT"
printf 'Not launched: this recipe never creates a virtual display or requests TCC permissions.\n'
