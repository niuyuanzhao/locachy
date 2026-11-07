#!/bin/sh
# font-fixtures.sh DIR: fontconfig configurations for the font tests
# (test-fonts.sh, test-content-font.sh), each with a fonts directory and a
# cache of its own, from the fonts this host has installed (found with the
# host's fontconfig; no font file path is assumed). In DIR:
#   cjk.conf    DejaVu Sans Mono and Noto CJK, fontconfig's stock order for
#               them (both monospace families, 45-latin.conf; DejaVu Sans
#               Mono first, 60-latin.conf; Noto Sans Mono CJK KR,
#               65-nonlatin.conf) and LOCachy's link/64-locachy-cjk-prefer.conf
#               between them, as in conf.d
#   stock.conf  the same without LOCachy's file
#   latin.conf  DejaVu Sans Mono only: no CJK font
#   user.conf   cjk.conf with a user's preference before LOCachy's (as
#               50-user.conf is): Liberation Mono, Noto Sans Mono CJK JP
#   other.conf  DejaVu Sans Mono and a CJK family other than Noto CJK
#   reload.conf latin.conf's settings for fonts-reload, a directory the
#               reload test adds a CJK font to
#   env         MONO (DejaVu Sans Mono's regular file), CJK_FILE (Noto CJK's),
#               OTHER_CJK (that family's name; empty when this host has none)
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
dir=${1:?usage: font-fixtures.sh DIR}
mkdir -p "$dir"
dir=$(CDPATH= cd -- "$dir" && pwd)

file() { # PATTERN: the installed file fontconfig lists for it, or nothing
    fc-list -f '%{file}\n' "$1" | sort | head -n 1
}
need() { # NAME PATTERN
    f=$(file "$2")
    [ -n "$f" ] || { echo "font-fixtures: no $1 installed ($2)" >&2; exit 1; }
    printf '%s\n' "$f"
}
mono=$(need "DejaVu Sans Mono" "DejaVu Sans Mono:style=Book")
mono_b=$(need "DejaVu Sans Mono Bold" "DejaVu Sans Mono:style=Bold")
mono_i=$(need "DejaVu Sans Mono Oblique" "DejaVu Sans Mono:style=Oblique")
mono_bi=$(need "DejaVu Sans Mono Bold Oblique" "DejaVu Sans Mono:style=Bold Oblique")
cjk=$(need "Noto CJK" "Noto Sans Mono CJK SC:style=Regular")
cjk_b=$(need "Noto CJK Bold" "Noto Sans Mono CJK SC:style=Bold")
liberation=$(need "Liberation Mono" "Liberation Mono:style=Regular")
other=$(fc-list -f '%{file}\n' ':lang=zh' | grep -v -i noto | sort | head -n 1 || true)
other_family=
[ -z "$other" ] || other_family=$(fc-scan -f '%{family[0]}\n' "$other" | head -n 1)

fonts() { # NAME FILE...: DIR/fonts-NAME with those
    d=$dir/fonts-$1
    shift
    rm -rf "$d"
    mkdir -p "$d"
    for f in "$@"; do
        ln -s "$f" "$d/"
    done
}
fonts latin "$mono" "$mono_b" "$mono_i" "$mono_bi"
fonts cjk "$mono" "$mono_b" "$mono_i" "$mono_bi" "$cjk" "$cjk_b"
fonts user "$mono" "$mono_b" "$mono_i" "$mono_bi" "$cjk" "$cjk_b" "$liberation"
if [ -n "$other" ]; then
    fonts other "$mono" "$mono_b" "$mono_i" "$mono_bi" "$other"
fi

conf() { # NAME FONTS LOCACHY USER
    {
        printf '<?xml version="1.0"?>\n<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">\n<fontconfig>\n'
        printf '  <dir>%s/fonts-%s</dir>\n  <cachedir>%s/cache-%s</cachedir>\n' "$dir" "$2" "$dir" "$1"
        printf '  <alias><family>DejaVu Sans Mono</family><default><family>monospace</family></default></alias>\n'
        printf '  <alias><family>Liberation Mono</family><default><family>monospace</family></default></alias>\n'
        if [ "$4" = 1 ]; then # 50-user.conf: before the system's
            printf '  <alias><family>monospace</family><prefer><family>Liberation Mono</family>'
            printf '<family>Noto Sans Mono CJK JP</family></prefer></alias>\n'
        fi
        printf '  <alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>\n'
        [ "$3" = 0 ] || printf '  <include>%s/link/64-locachy-cjk-prefer.conf</include>\n' "$src"
        printf '  <alias><family>monospace</family><prefer><family>Noto Sans Mono CJK KR</family></prefer></alias>\n'
        printf '</fontconfig>\n'
    } >"$dir/$1.conf"
}
conf cjk cjk 1 0
conf stock cjk 0 0
conf latin latin 1 0
conf reload reload 1 0 # fonts-reload: test-fonts.sh makes it, the test changes it
conf user user 1 1
[ -z "$other" ] || conf other other 1 0
printf "MONO='%s'\nCJK_FILE='%s'\nOTHER_CJK='%s'\n" "$mono" "$cjk" "$other_family" >"$dir/env"
