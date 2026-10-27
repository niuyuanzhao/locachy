/*
 * locachy-overview (/usr/local/bin): the shared display's window overview,
 * as KDE's Overview is (KWin's overview effect: Main.qml, WindowHeap.qml,
 * WindowHeapDelegate.qml, expolayout.cpp). locachy-bar starts it when the
 * pointer is pushed into the top left corner; labwc's Super+W runs it; run
 * again while it is up, it closes it (as Meta+W does). --show and --hide
 * (libinput-gestures, lisgd: KWin's swipes up and down) only show or close.
 *
 * On every screen, over everything: the screen's own picture blurred under
 * the theme's background at 0.7, a search field at the top, and that
 * screen's windows, minimized ones too, as live pictures (ext-image-copy-
 * capture of ext-foreign-toplevel-list's windows, a frame asked for as soon
 * as one comes), laid out as KWin's ExpoLayout does: one scale for all, so
 * they keep their sizes relative to each other, at most their size on the
 * screen, in centred rows. Over each one's bottom edge its container's name
 * (where KDE puts the application's icon), under that its title.
 *   pointer over a window: framed, with a close button at its top right;
 *     touched, every window shows it (until the pointer moves)
 *   click a window: it is activated (brought back if minimized), and the
 *     overview ends; middle click or its close button: the window is closed
 *   touch: a tap as a click; a window dragged follows the finger, fading,
 *     and let go 0.6 of the way down to the bottom of the windows, closes
 *   click elsewhere, Esc (the search cleared first if there is one), or run
 *     again (Super+W, the top left corner pushed again: the overview leaves
 *     that pixel to locachy-bar, whose corner it is): the overview ends
 *   typing: the search, which leaves the windows whose title, application or
 *     container has it ("No matching windows" if none); Backspace
 *   arrows: the selection moves to the window that way; Home, End: the
 *     first, last; Enter (Space while not searching): the selected one, or
 *     the only one left, activated; the keys go to the screen last
 *     interacted with
 * Activating and closing go through wlr-foreign-toplevel-management, whose
 * windows are paired with ext-foreign-toplevel-list's by application and
 * title. Not here: KDE's virtual desktop bar, its grid of desktops and
 * moving a window to another desktop (there are no virtual desktops),
 * KRunner's results (no KRunner), dragging a window to another screen and
 * the 300 ms animation from the windows' places (no protocol lets a client
 * move a window, or tells it where windows are).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "base.h"

#define VEIL       0xB3202326u /* KDE: the theme's background at 0.7 over the blurred desktop */
#define TEXT       0xFFFCFCFCu
#define DIM_TEXT   0xFFA0A8B0u
#define HIGHLIGHT  0xFF3DAEE9u /* KDE's highlight colour (Breeze) */
#define LABEL_BG   0xFF202326u /* Breeze's window background, KDE's caption ground */
#define FIELD_BG   0xFF141618u /* Breeze's view background, the search field's */
#define CLOSE_RED  0xFFDA4453u /* Breeze's negative colour */
#define BLUR_DOWN  8           /* the background blurred at 1/8 of the screen's size */
#define MAX_SCALE  1.0         /* ExpoLayout never enlarges a window */
#define MAX_SHOWN  64

struct image {
    uint32_t *px;
    int w, h;
};

struct output {
    uint32_t name;
    struct wl_output *wl;
    struct wl_surface *surf;
    struct zwlr_layer_surface_v1 *layer;
    struct wp_fractional_scale_v1 *fscale;
    struct wp_viewport *viewport;
    uint32_t scale120, w, h; /* logical size, from the configure */
    int configured, dirty, keyboard;
    struct image bg, veiled;  /* captured (then blurred, small), and veiled at the drawn size */
    int heap_bottom;          /* the windows' area's bottom, in the screen's pixels */
    int selected, hovered, hover_close;
    struct output *next;
};

struct window;
struct capture {
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;
    struct wl_buffer *buffer;
    uint32_t *px;
    int w, h;          /* the session's size */
    int bw, bh;        /* the buffer's */
    uint32_t format, bformat; /* the session's wl_shm format, and the buffer's */
    int rank, done;    /* rank: format's (0: none read here); done: a first frame, or none to come */
    uint32_t pending, offered; /* the format of the constraints being sent; the last offered */
    int pending_rank;
    struct image *into;
    struct window *win; /* a window's (kept up to date), or a screen's */
};

struct listed {               /* a window as ext-foreign-toplevel-list has it */
    struct ext_foreign_toplevel_handle_v1 *handle;
    char title[128], app_id[64];
    int paired;
    struct listed *next;
};

