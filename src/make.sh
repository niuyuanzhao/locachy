#!/bin/sh
# make.sh [--boot] LEVEL [CPU]: build our part of the system on the system
# itself, with its own compiler and C library (musl), for CPU (gcc's -march:
# the CPU model the installer found, else the CPU level LEVEL: x86-64,
# x86-64-v2, ...; LEVEL is the packages', for locachy-repos). make-root.sh runs it chrooted in a
# new root; the system can run it again on itself (its sources are next to it).
# Installs, into the root file system (takes effect when a program next starts):
#   /usr/local/bin/home, term, settings, note  the programs
#   /usr/local/bin/lock                        the lock screen (a PAM client)
#   /etc/pam.d/locachy-lock                    its password: a local login
#   /etc/pam.d/locachy-lock-fingerprint        its fingerprint (pam_fprintd)
#   /etc/pam.d/locachy-lock-face               its face (howdy, the IR emitter)
#   /usr/lib/security/pam_locachy_any.so       those for sudo beside the password
#   /usr/local/bin/packages                    installs and updates (locachy-pkg)
#   /usr/local/bin/locachy-seat, locachy-bar   the shared compositor's seat and bar
#   /usr/local/bin/locachy-overview            its windows' overview (the top left corner, Super+W)
#   /usr/local/lib/locachy/gestures            its gestures (libinput-gestures, lisgd: swipes for home, the next and previous task, the overview)
#   /etc/locachy/libinput-gestures.conf        the touchpad's
#   /usr/local/lib/locachy/labwc-control       its trusted client (containers' display)
#   /usr/local/lib/locachy/xdg-open            containers' xdg-open (to locachy-bar)
#   /etc/locachy/labwc/rc.xml                  its configuration
#   /etc/locachy/labwc/xinitrc                 its Xwayland's start (XWAYLAND.md)
#   /usr/local/lib/locachy/link-client         the cable debug link (a service)
#   /usr/local/lib/locachy/user-shell          term's shell, as the user (program_term)
#   /usr/local/lib/locachy/link-connect        its connection, with keepalive
#   /usr/local/lib/locachy/network-client      isolated NetworkManager service
#   /usr/local/lib/locachy/drives              USB drives in ~/Drives (a service)
#   /usr/local/lib/locachy/udev-client         the devices: systemd-udevd as packaged (a service)
#   /usr/local/lib/udev/rules.d/               LOCachy's preset udev rules
#   /usr/local/lib/locachy/ntp-client          network time (a service)
#   /usr/local/lib/locachy/timezone-client     the time zone from the network (a service)
#   /usr/local/share/locachy/font.psf          the font when no other can be read (Terminus 16 bold)
#   /usr/local/lib/locachy/keytable            the key table of a keymap (libxkbcommon)
#   /usr/local/share/locachy/keymap.table      rc.conf keymap's: what each key types
#   /usr/local/share/locachy/terminus/         Terminus 12..32: the text at each scale, and the bar's
#   /etc/locachy/rc.conf                       the startup file (when missing)
#   /etc/os-release                            the system's name: LOCachy
#   /etc/sudoers.d/10-locachy-user             sudo for the user (uid 1000), with its password
#   /usr/local/share/locachy/containers/       presets of containers (NAME.conf)
#   /usr/local/bin/locachy-boot                the boot files, put on the ESP; run
#   /etc/pacman.d/hooks/90-locachy-boot.hook   by pacman for a new kernel or microcode
#   /etc/pacman.d/hooks/91-locachy-rebuild.hook  all of this again (locachy-pkg rebuild),
#                                              by pacman after any package changes
# With --boot, also process 1, and then the boot files (locachy-boot: in
# /usr/local/lib/locachy/boot/, and on the ESP if it is mounted; only needed
# when init/ or base/ as linked into process 1 change; they take effect at
# the next boot):
#   /usr/local/lib/locachy/init                process 1
set -eu

level= cpu=
boot_mode=0
for arg in "$@"; do
    if [ "$arg" = --boot ]; then
        boot_mode=1
    elif [ -z "$level" ]; then
        level=$arg
    elif [ -z "$cpu" ]; then
        cpu=$arg
    fi
done
[ -n "$level" ] || { echo "usage: make.sh [--boot] LEVEL [CPU]" >&2; exit 2; }
cpu=${cpu:-$level}

src=$(cd "$(dirname "$0")" && pwd)
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
lib=/usr/local/lib/locachy
mkdir -p /usr/local/bin "$lib" /usr/local/share/locachy
# For locachy-pkg, which builds it again after updates, and locachy-repos.
echo "$level" >"$lib/level"
echo "$cpu" >"$lib/cpu"

# The kernel's UAPI headers, as installed with the C library (linux-api-
# headers), without the C library's own headers: musl brings its own.
mkdir -p "$b/uapi"
for d in linux asm asm-generic drm sound; do
    ln -s "/usr/include/$d" "$b/uapi/$d"
