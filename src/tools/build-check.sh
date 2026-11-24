#!/bin/sh
# Build candidates without replacing the running host or its boot image.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: build-check.sh OUTPUT_DIR}
mkdir -p "$out/obj" "$out/uapi"
for name in linux asm asm-generic drm sound; do
    ln -sfn "/usr/include/$name" "$out/uapi/$name"
done

sec_flags="-ffunction-sections -fdata-sections"
gc_ldflags="-Wl,--gc-sections -Wl,--as-needed"
lto_flags=
ar_bin=ar
if command -v gcc-ar >/dev/null 2>&1; then
    lto_flags="-flto=auto"
    ar_bin=gcc-ar
fi
musl_lto=
musl_ar=ar
if [ -n "$lto_flags" ] && printf 'int main(void){return 0;}\n' | \
    musl-gcc -static -O2 $sec_flags $lto_flags -Wl,--gc-sections -x c - \
    -o "$out/obj/musl-lto-probe" >/dev/null 2>&1; then
    musl_lto="$lto_flags"
    musl_ar="$ar_bin"
fi
rm -f "$out/obj/musl-lto-probe"

for file in "$src"/base/*.c; do
    name=$(basename "$file" .c)
    [ "$name" != open ] || continue # GIO's, the programs' only (make.sh)
    [ "$name" != jobs ] || continue # locachy-jobs' clients: the programs' only
    musl-gcc -static -O2 $sec_flags $musl_lto -Wall -Wextra -I"$src/base/include" -I"$src/base" \
        -isystem "$out/uapi" -c "$file" -o "$out/obj/musl-$name.o"
done
rm -f "$out/obj/liblocachy-musl.a"
"$musl_ar" rcs "$out/obj/liblocachy-musl.a" "$out"/obj/musl-*.o
musl-gcc -static -O2 $sec_flags $musl_lto -Wl,--gc-sections -Wall -Wextra \
    -I"$src/base/include" -I"$src/base" -isystem "$out/uapi" \
    "$src/init/init.c" "$src/init/apps.c" \
    -Wl,-u,theme_get -Wl,-u,theme_reload \
    "$out/obj/liblocachy-musl.a" -o "$out/init"

core_cflags="-O2 $sec_flags $lto_flags -Wall -Wextra -Werror -D_GNU_SOURCE"
ft_cflags="$(pkg-config --cflags freetype2 fontconfig) -DCONFIG_FREETYPE"
ft_libs="$(pkg-config --libs freetype2 fontconfig)"
nm_cflags="$(pkg-config --cflags libnm) -DCONFIG_NETWORKMANAGER"
nm_libs="$(pkg-config --libs libnm)"
input_cflags="$(pkg-config --cflags libinput) -DCONFIG_LIBINPUT"
input_libs="$(pkg-config --libs libinput) -lm"
gio_cflags="$(pkg-config --cflags gio-2.0)"
gio_libs="$(pkg-config --libs gio-2.0)"
ui_cflags="$ft_cflags $nm_cflags $input_cflags"
ui_libs="$ft_libs $nm_libs $input_libs"

# Base modules partitioned into isolated static archives:
#   liblocachy-core.a   zero external shared libraries (rc, say, control, theme, ...)
#   liblocachy-ui.a     direct DRM + evdev + FreeType/Fontconfig + libinput
#   liblocachy-status.a status bar / NetworkManager + BlueZ D-Bus
#   liblocachy-open.a   content-type rules and open-request spool (GIO)
#   liblocachy-ime.a    input method (librime, when installed)
core_mods="rc say control duress jobs i18n key-action monitor-modes monitors backlight clipboard theme theme-desktop input-raw modload"
ui_mods="screen font fontmatch keys pointer ui help textsel panel"

for name in $core_mods; do
    gcc $core_cflags -I"$src/base/include" -I"$src/base" \
        -c "$src/base/$name.c" -o "$out/obj/base-$name.o"
done
for name in $ui_mods wayland-data; do
    gcc $core_cflags $ui_cflags -I"$src/base/include" -I"$src/base" \
        -c "$src/base/$name.c" -o "$out/obj/base-$name.o"
done
gcc $core_cflags $nm_cflags -I"$src/base/include" -I"$src/base" \
    -c "$src/base/status.c" -o "$out/obj/base-status.o"
gcc $core_cflags $gio_cflags -I"$src/base/include" -I"$src/base" \
    -c "$src/base/open.c" -o "$out/obj/base-open.o"

ime_flags= ime_libs=
if [ -f /usr/include/rime_api.h ] && [ -f /usr/lib/librime.so ]; then
    ime_flags=-DCONFIG_IME
    # Rime's Lua plugin's Lua before libinput's, as make.sh links it.
    rime_lua=$(ldd /usr/lib/rime-plugins/librime-lua.so 2>/dev/null | sed -n 's/^[[:space:]]*\(liblua[^ ]*\) => .*/\1/p' | head -n 1)
    ime_libs="-lrime${rime_lua:+ -Wl,--push-state,--no-as-needed -l:$rime_lua -Wl,--pop-state}"