struct window {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char title[128], app_id[64], container[OPEN_NAME];
    int minimized, activated;
    struct wl_output *outputs[4];
    int noutputs;
    struct listed *listed;
    struct capture cap;
    struct image thumb, scaled; /* captured, and at its place's size */
    int x, y, w, h;             /* its place on its screen, in the screen's pixels */
    struct window *next;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct wl_touch *touch;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_foreign_toplevel_manager_v1 *toplevels;
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
static struct window *windows;
static struct listed *listed;
static struct output *pointer_output;
static double pointer_x, pointer_y;
static char search[128];
static volatile sig_atomic_t stopping;
static int finished; /* a window activated, or the overview closed */
/* Touch, as KDE's overview takes it: a tap is a click, and once the screen
 * is touched every window shows its close button (until the pointer moves);
 * a window dragged (more than Qt's 10 pixels) follows the finger, fading,
 * with "Drag Down To Close" on it, and let go more than 0.6 of the way from
 * where it was pressed to the bottom of the windows' area, it is closed. */
#define DRAG_START 10
static struct {
    int32_t id;                  /* the finger followed; -1: none */
    struct output *o;
    double x0, y0, x, y;         /* where it was pressed and is, logical */
    int win, on_close, dragging; /* the window pressed (-1: none), on its close button */
} touching = { .id = -1 };
static int touched;
static char pid_path[128];

static int window_on(const struct window *w, const struct output *o);

static void dirty_all(void)
{
    for (struct output *o = outputs; o; o = o->next) {
        o->dirty = 1;
    }
}

/* Pixels */

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

/* How far (x, y) lies into a rounded rectangle's corner circle: 0 outside
 * the corners' squares. */
static int corner(int x, int y, int rx, int ry, int rw, int rh, int r, int *dx, int *dy)
{
    *dx = x < rx + r ? rx + r - x : x >= rx + rw - r ? x - (rx + rw - r - 1) : 0;
    *dy = y < ry + r ? ry + r - y : y >= ry + rh - r ? y - (ry + rh - r - 1) : 0;
    return *dx && *dy;
}

/* A rounded rectangle, filled, or (line > 0) only its border that wide. */
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

/* src scaled to w x h, each pixel the average of (some of) those it covers. */
static int scale_image(const struct image *src, struct image *dst, int w, int h)
{
    if (dst->px && dst->w == w && dst->h == h) return 0;
    free(dst->px);
    dst->px = NULL;
    if (w <= 0 || h <= 0 || !src->px || !(dst->px = malloc((size_t)w * (size_t)h * 4))) return -1;
    dst->w = w;
    dst->h = h;
    for (int y = 0; y < h; y++) {
        int sy0 = (int)((long)y * src->h / h), sy1 = (int)((long)(y + 1) * src->h / h);
        sy1 = sy1 > sy0 ? sy1 : sy0 + 1;
        for (int x = 0; x < w; x++) {
            int sx0 = (int)((long)x * src->w / w), sx1 = (int)((long)(x + 1) * src->w / w);
            sx1 = sx1 > sx0 ? sx1 : sx0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < src->h; sy += 1 + (sy1 - sy0) / 4) {
                for (int sx = sx0; sx < sx1 && sx < src->w; sx += 1 + (sx1 - sx0) / 4) {
                    uint32_t p = src->px[sy * src->w + sx];
                    r += p >> 16 & 255;
                    g += p >> 8 & 255;
                    b += p & 255;
                    n++;
                }
            }
            n = n ? n : 1;
            dst->px[y * w + x] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | b / n;
        }
    }
    return 0;
}

/* src enlarged to w x h, each pixel between its four nearest (bilinear): the
 * blurred background, which scale_image would show in blocks. */
static int smooth_image(const struct image *src, struct image *dst, int w, int h)
{
    free(dst->px);
    dst->px = NULL;
    if (w <= 0 || h <= 0 || !src->px || !(dst->px = malloc((size_t)w * (size_t)h * 4))) return -1;
    dst->w = w;
    dst->h = h;
    for (int y = 0; y < h; y++) {
        double fy = fmax(0, (y + 0.5) * src->h / h - 0.5), ty;
        int y0 = (int)fy, y1 = y0 + 1 < src->h ? y0 + 1 : y0;
        ty = fy - y0;
        for (int x = 0; x < w; x++) {
            double fx = fmax(0, (x + 0.5) * src->w / w - 0.5), tx;
            int x0 = (int)fx, x1 = x0 + 1 < src->w ? x0 + 1 : x0;
            tx = fx - x0;
            uint32_t a = src->px[y0 * src->w + x0], b = src->px[y0 * src->w + x1];
            uint32_t c = src->px[y1 * src->w + x0], d = src->px[y1 * src->w + x1], out = 0xFF000000u;
            for (int s = 0; s < 24; s += 8) {
                double top = (a >> s & 255) * (1 - tx) + (b >> s & 255) * tx;
                double bottom = (c >> s & 255) * (1 - tx) + (d >> s & 255) * tx;
                out |= (uint32_t)(top * (1 - ty) + bottom * ty + 0.5) << s;
            }
            dst->px[y * w + x] = out;
        }
    }
    return 0;
}

/* src at w x h, made again only when that size or src changed: averaged
 * when smaller, bilinear when larger. */
static int fit_image(const struct image *src, struct image *dst, int w, int h)
{
    if (dst->px && dst->w == w && dst->h == h) return 0;
    return w > src->w ? smooth_image(src, dst, w, h) : scale_image(src, dst, w, h);
}

/* Text over whatever is drawn there: rendered white on black, which is then
 * how much of fg covers each pixel. */
static void text_over(uint32_t *px, int W, int H, int x, int y, const char *s, uint32_t fg,
                      const struct panel_font *f, int ch)
{
    int tw = panel_text_width(s, f);
    uint32_t *mask = tw > 0 && ch > 0 ? calloc((size_t)tw * (size_t)ch, 4) : NULL;
    if (!mask) return;
    panel_text(mask, tw, ch, tw, 0, 0, s, 0xFFFFFFFFu, 0xFF000000u, f);
    for (int yy = 0; yy < ch; yy++) {
        for (int xx = 0; xx < tw; xx++) {
            if (x + xx >= 0 && x + xx < W && y + yy >= 0 && y + yy < H) {
                uint32_t *p = &px[(y + yy) * W + x + xx];
                *p = blend(*p, (mask[yy * tw + xx] >> 8 & 255) << 24 | (fg & 0xFFFFFFu));
            }
        }
    }
    free(mask);
}

/* The screen's picture as KDE shows the desktop under the overview: made
 * smaller and blurred (a box blur, three passes each way). */
static void blur(struct image *im)
{
    struct image small = { 0 };
    if (scale_image(im, &small, im->w / BLUR_DOWN > 0 ? im->w / BLUR_DOWN : 1,
                    im->h / BLUR_DOWN > 0 ? im->h / BLUR_DOWN : 1) < 0) {
        return;
    }
    uint32_t *tmp = malloc((size_t)small.w * (size_t)small.h * 4);
    if (!tmp) {
        free(small.px);
        return;
    }
    const int r = 4;
    for (int pass = 0; pass < 3; pass++) {
        for (int dir = 0; dir < 2; dir++) {
            int len = dir ? small.h : small.w, lines = dir ? small.w : small.h;
            for (int l = 0; l < lines; l++) {
                for (int i = 0; i < len; i++) {
                    uint32_t sr = 0, sg = 0, sb = 0, n = 0;
                    for (int k = i - r; k <= i + r; k++) {
                        int c = k < 0 ? 0 : k >= len ? len - 1 : k;
                        uint32_t p = dir ? small.px[c * small.w + l] : small.px[l * small.w + c];
                        sr += p >> 16 & 255;
                        sg += p >> 8 & 255;
                        sb += p & 255;
                        n++;
                    }
                    tmp[dir ? i * small.w + l : l * small.w + i] = 0xFF000000u | (sr / n) << 16 | (sg / n) << 8 | sb / n;
                }
            }
            memcpy(small.px, tmp, (size_t)small.w * (size_t)small.h * 4);
        }
    }
    free(tmp);
    free(im->px);
    *im = small;
}