done
cc="musl-gcc -static -O2 -march=$cpu -Wall -Wextra -I$src/base/include -isystem $b/uapi"

# Process 1 (the switcher) runs in our initramfs before the root partition is
# mounted: linked statically (musl). It is only used through the initramfs.
if [ "$boot_mode" -eq 1 ]; then
    for f in "$src"/base/*.c; do
        # Opening files is the programs' (GIO's content types), not process 1's.
        [ "$(basename "$f")" = "open.c" ] && continue
        $cc -I"$src/base" -c -o "$b/musl-base-$(basename "$f" .c).o" "$f"
    done
    $cc -I"$src/base" -o "$lib/init" "$src/init/init.c" "$src/init/apps.c" "$b"/musl-base-*.o
fi

# FreeType CJK support for programs running in glibc root.
FT_CFLAGS=""
FT_LIBS=""
if pkg-config --exists freetype2; then
    FT_CFLAGS="-DCONFIG_FREETYPE $(pkg-config --cflags freetype2)"
    FT_LIBS="$(pkg-config --libs freetype2)"
fi

NM_CFLAGS="-DCONFIG_NETWORKMANAGER $(pkg-config --cflags libnm)"
NM_LIBS="$(pkg-config --libs libnm)"
# The framebuffer programs' pointer: libinput, as the shared compositor has it.
INPUT_CFLAGS="-DCONFIG_LIBINPUT $(pkg-config --cflags libinput)"
INPUT_LIBS="$(pkg-config --libs libinput) -lm"
FT_LIBS="$FT_LIBS $NM_LIBS $INPUT_LIBS"
GCC="gcc -O2 -march=$cpu -Wall -Wextra -D_GNU_SOURCE -I$src/base/include -I$src/base $FT_CFLAGS $NM_CFLAGS $INPUT_CFLAGS"
IME_CFLAGS=""
IME_LIBS=""
if [ "${IME:-1}" != 0 ] && [ -f /usr/include/rime_api.h ] && [ -f /usr/lib/librime.so ]; then
    IME_CFLAGS="-DCONFIG_IME"
    # Rime's Lua plugin and libinput link different Lua versions, whose
    # functions have the same names: the plugin's comes first, or the schema's
    # Lua scripts get the other one and Rime aborts in a Lua panic. (libinput
    # runs Lua only for plugins, which these programs do not load.)
    rime_lua=$(ldd /usr/lib/rime-plugins/librime-lua.so 2>/dev/null | sed -n 's/^[[:space:]]*\(liblua[^ ]*\) => .*/\1/p' | head -n 1)
    IME_LIBS="-lrime${rime_lua:+ -Wl,--push-state,--no-as-needed -l:$rime_lua -Wl,--pop-state}"
    if [ -x /usr/bin/rime_deployer ]; then
        mkdir -p /var/lib/locachy/rime
        rime_changed=0
        for rf in "$src/base/rime/default.custom.yaml" "$src/base/rime/easy_en.schema.yaml"; do
            dest="/var/lib/locachy/rime/$(basename "$rf")"
            cmp -s "$rf" "$dest" 2>/dev/null || { cp "$rf" "$dest"; rime_changed=1; }
        done
        if [ -f /usr/share/rime-data/rime_frost.schema.yaml ]; then
            dest="/var/lib/locachy/rime/rime_frost.custom.yaml"
            cmp -s "$src/base/rime/rime_frost.custom.yaml" "$dest" 2>/dev/null ||
                { cp "$src/base/rime/rime_frost.custom.yaml" "$dest"; rime_changed=1; }
        elif [ -f /var/lib/locachy/rime/rime_frost.custom.yaml ]; then
            rm -f /var/lib/locachy/rime/rime_frost.custom.yaml
            rime_changed=1
        fi
        if [ ! -f /var/lib/locachy/rime/easy_en.dict.yaml ] && [ -f /usr/share/dict/words ]; then
            python3 "$src/base/rime/build_dict.py" /usr/share/dict/words /var/lib/locachy/rime/easy_en.dict.yaml
        fi
        # Deployed again when its configuration changed (as when it is missing).
        if [ "$rime_changed" = 1 ] || [ ! -f /var/lib/locachy/rime/build/default.yaml ] ||
           [ ! -f /var/lib/locachy/rime/build/easy_en.prism.bin ]; then
            rime_deployer --build /var/lib/locachy/rime /usr/share/rime-data >/dev/null 2>&1 || true
            rime_deployer --compile /var/lib/locachy/rime/easy_en.schema.yaml /var/lib/locachy/rime /usr/share/rime-data >/dev/null 2>&1 || true
            # The Chinese schema ime.c takes first when it is there; not in the
            # schema list, so --build leaves it as it was.
            if [ -f /var/lib/locachy/rime/context_pinyin.schema.yaml ]; then
                rime_deployer --compile /var/lib/locachy/rime/context_pinyin.schema.yaml /var/lib/locachy/rime /usr/share/rime-data >/dev/null 2>&1 || true
            fi
        fi
    fi
