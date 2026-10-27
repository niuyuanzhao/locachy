/*
 * locachy-shot (/usr/local/bin): the shared display's screenshot tool,
 * modelled after KDE Spectacle.
 *
 * Modes and options (matching Spectacle):
 *   (none), -g, --gui               Interactive Rectangular Region & HUD overlay
 *   -r, --region                    Interactive Rectangular Region selection
 *   -f, --fullscreen                Capture entire desktop (all outputs stitched)
 *   -m, --current                   Capture current screen (focused output)
 *   -a, --activewindow              Capture currently active window
 *   -u, --windowundercursor         Capture window under cursor (click in UI, or
 *                                   active window in -b background mode)
 *   -b, --background                Save immediately without showing UI and exit
 *   -c, --copy-image                With -b: the image to the clipboard (image/png),
 *                                   not saved, unless -o is also given
 *   -C, --copy-path                 With -b: saved, and its path to the clipboard
 *   -n, --nonotify                  Accepted for Spectacle CLI compatibility
 *   -o, --output FILE               Save PNG to FILE instead of default path
 *
 * Default save path:
 *   ~/Pictures/Screenshots/Screenshot_YYYYMMDD_HHMMSS.png (owned by uid 1000,
 *   with _1, _2 collision suffixes if taken).
 *
 * Interactive overlay (zwlr_layer_shell_v1 OVERLAY, exclusive keyboard):
 *   - Frozen screen capture darkened outside the selection (VEIL) and full
 *     brightness inside the selection.
 *   - Drag anywhere to select a rectangular region; drag inside to move it;
 *     drag any of the 8 handles (4 corners + 4 midpoints) to resize it.
 *   - Shows a pixel size badge (WxH (X, Y)) next to the selection, and a
 *     magnifier loupe with crosshair and pixel coordinates while dragging a
 *     corner or edge.
 *   - Annotation tools drawn onto the capture:
 *       1: Arrow   2: Rectangle   3: Freehand/Pen   4: Highlighter   5: Number
 *       Ctrl+Z / u: Undo last annotation
 *   - Toolbar (draw_hud) & keyboard actions:
 *       Enter / double-click inside selection / Ctrl+C / [ Copy ]: the image
 *         to the clipboard & exit (with nothing selected: the whole desktop)
 *       Ctrl+S / [ Save ]: saved to the default path & exit
 *       v / [ Open in view ]: saved & opened in view via open_in_home
 *       F: Full Screen   M: Current Screen   A: Active Window   W: Pick Window
 *       Escape: clears selection (or exits Pick Window) if present; second
 *               Escape (or q / right-click with no selection) cancels and exits.
 *   A failed copy or save keeps the overlay, said in the toolbar.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <math.h>
#include <png.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "base.h"
#include "clipboard.h"

#define VEIL       0xB3202326u /* Spectacle's darkened veil outside selection */
#define TEXT       0xFFFCFCFCu
#define DIM_TEXT   0xFFA0A8B0u
#define HIGHLIGHT  0xFF3DAEE9u /* KDE Breeze highlight blue */
#define PANEL_DARK 0xE6202326u /* HUD bar background */
#define BTN_BG     0xFF31363Bu /* HUD button background */
#define BTN_HOVER  0xFF4D545Eu
#define ANNO_RED   0xFFDA4453u /* Default annotation red/coral */
#define ANNO_YEL   0x80FDBC4Bu /* Translucent highlighter yellow */
#define HANDLE_R   5
#define MAX_ANNOS  256
#define MAX_PTS    2048
#define MAX_HITS   32

enum mode {
    MODE_GUI,
    MODE_REGION,
    MODE_FULLSCREEN,
    MODE_CURRENT,
    MODE_ACTIVEWIN,
    MODE_UNDERCURSOR,
};

enum tool {
    TOOL_NONE = 0,
    TOOL_ARROW = 1,
    TOOL_RECT = 2,
    TOOL_PEN = 3,
    TOOL_HILITE = 4,
    TOOL_NUMBER = 5,
};

enum drag_kind {
    DRAG_NONE = 0,
    DRAG_NEW,
    DRAG_MOVE,
    DRAG_NW, DRAG_N, DRAG_NE,
    DRAG_E,  DRAG_SE, DRAG_S,
    DRAG_SW, DRAG_W,
    DRAG_ANNO,
};

enum hit_id {
    HIT_MODE_REGION = 1,
    HIT_MODE_FULL,
    HIT_MODE_CURRENT,
    HIT_MODE_ACTIVE,
    HIT_MODE_WINDOW,
    HIT_TOOL_ARROW,
    HIT_TOOL_RECT,
    HIT_TOOL_PEN,
    HIT_TOOL_HILITE,
    HIT_TOOL_NUMBER,
    HIT_UNDO,
    HIT_SAVE,
    HIT_COPY,
    HIT_VIEW,
    HIT_CANCEL,
};

struct image {
    uint32_t *px;
    int w, h;
};

struct hit {
    int id;
    int x, y, w, h; /* in buffer pixels */
};

struct output {
    uint32_t name;
    struct wl_output *wl;
    char conn[64];
    int lx, ly, lw, lh;       /* layout geometry from output management or configure */
    struct wl_surface *surf;
    struct zwlr_layer_surface_v1 *layer;
    struct wp_fractional_scale_v1 *fscale;
    struct wp_viewport *viewport;
    uint32_t scale120, w, h;  /* logical size from layer_configure */
    int configured, dirty, keyboard;
    struct image bg, scaled;  /* captured raw output image, and scaled to drawn WxH */
    struct hit hits[MAX_HITS];
    int nhits, hovered_hit;
    struct output *next;
};

struct head_info {
    struct zwlr_output_head_v1 *head;
    char name[64];
    int enabled, x, y, w, h;
    double scale;
    struct head_info *next;
};

struct window;
struct capture {
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;
    struct wl_buffer *buffer;
    uint32_t *px;
    int w, h;
    int bw, bh;
    uint32_t format, bformat;
    int rank, done;
    uint32_t pending, offered;
    int pending_rank;
    struct image *into;
};

struct listed {
    struct ext_foreign_toplevel_handle_v1 *handle;
    char title[128], app_id[64];
    int paired;
    struct listed *next;
};

struct window {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char title[128], app_id[64];
    int minimized, activated;
    struct wl_output *outputs[4];
    int noutputs;
    struct listed *listed;
    struct capture cap;
    struct image img;
    struct window *next;
};

struct pt {
    int x, y;
};

struct annotation {
    enum tool type;
    struct output *out;
    int x0, y0, x1, y1; /* in buffer pixels of out */
    int num;
    struct pt *pts;
    int npts, cap_pts;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_foreign_toplevel_manager_v1 *toplevels;
static struct zwlr_output_manager_v1 *output_mgr;
static struct ext_foreign_toplevel_list_v1 *toplevel_list;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *window_sources;
static struct ext_output_image_capture_source_manager_v1 *output_sources;
static struct ext_image_copy_capture_manager_v1 *copier;
static struct wp_fractional_scale_manager_v1 *scale_mgr;
static struct wp_viewporter *viewporter;
static struct wl_cursor_theme *cursor_theme;
static struct wl_surface *cursor_surface;
static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_map;
static struct xkb_state *xkb_st;

static struct output *outputs;
static struct head_info *heads;
static struct window *windows;
static struct listed *listed;
static struct output *pointer_output;
static double pointer_x, pointer_y;
static uint32_t last_click_ms;
static double last_click_x, last_click_y;

static volatile sig_atomic_t stopping;
static int finished;
static int exit_code;

static enum mode opt_mode = MODE_GUI;
static int opt_background, opt_copy_image, opt_copy_path;
static const char *opt_output;

enum finish { FINISH_COPY, FINISH_SAVE, FINISH_VIEW };
static char status_msg[96]; /* a failed copy or save, in the toolbar */

/* Selection state (in buffer pixels of sel_out) */
static struct output *sel_out;
static int sel_has;
static int sel_x, sel_y, sel_w, sel_h;
static enum drag_kind drag_state = DRAG_NONE;
static int drag_x0, drag_y0;
static int drag_orig_x, drag_orig_y, drag_orig_w, drag_orig_h;
static int cur_bx, cur_by; /* pointer in buffer pixels of active output */

/* Window picker mode ('W' or Window Under Cursor button) */
static int picking_window;
static struct window *picked_window; /* if set, Save exports this window's image */

/* Annotations */
static enum tool active_tool = TOOL_NONE;
static struct annotation annos[MAX_ANNOS];
static int nannos;
static int next_badge_num = 1;

static void dirty_all(void)
{
    for (struct output *o = outputs; o; o = o->next) {
        o->dirty = 1;
    }
}

static int buf_w(const struct output *o)
{
    uint32_t s120 = o->viewport ? o->scale120 : 120;
    int w = (int)((o->w * s120 + 60) / 120);
    return w > 0 ? w : (o->bg.w > 0 ? o->bg.w : 1280);
}

static int buf_h(const struct output *o)
{
    uint32_t s120 = o->viewport ? o->scale120 : 120;
    int h = (int)((o->h * s120 + 60) / 120);
    return h > 0 ? h : (o->bg.h > 0 ? o->bg.h : 720);
}

/* Pixels and drawing primitives */

static uint32_t blend(uint32_t dst, uint32_t src)
{
    uint32_t a = src >> 24, out = 0xFF000000u;
    if (a == 255) return src;
    if (a == 0) return dst;
    for (int s = 0; s < 24; s += 8) {
        out |= (((src >> s & 255) * a + (dst >> s & 255) * (255 - a) + 127) / 255) << s;
    }
    return out;
}

static void put_px(uint32_t *px, int W, int H, int x, int y, uint32_t argb)
{
    if (x >= 0 && x < W && y >= 0 && y < H) {
        px[y * W + x] = blend(px[y * W + x], argb);
    }
}

static void fill_rect(uint32_t *px, int W, int H, int x, int y, int w, int h, uint32_t argb)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            px[yy * W + xx] = blend(px[yy * W + xx], argb);
        }
    }
}

static int corner(int x, int y, int rx, int ry, int rw, int rh, int r, int *dx, int *dy)
{
    *dx = x < rx + r ? rx + r - x : x >= rx + rw - r ? x - (rx + rw - r - 1) : 0;
    *dy = y < ry + r ? ry + r - y : y >= ry + rh - r ? y - (ry + rh - r - 1) : 0;
    return *dx && *dy;
}

