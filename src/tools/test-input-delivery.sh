#!/bin/bash
# Real headless keyboard delivery, no physical input, host install or lock action.
set -euo pipefail
runtime=${1:?usage: test-input-delivery.sh RUNTIME_ROOT NEW_OUTPUT_DIR}
out=${2:?}
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
runtime=$(realpath -e -- "$runtime")
[ ! -e "$out" ] || { printf 'Output already exists: %s\n' "$out" >&2; exit 2; }
mkdir -m 0700 -p "$out"
out=$(realpath -e -- "$out")
case "$out" in *[!a-zA-Z0-9_./-]*) printf 'Output path contains unsupported characters\n' >&2; exit 2;; esac
[ -x "$runtime/usr/bin/labwc" ] && [ -x "$runtime/usr/lib/ld-linux-x86-64.so.2" ]
mkdir "$out/proto" "$out/config" "$out/runtime" "$out/bin"
chmod 0700 "$out/runtime"
for p in wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 xdg-shell \
    wlr-foreign-toplevel-management-unstable-v1 fractional-scale-v1 viewporter \
    relative-pointer-unstable-v1 wlr-virtual-pointer-unstable-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$out/proto/$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$out/proto/$p-protocol.c"
done
flags=(-O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -ffunction-sections -fdata-sections
       -I"$src/base/include" -I"$src/base" -I"$src/tools" -I"$out/proto")
gcc "${flags[@]}" "$src/tools/test-input-delivery-client.c" "$out/proto/xdg-shell-protocol.c" \
    -Wl,--gc-sections -lwayland-client -lxkbcommon -o "$out/bin/client"
gcc "${flags[@]}" "$src/tools/test-keys.c" "$out/proto/virtual-keyboard-unstable-v1-protocol.c" \
    -Wl,--gc-sections -lwayland-client -lxkbcommon -o "$out/bin/keys"
gcc "${flags[@]}" "$src/tools/test-click.c" "$out/proto/wlr-virtual-pointer-unstable-v1-protocol.c" \
    -Wl,--gc-sections -lwayland-client -o "$out/bin/click"
gcc "${flags[@]}" -U_GNU_SOURCE "$src/tools/test-seat.c" "$out/proto/virtual-keyboard-unstable-v1-protocol.c" \
    "$out/proto/wlr-virtual-pointer-unstable-v1-protocol.c" -Wl,--gc-sections \
    -lwayland-client -lxkbcommon -o "$out/bin/seat"
