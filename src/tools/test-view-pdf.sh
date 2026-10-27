#!/bin/sh
# Run only in the approved disposable Linux root, source mounted at /work.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
dir=/tmp/view-pdf-test
mkdir -p "$dir"
chmod 755 "$dir"
for build in normal sanitize; do
    flags="-std=c11 -O2 -g -Wall -Wextra -Werror"
    [ "$build" = normal ] || flags="$flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
    gcc $flags "$src/progs/view-pdf.c" "$src/tools/test-view-pdf.c" -o "$dir/test-$build" -lmupdf -lm
    data="$dir/$build"
    mkdir -p "$data/root-only"
    chmod 700 "$data/root-only"
    printf '%s' 'ROOT ORIGINAL' > "$data/root-only/original.pdf"
    "$dir/test-$build" fixture "$data"
    python3 "$src/tools/test-view-pdf-sign.py" "$data" make
    "$dir/test-$build" root "$data"
    chown 1000:1000 "$data"
    find "$data" -maxdepth 1 -type f -exec chown 1000:1000 {} +
    timeout 120 setpriv --reuid=1000 --regid=1000 --clear-groups "$dir/test-$build" user "$data"
    python3 "$src/tools/test-view-pdf-sign.py" "$data" check
    [ "$(cat "$data/root-only/original.pdf")" = 'ROOT ORIGINAL' ]
    if find "$data" -name '.locachy-pdf-*' | grep -q .; then
        printf '%s\n' 'FAIL leaked PDF save temporary'
        exit 1
    fi
    printf '%s\n' "PASS view-pdf $build"
done
