#!/bin/sh
# test-fonts.sh OUT: how the FreeType programs find fonts (base/fontmatch.c,
# tools/FONTS.md), device-free, in fixture fontconfig configurations
# (font-fixtures.sh): the configured monospace is the primary face, CJK comes
# from fontconfig's fallback (LOCachy's 64-locachy-cjk-prefer.conf choosing
# Noto CJK's Simplified Chinese variant, a user's fonts.conf before it, any
# other CJK family), without a CJK font nothing fails, and fonts installed
# later reach the running bar on its reload. Then the content
# font's tests (test-content-font.sh) in the same fixtures.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: test-fonts.sh OUT}
mkdir -p "$out"
sh "$src/tools/font-fixtures.sh" "$out/fixtures"
. "$out/fixtures/env"
flags="-std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -DCONFIG_FREETYPE -I$src/base/include -I$src/base"
ft="$(pkg-config --cflags --libs freetype2 fontconfig)"
# The user's own fontconfig files (50-user.conf) are not the fixtures'.
export HOME="$out/home" XDG_CONFIG_HOME="$out/home/.config"
unset FC_LANG LC_ALL LC_CTYPE
for kind in normal sanitize; do
    extra=
    [ "$kind" = normal ] || extra="-g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
    gcc $flags $extra "$src/tools/test-fonts.c" $ft -o "$out/test-fonts-$kind"
    run() { # CASE PRIMARY CJK [LANG]
        FONTCONFIG_FILE="$out/fixtures/$1.conf" PRIMARY="$2" CJK="$3" LANG="${4:-C.UTF-8}" \
            "$out/test-fonts-$kind" "$1${4:+ $4}"
    }
    run cjk "DejaVu Sans Mono" " SC"
    run cjk "DejaVu Sans Mono" " SC" en_US.UTF-8
    run stock "DejaVu Sans Mono" " KR" # fontconfig's order alone: Korean forms
    run latin "DejaVu Sans Mono" -
    run user "Liberation Mono" " JP"
    rm -rf "$out/fixtures/fonts-reload"
    cp -R "$out/fixtures/fonts-latin" "$out/fixtures/fonts-reload"
    FONTCONFIG_FILE="$out/fixtures/reload.conf" FONT_DIR="$out/fixtures/fonts-reload" FONT_ADD="$CJK_FILE" \
        LANG=C.UTF-8 "$out/test-fonts-$kind" reload
    if [ -n "$OTHER_CJK" ]; then
        run other "DejaVu Sans Mono" "$OTHER_CJK"
    else
        echo "SKIP other: no CJK family besides Noto CJK is installed on this host"
    fi
done
FONT_FIXTURES="$out/fixtures" sh "$src/tools/test-content-font.sh" "$out/content-font"
echo "PASS test-fonts: fontconfig's fonts for the primary face and the CJK fallback"
