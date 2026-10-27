#!/bin/bash
# test-shot.sh: locachy-shot on a headless labwc with two screens (1 and 1.5),
# as root on a built system. Uses virtual keyboard and pointer (no uinput,
# empty /dev/input) in private mount and PID namespaces.
#   1. locachy-shot -f -b -C -o $out/full.png: full desktop capture stitched
#      across outputs (2133x720) with window colours, path copied to clipboard;
#      -f -b -c: the same image as image/png in the clipboard, nothing saved
#   2. locachy-shot -m -b -o $out/current.png: current screen capture
#   3. locachy-shot -a -b -o $out/active.png: active window capture via
#      ext-foreign-toplevel-image-capture-source-v1, exact window size (160x100)
#      and colour (4488cc)
#   4. Interactive region mode (locachy-shot -r): drag a region (300x200), the
#      toolbar's buttons inside its frame on both screens, draw an arrow,
#      Return: the 300x200 image with the arrow (da4453) in the clipboard and
#      no file; again with Ctrl+S: ~/Pictures/Screenshots/Screenshot_*.png,
#      300x200, owned by uid 1000
#   5. Escape clears selection first, second Escape cancels without writing a
#      file
# Tested with both pixman and gles2 (XBGR8888) renderers.
set -u
src=$(cd "$(dirname "$0")/.." && pwd)
out=/var/tmp/locachy-shot-test
rm -rf "$out"
mkdir -p "$out"
cd "$out"

for p in wlr-foreign-toplevel-management-unstable-v1 wlr-layer-shell-unstable-v1 virtual-keyboard-unstable-v1 \
         xdg-shell wlr-screencopy-unstable-v1 wlr-virtual-pointer-unstable-v1 fractional-scale-v1 viewporter \
         relative-pointer-unstable-v1 ext-foreign-toplevel-list-v1 ext-image-capture-source-v1 \
         ext-image-copy-capture-v1 security-context-v1 wlr-output-management-unstable-v1 ext-idle-notify-v1 \
         ext-data-control-v1; do
    wayland-scanner client-header "$src/base/protocols/$p.xml" "$p-client-protocol.h"
    wayland-scanner private-code "$src/base/protocols/$p.xml" "$p-protocol.c"
