#!/bin/sh
# Build the ACTUAL editor UI via its existing screen/key/pointer harness.
# Rename only its test main in a generated temporary copy; repository tests
# and editor sources are never rewritten or patched by this script.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:-/tmp/test-edit-folding-ui}
mkdir -p "$out"
python3 - "$src/tools/test-edit-ui.c" "$out/harness.c" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
old = 'int main(int argc, char **argv)'
assert source.count(old) == 1
pathlib.Path(sys.argv[2]).write_text(source.replace(old, 'int folding_harness_main(int argc, char **argv)'))
PY
cc=${CC:-cc}
for kind in plain sanitized; do
    extra=
    [ "$kind" = plain ] || extra='-g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'
    # shellcheck disable=SC2086
    "$cc" -O2 -D_GNU_SOURCE -Wall -Wextra -Werror $extra \
        -DCONFIG_IME -DCONFIG_FREETYPE $(pkg-config --cflags freetype2 json-glib-1.0) \
        -I"$src/base/include" -I"$src/base" \
        "-DEDIT_TEST_SOURCE=\"$src/progs/edit.c\"" \
        "-DEDIT_FOLDING_UI_HARNESS=\"$out/harness.c\"" \
        "-DEDIT_QUERIES=\"$src/edit/queries\"" \
        "$src/tools/test-edit-folding-ui.c" \
        "$src/progs/edit-buffer.c" "$src/progs/edit-doc.c" "$src/progs/edit-file.c" \
        "$src/progs/edit-search.c" "$src/progs/edit-syntax.c" "$src/progs/edit-tree.c" \
        "$src/progs/edit-layout.c" "$src/progs/edit-lsp.c" "$src/progs/edit-session.c" \
        "$src/progs/edit-filesearch.c" "$src/base/ui.c" \
        -o "$out/test-edit-folding-ui-$kind" -ltree-sitter -lpcre2-8 $(pkg-config --libs freetype2 json-glib-1.0) -lm
    dir=$(mktemp -d "$out/run-$kind-XXXXXX")
    rmdir "$dir"
    ASAN_OPTIONS=detect_leaks=0 "$out/test-edit-folding-ui-$kind" "$dir"
done
printf '%s\n' 'PASS actual editor folding UI (plain and ASan/UBSan)'
