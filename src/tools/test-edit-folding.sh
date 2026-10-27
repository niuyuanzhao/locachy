#!/bin/sh
# Build and run with the REAL installed tree-sitter and all eleven mapped
# grammars; no mock-parser fallback. Run inside the approved Linux devroot.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:-/tmp/test-edit-folding}
mkdir -p "$out/query-fixture/c" "$out/query-fixture/folding-common"
# The fixture uses real C grammar nodes, plus the same query format used
# by packages. It proves precedence/predicates/inheritance, not a fake parser.
printf '%s\n' '; inherits: folding-common' '(compound_statement) @_private' > "$out/query-fixture/c/folds.scm"
printf '%s\n' '((comment) @fold (#match? @fold "^/\\* keep"))' > "$out/query-fixture/folding-common/folds.scm"
printf '%s\n' '(comment) @comment.documentation' > "$out/query-fixture/c/highlights.scm"
cc=${CC:-cc}
for kind in plain sanitized; do
    extra=
    [ "$kind" = plain ] || extra='-g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'
    for query in packaged fixture; do
        query_flags=
        [ "$query" = packaged ] || query_flags="-DPACKAGED_QUERIES=\"$out/query-fixture\""
        # shellcheck disable=SC2086
        "$cc" -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror $extra \
            -I"$src/base/include" -I"$src/base" \
            "-DEDIT_QUERIES=\"$src/edit/queries\"" $query_flags \
            "$src/tools/test-edit-folding.c" "$src/progs/edit-syntax.c" "$src/progs/edit-buffer.c" \
            -o "$out/test-edit-folding-$query-$kind" -ltree-sitter -lpcre2-8 -ldl
        if [ "$query" = packaged ]; then
            ASAN_OPTIONS=detect_leaks=0 "$out/test-edit-folding-$query-$kind"
        else
            ASAN_OPTIONS=detect_leaks=0 "$out/test-edit-folding-$query-$kind" query
        fi
    done
done
printf '%s\n' 'PASS folding/styles plain and AddressSanitizer/UBSan (actual installed grammars)'
