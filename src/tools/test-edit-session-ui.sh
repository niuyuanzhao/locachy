#!/bin/sh
# Actual edit session UI integration in the approved disposable Linux root:
# the real edit.c (with its session, language and multicursor includes) over
# test-edit-ui.c's scripted screen/keys fixture. That fixture's own main is
# renamed in a temporary copy so this test can supply its own.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=$(CDPATH= cd -- "$here/.." && pwd)
tmp=$(mktemp -d)
chmod 0711 "$tmp"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
pkg-config --atleast-version=1.10 json-glib-1.0
sed 's/^int main(int argc, char \*\*argv)$/static int edit_ui_main(int argc, char **argv)/' \
    "$here/test-edit-ui.c" > "$tmp/test-edit-ui.c"
[ "$(grep -c '^static int edit_ui_main(' "$tmp/test-edit-ui.c")" = 1 ]
cp "$here/test-edit-session-ui.c" "$tmp/"
flags=''
if [ "${SANITIZE:-0}" = 1 ]; then
    flags='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'
fi
# shellcheck disable=SC2046,SC2086
cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -D_GNU_SOURCE -g -O1 $flags \
    -DCONFIG_IME -DCONFIG_FREETYPE -DEDIT_QUERIES=\""$src/edit/queries"\" \
    -DEDIT_TEST_SOURCE=\""$src/progs/edit.c"\" \
    -I"$here" -I"$src/progs" -I"$src/base/include" -I"$src/base" $(pkg-config --cflags freetype2 json-glib-1.0) \
    "$tmp/test-edit-session-ui.c" "$src/progs/edit-buffer.c" "$src/progs/edit-doc.c" \
    "$src/progs/edit-file.c" "$src/progs/edit-search.c" "$src/progs/edit-syntax.c" \
    "$src/progs/edit-tree.c" "$src/progs/edit-layout.c" "$src/progs/edit-session.c" \
    "$src/progs/edit-lsp.c" "$src/progs/edit-filesearch.c" "$src/base/ui.c" \
    -Wl,--wrap=getpwuid $(pkg-config --libs freetype2 json-glib-1.0) -ltree-sitter -lpcre2-8 -lm \
    -o "$tmp/test-edit-session-ui"
ASAN_OPTIONS=detect_leaks=0 timeout 120 "$tmp/test-edit-session-ui" "$tmp/home"
