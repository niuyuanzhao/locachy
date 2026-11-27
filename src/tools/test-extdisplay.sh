#!/bin/sh
# The extended-display receiver's focused tests (tools/EXTDISPLAY.md): protocol
# cases, then real FFmpeg decoding of H.264 and HEVC streams made by the
# installed ffmpeg's encoders, through the receiver's demux and decoder.
# On Linux it also builds the worker and the client (sandbox, memfd, IPC).
# Nothing is installed; no VM, no display, no network.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/locachy-extdisplay.XXXXXX")
trap 'rm -rf "$out"' EXIT
pkgs="json-c libavcodec libavutil libswscale"
if ! pkg-config --exists $pkgs; then
    echo "SKIP: $pkgs are not all installed" >&2
    exit 77
fi
cflags="-std=c11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I$src/progs"
extra=
if [ "$(uname -s)" = Linux ]; then
    extra="$src/progs/extdisplay-client.c"
fi
cc $cflags -DCONFIG_EXTDISPLAY_FFMPEG $(pkg-config --cflags $pkgs) "$src/tools/test-extdisplay.c" \
    "$src/progs/extdisplay-protocol.c" "$src/progs/extdisplay-decode.c" $extra $(pkg-config --libs $pkgs) -lm -o "$out/test-extdisplay"
"$out/test-extdisplay"
if [ "$(uname -s)" = Linux ]; then
    va_flags= va_libs=
    if pkg-config --exists libva libva-drm; then
        va_flags="-DCONFIG_EXTDISPLAY_VAAPI $(pkg-config --cflags libva libva-drm)"
        va_libs="$(pkg-config --libs libva libva-drm)"
        if pkg-config --exists gbm; then
            va_flags="$va_flags -DCONFIG_EXTDISPLAY_GBM $(pkg-config --cflags gbm)"
            va_libs="$va_libs $(pkg-config --libs gbm)"
        fi
    fi
    cc $cflags -O3 -DCONFIG_EXTDISPLAY_FFMPEG $va_flags $(pkg-config --cflags $pkgs) "$src/progs/extdisplay-worker.c" \
        "$src/progs/extdisplay-protocol.c" "$src/progs/extdisplay-decode.c" $(pkg-config --libs $pkgs) $va_libs -lm -o "$out/extdisplay-worker"
    cc $cflags -c "$src/progs/extdisplay-client.c" -o "$out/extdisplay-client.o"
    echo "PASS: extdisplay-worker and extdisplay-client build"
    # The worker's Caps Lock and framing cases: the same worker built for tests only (no
    # sandbox, so an unprivileged runner can start it), driven over real channels.
    cc $cflags -O2 -DEXTDISPLAY_WORKER_TEST_NO_SANDBOX -DCONFIG_EXTDISPLAY_FFMPEG $va_flags $(pkg-config --cflags $pkgs) \
        "$src/progs/extdisplay-worker.c" "$src/progs/extdisplay-protocol.c" "$src/progs/extdisplay-decode.c" \
        $(pkg-config --libs $pkgs) $va_libs -lm -o "$out/extdisplay-worker-test"
    "$out/test-extdisplay" worker "$out/extdisplay-worker-test"
    # The pairing client against a stub broker: a real child process, fd 3 only.
    cc $cflags -I"$src/base/include" -I"$src/base" "$src/tools/test-extdisplay-setup.c" "$src/progs/extdisplay-setup.c" \
        -o "$out/test-extdisplay-setup"
    "$out/test-extdisplay-setup"
    # Without its two channels the worker must refuse to run (exit 78), never decode.
    rc=0
    "$out/extdisplay-worker" 3<&- 4<&- </dev/null >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 78 ]; then
        echo "PASS: extdisplay-worker refuses to run without its channels"
    else
        echo "FAIL: extdisplay-worker exited $rc without its channels (expected 78)"
        exit 1
    fi
    # Raw input: local bindings, autorepeat and cancellation through evdev.
    cat >"$out/raw.c" <<'EOF'
#include "base.h"
#include "input-raw.h"
#include "internal.h"
#include <assert.h>
#include <linux/input.h>
#include <stdio.h>
#include <unistd.h>
void say(const char *fmt, ...) { (void)fmt; }
int rc_read(void (*fn)(const char *, const char *, void *), void *arg) { (void)fn; (void)arg; return 0; }
static int key(const int p[2], unsigned short code, int value)
{
    struct input_event ev = { .type = EV_KEY, .code = code, .value = value };
    assert(write(p[1], &ev, sizeof ev) == (ssize_t)sizeof ev);
    return keys_event(p[0]);
}

static struct base_raw_key expect_key(uint32_t code, uint32_t state)
{
    struct base_raw_key rk;
    assert(base_raw_next(&rk) && rk.code == code && rk.state == state);
    return rk;
}

