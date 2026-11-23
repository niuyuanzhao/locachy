#!/bin/sh
# test-docscan.sh [--require-pdf] [test-docscan args...]:
# Build locachy-docscan and test-docscan on Linux with LOCachy's strict
# compiler flags and run the test suite.
set -eu

require_pdf=0
if [ "${1:-}" = "--require-pdf" ]; then
    require_pdf=1
    shift
fi

src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
base_tmp=${TMPDIR:-/tmp}
if [ -n "${CLAUDE_JOB_DIR:-}" ]; then
    base_tmp="$CLAUDE_JOB_DIR/tmp"
    mkdir -p "$base_tmp"
fi
out=$(mktemp -d "$base_tmp/docscan-build.XXXXXX")
trap 'rm -rf "$out"' EXIT

if ! pkg-config --exists libarchive; then
    echo "FAIL test-docscan: libarchive pkg-config metadata not found" >&2
    exit 1
fi

arch_cflags=$(pkg-config --cflags libarchive)
arch_libs=$(pkg-config --libs libarchive)

mupdf_cflags=""
mupdf_libs=""
no_pdf_arg=""
if pkg-config --exists mupdf; then
    mupdf_cflags=$(pkg-config --cflags mupdf)
    mupdf_libs="$(pkg-config --libs mupdf) -lm"
elif [ -f /usr/include/mupdf/fitz.h ]; then
    mupdf_libs="-lmupdf -lm"
else
    if [ "$require_pdf" -eq 1 ]; then
        echo "FAIL test-docscan: libmupdf headers (/usr/include/mupdf/fitz.h) not installed" >&2
        exit 1
    fi
    echo "NOTE test-docscan: libmupdf not installed; building with -DDOCSCAN_NO_MUPDF (PDF extraction disabled)" >&2
    mupdf_cflags="-DDOCSCAN_NO_MUPDF"
    no_pdf_arg="--no-pdf"
fi

cc_flags="-D_GNU_SOURCE -O2 -Wall -Wextra -Werror"

# shellcheck disable=SC2086
gcc $cc_flags $arch_cflags $mupdf_cflags \
    "$src/progs/locachy-docscan.c" \
    $arch_libs $mupdf_libs \
    -o "$out/locachy-docscan"

# shellcheck disable=SC2086
gcc $cc_flags $arch_cflags \
    "$src/tools/test-docscan.c" \
    $arch_libs \
    -o "$out/test-docscan"

# shellcheck disable=SC2086
"$out/test-docscan" "$out/locachy-docscan" $no_pdf_arg "$@"
