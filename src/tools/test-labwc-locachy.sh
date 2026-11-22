#!/bin/bash
# Start the installed compositor with no seat and verify that the LOCachy
# clipboard guard initializes. The focused compositor contracts and broader
# device/interaction regressions remain separate developer-only checks.
set -euo pipefail
out=${1:?usage: test-labwc-locachy.sh RESULT_DIRECTORY}
rm -f -- "$out/gate.json"
runtime=$(mktemp -d /run/locachy-labwc-smoke.XXXXXXXX)
comp=
cleanup() {
    if [ -n "$comp" ]; then
        kill "$comp" 2>/dev/null || true
        wait "$comp" 2>/dev/null || true
    fi
    rm -rf -- "$runtime"
}
trap cleanup EXIT
export XDG_RUNTIME_DIR="$runtime" WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1
printf '%s\n' '<labwc_config/>' >"$runtime/rc.xml"
/usr/bin/labwc -C "$runtime" -c "$runtime/rc.xml" >"$out/labwc.log" 2>&1 &
comp=$!
ready=0
for ((i = 0; i < 100; i++)); do
    kill -0 "$comp" 2>/dev/null || break
    if [ -S "$runtime/wayland-0" ] && [ -S "$runtime/locachy-clipboard-guard.sock" ]; then
        ready=1
        break
    fi
    sleep .05
done
if [ "$ready" != 1 ]; then
    echo 'FAIL labwc did not start with the LOCachy clipboard guard' >&2
    tail -n 40 "$out/labwc.log" >&2
    exit 1
fi
python3 -I - "$out/gate.json" <<'PY'
import json, sys
with open(sys.argv[1], 'w') as output:
    json.dump({'name': 'labwc-locachy', 'id': 'labwc-locachy-update-smoke-v1',
               'scope': 'update-smoke', 'passed': True,
               'tests': ['headless-start', 'clipboard-guard-socket']}, output, sort_keys=True)
PY
printf '%s\n' 'PASS labwc starts headlessly with the LOCachy clipboard guard'
