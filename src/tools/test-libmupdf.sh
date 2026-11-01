#!/bin/bash
# The libmupdf contract in a disposable target root: the candidate opens
# Markdown by itself (its bundled cmark-gfm gives headings their ids, so
# links to them resolve), still opens PDF and EPUB, and view-render builds
# against it and passes its own test.
set -euo pipefail
out=${1:?usage: test-libmupdf.sh RESULT_DIRECTORY}
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
exec > >(tee "$out/libmupdf.log") 2>&1
dir=/tmp/libmupdf-test
rm -rf "$dir"
mkdir -p "$dir"
gcc -O2 -Wall -Wextra -Werror "$src/tools/test-libmupdf.c" -o "$dir/alone" -lmupdf
# Nothing but libmupdf may supply cmark-gfm here.
ldd "$dir/alone" >"$out/alone.ldd"
if grep -q 'libcmark-gfm' "$out/alone.ldd"; then
    echo 'FAIL libmupdf loads a system cmark-gfm'
    exit 1
fi
timeout 60 "$dir/alone" "$dir"
# Our renderer, built against the candidate's headers and library. Its test
# runs it as tester (uid 1000) in /home/tester, the development root's
# account; the build root hides /home. Give this disposable root that
# account and home (the host's own uid 1000 keeps its files: only this
# copy of the root is renamed).
owner=$(getent passwd 1000 | cut -d: -f1 || true) # none yet in a new root
if [ -z "$owner" ]; then
    useradd -u 1000 -U -M -d /home/tester -s /usr/bin/nologin tester
elif [ "$(getent passwd 1000 | cut -d: -f1,6)" != tester:/home/tester ]; then
    usermod -l tester -d /home/tester "$owner"
fi
install -d -o 1000 -g 1000 -m 0700 /home/tester
timeout 400 sh "$src/tools/test-view-render.sh" >"$out/view-render.log" 2>&1 || {
    tail -n 40 "$out/view-render.log"
    exit 1
}
tail -n 3 "$out/view-render.log"
python3 -I - "$out/gate.json" <<'PY'
import json, sys
json.dump({'name': 'libmupdf', 'id': 'libmupdf-markdown-v1', 'passed': True,
           'tests': ['markdown-heading-anchors', 'pdf', 'epub', 'view-render']},
          open(sys.argv[1], 'w'), sort_keys=True)
PY
printf '%s\n' 'PASS libmupdf opens Markdown by itself; PDF, EPUB and view-render still work'