static void round_rect(uint32_t *px, int W, int H, int x, int y, int w, int h, int r, int line, uint32_t argb)
{
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < H; yy++) {
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < W; xx++) {
            int dx, dy;
            if (corner(xx, yy, x, y, w, h, r, &dx, &dy) && dx * dx + dy * dy > r * r) continue;
            if (line > 0) {
                int ir = r > line ? r - line : 0, idx, idy;
                int inside = xx >= x + line && xx < x + w - line && yy >= y + line && yy < y + h - line;
                if (inside && !(corner(xx, yy, x + line, y + line, w - 2 * line, h - 2 * line, ir, &idx, &idy) &&
                                idx * idx + idy * idy > ir * ir)) {
                    continue;
                }
            }
            px[yy * W + xx] = blend(px[yy * W + xx], argb);
        }
    }
}

static void draw_disc(uint32_t *px, int W, int H, int cx, int cy, int r, uint32_t argb)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                put_px(px, W, H, cx + dx, cy + dy, argb);
            }
        }
    }
}

static void draw_thick_line(uint32_t *px, int W, int H, int x0, int y0, int x1, int y1, int rad, uint32_t argb)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int oy = -rad; oy <= rad; oy++) {
            for (int ox = -rad; ox <= rad; ox++) {
                if (ox * ox + oy * oy <= rad * rad + 1) {
                    put_px(px, W, H, x0 + ox, y0 + oy, argb);
                }
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_arrow(uint32_t *px, int W, int H, int x0, int y0, int x1, int y1, int rad, uint32_t argb)
{
    draw_thick_line(px, W, H, x0, y0, x1, y1, rad, argb);
    double dx = x1 - x0, dy = y1 - y0;
    double len = hypot(dx, dy);
    if (len < 1.0) return;
    double angle = atan2(dy, dx);
    double head = fmax(14.0, rad * 7.0);
    double spread = 0.45;
    int ax = (int)(x1 - head * cos(angle - spread));
    int ay = (int)(y1 - head * sin(angle - spread));
    int bx = (int)(x1 - head * cos(angle + spread));
    int by = (int)(y1 - head * sin(angle + spread));
    draw_thick_line(px, W, H, x1, y1, ax, ay, rad, argb);
    draw_thick_line(px, W, H, x1, y1, bx, by, rad, argb);
}

/* s at (x, y), no wider than maxw. */
static void text_clip(uint32_t *px, int W, int H, int x, int y, const char *s, uint32_t fg,
                      const struct panel_font *f, int ch, int maxw)
{
    int tw = panel_text_width(s, f);
    uint32_t *mask = tw > 0 && ch > 0 ? calloc((size_t)tw * (size_t)ch, 4) : NULL;
    if (!mask) return;
    panel_text(mask, tw, ch, tw, 0, 0, s, 0xFFFFFFFFu, 0xFF000000u, f);
    for (int yy = 0; yy < ch; yy++) {
        for (int xx = 0; xx < tw && xx < maxw; xx++) {
            if (x + xx >= 0 && x + xx < W && y + yy >= 0 && y + yy < H) {
                uint32_t *p = &px[(y + yy) * W + x + xx];
                *p = blend(*p, (mask[yy * tw + xx] >> 8 & 255) << 24 | (fg & 0xFFFFFFu));
            }
        }
    }
    free(mask);
}

static void text_over(uint32_t *px, int W, int H, int x, int y, const char *s, uint32_t fg,
                      const struct panel_font *f, int ch)
{
    text_clip(px, W, H, x, y, s, fg, f, ch, INT_MAX);
}

static int scale_image(const struct image *src, struct image *dst, int w, int h)
{
    if (dst->px && dst->w == w && dst->h == h) return 0;
    free(dst->px);
    dst->px = NULL;
    if (w <= 0 || h <= 0 || !src->px || !(dst->px = malloc((size_t)w * (size_t)h * 4))) return -1;
    dst->w = w;
    dst->h = h;
    if (w == src->w && h == src->h) {
        memcpy(dst->px, src->px, (size_t)w * (size_t)h * 4);
        return 0;
    }
    for (int y = 0; y < h; y++) {
        int sy = (int)((long)y * src->h / h);
        if (sy >= src->h) sy = src->h - 1;
        for (int x = 0; x < w; x++) {
            int sx = (int)((long)x * src->w / w);
            if (sx >= src->w) sx = src->w - 1;
            dst->px[y * w + x] = src->px[sy * src->w + sx] | 0xFF000000u;
        }
    }
    return 0;
}

static void render_annotations(uint32_t *px, int W, int H, const struct output *o, int ox, int oy)
{
    uint32_t s120 = o && o->viewport ? o->scale120 : 120;
    int row = text_px(s120 / 120.0), cw = row / 2, ch = row;
    const struct panel_font *bold = panel_font(row, 1);
    panel_font_size(bold, &cw, &ch);
    int rad = s120 >= 180 ? 3 : 2;

    for (int i = 0; i < nannos; i++) {
        const struct annotation *a = &annos[i];
        if (o && a->out != o) continue;
        int x0 = a->x0 - ox, y0 = a->y0 - oy;
        int x1 = a->x1 - ox, y1 = a->y1 - oy;
        switch (a->type) {
        case TOOL_ARROW:
            draw_arrow(px, W, H, x0, y0, x1, y1, rad, ANNO_RED);
            break;
        case TOOL_RECT: {
            int rx = x0 < x1 ? x0 : x1, ry = y0 < y1 ? y0 : y1;
            int rw = abs(x1 - x0) + 1, rh = abs(y1 - y0) + 1;
            round_rect(px, W, H, rx, ry, rw, rh, 0, rad * 2, ANNO_RED);
            break;
        }
        case TOOL_PEN:
            if (a->npts == 1) {
                draw_disc(px, W, H, a->pts[0].x - ox, a->pts[0].y - oy, rad, ANNO_RED);
            }
            for (int k = 1; k < a->npts; k++) {
                draw_thick_line(px, W, H, a->pts[k - 1].x - ox, a->pts[k - 1].y - oy,
                                a->pts[k].x - ox, a->pts[k].y - oy, rad, ANNO_RED);
            }
            break;
        case TOOL_HILITE: {
            int rx = x0 < x1 ? x0 : x1, ry = y0 < y1 ? y0 : y1;
            int rw = abs(x1 - x0) + 1, rh = abs(y1 - y0) + 1;
            if (rh < row / 2) {
                ry -= (row / 2 - rh) / 2;
                rh = row / 2;
            }
            fill_rect(px, W, H, rx, ry, rw, rh, ANNO_YEL);
            break;
        }
        case TOOL_NUMBER: {
            int r = row * 2 / 3;
            draw_disc(px, W, H, x0, y0, r + 1, 0xFFFFFFFFu);
            draw_disc(px, W, H, x0, y0, r, ANNO_RED);
            char num[16];
            snprintf(num, sizeof num, "%d", a->num);
            int tw = panel_text_width(num, bold);
            text_over(px, W, H, x0 - tw / 2, y0 - ch / 2, num, TEXT, bold, ch);
            break;
        }
        default:
            break;
        }
    }
}

/* PNG writing and user (uid 1000) file ownership */

static void chown_user(const char *path)
{
    struct passwd *pw = getpwuid(1000);
    if (pw) {
        if (chown(path, pw->pw_uid, pw->pw_gid) < 0) {
            /* ignore if unprivileged */
        }
    }
}

static void mkdir_p_user(const char *dir)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) == 0) chown_user(tmp);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) == 0) chown_user(tmp);
}

static int default_shot_path(char *out, size_t size)
{
    struct passwd *pw = getpwuid(1000);
    const char *home = pw && pw->pw_dir && pw->pw_dir[0] ? pw->pw_dir : getenv("HOME");
    if (!home || !home[0]) home = "/tmp";

    char dir[512];
    snprintf(dir, sizeof dir, "%.480s/Pictures/Screenshots", home);
    mkdir_p_user(dir);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);

    for (int suf = 0; suf < 1000; suf++) {
        int n = suf == 0 ? snprintf(out, size, "%s/Screenshot_%s.png", dir, stamp) :
                           snprintf(out, size, "%s/Screenshot_%s_%d.png", dir, stamp, suf);
        if (n < 0 || (size_t)n >= size) return -1;
        if (access(out, F_OK) != 0) return 0;
    }
    return -1;
}

/* The image as PNG into fp, which is closed. */
static int write_png(FILE *fp, const uint32_t *px, int w, int h)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return -1;
    }
    uint8_t *row = malloc((size_t)w * 3);
    if (!row || setjmp(png_jmpbuf(png))) {
        free(row);
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < h; y++) {
        const uint32_t *src = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t p = src[x];
            row[x * 3 + 0] = (uint8_t)(p >> 16 & 255);
            row[x * 3 + 1] = (uint8_t)(p >> 8 & 255);
            row[x * 3 + 2] = (uint8_t)(p & 255);
        }
        png_write_row(png, row);
    }
    png_write_end(png, NULL);
    free(row);
    png_destroy_write_struct(&png, &info);
    return fclose(fp) == 0 ? 0 : -1;
}

static int write_png_file(const char *path, const uint32_t *px, int w, int h)
{
    if (w <= 0 || h <= 0 || !px) return -1;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        mkdir_p_user(dir);
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        say("locachy-shot: cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (write_png(fp, px, w, h) < 0) return -1;
    chmod(path, 0644);
    chown_user(path);
    return 0;
}

/* The image as image/png in the host clipboard (Spectacle's Copy). */
static int copy_png(const uint32_t *px, int w, int h)
{
    if (w <= 0 || h <= 0 || !px) return -1;
    char *data = NULL;
    size_t size = 0;
    FILE *fp = open_memstream(&data, &size);
    if (!fp) return -1;
    int r = write_png(fp, px, w, h) == 0 && clipboard_set_png(data, size) == CLIPBOARD_OK ? 0 : -1;
    if (r < 0) say("locachy-shot: could not copy the image to the clipboard");
    free(data);
    return r;
}

/* Capture via ext-image-copy-capture-v1 */

static void buffer_free(struct capture *c)
{
    if (c->buffer) wl_buffer_destroy(c->buffer);
    if (c->px) munmap(c->px, (size_t)c->bw * (size_t)c->bh * 4);
    c->buffer = NULL;
    c->px = NULL;
}

static void cap_free(struct capture *c)
{
    if (c->frame) ext_image_copy_capture_frame_v1_destroy(c->frame);
    if (c->session) ext_image_copy_capture_session_v1_destroy(c->session);
    c->frame = NULL;
    c->session = NULL;
    buffer_free(c);
}

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t t)
{ (void)data; (void)f; (void)t; }
static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y, int32_t w,
                         int32_t h)
{ (void)data; (void)f; (void)x; (void)y; (void)w; (void)h; }
static void frame_time(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns)
{ (void)data; (void)f; (void)hi; (void)lo; (void)ns; }

