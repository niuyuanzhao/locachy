#!/bin/sh
# Build candidates without replacing the running host or its boot image.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: build-check.sh OUTPUT_DIR}
mkdir -p "$out/obj" "$out/uapi"
for name in linux asm asm-generic drm sound; do
    ln -sfn "/usr/include/$name" "$out/uapi/$name"
done
for file in "$src"/base/*.c; do
    name=$(basename "$file" .c)
    [ "$name" != open ] || continue # GIO's, the programs' only (make.sh)
    musl-gcc -static -O2 -Wall -Wextra -I"$src/base/include" -I"$src/base" \
        -isystem "$out/uapi" -c "$file" -o "$out/obj/musl-$name.o"
done
musl-gcc -static -O2 -Wall -Wextra -I"$src/base/include" -I"$src/base" \
    -isystem "$out/uapi" "$src/init/init.c" "$src/init/apps.c" \
    "$out"/obj/musl-*.o -o "$out/init"

flags="$(pkg-config --cflags freetype2 libnm libinput) -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -DCONFIG_LIBINPUT"
libs="$(pkg-config --libs freetype2 libnm libinput) -lm"
for file in "$src"/base/*.c; do
    name=$(basename "$file" .c)
    [ "$name" != ime ] || continue
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $flags \
        -I"$src/base/include" -I"$src/base" -c "$file" -o "$out/obj/base-$name.o"
done
ime_flags= ime_libs=
if [ -f /usr/include/rime_api.h ] && [ -f /usr/lib/librime.so ]; then
    ime_flags=-DCONFIG_IME
    # Rime's Lua plugin's Lua before libinput's, as make.sh links it.
    rime_lua=$(ldd /usr/lib/rime-plugins/librime-lua.so 2>/dev/null | sed -n 's/^[[:space:]]*\(liblua[^ ]*\) => .*/\1/p' | head -n 1)
    ime_libs="-lrime${rime_lua:+ -Wl,--push-state,--no-as-needed -l:$rime_lua -Wl,--pop-state}"
fi
gcc -O2 -Wall -Wextra -Werror $ime_flags -I"$src/base/include" \
    -c "$src/base/ime.c" -o "$out/obj/ime.o"
for name in note seat; do
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $flags \
        -I"$src/base/include" -I"$src/base" "$src/progs/$name.c" \
        "$out"/obj/base-*.o $libs -o "$out/$name"
done
# The live medium's full-screen installer (installer/make-live.sh).
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $flags $(pkg-config --cflags json-c) \
    -I"$src/base/include" -I"$src/base" "$src/progs/installer.c" \
    "$out"/obj/base-*.o $libs $(pkg-config --libs json-c) -o "$out/installer"
for name in home packages firmware archive; do
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $flags \
        -I"$src/base/include" -I"$src/base" "$src/progs/$name.c" "$src/progs/files.c" \
        "$out"/obj/base-*.o $libs -o "$out/$name"
done
# The archive manager's worker: libarchive (pacman's) and OpenSSL's hashes.
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE "$src/progs/archive-worker.c" \
    $(pkg-config --cflags --libs libarchive libcrypto) -o "$out/archive-worker"
# The key table of a keymap: libxkbcommon (make.sh, installer/make-live.sh).
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE "$src/progs/keytable.c" \
    $(pkg-config --cflags --libs xkbcommon) -o "$out/keytable"
# The document viewer, where its libraries are installed (make.sh).
view_pkgs="libspectre libheif libjxl libwebp libwebpdemux libarchive ddjvuapi MagickWand libraw"
if pkg-config --exists $view_pkgs && [ -f /usr/include/mupdf/fitz.h ] && [ -f /usr/include/chm_lib.h ]; then
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE "$src/progs/view-render.c" "$src/progs/view-image.c" \
        $(pkg-config --cflags --libs $view_pkgs) -lmupdf -lchm -lm -o "$out/view-render"
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $ime_flags $flags \
        -I"$src/base/include" -I"$src/base" "$src/progs/view.c" "$src/progs/view-layout.c" \
        "$src/progs/view-client.c" "$src/progs/files.c" "$out"/obj/base-*.o "$out/obj/ime.o" \
        $ime_libs $libs -o "$out/view"
fi
# The text editor, where tree-sitter and pcre2 are installed (make.sh).
if pkg-config --exists tree-sitter libpcre2-8 json-glib-1.0; then
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $ime_flags $flags \
        $(pkg-config --cflags json-glib-1.0) -I"$src/base/include" -I"$src/base" "$src/progs/edit.c" "$src/progs/edit-buffer.c" \
        "$src/progs/edit-doc.c" "$src/progs/edit-file.c" "$src/progs/edit-search.c" "$src/progs/edit-syntax.c" \
        "$src/progs/edit-tree.c" "$src/progs/edit-layout.c" "$src/progs/edit-lsp.c" \
        "$src/progs/edit-session.c" "$src/progs/edit-filesearch.c" "$src/progs/files.c" "$out"/obj/base-*.o "$out/obj/ime.o" $ime_libs $libs \
        -ltree-sitter -lpcre2-8 $(pkg-config --libs json-glib-1.0) -o "$out/edit"
