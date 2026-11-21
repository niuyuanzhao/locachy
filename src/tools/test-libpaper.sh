#!/bin/bash
# Installed candidate lifecycle, no display or devices. The full regression
# also runs under ASan/UBSan with leak detection.
set -euo pipefail
usage='usage: test-libpaper.sh RESULT_DIRECTORY [--update-smoke]'
out=${1:?$usage}
if [ "$#" -gt 2 ] || { [ "$#" -eq 2 ] && [ "$2" != --update-smoke ]; }; then
    printf '%s\n' "$usage" >&2
    exit 2
fi
scope=${2-}
probe() {
    if [ "$scope" = --update-smoke ]; then
        timeout --kill-after=5 30 "$@"
    else
        "$@"
    fi
}
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
exec > >(tee "$out/library.log") 2>&1
rm -f "$out/gate.json"
modes=(normal sanitizer)
if [ "$scope" = --update-smoke ]; then
    modes=(normal)
fi
probe pacman -Q libpaper >"$out/package.txt"
for mode in "${modes[@]}"; do
    flags=(-O2 -g -Wall -Wextra -Werror)
    if [ "$mode" = sanitizer ]; then
        flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    fi
    probe gcc "${flags[@]}" "$src/tools/test-libpaper.c" -lpaper -o "$out/lifecycle-$mode"
    if [ "$scope" = --update-smoke ]; then
        probe "$out/lifecycle-$mode" "$out/fixture-$mode"
    else
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 timeout 30 "$out/lifecycle-$mode" "$out/fixture-$mode"
    fi
done
probe python3 -I - "$out/gate.json" "$scope" <<'PY'
import json, sys
proof = {'name': 'libpaper', 'id': 'libpaper-lifecycle-v1', 'passed': True,
         'tests': ['repeated-init-done', 'catalog-release', 'xdg-ownership']}
if sys.argv[2] == '--update-smoke':
    proof.update(id='libpaper-lifecycle-smoke-v1', scope='update-smoke')
else:
    proof['tests'].append('asan-ubsan')
with open(sys.argv[1], 'w') as result:
    json.dump(proof, result, sort_keys=True)
PY
if [ "$scope" = --update-smoke ]; then
    printf '%s\n' 'PASS libpaper lifecycle, catalog cleanup and ownership'
else
    printf '%s\n' 'PASS libpaper lifecycle, catalog cleanup and ownership under sanitizers'
fi