static void next_frame(struct capture *c);

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f)
{
    struct capture *c = data;
    struct image *im = c->into;
    if (!im->px || im->w != c->bw || im->h != c->bh) {
        free(im->px);
        im->px = malloc((size_t)c->bw * (size_t)c->bh * 4);
    }
    if (im->px) {
        im->w = c->bw;
        im->h = c->bh;
        int swap = c->bformat == WL_SHM_FORMAT_XBGR8888 || c->bformat == WL_SHM_FORMAT_ABGR8888;
        for (size_t i = 0; i < (size_t)c->bw * (size_t)c->bh; i++) {
            uint32_t p = c->px[i];
            if (swap) p = (p & 0x0000FF00u) | (p & 0xFFu) << 16 | (p >> 16 & 0xFFu);
            im->px[i] = p | 0xFF000000u;
        }
    }
    ext_image_copy_capture_frame_v1_destroy(f);
    c->frame = NULL;
    c->done = 1;
    cap_free(c);
    dirty_all();
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason)
{
    struct capture *c = data;
    ext_image_copy_capture_frame_v1_destroy(f);
    c->frame = NULL;
    if (reason != EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
        say("locachy-shot: capture failed (%u)", reason);
    }
    c->done = 1;
    cap_free(c);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    frame_transform, frame_damage, frame_time, frame_ready, frame_failed,
};

static void session_size(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t w, uint32_t h)
{
    struct capture *c = data;
    (void)s;
    c->w = (int)w;
    c->h = (int)h;
}

static int format_rank(uint32_t format)
{
    switch (format) {
    case WL_SHM_FORMAT_XRGB8888: return 4;
    case WL_SHM_FORMAT_ARGB8888: return 3;
    case WL_SHM_FORMAT_XBGR8888: return 2;
    case WL_SHM_FORMAT_ABGR8888: return 1;
    default: return 0;
    }
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t format)
{
    struct capture *c = data;
    (void)s;
    c->offered = format;
    if (format_rank(format) > c->pending_rank) {
        c->pending_rank = format_rank(format);
        c->pending = format;
    }
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *s, struct wl_array *d)
{ (void)data; (void)s; (void)d; }
static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t f,
                                  struct wl_array *m)
{ (void)data; (void)s; (void)f; (void)m; }

static void next_frame(struct capture *c)
{
    int fresh = !c->buffer || c->bw != c->w || c->bh != c->h || c->bformat != c->format;
    if (fresh) {
        buffer_free(c);
        size_t size = (size_t)c->w * (size_t)c->h * 4;
        int fd = c->rank && c->w > 0 && c->h > 0 ? memfd_create("locachy-shot-cap", MFD_CLOEXEC) : -1;
        if (fd < 0 || ftruncate(fd, (off_t)size) < 0 ||
            (c->px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
            c->px = NULL;
            if (fd >= 0) close(fd);
            c->done = 1;
            cap_free(c);
            return;
        }
        c->bw = c->w;
        c->bh = c->h;
        c->bformat = c->format;
        struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
        c->buffer = wl_shm_pool_create_buffer(pool, 0, c->w, c->h, c->w * 4, c->format);
        wl_shm_pool_destroy(pool);
        close(fd);
    }
    c->frame = ext_image_copy_capture_session_v1_create_frame(c->session);
    ext_image_copy_capture_frame_v1_add_listener(c->frame, &frame_listener, c);
    ext_image_copy_capture_frame_v1_attach_buffer(c->frame, c->buffer);
    if (fresh) ext_image_copy_capture_frame_v1_damage_buffer(c->frame, 0, 0, c->w, c->h);
    ext_image_copy_capture_frame_v1_capture(c->frame);
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *s)
{
    struct capture *c = data;
    (void)s;
    c->rank = c->pending_rank;
    c->format = c->pending;
    c->pending_rank = 0;
    if (!c->rank) say("locachy-shot: unsupported shm format (0x%08x)", c->offered);
    if (!c->frame && !c->buffer) next_frame(c);
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *s)
{
    struct capture *c = data;
    (void)s;
    c->done = 1;
    cap_free(c);
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    session_size, session_shm_format, session_dmabuf_device, session_dmabuf_format, session_done, session_stopped,
};

static void capture(struct capture *c, struct ext_image_capture_source_v1 *source, struct image *into)
{
    memset(c, 0, sizeof *c);
    c->into = into;
    if (!source) {
        c->done = 1;
        return;
    }
    c->session = ext_image_copy_capture_manager_v1_create_session(copier, source, 0);
    ext_image_copy_capture_session_v1_add_listener(c->session, &session_listener, c);
    ext_image_capture_source_v1_destroy(source);
}

/* Windows & Outputs listeners */

static void win_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title)
{
    struct window *w = data;
    (void)h;
    snprintf(w->title, sizeof w->title, "%s", title);
}

static void win_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *id)
{
    struct window *w = data;
    (void)h;
    snprintf(w->app_id, sizeof w->app_id, "%s", id);
}

static void win_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
    struct window *w = data;
    (void)h;
    for (int i = 0; i < w->noutputs; i++) {
        if (w->outputs[i] == o) return;
    }
    if (w->noutputs < 4) w->outputs[w->noutputs++] = o;
}

static void win_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
    struct window *w = data;
    (void)h;
    for (int i = 0; i < w->noutputs; i++) {
        if (w->outputs[i] == o) {
            w->outputs[i] = w->outputs[--w->noutputs];
            break;
        }
    }
}

static void win_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *states)
{
    struct window *w = data;
    uint32_t *s;
    (void)h;
    w->activated = w->minimized = 0;
    wl_array_for_each(s, states) {
        w->activated |= *s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED;
        w->minimized |= *s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED;
    }
}

static void win_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) { (void)data; (void)h; }
static void win_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) { (void)data; (void)h; }
static void win_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                       struct zwlr_foreign_toplevel_handle_v1 *p)
{ (void)data; (void)h; (void)p; }

static const struct zwlr_foreign_toplevel_handle_v1_listener win_listener = {
    win_title, win_app_id, win_output_enter, win_output_leave, win_state, win_done, win_closed, win_parent,
};

static void new_window(void *data, struct zwlr_foreign_toplevel_manager_v1 *m,
                       struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)data;
    (void)m;
    struct window *w = calloc(1, sizeof *w), **tail = &windows;
    if (!w) return;
    w->handle = h;
    w->cap.done = 1;
    while (*tail) tail = &(*tail)->next;
    *tail = w;
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &win_listener, w);
}

static void toplevels_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *m) { (void)data; (void)m; }
static const struct zwlr_foreign_toplevel_manager_v1_listener toplevels_listener = { new_window, toplevels_finished };

static void listed_closed(void *data, struct ext_foreign_toplevel_handle_v1 *h) { (void)data; (void)h; }
static void listed_done(void *data, struct ext_foreign_toplevel_handle_v1 *h) { (void)data; (void)h; }
static void listed_title(void *data, struct ext_foreign_toplevel_handle_v1 *h, const char *t)
{
    struct listed *e = data;
    (void)h;
    snprintf(e->title, sizeof e->title, "%s", t);
}
static void listed_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *h, const char *id)
{
    struct listed *e = data;
    (void)h;
    snprintf(e->app_id, sizeof e->app_id, "%s", id);
}
static void listed_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *h, const char *id)
{ (void)data; (void)h; (void)id; }

static const struct ext_foreign_toplevel_handle_v1_listener listed_listener = {
    listed_closed, listed_done, listed_title, listed_app_id, listed_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *l, struct ext_foreign_toplevel_handle_v1 *h)
{
    (void)data;
    (void)l;
    struct listed *e = calloc(1, sizeof *e), **tail = &listed;
    if (!e) return;
    e->handle = h;
    while (*tail) tail = &(*tail)->next;
    *tail = e;
    ext_foreign_toplevel_handle_v1_add_listener(h, &listed_listener, e);
}
static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *l) { (void)data; (void)l; }
static const struct ext_foreign_toplevel_list_v1_listener list_listener = { list_toplevel, list_finished };

static void pair_windows(void)
{
    for (int loose = 0; loose < 2; loose++) {
        for (struct window *w = windows; w; w = w->next) {
            for (struct listed *e = listed; e && !w->listed; e = e->next) {
                if (!e->paired && strcmp(e->app_id, w->app_id) == 0 && (loose || strcmp(e->title, w->title) == 0)) {
                    e->paired = 1;
                    w->listed = e;
                }
            }
        }
    }
}

/* wl_output & wlr-output-management for layout coordinates */

static void out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                         int32_t sub, const char *make, const char *model, int32_t tr)
{
    struct output *out = data;
    (void)o; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)tr;
    out->lx = x;
    out->ly = y;
}
static void out_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r)
{
    struct output *out = data;
    (void)o; (void)r;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        if (!out->lw) out->lw = w;
        if (!out->lh) out->lh = h;
    }
}
static void out_done(void *data, struct wl_output *o) { (void)data; (void)o; }
static void out_scale(void *data, struct wl_output *o, int32_t f) { (void)data; (void)o; (void)f; }
static void out_name(void *data, struct wl_output *o, const char *name)
{
    struct output *out = data;
    (void)o;
    snprintf(out->conn, sizeof out->conn, "%s", name);
}
static void out_desc(void *data, struct wl_output *o, const char *d) { (void)data; (void)o; (void)d; }

static const struct wl_output_listener wl_output_listener = {
    out_geometry, out_mode, out_done, out_scale, out_name, out_desc,
};

static void mode_noop_sz(void *d, struct zwlr_output_mode_v1 *m, int32_t w, int32_t h) { (void)d; (void)m; (void)w; (void)h; }
static void mode_noop_ref(void *d, struct zwlr_output_mode_v1 *m, int32_t r) { (void)d; (void)m; (void)r; }
static void mode_noop(void *d, struct zwlr_output_mode_v1 *m) { (void)d; (void)m; }
static const struct zwlr_output_mode_v1_listener mode_noop_listener = {
    mode_noop_sz, mode_noop_ref, mode_noop, mode_noop,
};