/* Capture: a screen's picture, once; a window's, kept up to date (KDE's
 * thumbnails are live): after each frame the next is asked for, which the
 * compositor sends when the window has changed. */

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

/* The screens to draw again: a window's, or all for a screen's picture. */
static void captured(struct capture *c)
{
    for (struct output *o = outputs; o; o = o->next) {
        if (!c->win || window_on(c->win, o)) o->dirty = 1;
    }
}

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
            if (swap) p = (p & 0x0000FF00u) | (p & 0xFFu) << 16 | (p >> 16 & 0xFFu); /* R and B, to XRGB */
            im->px[i] = p | 0xFF000000u; /* drawn opaque */
        }
    }
    if (c->win) { /* scaled again when drawn */
        free(c->win->scaled.px);
        c->win->scaled.px = NULL;
    }
    ext_image_copy_capture_frame_v1_destroy(f);
    c->frame = NULL;
    c->done = 1;
    captured(c);
    if (c->win) next_frame(c);
    else cap_free(c);
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason)
{
    struct capture *c = data;
    ext_image_copy_capture_frame_v1_destroy(f);
    c->frame = NULL;
    if (c->win && reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
        next_frame(c); /* the window's size changed: a buffer of the new size */
        return;
    }
    if (reason != EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
        say("locachy-overview: a capture failed (%u)", reason);
    }
    c->done = 1;
    cap_free(c);
    captured(c);
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

/* The formats a picture is read in, best first: the images' own XRGB order,
 * then RGBA in memory, which a GLES renderer reads back (labwc on the X230's
 * GPU offers only XBGR8888); 0: not read here. */
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

/* A frame asked for, into a buffer of the session's size (made anew when
 * that changed). */
static void next_frame(struct capture *c)
{
    int fresh = !c->buffer || c->bw != c->w || c->bh != c->h || c->bformat != c->format;
    if (fresh) {
        buffer_free(c);
        size_t size = (size_t)c->w * (size_t)c->h * 4;
        int fd = c->rank && c->w > 0 && c->h > 0 ? memfd_create("locachy-overview", MFD_CLOEXEC) : -1;
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
    ext_image_copy_capture_frame_v1_capture(c->frame); /* the compositor adds what changed since */
}

/* Its size and formats known (again after a change, which fails a frame
 * under way): the first frame. */
static void session_done(void *data, struct ext_image_copy_capture_session_v1 *s)
{
    struct capture *c = data;
    (void)s;
    c->rank = c->pending_rank;
    c->format = c->pending;
    c->pending_rank = 0;
    if (!c->rank) say("locachy-overview: no picture format it reads (the compositor offers 0x%08x)", c->offered);
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

/* A capture of source into into; win: a window's, kept up to date. */
static void capture(struct capture *c, struct ext_image_capture_source_v1 *source, struct image *into,
                    struct window *win)
{
    memset(c, 0, sizeof *c);
    c->into = into;
    c->win = win;
    if (!source) {
        c->done = 1;
        return;
    }
    c->session = ext_image_copy_capture_manager_v1_create_session(copier, source, 0);
    ext_image_copy_capture_session_v1_add_listener(c->session, &session_listener, c);
    ext_image_capture_source_v1_destroy(source); /* the session keeps what it needs */
}

/* Windows, as the two protocols list them */

static void win_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title)
{
    struct window *w = data;
    (void)h;
    snprintf(w->title, sizeof w->title, "%s", title);
}

static void win_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *id)
{
    struct window *w = data;
    char names[64][OPEN_NAME], ids[16][OPEN_WINDOW_ID];
    (void)h;
    snprintf(w->app_id, sizeof w->app_id, "%s", id);
    w->container[0] = '\0';
    for (int i = open_containers(names, 64) - 1; i >= 0 && !w->container[0]; i--) {
        int n = open_window_ids(names[i], ids, 16);
        for (int k = 0; k < n; k++) {
            if (strcasecmp(id, ids[k]) == 0) {
                memcpy(w->container, names[i], sizeof w->container);
                break;
            }
        }
    }
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

static void win_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)data;
    (void)h;
    dirty_all();
}

static void win_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    struct window *w = data;
    for (struct window **p = &windows; *p; p = &(*p)->next) {
        if (*p == w) {
            *p = w->next;
            break;
        }
    }
    cap_free(&w->cap);
    free(w->thumb.px);
    free(w->scaled.px);
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    free(w);
    for (struct output *o = outputs; o; o = o->next) {
        o->selected = o->hovered = -1;
    }
    dirty_all();
}

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
    if (!w) {
        zwlr_foreign_toplevel_handle_v1_destroy(h);
        return;
    }
    w->handle = h;
    w->cap.done = 1;
    while (*tail) tail = &(*tail)->next;
    *tail = w;
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &win_listener, w);
}

static void toplevels_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *m)
{ (void)data; (void)m; }

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

/* Each window its ext-foreign-toplevel-list twin: the first not yet taken
 * with the same application and title (both lists in the order windows
 * came), else with the same application. */
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

/* Screens and their surfaces */

/* A window is on one screen, as in KWin's overview: the first it entered
 * (KWin: the one with most of it); on none (minimized), on the first. */
static int window_on(const struct window *w, const struct output *o)
{
    return w->noutputs ? w->outputs[0] == o->wl : o == outputs;
}

static int matches(const struct window *w)
{
    return !search[0] || strcasestr(w->title, search) || strcasestr(w->app_id, search) ||
           strcasestr(w->container, search);
}