static void expect_routed(const int p[2], uint16_t code, int state, int action,
                          uint32_t mods, int local)
{
    assert(key(p, code, state) == action);
    struct base_raw_key e = expect_key(code, (uint32_t)state);
    assert(e.modifiers == mods && !!(e.flags & BASE_RAW_LOCAL) == local);
}

int main(void) {
    static const struct {
        uint16_t prefix[2], code;
        uint32_t mods;
        int action, local;
    } cases[] = {
        { {KEY_LEFTMETA}, KEY_SPACE, KEYS_META, K_ACT_SEARCH, 1 },
        { {KEY_LEFTMETA}, KEY_ESC, KEYS_META, K_ACT_HOME, 1 },
        { {KEY_LEFTCTRL, KEY_LEFTALT}, KEY_LEFTMETA, KEYS_CTRL | KEYS_ALT, K_ACT_HOME, 1 },
        { {KEY_LEFTMETA}, KEY_L, KEYS_META, K_ACT_LOCK, 1 },
        { {KEY_LEFTCTRL}, KEY_SPACE, KEYS_CTRL, K_CTRL_SPACE, 0 },
        { {KEY_LEFTCTRL}, KEY_Q, KEYS_CTRL, 17, 0 },
        { {KEY_LEFTCTRL, KEY_LEFTALT}, KEY_DELETE, KEYS_CTRL | KEYS_ALT, K_DEL, 0 },
        { {0}, KEY_VOLUMEUP, 0, K_VOL_UP, 1 },
        { {0}, KEY_VOLUMEDOWN, 0, K_VOL_DOWN, 1 },
        { {0}, KEY_MUTE, 0, K_VOL_MUTE, 1 },
        { {0}, KEY_MICMUTE, 0, K_MIC_MUTE, 1 },
        { {0}, KEY_BRIGHTNESSUP, 0, K_BRI_UP, 1 },
        { {0}, KEY_BRIGHTNESSDOWN, 0, K_BRI_DOWN, 1 },
        { {0}, KEY_VENDOR, 0, K_ACT_HOME, 1 },
        { {0}, KEY_A, 0, 'a', 0 },
    };
    int p[2];
    assert(pipe(p) == 0);
    for (int mode = BASE_RAW_SHARED; mode <= BASE_RAW_EXCLUSIVE; mode++) {
        base_raw_enable(mode);
        int shared = mode == BASE_RAW_SHARED;
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            for (unsigned j = 0; j < 2 && cases[i].prefix[j]; j++) {
                assert(key(p, cases[i].prefix[j], 1) == 0);
                assert(!expect_key(cases[i].prefix[j], 1).flags);
            }
            uint32_t mods = cases[i].mods;
            int local = shared && cases[i].local;
            expect_routed(p, cases[i].code, 1, shared ? cases[i].action : 0, mods, local);
            /* The modifier-only Home binding also holds Super until release. */
            if (cases[i].code == KEY_LEFTMETA) mods |= KEYS_META;
            expect_routed(p, cases[i].code, 0, 0, mods, local);
            /* Release the chord's modifiers and check those events too. */
            for (int j = 1; j >= 0; j--) if (cases[i].prefix[j]) {
                assert(key(p, cases[i].prefix[j], 0) == 0);
                assert(!expect_key(cases[i].prefix[j], 0).flags);
            }
            assert(!base_raw_pending());
        }
    }
    puts("PASS: exclusive raw mode forwards Super+Space, all other tested bindings, and ordinary keys without local actions");

    key(p, KEY_LEFTMETA, 1);
    key(p, KEY_LEFTMETA, 2);
    key(p, KEY_A, 1);
    key(p, KEY_A, 2);
    key(p, KEY_LEFTMETA, 2);
    key(p, KEY_A, 0);
    key(p, KEY_LEFTMETA, 0);
    assert(expect_key(KEY_LEFTMETA, 1).modifiers == 0);
    assert(expect_key(KEY_A, 1).modifiers == KEYS_META);
    assert(expect_key(KEY_A, 2).modifiers == KEYS_META);
    expect_key(KEY_A, 0);
    assert(expect_key(KEY_LEFTMETA, 0).modifiers == KEYS_META);
    key(p, KEY_A, 1);
    key(p, KEY_A, 0);
    assert(expect_key(KEY_A, 1).modifiers == 0);
    assert(expect_key(KEY_A, 0).modifiers == 0);
    assert(!base_raw_pending());
    puts("PASS: ordinary keys repeat, but held Meta does not emit raw repeats or stay held after release");

    key(p, KEY_LEFTMETA, 1);
    expect_key(KEY_LEFTMETA, 1);
    base_raw_cancel();
    expect_key(0, 3);
    assert(keys_modifiers() == KEYS_META); /* Local state still sees the held key. */
    key(p, KEY_A, 1);
    key(p, KEY_A, 2);
    key(p, KEY_LEFTMETA, 0);
    key(p, KEY_A, 0);
    assert(expect_key(KEY_A, 1).modifiers == 0);
    assert(expect_key(KEY_A, 2).modifiers == 0);
    assert(expect_key(KEY_A, 0).modifiers == 0);
    assert(!base_raw_pending());
    puts("PASS: raw cancel discards held modifiers without changing local state");

    /* Release safety: switching back to shared/off flushes queued raw events
     * and held keys, and restores local action dispatch. */
    assert(base_raw_enable(BASE_RAW_EXCLUSIVE) == BASE_RAW_EXCLUSIVE);
    key(p, KEY_LEFTMETA, 1);
    key(p, KEY_SPACE, 1);
    assert(base_raw_pending());
    assert(base_raw_enable(BASE_RAW_SHARED) == BASE_RAW_EXCLUSIVE);
    assert(!base_raw_pending());
    key(p, KEY_SPACE, 0);
    key(p, KEY_LEFTMETA, 0);
    assert(!base_raw_pending());
    key(p, KEY_LEFTMETA, 1);
    assert(!expect_key(KEY_LEFTMETA, 1).flags);
    expect_routed(p, KEY_SPACE, 1, K_ACT_SEARCH, KEYS_META, 1);
    expect_routed(p, KEY_SPACE, 0, 0, KEYS_META, 1);
    key(p, KEY_LEFTMETA, 0);
    assert(!expect_key(KEY_LEFTMETA, 0).flags);
    assert(!base_raw_pending());
    puts("PASS: leaving exclusive mode flushes held raw state and restores local bindings");

    /* SYN_DROPPED and keys_release() each cancel the held keys once: the
     * releases that follow are not sent again, and nothing stays pending. */
    assert(base_raw_enable(BASE_RAW_EXCLUSIVE) == BASE_RAW_SHARED);
    key(p, KEY_LEFTMETA, 1);
    key(p, KEY_SPACE, 1);
    expect_key(KEY_LEFTMETA, 1);
    expect_key(KEY_SPACE, 1);
    struct input_event dropped = { .type = EV_SYN, .code = SYN_DROPPED };
    assert(write(p[1], &dropped, sizeof dropped) == (ssize_t)sizeof dropped);
    assert(keys_event(p[0]) == 0);
    expect_key(0, 3);
    key(p, KEY_SPACE, 0);
    key(p, KEY_LEFTMETA, 0);
    assert(!base_raw_pending());
    key(p, KEY_LEFTMETA, 1);
    expect_key(KEY_LEFTMETA, 1);
    keys_release();
    expect_key(0, 3);
    key(p, KEY_LEFTMETA, 0);
    assert(!base_raw_pending() && keys_modifiers() == 0);
    puts("PASS: SYN_DROPPED and keys_release cancel held raw keys once; late releases are not sent again");

    base_raw_enable(BASE_RAW_OFF);
    close(p[0]);
    close(p[1]);
    return 0;
}
EOF
    cc $cflags -DKEYS_TABLE=\"/nonexistent/keymap.table\" -ffunction-sections -fdata-sections \
        -I"$src/base/include" -I"$src/base" "$out/raw.c" "$src/base/keys.c" "$src/base/input-raw.c" \
        "$src/base/key-action.c" -Wl,--gc-sections -lm -o "$out/raw"
    "$out/raw"
    cc $cflags -DCONFIG_EXTDISPLAY_FFMPEG -DEXTDISPLAY_DECODE_TEST -ffunction-sections -fdata-sections \
        -I"$src/base/include" -I"$src/base" $(pkg-config --cflags $pkgs) \
        "$src/tools/test-extdisplay-dmabuf.c" "$src/progs/extdisplay-decode.c" "$src/progs/extdisplay-protocol.c" \
        "$src/base/key-action.c" "$src/base/help.c" \
        $(pkg-config --libs $pkgs) -lm -Wl,--gc-sections -o "$out/test-extdisplay-dmabuf"
    "$out/test-extdisplay-dmabuf"
else
    echo "SKIP: extdisplay-worker, extdisplay-client and the IPC cases need Linux (Landlock, seccomp, memfd seals)"
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "SKIP: no ffmpeg to make the test streams" >&2
    exit 0
fi
lavfi="testsrc2=size=320x240:rate=30"
if ffmpeg -hide_banner -loglevel error -f lavfi -i "$lavfi" -frames:v 12 -an -c:v libx264 -bf 0 -g 6 \
    -pix_fmt yuv420p -f h264 "$out/test.h264"; then
    "$out/test-extdisplay" h264 320 240 "$out/test.h264"
else
    echo "SKIP: ffmpeg has no libx264"
fi
if ffmpeg -hide_banner -loglevel error -f lavfi -i "$lavfi" -frames:v 12 -an -c:v libx265 \
    -x265-params log-level=error:bframes=0:keyint=6 -pix_fmt yuv420p -f hevc "$out/test.hevc"; then
    "$out/test-extdisplay" hevc 320 240 "$out/test.hevc"
else
    echo "SKIP: ffmpeg has no libx265"
fi
