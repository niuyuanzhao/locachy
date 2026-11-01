#!/bin/sh
# Actual editor workspace operations, no display or input devices.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT HUP INT TERM
python3 - "$src/tools/test-edit-ui.c" "$out/harness.inc" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text().replace('int main(int argc, char **argv)',
                                           'int original_editor_ui_main(int argc, char **argv)')
Path(sys.argv[2]).write_text(text)
PY
flags=
[ "${SANITIZE:-0}" != 1 ] || flags='-fsanitize=address,undefined -fno-omit-frame-pointer'
# shellcheck disable=SC2046,SC2086
cc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -DCONFIG_IME -DCONFIG_FREETYPE $flags \
    -I"$src/base/include" -I"$src/base" -I"$src/progs" \
    -DEDIT_TEST_SOURCE=\""$src/progs/edit.c"\" -DEDIT_WORKSPACE_HARNESS=\""$out/harness.inc"\" \
    -DEDIT_QUERIES=\""$src/edit/queries"\" $(pkg-config --cflags freetype2 json-glib-1.0) \
    "$src/tools/test-edit-workspace.c" "$src/progs/edit-buffer.c" "$src/progs/edit-doc.c" \
    "$src/progs/edit-file.c" "$src/progs/edit-search.c" "$src/progs/edit-syntax.c" "$src/progs/edit-tree.c" \
    "$src/progs/edit-layout.c" "$src/progs/edit-lsp.c" "$src/progs/edit-session.c" "$src/progs/edit-filesearch.c" \
    "$src/base/ui.c" $(pkg-config --libs freetype2 json-glib-1.0) -ltree-sitter -lpcre2-8 -lm -o "$out/test"
ASAN_OPTIONS=detect_leaks=0 timeout 30 "$out/test" "$out/fixtures"