/* The windows of this screen that the search leaves, in order. */
static int shown_windows(const struct output *o, struct window **list)
{
    int n = 0;
    for (struct window *w = windows; w && n < MAX_SHOWN; w = w->next) {
        if (window_on(w, o) && matches(w)) list[n++] = w;
    }
    return n;
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *l, uint32_t serial, uint32_t w, uint32_t h)
{
    struct output *o = data;
    zwlr_layer_surface_v1_ack_configure(l, serial);
    o->w = w;
    o->h = h;
    o->configured = o->dirty = 1;
    /* Input everywhere but the top left pixel: there the pointer stays on
     * locachy-bar, which shows and closes the overview from that corner. */
    struct wl_region *input = wl_compositor_create_region(compositor);
    wl_region_add(input, 0, 0, (int32_t)w, (int32_t)h);
    wl_region_subtract(input, 0, 0, 1, 1);
    wl_surface_set_input_region(o->surf, input);
    wl_region_destroy(input);
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
    o->selected = o->hovered = -1;
    o->keyboard = keyboard_here;
    if (scale_mgr && viewporter) {
        o->fscale = wp_fractional_scale_manager_v1_get_fractional_scale(scale_mgr, o->surf);
        wp_fractional_scale_v1_add_listener(o->fscale, &scale_listener, o);
        o->viewport = wp_viewporter_get_viewport(viewporter, o->surf);
    }
    o->layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, o->surf, o->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                                                     "locachy-overview");
    zwlr_layer_surface_v1_set_anchor(o->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, keyboard_here ?
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
    wl_surface_commit(o->surf);
}

/* Layout, as KWin's ExpoLayout: the windows in order (KWin keeps them near
 * their places, which a client is not told), split into rows of about equal
 * width; one scale for all, the largest that lets every row fit the width
 * and the rows (with what is under each) the height, at most MAX_SCALE;
 * rows centred, each window centred in its row. In the screen's pixels; a
 * window's size is its picture's, which the compositor makes at its size in
 * the layout (wlroots' scene capture), times the screen's scale sc (a window
 * not pictured counts as 400x300). */
static double natural_w(const struct window *w, double sc) { return (w->thumb.w > 0 ? w->thumb.w : 400) * sc; }
static double natural_h(const struct window *w, double sc) { return (w->thumb.h > 0 ? w->thumb.h : 300) * sc; }

/* Row starts for the list split into at most rows rows; the number made. */
static int split_rows(struct window **list, int n, int rows, double gap, double sc, int *starts)
{
    double total = 0, row_w = 0;
    int made = 1;
    for (int i = 0; i < n; i++) total += natural_w(list[i], sc) + gap;
    starts[0] = 0;
    for (int i = 0; i < n; i++) {
        double ww = natural_w(list[i], sc) + gap;
        if (row_w > 0 && row_w + ww / 2 > total / rows && made < rows) {
            starts[made++] = i;
            row_w = 0;
        }
        row_w += ww;
    }
    starts[made] = n;
    return made;
}

static void layout(struct output *o, int ax, int ay, int aw, int ah, int gap, int under)
{
    double sc = (o->viewport ? o->scale120 : 120) / 120.0;
    struct window *list[MAX_SHOWN];
    int n = shown_windows(o, list), starts[MAX_SHOWN + 1], best_rows = 1;
    double best = 0;
    if (n == 0) return;
    for (int rows = 1; rows <= n; rows++) {
        int made = split_rows(list, n, rows, gap, sc, starts);
        double max_w = 0, sum_h = 0;
        for (int r = 0; r < made; r++) {
            double row_w = 0, row_h = 0;
            for (int i = starts[r]; i < starts[r + 1]; i++) {
                row_w += natural_w(list[i], sc);
                row_h = fmax(row_h, natural_h(list[i], sc));
            }
            max_w = fmax(max_w, row_w / (aw - gap * (starts[r + 1] - starts[r] - 1)));
            sum_h += row_h;
        }
        double scale = fmin(MAX_SCALE, fmin(1 / max_w, (ah - made * under - (made - 1) * gap) / sum_h));
        if (scale > best) {
            best = scale;
            best_rows = rows;
        }
    }
    int made = split_rows(list, n, best_rows, gap, sc, starts);
    double heights = 0;
    for (int r = 0; r < made; r++) {
        double row_h = 0;
        for (int i = starts[r]; i < starts[r + 1]; i++) row_h = fmax(row_h, natural_h(list[i], sc) * best);
        heights += row_h + under;
    }
    double gap_y = fmax(0, fmin(gap, (ah - heights) / (made + 1)));
    double y = ay + (ah - heights - gap_y * (made - 1)) / 2;
    for (int r = 0; r < made; r++) {
        double width = 0, row_h = 0;
        int count = starts[r + 1] - starts[r];
        for (int i = starts[r]; i < starts[r + 1]; i++) {
            width += natural_w(list[i], sc) * best;
            row_h = fmax(row_h, natural_h(list[i], sc) * best);
        }
        double gap_x = fmax(0, fmin(gap, (aw - width) / (count + 1)));
        double x = ax + (aw - width - gap_x * (count - 1)) / 2;
        for (int i = starts[r]; i < starts[r + 1]; i++) {
            struct window *w = list[i];
            double ww = natural_w(w, sc) * best, hh = natural_h(w, sc) * best;
            w->x = (int)x;
            w->y = (int)(y + (row_h - hh) / 2);
            w->w = ww >= 1 ? (int)ww : 1;
            w->h = hh >= 1 ? (int)hh : 1;
            x += ww + gap_x;
        }
        y += row_h + under + gap_y;
    }
}

/* Drawing */