fi
gcc $core_cflags $ime_flags -I"$src/base/include" \
    -c "$src/base/ime.c" -o "$out/obj/ime.o"

rm -f "$out/obj/liblocachy-core.a" "$out/obj/liblocachy-ui.a" \
      "$out/obj/liblocachy-status.a" "$out/obj/liblocachy-open.a" "$out/obj/liblocachy-ime.a"
"$ar_bin" rcs "$out/obj/liblocachy-core.a" \
    "$out/obj/base-rc.o" "$out/obj/base-say.o" "$out/obj/base-control.o" "$out/obj/base-duress.o" \
    "$out/obj/base-jobs.o" "$out/obj/base-i18n.o" "$out/obj/base-key-action.o" \
    "$out/obj/base-monitor-modes.o" "$out/obj/base-monitors.o" "$out/obj/base-backlight.o" \
    "$out/obj/base-clipboard.o" "$out/obj/base-theme.o" "$out/obj/base-theme-desktop.o" \
    "$out/obj/base-input-raw.o" "$out/obj/base-modload.o"
"$ar_bin" rcs "$out/obj/liblocachy-ui.a" \
    "$out/obj/base-screen.o" "$out/obj/base-font.o" "$out/obj/base-fontmatch.o" \
    "$out/obj/base-keys.o" "$out/obj/base-pointer.o" "$out/obj/base-ui.o" \
    "$out/obj/base-help.o" "$out/obj/base-textsel.o" "$out/obj/base-panel.o" \
    "$out/obj/base-wayland-data.o"
"$ar_bin" rcs "$out/obj/liblocachy-status.a" "$out/obj/base-status.o"
"$ar_bin" rcs "$out/obj/liblocachy-open.a" "$out/obj/base-open.o"
"$ar_bin" rcs "$out/obj/liblocachy-ime.a" "$out/obj/ime.o"

lib_core="$out/obj/liblocachy-core.a"
lib_ui="$out/obj/liblocachy-ui.a"
lib_status="$out/obj/liblocachy-status.a"
lib_open="$out/obj/liblocachy-open.a"
lib_ime="$out/obj/liblocachy-ime.a"

# Weak hook anchors: GNU ld does not pull archive members for weak undefined
# references alone, so real binaries force the base hooks they actually expect
# while --gc-sections still drops unused functions from those objects.
hook_theme="-Wl,-u,theme_get -Wl,-u,theme_reload"
hook_theme_watch="$hook_theme -Wl,-u,theme_watch_open -Wl,-u,theme_watch_read"
hook_bar="-Wl,-u,base_status_fields"
hook_open="-Wl,-u,open_request_publish"
hook_win="-Wl,-u,wl_data_init -Wl,-u,wl_data_active"
# Weak hooks of the base, resolved by pulling their archive members: input-raw.o
# (keys.c, pointer.c, the window transport) and the data device (clipboard.c, files.c).
hook_raw="-Wl,-u,base_raw_cancel -Wl,-u,base_raw_emit -Wl,-u,base_raw_forward_home -Wl,-u,base_raw_pending"
hook_data="-Wl,-u,wl_data_active -Wl,-u,wl_data_clipboard_get -Wl,-u,wl_data_clipboard_set -Wl,-u,wl_data_clipboard_clear -Wl,-u,wl_data_drag_active -Wl,-u,wl_data_drag_start -Wl,-u,wl_data_drag_start_key -Wl,-u,wl_data_press_serial"
hook_base="$hook_raw $hook_data"

