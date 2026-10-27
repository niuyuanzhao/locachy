#!/bin/sh
# Compile the actual editor + in-memory device fixture; never access a display,
# input device, X230, Docker or a running application. Use a unique OUT per run.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:-$(mktemp -d /tmp/test-edit-multicursor.XXXXXX)}
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
# The existing fixture defines main twice (edit.c and its test runner). Rename
# only its runner, preserving real edit.c inclusion and all existing stubs.
# Do not edit/copy editor code or mutate the shared test-edit-ui.c.
python3 - "$src" "$out/fixture.inc" <<'PY'
import pathlib, sys
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
s = (src / 'tools/test-edit-ui.c').read_text()
s = '#define EDIT_TEST_SOURCE "' + str(src / 'progs/edit.c') + '"\n' + s
s = s.replace('#include "../progs/edit.c"', '#include "' + str(src / 'progs/edit.c') + '"', 1)
s = s.replace('int main(int argc, char **argv)', 'int edit_multicursor_fixture_main(int argc, char **argv)', 1)
s = 'static unsigned mc_fixture_caret_paints;\n' + s
s = s.replace('    caret_x = x;', '    mc_fixture_caret_paints++;\n    caret_x = x;', 1)
dst.write_text(s)
PY
modules="$src/progs/edit-buffer.c $src/progs/edit-doc.c $src/progs/edit-file.c
$src/progs/edit-search.c $src/progs/edit-syntax.c $src/progs/edit-tree.c
$src/progs/edit-layout.c $src/progs/edit-lsp.c $src/progs/edit-session.c $src/progs/edit-filesearch.c
$src/base/ui.c"
for kind in plain asan; do
    extra=
    [ "$kind" = plain ] || extra='-g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'
    # shellcheck disable=SC2086
    gcc -O1 -Wall -Wextra -Werror -D_GNU_SOURCE -DCONFIG_IME -DCONFIG_FREETYPE \
        -I"$src/base/include" -I"$src/base" $(pkg-config --cflags freetype2 json-glib-1.0) \
        "-DEDIT_QUERIES=\"$src/edit/queries\"" "-DEDIT_MULTICURSOR_FIXTURE=\"$out/fixture.inc\"" \
        $extra -o "$out/test-edit-multicursor-$kind" "$src/tools/test-edit-multicursor.c" $modules \
        -ltree-sitter -lpcre2-8 $(pkg-config --libs freetype2 json-glib-1.0) -lm
    ASAN_OPTIONS=detect_leaks=1 "$out/test-edit-multicursor-$kind"
done
