#!/bin/sh
# make-live.sh SOURCE ROOT: the live medium's own programs, built on the
# ISO's build host (the same Arch packages as the medium's) for any x86-64,
# into its root file system ROOT (archiso's airootfs), as make.sh builds and
# installs them on a system:
#   /usr/local/bin/installer             the full-screen installation (progs/installer.c)
#   /usr/local/bin/rescue                the independent rescue interface (progs/rescue.c)
#   /usr/local/bin/term                  its terminal: a root shell, for repairs
#   /usr/local/lib/locachy/keytable      the key table of the layout chosen (libxkbcommon)
#   /usr/local/lib/locachy/clipboard     the clipboard they copy and paste with (its socket)
#   /usr/local/share/locachy/font.psf, terminus/   their text (Terminus)
#   /etc/fonts/conf.d/64-locachy-cjk-prefer.conf    Han text's CJK variant (tools/FONTS.md)
#   /usr/local/share/locale/LL_CC/       their translations (po/LINGUAS)
#   /usr/lib/locale/LL_CC.utf8/          the locales those need (localedef)
set -eu
src=$1
root=$2
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT

# As make.sh's $GCC, for the CPU level every x86-64 runs.
sec_flags="-ffunction-sections -fdata-sections"
gc_ldflags="-Wl,--gc-sections -Wl,--as-needed"
# gio-2.0 is named explicitly: libnm's headers include GIO, and status.c's BlueZ
# client uses GDBus. open.c (the content-type rules) is not in the live medium.
gcc="gcc -O2 -march=x86-64 -mtune=generic $sec_flags -Wall -Wextra -D_GNU_SOURCE -I$src/base/include -I$src/base
     -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -DCONFIG_LIBINPUT $(pkg-config --cflags freetype2 fontconfig libnm libinput gio-2.0)"
libs="$(pkg-config --libs freetype2 fontconfig libnm libinput gio-2.0) -lm"
for f in "$src"/base/*.c; do
    # Container MIME/open rules (GIO) and locachy-jobs client are not used in the live medium.
    case "$(basename "$f")" in open.c | jobs.c) continue ;; esac
    $gcc -c -o "$b/base-$(basename "$f" .c).o" "$f" # ime.c without CONFIG_IME: no input method here
done
ar rcs "$b/liblocachy-live.a" "$b"/base-*.o
# Weak hooks of the base resolved from the archive (as make.sh's HOOK_THEME/HOOK_BASE/HOOK_BAR).
hook_theme="-Wl,-u,theme_get -Wl,-u,theme_reload"
hook_bar="-Wl,-u,base_status_fields"
hook_raw="-Wl,-u,base_raw_cancel -Wl,-u,base_raw_emit -Wl,-u,base_raw_forward_home -Wl,-u,base_raw_pending"
hook_data="-Wl,-u,wl_data_active -Wl,-u,wl_data_clipboard_get -Wl,-u,wl_data_clipboard_set -Wl,-u,wl_data_clipboard_clear -Wl,-u,wl_data_drag_active -Wl,-u,wl_data_drag_start -Wl,-u,wl_data_drag_start_key -Wl,-u,wl_data_press_serial"
live_hooks="$hook_theme $hook_raw $hook_data $hook_bar"
mkdir -p "$root/usr/local/bin" "$root/usr/local/share/locachy/terminus"
$gcc $gc_ldflags $(pkg-config --cflags json-c) -o "$root/usr/local/bin/installer" "$src/progs/installer.c" \
    $live_hooks "$b/liblocachy-live.a" $libs $(pkg-config --libs json-c)
$gcc $gc_ldflags -o "$root/usr/local/bin/term" "$src/progs/term.c" \
    $live_hooks "$b/liblocachy-live.a" $libs
$gcc $gc_ldflags $(pkg-config --cflags json-c) -o "$root/usr/local/bin/rescue" "$src/progs/rescue.c" \
    $live_hooks "$b/liblocachy-live.a" $libs $(pkg-config --libs json-c)
mkdir -p "$root/usr/local/lib/locachy"
$gcc $gc_ldflags -o "$root/usr/local/lib/locachy/keytable" "$src/progs/keytable.c" $(pkg-config --cflags --libs xkbcommon)
p=ext-data-control-v1
wayland-scanner client-header "$src/base/protocols/$p.xml" "$b/$p-client-protocol.h"
wayland-scanner private-code "$src/base/protocols/$p.xml" "$b/$p-protocol.c"
$gcc $gc_ldflags -I"$b" -o "$root/usr/local/lib/locachy/clipboard" "$src/progs/clipboard.c" "$b/$p-protocol.c" \
    "$b/base-clipboard.o" -lwayland-client

gzip -dc /usr/share/kbd/consolefonts/ter-u16b.psf.gz >"$root/usr/local/share/locachy/font.psf"
# Han text's Noto CJK variant, as on an installed system (make.sh, tools/FONTS.md).
install -D -m 644 "$src/link/64-locachy-cjk-prefer.conf" "$root/etc/fonts/conf.d/64-locachy-cjk-prefer.conf"
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