# Headless services: zero UI/NM/GIO/FreeType shared dependencies.
gcc $core_cflags $gc_ldflags -I"$src/base/include" -I"$src/base" \
    "$src/progs/drives.c" "$lib_core" -o "$out/drives"
gcc $core_cflags $gc_ldflags -I"$src/base/include" -I"$src/base" \
    "$src/progs/seat.c" -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" -Wl,--end-group \
    -o "$out/seat"
gcc $core_cflags $gc_ldflags $ui_cflags -I"$src/base/include" -I"$src/base" \
    "$src/progs/note.c" $hook_theme $hook_base $hook_bar \
    -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" -Wl,--end-group \
    $ui_libs -o "$out/note"

# The live medium's independent installer and rescue applications.
for name in installer rescue; do
    gcc $core_cflags $gc_ldflags $ui_cflags $(pkg-config --cflags json-c) \
        -I"$src/base/include" -I"$src/base" "$src/progs/$name.c" $hook_theme $hook_base $hook_bar \
        -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" -Wl,--end-group \
        $ui_libs $(pkg-config --libs json-c) -o "$out/$name"
done

# These direct clients compile the file manager with identical options.
gcc $core_cflags $ui_cflags -I"$src/base/include" -I"$src/base" \
    -c "$src/progs/files.c" -o "$out/obj/files-direct.o"
for name in home packages firmware archive; do
    gcc $core_cflags $gc_ldflags $ui_cflags -I"$src/base/include" -I"$src/base" \
        "$src/progs/$name.c" "$out/obj/files-direct.o" $hook_theme $hook_base $hook_bar $hook_open \
        -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" "$lib_open" -Wl,--end-group \
        $ui_libs $gio_libs -o "$out/$name"
done

# The archive manager's worker: libarchive (pacman's) and OpenSSL's hashes.
gcc $core_cflags $gc_ldflags "$src/progs/archive-worker.c" \
    $(pkg-config --cflags --libs libarchive libcrypto) -o "$out/archive-worker"
gcc $core_cflags $gc_ldflags "$src/progs/files-cloud-list.c" \
    $gio_cflags $gio_libs -o "$out/files-cloud-list"
gcc $core_cflags $gc_ldflags "$src/progs/container-documents.c" \
    $gio_cflags $gio_libs -o "$out/container-documents"
gcc $core_cflags $gc_ldflags "$src/progs/locachy-docs.c" \
    $(pkg-config --cflags --libs gio-unix-2.0) -o "$out/locachy-docs"
gcc $core_cflags $gc_ldflags "$src/progs/battery-history.c" \
    -o "$out/battery-history"
gcc $core_cflags $gc_ldflags -I"$src/base/include" "$src/progs/locachy-jobs.c" \
    -o "$out/locachy-jobs"
# Global search daemon and sandboxed document text extractor (tools/SEARCH.md).
if pkg-config --exists sqlite3; then
    gcc $core_cflags $gc_ldflags $(pkg-config --cflags sqlite3) \
        "$src/progs/locachy-searchd.c" "$src/progs/search-index.c" \
        $(pkg-config --libs sqlite3) -lm -o "$out/locachy-searchd"
fi
if pkg-config --exists libarchive; then
    docscan_mupdf_cflags= docscan_mupdf_libs=
    if pkg-config --exists mupdf; then
        docscan_mupdf_cflags=$(pkg-config --cflags mupdf)
        docscan_mupdf_libs="$(pkg-config --libs mupdf) -lm"
    elif [ -f /usr/include/mupdf/fitz.h ]; then
        docscan_mupdf_libs="-lmupdf -lm"
    else
        docscan_mupdf_cflags="-DDOCSCAN_NO_MUPDF"
    fi
    gcc $core_cflags $gc_ldflags $(pkg-config --cflags libarchive) $docscan_mupdf_cflags \
        "$src/progs/locachy-docscan.c" $(pkg-config --libs libarchive) $docscan_mupdf_libs \
        -o "$out/locachy-docscan"