static void head_name(void *data, struct zwlr_output_head_v1 *hd, const char *name)
{
    struct head_info *h = data;
    (void)hd;
    snprintf(h->name, sizeof h->name, "%s", name);
}
static void head_str(void *data, struct zwlr_output_head_v1 *hd, const char *s) { (void)data; (void)hd; (void)s; }
static void head_phys(void *data, struct zwlr_output_head_v1 *hd, int32_t w, int32_t h) { (void)data; (void)hd; (void)w; (void)h; }
static void head_mode(void *data, struct zwlr_output_head_v1 *hd, struct zwlr_output_mode_v1 *m)
{
    (void)data; (void)hd;
    zwlr_output_mode_v1_add_listener(m, &mode_noop_listener, NULL);
}
static void head_enabled(void *data, struct zwlr_output_head_v1 *hd, int32_t en)
{
    struct head_info *h = data;
    (void)hd;
    h->enabled = en;
}
static void head_cur_mode(void *data, struct zwlr_output_head_v1 *hd, struct zwlr_output_mode_v1 *m) { (void)data; (void)hd; (void)m; }
static void head_pos(void *data, struct zwlr_output_head_v1 *hd, int32_t x, int32_t y)
{
    struct head_info *h = data;
    (void)hd;
    h->x = x;
    h->y = y;
}
static void head_tr(void *data, struct zwlr_output_head_v1 *hd, int32_t t) { (void)data; (void)hd; (void)t; }
static void head_sc(void *data, struct zwlr_output_head_v1 *hd, wl_fixed_t s)
{
    struct head_info *h = data;
    (void)hd;
    h->scale = wl_fixed_to_double(s);
}
static void head_fin(void *data, struct zwlr_output_head_v1 *hd) { (void)data; (void)hd; }
static void head_sync(void *data, struct zwlr_output_head_v1 *hd, uint32_t st) { (void)data; (void)hd; (void)st; }

static const struct zwlr_output_head_v1_listener head_listener = {
    head_name, head_str, head_phys, head_mode, head_enabled, head_cur_mode,
    head_pos, head_tr, head_sc, head_fin, head_str, head_str, head_str, head_sync,
};

static void mgr_head(void *data, struct zwlr_output_manager_v1 *m, struct zwlr_output_head_v1 *hd)
{
    (void)data; (void)m;
    struct head_info *h = calloc(1, sizeof *h);
    if (!h) return;
    h->head = hd;
    h->scale = 1.0;
    h->next = heads;
    heads = h;
    zwlr_output_head_v1_add_listener(hd, &head_listener, h);
}
static void mgr_done(void *data, struct zwlr_output_manager_v1 *m, uint32_t s) { (void)data; (void)m; (void)s; }
static void mgr_fin(void *data, struct zwlr_output_manager_v1 *m) { (void)data; (void)m; }
static const struct zwlr_output_manager_v1_listener output_mgr_listener = { mgr_head, mgr_done, mgr_fin };

static void sync_output_geometries(void)
{
    for (struct output *o = outputs; o; o = o->next) {
        for (struct head_info *h = heads; h; h = h->next) {
            if (h->enabled && h->name[0] && strcmp(o->conn, h->name) == 0) {
                o->lx = h->x;
                o->ly = h->y;
                if (h->scale > 0 && o->bg.w > 0 && o->bg.h > 0) {
                    o->lw = (int)(o->bg.w / h->scale + 0.5);
                    o->lh = (int)(o->bg.h / h->scale + 0.5);
                }
            }
        }
        if (o->lw <= 0) o->lw = o->bg.w > 0 ? o->bg.w : 1280;
        if (o->lh <= 0) o->lh = o->bg.h > 0 ? o->bg.h : 720;
    }
}

static struct window *active_window(void)
{
    for (struct window *w = windows; w; w = w->next) {
        if (w->activated) return w;
    }
    return windows;
}

static struct output *focused_output(void)
{
    if (pointer_output) return pointer_output;
    struct window *w = active_window();
    if (w && w->noutputs) {
        for (struct output *o = outputs; o; o = o->next) {
            if (o->wl == w->outputs[0]) return o;
        }
    }
    return outputs;
}

/* Composite all outputs into a single image according to layout coordinates */
static int stitch_fullscreen(struct image *out)
{
    sync_output_geometries();
    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
    for (struct output *o = outputs; o; o = o->next) {
        if (!o->bg.px) continue;
        if (o->lx < min_x) min_x = o->lx;
        if (o->ly < min_y) min_y = o->ly;
        int w = o->lw > 0 ? o->lw : o->bg.w;
        int h = o->lh > 0 ? o->lh : o->bg.h;
        if (o->lx + w > max_x) max_x = o->lx + w;
        if (o->ly + h > max_y) max_y = o->ly + h;
    }
    if (min_x >= max_x || min_y >= max_y) return -1;
    int W = max_x - min_x, H = max_y - min_y;
    uint32_t *px = calloc((size_t)W * (size_t)H, 4);
    if (!px) return -1;
    for (int i = 0; i < W * H; i++) px[i] = 0xFF000000u;

    for (struct output *o = outputs; o; o = o->next) {
        if (!o->bg.px) continue;
        int ox = o->lx - min_x, oy = o->ly - min_y;
        int ow = o->lw > 0 ? o->lw : o->bg.w;
        int oh = o->lh > 0 ? o->lh : o->bg.h;
        struct image tmp = { 0 };
        if (scale_image(&o->bg, &tmp, ow, oh) == 0) {
            render_annotations(tmp.px, tmp.w, tmp.h, o, 0, 0);
            for (int y = 0; y < tmp.h && oy + y < H; y++) {
                if (oy + y < 0) continue;
                for (int x = 0; x < tmp.w && ox + x < W; x++) {
                    if (ox + x < 0) continue;
                    px[(oy + y) * W + (ox + x)] = tmp.px[y * tmp.w + x];
                }
            }
            free(tmp.px);
        }
    }
    out->px = px;
    out->w = W;
    out->h = H;
    return 0;
}

/* Crop or build the target image for saving */
static int build_export_image(struct image *out)
{
    memset(out, 0, sizeof *out);
    if (picked_window && picked_window->img.px) {
        int w = picked_window->img.w, h = picked_window->img.h;
        out->px = malloc((size_t)w * (size_t)h * 4);
        if (!out->px) return -1;
        memcpy(out->px, picked_window->img.px, (size_t)w * (size_t)h * 4);
        out->w = w;
        out->h = h;
        return 0;
    }
    if (sel_has && sel_out && sel_w > 0 && sel_h > 0) {
        int W = buf_w(sel_out), H = buf_h(sel_out);
        struct image full = { 0 };
        if (scale_image(&sel_out->bg, &full, W, H) < 0) return -1;
        render_annotations(full.px, W, H, sel_out, 0, 0);
        int x0 = sel_x < 0 ? 0 : sel_x, y0 = sel_y < 0 ? 0 : sel_y;
        int x1 = sel_x + sel_w > W ? W : sel_x + sel_w;
        int y1 = sel_y + sel_h > H ? H : sel_y + sel_h;
        int cw = x1 - x0, ch = y1 - y0;
        if (cw <= 0 || ch <= 0) {
            free(full.px);
            return -1;
        }
        out->px = malloc((size_t)cw * (size_t)ch * 4);
        if (!out->px) {
            free(full.px);
            return -1;
        }
        for (int y = 0; y < ch; y++) {
            memcpy(out->px + (size_t)y * cw, full.px + (size_t)(y0 + y) * W + x0, (size_t)cw * 4);
        }
        free(full.px);
        out->w = cw;
        out->h = ch;
        return 0;
    }
    if (opt_mode == MODE_ACTIVEWIN || opt_mode == MODE_UNDERCURSOR) {
        struct window *w = active_window();
        if (w && w->img.px) {
            out->px = malloc((size_t)w->img.w * (size_t)w->img.h * 4);
            if (!out->px) return -1;
            memcpy(out->px, w->img.px, (size_t)w->img.w * (size_t)w->img.h * 4);
            out->w = w->img.w;
            out->h = w->img.h;
            return 0;
        }
    }
    if (opt_mode == MODE_CURRENT) {
        struct output *o = focused_output();
        if (o && o->bg.px) {
            int w = o->bg.w, h = o->bg.h;
            out->px = malloc((size_t)w * (size_t)h * 4);
            if (!out->px) return -1;
            memcpy(out->px, o->bg.px, (size_t)w * (size_t)h * 4);
            render_annotations(out->px, w, h, o, 0, 0);
            out->w = w;
            out->h = h;
            return 0;
        }
    }
    return stitch_fullscreen(out);
}

static int perform_save(char *saved_path, size_t path_size)
{
    if (opt_output && opt_output[0]) {
        snprintf(saved_path, path_size, "%s", opt_output);
    } else if (default_shot_path(saved_path, path_size) < 0) {
        return -1;
    }
    struct image exp = { 0 };
    if (build_export_image(&exp) < 0) {
        say("locachy-shot: nothing captured to save");
        return -1;
    }
    int r = write_png_file(saved_path, exp.px, exp.w, exp.h);
    free(exp.px);
    if (r == 0 && opt_copy_path) {
        clipboard_set(0, saved_path, strlen(saved_path));
    }
    return r;
}

static int perform_copy(void)
{
    struct image exp = { 0 };
    if (build_export_image(&exp) < 0) {
        say("locachy-shot: nothing captured to copy");
        return -1;
    }
    int r = copy_png(exp.px, exp.w, exp.h);
    free(exp.px);
    return r;
}

/* Copy (Enter, double-click, Ctrl+C), Save (Ctrl+S) or open in view (v).
 * What fails stays on the screen, said in the toolbar, to be tried again. */
static void finish_with(enum finish how)
{
    char path[PATH_MAX];
    int r = how == FINISH_COPY ? perform_copy() : perform_save(path, sizeof path);
    if (r < 0) {
        snprintf(status_msg, sizeof status_msg, "%s",
                 how == FINISH_COPY ? _("Could not copy to the clipboard") : _("Could not save the file"));
        dirty_all();
        return;
    }
    if (how == FINISH_VIEW) open_in_home("view", path);
    exit_code = 0;
    finished = 1;
}

/* Mode switching inside the interactive HUD */