static void buffer_release(void *data, struct wl_buffer *b)
{
    (void)data;
    wl_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void close_button(const struct window *w, int row, int *cx, int *cy, int *r)
{
    *r = row * 2 / 3;
    *cx = w->x + w->w - *r - row / 4;
    *cy = w->y + *r + row / 4;
}

/* A label on a rounded ground, centred on cx, its top at y. */
static void label(uint32_t *px, int W, int H, int cx, int y, int width, const char *s, const struct panel_font *f,
                  int ch, uint32_t fg, uint32_t bg)
{
    char fit[192];
    int pad = ch / 3;
    panel_text_fit(fit, sizeof fit, s, width - 2 * pad, f);
    int tw = panel_text_width(fit, f) + 2 * pad;
    round_rect(px, W, H, cx - tw / 2, y, tw, ch + ch / 4, ch / 3, 0, bg);
    text_over(px, W, H, cx - tw / 2 + pad, y + ch / 8, fit, fg, f, ch);
}

/* An image at (x, y), clipped to the buffer, at alpha (1: as it is). */
static void put_image(uint32_t *px, int W, int H, const struct image *im, int x, int y, double alpha)
{
    uint32_t a = alpha >= 1 ? 255 : alpha <= 0 ? 0 : (uint32_t)(alpha * 255);
    int x0 = x < 0 ? -x : 0, x1 = x + im->w > W ? W - x : im->w;
    for (int r = 0; r < im->h && x1 > x0; r++) {
        if (y + r < 0 || y + r >= H) continue;
        uint32_t *to = px + (size_t)(y + r) * W + x, *from = im->px + (size_t)r * im->w;
        if (a == 255) {
            memcpy(to + x0, from + x0, (size_t)(x1 - x0) * 4);
        } else {
            for (int c = x0; c < x1; c++) to[c] = blend(to[c], a << 24 | (from[c] & 0xFFFFFFu));
        }
    }
}

/* How far a window dragged by touch is on its way down: 0 where pressed, 1
 * at the bottom of the windows' area (KDE's downGestureProgress). */
static double down_progress(void)
{
    struct output *o = touching.o;
    double sc = (o->viewport ? o->scale120 : 120) / 120.0, bottom = o->heap_bottom / sc;
    double from = bottom - touching.y0, now = bottom - touching.y;
    return from <= 0 ? 0 : 1 - fmin(fmax(now / from, 0), 1);
}

static void draw(struct output *o)
{
    if (!o->configured || !o->w || !o->h) return;
    o->dirty = 0;
    uint32_t s120 = o->viewport ? o->scale120 : 120;
    int W = (int)((o->w * s120 + 60) / 120), H = (int)((o->h * s120 + 60) / 120);
    int row = text_px(s120 / 120.0), cw = row / 2, ch = row;
    const struct panel_font *font = panel_font(row, 0), *bold = panel_font(row, 1);
    panel_font_size(font, &cw, &ch);

    /* The veiled background, made once for this size. */
    if (!o->veiled.px || o->veiled.w != W || o->veiled.h != H) {
        free(o->veiled.px);
        o->veiled.px = NULL;
        if (o->bg.px) {
            smooth_image(&o->bg, &o->veiled, W, H);
        } else if ((o->veiled.px = calloc((size_t)W * (size_t)H, 4)) != NULL) {
            o->veiled.w = W;
            o->veiled.h = H;
        }
        for (size_t i = 0; o->veiled.px && i < (size_t)W * (size_t)H; i++) {
            o->veiled.px[i] = blend(o->veiled.px[i] | 0xFF000000u, VEIL);
        }
    }
    size_t size = (size_t)W * (size_t)H * 4;
    int fd = memfd_create("locachy-overview", MFD_CLOEXEC);
    uint32_t *px = MAP_FAILED;
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0 ||
        (px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        if (fd >= 0) close(fd);
        return;
    }
    if (o->veiled.px) memcpy(px, o->veiled.px, size);

    /* The search field, at the top in the middle. */
    int margin = row, field_w = cw * 40 < W - 2 * margin ? cw * 40 : W - 2 * margin, field_h = row * 3 / 2;
    int fx = (W - field_w) / 2, fy = margin, line = s120 >= 180 ? 3 : 2;
    round_rect(px, W, H, fx, fy, field_w, field_h, row / 3, 0, FIELD_BG);
    if (o->keyboard) round_rect(px, W, H, fx, fy, field_w, field_h, row / 3, line / 2 + 1, HIGHLIGHT);
    char text[192];
    panel_text_fit(text, sizeof text, search[0] ? search : _("Search..."), field_w - 2 * cw, font);
    text_over(px, W, H, fx + cw, fy + (field_h - ch) / 2, text, search[0] ? TEXT : DIM_TEXT, font, ch);

    /* The windows below it, each with its label (where KDE puts the icon)
     * over its bottom edge and its title under that. */
    int under = ch * 2 + ch / 2, gap = row;
    int ax = 2 * margin, ay = fy + field_h + 2 * margin, aw = W - 4 * margin, ah = H - ay - 2 * margin;
    layout(o, ax, ay, aw, ah, gap, under);
    o->heap_bottom = ay + ah;
    double sc = s120 / 120.0;
    struct window *list[MAX_SHOWN];
    int n = shown_windows(o, list);
    for (int pass = 0; pass < 2; pass++) { /* the window dragged over the others */
        for (int i = 0; i < n; i++) {
            struct window *w = list[i];
            int dragged = touching.id >= 0 && touching.dragging && touching.o == o && touching.win == i;
            if (dragged != pass) continue;
            int dx = dragged ? (int)((touching.x - touching.x0) * sc) : 0;
            int dy = dragged ? (int)((touching.y - touching.y0) * sc) : 0;
            if (w->thumb.px && fit_image(&w->thumb, &w->scaled, w->w, w->h) == 0) {
                put_image(px, W, H, &w->scaled, w->x + dx, w->y + dy, dragged ? 1 - down_progress() : 1);
            } else {
                round_rect(px, W, H, w->x + dx, w->y + dy, w->w, w->h, row / 4, 0, LABEL_BG);
            }
            if (dragged) {
                label(px, W, H, w->x + dx + w->w / 2, w->y + dy + (w->h - ch) / 2, W, _("Drag Down To Close"), font, ch,
                      TEXT, LABEL_BG); /* KDE's: as wide as its text */
                continue;
            }
            if (i == o->hovered || i == o->selected) {
                round_rect(px, W, H, w->x - 2 * line, w->y - 2 * line, w->w + 4 * line, w->h + 4 * line, row / 3,
                           line, HIGHLIGHT);
            }
            if (i == o->hovered || touched) {
                int cx, cy, r;
                close_button(w, row, &cx, &cy, &r);
                round_rect(px, W, H, cx - r, cy - r, 2 * r, 2 * r, r, 0,
                           i == o->hovered && o->hover_close ? CLOSE_RED : LABEL_BG);
                for (int t = -r / 2; t <= r / 2; t++) { /* its cross */
                    for (int k = -line / 2; k <= line / 2; k++) {
                        int ya = cy + t + k, yb = cy - t + k, x = cx + t;
                        if (x >= 0 && x < W && ya >= 0 && ya < H) px[ya * W + x] = TEXT;
                        if (x >= 0 && x < W && yb >= 0 && yb < H) px[yb * W + x] = TEXT;
                    }
                }
            }
            const char *name = w->container[0] ? w->container : w->app_id[0] ? w->app_id : "?";
            int ly = w->y + w->h - ch / 2;
            label(px, W, H, w->x + w->w / 2, ly, w->w + gap, name, bold, ch, 0xFF000000u, HIGHLIGHT);
            label(px, W, H, w->x + w->w / 2, ly + ch + ch / 2, w->w + gap, w->title[0] ? w->title : name, font, ch,
                  TEXT, LABEL_BG);
        }
    }
    if (n == 0 && search[0]) {
        const char *none = _("No matching windows");
        text_over(px, W, H, (W - panel_text_width(none, font)) / 2, ay + ah / 2 - ch / 2, none, DIM_TEXT, font, ch);
    }
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

/* Input */

static int window_at(struct output *o, double lx, double ly, int *on_close)
{
    struct window *list[MAX_SHOWN];
    uint32_t s120 = o->viewport ? o->scale120 : 120;
    double x = lx * s120 / 120.0, y = ly * s120 / 120.0;
    int n = shown_windows(o, list), row = text_px(s120 / 120.0);
    int under = row * 2 + row / 2;
    *on_close = 0;
    for (int i = 0; i < n; i++) {
        struct window *w = list[i];
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h + under) {
            int cx, cy, r;
            close_button(w, row, &cx, &cy, &r);
            *on_close = y < w->y + w->h && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= (double)r * r;
            return i;
        }
    }
    return -1;
}

static void activate(struct window *w)
{
    /* Unmap the exclusive overlay surfaces first so labwc clears
     * seat->focused_layer before handling request_activate. */
    for (struct output *o = outputs; o; o = o->next) {
        if (o->layer) zwlr_layer_surface_v1_destroy(o->layer);
        if (o->surf) wl_surface_destroy(o->surf);
        o->layer = NULL;
        o->surf = NULL;
    }
    /* Only the compositor: this client has no channel to process 1 (as the
     * bar's window list, which activates the same way). */
    if (w->minimized) zwlr_foreign_toplevel_handle_v1_unset_minimized(w->handle);
    zwlr_foreign_toplevel_handle_v1_activate(w->handle, seat);
    finished = 1;
}

static struct window *nth_shown(struct output *o, int i)
{
    struct window *list[MAX_SHOWN];
    int n = shown_windows(o, list);
    return i >= 0 && i < n ? list[i] : NULL;
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x,
                          wl_fixed_t y)
{
    (void)data;
    pointer_output = NULL;
    for (struct output *o = outputs; o; o = o->next) {
        if (o->surf == s) pointer_output = o;
    }
    pointer_x = wl_fixed_to_double(x);
    pointer_y = wl_fixed_to_double(y);
    struct wl_cursor *cursor = cursor_theme ? wl_cursor_theme_get_cursor(cursor_theme, "left_ptr") : NULL;
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
    (void)data;
    (void)p;
    (void)serial;
    (void)s;
    if (pointer_output && pointer_output->hovered >= 0) {
        pointer_output->hovered = -1;
        pointer_output->dirty = 1;
    }
    pointer_output = NULL;
}

/* The keys to this screen's search and windows: KWin's active screen is
 * the one last interacted with, the pointer moved or a window activated on
 * it (the overview starts on the activated window's). */
static void keyboard_to(struct output *o)
{
    if (o->keyboard) return;
    for (struct output *e = outputs; e; e = e->next) {
        e->keyboard = e == o;
        zwlr_layer_surface_v1_set_keyboard_interactivity(e->layer, e == o ?
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        e->dirty = 1; /* committed when drawn */
    }
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    (void)p;
    (void)time;
    pointer_x = wl_fixed_to_double(x);
    pointer_y = wl_fixed_to_double(y);
    struct output *o = pointer_output;
    if (!o) return;
    keyboard_to(o);
    if (touched) { /* KDE: the close buttons only under the pointer again */
        touched = 0;
        dirty_all();
    }
    int on_close, i = window_at(o, pointer_x, pointer_y, &on_close);
    if (i != o->hovered || on_close != o->hover_close) {
        o->hovered = i;
        o->hover_close = on_close;
        if (i >= 0) o->selected = i; /* KDE: hovering selects */
        o->dirty = 1;
    }
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    static int press_handled;
    (void)data;
    (void)p;
    (void)serial;
    (void)time;
    struct output *o = pointer_output ? pointer_output : (outputs && !outputs->next ? outputs : NULL);
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (!o) {
            press_handled = 0;
            return;
        }
        press_handled = 1;
    } else {
        if (press_handled) {
            press_handled = 0;
            return;
        }
        if (!o) return;
    }
    keyboard_to(o);
    int on_close, i = window_at(o, pointer_x, pointer_y, &on_close);
    struct window *w = nth_shown(o, i);
    if (!w) {
        finished = button == BTN_LEFT; /* a click on nothing */
    } else if (button == BTN_MIDDLE || (button == BTN_LEFT && on_close)) {
        zwlr_foreign_toplevel_handle_v1_close(w->handle);
    } else if (button == BTN_LEFT) {
        activate(w);
    }
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{ (void)data; (void)p; (void)time; (void)axis; (void)value; }

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion, .button = pointer_button,
    .axis = pointer_axis,
};

static void touch_down(void *data, struct wl_touch *t, uint32_t serial, uint32_t time, struct wl_surface *s,
                       int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    (void)t;
    (void)serial;
    (void)time;
    struct output *o = NULL;
    for (struct output *e = outputs; e; e = e->next) {
        if (e->surf == s) o = e;
    }
    if (touching.id >= 0 || !o) return; /* one finger at a time */
    keyboard_to(o);
    touched = 1;
    touching.id = id;
    touching.o = o;
    touching.x0 = touching.x = wl_fixed_to_double(x);
    touching.y0 = touching.y = wl_fixed_to_double(y);
    touching.win = window_at(o, touching.x, touching.y, &touching.on_close);
    touching.dragging = 0;
    o->hovered = -1;
    dirty_all(); /* the close buttons */
}

static void touch_motion(void *data, struct wl_touch *t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    (void)t;
    (void)time;
    if (id != touching.id) return;
    touching.x = wl_fixed_to_double(x);
    touching.y = wl_fixed_to_double(y);
    if (!touching.dragging && touching.win >= 0 &&
        hypot(touching.x - touching.x0, touching.y - touching.y0) > DRAG_START) {
        touching.dragging = 1;
    }
    if (touching.dragging) touching.o->dirty = 1;
}

static void touch_up(void *data, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id)
{
    (void)data;
    (void)t;
    (void)serial;
    (void)time;
    if (id != touching.id) return;
    struct window *w = nth_shown(touching.o, touching.win);
    if (touching.dragging) {
        if (w && down_progress() > 0.6) zwlr_foreign_toplevel_handle_v1_close(w->handle);
    } else if (!w) {
        finished = 1; /* a tap on nothing */
    } else if (touching.on_close) {
        zwlr_foreign_toplevel_handle_v1_close(w->handle);
    } else {
        activate(w);
    }
    touching.o->dirty = 1; /* the window dragged back in its place */
    touching.id = -1;
    touching.dragging = 0;
}

static void touch_frame(void *data, struct wl_touch *t) { (void)data; (void)t; }

static void touch_cancel(void *data, struct wl_touch *t)
{
    (void)data;
    (void)t;
    touching.id = -1;
    touching.dragging = 0;
    dirty_all();
}

static const struct wl_touch_listener touch_listener = {
    .down = touch_down, .up = touch_up, .motion = touch_motion, .frame = touch_frame, .cancel = touch_cancel,
};

#ifdef OVERVIEW_TEST
/* Tests only (no virtual touch screen in wlroots): touch from standard
 * input, a line each, logical pixels: "down WxH X Y" (on the screen W by H
 * logical pixels large), "motion X Y", "up". 0 at its end. */
static int test_touch(void)
{
    static char buf[512];
    static size_t len;
    ssize_t got = read(0, buf + len, sizeof buf - 1 - len);
    if (got <= 0) return 0;
    len += (size_t)got;
    buf[len] = '\0';
    char *line = buf, *end;
    while ((end = strchr(line, '\n')) != NULL) {
        unsigned w, h;
        double x, y;
        *end = '\0';
        if (sscanf(line, "down %ux%u %lf %lf", &w, &h, &x, &y) == 4) {
            struct output *o = outputs;
            while (o && (o->w != w || o->h != h)) o = o->next;
            if (o) touch_down(NULL, NULL, 0, 0, o->surf, 1, wl_fixed_from_double(x), wl_fixed_from_double(y));
        } else if (sscanf(line, "motion %lf %lf", &x, &y) == 2) {
            touch_motion(NULL, NULL, 0, 1, wl_fixed_from_double(x), wl_fixed_from_double(y));
        } else if (strcmp(line, "up") == 0) {
            touch_up(NULL, NULL, 0, 0, 1);
        }
        line = end + 1;
    }
    len = strlen(line);
    memmove(buf, line, len + 1);
    return 1;
}
#endif

/* The window that way from the selected one ((dx, dy) the direction): the
 * nearest whose centre lies that way, nearness along it counting less than
 * across it. None selected: the first. */
static int select_toward(struct output *o, int dx, int dy)
{
    struct window *list[MAX_SHOWN];
    int n = shown_windows(o, list), pick;
    if (n == 0) return -1;
    if (o->selected < 0 || o->selected >= n) return 0;
    struct window *from = list[o->selected];
    double fx = from->x + from->w / 2.0, fy = from->y + from->h / 2.0, best = 1e18;
    pick = o->selected;
    for (int i = 0; i < n; i++) {
        double x = list[i]->x + list[i]->w / 2.0 - fx, y = list[i]->y + list[i]->h / 2.0 - fy;
        double along = x * dx + y * dy, across = fabs(x * dy) + fabs(y * dx);
        if (i != o->selected && along > 0 && along + 2 * across < best) {
            best = along + 2 * across;
            pick = i;
        }
    }
    return pick;
}

static void key_pressed(struct output *o, xkb_keysym_t sym, const char *utf8)
{
    struct window *list[MAX_SHOWN];
    int n = shown_windows(o, list);
    switch (sym) {
    case XKB_KEY_Escape:
        if (search[0]) search[0] = '\0';
        else finished = 1;
        break;
    case XKB_KEY_space:
        if (search[0]) goto text; /* a space in the search */
        /* KDE: Space activates while the windows, not the search, have the keys */
        if (o->selected >= 0 && o->selected < n) activate(list[o->selected]);
        else if (n == 1) activate(list[0]);
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (o->selected >= 0 && o->selected < n) activate(list[o->selected]);
        else if (n == 1) activate(list[0]);
        break;
    case XKB_KEY_Left: o->selected = select_toward(o, -1, 0); break;
    case XKB_KEY_Right: o->selected = select_toward(o, 1, 0); break;
    case XKB_KEY_Up: o->selected = select_toward(o, 0, -1); break;
    case XKB_KEY_Down: o->selected = select_toward(o, 0, 1); break;
    case XKB_KEY_Home: o->selected = n ? 0 : -1; break;
    case XKB_KEY_End: o->selected = n - 1; break;
    case XKB_KEY_BackSpace:
        if (search[0]) {
            size_t len = strlen(search);
            while (len > 1 && (search[len - 1] & 0xC0) == 0x80) len--; /* a whole character */
            search[len - 1] = '\0';
            o->selected = -1;
        }
        break;
    default:
    text:
        if ((unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7F && strlen(search) + strlen(utf8) < sizeof search) {
            strcat(search, utf8);
            o->selected = -1;
        }
    }
    dirty_all();
}

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data;
    (void)k;
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
    (void)data;
    (void)k;
    (void)serial;
    (void)time;
    struct output *o = outputs;
    for (struct output *e = outputs; e; e = e->next) {
        if (e->keyboard) o = e;
    }
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !xkb_st || !o) return;
    char utf8[16];
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_st, key + 8);
    xkb_state_key_get_utf8(xkb_st, key + 8, utf8, sizeof utf8);
    if (xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 ||
        xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0 ||
        xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) {
        utf8[0] = '\0'; /* a shortcut, not text */
    }
    key_pressed(o, sym, utf8);
}