fi
if [ -f "$src/progs/search.c" ]; then
    gcc $core_cflags $gc_ldflags $ime_flags $ui_cflags -I"$src/base/include" -I"$src/base" \
        "$src/progs/search.c" $hook_theme $hook_base $hook_bar $hook_open -Wl,-u,clipboard_get \
        -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" "$lib_open" "$lib_ime" -Wl,--end-group \
        $ime_libs $ui_libs $gio_libs -o "$out/search"
fi
gcc $core_cflags $gc_ldflags -I"$src/base/include" "$src/progs/locachy-config.c" \
    "$lib_core" -o "$out/locachy-config"
# The key table of a keymap: libxkbcommon (make.sh, installer/make-live.sh).
gcc $core_cflags $gc_ldflags "$src/progs/keytable.c" \
    $(pkg-config --cflags --libs xkbcommon) -o "$out/keytable"
# The document viewer, where its libraries are installed (make.sh).
view_pkgs="libspectre libheif libjxl libwebp libwebpdemux libarchive ddjvuapi MagickWand libraw"
if pkg-config --exists $view_pkgs && [ -f /usr/include/mupdf/fitz.h ] && [ -f /usr/include/chm_lib.h ]; then
    gcc $core_cflags $gc_ldflags "$src/progs/view-render.c" "$src/progs/view-image.c" \
        $(pkg-config --cflags --libs $view_pkgs) -lmupdf -lchm -lm -o "$out/view-render"
    gcc $core_cflags $gc_ldflags $ime_flags $ui_cflags \
        -I"$src/base/include" -I"$src/base" "$src/progs/view.c" "$src/progs/view-layout.c" \
        "$src/progs/view-client.c" "$out/obj/files-direct.o" $hook_theme $hook_base $hook_bar $hook_open \
        -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" "$lib_open" "$lib_ime" -Wl,--end-group \
        $ime_libs $ui_libs $gio_libs -o "$out/view"
fi
# The text editor, where tree-sitter and pcre2 are installed (make.sh).
if pkg-config --exists tree-sitter libpcre2-8 json-glib-1.0; then
    gcc $core_cflags $gc_ldflags $ime_flags $ui_cflags \
        $(pkg-config --cflags json-glib-1.0) -I"$src/base/include" -I"$src/base" \
        "$src/progs/edit.c" "$src/progs/edit-buffer.c" \
        "$src/progs/edit-doc.c" "$src/progs/edit-file.c" "$src/progs/edit-search.c" "$src/progs/edit-syntax.c" \
        "$src/progs/edit-tree.c" "$src/progs/edit-layout.c" "$src/progs/edit-lsp.c" \
        "$src/progs/edit-session.c" "$src/progs/edit-filesearch.c" "$out/obj/files-direct.o" \
        $hook_theme $hook_base $hook_bar $hook_open \
        -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" "$lib_open" "$lib_ime" -Wl,--end-group \
        $ime_libs $ui_libs $gio_libs -ltree-sitter -lpcre2-8 $(pkg-config --libs json-glib-1.0) -o "$out/edit"