gcc "${flags[@]}" $(pkg-config --cflags gio-2.0) "$src/tools/test-bar-keyboard.c" "$src/base/keys.c" \
    "$out"/proto/*-protocol.c -Wl,--gc-sections $(pkg-config --libs gio-2.0) \
    -lwayland-client -lwayland-cursor -lxkbcommon -o "$out/bin/bar-driver"

rime=0
if [ -f "$runtime/usr/include/rime_api.h" ] && [ -f "$runtime/usr/lib/librime.so" ] && command -v bwrap >/dev/null; then
    gcc "${flags[@]}" -DCONFIG_IME -I"$runtime/usr/include" $(pkg-config --cflags gio-2.0) \
        "$src/tools/test-bar-keyboard.c" "$src/base/keys.c" "$src/base/ime.c" "$out"/proto/*-protocol.c \
        -Wl,--gc-sections -Wl,-rpath-link,"$runtime/usr/lib" -L"$runtime/usr/lib" \
        $(pkg-config --libs gio-2.0) -lwayland-client -lwayland-cursor -lxkbcommon -lrime -o "$out/bin/rime-driver"
    rime=1
else
    printf 'SKIP actual Rime: runtime headers/library or unprivileged bwrap unavailable\n'
fi

export XDG_RUNTIME_DIR=$out/runtime XDG_CONFIG_HOME=$out/config WAYLAND_DISPLAY=wayland-0
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=2 XKB_DEFAULT_LAYOUT=us
cat >"$out/bin/mark" <<EOF
#!/bin/sh
printf 'action\\n' >>"$out/global-actions"
EOF
chmod 0700 "$out/bin/mark"
# The input method key's binding as the tracked compositor configuration has it.
ime_bind=$(grep -m 1 'key="IME-KEY"' "$src/link/labwc-rc.xml" | sed 's/IME-KEY/C-space/')
[ -n "$ime_bind" ]
printf '<labwc_config><keyboard><keybind key="W-Escape"><action name="Execute" command="%s/bin/mark"/></keybind>%s</keyboard></labwc_config>\n' \
    "$out" "$ime_bind" >"$out/config/rc.xml"
comp= client= seat=
cleanup() {
    [ -z "$seat" ] || kill "$seat" 2>/dev/null || true
    [ -z "$client" ] || kill "$client" 2>/dev/null || true
    [ -z "$comp" ] || kill "$comp" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT
"$runtime/usr/lib/ld-linux-x86-64.so.2" --library-path "$runtime/usr/lib" "$runtime/usr/bin/labwc" \
    -C "$out/config" -c "$out/config/rc.xml" >"$out/labwc.log" 2>&1 &
comp=$!
for _ in {1..80}; do
    kill -0 "$comp" || { cat "$out/labwc.log"; exit 1; }
    [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] || break
    sleep 0.1
done
[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]
"$out/bin/client" "$out/client.log" >"$out/client.stdout" 2>&1 &
client=$!
"$out/bin/seat" >"$out/seat.stdout" 2>&1 &
seat=$!
# The persistent virtual keyboard exposes keyboard capability on the headless seat.
timeout 10 "$out/bin/keys" F12
for _ in {1..60}; do grep -q '^FOCUS' "$out/client.log" 2>/dev/null && break; sleep 0.1; done
grep -q '^FOCUS' "$out/client.log"
# Its configured compositor action is harmless and counted locally, never home/lock.
timeout 10 "$out/bin/keys" Super+Escape Right
for _ in {1..60}; do [ -f "$out/global-actions" ] && break; sleep 0.1; done
[ "$(wc -l <"$out/global-actions")" -eq 1 ]
# Direct-input mode: the bar acts on the input method key; the compositor only consumes it.
timeout 10 "$out/bin/keys" Ctrl+space Right
for _ in {1..60}; do [ "$(grep -c '^KEY 106 0' "$out/client.log")" -ge 2 ] && break; sleep 0.1; done
wc -l <"$out/client.log" >"$out/ime-mark"
# The maximized client covers the first 1280x720 headless output.
timeout 10 "$out/bin/click" 640 360 2560 720 left

timeout 25 "$out/bin/bar-driver" --headless >"$out/bar.log" 2>&1
if [ "$rime" = 1 ]; then
    bwrap --unshare-all --die-with-parent --ro-bind "$runtime" / --proc /proc --dev /dev --tmpfs /tmp \
        --bind "$out" /work --setenv HOME /work --setenv XDG_RUNTIME_DIR /work/runtime \
        --setenv WAYLAND_DISPLAY wayland-0 --setenv XKB_DEFAULT_LAYOUT us \
        /usr/bin/timeout 35 /work/bin/rime-driver --headless /work/rime-schema /work/rime-user >"$out/rime.log" 2>&1
fi
sleep 0.3
kill -0 "$comp"
kill -0 "$client"
python3 -I - "$out" "$rime" <<'PY'
from pathlib import Path
import json, sys
root = Path(sys.argv[1])
text = (root / 'client.log').read_text()
keys = [line.split(' ', 3) for line in text.splitlines() if line.startswith('KEY ')]
assert not any(int(row[1]) == 1 for row in keys), 'consumed Escape press/release leaked'
before_ime_mark = text.splitlines()[:int((root / 'ime-mark').read_text())]
assert not any(line.startswith('KEY 57 ') for line in before_ime_mark), 'consumed input method Space leaked'
assert sum(line.startswith('KEY 106 1') for line in before_ime_mark) == 2, 'Right after Ctrl+Space missing'
assert (root / 'global-actions').read_text().splitlines() == ['action'], 'global action did not fire once'
held = {}
for _, code, value, *_ in keys:
    code, value = int(code), int(value)
    if value:
        assert not held.get(code), ('duplicate press', code)
        held[code] = True
    else:
        assert held.get(code), ('unmatched release', code)
        held[code] = False
assert not any(held.values()), ('keys left held', held)
assert any(row[1:3] == ['106', '1'] for row in keys), 'plain Right missing'
buttons = [line.split() for line in text.splitlines() if line.startswith('BUTTON ')]
assert buttons == [['BUTTON', '272', '1'], ['BUTTON', '272', '0']], ('plain click missing/unbalanced', buttons)
received = ''.join(row[3] for row in keys if row[2] == '1' and len(row) == 4)
assert '你好' in received, ('virtual UTF-8 commit missing', received)
actual_rime = sys.argv[2] == '1'
if actual_rime:
    assert received.count('你好') >= 3, ('actual CN commit missing beyond two synthetic commits', received)
    assert 'hello' in received, ('actual EN commit missing', received)
    assert 'PASS real Rime CN/EN composition and commit' in (root / 'rime.log').read_text()
# A keyboard with its own layout (us) beside the system's (jp): each key in
# its keyboard's keymap, the modifiers of both keyboards, one Caps Lock.
pressed = [(row[1], row[3]) for row in keys if row[2] == '1' and len(row) == 4]
assert ('3', '"') in pressed and ('3', '@') in pressed, ('per-keyboard layouts', [p for p in pressed if p[0] == '3'])
assert ('30', 'A') in pressed, 'Caps Lock of one keyboard missing on the other'
depressed, ctrl_c = 0, False
for line in text.splitlines():
    fields = line.split(' ')
    if fields[0] == 'MOD':
        depressed = int(fields[1])
    elif fields[:3] == ['KEY', '46', '1'] and depressed & 4:
        ctrl_c = True
assert ctrl_c, 'Ctrl of one keyboard missing on the other'
print('PASS focused client: Super+Escape consumed, action once, no Escape press or unmatched release')
print('PASS two keyboard layouts at once: " and @ on the same key, Ctrl and Caps Lock across keyboards')
print('PASS direct-input mode: the configured input method key is consumed by the compositor binding')
print('PASS ordinary Right and left click reach the focused client with balanced state')
print('PASS actual virtual UTF-8 commit, duplicate physical keycodes, unplug and output reset delivery')
if actual_rime:
    print('PASS actual librime CN and EN commit reaches a standard client')
(root / 'result.json').write_text(json.dumps({'passed': True, 'global_shortcut': True,
    'balanced_keyboard': True, 'plain_click': True, 'actual_rime': actual_rime, 'two_layouts': True},
    sort_keys=True) + '\n')
PY
printf 'Input delivery evidence: %s\n' "$out"