static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat,
                         uint32_t lock, uint32_t group)
{
    (void)data;
    (void)k;
    (void)serial;
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
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !touch) {
        touch = wl_seat_get_touch(s);
        wl_touch_add_listener(touch, &touch_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && touch) {
        wl_touch_destroy(touch);
        touch = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *s, const char *name) { (void)data; (void)s; (void)name; }

static const struct wl_seat_listener seat_listener = { seat_capabilities, seat_name };

/* Setup */

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
    (void)data;
#define IS(i) (strcmp(iface, i##_interface.name) == 0)
    if (IS(wl_compositor)) {
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (IS(wl_shm)) {
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (IS(wl_seat) && !seat) {
        seat = wl_registry_bind(r, name, &wl_seat_interface, version < 4 ? version : 4); /* no pointer frames */
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (IS(wl_output)) {
        struct output *o = calloc(1, sizeof *o), **tail = &outputs;
        if (!o) return;
        o->name = name;
        o->wl = wl_registry_bind(r, name, &wl_output_interface, 1);
        while (*tail) tail = &(*tail)->next;
        *tail = o;
    } else if (IS(zwlr_layer_shell_v1)) {
        layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (IS(zwlr_foreign_toplevel_manager_v1)) {
        toplevels = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, version < 3 ? version : 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(toplevels, &toplevels_listener, NULL);
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
    (void)data;
    (void)r;
    for (struct output *o = outputs; o; o = o->next) {
        if (o->name == name) finished = 1; /* a screen went: shown again when asked again */
    }
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static void on_stop(int sig)
{
    (void)sig;
    stopping = 1;
}

/* One running already: it is ended (not with --show), and this ends too
 * (run again, it closes). None: this one runs (not with --hide), its pid
 * written down. --show and --hide are KWin's gestures' ways: swiped up, it
 * shows; down, it closes. */
static int toggle(const char *how)
{
    int show = how && strcmp(how, "--show") == 0, hide = how && strcmp(how, "--hide") == 0;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char comm[64] = "", path[64];
    int pid = 0;
    snprintf(pid_path, sizeof pid_path, "%s/locachy-overview.pid", runtime && runtime[0] ? runtime : "/tmp");
    FILE *f = fopen(pid_path, "r");
    if (f) {
        if (fscanf(f, "%d", &pid) != 1) pid = 0;
        fclose(f);
    }
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    f = pid > 0 && pid != getpid() ? fopen(path, "r") : NULL;
    if (f) {
        if (!fgets(comm, sizeof comm, f)) comm[0] = '\0';
        fclose(f);
    }
    if (strncmp(comm, "locachy-overvie", 15) == 0) {
        if (!show) kill(pid, SIGTERM);
        return 1;
    }
    if (hide) return 1;
    f = fopen(pid_path, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (toggle(argc > 1 ? argv[1] : NULL)) return 0;
    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    display = wl_display_connect(NULL);
    if (!display) {
        say("locachy-overview: no compositor");
        unlink(pid_path);
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); /* the windows: titles, states, screens */
    if (!compositor || !shm || !layer_shell || !toplevels || !outputs) {
        say("locachy-overview: the compositor lacks the layer shell or window management");
        unlink(pid_path);
        return 1;
    }
    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    cursor_theme = wl_cursor_theme_load(NULL, 24, shm);
    cursor_surface = wl_compositor_create_surface(compositor);
    pair_windows();
    for (struct window *w = windows; w && copier; w = w->next) {
        capture(&w->cap, w->listed && window_sources ?
                ext_foreign_toplevel_image_capture_source_manager_v1_create_source(window_sources, w->listed->handle) :
                NULL, &w->thumb, w);
    }
    /* The screens' pictures, before the overview covers them. */
    struct capture bg_caps[16];
    int nbg = 0;
    for (struct output *o = outputs; o && nbg < 16; o = o->next) {
        capture(&bg_caps[nbg++], copier && output_sources ?
                ext_output_image_capture_source_manager_v1_create_source(output_sources, o->wl) : NULL, &o->bg, NULL);
    }
    for (int tries = 0; tries < 50; tries++) { /* at most half a second for them and the windows */
        int pending = 0;
        for (int i = 0; i < nbg; i++) pending |= !bg_caps[i].done;
        for (struct window *w = windows; w; w = w->next) pending |= !w->cap.done;
        if (!pending) break;
        wl_display_roundtrip(display);
        nanosleep(&(struct timespec){ 0, 10000000 }, NULL);
    }
    for (int i = 0; i < nbg; i++) cap_free(&bg_caps[i]); /* one too late would picture the overview */
    for (struct output *o = outputs; o; o = o->next) {
        if (o->bg.px) blur(&o->bg);
    }
    struct wl_output *focus = NULL;
    for (struct window *w = windows; w; w = w->next) {
        if (w->activated && w->noutputs) focus = w->outputs[0];
    }
    for (struct output *o = outputs; o; o = o->next) {
        create_surface(o, focus ? o->wl == focus : o == outputs); /* the keys go to the focused screen's */
    }
    while (!finished && !stopping) {
        wl_display_dispatch_pending(display);
        if (finished || stopping) break;
        for (struct output *o = outputs; o; o = o->next) {
            if (o->dirty && o->surf) draw(o);
        }
        wl_display_flush(display);
        struct pollfd p[2] = { { .fd = wl_display_get_fd(display), .events = POLLIN }, { .fd = -1 } };
#ifdef OVERVIEW_TEST
        static int test_input = 1;
        p[1].fd = test_input ? 0 : -1;
        p[1].events = POLLIN;
#endif
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
#ifdef OVERVIEW_TEST
        if (p[1].revents & (POLLIN | POLLHUP)) test_input = test_touch();
#endif
        if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) && wl_display_dispatch(display) < 0) break;
    }
    wl_display_roundtrip(display);
    unlink(pid_path);
    wl_display_disconnect(display);
    return 0;
}