fi
# The extended display's receiver on the direct screen and its sandboxed
# decoder (make.sh).
extdisplay_pkgs="json-c libavcodec libavutil libswscale avahi-core"
if pkg-config --exists $extdisplay_pkgs; then
    va_cflags= va_libs=
    if pkg-config --exists libva libva-drm; then
        va_cflags="-DCONFIG_EXTDISPLAY_VAAPI $(pkg-config --cflags libva libva-drm)"
        va_libs="$(pkg-config --libs libva libva-drm)"
    fi
    gcc -O3 $sec_flags $lto_flags -Wall -Wextra -Werror -D_GNU_SOURCE -DCONFIG_EXTDISPLAY_FFMPEG $va_cflags \
        $(pkg-config --cflags $extdisplay_pkgs) -c "$src/progs/extdisplay-decode.c" -o "$out/obj/extdisplay-decode.o"
    gcc $core_cflags $gc_ldflags -DCONFIG_EXTDISPLAY_FFMPEG $va_cflags $(pkg-config --cflags $extdisplay_pkgs) \
        "$src/progs/extdisplay-worker.c" "$src/progs/extdisplay-protocol.c" "$out/obj/extdisplay-decode.o" \
        $(pkg-config --libs $extdisplay_pkgs) $va_libs -lm -o "$out/extdisplay-worker"
    gcc $core_cflags $gc_ldflags $ui_cflags $(pkg-config --cflags json-c) \
        -I"$src/base/include" -I"$src/base" "$src/progs/extdisplay.c" "$src/progs/extdisplay-setup.c" \
        "$src/progs/extdisplay-client.c" "$src/progs/extdisplay-protocol.c" \
        $hook_theme $hook_base $hook_bar -Wl,-u,base_raw_emit \
        -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" -Wl,--end-group \
        $ui_libs $(pkg-config --libs json-c) -o "$out/extdisplay"
else
    echo "build-check: extdisplay not built: json-c, Avahi and FFmpeg (libavcodec, libavutil, libswscale) are not all installed" >&2
fi
# The extended display's broker (tools/EXTDISPLAY.md), built under -Werror as make.sh installs it.
if pkg-config --exists $extdisplay_pkgs openssl; then
    gcc $core_cflags $gc_ldflags $(pkg-config --cflags avahi-core) -o "$out/extdisplay-advertise" \
        "$src/progs/extdisplay-advertise.c" $(pkg-config --libs avahi-core)
    gcc $core_cflags $gc_ldflags $(pkg-config --cflags openssl) -o "$out/extdisplay-broker" \
        "$src/progs/extdisplay-broker.c" "$src/progs/extdisplay-broker-net.c" "$src/progs/extdisplay-broker-pump.c" \
        "$src/progs/extdisplay-broker-tls.c" "$src/progs/extdisplay-discovery.c" $(pkg-config --libs openssl)
fi
gcc $core_cflags $gc_ldflags $ui_cflags -I"$src/base/include" -I"$src/base" \
    "$src/progs/settings.c" "$out/obj/files-direct.o" "$src/progs/auth.c" \
    $hook_theme $hook_base $hook_bar $hook_open \
    -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" "$lib_open" -Wl,--end-group \
    $ui_libs $gio_libs -lpam -o "$out/settings"
gcc $core_cflags $gc_ldflags $ui_cflags -I"$src/base/include" -I"$src/base" \
    "$src/progs/lock.c" "$src/progs/auth.c" "$src/progs/duress-shadow.c" \
    $hook_theme $hook_base $hook_bar \
    -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" -Wl,--end-group \
    $ui_libs -lpam -lcrypt -o "$out/lock"
gcc $core_cflags $gc_ldflags -shared -fPIC "$src/progs/pam_locachy_any.c" "$src/progs/auth.c" \
    -lpam -o "$out/pam_locachy_any.so"
gcc $core_cflags -fPIC -I"$src/base/include" -c "$src/base/duress.c" -o "$out/obj/pic-duress.o"
gcc $core_cflags $gc_ldflags -shared -fPIC -I"$src/base/include" "$src/progs/pam_locachy_duress.c" \
    "$src/progs/duress-shadow.c" "$out/obj/pic-duress.o" -lpam -lcrypt -o "$out/pam_locachy_duress.so"
gcc $core_cflags $gc_ldflags -I"$src/base/include" "$src/progs/locachy-duress.c" \
    "$src/progs/duress-shadow.c" "$lib_core" -lcrypt -o "$out/locachy-duress"
gcc $core_cflags $gc_ldflags $ime_flags $ui_cflags -I"$src/base/include" \
    "$src/progs/term.c" $hook_theme $hook_base $hook_bar \
    -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" "$lib_ime" -Wl,--end-group \
    $ime_libs $ui_libs -o "$out/term"
printf '%s\n' "$ime_libs" >"$out/ime-libs"
if [ -f "$src/progs/app-init.c" ]; then
    musl-gcc -static -O2 $sec_flags $musl_lto -Wl,--gc-sections -Wall -Wextra -Werror \
        "$src/progs/app-init.c" -o "$out/app-init"
