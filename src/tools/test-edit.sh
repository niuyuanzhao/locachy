#!/bin/sh
# test-edit.sh [OUT]: build edit as make.sh builds the framebuffer programs
# (to see that it compiles with -Werror and links), then build its tests
# twice, as they are and with AddressSanitizer and UBSan, and run them all.
# For the development root: its queries are read from edit/queries here
# (installed by main to /usr/local/share/locachy/edit/queries). As root, the
# tests also check that files are read and written as the user (uid 1000).
# Prints PASS lines; exits non-zero on any failure.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:-/tmp/test-edit}
rm -rf "$out"
mkdir -p "$out/obj"

flags="$(pkg-config --cflags freetype2 libnm libinput) -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -DCONFIG_LIBINPUT"
libs="$(pkg-config --libs freetype2 libnm libinput) -lm"
ime_flags= ime_libs=
if [ -f /usr/include/rime_api.h ] && [ -f /usr/lib/librime.so ]; then
    ime_flags=-DCONFIG_IME
    rime_lua=$(ldd /usr/lib/rime-plugins/librime-lua.so 2>/dev/null | sed -n 's/^[[:space:]]*\(liblua[^ ]*\) => .*/\1/p' | head -n 1)
    ime_libs="-lrime${rime_lua:+ -Wl,--push-state,--no-as-needed -l:$rime_lua -Wl,--pop-state}"
fi
cc="gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I$src/base/include -I$src/base"
for f in "$src"/base/*.c; do
    name=$(basename "$f" .c)
    [ "$name" != ime ] || continue
    $cc $flags -c "$f" -o "$out/obj/base-$name.o"
done
$cc $ime_flags -c "$src/base/ime.c" -o "$out/obj/ime.o"
edit_src="$src/progs/edit.c $src/progs/edit-buffer.c $src/progs/edit-doc.c $src/progs/edit-file.c
    $src/progs/edit-search.c $src/progs/edit-syntax.c $src/progs/edit-tree.c $src/progs/edit-layout.c
    $src/progs/edit-lsp.c $src/progs/edit-session.c $src/progs/edit-filesearch.c"
# As make.sh would: edit's modules, files.c (files_pick), the base library
# and the input method, tree-sitter and PCRE2.
$cc $ime_flags $flags $(pkg-config --cflags json-glib-1.0) -o "$out/edit" $edit_src "$src/progs/files.c" "$out"/obj/base-*.o "$out/obj/ime.o" \
    $ime_libs $libs -ltree-sitter -lpcre2-8 $(pkg-config --libs json-glib-1.0)
echo "PASS build: edit links as make.sh builds it ($out/edit)"

modules="$src/progs/edit-buffer.c $src/progs/edit-doc.c $src/progs/edit-file.c $src/progs/edit-search.c
    $src/progs/edit-syntax.c $src/progs/edit-tree.c $src/progs/edit-layout.c"
queries="-DEDIT_QUERIES=\"$src/edit/queries\""
status=0
for kind in plain asan; do
    extra=
    [ "$kind" = plain ] || extra="-g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
    for t in core syntax ui layout; do
        files="$src/tools/test-edit-$t.c $modules"
        [ "$t" != ui ] || files="$src/tools/test-edit-$t.c $src/progs/edit-buffer.c $src/progs/edit-doc.c
            $src/progs/edit-file.c $src/progs/edit-search.c $src/progs/edit-syntax.c $src/progs/edit-tree.c
            $src/progs/edit-layout.c $src/progs/edit-lsp.c $src/progs/edit-session.c $src/progs/edit-filesearch.c
            $src/base/ui.c"
        # shellcheck disable=SC2086
        $cc -DCONFIG_IME -DCONFIG_FREETYPE $(pkg-config --cflags freetype2 json-glib-1.0) $queries $extra -o "$out/test-edit-$t-$kind" $files \
            -ltree-sitter -lpcre2-8 $(pkg-config --libs freetype2 json-glib-1.0) -lm
    done
    for t in core syntax ui layout; do
        dir="$out/run-$t-$kind"
        echo "== test-edit-$t ($kind)"
        if ! (cd "$out" && ASAN_OPTIONS=detect_leaks=0 "./test-edit-$t-$kind" "$dir"); then
            echo "FAIL test-edit-$t ($kind)"
            status=1
        fi
    done
    for case in horizontal tree-wheel tree-refresh tree-git-refresh tree-empty selection-scope encoding-controls \
        reload-safety readonly-save-as save-as-open-modified selection-no-scope; do
        dir="$out/run-audit-$case-$kind"
        echo "== test-edit-ui audit: $case ($kind)"
        if ! ASAN_OPTIONS=detect_leaks=0 "$out/test-edit-ui-$kind" "$dir" "$case"; then
            echo "FAIL test-edit-ui audit: $case ($kind)"
            status=1
        fi
    done
done
# The modules and the editor features built on them, each with its own
# fixtures: these scripts build plain and sanitized themselves...
for t in folding folding-ui multicursor filesearch filesearch-ui; do
    echo "== test-edit-$t"
    sh "$src/tools/test-edit-$t.sh" "$out/$t" || { echo "FAIL test-edit-$t"; status=1; }
done
echo "== test-content-font"
sh "$src/tools/test-content-font.sh" "$out/content-font" || { echo "FAIL test-content-font"; status=1; }
# ...these once per SANITIZE value.
for t in lsp lsp-ui session session-ui workspace kate; do
    for sanitize in 0 1; do
        echo "== test-edit-$t (SANITIZE=$sanitize)"
        SANITIZE=$sanitize sh "$src/tools/test-edit-$t.sh" || { echo "FAIL test-edit-$t (SANITIZE=$sanitize)"; status=1; }
    done
done
[ "$status" = 0 ] && echo "PASS all edit tests (plain and sanitized)"
exit "$status"
