#!/bin/sh
# Kate's visible behaviour in the real editor (test-edit-kate.c), no display
# or input devices; SANITIZE=1 builds it with ASan/UBSan.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT HUP INT TERM
chmod 755 "$out" # the fixtures are read as the user (uid 1000), as edit reads files
python3 - "$src/tools/test-edit-ui.c" "$out/harness.inc" <<'PY'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text()
assert text.count('int main(int argc, char **argv)') == 1
Path(sys.argv[2]).write_text(text.replace('int main(int argc, char **argv)', 'int original_editor_ui_main(int argc, char **argv)'))
PY
flags=
[ "${SANITIZE:-0}" != 1 ] || flags='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'
# shellcheck disable=SC2046,SC2086
cc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -DCONFIG_IME -DCONFIG_FREETYPE $flags \
    -I"$src/base/include" -I"$src/base" -I"$src/progs" \
    -DEDIT_TEST_SOURCE=\""$src/progs/edit.c"\" -DEDIT_KATE_HARNESS=\""$out/harness.inc"\" \
    -DEDIT_QUERIES=\""$src/edit/queries"\" $(pkg-config --cflags freetype2 json-glib-1.0) \
    "$src/tools/test-edit-kate.c" "$src/progs/edit-buffer.c" "$src/progs/edit-doc.c" \
    "$src/progs/edit-file.c" "$src/progs/edit-search.c" "$src/progs/edit-syntax.c" "$src/progs/edit-tree.c" \
    "$src/progs/edit-layout.c" "$src/progs/edit-lsp.c" "$src/progs/edit-session.c" "$src/progs/edit-filesearch.c" \
    "$src/base/ui.c" -Wl,--wrap=getpwuid $(pkg-config --libs freetype2 json-glib-1.0) -ltree-sitter -lpcre2-8 -lm \
    -o "$out/test"
ASAN_OPTIONS=detect_leaks=0 timeout 120 "$out/test" "$out/fixtures"