fi

for f in "$src"/base/*.c; do
    [ "$(basename "$f")" = "ime.c" ] && continue
    $GCC -c -o "$b/glibc-base-$(basename "$f" .c).o" "$f"
done
$GCC $IME_CFLAGS -c -o "$b/glibc-ime.o" "$src/base/ime.c"

$GCC -o /usr/local/bin/home "$src/progs/home.c" "$src/progs/files.c" "$b"/glibc-base-*.o $FT_LIBS
$GCC $IME_CFLAGS -o /usr/local/bin/term "$src/progs/term.c" "$b"/glibc-base-*.o "$b/glibc-ime.o" $IME_LIBS $FT_LIBS
$GCC -o /usr/local/bin/settings "$src/progs/settings.c" "$src/progs/files.c" "$src/progs/auth.c" \
    "$b"/glibc-base-*.o $FT_LIBS -lpam
$GCC -o /usr/local/bin/lock "$src/progs/lock.c" "$src/progs/auth.c" "$b"/glibc-base-*.o $FT_LIBS -lpam
$GCC -shared -fPIC -o /usr/lib/security/pam_locachy_any.so "$src/progs/pam_locachy_any.c" "$src/progs/auth.c" -lpam
$GCC -o /usr/local/bin/locachy-seat "$src/progs/seat.c" "$b"/glibc-base-*.o $FT_LIBS
$GCC -o /usr/local/bin/note "$src/progs/note.c" "$b"/glibc-base-*.o $FT_LIBS
$GCC -o /usr/local/bin/packages "$src/progs/packages.c" "$src/progs/files.c" "$b"/glibc-base-*.o $FT_LIBS
$GCC -o /usr/local/bin/firmware "$src/progs/firmware.c" "$src/progs/files.c" "$b"/glibc-base-*.o $FT_LIBS
# The archive manager: archive on the screen, archive-worker reading and
# writing the archives as the user in a sandbox (progs/archive-proto.h), with
# libarchive and OpenSSL, which pacman itself needs.
$GCC -o "$lib/archive-worker" "$src/progs/archive-worker.c" $(pkg-config --cflags --libs libarchive libcrypto)
$GCC -o /usr/local/bin/archive "$src/progs/archive.c" "$src/progs/files.c" "$b"/glibc-base-*.o $FT_LIBS
$GCC -o "$lib/drives" "$src/progs/drives.c" "$b"/glibc-base-*.o $FT_LIBS
# The document viewer: view on the screen, view-render reading the documents
# as the user in a sandbox of its own (progs/view-proto.h). Their libraries
# are the manifest's (a system from before it: locachy-pkg install them).
VIEW_PKGS="libspectre libheif libjxl libwebp libwebpdemux libarchive ddjvuapi MagickWand libraw"
if pkg-config --exists $VIEW_PKGS && [ -f /usr/include/mupdf/fitz.h ] && [ -f /usr/include/chm_lib.h ]; then
    $GCC -o "$lib/view-render" "$src/progs/view-render.c" "$src/progs/view-image.c" $(pkg-config --cflags --libs $VIEW_PKGS) -lmupdf -lchm -lm
    $GCC $IME_CFLAGS -o /usr/local/bin/view "$src/progs/view.c" "$src/progs/view-layout.c" "$src/progs/view-client.c" \
        "$src/progs/files.c" "$b"/glibc-base-*.o "$b/glibc-ime.o" $IME_LIBS $FT_LIBS
else
    rm -f /usr/local/bin/view "$lib/view-render"
    echo "make: view not built: libmupdf, djvulibre, libspectre, libheif, libjxl, libwebp, imagemagick, libarchive, libraw, chmlib are not all installed" >&2
fi
# The text editor, with tree-sitter's grammars (dlopen'ed) and the queries
# the grammar packages leave out (edit/queries, their upstream files).
if pkg-config --exists tree-sitter libpcre2-8 json-glib-1.0; then
    $GCC $IME_CFLAGS $(pkg-config --cflags json-glib-1.0) -o /usr/local/bin/edit "$src/progs/edit.c" "$src/progs/edit-buffer.c" "$src/progs/edit-doc.c" \
        "$src/progs/edit-file.c" "$src/progs/edit-search.c" "$src/progs/edit-syntax.c" "$src/progs/edit-tree.c" \
        "$src/progs/edit-layout.c" "$src/progs/edit-lsp.c" "$src/progs/edit-session.c" \
        "$src/progs/edit-filesearch.c" "$src/progs/files.c" "$b"/glibc-base-*.o "$b/glibc-ime.o" $IME_LIBS $FT_LIBS -ltree-sitter -lpcre2-8 $(pkg-config --libs json-glib-1.0)
    rm -rf /usr/local/share/locachy/edit
    install -d /usr/local/share/locachy/edit/queries
    cp -R "$src/edit/queries/." /usr/local/share/locachy/edit/queries/
else
    rm -rf /usr/local/bin/edit /usr/local/share/locachy/edit
    echo "make: edit not built: tree-sitter, pcre2 and json-glib are not all installed" >&2
fi
if [ -f "$src/progs/app-init.c" ]; then
    $GCC -o "$lib/app-init" "$src/progs/app-init.c"
fi
# Mounted into every container as /usr/local/bin/xdg-open: static, so that it
# runs whatever the container's C library.
$cc -o "$lib/xdg-open" "$src/progs/xdg-open.c"

# The shared compositor's host clients: labwc-control (the containers'
# display, window control, screens) and locachy-bar (status bar, input
# method), with the Wayland protocols they speak (built when the desktop
# module's Wayland libraries are installed).
if command -v wayland-scanner >/dev/null 2>&1 && pkg-config --exists wayland-client wayland-cursor xkbcommon libpng; then
    for p in security-context-v1 wlr-foreign-toplevel-management-unstable-v1 \
             wlr-output-management-unstable-v1 wlr-layer-shell-unstable-v1 \
             virtual-keyboard-unstable-v1 xdg-shell fractional-scale-v1 viewporter ext-data-control-v1 \
             ext-idle-notify-v1 relative-pointer-unstable-v1 ext-foreign-toplevel-list-v1 \
             ext-image-capture-source-v1 ext-image-copy-capture-v1; do
        wayland-scanner client-header "$src/base/protocols/$p.xml" "$b/$p-client-protocol.h"
        wayland-scanner private-code "$src/base/protocols/$p.xml" "$b/$p-protocol.c"
        gcc -O2 -march=$cpu -c -o "$b/proto-$p.o" "$b/$p-protocol.c"
    done
    $GCC -I"$b" -o "$lib/clipboard" "$src/progs/clipboard.c" "$b/glibc-base-clipboard.o" \
        "$b/proto-ext-data-control-v1.o" -lwayland-client
    $GCC -I"$b" -o "$lib/labwc-control" "$src/progs/labwc-control.c" \
        "$b/proto-security-context-v1.o" "$b/proto-wlr-foreign-toplevel-management-unstable-v1.o" \
        "$b/proto-wlr-output-management-unstable-v1.o" "$b/proto-ext-idle-notify-v1.o" "$b"/glibc-base-*.o \
        $FT_LIBS -lwayland-client -lm
    $GCC $IME_CFLAGS -I"$b" -o /usr/local/bin/locachy-bar "$src/progs/bar.c" \
        "$b/proto-wlr-layer-shell-unstable-v1.o" "$b/proto-virtual-keyboard-unstable-v1.o" \
        "$b/proto-xdg-shell.o" "$b/proto-wlr-foreign-toplevel-management-unstable-v1.o" \
        "$b/proto-fractional-scale-v1.o" "$b/proto-viewporter.o" "$b/proto-relative-pointer-unstable-v1.o" \
        "$b"/glibc-base-*.o "$b/glibc-ime.o" $IME_LIBS $FT_LIBS \
        -lwayland-client -lwayland-cursor -lxkbcommon
    $GCC -I"$b" -o /usr/local/bin/locachy-overview "$src/progs/overview.c" \
        "$b/proto-wlr-layer-shell-unstable-v1.o" "$b/proto-wlr-foreign-toplevel-management-unstable-v1.o" \
        "$b/proto-ext-foreign-toplevel-list-v1.o" "$b/proto-ext-image-capture-source-v1.o" \
        "$b/proto-ext-image-copy-capture-v1.o" "$b/proto-fractional-scale-v1.o" "$b/proto-viewporter.o" \
        "$b/proto-xdg-shell.o" "$b"/glibc-base-*.o $FT_LIBS \
        -lwayland-client -lwayland-cursor -lxkbcommon -lm
    $GCC -I"$b" -o /usr/local/bin/locachy-shot "$src/progs/shot.c" \
        "$b/proto-wlr-layer-shell-unstable-v1.o" "$b/proto-wlr-foreign-toplevel-management-unstable-v1.o" \
        "$b/proto-wlr-output-management-unstable-v1.o" "$b/proto-ext-foreign-toplevel-list-v1.o" \
        "$b/proto-ext-image-capture-source-v1.o" "$b/proto-ext-image-copy-capture-v1.o" \
        "$b/proto-fractional-scale-v1.o" "$b/proto-viewporter.o" \
        "$b/proto-xdg-shell.o" "$b"/glibc-base-*.o $FT_LIBS \
        -lwayland-client -lwayland-cursor -lxkbcommon -lpng -lm
else
    rm -f "$lib/clipboard" "$lib/labwc-control" /usr/local/bin/locachy-bar \
        /usr/local/bin/locachy-overview /usr/local/bin/locachy-shot
    echo "make: Wayland clients not built (desktop module not installed)" >&2
fi
if pkg-config --exists libcryptsetup json-c openssl; then
    install -d -m 755 /usr/lib/cryptsetup
    gcc -O2 -march="$cpu" -Wall -Wextra -D_GNU_SOURCE -shared -fPIC \
        $(pkg-config --cflags libcryptsetup json-c openssl) \
        "$src/progs/token-tpm12.c" \
        -Wl,--version-script="$src/progs/token-tpm12.sym" \
        $(pkg-config --libs libcryptsetup json-c openssl) \
        -o /usr/lib/cryptsetup/libcryptsetup-token-locachy-tpm12.so
fi

# The startup file and package manifest with their defaults, unless there is one already.
[ -e /etc/locachy/rc.conf ] || install -D -m 644 "$src/rc.conf" /etc/locachy/rc.conf
# The system's name, in place of the (unowned) symlink to Arch's.
rm -f /etc/os-release
install -m 644 "$src/link/os-release" /etc/os-release
# The D-Bus buses, services of their own (bus-client): an rc.conf from before,
# when audio-client and network-client started them in passing, gets them.
for bus in session system; do
    if ! grep -q "^[[:space:]]*service_${bus}_bus=" /etc/locachy/rc.conf; then
        printf 'service_%s_bus="/usr/local/lib/locachy/bus-client %s"\n' "$bus" "$bus" >>/etc/locachy/rc.conf
        echo "make: rc.conf: service_${bus}_bus added"
    fi
done
if [ -x "$lib/clipboard" ]; then
    if ! grep -q '^[[:space:]]*service_clipboard=' /etc/locachy/rc.conf; then
        printf 'service_clipboard="/usr/local/lib/locachy/clipboard --serve"\n' >>/etc/locachy/rc.conf
        echo "make: rc.conf: service_clipboard added"
    fi
elif grep -q '^[[:space:]]*service_clipboard=' /etc/locachy/rc.conf; then
    sed -i '/^[[:space:]]*service_clipboard=/d' /etc/locachy/rc.conf
fi
if ! grep -q '^[[:space:]]*program_firmware=' /etc/locachy/rc.conf; then
    printf 'program_firmware="/usr/local/bin/firmware"\n' >>/etc/locachy/rc.conf
    echo "make: rc.conf: program_firmware added"
fi
# The framebuffer programs that open files (open.c's program:NAME): their
# .desktop files, and home's entries in an rc.conf from before them.
rm -rf /usr/local/share/applications/locachy
install -d /usr/local/share/applications/locachy
for program in view edit archive; do
    if [ ! -x "/usr/local/bin/$program" ]; then
        sed -i "/^[[:space:]]*program_${program}=/d" /etc/locachy/rc.conf
        continue
    fi
    install -m 644 "$src/link/applications/$program.desktop" /usr/local/share/applications/locachy/
    if ! grep -q "^[[:space:]]*program_$program=" /etc/locachy/rc.conf; then
        printf 'program_%s="/usr/local/bin/%s"\n' "$program" "$program" >>/etc/locachy/rc.conf
        echo "make: rc.conf: program_$program added"
    fi
done
if [ -x /usr/bin/bluetui ] && ! grep -q '^[[:space:]]*program_bluetooth=' /etc/locachy/rc.conf; then
    printf 'program_bluetooth="/usr/local/bin/term /usr/bin/bluetui"\n' >>/etc/locachy/rc.conf
    echo "make: rc.conf: program_bluetooth added"
fi
[ -e /etc/locachy/packages ] || install -D -m 644 "$src/packages" /etc/locachy/packages
python3 -I "$src/link/migrate-audio-packages" /etc/locachy/packages
python3 -I "$src/link/declare-packages" "$src" /etc/locachy/packages
# Containers: their configurations (/etc/locachy/containers/NAME.conf, the
# user's) and the presets to choose from (ours, replaced each time).
mkdir -p /etc/locachy/containers
rm -rf /usr/local/share/locachy/containers
install -d /usr/local/share/locachy/containers
install -m 644 "$src"/containers/*.conf /usr/local/share/locachy/containers/
if [ -d "$src/machines" ]; then
    rm -rf /usr/local/share/locachy/machines
    install -d /usr/local/share/locachy/machines
    install -m 644 "$src"/machines/*.conf /usr/local/share/locachy/machines/
fi
# Host programs (host="1"): their packages are the host's (the manifest), so
# their configurations come with the system once the package is there, unless
# the user has one.
for preset in "$src"/containers/*.conf; do
    grep -q '^host="1"' "$preset" || continue
    package=$(sed -n 's/^package="\(.*\)"$/\1/p' "$preset")
    if [ ! -e "/etc/locachy/containers/${preset##*/}" ] && pacman -Q "$package" >/dev/null 2>&1; then
        install -m 644 "$preset" /etc/locachy/containers/
        echo "make: host program ${preset##*/}"
    fi
