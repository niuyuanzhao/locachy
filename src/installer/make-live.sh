#!/bin/sh
# make-live.sh SOURCE ROOT: the live medium's own programs, built on the
# ISO's build host (the same Arch packages as the medium's) for any x86-64,
# into its root file system ROOT (archiso's airootfs), as make.sh builds and
# installs them on a system:
#   /usr/local/bin/installer             the full-screen installation (progs/installer.c)
#   /usr/local/bin/term                  its terminal: a root shell, for repairs
#   /usr/local/lib/locachy/keytable      the key table of the layout chosen (libxkbcommon)
#   /usr/local/share/locachy/font.psf, terminus/   their text (Terminus)
#   /usr/local/share/locale/LL_CC/       their translations (po/LINGUAS)
#   /usr/lib/locale/LL_CC.utf8/          the locales those need (localedef)
set -eu
src=$1
root=$2
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT

# As make.sh's $GCC, for the CPU level every x86-64 runs.
gcc="gcc -O2 -march=x86-64 -mtune=generic -Wall -Wextra -D_GNU_SOURCE -I$src/base/include -I$src/base
     -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -DCONFIG_LIBINPUT $(pkg-config --cflags freetype2 libnm libinput)"
libs="$(pkg-config --libs freetype2 libnm libinput) -lm"
for f in "$src"/base/*.c; do
    $gcc -c -o "$b/base-$(basename "$f" .c).o" "$f" # ime.c without CONFIG_IME: no input method here
done
mkdir -p "$root/usr/local/bin" "$root/usr/local/share/locachy/terminus"
$gcc $(pkg-config --cflags json-c) -o "$root/usr/local/bin/installer" "$src/progs/installer.c" "$b"/base-*.o \
    $libs $(pkg-config --libs json-c)
$gcc -o "$root/usr/local/bin/term" "$src/progs/term.c" "$b"/base-*.o $libs
mkdir -p "$root/usr/local/lib/locachy"
$gcc -o "$root/usr/local/lib/locachy/keytable" "$src/progs/keytable.c" $(pkg-config --cflags --libs xkbcommon)

gzip -dc /usr/share/kbd/consolefonts/ter-u16b.psf.gz >"$root/usr/local/share/locachy/font.psf"
for size in 12 14 16 18 20 22 24 28 32; do
    for style in n b; do
        f=/usr/share/kbd/consolefonts/ter-u$size$style.psf.gz
        [ ! -e "$f" ] || gzip -dc "$f" >"$root/usr/local/share/locachy/terminus/ter-u$size$style.psf"
    done
done

# The translations, and their locales: the interface's language is chosen
# by LC_MESSAGES (base/i18n.c), which needs the locale generated. Each is a
# directory, as glibc's own C.utf8: glibc's pacman hook (10-glibc-locale-
# gen.hook) runs locale-gen while the medium is made, which removes the
# archive (locale-archive) and makes what locale.gen asks for (nothing); a
# directory stays. The live system's glibc is this host's (the same
# packages): its format.
mkdir -p "$root/usr/lib/locale"
localedef -i en_US -f UTF-8 "$root/usr/lib/locale/en_US.utf8"
for lang in $(cat "$src/po/LINGUAS"); do
    install -d "$root/usr/local/share/locale/$lang/LC_MESSAGES"
    msgfmt --check -o "$root/usr/local/share/locale/$lang/LC_MESSAGES/locachy.mo" "$src/po/$lang.po"
    localedef -i "$lang" -f UTF-8 "$root/usr/lib/locale/$lang.utf8"
done