static void select_full_output(struct output *o)
{
    if (!o) o = focused_output();
    if (!o) return;
    picked_window = NULL;
    picking_window = 0;
    sel_out = o;
    sel_has = 1;
    sel_x = 0;
    sel_y = 0;
    sel_w = buf_w(o);
    sel_h = buf_h(o);
    dirty_all();
}

static void trigger_mode(enum mode m)
{
    opt_mode = m;
    picked_window = NULL;
    picking_window = 0;
    if (m == MODE_FULLSCREEN) {
        sel_has = 0; /* the whole desktop, every screen: Enter copies it */
        dirty_all();
    } else if (m == MODE_CURRENT) {
        select_full_output(focused_output());
    } else if (m == MODE_ACTIVEWIN) {
        struct window *w = active_window();
        if (w && w->img.px) {
            picked_window = w;
            struct output *o = focused_output();
            if (o) {
                int W = buf_w(o), H = buf_h(o);
                sel_out = o;
                sel_has = 1;
                sel_w = w->img.w <= W ? w->img.w : W;
                sel_h = w->img.h <= H ? w->img.h : H;
                sel_x = (W - sel_w) / 2;
                sel_y = (H - sel_h) / 2;
            }
            dirty_all();
        }
    } else if (m == MODE_UNDERCURSOR) {
        picking_window = 1;
        dirty_all();
    } else if (m == MODE_REGION) {
        dirty_all();
    }
}