done
# A container made before its configuration had a file of its own: its cmd,
# net and packages (in /var/lib/locachy/containers/NAME) become NAME.conf.
for d in /var/lib/locachy/containers/*; do
    name=${d##*/}
    conf=/etc/locachy/containers/$name.conf
    if [ ! -f "$d/cmd" ] || [ -e "$conf" ]; then
        continue
    fi
    {
        printf '# %s: made from its cmd, net and packages\n' "$name"
        printf 'command="%s"\n' "$(head -n 1 "$d/cmd")"
        if [ "$(cat "$d/net" 2>/dev/null)" = 1 ]; then
            printf 'network="1"\n'
        fi
        if [ -f "$d/packages" ]; then
            printf 'packages="%s"\n' "$(tr '\n' ' ' <"$d/packages" | sed 's/ *$//')"
        fi
    } >"$conf.new"
    mv "$conf.new" "$conf"
    rm -f "$d/cmd" "$d/net" "$d/packages"
    echo "make: container $name: its configuration is now $conf"
done
# A configuration from before package, extras and arguments: its packages
# become package (the first) and extras (the others), a command with
# arguments becomes command and arguments.
migrate_conf() { # CONF
    if grep -q '^packages=' "$1" && ! grep -q '^package=' "$1"; then
        words=$(sed -n 's/^packages="\(.*\)"$/\1/p' "$1" | tail -n 1)
        first=${words%% *}
        rest=""
        [ "$first" = "$words" ] || rest=${words#* }
        { grep -v '^packages=' "$1"; printf 'package="%s"\nextras="%s"\n' "$first" "$rest"; } >"$1.new"
        mv "$1.new" "$1"
        echo "make: $1: packages are now package ($first) and extras ($rest)"
    fi
    if ! grep -q '^arguments=' "$1"; then
        words=$(sed -n 's/^command="\(.*\)"$/\1/p' "$1" | tail -n 1)
        first=${words%% *}
        if [ "$first" != "$words" ]; then
            { grep -v '^command=' "$1"; printf 'command="%s"\narguments="%s"\n' "$first" "${words#* }"; } >"$1.new"
            mv "$1.new" "$1"
            echo "make: $1: its command is now command ($first) and arguments (${words#* })"
        fi
    fi
    # Repair the shipped Edge default, never replace custom arguments.
    if grep -qx 'package="microsoft-edge-stable-bin"' "$1" &&
       grep -qx 'command="/usr/bin/microsoft-edge-stable"' "$1" &&
       { ! grep -q '^arguments=' "$1" || grep -qx 'arguments=""' "$1"; } &&
       ! grep '^arguments=' "$1" | grep -qv '^arguments=""$'; then
        { grep -v '^arguments=' "$1"; printf 'arguments="--ozone-platform=wayland"\n'; } >"$1.new"
        mv "$1.new" "$1"
        echo "make: $1: Edge uses the native Wayland platform"
    fi
}
for conf in /etc/locachy/containers/*.conf; do
    if [ -f "$conf" ]; then
        migrate_conf "$conf"
    fi
done
install -m 644 "$src/base/rime/mode-labels.conf" /usr/local/share/locachy/ime-mode-labels.conf
# The interface's translations: each catalog of po/LINGUAS,
# checked and compiled, and its locale generated as the Arch Wiki's Locale
# page does it (locale.gen, locale-gen): the programs choose the language by
# LC_MESSAGES, which glibc's gettext needs to be a locale that is there.
rm -rf /usr/local/share/locachy/locale
if [ -f "$src/po/LINGUAS" ]; then
for lang in $(cat "$src/po/LINGUAS"); do
    install -d -m 755 "/usr/local/share/locale/$lang/LC_MESSAGES"
    msgfmt --check -o "/usr/local/share/locale/$lang/LC_MESSAGES/locachy.mo" "$src/po/$lang.po"
    if ! locale -a | grep -qix "$lang\.utf-\?8"; then
        sed -i "s/^#[[:space:]]*\($lang\.UTF-8 UTF-8\)/\1/" /etc/locale.gen
        grep -q "^$lang\.UTF-8 UTF-8" /etc/locale.gen || echo "$lang.UTF-8 UTF-8" >>/etc/locale.gen
        locale-gen
    fi
done
fi
# No shell script is installed that bash cannot read (a quote in a comment
# of an inner script would leave locachy-app unable to start containers).
for script in "$src"/link/*; do
    [ -f "$script" ] || continue # link/pam.d
    if head -n 1 "$script" | grep -q '^#!.*\(ba\)\?sh'; then
        bash -n "$script" || { echo "make: $script: syntax error; nothing of link/ installed" >&2; exit 1; }
    fi
done
install -m 755 "$src/link/link-client" "$lib/link-client"
install -m 755 "$src/link/user-shell" "$lib/user-shell"
install -m 755 "$src/link/network-client" "$lib/network-client"
install -m 755 "$src/link/audio-client" "$lib/audio-client"
install -m 755 "$src/link/bluetooth-client" "$lib/bluetooth-client"
install -D -m 644 "$src/link/50-locachy-bluetooth.conf" /etc/wireplumber/wireplumber.conf.d/50-locachy-bluetooth.conf
install -m 755 "$src/link/tailscale-client" "$lib/tailscale-client"
install -m 755 "$src/link/clash-client" "$lib/clash-client"
install -m 755 "$src/link/tor-client" "$lib/tor-client"
install -m 755 "$src/link/tor-enforce" "$lib/tor-enforce"
install -m 755 "$src/link/rclone-client" "$lib/rclone-client"
install -m 755 "$src/link/locachy-tpm" /usr/local/bin/locachy-tpm
install -m 755 "$src/link/locachy-face-camera" /usr/local/bin/locachy-face-camera
install -m 755 "$src/link/locachy-firmware" /usr/local/bin/locachy-firmware
install -m 755 "$src/link/reboot" /usr/local/bin/reboot
install -m 755 "$src/link/poweroff" /usr/local/bin/poweroff
install -m 755 "$src/link/udev-client" "$lib/udev-client"
install -m 755 "$src/link/bus-client" "$lib/bus-client"
install -D -m 644 "$src/link/90-locachy-network.rules" /usr/local/lib/udev/rules.d/90-locachy-network.rules
install -D -m 644 "$src/link/91-locachy-cameras.rules" /usr/local/lib/udev/rules.d/91-locachy-cameras.rules
# Screen sharing in the containers (locachy-app starts the backend): which
# portal backend answers, with the desktop module's xdg-desktop-portal-wlr.
if [ -x /usr/lib/xdg-desktop-portal-wlr ]; then
    install -D -m 644 "$src/link/portals.conf" /etc/xdg/xdg-desktop-portal/portals.conf
else
    rm -f /etc/xdg/xdg-desktop-portal/portals.conf
fi
install -m 755 "$src/link/ntp-client" "$lib/ntp-client"
install -m 755 "$src/link/timezone-client" "$lib/timezone-client"
install -D -m 644 "$src/link/NetworkManager.conf" /etc/NetworkManager/conf.d/30-locachy-network.conf
sh "$src/link/install-pkg-tools" "$src"
# sudo for the user (uid 1000), with its own password. By name: its programs
# run without supplementary groups (app-init), so %wheel would not match there.
user=$(getent passwd 1000 | cut -d: -f1 || true)
if [ -n "$user" ]; then
    printf '%s ALL=(ALL:ALL) ALL\n' "$user" >/etc/sudoers.d/10-locachy-user.new # a dot: not read yet
    chmod 0440 /etc/sudoers.d/10-locachy-user.new
    if visudo -cqf /etc/sudoers.d/10-locachy-user.new; then
        mv /etc/sudoers.d/10-locachy-user.new /etc/sudoers.d/10-locachy-user
    else
        rm -f /etc/sudoers.d/10-locachy-user.new
        echo "make: sudo for $user not set (visudo refused it)" >&2
    fi
fi
# Before it: one overlay kept from build to build, as nobody, whose home is /
# (pikaur's leaked onto the root).
rm -rf /var/lib/locachy/builder/upper /var/lib/locachy/builder/work /var/lib/locachy/builder/merged \
    /var/lib/locachy/builder/host
for d in /.cache/pikaur /root/.cache/pikaur; do
    if [ -d "$d" ] && [ "$(stat -c %U "$d")" = nobody ]; then
        rm -rf "$d"
    fi
done
# Before process 1 set HOME (/root): what root's programs kept in / as theirs.
rm -rf /.cache/mesa_shader_cache /.cache/dconf /.config/pikaur.conf /.config/pulse
rmdir /.cache /.config 2>/dev/null || true
install -m 755 "$src/link/locachy-app" /usr/local/bin/locachy-app
install -m 755 "$src/link/code-oss" "$lib/code-oss"
# What host programs find first on their PATH: the xdg-open that hands paths
# and URLs to locachy-bar.
install -d "$lib/host-bin"
ln -sfn ../xdg-open "$lib/host-bin/xdg-open"
install -m 755 "$src/link/locachy-net" /usr/local/bin/locachy-net
install -m 755 "$src/link/locachy-setup" /usr/local/bin/locachy-setup
install -m 755 "$src/link/locachy-boot" /usr/local/bin/locachy-boot
install -m 755 "$src/link/locachy-efi" /usr/local/bin/locachy-efi
install -D -m 644 "$src/link/90-locachy-boot.hook" /etc/pacman.d/hooks/90-locachy-boot.hook
install -D -m 644 "$src/link/pam.d/locachy-lock" /etc/pam.d/locachy-lock
install -D -m 644 "$src/link/pam.d/locachy-lock-fingerprint" /etc/pam.d/locachy-lock-fingerprint
install -D -m 644 "$src/link/pam.d/locachy-lock-face" /etc/pam.d/locachy-lock-face
install -D -m 644 "$src/link/91-locachy-rebuild.hook" /etc/pacman.d/hooks/91-locachy-rebuild.hook
gcc -O2 -o "$lib/gen_init_cpio" "$src/tools/gen_init_cpio.c" # the initramfs (locachy-boot)
# The shared compositor's configuration (locachy-seat sets XDG_CONFIG_HOME).
# The shared display's gestures (libinput-gestures, lisgd), which locachy-seat
# runs while it is on the screen.
install -m 755 "$src/link/gestures" "$lib/gestures"
install -D -m 644 "$src/link/libinput-gestures.conf" /etc/locachy/libinput-gestures.conf
install -D -m 644 "$src/link/labwc-rc.xml" /etc/locachy/labwc/rc.xml
# Run by labwc whenever it starts Xwayland: the user admitted, the display published.
install -D -m 644 "$src/link/labwc-xinitrc" /etc/locachy/labwc/xinitrc
# Each container's own session bus (locachy-app), with locachy-bar its tray.
install -D -m 644 "$src/link/container-bus.conf" /usr/local/share/locachy/container-bus.conf
rm -f /etc/locachy/labwc/menu.xml
gcc -O2 -Wall -Wextra -o "$lib/link-connect" "$src/link/link-connect.c"
# The framebuffer programs' text is Terminus of the screen's scale (screen.c);
# font.psf, the size at scale 1, is used when those cannot be read, as in the
# initramfs (its copy there belongs to --boot).
gzip -dc /usr/share/kbd/consolefonts/ter-u16b.psf.gz >/usr/local/share/locachy/font.psf
# Every size: the text at each scale, and what fits the bar (panel.c).
mkdir -p /usr/local/share/locachy/terminus
for size in 12 14 16 18 20 22 24 28 32; do
    for style in n b; do
        f=/usr/share/kbd/consolefonts/ter-u$size$style.psf.gz
        [ ! -e "$f" ] || gzip -dc "$f" >"/usr/local/share/locachy/terminus/ter-u$size$style.psf"
    done
done
rm -f /usr/local/share/locachy/font-small.psf /usr/local/share/locachy/font-bar.psf
# The key table of rc.conf's keymap (keytable): what each key types in the
# direct programs, the lock and process 1's passphrase prompt (in the
# initramfs, as the font: its copy there belongs to --boot), and the layout
# locachy-seat gives the shared display. Its dead keys' sequences are the
# compose table of the system's locale, as the shared display's programs
# have them. A keymap it cannot read types US.
$GCC -o "$lib/keytable" "$src/progs/keytable.c" $(pkg-config --cflags --libs xkbcommon)
keymap=$(sed -n 's/^[[:space:]]*keymap="\{0,1\}\([^"]*\)"\{0,1\}[[:space:]]*$/\1/p' /etc/locachy/rc.conf | tail -n 1)
lang=$(sed -n 's/^LANG="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' /etc/locale.conf 2>/dev/null | tail -n 1)
if ! LC_ALL=${lang:-C} "$lib/keytable" "${keymap:-us}" /usr/local/share/locachy/keymap.table; then
    rm -f /usr/local/share/locachy/keymap.table
    echo "make: no key table for keymap ${keymap:-us}: keys type US" >&2
fi
# The tables of the keymaps keyboards have of their own (settings writes
# keyboards.conf; keys.c reads it), made again as the system's is. A keyboard
# whose table cannot be made types as the system's (keys.c says so).
mkdir -p /usr/local/share/locachy/keymaps
rm -f /usr/local/share/locachy/keymaps/*.table
if [ -f /etc/locachy/keyboards.conf ]; then
    for k in $(awk '!/^#/ && NF >= 2 { print $2 }' /etc/locachy/keyboards.conf | sort -u); do
        case $k in .*|*[!A-Za-z0-9._-]*) continue ;; esac
        LC_ALL=${lang:-C} "$lib/keytable" "$k" "/usr/local/share/locachy/keymaps/$k.table" ||
            echo "make: no key table for keymap $k (keyboards.conf)" >&2
    done
fi

if [ "$boot_mode" -eq 1 ]; then
    /usr/local/bin/locachy-boot
    # What all of it is built against: the packages as locachy-pkg hashes
    # them (host_hash), so that it builds again only when they change.
    pacman -Q | sha256sum | cut -c1-16 >"$lib/built-host"
    rm -f "$lib/source-unbuilt" # built from the installed source (locachy-pkg apply)
    echo "make: $level: init, home, term, settings, note; boot files made"
else
    echo "make: $level: home, term, settings, note and services (process 1 and boot files unchanged)"
fi
