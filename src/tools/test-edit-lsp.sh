#!/bin/sh
# Run inside the approved disposable Linux root, never on the deployed machine.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
command -v clangd >/dev/null
pkg-config --exists json-glib-1.0
flags=''
if [ "${SANITIZE:-0}" = 1 ]; then
    flags='-fsanitize=address,undefined -fno-omit-frame-pointer'
fi
# shellcheck disable=SC2046,SC2086
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 $flags -I"$here/../base/include" \
    "$here/../progs/edit-lsp.c" "$here/test-edit-lsp.c" \
    $(pkg-config --cflags --libs json-glib-1.0) -o "$tmp/test-edit-lsp"
EDIT_LSP_TEST_SERVER="$here/test-edit-lsp-server.py" \
    timeout 90 "$tmp/test-edit-lsp" "$@"
