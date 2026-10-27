#!/bin/sh
# Build/run only generated fixtures inside the disposable Linux development root.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:-/tmp/test-edit-filesearch}
mkdir -p "$out"
for kind in plain asan; do
    extra=
    [ "$kind" = plain ] || extra="-g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
    # The test includes edit-filesearch.c to exercise private transport frames.
    # shellcheck disable=SC2086
    gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE $extra -I"$src/base/include" $(pkg-config --cflags glib-2.0) \
        "$src/tools/test-edit-filesearch.c" "$src/progs/edit-buffer.c" "$src/progs/edit-file.c" \
        "$src/progs/edit-search.c" $(pkg-config --libs glib-2.0) -lpcre2-8 -o "$out/$kind"
    printf '%s\n' "== asynchronous file-search ($kind)"
    ASAN_OPTIONS=detect_leaks=0 "$out/$kind"
done
