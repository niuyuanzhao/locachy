#!/bin/sh
# Device-free seat handover deadline and helper process-group cleanup.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-seat-release.sh NEW_OUTPUT_DIRECTORY}
case "$out" in /*) ;; *) printf '%s\n' 'Output must be a new absolute directory' >&2; exit 2 ;; esac
[ ! -e "$out" ] || { printf '%s\n' 'Output must be a new absolute directory' >&2; exit 2; }
mkdir -p "$out"
gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -ffunction-sections -fdata-sections \
    -I"$src/base/include" -I"$src/base" "$src/tools/test-seat-release.c" \
    -Wl,--gc-sections -o "$out/test-seat-release"
timeout 10 "$out/test-seat-release"