fi
if command -v wayland-scanner >/dev/null 2>&1 && pkg-config --exists wayland-client wayland-cursor xkbcommon libpng; then
    wayland-scanner client-header "$src/base/protocols/ext-data-control-v1.xml" "$out/ext-data-control-v1-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/ext-data-control-v1.xml" "$out/ext-data-control-v1-protocol.c"
    gcc $core_cflags $gc_ldflags -I"$src/base/include" -I"$out" \
        "$src/progs/clipboard.c" "$out/ext-data-control-v1-protocol.c" \
        "$lib_core" -lwayland-client -o "$out/clipboard"
    gcc $core_cflags $gc_ldflags "$src/progs/appearance-portal.c" \
        $gio_cflags $gio_libs -o "$out/appearance-portal"
    gcc $core_cflags $gc_ldflags -I"$src/base/include" "$src/progs/locachy-foot.c" \
        -Wl,-u,theme_get "$lib_core" -o "$out/locachy-foot"
    # The shared compositor's bar, window control, overview and screenshot tool, with the protocols they speak.
    wl="wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 xdg-shell wlr-foreign-toplevel-management-unstable-v1
        wlr-output-management-unstable-v1 wlr-output-power-management-unstable-v1 fractional-scale-v1 viewporter
        relative-pointer-unstable-v1 ext-foreign-toplevel-list-v1 ext-image-capture-source-v1
        ext-image-copy-capture-v1 security-context-v1 ext-idle-notify-v1"
    for p in $wl; do
        wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/$p-client-protocol.h"
        wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/$p-protocol.c"
    done
    proto() { for p in "$@"; do printf '%s ' "$out/$p-protocol.c"; done; }
    wl_libs="-lwayland-client -lwayland-cursor -lxkbcommon"
    gcc $core_cflags $gc_ldflags $ime_flags $ui_cflags \
        -I"$src/base/include" -I"$src/base" -I"$out" "$src/progs/bar.c" \
        $(proto wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 xdg-shell \
            wlr-foreign-toplevel-management-unstable-v1 fractional-scale-v1 viewporter relative-pointer-unstable-v1) \
        $hook_theme $hook_base $hook_bar $hook_open \
        -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" "$lib_open" "$lib_ime" -Wl,--end-group \
        $ime_libs $ft_libs $nm_libs $gio_libs $wl_libs -lm -o "$out/locachy-bar"
    gcc $core_cflags $gc_ldflags \
        -I"$src/base/include" -I"$src/base" -I"$out" "$src/progs/labwc-control.c" \
        $(proto security-context-v1 wlr-foreign-toplevel-management-unstable-v1 wlr-output-management-unstable-v1 \
            wlr-output-power-management-unstable-v1 ext-idle-notify-v1) \
        -Wl,--start-group "$lib_core" "$lib_open" -Wl,--end-group \
        -lwayland-client -lm -o "$out/labwc-control"
    gcc $core_cflags $gc_ldflags $ft_cflags \
        -I"$src/base/include" -I"$src/base" -I"$out" "$src/progs/overview.c" \
        $(proto wlr-layer-shell-unstable-v1 wlr-foreign-toplevel-management-unstable-v1 ext-foreign-toplevel-list-v1 \
            ext-image-capture-source-v1 ext-image-copy-capture-v1 fractional-scale-v1 viewporter xdg-shell) \
        $hook_theme_watch $hook_base \
        -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" "$lib_open" -Wl,--end-group \
        $ft_libs $wl_libs -lm -o "$out/locachy-overview"
    gcc $core_cflags $gc_ldflags $ft_cflags \
        -I"$src/base/include" -I"$src/base" -I"$out" "$src/progs/shot.c" \
        $(proto wlr-layer-shell-unstable-v1 wlr-foreign-toplevel-management-unstable-v1 wlr-output-management-unstable-v1 \
            ext-foreign-toplevel-list-v1 ext-image-capture-source-v1 ext-image-copy-capture-v1 \
            fractional-scale-v1 viewporter xdg-shell) \
        $hook_theme_watch $hook_base $hook_open \
        -Wl,--start-group "$lib_core" "$lib_ui" "$lib_status" "$lib_open" -Wl,--end-group \
        $ft_libs $wl_libs -lpng -lm -o "$out/locachy-shot"
    # The programs that also open as windows (make.sh's liblocachy-win.a):
    # UI + window transport (wayland-backend.c, wayland-data.c, protocols);
    # direct-only builds above stay on liblocachy-ui.a.
    for p in primary-selection-unstable-v1 xdg-decoration-unstable-v1; do
        wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/$p-client-protocol.h"
        wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/$p-protocol.c"
    done
    win_proto_objs=
    for p in xdg-shell fractional-scale-v1 viewporter primary-selection-unstable-v1 xdg-decoration-unstable-v1 wlr-layer-shell-unstable-v1; do
        gcc -O2 $sec_flags $lto_flags -c "$out/$p-protocol.c" -o "$out/obj/proto-$p.o"
        win_proto_objs="$win_proto_objs $out/obj/proto-$p.o"
    done
    wl_cflags="$(pkg-config --cflags wayland-client wayland-cursor xkbcommon)"
    for name in $ui_mods wayland-backend wayland-data; do
        gcc $core_cflags -DCONFIG_WAYLAND $ui_cflags $wl_cflags \
            -I"$src/base/include" -I"$src/base" -I"$out" \
            -c "$src/base/$name.c" -o "$out/obj/win-$name.o"
    done
    rm -f "$out/obj/liblocachy-win.a"
    "$ar_bin" rcs "$out/obj/liblocachy-win.a" \
        "$out/obj/win-screen.o" "$out/obj/win-font.o" "$out/obj/win-fontmatch.o" \
        "$out/obj/win-keys.o" "$out/obj/win-pointer.o" "$out/obj/win-ui.o" \
        "$out/obj/win-help.o" "$out/obj/win-textsel.o" "$out/obj/win-panel.o" \
        "$out/obj/win-wayland-backend.o" "$out/obj/win-wayland-data.o" $win_proto_objs
    lib_win="$out/obj/liblocachy-win.a"
    gcc $core_cflags $gc_ldflags $ime_flags $ui_cflags -I"$src/base/include" -I"$out" \
        "$src/progs/term.c" $hook_theme_watch $hook_base $hook_bar $hook_win \
        -Wl,--start-group "$lib_win" "$lib_core" "$lib_status" "$lib_ime" -Wl,--end-group \
        $ime_libs $ui_libs $wl_libs -o "$out/term-window"
    gcc $core_cflags $gc_ldflags $ui_cflags -I"$src/base/include" -I"$src/base" -I"$out" \
        "$src/progs/settings.c" "$out/obj/files-direct.o" "$src/progs/auth.c" \
        $hook_theme_watch $hook_base $hook_bar $hook_open $hook_win \
        -Wl,--start-group "$lib_win" "$lib_core" "$lib_status" "$lib_open" -Wl,--end-group \
        $ui_libs $gio_libs $wl_libs -lpam -o "$out/settings-window"
    gcc $core_cflags $gc_ldflags $ui_cflags -I"$src/base/include" -I"$src/base" -I"$out" \
        "$src/progs/files-main.c" "$out/obj/files-direct.o" \
        $hook_theme_watch $hook_base $hook_bar $hook_open $hook_win \
        -Wl,--start-group "$lib_win" "$lib_core" "$lib_status" "$lib_open" -Wl,--end-group \
        $ui_libs $gio_libs $wl_libs -o "$out/files"
    for name in packages firmware archive; do
        gcc $core_cflags $gc_ldflags $ui_cflags -I"$src/base/include" -I"$src/base" -I"$out" \
            "$src/progs/$name.c" "$out/obj/files-direct.o" \
            $hook_theme_watch $hook_base $hook_bar $hook_open $hook_win \
            -Wl,--start-group "$lib_win" "$lib_core" "$lib_status" "$lib_open" -Wl,--end-group \
            $ui_libs $gio_libs $wl_libs -o "$out/$name-window"
    done
    if [ -f "$src/progs/search.c" ]; then
        gcc $core_cflags $gc_ldflags $ime_flags $ui_cflags -I"$src/base/include" -I"$src/base" -I"$out" \
            "$src/progs/search.c" $hook_theme_watch $hook_base $hook_bar $hook_open $hook_win -Wl,-u,clipboard_get \
            -Wl,--start-group "$lib_win" "$lib_core" "$lib_status" "$lib_open" "$lib_ime" -Wl,--end-group \
            $ime_libs $ui_libs $gio_libs $wl_libs -o "$out/search-window"
    fi
    if [ -e "$out/view" ]; then
        gcc $core_cflags $gc_ldflags $ime_flags $ui_cflags -I"$src/base/include" -I"$src/base" -I"$out" \
            "$src/progs/view.c" "$src/progs/view-layout.c" "$src/progs/view-client.c" "$out/obj/files-direct.o" \
            $hook_theme_watch $hook_base $hook_bar $hook_open $hook_win \
            -Wl,--start-group "$lib_win" "$lib_core" "$lib_status" "$lib_open" "$lib_ime" -Wl,--end-group \
            $ime_libs $ui_libs $gio_libs $wl_libs -o "$out/view-window"
    fi
    if [ -e "$out/edit" ]; then
        gcc $core_cflags $gc_ldflags $ime_flags $ui_cflags \
            $(pkg-config --cflags json-glib-1.0) -I"$src/base/include" -I"$src/base" -I"$out" \
            "$src/progs/edit.c" "$src/progs/edit-buffer.c" "$src/progs/edit-doc.c" "$src/progs/edit-file.c" \
            "$src/progs/edit-search.c" "$src/progs/edit-syntax.c" "$src/progs/edit-tree.c" \
            "$src/progs/edit-layout.c" "$src/progs/edit-lsp.c" "$src/progs/edit-session.c" \
            "$src/progs/edit-filesearch.c" "$out/obj/files-direct.o" \
            $hook_theme_watch $hook_base $hook_bar $hook_open $hook_win \
            -Wl,--start-group "$lib_win" "$lib_core" "$lib_status" "$lib_open" "$lib_ime" -Wl,--end-group \
            $ime_libs $ui_libs $gio_libs $wl_libs -ltree-sitter -lpcre2-8 $(pkg-config --libs json-glib-1.0) \
            -o "$out/edit-window"
    fi
