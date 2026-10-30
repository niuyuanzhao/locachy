#!/bin/bash
# Installed candidate lifecycle, no display or devices; LSan must stay on.
set -euo pipefail
out=${1:?usage: test-libpaper.sh RESULT_DIRECTORY}
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
exec > >(tee "$out/library.log") 2>&1
pacman -Q libpaper >"$out/package.txt"
for mode in normal sanitizer; do
    flags=(-O2 -g -Wall -Wextra -Werror)
    if [ "$mode" = sanitizer ]; then
        flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    fi
    gcc "${flags[@]}" "$src/tools/test-libpaper.c" -lpaper -o "$out/lifecycle-$mode"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 timeout 30 "$out/lifecycle-$mode" "$out/fixture-$mode"
done
python3 -I - "$out/gate.json" <<'PY'
import json, sys
json.dump({'name': 'libpaper', 'id': 'libpaper-lifecycle-v1', 'passed': True,
           'tests': ['repeated-init-done', 'catalog-release', 'xdg-ownership', 'asan-ubsan']},
          open(sys.argv[1], 'w'), sort_keys=True)
PY
printf '%s\n' 'PASS libpaper lifecycle, catalog cleanup and ownership under sanitizers'