fi
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $flags \
    -I"$src/base/include" -I"$src/base" "$src/progs/settings.c" "$src/progs/files.c" "$src/progs/auth.c" \
    "$out"/obj/base-*.o $libs -lpam -o "$out/settings"
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $flags \
    -I"$src/base/include" -I"$src/base" "$src/progs/lock.c" "$src/progs/auth.c" \
    "$out"/obj/base-*.o $libs -lpam -o "$out/lock"
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -shared -fPIC "$src/progs/pam_locachy_any.c" "$src/progs/auth.c" \
    -lpam -o "$out/pam_locachy_any.so"
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $ime_flags $flags \
    -I"$src/base/include" "$src/progs/term.c" "$out"/obj/base-*.o \
    "$out/obj/ime.o" $ime_libs $libs -o "$out/term"
printf '%s\n' "$ime_libs" >"$out/ime-libs"
if [ -f "$src/progs/app-init.c" ]; then
    gcc -O2 -Wall -Wextra -Werror "$src/progs/app-init.c" -o "$out/app-init"
fi
if command -v wayland-scanner >/dev/null 2>&1 && pkg-config --exists wayland-client wayland-cursor xkbcommon libpng; then
    wayland-scanner client-header "$src/base/protocols/ext-data-control-v1.xml" "$out/ext-data-control-v1-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/ext-data-control-v1.xml" "$out/ext-data-control-v1-protocol.c"
    gcc -O2 -Wall -Wextra -Werror -I"$src/base/include" -I"$out" \
        "$src/progs/clipboard.c" "$src/base/clipboard.c" "$out/ext-data-control-v1-protocol.c" \
        -lwayland-client -o "$out/clipboard"
    # The shared compositor's bar, overview and screenshot tool, with the protocols they speak.
    wl="wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 xdg-shell wlr-foreign-toplevel-management-unstable-v1
        wlr-output-management-unstable-v1 fractional-scale-v1 viewporter relative-pointer-unstable-v1
        ext-foreign-toplevel-list-v1 ext-image-capture-source-v1 ext-image-copy-capture-v1"
    for p in $wl; do
        wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/$p-client-protocol.h"
        wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/$p-protocol.c"
    done
    proto() { for p in "$@"; do printf '%s ' "$out/$p-protocol.c"; done; }
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $ime_flags $flags \
        -I"$src/base/include" -I"$src/base" -I"$out" "$src/progs/bar.c" \
        $(proto wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 xdg-shell \
            wlr-foreign-toplevel-management-unstable-v1 fractional-scale-v1 viewporter relative-pointer-unstable-v1) \
        "$out"/obj/base-*.o "$out/obj/ime.o" $ime_libs $libs \
        -lwayland-client -lwayland-cursor -lxkbcommon -o "$out/locachy-bar"
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $flags \
        -I"$src/base/include" -I"$src/base" -I"$out" "$src/progs/overview.c" \
        $(proto wlr-layer-shell-unstable-v1 wlr-foreign-toplevel-management-unstable-v1 ext-foreign-toplevel-list-v1 \
            ext-image-capture-source-v1 ext-image-copy-capture-v1 fractional-scale-v1 viewporter xdg-shell) \
        "$out"/obj/base-*.o $libs -lwayland-client -lwayland-cursor -lxkbcommon -lm -o "$out/locachy-overview"
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $flags \
        -I"$src/base/include" -I"$src/base" -I"$out" "$src/progs/shot.c" \
        $(proto wlr-layer-shell-unstable-v1 wlr-foreign-toplevel-management-unstable-v1 wlr-output-management-unstable-v1 \
            ext-foreign-toplevel-list-v1 ext-image-capture-source-v1 ext-image-copy-capture-v1 \
            fractional-scale-v1 viewporter xdg-shell) \
        "$out"/obj/base-*.o $libs -lwayland-client -lwayland-cursor -lxkbcommon -lpng -lm -o "$out/locachy-shot"
fi
if pkg-config --exists libcryptsetup json-c openssl; then
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -shared -fPIC \
        $(pkg-config --cflags libcryptsetup json-c openssl) \
        "$src/progs/token-tpm12.c" \
        -Wl,--version-script="$src/progs/token-tpm12.sym" \
        $(pkg-config --libs libcryptsetup json-c openssl) \
        -o "$out/libcryptsetup-token-locachy-tpm12.so"
fi
printf 'Candidate binaries built in %s; installed system unchanged.\n' "$out"