/* Layer shell & drawing */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *l, uint32_t serial, uint32_t w, uint32_t h)
{
    struct output *o = data;
    zwlr_layer_surface_v1_ack_configure(l, serial);
    o->w = w;
    o->h = h;
    if (!o->lw) o->lw = (int)w;
    if (!o->lh) o->lh = (int)h;
    o->configured = o->dirty = 1;
    if (opt_mode == MODE_CURRENT && !sel_has && o == focused_output()) {
        select_full_output(o);
    } else if (opt_mode == MODE_ACTIVEWIN && !sel_has && o == focused_output()) {
        trigger_mode(MODE_ACTIVEWIN);
    }
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *l)
{
    (void)data;
    (void)l;
    finished = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = { layer_configure, layer_closed };

static void output_scale(void *data, struct wp_fractional_scale_v1 *s, uint32_t scale120)
{
    struct output *o = data;
    (void)s;
    if (o->scale120 != scale120) {
        o->scale120 = scale120;
        o->dirty = 1;
    }
}

static const struct wp_fractional_scale_v1_listener scale_listener = { output_scale };

static void create_surface(struct output *o, int keyboard_here)
{
    o->surf = wl_compositor_create_surface(compositor);
    o->scale120 = 120;
    o->hovered_hit = -1;
    o->keyboard = keyboard_here;
    if (scale_mgr && viewporter) {
        o->fscale = wp_fractional_scale_manager_v1_get_fractional_scale(scale_mgr, o->surf);
        wp_fractional_scale_v1_add_listener(o->fscale, &scale_listener, o);
        o->viewport = wp_viewporter_get_viewport(viewporter, o->surf);
    }
    o->layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, o->surf, o->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                                                     "locachy-shot");
    zwlr_layer_surface_v1_set_anchor(o->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, keyboard_here ?
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
    wl_surface_commit(o->surf);
}

static void buffer_release(void *data, struct wl_buffer *b)
{
    (void)data;
    wl_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void get_handles(int x, int y, int w, int h, int hx[8], int hy[8])
{
    hx[0] = x;         hy[0] = y;         /* NW */
    hx[1] = x + w / 2; hy[1] = y;         /* N  */
    hx[2] = x + w;     hy[2] = y;         /* NE */
    hx[3] = x + w;     hy[3] = y + h / 2; /* E  */
    hx[4] = x + w;     hy[4] = y + h;     /* SE */
    hx[5] = x + w / 2; hy[5] = y + h;     /* S  */
    hx[6] = x;         hy[6] = y + h;     /* SW */
    hx[7] = x;         hy[7] = y + h / 2; /* W  */
}

/* The toolbar, like Spectacle's floating one: the actions, the capture modes,
 * the annotation tools and the keys, each group from a new row. Every item is
 * measured first; a row that does not fit the screen wraps, and the panel is
 * as wide as its widest row, so nothing is drawn outside it. Under the
 * selection, else over it, else inside it at the bottom; with no selection,
 * at the bottom of the screen. */
struct hud_item {
    const char *label;
    int id; /* HIT_*; 0: text */
    int active, strong, row_break;
    int x, y, w;
};

static void draw_hud(struct output *o, uint32_t *px, int W, int H, const struct panel_font *font,
                     const struct panel_font *bold, int ch, int row)
{
    struct hud_item it[32];
    int n = 0;
    int whole = opt_mode == MODE_FULLSCREEN && !sel_has && !picking_window;
#define ITEM(l, i, a, s, b) \
    if (n < (int)(sizeof it / sizeof it[0])) it[n++] = (struct hud_item){ .label = (l), .id = (i), \
        .active = (a), .strong = (s), .row_break = (b) }
    ITEM(_("Copy"), HIT_COPY, 0, 1, 1);
    ITEM(_("Save"), HIT_SAVE, 0, 0, 0);
    /* TRANSLATORS: view is the document viewer program; keep its name. */
    ITEM(_("Open in view"), HIT_VIEW, 0, 0, 0);
    ITEM(_("Cancel"), HIT_CANCEL, 0, 0, 0);
    ITEM(_("Rectangular region"), HIT_MODE_REGION, !picking_window && !picked_window && !whole &&
         opt_mode != MODE_CURRENT, 0, 1);
    ITEM(_("Full screen"), HIT_MODE_FULL, whole, 0, 0);
    ITEM(_("Current screen"), HIT_MODE_CURRENT, opt_mode == MODE_CURRENT && sel_has && !picked_window, 0, 0);
    ITEM(_("Active window"), HIT_MODE_ACTIVE, picked_window != NULL, 0, 0);
    ITEM(_("Window under cursor"), HIT_MODE_WINDOW, picking_window, 0, 0);
    ITEM(_("Arrow"), HIT_TOOL_ARROW, active_tool == TOOL_ARROW, 0, 1);
    ITEM(_("Rectangle"), HIT_TOOL_RECT, active_tool == TOOL_RECT, 0, 0);
    ITEM(_("Freehand"), HIT_TOOL_PEN, active_tool == TOOL_PEN, 0, 0);
    ITEM(_("Highlighter"), HIT_TOOL_HILITE, active_tool == TOOL_HILITE, 0, 0);
    ITEM(_("Number"), HIT_TOOL_NUMBER, active_tool == TOOL_NUMBER, 0, 0);
    ITEM(_("Undo"), HIT_UNDO, 0, 0, 0);
    if (status_msg[0]) ITEM(status_msg, 0, 0, 0, 1);
    int keys_row = !status_msg[0]; /* the keys from a new row, after a message's */
    if (picking_window) {
        ITEM(_("Click a window to capture it"), 0, 0, 0, keys_row);
        /* TRANSLATORS: Esc is the key; keep it. */
        ITEM(_("Esc: cancel"), 0, 0, 0, 0);
    } else if (sel_has) {
        /* TRANSLATORS: Enter is the key; keep it. */
        ITEM(_("Enter or double-click: copy"), 0, 0, 0, keys_row);
        /* TRANSLATORS: Ctrl+S is the key; keep it. */
        ITEM(_("Ctrl+S: save"), 0, 0, 0, 0);
        ITEM(_("Drag the handles: resize"), 0, 0, 0, 0);
        /* TRANSLATORS: Esc is the key; keep it. */
        ITEM(_("Esc: clear"), 0, 0, 0, 0);
    } else {
        ITEM(_("Drag: select a region"), 0, 0, 0, keys_row);
        /* TRANSLATORS: Enter is the key; keep it. */
        ITEM(_("Enter: copy the whole desktop"), 0, 0, 0, 0);
        /* TRANSLATORS: Esc is the key; keep it. */
        ITEM(_("Esc: cancel"), 0, 0, 0, 0);
    }
#undef ITEM

    int pad = ch / 2, gap = ch / 3, item_h = ch + ch / 2, margin = row / 2;
    int maxw = W - 2 * margin - 2 * pad;
    if (maxw < 4 * ch) maxw = 4 * ch;
    int x = 0, y = 0, used = 0, widest = 0;
    for (int i = 0; i < n; i++) {
        const struct panel_font *f = it[i].strong ? bold : font;
        int w = panel_text_width(it[i].label, f) + (it[i].id ? 2 * pad : 0);
        int space = it[i].id ? gap : 2 * ch; /* keys apart from each other */
        if (w > maxw) w = maxw;
        if (used && (it[i].row_break || x + space + w > maxw)) {
            y += item_h + gap;
            x = 0;
            used = 0;
        }
        if (used) x += space;
        it[i].x = x;
        it[i].y = y;
        it[i].w = w;
        x += w;
        used = 1;
        if (x > widest) widest = x;
    }
    int pw = widest + 2 * pad, ph = y + item_h + 2 * pad;

    int px0, py0;
    if (sel_has && sel_out == o) {
        int below = sel_y + sel_h + margin, above = sel_y - margin - ph;
        py0 = below + ph <= H - margin ? below : above >= margin ? above : H - margin - ph;
        px0 = sel_x + sel_w / 2 - pw / 2;
    } else {
        px0 = (W - pw) / 2;
        py0 = H - ph - row;
    }
    if (px0 > W - margin - pw) px0 = W - margin - pw;
    if (px0 < margin) px0 = margin;
    if (py0 > H - margin - ph) py0 = H - margin - ph;
    if (py0 < margin) py0 = margin;

    round_rect(px, W, H, px0, py0, pw, ph, ch / 3, 0, PANEL_DARK);
    round_rect(px, W, H, px0, py0, pw, ph, ch / 3, 1, HIGHLIGHT);
    for (int i = 0; i < n; i++) {
        int bx = px0 + pad + it[i].x, by = py0 + pad + it[i].y;
        const struct panel_font *f = it[i].strong ? bold : font;
        if (!it[i].id) {
            uint32_t fg = it[i].label == status_msg ? ANNO_RED : DIM_TEXT;
            text_clip(px, W, H, bx, by + ch / 4, it[i].label, fg, f, ch, it[i].w);
            continue;
        }
        uint32_t bg = it[i].active ? HIGHLIGHT : (o->hovered_hit == it[i].id ? BTN_HOVER : BTN_BG);
        uint32_t fg = it[i].active ? 0xFF000000u : TEXT;
        round_rect(px, W, H, bx, by, it[i].w, item_h, ch / 4, 0, bg);
        text_clip(px, W, H, bx + pad, by + ch / 4, it[i].label, fg, f, ch, it[i].w - 2 * pad);
        if (o->nhits < MAX_HITS) {
            o->hits[o->nhits++] = (struct hit){ .id = it[i].id, .x = bx, .y = by, .w = it[i].w, .h = item_h };
        }
    }
}

/* Magnifier loupe near cursor when dragging selection or handle */
static void draw_magnifier(uint32_t *px, int W, int H, const struct image *src, int cx, int cy,
                           const struct panel_font *font, int ch)
{
    if (!src->px) return;
    int cells = 9, zoom = 6;
    int box = cells * zoom;
    int lx = cx + 24, ly = cy + 24;
    if (lx + box + 8 > W) lx = cx - box - 24;
    if (ly + box + ch + 16 > H) ly = cy - box - ch - 24;
    if (lx < 4) lx = 4;
    if (ly < 4) ly = 4;

    round_rect(px, W, H, lx - 2, ly - 2, box + 4, box + ch + 8, 4, 0, PANEL_DARK);
    for (int gy = 0; gy < cells; gy++) {
        for (int gx = 0; gx < cells; gx++) {
            int sx = cx + (gx - cells / 2), sy = cy + (gy - cells / 2);
            uint32_t col = (sx >= 0 && sx < src->w && sy >= 0 && sy < src->h) ?
                           src->px[sy * src->w + sx] : 0xFF000000u;
            fill_rect(px, W, H, lx + gx * zoom, ly + gy * zoom, zoom, zoom, col);
        }
    }
    /* Crosshair on center pixel */
    int mid = (cells / 2) * zoom;
    round_rect(px, W, H, lx + mid, ly, zoom, box, 0, 1, HIGHLIGHT);
    round_rect(px, W, H, lx, ly + mid, box, zoom, 0, 1, HIGHLIGHT);
    round_rect(px, W, H, lx - 2, ly - 2, box + 4, box + 4, 2, 2, TEXT);

    char coord[64];
    snprintf(coord, sizeof coord, "(%d, %d)", cx, cy);
    int tw = panel_text_width(coord, font);
    text_over(px, W, H, lx + (box - tw) / 2, ly + box + 2, coord, TEXT, font, ch);
}

static void draw(struct output *o)
{
    if (!o->configured || !o->w || !o->h) return;
    o->dirty = 0;
    o->nhits = 0;
    uint32_t s120 = o->viewport ? o->scale120 : 120;
    int W = buf_w(o), H = buf_h(o);
    int row = text_px(s120 / 120.0), cw = row / 2, ch = row;
    const struct panel_font *font = panel_font(row, 0), *bold = panel_font(row, 1);
    panel_font_size(font, &cw, &ch);
    int line = s120 >= 180 ? 3 : 2;

    if (o->bg.px) {
        scale_image(&o->bg, &o->scaled, W, H);
    } else if (!o->scaled.px || o->scaled.w != W || o->scaled.h != H) {
        free(o->scaled.px);
        o->scaled.px = calloc((size_t)W * (size_t)H, 4);
        o->scaled.w = W;
        o->scaled.h = H;
    }

    size_t size = (size_t)W * (size_t)H * 4;
    int fd = memfd_create("locachy-shot", MFD_CLOEXEC);
    uint32_t *px = MAP_FAILED;
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0 ||
        (px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        if (fd >= 0) close(fd);
        return;
    }

    /* Base image with annotations */
    struct image annotated = { 0 };
    annotated.px = malloc(size);
    annotated.w = W;
    annotated.h = H;
    if (annotated.px && o->scaled.px) {
        memcpy(annotated.px, o->scaled.px, size);
        render_annotations(annotated.px, W, H, o, 0, 0);
    }

    /* Darkened veil everywhere outside the selection; none with the whole
     * desktop chosen (Full screen), every screen outlined instead */
    int whole = opt_mode == MODE_FULLSCREEN && !sel_has && !picking_window;
    for (int i = 0; i < W * H; i++) {
        uint32_t base = annotated.px ? annotated.px[i] : 0xFF000000u;
        px[i] = whole ? base | 0xFF000000u : blend(base | 0xFF000000u, VEIL);
    }
    if (whole) round_rect(px, W, H, 0, 0, W, H, 0, 2 * line, HIGHLIGHT);

    /* Bright selection region */
    if (sel_has && sel_out == o && sel_w > 0 && sel_h > 0) {
        int x0 = sel_x < 0 ? 0 : sel_x, y0 = sel_y < 0 ? 0 : sel_y;
        int x1 = sel_x + sel_w > W ? W : sel_x + sel_w;
        int y1 = sel_y + sel_h > H ? H : sel_y + sel_h;
        if (picked_window && picked_window->img.px) {
            struct image wsc = { 0 };
            if (scale_image(&picked_window->img, &wsc, sel_w, sel_h) == 0) {
                for (int y = y0; y < y1; y++) {
                    memcpy(px + (size_t)y * W + x0, wsc.px + (size_t)(y - sel_y) * sel_w + (x0 - sel_x),
                           (size_t)(x1 - x0) * 4);
                }
                free(wsc.px);
            }
        } else if (annotated.px) {
            for (int y = y0; y < y1; y++) {
                memcpy(px + (size_t)y * W + x0, annotated.px + (size_t)y * W + x0, (size_t)(x1 - x0) * 4);
            }
        }

        /* Selection border and 8 resize handles */
        round_rect(px, W, H, sel_x - line, sel_y - line, sel_w + 2 * line, sel_h + 2 * line, 0, line, HIGHLIGHT);
        int hx[8], hy[8], hr = HANDLE_R * (s120 >= 180 ? 2 : 1);
        get_handles(sel_x, sel_y, sel_w, sel_h, hx, hy);
        for (int i = 0; i < 8; i++) {
            draw_disc(px, W, H, hx[i], hy[i], hr + 1, 0xFFFFFFFFu);
            draw_disc(px, W, H, hx[i], hy[i], hr, HIGHLIGHT);
        }

        /* Pixel size badge: WxH (X, Y) */
        char badge[96];
        snprintf(badge, sizeof badge, "%dx%d (%d, %d)", sel_w, sel_h, sel_x, sel_y);
        int pad = ch / 3;
        int bw = panel_text_width(badge, bold) + 2 * pad, bh = ch + ch / 3;
        int bx = sel_x, by = sel_y - bh - 6;
        if (by < 4) by = sel_y + sel_h + 6;
        if (by + bh > H - 4) by = sel_y + 4;
        if (bx + bw > W - 4) bx = W - bw - 4;
        if (bx < 4) bx = 4;
        round_rect(px, W, H, bx, by, bw, bh, ch / 4, 0, PANEL_DARK);
        text_over(px, W, H, bx + pad, by + ch / 6, badge, TEXT, bold, ch);
    }

    /* Magnifier loupe while dragging a new selection or resizing handles */
    if (sel_out == o && (drag_state == DRAG_NEW || (drag_state >= DRAG_NW && drag_state <= DRAG_W))) {
        draw_magnifier(px, W, H, &annotated, cur_bx, cur_by, font, ch);
    }
    free(annotated.px);

    draw_hud(o, px, W, H, font, bold, ch, row);

    munmap(px, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(buffer, &buffer_listener, NULL);
    wl_surface_attach(o->surf, buffer, 0, 0);
    if (o->viewport) wp_viewport_set_destination(o->viewport, (int32_t)o->w, (int32_t)o->h);
    wl_surface_damage_buffer(o->surf, 0, 0, W, H);
    wl_surface_commit(o->surf);
}

/* Input handling */

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void keyboard_to(struct output *o)
{
    if (!o || o->keyboard) return;
    for (struct output *e = outputs; e; e = e->next) {
        e->keyboard = e == o;
    }
}

static int hit_at(const struct output *o, int bx, int by)
{
    for (int i = 0; i < o->nhits; i++) {
        const struct hit *h = &o->hits[i];
        if (bx >= h->x && bx < h->x + h->w && by >= h->y && by < h->y + h->h) {
            return h->id;
        }
    }
    return -1;
}

static enum drag_kind handle_at(const struct output *o, int bx, int by)
{
    if (!sel_has || sel_out != o) return DRAG_NONE;
    uint32_t s120 = o->viewport ? o->scale120 : 120;
    int r = (HANDLE_R + 4) * (s120 >= 180 ? 2 : 1);
    int hx[8], hy[8];
    get_handles(sel_x, sel_y, sel_w, sel_h, hx, hy);
    static const enum drag_kind kinds[8] = {
        DRAG_NW, DRAG_N, DRAG_NE, DRAG_E, DRAG_SE, DRAG_S, DRAG_SW, DRAG_W
    };
    for (int i = 0; i < 8; i++) {
        int dx = bx - hx[i], dy = by - hy[i];
        if (dx * dx + dy * dy <= r * r) return kinds[i];
    }
    return DRAG_NONE;
}

static void normalize_rect(int x0, int y0, int x1, int y1, int W, int H, int *rx, int *ry, int *rw, int *rh)
{
    if (x0 < 0) x0 = 0;
    if (x0 > W) x0 = W;
    if (x1 < 0) x1 = 0;
    if (x1 > W) x1 = W;
    if (y0 < 0) y0 = 0;
    if (y0 > H) y0 = H;
    if (y1 < 0) y1 = 0;
    if (y1 > H) y1 = H;
    *rx = x0 < x1 ? x0 : x1;
    *ry = y0 < y1 ? y0 : y1;
    *rw = abs(x1 - x0);
    *rh = abs(y1 - y0);
}

static void undo_anno(void)
{
    if (nannos > 0) {
        nannos--;
        if (annos[nannos].type == TOOL_NUMBER && next_badge_num > 1) {
            next_badge_num--;
        }
        free(annos[nannos].pts);
        annos[nannos].pts = NULL;
        dirty_all();
    }
}

static void handle_hud_click(int id)
{
    switch (id) {
    case HIT_MODE_REGION:
        active_tool = TOOL_NONE;
        trigger_mode(MODE_REGION);
        break;
    case HIT_MODE_FULL:
        trigger_mode(MODE_FULLSCREEN);
        break;
    case HIT_MODE_CURRENT:
        trigger_mode(MODE_CURRENT);
        break;
    case HIT_MODE_ACTIVE:
        trigger_mode(MODE_ACTIVEWIN);
        break;
    case HIT_MODE_WINDOW:
        trigger_mode(MODE_UNDERCURSOR);
        break;
    case HIT_TOOL_ARROW:
        active_tool = active_tool == TOOL_ARROW ? TOOL_NONE : TOOL_ARROW;
        dirty_all();
        break;
    case HIT_TOOL_RECT:
        active_tool = active_tool == TOOL_RECT ? TOOL_NONE : TOOL_RECT;
        dirty_all();
        break;
    case HIT_TOOL_PEN:
        active_tool = active_tool == TOOL_PEN ? TOOL_NONE : TOOL_PEN;
        dirty_all();
        break;
    case HIT_TOOL_HILITE:
        active_tool = active_tool == TOOL_HILITE ? TOOL_NONE : TOOL_HILITE;
        dirty_all();
        break;
    case HIT_TOOL_NUMBER:
        active_tool = active_tool == TOOL_NUMBER ? TOOL_NONE : TOOL_NUMBER;
        dirty_all();
        break;
    case HIT_UNDO:
        undo_anno();
        break;
    case HIT_COPY:
        finish_with(FINISH_COPY);
        break;
    case HIT_SAVE:
        finish_with(FINISH_SAVE);
        break;
    case HIT_VIEW:
        finish_with(FINISH_VIEW);
        break;
    case HIT_CANCEL:
        exit_code = 1;
        finished = 1;
        break;
    }
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    pointer_output = NULL;
    for (struct output *o = outputs; o; o = o->next) {
        if (o->surf == s) pointer_output = o;
    }
    pointer_x = wl_fixed_to_double(x);
    pointer_y = wl_fixed_to_double(y);
    struct wl_cursor *cursor = cursor_theme ? wl_cursor_theme_get_cursor(cursor_theme, "crosshair") : NULL;
    if (!cursor && cursor_theme) cursor = wl_cursor_theme_get_cursor(cursor_theme, "left_ptr");
    if (!cursor && cursor_theme) cursor = wl_cursor_theme_get_cursor(cursor_theme, "default");
    if (cursor && cursor_surface) {
        struct wl_cursor_image *image = cursor->images[0];
        wl_surface_attach(cursor_surface, wl_cursor_image_get_buffer(image), 0, 0);
        wl_surface_damage_buffer(cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
        wl_surface_commit(cursor_surface);
        wl_pointer_set_cursor(p, serial, cursor_surface, (int32_t)image->hotspot_x, (int32_t)image->hotspot_y);
    }
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
    (void)data; (void)p; (void)serial; (void)s;
    if (pointer_output && pointer_output->hovered_hit >= 0) {
        pointer_output->hovered_hit = -1;
        pointer_output->dirty = 1;
    }
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)p; (void)time;
    pointer_x = wl_fixed_to_double(x);
    pointer_y = wl_fixed_to_double(y);
    struct output *o = pointer_output ? pointer_output : focused_output();
    if (!o) return;
    keyboard_to(o);
    uint32_t s120 = o->viewport ? o->scale120 : 120;
    int bx = (int)(pointer_x * s120 / 120.0 + 0.5);
    int by = (int)(pointer_y * s120 / 120.0 + 0.5);
    int W = buf_w(o), H = buf_h(o);
    cur_bx = bx;
    cur_by = by;

    if (drag_state == DRAG_NONE) {
        int h = hit_at(o, bx, by);
        if (h != o->hovered_hit) {
            o->hovered_hit = h;
            o->dirty = 1;
        }
        return;
    }

    if (drag_state == DRAG_ANNO && nannos > 0) {
        struct annotation *a = &annos[nannos - 1];
        a->x1 = bx;
        a->y1 = by;
        if (a->type == TOOL_PEN && a->npts < a->cap_pts) {
            a->pts[a->npts++] = (struct pt){ bx, by };
        }
        o->dirty = 1;
        return;
    }

    if (sel_out != o) return;
    picked_window = NULL;

    if (drag_state == DRAG_NEW) {
        normalize_rect(drag_x0, drag_y0, bx, by, W, H, &sel_x, &sel_y, &sel_w, &sel_h);
        sel_has = sel_w > 0 && sel_h > 0;
    } else if (drag_state == DRAG_MOVE) {
        int nx = drag_orig_x + (bx - drag_x0);
        int ny = drag_orig_y + (by - drag_y0);
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx + sel_w > W) nx = W - sel_w;
        if (ny + sel_h > H) ny = H - sel_h;
        sel_x = nx;
        sel_y = ny;
    } else {
        int x0 = drag_orig_x, y0 = drag_orig_y;
        int x1 = drag_orig_x + drag_orig_w, y1 = drag_orig_y + drag_orig_h;
        if (drag_state == DRAG_NW || drag_state == DRAG_W || drag_state == DRAG_SW) x0 = bx;
        if (drag_state == DRAG_NE || drag_state == DRAG_E || drag_state == DRAG_SE) x1 = bx;
        if (drag_state == DRAG_NW || drag_state == DRAG_N || drag_state == DRAG_NE) y0 = by;
        if (drag_state == DRAG_SW || drag_state == DRAG_S || drag_state == DRAG_SE) y1 = by;
        normalize_rect(x0, y0, x1, y1, W, H, &sel_x, &sel_y, &sel_w, &sel_h);
    }
    o->dirty = 1;
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    (void)data; (void)p; (void)serial; (void)time;
    struct output *o = pointer_output ? pointer_output : focused_output();
    if (!o) return;
    keyboard_to(o);
    uint32_t s120 = o->viewport ? o->scale120 : 120;
    int bx = (int)(pointer_x * s120 / 120.0 + 0.5);
    int by = (int)(pointer_y * s120 / 120.0 + 0.5);
    cur_bx = bx;
    cur_by = by;

    if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
        if (drag_state != DRAG_NONE) {
            drag_state = DRAG_NONE;
            o->dirty = 1;
        }
        return;
    }

    if (button == BTN_RIGHT) {
        if (sel_has) {
            sel_has = 0;
            picked_window = NULL;
            dirty_all();
        } else {
            exit_code = 1;
            finished = 1;
        }
        return;
    }
    if (button != BTN_LEFT) return;

    /* Check HUD buttons first */
    int hit = hit_at(o, bx, by);
    if (hit > 0) {
        handle_hud_click(hit);
        return;
    }

    /* Window picker mode: select window on this output */
    if (picking_window) {
        picking_window = 0;
        struct window *pick = NULL;
        for (struct window *w = windows; w; w = w->next) {
            if (!w->img.px) continue;
            for (int i = 0; i < w->noutputs; i++) {
                if (w->outputs[i] == o->wl) {
                    if (!pick || w->activated) pick = w;
                }
            }
        }
        if (!pick) pick = active_window();
        if (pick && pick->img.px) {
            picked_window = pick;
            int W = buf_w(o), H = buf_h(o);
            sel_out = o;
            sel_has = 1;
            sel_w = pick->img.w <= W ? pick->img.w : W;
            sel_h = pick->img.h <= H ? pick->img.h : H;
            sel_x = (W - sel_w) / 2;
            sel_y = (H - sel_h) / 2;
        }
        dirty_all();
        return;
    }

    /* Annotation tool active */
    if (active_tool != TOOL_NONE && nannos < MAX_ANNOS) {
        struct annotation *a = &annos[nannos++];
        memset(a, 0, sizeof *a);
        a->type = active_tool;
        a->out = o;
        a->x0 = a->x1 = bx;
        a->y0 = a->y1 = by;
        if (active_tool == TOOL_NUMBER) {
            a->num = next_badge_num++;
            o->dirty = 1;
            return;
        }
        if (active_tool == TOOL_PEN) {
            a->cap_pts = MAX_PTS;
            a->pts = malloc((size_t)a->cap_pts * sizeof *a->pts);
            if (a->pts) a->pts[a->npts++] = (struct pt){ bx, by };
        }
        drag_state = DRAG_ANNO;
        o->dirty = 1;
        return;
    }

    /* Double-click inside the selection: accept it, copied */
    uint32_t ms = now_ms();
    int inside = sel_has && sel_out == o && bx >= sel_x && bx < sel_x + sel_w && by >= sel_y && by < sel_y + sel_h;
    if (inside && ms - last_click_ms < 350 && hypot(pointer_x - last_click_x, pointer_y - last_click_y) < 10) {
        finish_with(FINISH_COPY);
        return;
    }
    last_click_ms = ms;
    last_click_x = pointer_x;
    last_click_y = pointer_y;

    /* Resize handle? */
    enum drag_kind hk = handle_at(o, bx, by);
    if (hk != DRAG_NONE) {
        drag_state = hk;
        drag_x0 = bx;
        drag_y0 = by;
        drag_orig_x = sel_x;
        drag_orig_y = sel_y;
        drag_orig_w = sel_w;
        drag_orig_h = sel_h;
        o->dirty = 1;
        return;
    }

    /* Move existing selection? */
    if (inside) {
        drag_state = DRAG_MOVE;
        drag_x0 = bx;
        drag_y0 = by;
        drag_orig_x = sel_x;
        drag_orig_y = sel_y;
        o->dirty = 1;
        return;
    }

    /* Start a new selection rectangle */
    sel_out = o;
    sel_has = 0;
    picked_window = NULL;
    if (opt_mode == MODE_FULLSCREEN) opt_mode = MODE_REGION;
    drag_state = DRAG_NEW;
    drag_x0 = bx;
    drag_y0 = by;
    dirty_all();
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{ (void)data; (void)p; (void)time; (void)axis; (void)value; }

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion, .button = pointer_button,
    .axis = pointer_axis,
};

static void key_pressed(xkb_keysym_t sym, int ctrl)
{
    if (ctrl && (sym == XKB_KEY_z || sym == XKB_KEY_Z)) {
        undo_anno();
        return;
    }
    if (ctrl && (sym == XKB_KEY_c || sym == XKB_KEY_C)) {
        finish_with(FINISH_COPY);
        return;
    }
    if (ctrl && (sym == XKB_KEY_s || sym == XKB_KEY_S)) {
        finish_with(FINISH_SAVE);
        return;
    }
    switch (sym) {
    case XKB_KEY_Escape:
        if (picking_window) {
            picking_window = 0;
            dirty_all();
        } else if (active_tool != TOOL_NONE) {
            active_tool = TOOL_NONE;
            dirty_all();
        } else if (sel_has) {
            sel_has = 0;
            picked_window = NULL;
            dirty_all();
        } else if (opt_mode == MODE_FULLSCREEN) {
            opt_mode = MODE_REGION;
            dirty_all();
        } else {
            exit_code = 1;
            finished = 1;
        }
        break;
    case XKB_KEY_q:
    case XKB_KEY_Q:
        exit_code = 1;
        finished = 1;
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        finish_with(FINISH_COPY);
        break;
    case XKB_KEY_v:
    case XKB_KEY_V:
        finish_with(FINISH_VIEW);
        break;
    case XKB_KEY_1:
        active_tool = active_tool == TOOL_ARROW ? TOOL_NONE : TOOL_ARROW;
        dirty_all();
        break;
    case XKB_KEY_2:
        active_tool = active_tool == TOOL_RECT ? TOOL_NONE : TOOL_RECT;
        dirty_all();
        break;
    case XKB_KEY_3:
        active_tool = active_tool == TOOL_PEN ? TOOL_NONE : TOOL_PEN;
        dirty_all();
        break;
    case XKB_KEY_4:
        active_tool = active_tool == TOOL_HILITE ? TOOL_NONE : TOOL_HILITE;
        dirty_all();
        break;
    case XKB_KEY_5:
        active_tool = active_tool == TOOL_NUMBER ? TOOL_NONE : TOOL_NUMBER;
        dirty_all();
        break;
    case XKB_KEY_u:
    case XKB_KEY_U:
        undo_anno();
        break;
    case XKB_KEY_f:
    case XKB_KEY_F:
        trigger_mode(MODE_FULLSCREEN);
        break;
    case XKB_KEY_m:
    case XKB_KEY_M:
        trigger_mode(MODE_CURRENT);
        break;
    case XKB_KEY_a:
    case XKB_KEY_A:
        trigger_mode(MODE_ACTIVEWIN);
        break;
    case XKB_KEY_w:
    case XKB_KEY_W:
        trigger_mode(MODE_UNDERCURSOR);
        break;
    case XKB_KEY_r:
    case XKB_KEY_R:
        trigger_mode(MODE_REGION);
        break;
    default:
        break;
    }
}

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)k;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    if (xkb_st) xkb_state_unref(xkb_st);
    if (xkb_map) xkb_keymap_unref(xkb_map);
    xkb_map = xkb_keymap_new_from_string(xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    xkb_st = xkb_map ? xkb_state_new(xkb_map) : NULL;
}

