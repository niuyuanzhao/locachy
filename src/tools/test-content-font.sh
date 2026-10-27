#!/bin/sh
# Mock-screen tests using packaged DejaVu and Noto CJK fonts, no display/input.
# Run in the approved Linux development root with FreeType development files.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:-/tmp/test-content-font}
mkdir -p "$out"
cc=${CC:-cc}
flags="-std=c11 -O2 -Wall -Wextra -Werror -I$src/base/include"
ft_flags=$(pkg-config --cflags freetype2)
ft_libs=$(pkg-config --libs freetype2)
for kind in normal sanitize; do
    extra=
    [ "$kind" = normal ] || extra="-g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
    # Compile separately too, so including the implementation in the tests
    # does not hide unused helpers or accidental external dependencies.
    $cc $flags $extra $ft_flags -DCONFIG_FREETYPE -c "$src/base/font.c" -o "$out/font-$kind.o"
    $cc $flags $extra $ft_flags -DCONFIG_FREETYPE "$src/tools/test-content-font.c" $ft_libs -o "$out/test-$kind"
    "$out/test-$kind"
    $cc $flags $extra "$src/tools/test-content-font.c" -o "$out/test-unavailable-$kind"
    "$out/test-unavailable-$kind"
done
if command -v musl-gcc >/dev/null 2>&1; then
    musl-gcc $flags -static "$src/tools/test-content-font.c" -o "$out/test-unavailable-musl"
    "$out/test-unavailable-musl"
fi
