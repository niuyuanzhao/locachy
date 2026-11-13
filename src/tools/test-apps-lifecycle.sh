#!/bin/sh
# test-apps-lifecycle.sh NEW_OUTPUT: process 1's application operations
# (init/apps.c) with real processes and fixture cgroups
# (test-apps-lifecycle.c), as an ordinary user; once as built, once with
# AddressSanitizer and UndefinedBehaviorSanitizer.
set -u
[ "$#" = 1 ] || { echo "usage: test-apps-lifecycle.sh NEW_OUTPUT" >&2; exit 2; }
out=$(realpath -m "$1")
[ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
mkdir -p "$out"
src=$(cd "$(dirname "$0")/.." && pwd)
install -m 755 "$src/tools/test-apps-lifecycle-tool.sh" "$out/locachy-app"
status=0
for build in plain san; do
    flags=
    [ "$build" = san ] && flags="-fsanitize=address,undefined -fno-omit-frame-pointer"
    # The fixture root is longer than /sys/fs/cgroup, which apps.c's path
    # buffers are sized for: their truncation warnings are not this build's.
    # shellcheck disable=SC2086
    gcc -O2 -g -Wall -Wextra -Werror -Wno-format-truncation -D_GNU_SOURCE $flags -I"$src/base/include" -I"$src/init" \
        -DFX="\"$out/fx-$build\"" -DTOOL="\"$out/locachy-app\"" \
        "$src/tools/test-apps-lifecycle.c" -o "$out/apps-lifecycle-$build" ||
        { echo "FAIL apps-lifecycle ($build) does not build"; exit 1; }
    result=0
    timeout 240 "$out/apps-lifecycle-$build" >"$out/$build.log" 2>&1 || result=1
    if [ "$result" = 0 ] && ! grep -qE 'Sanitizer|runtime error' "$out/$build.log"; then
        grep '^PASS' "$out/$build.log" | sed "s/^PASS/PASS ($build)/"
    else
        grep -E '^(FAIL|  )' "$out/$build.log" | sed "s/^FAIL/FAIL ($build)/" || true
        echo "FAIL apps-lifecycle ($build): $out/$build.log"
        status=1
    fi
done
exit "$status"
