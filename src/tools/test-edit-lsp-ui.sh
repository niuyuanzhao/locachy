#!/bin/sh
# Actual framebuffer editor callbacks; run only in the disposable Linux root.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# Current development root already contains editor dependencies.
# The generated translation unit is a test input, never a repository edit.
python3 - "$src" "$tmp/edit-integrated.c" <<'PY'
import pathlib
import sys
src = pathlib.Path(sys.argv[1])
text = (src / 'progs/edit.c').read_text()
if '#include "edit-language.inc"' not in text:
    text = text.replace('#include "edit-actions.inc"', '#include "edit-language.inc"\n#include "edit-actions.inc"')
pathlib.Path(sys.argv[2]).write_text(text)
harness = (src / 'tools/test-edit-ui.c').read_text()
harness = harness.replace('int main(int argc, char **argv)', 'int edit_lsp_original_ui_main(int argc, char **argv)')
pathlib.Path(sys.argv[2]).with_name('test-edit-lsp-ui-harness.inc').write_text(harness)
PY
extra=''
if [ "${SANITIZE:-0}" = 1 ]; then extra='-fsanitize=address,undefined -fno-omit-frame-pointer'; fi
# shellcheck disable=SC2046,SC2086
cc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-function -Wno-format-truncation -g -O1 $extra \
    -DCONFIG_IME -DCONFIG_FREETYPE -I"$src/base/include" -I"$src/base" -I"$src/progs" -I"$tmp" \
    -DEDIT_TEST_SOURCE=\"$tmp/edit-integrated.c\" -DEDIT_QUERIES=\"$src/edit/queries\" \
    $(pkg-config --cflags freetype2 json-glib-1.0) \
    "$src/tools/test-edit-lsp-ui.c" "$src/progs/edit-buffer.c" "$src/progs/edit-doc.c" \
    "$src/progs/edit-file.c" "$src/progs/edit-search.c" "$src/progs/edit-syntax.c" \
    "$src/progs/edit-tree.c" "$src/progs/edit-layout.c" "$src/progs/edit-lsp.c" \
    "$src/progs/edit-session.c" "$src/progs/edit-filesearch.c" \
    "$src/base/ui.c" -o "$tmp/test-edit-lsp-ui" \
    -ltree-sitter -lpcre2-8 $(pkg-config --libs freetype2 json-glib-1.0) -lm
ASAN_OPTIONS=detect_leaks=0 timeout 45 "$tmp/test-edit-lsp-ui" "$tmp/run"
