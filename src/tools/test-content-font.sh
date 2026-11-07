#!/bin/sh
# Mock-screen tests of the content font with installed DejaVu Sans Mono and
# Noto CJK, chosen through fontconfig in fixture configurations
# (font-fixtures.sh; FONT_FIXTURES: ones made already), no display/input.
# Run in the approved Linux development root with FreeType development files.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:-/tmp/test-content-font}
mkdir -p "$out"
fixtures=${FONT_FIXTURES:-$out/fixtures}
[ -n "${FONT_FIXTURES:-}" ] || sh "$src/tools/font-fixtures.sh" "$fixtures"
. "$fixtures/env"
export HOME="$out/home" XDG_CONFIG_HOME="$out/home/.config" LANG=C.UTF-8
unset FC_LANG LC_ALL LC_CTYPE
cc=${CC:-cc}
flags="-std=c11 -O2 -Wall -Wextra -Werror -I$src/base/include"
ft_flags=$(pkg-config --cflags freetype2 fontconfig)
ft_libs=$(pkg-config --libs freetype2 fontconfig)
for kind in normal sanitize; do
    extra=
    [ "$kind" = normal ] || extra="-g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
    # Compile separately too, so including the implementation in the tests
    # does not hide unused helpers or accidental external dependencies.
    $cc $flags $extra $ft_flags -DCONFIG_FREETYPE -c "$src/base/font.c" -o "$out/font-$kind.o"
    $cc $flags $extra $ft_flags -DCONFIG_FREETYPE "$src/tools/test-content-font.c" "$src/base/fontmatch.c" \
        $ft_libs -o "$out/test-$kind"
    FONTCONFIG_FILE="$fixtures/cjk.conf" "$out/test-$kind" cjk "$MONO"
    FONTCONFIG_FILE="$fixtures/latin.conf" "$out/test-$kind" latin
    $cc $flags $extra "$src/tools/test-content-font.c" -o "$out/test-unavailable-$kind"
    "$out/test-unavailable-$kind"
done
if command -v musl-gcc >/dev/null 2>&1; then
    musl-gcc $flags -static "$src/tools/test-content-font.c" -o "$out/test-unavailable-musl"
    "$out/test-unavailable-musl"
fi