done
cc="gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -I$src/base/include -I$src/base -I$out $(pkg-config --cflags freetype2 libnm)"
libs="$(pkg-config --libs freetype2 libnm) -lwayland-client -lm"
base=$(ls "$src"/base/*.c | grep -v '/ime.c$')
$cc -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -o labwc-control "$src/progs/labwc-control.c" $base \
    security-context-v1-protocol.c wlr-foreign-toplevel-management-unstable-v1-protocol.c \
    wlr-output-management-unstable-v1-protocol.c ext-idle-notify-v1-protocol.c $libs || exit 1
$cc -o clipboard "$src/progs/clipboard.c" "$src/base/clipboard.c" ext-data-control-v1-protocol.c -lwayland-client || exit 1
$cc -DCONFIG_FREETYPE -o locachy-shot "$src/progs/shot.c" $base \
    wlr-layer-shell-unstable-v1-protocol.c wlr-foreign-toplevel-management-unstable-v1-protocol.c \
    wlr-output-management-unstable-v1-protocol.c ext-foreign-toplevel-list-v1-protocol.c \
    ext-image-capture-source-v1-protocol.c ext-image-copy-capture-v1-protocol.c \
    fractional-scale-v1-protocol.c viewporter-protocol.c xdg-shell-protocol.c \
    $libs -lwayland-cursor -lxkbcommon -lpng || exit 1
$cc -o test-window "$src/tools/test-window.c" xdg-shell-protocol.c -lwayland-client || exit 1
$cc -o test-screenshot "$src/tools/test-screenshot.c" wlr-screencopy-unstable-v1-protocol.c -lwayland-client || exit 1
$cc -o test-click "$src/tools/test-click.c" wlr-virtual-pointer-unstable-v1-protocol.c -lwayland-client || exit 1
$cc -o test-keys "$src/tools/test-keys.c" virtual-keyboard-unstable-v1-protocol.c -lwayland-client -lxkbcommon || exit 1
$cc -DCONFIG_FREETYPE -DCONFIG_NETWORKMANAGER -DCONFIG_IME -o locachy-bar "$src/progs/bar.c" \
    $base "$src/base/ime.c" wlr-layer-shell-unstable-v1-protocol.c virtual-keyboard-unstable-v1-protocol.c \
    xdg-shell-protocol.c wlr-foreign-toplevel-management-unstable-v1-protocol.c \
    fractional-scale-v1-protocol.c viewporter-protocol.c relative-pointer-unstable-v1-protocol.c \
    $libs -lrime -lxkbcommon -lwayland-cursor || exit 1

cat >test-drag.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *manager;

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
    (void)data; (void)v;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !seat)
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
        manager = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) { (void)data; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { global, global_remove };

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(int argc, char **argv)
{
    if (argc < 7) return 2;
    int x0 = atoi(argv[1]), y0 = atoi(argv[2]), x1 = atoi(argv[3]), y1 = atoi(argv[4]);
    uint32_t w = (uint32_t)atoi(argv[5]), h = (uint32_t)atoi(argv[6]);
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return 1;
    wl_registry_add_listener(wl_display_get_registry(dpy), &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!manager) return 1;
    struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
    wl_display_roundtrip(dpy);
    usleep(250000);
    zwlr_virtual_pointer_v1_motion_absolute(p, now_ms(), (uint32_t)x0, (uint32_t)y0, w, h);
    zwlr_virtual_pointer_v1_frame(p);
    wl_display_roundtrip(dpy);
    usleep(50000);
    zwlr_virtual_pointer_v1_button(p, now_ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(p);
    wl_display_roundtrip(dpy);
    int steps = 10;
    for (int i = 1; i <= steps; i++) {
        int cx = x0 + (x1 - x0) * i / steps;
        int cy = y0 + (y1 - y0) * i / steps;
        usleep(15000);
        zwlr_virtual_pointer_v1_motion_absolute(p, now_ms(), (uint32_t)cx, (uint32_t)cy, w, h);
        zwlr_virtual_pointer_v1_frame(p);
        wl_display_flush(dpy);
    }
    wl_display_roundtrip(dpy);
    usleep(50000);
    zwlr_virtual_pointer_v1_button(p, now_ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(p);
    wl_display_roundtrip(dpy);
    usleep(80000);
    zwlr_virtual_pointer_v1_destroy(p);
    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
EOF
$cc -o test-drag test-drag.c wlr-virtual-pointer-unstable-v1-protocol.c -lwayland-client || exit 1

cat >png-info.c <<'EOF'
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) return 1;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info || setjmp(png_jmpbuf(png))) return 1;
    png_init_io(png, fp);
    png_read_info(png, info);
    int w = (int)png_get_image_width(png, info);
    int h = (int)png_get_image_height(png, info);
    int ct = png_get_color_type(png, info);
    int bd = png_get_bit_depth(png, info);
    if (bd == 16) png_set_strip_16(png);
    if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    if (ct & PNG_COLOR_MASK_ALPHA) png_set_strip_alpha(png);
    png_read_update_info(png, info);

    uint8_t *row = malloc((size_t)w * 3);
    long count = 0;
    uint32_t target = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 16) : 0;
    for (int y = 0; y < h; y++) {
        png_read_row(png, row, NULL);
        if (argc > 2) {
            for (int x = 0; x < w; x++) {
                uint32_t c = ((uint32_t)row[x * 3] << 16) | ((uint32_t)row[x * 3 + 1] << 8) | row[x * 3 + 2];
                if (c == target) count++;
            }
        }
    }
    free(row);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    if (argc > 2) printf("%dx%d %ld\n", w, h, count);
    else printf("%dx%d\n", w, h);
    return 0;
}
EOF
$cc -o png-info png-info.c -lpng || exit 1

# The toolbar inside its frame: on every row with button pixels (BTN_BG,
# 31363b), the panel's border (HIGHLIGHT, 3daee9) is left and right of them.
# Prints the rows with buttons and the rows where a button sticks out.
cat >hud-inside.py <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
magic, size, depth, px = data.split(b'\n', 3)
w, h = map(int, size.split())
btn, hi = bytes.fromhex('31363b'), bytes.fromhex('3daee9')
rows = bad = 0
for y in range(h):
    row = px[y * w * 3:(y + 1) * w * 3]
    b = [x for x in range(w) if row[3 * x:3 * x + 3] == btn]
    if not b:
        continue
    e = [x for x in range(w) if row[3 * x:3 * x + 3] == hi]
    rows += 1
    if not e or min(e) >= min(b) or max(e) <= max(b):
        bad += 1
print(rows, bad)
EOF

mkdir -p etc
cp -a /etc/locachy/. etc/
rm -rf etc/containers && mkdir etc/containers
printf "HEADLESS-1=origin,1280x720,1\nHEADLESS-2=right,preferred,1.5\n" >etc/monitors.conf
python3 - "$src/link/labwc-rc.xml" etc/labwc/rc.xml <<'EOF'
import sys
s = open(sys.argv[1]).read().replace("TERMINAL-KEY", "C-A-t")
rules = """<windowRules>
  <windowRule title="First:*"><action name="MoveToOutput" output="HEADLESS-1"/></windowRule>
  <windowRule title="Second:*"><action name="MoveToOutput" output="HEADLESS-2"/></windowRule>
</windowRules>
"""
open(sys.argv[2], "w").write(s.replace("</openbox_config>", rules + "</openbox_config>"))
EOF

run_suite() {
    export out src SHOT_RENDERER="$1"
    echo "=== Renderer: $SHOT_RENDERER ==="
    unshare --mount --propagation private --pid --fork --kill-child --mount-proc bash -c '
    set -u
    fail=0
    check() { if eval "$2"; then echo "PASS [$SHOT_RENDERER] $1"; else echo "FAIL [$SHOT_RENDERER] $1"; fail=1; fi; }
    mkdir -p /run/user /run/locachy /dev/input
    mount -t tmpfs -o mode=0755 tmpfs /run/user
    mkdir -m 0700 /run/user/0
    export XDG_RUNTIME_DIR=/run/user/0
    mount -t tmpfs -o mode=0755 tmpfs /run/locachy
    mount -t tmpfs -o mode=0755 tmpfs /dev/input
    mount --bind $out/etc /etc/locachy
    mkdir -p $out/rime-upper $out/rime-work $out/bin-upper $out/bin-work
    mount -t overlay overlay -o lowerdir=/var/lib/locachy/rime,upperdir=$out/rime-upper,workdir=$out/rime-work \
        /var/lib/locachy/rime
    mount -t overlay overlay -o lowerdir=/usr/local/bin,upperdir=$out/bin-upper,workdir=$out/bin-work /usr/local/bin
    cp $out/locachy-shot /usr/local/bin/locachy-shot

    user_home=$(getent passwd 1000 | cut -d: -f6)
    [ -n "$user_home" ] || user_home=/home/tester
    mkdir -p "$user_home"
    mount -t tmpfs -o mode=0755,uid=1000,gid=1000 tmpfs "$user_home"

    WLR_BACKENDS=headless WLR_RENDERER=$SHOT_RENDERER WLR_HEADLESS_OUTPUTS=2 \
        labwc -C /etc/locachy/labwc -c /etc/locachy/labwc/rc.xml >$out/labwc-$SHOT_RENDERER.log 2>&1 &
    for i in $(seq 50); do [ -S /run/user/0/wayland-0 ] && break; sleep 0.1; done
    export WAYLAND_DISPLAY=wayland-0
    $out/clipboard --serve >$out/clipboard-$SHOT_RENDERER.log 2>&1 &
    $out/labwc-control >$out/control-$SHOT_RENDERER.log 2>&1 &
    for i in $(seq 50); do [ -S /run/user/0/locachy-0 ] && break; sleep 0.1; done
    $out/locachy-bar >$out/bar-$SHOT_RENDERER.log 2>&1 &
    sleep 1.2

    L="2133 720"
    up() { pgrep -x locachy-shot >/dev/null; }
    keys() { $out/test-keys "$@" 2>>$out/client.log; }
    click() { $out/test-click "$@" 2>>$out/client.log; }
    drag() { $out/test-drag "$@" 2>>$out/client.log; }

    $out/test-window --title "First: one" >$out/win-one &
    sleep 0.4
    $out/test-window --title "Second: two" >$out/win-two &
    sleep 0.8

    echo "-- 1. Full desktop capture (-f -b -C -o: saved, its path copied)"
    rm -f $out/full.png
    /usr/local/bin/locachy-shot -f -b -C -o $out/full.png
    info=$($out/png-info $out/full.png 4488cc 2>/dev/null || echo "none 0")
    dim=$(echo "$info" | cut -d" " -f1)
    pxs=$(echo "$info" | cut -d" " -f2)
    clip=$($out/clipboard get 2>/dev/null || true)
    check "full.png is valid 2133x720 PNG with window colour ($info) and path in clipboard ($clip)" \
        "[ \"$dim\" = 2133x720 ] && [ \"$pxs\" -ge 16000 ] && [ \"$clip\" = \"$out/full.png\" ]"

    echo "-- 1b. Full desktop straight to the clipboard (-f -b -c), nothing saved"
    rm -rf "$user_home/Pictures/Screenshots"
    /usr/local/bin/locachy-shot -f -b -c
    $out/clipboard get --png >$out/clip-full.png 2>/dev/null
    info=$($out/png-info $out/clip-full.png 4488cc 2>/dev/null || echo "none 0")
    dim=$(echo "$info" | cut -d" " -f1)
    pxs=$(echo "$info" | cut -d" " -f2)
    files=$(ls "$user_home"/Pictures/Screenshots 2>/dev/null | wc -l)
    check "clipboard image/png is the 2133x720 desktop ($info), no file saved ($files)" \
        "[ \"$dim\" = 2133x720 ] && [ \"$pxs\" -ge 16000 ] && [ \"$files\" = 0 ]"

    echo "-- 2. Current screen capture (-m -b -o)"
    rm -f $out/current.png
    /usr/local/bin/locachy-shot -m -b -o $out/current.png
    info=$($out/png-info $out/current.png 4488cc 2>/dev/null || echo "none 0")
    dim=$(echo "$info" | cut -d" " -f1)
    pxs=$(echo "$info" | cut -d" " -f2)
    check "current.png is single screen 1280x720 PNG with window colour ($info)" \
        "[ \"$dim\" = 1280x720 ] && [ \"$pxs\" -ge 10000 ]"

    echo "-- 3. Active window capture (-a -b -o)"
    rm -f $out/active.png
    /usr/local/bin/locachy-shot -a -b -o $out/active.png
    info=$($out/png-info $out/active.png 4488cc 2>/dev/null || echo "none 0")
    dim=$(echo "$info" | cut -d" " -f1)
    pxs=$(echo "$info" | cut -d" " -f2)
    check "active.png is exact window size 160x100 and colour 4488cc ($info)" \
        "[ \"$dim\" = 160x100 ] && [ \"$pxs\" = 16000 ]"

    echo "-- 4. Interactive region mode (-r): drag region, toolbar inside its frame, annotate, Return copies"
    rm -rf "$user_home/Pictures/Screenshots"
    $out/clipboard clear
    /usr/local/bin/locachy-shot -r >$out/shot-ui.log 2>&1 &
    for i in $(seq 30); do up && break; sleep 0.1; done
    sleep 0.6
    check "interactive locachy-shot -r overlay is up" "up"
    # Drag a 300x200 region from (100, 100) to (400, 300) on HEADLESS-1
    drag 100 100 400 300 $L
    sleep 0.3
    $out/test-screenshot $out/hud-$SHOT_RENDERER 2>>$out/client.log
    inside1=$(python3 -I $out/hud-inside.py $out/hud-$SHOT_RENDERER-HEADLESS-1.ppm 2>/dev/null || echo "0 -1")
    inside2=$(python3 -I $out/hud-inside.py $out/hud-$SHOT_RENDERER-HEADLESS-2.ppm 2>/dev/null || echo "0 -1")
    check "toolbar buttons inside the panel (rows, outside: $inside1 at 1, $inside2 at 1.5)" \
        "[ \"${inside1%% *}\" -gt 0 ] && [ \"${inside1##* }\" = 0 ] && [ \"${inside2##* }\" = 0 ]"
    # Press 1 to select Arrow annotation tool, drag inside region from (150, 150) to (300, 250)
    keys 1
    sleep 0.2
    drag 150 150 300 250 $L
    sleep 0.3
    # Return: the image to the clipboard, nothing saved
    keys Return
    for i in $(seq 30); do ! up && break; sleep 0.1; done
    $out/clipboard get --png >$out/clip-region.png 2>/dev/null
    info=$($out/png-info $out/clip-region.png da4453 2>/dev/null || echo "none 0")
    dim=$(echo "$info" | cut -d" " -f1)
    annopx=$(echo "$info" | cut -d" " -f2)
    files=$(ls "$user_home"/Pictures/Screenshots 2>/dev/null | wc -l)
    check "Return copied the 300x200 region with the arrow ($info) to the clipboard, no file ($files)" \
        "! up && [ \"$dim\" = 300x200 ] && [ \"$annopx\" -ge 100 ] && [ \"$files\" = 0 ]"

    echo "-- 4b. Interactive region mode: Ctrl+S saves the file"
    /usr/local/bin/locachy-shot -r >>$out/shot-ui.log 2>&1 &
    for i in $(seq 30); do up && break; sleep 0.1; done
    sleep 0.6
    drag 100 100 400 300 $L
    sleep 0.3
    keys Ctrl+s
    for i in $(seq 30); do ! up && break; sleep 0.1; done
    saved=$(ls "$user_home"/Pictures/Screenshots/Screenshot_*.png 2>/dev/null | head -n 1)
    info=$([ -n "$saved" ] && $out/png-info "$saved" 4488cc 2>/dev/null || echo "none 0")
    dim=$(echo "$info" | cut -d" " -f1)
    owner=$([ -n "$saved" ] && stat -c "%u:%g" "$saved" || echo "none")
    dir_owner=$([ -d "$user_home/Pictures/Screenshots" ] && stat -c "%u:%g" "$user_home/Pictures/Screenshots" || echo "none")
    check "saved $saved is 300x200 ($info) and owned by 1000:1000 ($owner, dir $dir_owner)" \
        "! up && [ -n \"$saved\" ] && [ \"$dim\" = 300x200 ] && [ \"$owner\" = 1000:1000 ] && [ \"$dir_owner\" = 1000:1000 ]"

    echo "-- 5. Escape clears selection, second Escape cancels without writing file"
    rm -rf "$user_home/Pictures/Screenshots"
    /usr/local/bin/locachy-shot -r >>$out/shot-ui.log 2>&1 &
    for i in $(seq 30); do up && break; sleep 0.1; done
    sleep 0.6
    drag 120 120 320 260 $L
    sleep 0.3
    keys Escape
    sleep 0.4
    check "first Escape clears selection and keeps overlay up" "up"
    keys Escape
    for i in $(seq 30); do ! up && break; sleep 0.1; done
    leftover=$(ls "$user_home"/Pictures/Screenshots/Screenshot_*.png 2>/dev/null | wc -l)
    check "second Escape exits without saving a file (files: $leftover)" "! up && [ \"$leftover\" = 0 ]"

    pkill -f "$out/test-window"
    exit $fail
    '
}

total_fail=0
run_suite pixman || total_fail=1
if ls /dev/dri/renderD* >/dev/null 2>&1; then
    run_suite gles2 || total_fail=1
else
    echo "SKIP [gles2]: no /dev/dri/renderD* node in environment"
fi
[ $total_fail = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $total_fail
