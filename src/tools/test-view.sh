#!/bin/sh
# test-view.sh: build view (as make.sh builds the framebuffer programs) and
# run its tests, normally and under AddressSanitizer and UBSan: the layout's
# arithmetic (test-view-layout) and the program driven by its keys and
# pointer against the real view-render (test-view). Run it as root in a
# development root with the packages view-render needs and a user of uid
# 1000; the source tree is the current directory or $1. PASS lines; the exit
# status is not 0 on any failure.
set -eu
src=$(cd "${1:-.}" && pwd)
b=$(mktemp -d /tmp/test-view.XXXXXX)
chmod 755 "$b"
trap 'rm -rf "$b"' EXIT
cd "$src"

FT_CFLAGS="-DCONFIG_FREETYPE $(pkg-config --cflags freetype2)"
NM_CFLAGS="-DCONFIG_NETWORKMANAGER $(pkg-config --cflags libnm)"
INPUT_CFLAGS="-DCONFIG_LIBINPUT $(pkg-config --cflags libinput)"
LIBS="$(pkg-config --libs freetype2) $(pkg-config --libs libnm) $(pkg-config --libs libinput) -lm"
GCC="gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I$src/base/include -I$src/base $FT_CFLAGS $NM_CFLAGS $INPUT_CFLAGS"

# The program itself, linked as make.sh links the others (a build check).
for f in base/*.c; do
    [ "$(basename "$f")" = ime.c ] && continue
    $GCC -c -o "$b/glibc-base-$(basename "$f" .c).o" "$f"
done
$GCC -o "$b/view" progs/view.c progs/view-layout.c progs/files.c progs/view-client.c "$b"/glibc-base-*.o $LIBS
echo "PASS view builds with -Werror"

# view-render, as its own tests build it, where uid 1000 can run it.
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE progs/view-render.c progs/view-image.c -o "$b/view-render" \
    $(pkg-config --cflags --libs libspectre libheif libjxl libwebp libwebpdemux libarchive ddjvuapi MagickWand) -lmupdf -lm
sh tools/make-view-samples.sh "$b/samples" >/dev/null
# The PDF fixtures of test-view-pdf (forms, an encrypted copy, a signature
# placeholder) and, with OpenSSL, the generated CMS-signed copy.
gcc -std=c11 -O2 -Wall -Wextra -Werror progs/view-pdf.c tools/test-view-pdf.c -o "$b/test-view-pdf" -lmupdf -lm
mkdir -p "$b/pdf"
"$b/test-view-pdf" fixture "$b/pdf"
if command -v python3 >/dev/null && command -v openssl >/dev/null; then
    python3 tools/test-view-pdf-sign.py "$b/pdf" make >/dev/null
else
    echo "SKIP signed PDF fixture: python3 or openssl missing"
fi
chmod -R a+rX "$b"

status=0
for mode in normal sanitizer; do
    flags="-O1 -g"
    run_env=""
    if [ "$mode" = sanitizer ]; then
        flags="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
        run_env="ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1"
    fi
    gcc $flags -Wall -Wextra -Werror -D_GNU_SOURCE -Iprogs -o "$b/test-view-layout-$mode" \
        tools/test-view-layout.c progs/view-layout.c -lm
    if env $run_env "$b/test-view-layout-$mode"; then
        echo "PASS test-view-layout ($mode)"
    else
        echo "FAIL test-view-layout ($mode)"
        status=1
    fi
    gcc $flags -Wall -Wextra -Werror -D_GNU_SOURCE -I"$src/base/include" -I"$src/base" -Iprogs \
        -DVIEW_RENDER_TEST_PATH="\"$b/view-render\"" -o "$b/test-view-$mode" \
        tools/test-view.c progs/view-layout.c progs/view-client.c base/ui.c -lm
    if env $run_env "$b/test-view-$mode" "$b/samples" "$b/work-$mode" "$b/pdf"; then
        echo "PASS test-view ($mode)"
    else
        echo "FAIL test-view ($mode)"
        status=1
    fi
done

# Exercise the same handle() path with the real Rime engine, not a commit
# stub. Its schema/user data are this disposable development root's only.
if pkg-config --exists rime; then
    lua=$(ldd /usr/lib/rime-plugins/librime-lua.so 2>/dev/null | sed -n 's/^[[:space:]]*\(liblua[^ ]*\) => .*/\1/p' | head -n 1)
    gcc -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -DCONFIG_IME -I"$src/base/include" -I"$src/base" -Iprogs \
        -DVIEW_RENDER_TEST_PATH="\"$b/view-render\"" -o "$b/test-view-ime" \
        tools/test-view.c progs/view-layout.c progs/view-client.c base/ui.c base/ime.c \
        $(pkg-config --cflags --libs rime) ${lua:+-Wl,--push-state,--no-as-needed -l:$lua -Wl,--pop-state} -lm
    if "$b/test-view-ime" "$b/samples" "$b/work-ime" "$b/pdf"; then
        echo "PASS test-view (real Rime IME)"
    else
        echo "FAIL test-view (real Rime IME)"
        status=1
    fi
else
    echo "SKIP real Rime IME: package not installed"
fi
exit $status