fi
if pkg-config --exists libcryptsetup json-c openssl; then
    gcc $core_cflags $gc_ldflags -shared -fPIC \
        $(pkg-config --cflags libcryptsetup json-c openssl) \
        "$src/progs/token-tpm12.c" \
        -Wl,--version-script="$src/progs/token-tpm12.sym" \
        $(pkg-config --libs libcryptsetup json-c openssl) \
        -o "$out/libcryptsetup-token-locachy-tpm12.so"
fi
# The shell scripts parse (an apostrophe in a comment of a single-quoted
# inner script ends the quote: the script then fails as a whole).
for f in "$src"/link/* "$src"/*.sh "$src"/tools/*.sh; do
    case "$(head -n 1 "$f" 2>/dev/null)" in
    '#!/bin/bash'* | '#!/usr/bin/env bash'*) bash -n "$f" || { echo "build-check: $f does not parse" >&2; exit 1; } ;;
    '#!/bin/sh'*) sh -n "$f" || { echo "build-check: $f does not parse" >&2; exit 1; } ;;
    esac
done
if command -v setpriv >/dev/null 2>&1; then
    caps=$(setpriv --list-caps)
    for spec in $(grep -d skip -hoE -- '--bounding-set=[^[:space:]"]+' "$src"/link/*); do
        for tok in $(printf '%s\n' "${spec#--bounding-set=}" | tr ',' ' '); do
            cap=${tok#[+-]}
            [ "$cap" = all ] || printf '%s\n' "$caps" | grep -qxF "$cap" ||
                { echo "build-check: unknown setpriv capability '$cap' in $spec" >&2; exit 1; }
        done
    done
fi
printf 'Candidate binaries built in %s; installed system unchanged.\n' "$out"