static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{ (void)data; (void)k; (void)serial; (void)s; (void)keys; }
static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{ (void)data; (void)k; (void)serial; (void)s; }

static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)data; (void)k; (void)serial; (void)time;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !xkb_st) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_st, key + 8);
    int ctrl = xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
    key_pressed(sym, ctrl);
}

static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat,
                         uint32_t lock, uint32_t group)
{
    (void)data; (void)k; (void)serial;
    if (xkb_st) xkb_state_update_mask(xkb_st, dep, lat, lock, 0, 0, group);
}

static void kb_repeat(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{ (void)data; (void)k; (void)rate; (void)delay; }

static const struct wl_keyboard_listener keyboard_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
        wl_pointer_destroy(pointer);
        pointer = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) {
        wl_keyboard_destroy(keyboard);
        keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *s, const char *name) { (void)data; (void)s; (void)name; }
static const struct wl_seat_listener seat_listener = { seat_capabilities, seat_name };

/* Registry & main loop */

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
    (void)data;
#define IS(i) (strcmp(iface, i##_interface.name) == 0)
    if (IS(wl_compositor)) {
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (IS(wl_shm)) {
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (IS(wl_seat) && !seat) {
        seat = wl_registry_bind(r, name, &wl_seat_interface, version < 4 ? version : 4);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (IS(wl_output)) {
        struct output *o = calloc(1, sizeof *o), **tail = &outputs;
        if (!o) return;
        o->name = name;
        o->wl = wl_registry_bind(r, name, &wl_output_interface, version < 4 ? version : 4);
        wl_output_add_listener(o->wl, &wl_output_listener, o);
        while (*tail) tail = &(*tail)->next;
        *tail = o;
    } else if (IS(zwlr_layer_shell_v1)) {
        layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (IS(zwlr_foreign_toplevel_manager_v1)) {
        toplevels = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, version < 3 ? version : 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(toplevels, &toplevels_listener, NULL);
    } else if (IS(zwlr_output_manager_v1)) {
        output_mgr = wl_registry_bind(r, name, &zwlr_output_manager_v1_interface, version < 4 ? version : 4);
        zwlr_output_manager_v1_add_listener(output_mgr, &output_mgr_listener, NULL);
    } else if (IS(ext_foreign_toplevel_list_v1)) {
        toplevel_list = wl_registry_bind(r, name, &ext_foreign_toplevel_list_v1_interface, 1);
        ext_foreign_toplevel_list_v1_add_listener(toplevel_list, &list_listener, NULL);
    } else if (IS(ext_foreign_toplevel_image_capture_source_manager_v1)) {
        window_sources = wl_registry_bind(r, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
    } else if (IS(ext_output_image_capture_source_manager_v1)) {
        output_sources = wl_registry_bind(r, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (IS(ext_image_copy_capture_manager_v1)) {
        copier = wl_registry_bind(r, name, &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (IS(wp_fractional_scale_manager_v1)) {
        scale_mgr = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (IS(wp_viewporter)) {
        viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
    }
#undef IS
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name)
{
    (void)data; (void)r; (void)name;
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static void on_stop(int sig)
{
    (void)sig;
    stopping = 1;
}

static int parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-g") == 0 || strcmp(a, "--gui") == 0) {
            opt_mode = MODE_GUI;
        } else if (strcmp(a, "-r") == 0 || strcmp(a, "--region") == 0) {
            opt_mode = MODE_REGION;
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--fullscreen") == 0) {
            opt_mode = MODE_FULLSCREEN;
        } else if (strcmp(a, "-m") == 0 || strcmp(a, "--current") == 0) {
            opt_mode = MODE_CURRENT;
        } else if (strcmp(a, "-a") == 0 || strcmp(a, "--activewindow") == 0) {
            opt_mode = MODE_ACTIVEWIN;
        } else if (strcmp(a, "-u") == 0 || strcmp(a, "--windowundercursor") == 0) {
            opt_mode = MODE_UNDERCURSOR;
        } else if (strcmp(a, "-b") == 0 || strcmp(a, "--background") == 0) {
            opt_background = 1;
        } else if (strcmp(a, "-n") == 0 || strcmp(a, "--nonotify") == 0) {
            /* accepted for Spectacle CLI compatibility */
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--copy-image") == 0) {
            opt_copy_image = 1;
        } else if (strcmp(a, "-C") == 0 || strcmp(a, "--copy-path") == 0) {
            opt_copy_path = 1;
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) {
            if (++i >= argc) return -1;
            opt_output = argv[i];
        } else if (strncmp(a, "--output=", 9) == 0) {
            opt_output = a + 9;
        } else {
            fprintf(stderr, "usage: locachy-shot [-g|-r|-f|-m|-a|-u] [-b [-c|-C]] [-n] [-o FILE]\n");
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (parse_args(argc, argv) < 0) return 2;
    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);

    display = wl_display_connect(NULL);
    if (!display) {
        say("locachy-shot: no compositor");
        return 1;
    }
    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); /* windows, outputs, output heads */
    if (!compositor || !shm || !copier || !outputs) {
        say("locachy-shot: compositor lacks required capture or shm protocols");
        wl_display_disconnect(display);
        return 1;
    }

    pair_windows();
    for (struct window *w = windows; w; w = w->next) {
        capture(&w->cap, w->listed && window_sources ?
                ext_foreign_toplevel_image_capture_source_manager_v1_create_source(window_sources, w->listed->handle) :
                NULL, &w->img);
    }
    struct capture bg_caps[16];
    int nbg = 0;
    for (struct output *o = outputs; o && nbg < 16; o = o->next) {
        capture(&bg_caps[nbg++], output_sources ?
                ext_output_image_capture_source_manager_v1_create_source(output_sources, o->wl) : NULL, &o->bg);
    }
    for (int tries = 0; tries < 60; tries++) {
        int pending = 0;
        for (int i = 0; i < nbg; i++) pending |= !bg_caps[i].done;
        for (struct window *w = windows; w; w = w->next) pending |= !w->cap.done;
        if (!pending) break;
        wl_display_roundtrip(display);
        nanosleep(&(struct timespec){ 0, 10000000 }, NULL);
    }
    for (int i = 0; i < nbg; i++) cap_free(&bg_caps[i]);
    for (struct window *w = windows; w; w = w->next) cap_free(&w->cap);
    sync_output_geometries();

    /* Background mode (-b): saved, or with -c (and no -o) only copied */
    if (opt_background) {
        char path[PATH_MAX];
        int r = opt_copy_image && !opt_output ? perform_copy() : perform_save(path, sizeof path);
        wl_display_disconnect(display);
        return r == 0 ? 0 : 1;
    }

    if (!layer_shell) {
        say("locachy-shot: compositor lacks wlr-layer-shell");
        wl_display_disconnect(display);
        return 1;
    }

    cursor_theme = wl_cursor_theme_load(NULL, 24, shm);
    cursor_surface = wl_compositor_create_surface(compositor);

    if (opt_mode == MODE_UNDERCURSOR) {
        picking_window = 1;
    }

    struct output *focus = focused_output();
    for (struct output *o = outputs; o; o = o->next) {
        create_surface(o, o == focus);
    }
    if (opt_mode == MODE_FULLSCREEN) {
        /* Interactive fullscreen pre-selects the focused screen or lets user save all */
        sel_has = 0;
    }

    while (!finished && !stopping) {
        wl_display_dispatch_pending(display);
        if (finished || stopping) break;
        for (struct output *o = outputs; o; o = o->next) {
            if (o->dirty && o->surf) draw(o);
        }
        wl_display_flush(display);
        struct pollfd p = { .fd = wl_display_get_fd(display), .events = POLLIN };
        if (poll(&p, 1, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if ((p.revents & (POLLIN | POLLHUP | POLLERR)) && wl_display_dispatch(display) < 0) break;
    }

    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return exit_code;
}
