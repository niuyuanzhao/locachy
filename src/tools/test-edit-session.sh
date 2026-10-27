#!/bin/sh
# Run in the approved disposable Linux root, as root, with uid 1000 present.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
pkg-config --atleast-version=1.10 json-glib-1.0
flags=''
if [ "${SANITIZE:-0}" = 1 ]; then
    flags='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'
fi
# shellcheck disable=SC2046,SC2086
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 $flags \
    "$here/../progs/edit-session.c" "$here/../progs/edit-file.c" \
    "$here/test-edit-session.c" $(pkg-config --cflags --libs json-glib-1.0) \
    -Wl,--wrap=write -Wl,--wrap=renameat -Wl,--wrap=fsync -Wl,--wrap=getpwuid \
    -o "$tmp/test-edit-session"
timeout 90 "$tmp/test-edit-session"
