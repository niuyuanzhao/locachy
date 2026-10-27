#!/bin/sh
# Test the real adapter using a temporary copy of the existing UI harness.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:-/tmp/test-edit-filesearch-ui}
mkdir -p "$out"
python3 - "$src" "$out" <<'PY'
from pathlib import Path
import sys
src, out = map(Path, sys.argv[1:])
p = (src/'tools/test-edit-ui.c').read_text()
p = p.replace('int main(int argc, char **argv)', 'int edit_ui_original_main(int argc, char **argv)')
(out/'harness.c').write_text(p)
p = (src/'progs/edit.c').read_text()
if '#include "edit-filesearch-ui.inc"' not in p:
    p = p.replace('#include "edit-actions.inc"', '#include "edit-filesearch-ui.inc"\n#include "edit-actions.inc"')
    p = p.replace('static int session_quit(void);', 'static int session_quit(void);\nstatic void filesearch_menu(void);\nstatic void filesearch_tick(void);\nstatic int filesearch_active(void);\nstatic void filesearch_done(void);')
(out/'edit.c').write_text(p)
PY
for kind in plain asan; do
    extra=
    [ "$kind" = plain ] || extra="-g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
    # shellcheck disable=SC2086
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -DCONFIG_IME -DCONFIG_FREETYPE $extra \
        -I"$src/base/include" -I"$src/base" -I"$src/progs" \
        $(pkg-config --cflags freetype2 json-glib-1.0) \
        -DEDIT_TEST_SOURCE=\""$out/edit.c"\" -DEDIT_FILESEARCH_HARNESS=\""$out/harness.c"\" \
        "$src/tools/test-edit-filesearch-ui.c" "$src/progs/edit-buffer.c" "$src/progs/edit-doc.c" \
        "$src/progs/edit-file.c" "$src/progs/edit-search.c" "$src/progs/edit-syntax.c" \
        "$src/progs/edit-tree.c" "$src/progs/edit-layout.c" "$src/progs/edit-lsp.c" \
        "$src/progs/edit-session.c" "$src/progs/edit-filesearch.c" "$src/base/ui.c" \
        -ltree-sitter -lpcre2-8 -lm $(pkg-config --libs json-glib-1.0) -o "$out/$kind"
    ASAN_OPTIONS=detect_leaks=0 "$out/$kind" "$out/run-$kind"
done
