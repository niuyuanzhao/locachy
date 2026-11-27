/*
 * The screen. Everything is drawn into a shadow buffer in ordinary memory;
 * screen_flush mirrors what changed into a dumb buffer for each connected
 * output on one DRM card and tells the driver (DIRTYFB). The first selected
 * output sets the logical size on each take; changing output or mode resizes
 * the picture for the program to redraw. Other sizes fit it with black
 * borders. Programs may opt into extended mode: each connector then keeps an
 * independent native canvas, text scale and desktop rectangle. A program keeps
 * its pictures while it does not have the screen.
 *
 * Process 1 opens a card with a connected output itself (it is the
 * DRM master) and lends connectors and CRTCs in DRM leases (which bring
 * their primary planes along): all of them in one, or, with several
 * screens, each program the screens it shows, the leases side by side. A
 * program started by process 1 gets its lease through the existing control
 * channel, and the desktop's layout (every screen's place, its own or not)
 * with it; the screen the pointer is on, among several, has a green frame. Text is drawn pixel for
 * pixel in the logical picture with a PSF1 or PSF2 console font: Terminus,
 * TEXT_H pixels high times the first output's scale (MONITORS_CONF, as on the
 * shared display), so the cells, not the picture, grow with the scale.
 */
#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#ifdef CONFIG_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

#include "fontmatch.h"
#include "internal.h"
#include "monitor-layout.h"
#include "include/monitor-mode.h"
#include "include/monitor-profile.h"
#include "include/wayland-backend.h"

/* Optional in standalone screen unit tests that do not link status.c or rc.c. */
extern void base_status_fields(struct status_fields *out) __attribute__((weak));
extern int rc_read(void (*fn)(const char *name, const char *value, void *arg), void *arg) __attribute__((weak));

#define PSF1_MAGIC 0x0436u     /* bytes 36 04 */
#define PSF2_MAGIC 0x864AB572u /* bytes 72 b5 4a 86 */

struct output_surface;
static uint32_t *shadow;             /* the current drawing target */
static uint32_t width, height;
static uint32_t *presented;          /* last flushed snapshot of shadow */
static uint32_t presented_w, presented_h;
static int presented_valid;
static int extended, selected_output = -1;
static struct output_surface *surfaces, *drawing_surface;
static int card = -1;                /* the DRM fd the shadow is shown on */
struct output {
    char name[32];                   /* as the kernel names it: DP-3, eDP-1, HDMI-A-1 */
    uint32_t conn_id, crtc_id;
    uint32_t possible_crtcs, preferred_crtc;
    int internal;
    int x, y, logical_width, logical_height;
    struct output_surface *surface;
    struct drm_mode_modeinfo mode;
    struct drm_mode_modeinfo current_mode; /* advertised active timing before this take */
    struct drm_mode_modeinfo *modes;
    uint32_t nmodes;
    uint8_t *map;
    uint64_t map_size;
    uint32_t pitch, fb_id, handle;
    /* Optional second dumb framebuffer allocated on demand for tear-free
     * double-buffered CPU presentation (screen_present_shm_flip). When active,
     * map/fb_id/handle is always the currently scanned-out front buffer and
     * back_* is the off-screen back buffer; they swap on DRM_EVENT_FLIP_COMPLETE. */
    uint8_t *back_map;
    uint64_t back_map_size;
    uint32_t back_pitch, back_fb_id, back_handle;
    int shm_flip_pending;            /* 1 while a PAGE_FLIP to back_fb_id is in flight */
    int shm_flip_broken;             /* 1 if driver refused PAGE_FLIP between dumb FBs */
    uint32_t dmabuf_fb_id;           /* zero-copy DRM PRIME scanout FB, currently on screen */
    uint32_t dmabuf_pending_fb_id;   /* async PAGE_FLIP target FB, waiting for FLIP_COMPLETE */
    uint32_t dmabuf_cache_fb[SCREEN_DMABUF_SLOTS]; /* cached DRM FBs indexed by worker VPP slot */
    uint32_t dmabuf_cache_gen;       /* generation matching dmabuf_cache_fb[] */
    int dmabuf_active_slot;          /* currently scanning cached slot, or -1 */
    int dmabuf_pending_slot;         /* in-flight cached slot, or -1 */
    int dmabuf_flip_pending;         /* 1 while a DRM_IOCTL_MODE_PAGE_FLIP is in flight */
    int dmabuf_flip_broken;          /* 1 if driver refused PAGE_FLIP; fall back to SETCRTC */
    int soft_cursor;                 /* no cursor plane: drawn into the scanout */
    struct drm_clip_rect cursor_at;  /* where it was drawn there; empty if x1 == x2 */
    uint32_t lessee;                 /* process 1: the lease it is lent in; 0 if none */
    int idle_off;                    /* process 1 turned this connector off */
};
static struct output *outputs;
static uint32_t noutputs;
static int lent;                     /* suppress scanout while a frontend owns DRM */
static uint32_t active_lessee;       /* a revocable lease, distinct from raw master handoff */
/* Programs: the desktop's screens as process 1 gives them ("screens"), by
 * connector name, in logical pixels; the one the pointer is on (hover_output,
 * an index of outputs; -1: none), framed when there are two or more. */
#define MAX_DESK 8
static struct desk { char name[32]; int x, y, w, h; } desk[MAX_DESK];
static int ndesk, hover_output = -1;
static struct drm_clip_rect dirty;   /* drawn since the last flush; empty if x1 == x2 */

static uint8_t *font;
static const uint8_t *glyphs;
static uint32_t glyph_count, glyph_w, glyph_h, glyph_bytes, row_bytes;
static uint32_t glyph_mag = 1; /* each font pixel drawn as mag x mag (glyph_w, glyph_h: drawn) */
static struct { uint32_t cp, g; } utab[1024];
static int nutab;
static uint16_t ascii_map[128];
static uint8_t ascii_has[128];
static int cols, rows;
static uint32_t top;                 /* pixels above row 0: bar_h - glyph_h, row 0 within the bar */
static uint32_t bar_h = PANEL_H;     /* the top bar: one and a half text rows */
static double scale;                 /* the first output's; 0 until it is known */
static char font_fallback[256];      /* when no Terminus of the size can be read */

struct bar_cache {
    int valid;
    char title[128];
    char ime[64];
    struct status_fields fields;
    int frozen;
    uint32_t bar_w, bar_h;
    uint32_t theme_bg, theme_fg, theme_accent;
};
static struct bar_cache active_bar;

/* A connector keeps its canvas while its lease is released or it disconnects. */
struct output_surface {
    char name[32];
    uint32_t *pixels, width, height;
    uint32_t *presented;
    uint32_t presented_w, presented_h;
    int presented_valid;
    double scale;
    int redraw;
    struct drm_clip_rect dirty;
    uint8_t *font;
    const uint8_t *glyphs;
    uint32_t glyph_count, glyph_w, glyph_h, glyph_bytes, row_bytes, glyph_mag;
    struct { uint32_t cp, g; } utab[1024];
    int nutab, cols, rows;
    uint16_t ascii_map[128];
    uint8_t ascii_has[128];
    uint32_t top, bar_h;
    struct bar_cache bar;
    struct output_surface *next;
};
static struct output_surface mirrored;

/* The kernel's names of connector types (drm_connector_enum_list). */
static const char *const connector_types[] = {
    "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS", "Component",
    "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB"
};
#define NCONNECTOR_TYPES (sizeof connector_types / sizeof connector_types[0])

static void touch(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
static int cell_index(int64_t px, uint32_t step);
static void hover_frame(struct output *o);
static double screen_time(void);
static void cursor_put(int fd);
static void cursor_drop(int fd);
static void soft_cursor_flush(struct output *o, int draw);

#ifdef CONFIG_FREETYPE
/* What Terminus lacks (box drawing, symbols, CJK): fontconfig's monospace
 * and its fallbacks (fontmatch.c), as fontconfig's configuration has them. */
static FT_Library ft_lib;
static struct font_set *ft_fonts;
static int ft_ready;
static int ft_px; /* their size: the text's height, less a pixel above and below */

static void ft_size(void)
{
    ft_px = glyph_h > 2 ? (int)glyph_h - 2 : (int)glyph_h;
}

static void ft_init(void)
{
    if (ft_ready || FT_Init_FreeType(&ft_lib) != 0) {
        return;
    }
    ft_fonts = font_set_new(ft_lib, "monospace", 0, 0);
    ft_ready = ft_fonts != NULL;
    ft_size();
}

static int is_wide_cjk(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11FF) ||
           (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE10 && cp <= 0xFE4F) ||
           (cp >= 0xFF01 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x1F300 && cp <= 0x1FAFF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F);
}

static void draw_ft_glyph(int col, int row, uint32_t cp, int wide, uint32_t fg, uint32_t bg)
{
    int cell_w = wide ? (int)glyph_w * 2 : (int)glyph_w;
    int bx0 = col * (int)glyph_w;
    int by0 = (int)top + row * (int)glyph_h;
    const struct font_glyph *g = ft_ready && ft_px > 0 ? font_set_glyph(ft_fonts, cp, ft_px) : NULL;

    screen_fill_px(bx0, by0, cell_w, (int)glyph_h, bg);
    if (g == NULL || g->alpha == NULL || g->width == 0 || g->rows == 0) {
        return;
    }

    int x_off = bx0 + (wide ? (cell_w - (int)g->width) / 2 : (int)g->left);
    int baseline = by0 + (int)glyph_h - (int)glyph_h / 5;
    int y_off = baseline - (int)g->top;
    if (x_off < bx0) {
        x_off = bx0;
    }

    int gy0 = y_off < 0 ? -y_off : 0;
    int gy1 = y_off + (int)g->rows > (int)height ? (int)height - y_off : (int)g->rows;
    int gx0 = x_off < 0 ? -x_off : 0;
    int gx1 = x_off + (int)g->width > (int)width ? (int)width - x_off : (int)g->width;
    if (gy0 >= gy1 || gx0 >= gx1) {
        return;
    }

    uint32_t fg_rgb = fg & 0xFFFFFFu;
    uint32_t fg_r = (fg_rgb >> 16) & 0xFFu, fg_g = (fg_rgb >> 8) & 0xFFu, fg_b = fg_rgb & 0xFFu;
    uint32_t bg_r = (bg >> 16) & 0xFFu, bg_g = (bg >> 8) & 0xFFu, bg_b = bg & 0xFFu;

    for (int y = gy0; y < gy1; y++) {
        const uint8_t *arow = g->alpha + (size_t)y * g->width;
        uint32_t *drow = shadow + (uint64_t)(y_off + y) * width + (uint32_t)x_off;
        for (int x = gx0; x < gx1; x++) {
            uint32_t a = arow[x];
            if (a == 0) {
                continue;
            }
            if (a == 255) {
                drow[x] = fg_rgb;
            } else {
                uint32_t inv = 255u - a;
                uint32_t vr = fg_r * a + bg_r * inv + 128u;
                uint32_t vg = fg_g * a + bg_g * inv + 128u;
                uint32_t vb = fg_b * a + bg_b * inv + 128u;
                uint32_t r = (vr + (vr >> 8)) >> 8;
                uint32_t gg = (vg + (vg >> 8)) >> 8;
                uint32_t b = (vb + (vb >> 8)) >> 8;
                drow[x] = (r << 16) | (gg << 8) | b;
            }
        }
    }
    if (y_off + gy0 < (int)bar_h) {
        active_bar.valid = 0;
    }
    if (x_off + gx0 < bx0 || x_off + gx1 > bx0 + cell_w ||
        y_off + gy0 < by0 || y_off + gy1 > by0 + (int)glyph_h) {
        touch((uint32_t)(x_off + gx0), (uint32_t)(y_off + gy0),
              (uint32_t)(x_off + gx1), (uint32_t)(y_off + gy1));
    }
}
#endif

static uint32_t utf8(const uint8_t **p, const uint8_t *end)
{
    uint8_t b = *(*p)++;

    if (b < 0x80) {
        return b;
    }
    int n = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
    if (n == 0 || *p + n > end) {
        return '?';
    }
    uint32_t cp = b & (0x3Fu >> n);
    for (int i = 0; i < n; i++) {
        if (((*p)[i] & 0xC0) != 0x80) {
            return '?';
        }
        cp = (cp << 6) | ((*p)[i] & 0x3F);
    }
    *p += n;
    return cp;
}

/* The font in path; the current one stays if it cannot be used. */
static const char *load_font(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    off_t size = fd < 0 ? -1 : lseek(fd, 0, SEEK_END);
    uint8_t *buf = NULL;
    uint32_t h[8], w, gh, count, bytes, rb;
    const uint8_t *g;

    if (size < 4 || (buf = malloc((size_t)size)) == NULL ||
        pread(fd, buf, (size_t)size, 0) != size) {
        if (fd >= 0) {
            close(fd);
        }
        free(buf);
        return "cannot read the font";
    }
    close(fd);
    int psf2 = (size_t)size >= sizeof h && (memcpy(h, buf, sizeof h), h[0] == PSF2_MAGIC);
    if ((buf[0] | buf[1] << 8) == PSF1_MAGIC) {
        w = 8;
        gh = buf[3];
        count = (buf[2] & 1) ? 512 : 256;
        rb = 1;
        bytes = gh;
        g = buf + 4;
    } else if (psf2) {
        /* magic, version, header size, flags, glyph count, bytes per glyph, height, width */
        g = buf + h[2];
        count = h[4];
        bytes = h[5];
        gh = h[6];
        w = h[7];
        rb = (w + 7) / 8;
    } else {
        free(buf);
        return "the font is not a PSF font";
    }
    if (w == 0 || w > 32 || gh == 0 || count < 128 || bytes < rb * gh ||
        (psf2 && h[2] > (uint64_t)size) ||
        (uint64_t)(g - buf) + (uint64_t)count * bytes > (uint64_t)size) {
        free(buf);
        return "the font is truncated or has odd glyph sizes";
    }
    free(font);
    font = buf;
    glyphs = g;
    glyph_w = w;
    glyph_h = gh;
    glyph_mag = 1;
    glyph_count = count;
    glyph_bytes = bytes;
    row_bytes = rb;
    nutab = 0;
    memset(ascii_has, 0, sizeof ascii_has);
    if (psf2 && (h[3] & 1)) {
        const uint8_t *u = glyphs + (uint64_t)glyph_count * glyph_bytes, *end = font + size;
        for (uint32_t i = 0; i < glyph_count && u < end; i++) {
            while (u < end && *u != 0xFF) {
                if (*u == 0xFE) {
                    while (u < end && *u != 0xFF) {
                        u++;
                    }
                    break;
                }
                uint32_t cp = utf8(&u, end);
                if (nutab < (int)(sizeof utab / sizeof utab[0])) {
                    utab[nutab].cp = cp;
                    utab[nutab++].g = i;
                    if (cp < 128 && !ascii_has[cp] && i <= UINT16_MAX) {
                        ascii_map[cp] = (uint16_t)i;
                        ascii_has[cp] = 1;
                    }
                }
            }
            if (u < end) {
                u++;
            }
        }
    }
    if (nutab == 0) {
        for (uint32_t c = 0; c < 128 && c < glyph_count; c++) {
            ascii_map[c] = (uint16_t)c;
            ascii_has[c] = 1;
        }
    }
    return NULL;
}

int text_px(double scale)
{
    static const int sizes[] = { 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 56, 64 };
    int want = (int)(TEXT_H * scale + 0.5), best = sizes[0];
    for (size_t i = 1; i < sizeof sizes / sizeof sizes[0]; i++) {
        if (abs(sizes[i] - want) <= abs(best - want)) {
            best = sizes[i];
        }
    }
    return best;
}

/* Terminus text_px(s) high (above 32, one of half the size drawn twice as
 * large); else the fallback font (the initramfs has only that). */
static const char *scaled_font(double s)
{
    int best = text_px(s);
    uint32_t mag = best > 32 ? 2 : 1;
    char path[160];
    snprintf(path, sizeof path, BASE_FONT_DIR "/ter-u%02db.psf", best / (int)mag);
    if (load_font(path) != NULL) {
        /* bold, or normal where there is none (Terminus at 12) */
        snprintf(path, sizeof path, BASE_FONT_DIR "/ter-u%02dn.psf", best / (int)mag);
        if (load_font(path) != NULL) {
            return load_font(font_fallback);
        }
    }
    glyph_mag = mag;
    glyph_w *= mag;
    glyph_h *= mag;
    return NULL;
}

/* The first output's scale: its MONITORS_CONF line, else 1. */
static double output_scale(void)
{
    struct monitor_conf c;
    return noutputs > 0 && monitor_conf(outputs[0].name, &c) && c.scale > 0 ? c.scale : 1.0;
}

/* The top bar: one and a half text rows of the current font. */
static uint32_t bar_height(void)
{
    return glyph_h * 3 / 2;
}

static void layout(void)
{
    bar_h = bar_height();
    top = bar_h > glyph_h ? bar_h - glyph_h : 0;
    cols = (int)(width / glyph_w);
    rows = height > top ? (int)((height - top) / glyph_h) : 0;
    active_bar.valid = 0;
#ifdef CONFIG_FREETYPE
    ft_size();
#endif
}

static void drop_presented(uint32_t **buf, uint32_t *bw, uint32_t *bh, int *valid)
{
    free(*buf);
    *buf = NULL;
    *bw = 0;
    *bh = 0;
    *valid = 0;
}

static void save_surface(struct output_surface *s)
{
    s->pixels = shadow;
    s->width = width; s->height = height; s->scale = scale; s->dirty = dirty;
    s->presented = presented; s->presented_w = presented_w; s->presented_h = presented_h;
    s->presented_valid = presented_valid;
    s->font = font; s->glyphs = glyphs;
    s->glyph_count = glyph_count; s->glyph_w = glyph_w; s->glyph_h = glyph_h;
    s->glyph_bytes = glyph_bytes; s->row_bytes = row_bytes; s->glyph_mag = glyph_mag;
    memcpy(s->utab, utab, sizeof utab);
    memcpy(s->ascii_map, ascii_map, sizeof ascii_map);
    memcpy(s->ascii_has, ascii_has, sizeof ascii_has);
    s->nutab = nutab; s->cols = cols; s->rows = rows; s->top = top; s->bar_h = bar_h;
    s->bar = active_bar;
}

static void use_surface(struct output_surface *s)
{
    shadow = s->pixels;
    width = s->width; height = s->height; scale = s->scale; dirty = s->dirty;
    presented = s->presented; presented_w = s->presented_w; presented_h = s->presented_h;
    presented_valid = s->presented_valid;
    font = s->font; glyphs = s->glyphs;
    glyph_count = s->glyph_count; glyph_w = s->glyph_w; glyph_h = s->glyph_h;
    glyph_bytes = s->glyph_bytes; row_bytes = s->row_bytes; glyph_mag = s->glyph_mag;
    memcpy(utab, s->utab, sizeof utab);
    memcpy(ascii_map, s->ascii_map, sizeof ascii_map);
    memcpy(ascii_has, s->ascii_has, sizeof ascii_has);
    nutab = s->nutab; cols = s->cols; rows = s->rows; top = s->top; bar_h = s->bar_h;
    active_bar = s->bar;
#ifdef CONFIG_FREETYPE
    ft_size();
#endif
}

static void save_drawing_surface(void)
{
    save_surface(drawing_surface ? drawing_surface : &mirrored);
}

static const char *surface_for_output(struct output *o)
{
    struct monitor_conf conf;
    double s = monitor_conf(o->name, &conf) && conf.scale > 0 ? conf.scale : 1.0;
    struct output_surface *surface;
    for (surface = surfaces; surface; surface = surface->next) {
        if (strcmp(surface->name, o->name) == 0) break;
    }
    if (surface && surface->width == o->mode.hdisplay && surface->height == o->mode.vdisplay &&
        s > surface->scale - 0.001 && s < surface->scale + 0.001) {
        o->surface = surface;
        return NULL;
    }
    struct output_surface fresh = { .width = o->mode.hdisplay, .height = o->mode.vdisplay, .scale = s, .redraw = 1 };
    if ((uint64_t)fresh.width * fresh.height > SIZE_MAX / sizeof *fresh.pixels ||
        !(fresh.pixels = calloc((size_t)fresh.width * fresh.height, sizeof *fresh.pixels))) {
        return "no memory for the output canvas";
    }
    save_drawing_surface();
    use_surface(&fresh);
    const char *why = scaled_font(s);
    if (why == NULL) {
        layout();
        dirty = (struct drm_clip_rect){ 0, 0, (unsigned short)width, (unsigned short)height };
        save_surface(&fresh);
    }
    if (why != NULL) fresh.font = font;
    use_surface(drawing_surface ? drawing_surface : &mirrored);
    if (why != NULL) { free(fresh.pixels); free(fresh.font); return why; }
    if (surface) {
        uint32_t copy_w = surface->width < fresh.width ? surface->width : fresh.width;
        uint32_t copy_h = surface->height < fresh.height ? surface->height : fresh.height;
        for (uint32_t y = 0; y < copy_h; y++) {
            memcpy(fresh.pixels + (size_t)y * fresh.width, surface->pixels + (size_t)y * surface->width,
                   (size_t)copy_w * sizeof *fresh.pixels);
        }
        free(surface->pixels); free(surface->presented); free(surface->font);
        fresh.next = surface->next;
        *surface = fresh;
    } else {
        surface = malloc(sizeof *surface);
        if (!surface) { free(fresh.pixels); free(fresh.font); return "no memory for the output canvas"; }
        *surface = fresh;
        surface->next = surfaces;
        surfaces = surface;
    }
    snprintf(surface->name, sizeof surface->name, "%s", o->name);
    o->surface = surface;
    if (drawing_surface == surface) use_surface(surface);
    return NULL;
}

/* The desktop rectangles of outputs[0..noutputs) (logical_width and
 * logical_height set), placed as monitors.conf has them, as the shared
 * display does (monitor_layout); NULL, or why not. */
static const char *place_outputs(struct monitor_rect *rects)
{
    for (uint32_t i = 0; i < noutputs; i++) {
        struct output *o = &outputs[i];
        struct monitor_conf conf;
        int configured = monitor_conf(o->name, &conf), place = i == 0 ? MONITOR_ORIGIN : MONITOR_RIGHT;
        if (configured) {
            if (!strcmp(conf.place, "origin")) place = MONITOR_ORIGIN;
            else if (!strcmp(conf.place, "left")) place = MONITOR_LEFT;
            else if (!strcmp(conf.place, "above")) place = MONITOR_ABOVE;
            else if (!strcmp(conf.place, "below")) place = MONITOR_BELOW;
            else place = MONITOR_RIGHT;
        }
        if (o->logical_width < 1) o->logical_width = 1;
        if (o->logical_height < 1) o->logical_height = 1;
        if (configured && conf.has_position &&
            ((int64_t)conf.x + o->logical_width - 1 > INT_MAX ||
             (int64_t)conf.y + o->logical_height - 1 > INT_MAX)) return "output rectangle exceeds coordinate limits";
        rects[i] = (struct monitor_rect){ .name = o->name, .width = o->logical_width,
            .height = o->logical_height, .place = place,
            .has_position = configured && conf.has_position, .x = configured ? conf.x : 0,
            .y = configured ? conf.y : 0 };
    }
    monitor_layout(rects, noutputs);
    return NULL;
}

/* An output's place on the desktop as process 1 gave it; 0 when it gave none. */
static int desk_place(const char *name, int *x, int *y)
{
    for (int i = 0; i < ndesk; i++) {
        if (strcmp(desk[i].name, name) == 0) {
            *x = desk[i].x;
            *y = desk[i].y;
            return 1;
        }
    }
    return 0;
}

static const char *extended_layout(void)
{
    struct monitor_rect rects[32];
    if (noutputs > 32) return "too many output rectangles";
    for (uint32_t i = 0; i < noutputs; i++) {
        const char *why = surface_for_output(&outputs[i]);
        if (why) return why;
        struct output *o = &outputs[i];
        o->logical_width = (int)(o->surface->width / o->surface->scale);
        o->logical_height = (int)(o->surface->height / o->surface->scale);
    }
    const char *why = place_outputs(rects);
    if (why) return why;
    for (uint32_t i = 0; i < noutputs; i++) {
        /* A program with some of the screens: where they are on the desktop. */
        desk_place(outputs[i].name, &rects[i].x, &rects[i].y);
        if (outputs[i].x != rects[i].x || outputs[i].y != rects[i].y) outputs[i].surface->redraw = 1;
        outputs[i].x = rects[i].x; outputs[i].y = rects[i].y;
    }
    return NULL;
}

static void select_available_surface(void)
{
    selected_output = noutputs ? 0 : -1;
    if (drawing_surface) {
        for (uint32_t i = 0; i < noutputs; i++) {
            if (strcmp(drawing_surface->name, outputs[i].name) == 0) selected_output = (int)i;
        }
    }
    if (selected_output >= 0) {
        drawing_surface = outputs[selected_output].surface;
        use_surface(drawing_surface);
    }
}

static void *array(uint32_t count, size_t each)
{
    return calloc(count ? count : 1, each);
}

static int is_cached_dmabuf_fb(const struct output *o, uint32_t id)
{
    if (id == 0) return 0;
    for (int i = 0; i < SCREEN_DMABUF_SLOTS; i++) {
        if (o->dmabuf_cache_fb[i] == id) return 1;
    }
    return 0;
}

static int has_any_dmabuf_fb(const struct output *o)
{
    if (o->dmabuf_fb_id != 0 || o->dmabuf_pending_fb_id != 0) return 1;
    for (int i = 0; i < SCREEN_DMABUF_SLOTS; i++) {
        if (o->dmabuf_cache_fb[i] != 0) return 1;
    }
    return 0;
}

/* Release any active, in-flight, or cached dma-buf framebuffers on output o.
 * restore_crtc != 0 restores the dumb framebuffer before RMFB. */
static void clear_dmabuf_fbs(int fd, struct output *o, int restore_crtc)
{
    int had_scanout = o->dmabuf_fb_id != 0 || o->dmabuf_pending_fb_id != 0;
    if (!has_any_dmabuf_fb(o)) {
        o->dmabuf_cache_gen = 0;
        o->dmabuf_active_slot = -1;
        o->dmabuf_pending_slot = -1;
        o->dmabuf_flip_pending = 0;
        return;
    }
    if (restore_crtc && had_scanout && o->fb_id != 0 && fd >= 0) {
        struct drm_mode_crtc set = { .crtc_id = o->crtc_id, .fb_id = o->fb_id,
                                     .set_connectors_ptr = (uintptr_t)&o->conn_id,
                                     .count_connectors = 1, .mode = o->mode, .mode_valid = 1 };
        (void)ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set);
    }
    uint32_t ids[SCREEN_DMABUF_SLOTS + 2];
    int nids = 0;
    if (o->dmabuf_pending_fb_id != 0) ids[nids++] = o->dmabuf_pending_fb_id;
    if (o->dmabuf_fb_id != 0) ids[nids++] = o->dmabuf_fb_id;
    for (int i = 0; i < SCREEN_DMABUF_SLOTS; i++) {
        if (o->dmabuf_cache_fb[i] != 0) ids[nids++] = o->dmabuf_cache_fb[i];
        o->dmabuf_cache_fb[i] = 0;
    }
    for (int i = 0; fd >= 0 && i < nids; i++) {
        int dup = 0;
        for (int k = 0; k < i; k++) {
            if (ids[k] == ids[i]) { dup = 1; break; }
        }
        if (!dup) (void)ioctl(fd, DRM_IOCTL_MODE_RMFB, &ids[i]);
    }
    o->dmabuf_pending_fb_id = 0;
    o->dmabuf_fb_id = 0;
    o->dmabuf_cache_gen = 0;
    o->dmabuf_active_slot = -1;
    o->dmabuf_pending_slot = -1;
    o->dmabuf_flip_pending = 0;
}

/* Swap front and back dumb framebuffer metadata once a back-buffer flip lands. */
static void swap_dumb_buffers(struct output *o)
{
    uint8_t *tmp_map = o->map;
    uint64_t tmp_size = o->map_size;
    uint32_t tmp_pitch = o->pitch, tmp_fb = o->fb_id, tmp_handle = o->handle;
    o->map = o->back_map;
    o->map_size = o->back_map_size;
    o->pitch = o->back_pitch;
    o->fb_id = o->back_fb_id;
    o->handle = o->back_handle;
    o->back_map = tmp_map;
    o->back_map_size = tmp_size;
    o->back_pitch = tmp_pitch;
    o->back_fb_id = tmp_fb;
    o->back_handle = tmp_handle;
}

/* A back-buffer PAGE_FLIP that got no vblank in time (CRTC off, event lost):
 * show its target now with a synchronous SETCRTC and make it the front. Front
 * and back bookkeeping then always match the CRTC, so a late flip event for the
 * old target can never land on the buffer that is being written next. */
static void shm_settle_target(struct output *o)
{
    struct drm_mode_crtc set = {
        .crtc_id = o->crtc_id,
        .fb_id = o->back_fb_id,
        .set_connectors_ptr = (uintptr_t)&o->conn_id,
        .count_connectors = 1,
        .mode = o->mode,
        .mode_valid = 1,
    };
    o->shm_flip_pending = 0;
    if (card >= 0 && o->back_fb_id != 0 && ioctl(card, DRM_IOCTL_MODE_SETCRTC, &set) == 0) {
        swap_dumb_buffers(o);
    }
}

static void destroy_back_buffer(int fd, struct output *o)
{
    if (o->back_map != NULL) {
        munmap(o->back_map, (size_t)o->back_map_size);
        o->back_map = NULL;
    }
    o->back_map_size = 0;
    o->back_pitch = 0;
    if (o->back_fb_id != 0) {
        if (fd >= 0) ioctl(fd, DRM_IOCTL_MODE_RMFB, &o->back_fb_id);
        o->back_fb_id = 0;
    }
    if (o->back_handle != 0) {
        if (fd >= 0) {
            struct drm_mode_destroy_dumb d = { .handle = o->back_handle };
            ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        }
        o->back_handle = 0;
    }
    o->shm_flip_pending = 0;
    o->shm_flip_broken = 0;
}

/* Lazily create the second dumb framebuffer for tear-free CPU page flipping.
 * Ordinary TUI programs never trigger this, keeping their memory footprint and
 * single-buffer test expectations unchanged. */
static int ensure_back_buffer(int fd, struct output *o)
{
    if (o->back_map != NULL && o->back_fb_id != 0) return 0;
    destroy_back_buffer(fd, o);
    struct drm_mode_create_dumb dumb = {
        .width = o->mode.hdisplay,
        .height = o->mode.vdisplay,
        .bpp = 32,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) return -1;
    o->back_handle = dumb.handle;
    o->back_pitch = dumb.pitch;
    o->back_map_size = dumb.size;
    if (dumb.pitch < (uint32_t)o->mode.hdisplay * 4u || dumb.pitch % 4 != 0 ||
        dumb.size < (uint64_t)dumb.pitch * o->mode.vdisplay || dumb.size > SIZE_MAX) {
        destroy_back_buffer(fd, o);
        errno = EINVAL;
        return -1;
    }
    struct drm_mode_fb_cmd f = {
        .width = o->mode.hdisplay,
        .height = o->mode.vdisplay,
        .pitch = dumb.pitch,
        .bpp = 32,
        .depth = 24,
        .handle = dumb.handle,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &f) < 0) {
        int err = errno;
        destroy_back_buffer(fd, o);
        errno = err;
        return -1;
    }
    o->back_fb_id = f.fb_id;
    struct drm_mode_map_dumb m = { .handle = dumb.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) {
        int err = errno;
        destroy_back_buffer(fd, o);
        errno = err;
        return -1;
    }
    void *p = mmap(NULL, (size_t)dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
    if (p == MAP_FAILED) {
        int err = errno;
        destroy_back_buffer(fd, o);
        errno = err;
        return -1;
    }
    o->back_map = p;
    return 0;
}

/* Each successful step owns its resources immediately, including on failure. */
static void destroy_output(int fd, struct output *o)
{
    clear_dmabuf_fbs(fd, o, 1);
    destroy_back_buffer(fd, o);
    if (o->map != NULL) {
        munmap(o->map, (size_t)o->map_size);
        o->map = NULL;
    }
    if (o->fb_id != 0) {
        ioctl(fd, DRM_IOCTL_MODE_RMFB, &o->fb_id);
        o->fb_id = 0;
    }
    if (o->handle != 0) {
        struct drm_mode_destroy_dumb d = { .handle = o->handle };
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        o->handle = 0;
    }
}

static void clear_outputs(int fd)
{
    if (extended) save_drawing_surface();
    cursor_drop(fd);
    for (uint32_t i = 0; i < noutputs; i++) {
        destroy_output(fd, &outputs[i]);
        free(outputs[i].modes);
    }
    free(outputs);
    outputs = NULL;
    noutputs = 0;
}

/* Retry count/fill races, without ever using counts larger than allocations. */
static const char *read_resources(int fd, struct drm_mode_card_res *res,
                                  uint32_t **connectors, uint32_t **crtcs)
{
    *connectors = NULL;
    *crtcs = NULL;
    for (int retry = 0; retry < 4; retry++) {
        *res = (struct drm_mode_card_res){ 0 };
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res) < 0) {
            return "DRM_IOCTL_MODE_GETRESOURCES failed";
        }
        uint32_t nc = res->count_connectors, nr = res->count_crtcs;
        uint32_t *cs = array(nc, sizeof *cs), *rs = array(nr, sizeof *rs);
        if (cs == NULL || rs == NULL) {
            free(cs);
            free(rs);
            return "no memory for DRM resources";
        }
        res->connector_id_ptr = (uintptr_t)cs;
        res->crtc_id_ptr = (uintptr_t)rs;
        res->count_encoders = res->count_fbs = 0;
        int ok = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res);
        if (ok == 0 && res->count_connectors <= nc && res->count_crtcs <= nr) {
            *connectors = cs;
            *crtcs = rs;
            return NULL;
        }
        free(cs);
        free(rs);
        if (ok < 0) {
            return "DRM_IOCTL_MODE_GETRESOURCES failed";
        }
    }
    return "DRM resources kept changing";
}

/* 1: usable connector, 0: disconnected/unavailable, -1: allocation failed.
 * On a lease, GETENCODER filters possible_crtcs to the visible CRTC list.
 * Passing count_modes > 0 first uses DRM's cached mode list (drmModeGetConnectorCurrent
 * semantics), avoiding synchronous DDC/EDID probes on every task switch.
 * Positive probes immediately (legacy discovery and refresh), zero may fall
 * back to probing (leases), negative uses only cached state (hybrid try). */
static int probe_connectors = 1;

static int read_output(int fd, uint32_t id, struct output *o)
{
    for (int retry = 0; retry < 4; retry++) {
        struct drm_mode_modeinfo stack_modes[64];
        uint32_t stack_encs[8];
        struct drm_mode_get_connector c = { .connector_id = id };
        int used_stack = 0;
        if (probe_connectors <= 0) {
            c.count_modes = 64;
            c.modes_ptr = (uintptr_t)stack_modes;
            c.count_encoders = 8;
            c.encoders_ptr = (uintptr_t)stack_encs;
            used_stack = 1;
        }
        double t0 = screen_time();
        int got = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
        double elapsed = screen_time() - t0;
        if (elapsed >= 0.1) {
            say("screen: connector %u: initial query %.0f ms (result %d, state %u)",
                id, elapsed * 1000, got, c.connection);
        }
        if (got < 0) {
            return 0;
        }
        if (used_stack && (c.connection == 3 || (c.connection == 1 && c.count_modes == 0))) {
            /* A hybrid preference is speculative; leave unknown connectors
             * to the existing fallback. On a lease, keep the old probe. */
            if (probe_connectors < 0) return 0;
            c = (struct drm_mode_get_connector){ .connector_id = id };
            used_stack = 0;
            got = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
            if (got < 0) {
                return 0;
            }
        }
        if (c.connection != 1 || c.count_modes == 0) {
            return 0; /* 1 = connected */
        }
        uint32_t nm = c.count_modes, ne = c.count_encoders;
        struct drm_mode_modeinfo *modes = array(nm, sizeof *modes);
        uint32_t *encs = array(ne, sizeof *encs);
        if (modes == NULL || encs == NULL) {
            free(modes);
            free(encs);
            return -1;
        }
        if (used_stack && nm <= 64 && ne <= 8) {
            memcpy(modes, stack_modes, nm * sizeof *modes);
            if (ne > 0) memcpy(encs, stack_encs, ne * sizeof *encs);
        } else {
            c.modes_ptr = (uintptr_t)modes;
            c.encoders_ptr = (uintptr_t)encs;
            c.count_props = 0;
            int ok = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
            if (ok < 0 || c.connection != 1 || c.count_modes == 0) {
                free(modes);
                free(encs);
                return 0;
            }
            if (c.count_modes > nm || c.count_encoders > ne) {
                free(modes);
                free(encs);
                continue;
            }
        }
        *o = (struct output){ .conn_id = id, .dmabuf_active_slot = -1, .dmabuf_pending_slot = -1 };
        snprintf(o->name, sizeof o->name, "%s-%u", c.connector_type < NCONNECTOR_TYPES ?
                 connector_types[c.connector_type] : "Unknown", c.connector_type_id);
        o->internal = monitor_internal(o->name);
        struct monitor_conf conf;
        int configured = monitor_conf(o->name, &conf);
        /* Consider every encoder, not just the currently attached one. */
        for (uint32_t e = 0; e <= c.count_encoders; e++) {
            uint32_t eid = e == c.count_encoders ? c.encoder_id : encs[e];
            if (eid == 0) {
                continue;
            }
            struct drm_mode_get_encoder enc = { .encoder_id = eid };
            if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0) {
                o->possible_crtcs |= enc.possible_crtcs;
                if (eid == c.encoder_id) {
                    o->preferred_crtc = enc.crtc_id;
                    struct drm_mode_crtc current = { .crtc_id = enc.crtc_id };
                    if (enc.crtc_id && ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &current) == 0 && current.mode_valid) {
                        o->current_mode = current.mode;
                    }
                }
            }
        }
        int chosen = monitor_mode_choose(modes, c.count_modes, &o->current_mode,
                                         configured ? conf.width : 0, configured ? conf.height : 0,
                                         configured ? conf.refresh_mhz : 0);
        if (chosen < 0) {
            if (configured) say("screen: %s: saved mode %dx%d@%d mHz is not advertised; keeping an available mode",
                                o->name, conf.width, conf.height, conf.refresh_mhz);
            chosen = monitor_mode_choose(modes, c.count_modes, &o->current_mode, 0, 0, 0);
        }
        if (chosen >= 0) o->mode = modes[chosen];
        free(encs);
        if (!o->mode.hdisplay || !o->possible_crtcs) {
            free(modes);
            return 0;
        }
        o->modes = modes;
        o->nmodes = c.count_modes;
        return 1;
    }
    return 0;
}

/* Greedily use a free CRTC, then move earlier choices along augmenting paths.
 * This covers the maximum number of connectors without sharing a CRTC.
 * Internal panels have no matching priority; they only choose logical size. */
static int match_output(struct output *list, uint32_t index, const uint32_t *crtcs,
                        uint32_t count, uint32_t owners[32], uint32_t *seen)
{
    struct output *o = &list[index];
    uint32_t first = 0;
    for (uint32_t k = 0; k < count; k++) {
        if (crtcs[k] == o->preferred_crtc) {
            first = k;
            break;
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t j = 0; j < count; j++) {
            uint32_t k = (first + j) % count, bit = 1u << k;
            if (!(o->possible_crtcs & bit) || (*seen & bit) ||
                (pass == 0 && owners[k] != UINT32_MAX)) {
                continue;
            }
            *seen |= bit;
            if (owners[k] == UINT32_MAX ||
                match_output(list, owners[k], crtcs, count, owners, seen)) {
                owners[k] = index;
                o->crtc_id = crtcs[k];
                return 1;
            }
        }
    }
    return 0;
}

static const char *find_outputs(int fd)
{
    struct drm_mode_card_res res;
    uint32_t *connectors, *crtcs;
    const char *why = read_resources(fd, &res, &connectors, &crtcs);
    if (why != NULL) {
        return why;
    }
    struct output *list = array(res.count_connectors, sizeof *list);
    uint32_t count = 0;
    if (list == NULL) {
        why = "no memory for DRM outputs";
        goto done;
    }
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        int found = read_output(fd, connectors[i], &list[count]);
        if (found < 0) {
            why = "no memory for connector modes";
            goto done;
        }
        count += found != 0;
    }
    uint32_t owners[32]; /* possible_crtcs is a 32-bit mask, including on leases */
    for (uint32_t k = 0; k < 32; k++) {
        owners[k] = UINT32_MAX;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t seen = 0;
        match_output(list, i, crtcs, res.count_crtcs < 32 ? res.count_crtcs : 32,
                     owners, &seen);
    }
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (list[i].crtc_id != 0) {
            list[used++] = list[i];
        } else {
            say("screen: connector %u has no available CRTC", list[i].conn_id);
            free(list[i].modes);
            list[i].modes = NULL;
        }
    }
    if (used == 0) {
        why = "no connected output with a compatible CRTC";
        goto done;
    }
    /* Prefer the first selected internal panel only for logical size and the
     * single-output lease fallback. Keep resource order otherwise. */
    for (uint32_t i = 0; i < used; i++) {
        if (list[i].internal) {
            struct output first = list[i];
            memmove(list + 1, list, (size_t)i * sizeof *list);
            list[0] = first;
            break;
        }
    }
    outputs = list;
    noutputs = used;
    list = NULL;

done:
    if (list) {
        for (uint32_t i = 0; i < count; i++) free(list[i].modes);
    }
    free(list);
    free(connectors);
    free(crtcs);
    return why;
}

/* Log what a card is: its driver and every connector with its state, to see
 * why no output was found. */
static void describe_card(int fd, const char *path)
{
    static const char *const states[] = { "?", "connected", "disconnected", "unknown" };
    char name[32] = "";
    struct drm_version v = { .name = name, .name_len = sizeof name - 1 };
    struct drm_mode_card_res res = { 0 };
    char line[400];
    size_t len;

    ioctl(fd, DRM_IOCTL_VERSION, &v);
    len = (size_t)snprintf(line, sizeof line, "screen: %s is %s:", path, name[0] ? name : "?");
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        say("%s no mode setting (%m)", line);
        return;
    }
    uint32_t *connectors = array(res.count_connectors, sizeof *connectors);
    if (connectors == NULL) {
        say("%s no memory for connectors", line);
        return;
    }
    struct drm_mode_card_res r2 = { .connector_id_ptr = (uintptr_t)connectors,
                                    .count_connectors = res.count_connectors };
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2) < 0) {
        free(connectors);
        say("%s cannot read connectors (%m)", line);
        return;
    }
    for (uint32_t i = 0; i < res.count_connectors && i < r2.count_connectors && len < sizeof line; i++) {
        struct drm_mode_get_connector c = { .connector_id = connectors[i] };
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) {
            continue;
        }
        len += (size_t)snprintf(line + len, sizeof line - len, " %s-%u %s (%u modes)",
                                c.connector_type < NCONNECTOR_TYPES ?
                                connector_types[c.connector_type] : "?",
                                c.connector_type_id, c.connection < 4 ? states[c.connection] : "?",
                                c.count_modes);
    }
    say("%s%s", line, res.count_connectors ? "" : " no connectors");
    free(connectors);
}

static void mark_all(void)
{
    dirty = (struct drm_clip_rect){ 0, 0, (unsigned short)width, (unsigned short)height };
    presented_valid = 0;
    active_bar.valid = 0;
    if (extended) {
        for (uint32_t i = 0; i < noutputs; i++) {
            struct output_surface *s = outputs[i].surface;
            if (s) {
                s->dirty = (struct drm_clip_rect){ 0, 0, (unsigned short)s->width, (unsigned short)s->height };
                s->presented_valid = 0;
                s->bar.valid = 0;
            }
        }
    }
}

/* Tighten logical damage against the last-flushed snapshot so full-screen
 * redraws only upload rows and columns whose pixels actually changed. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O3")))
#endif
static int present_diff(const uint32_t *src, uint32_t w, uint32_t h, struct drm_clip_rect clip,
                        const uint32_t *snap, uint32_t snap_w, uint32_t snap_h, int valid,
                        struct drm_clip_rect *out)
{
    uint32_t x0 = clip.x1 < w ? clip.x1 : w;
    uint32_t y0 = clip.y1 < h ? clip.y1 : h;
    uint32_t x1 = clip.x2 <= w ? clip.x2 : w;
    uint32_t y1 = clip.y2 <= h ? clip.y2 : h;
    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }
    if (!valid || snap == NULL || snap_w != w || snap_h != h) {
        *out = (struct drm_clip_rect){ (unsigned short)x0, (unsigned short)y0,
                                       (unsigned short)x1, (unsigned short)y1 };
        return 1;
    }
    size_t row_bytes_n = (size_t)(x1 - x0) * sizeof *src;
    uint32_t dy0 = y0;
    while (dy0 < y1 &&
           memcmp(src + (size_t)dy0 * w + x0, snap + (size_t)dy0 * w + x0, row_bytes_n) == 0) {
        dy0++;
    }
    if (dy0 >= y1) {
        return 0;
    }
    uint32_t dy1 = y1;
    while (dy1 > dy0 + 1 &&
           memcmp(src + (size_t)(dy1 - 1) * w + x0, snap + (size_t)(dy1 - 1) * w + x0, row_bytes_n) == 0) {
        dy1--;
    }
    uint32_t dx0 = x1, dx1 = x0;
    for (uint32_t y = dy0; y < dy1; y++) {
        const uint32_t *a = src + (size_t)y * w;
        const uint32_t *b = snap + (size_t)y * w;
        if (dx0 > x0 && memcmp(a + x0, b + x0, (size_t)(dx0 - x0) * sizeof *src) != 0) {
            uint32_t x = x0;
            while (x < dx0 && a[x] == b[x]) x++;
            dx0 = x;
        }
        uint32_t rlim = dx1 > dx0 ? dx1 : dx0;
        if (rlim < x1 && memcmp(a + rlim, b + rlim, (size_t)(x1 - rlim) * sizeof *src) != 0) {
            uint32_t x = x1;
            while (x > rlim && a[x - 1] == b[x - 1]) x--;
            if (x > dx1) dx1 = x;
        } else if (dx1 <= dx0 && dx0 < x1) {
            dx1 = dx0 + 1;
        }
        if (dx0 == x0 && dx1 == x1) {
            break;
        }
    }
    *out = (struct drm_clip_rect){ (unsigned short)dx0, (unsigned short)dy0,
                                   (unsigned short)dx1, (unsigned short)dy1 };
    return 1;
}

static void present_commit(const uint32_t *src, uint32_t w, uint32_t h, struct drm_clip_rect rect,
                           uint32_t **snap, uint32_t *snap_w, uint32_t *snap_h, int *valid)
{
    uint64_t total = (uint64_t)w * h;
    if (total == 0 || total > SIZE_MAX / sizeof *src) {
        drop_presented(snap, snap_w, snap_h, valid);
        return;
    }
    if (*snap == NULL || *snap_w != w || *snap_h != h) {
        uint32_t *buf = malloc((size_t)total * sizeof *src);
        if (buf == NULL) {
            drop_presented(snap, snap_w, snap_h, valid);
            return;
        }
        free(*snap);
        *snap = buf;
        *snap_w = w;
        *snap_h = h;
        memcpy(buf, src, (size_t)total * sizeof *src);
        *valid = 1;
        return;
    }
    if (!*valid) {
        memcpy(*snap, src, (size_t)total * sizeof *src);
        *valid = 1;
        return;
    }
    uint32_t x0 = rect.x1 < w ? rect.x1 : w;
    uint32_t y0 = rect.y1 < h ? rect.y1 : h;
    uint32_t x1 = rect.x2 <= w ? rect.x2 : w;
    uint32_t y1 = rect.y2 <= h ? rect.y2 : h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (x0 == 0 && x1 == w) {
        memcpy(*snap + (size_t)y0 * w, src + (size_t)y0 * w,
               (size_t)(y1 - y0) * w * sizeof *src);
        return;
    }
    size_t span_bytes = (size_t)(x1 - x0) * sizeof *src;
    for (uint32_t y = y0; y < y1; y++) {
        memcpy(*snap + (size_t)y * w + x0, src + (size_t)y * w + x0, span_bytes);
    }
}

static int set_crtc(int fd, struct output *o)
{
    struct drm_mode_crtc set = { .crtc_id = o->crtc_id, .fb_id = o->fb_id,
                                 .set_connectors_ptr = (uintptr_t)&o->conn_id,
                                 .count_connectors = 1, .mode = o->mode, .mode_valid = 1 };
    return ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set);
}

/* Nonzero dimensions, aspect-preserving integer fit, centered in the output. */
static struct drm_clip_rect fit_rect(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
    uint32_t w = dw, h = dh;
    if ((uint64_t)dw * sh <= (uint64_t)dh * sw) {
        h = (uint32_t)((uint64_t)dw * sh / sw);
        if (h == 0) h = 1;
    } else {
        w = (uint32_t)((uint64_t)dh * sw / sh);
        if (w == 0) w = 1;
    }
    uint32_t x = (dw - w) / 2, y = (dh - h) / 2;
    return (struct drm_clip_rect){ (unsigned short)x, (unsigned short)y,
                                  (unsigned short)(x + w), (unsigned short)(y + h) };
}

/* Pure memory copy: identical sizes keep dirty rows; scaled outputs clip to
 * the mapped destination sub-rect and stream through a cached row buffer. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O3")))
#endif
static struct drm_clip_rect copy_picture(uint8_t *dst, uint32_t pitch, uint32_t dw, uint32_t dh,
                                         const uint32_t *src, uint32_t sw, uint32_t sh,
                                         struct drm_clip_rect clip)
{
    if (dw == sw && dh == sh) {
        for (uint32_t y = clip.y1; y < clip.y2; y++) {
            memcpy(dst + (uint64_t)y * pitch + clip.x1 * 4u,
                   src + (uint64_t)y * sw + clip.x1, (size_t)(clip.x2 - clip.x1) * 4);
        }
        return clip;
    }
    struct drm_clip_rect fit = fit_rect(sw, sh, dw, dh);
    uint32_t fw = fit.x2 - fit.x1, fh = fit.y2 - fit.y1;
    int full = clip.x1 == 0 && clip.y1 == 0 && clip.x2 >= sw && clip.y2 >= sh;
    uint32_t cx1 = clip.x1 < sw ? clip.x1 : sw;
    uint32_t cx2 = clip.x2 < sw ? clip.x2 : sw;
    uint32_t cy1 = clip.y1 < sh ? clip.y1 : sh;
    uint32_t cy2 = clip.y2 < sh ? clip.y2 : sh;
    uint32_t fx0 = full ? 0 : (uint32_t)(((uint64_t)cx1 * fw + sw - 1) / sw);
    uint32_t fx1 = full ? fw : (uint32_t)(((uint64_t)cx2 * fw + sw - 1) / sw);
    uint32_t fy0 = full ? 0 : (uint32_t)(((uint64_t)cy1 * fh + sh - 1) / sh);
    uint32_t fy1 = full ? fh : (uint32_t)(((uint64_t)cy2 * fh + sh - 1) / sh);
    if (fx1 > fw) fx1 = fw;
    if (fy1 > fh) fy1 = fh;
    uint32_t dx0 = fit.x1 + fx0, dx1 = fit.x1 + fx1;
    uint32_t dy0 = fit.y1 + fy0, dy1 = fit.y1 + fy1;
    uint32_t span = dx1 > dx0 ? dx1 - dx0 : 0;
    if (!full && (span == 0 || dy0 >= dy1)) {
        return (struct drm_clip_rect){ 0, 0, 0, 0 };
    }

    uint32_t stack_xmap[4096], stack_row[4096];
    uint32_t row_need = full ? dw : span;
    uint32_t *xmap = span <= 4096 ? stack_xmap : malloc((size_t)span * sizeof *xmap);
    uint32_t *row_buf = row_need <= 4096 ? stack_row : malloc((size_t)row_need * sizeof *row_buf);
    if (xmap != NULL) {
        for (uint32_t i = 0; i < span; i++) {
            xmap[i] = (uint32_t)((uint64_t)(fx0 + i) * sw / fw);
        }
    }

    if (full) {
        for (uint32_t y = 0; y < fit.y1; y++) {
            memset(dst + (uint64_t)y * pitch, 0, (size_t)dw * 4);
        }
        for (uint32_t y = fit.y1; y < fit.y2; y++) {
            const uint32_t *s = src + (uint64_t)((uint64_t)(y - fit.y1) * sh / fh) * sw;
            if (row_buf != NULL && xmap != NULL) {
                if (fit.x1 > 0) memset(row_buf, 0, (size_t)fit.x1 * 4);
                for (uint32_t i = 0; i < fw; i++) row_buf[fit.x1 + i] = s[xmap[i]];
                if (fit.x2 < dw) memset(row_buf + fit.x2, 0, (size_t)(dw - fit.x2) * 4);
                memcpy(dst + (uint64_t)y * pitch, row_buf, (size_t)dw * 4);
            } else {
                uint32_t *line = (uint32_t *)(dst + (uint64_t)y * pitch);
                memset(line, 0, (size_t)dw * 4);
                for (uint32_t x = 0; x < fw; x++) line[fit.x1 + x] = s[(uint64_t)x * sw / fw];
            }
        }
        for (uint32_t y = fit.y2; y < dh; y++) {
            memset(dst + (uint64_t)y * pitch, 0, (size_t)dw * 4);
        }
        if (xmap != stack_xmap) free(xmap);
        if (row_buf != stack_row) free(row_buf);
        return (struct drm_clip_rect){ 0, 0, (unsigned short)dw, (unsigned short)dh };
    }

    for (uint32_t y = dy0; y < dy1; y++) {
        const uint32_t *s = src + (uint64_t)((uint64_t)(y - fit.y1) * sh / fh) * sw;
        if (row_buf != NULL && xmap != NULL) {
            for (uint32_t i = 0; i < span; i++) row_buf[i] = s[xmap[i]];
            memcpy(dst + (uint64_t)y * pitch + (uint64_t)dx0 * 4u, row_buf, (size_t)span * 4);
        } else {
            uint32_t *line = (uint32_t *)(dst + (uint64_t)y * pitch);
            for (uint32_t i = 0; i < span; i++) line[dx0 + i] = s[(uint64_t)(fx0 + i) * sw / fw];
        }
    }
    if (xmap != stack_xmap) free(xmap);
    if (row_buf != stack_row) free(row_buf);
    return (struct drm_clip_rect){ (unsigned short)dx0, (unsigned short)dy0,
                                  (unsigned short)dx1, (unsigned short)dy1 };
}

static const char *create_output(int fd, struct output *o)
{
    struct drm_mode_create_dumb dumb = { .width = o->mode.hdisplay,
                                        .height = o->mode.vdisplay, .bpp = 32 };
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
        return "DRM_IOCTL_MODE_CREATE_DUMB failed";
    }
    o->handle = dumb.handle;
    o->pitch = dumb.pitch;
    o->map_size = dumb.size;
    if (dumb.pitch < (uint32_t)o->mode.hdisplay * 4u || dumb.pitch % 4 != 0 ||
        dumb.size < (uint64_t)dumb.pitch * o->mode.vdisplay || dumb.size > SIZE_MAX) {
        return "invalid dumb buffer size";
    }
    struct drm_mode_fb_cmd f = { .width = o->mode.hdisplay, .height = o->mode.vdisplay,
                                 .pitch = dumb.pitch, .bpp = 32, .depth = 24,
                                 .handle = dumb.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &f) < 0) {
        return "DRM_IOCTL_MODE_ADDFB failed";
    }
    o->fb_id = f.fb_id;
    struct drm_mode_map_dumb m = { .handle = dumb.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) {
        return "DRM_IOCTL_MODE_MAP_DUMB failed";
    }
    void *p = mmap(NULL, (size_t)dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
    if (p == MAP_FAILED) {
        return "cannot map the dumb buffer";
    }
    o->map = p;
    return NULL;
}

/* Retry only advertised timings: first the previous active mode, then modes
 * with strictly lower pixel-clock demand. Desired configuration stays saved. */
static uint64_t mode_cost(const struct drm_mode_modeinfo *m)
{
    if (m->clock) return (uint64_t)m->clock * 1000000;
    return (uint64_t)m->hdisplay * m->vdisplay * (unsigned)monitor_mode_millihz(m);
}

static int fallback_mode(const struct output *o, const struct drm_mode_modeinfo *failed,
                         uint64_t below, int active)
{
    if (active && o->current_mode.hdisplay && !monitor_mode_same(&o->current_mode, failed)) {
        int current = monitor_mode_choose(o->modes, o->nmodes, NULL, o->current_mode.hdisplay,
                                          o->current_mode.vdisplay, monitor_mode_millihz(&o->current_mode));
        if (current >= 0) return current;
    }
    int best = -1;
    uint64_t cost = 0;
    for (uint32_t i = 0; i < o->nmodes; i++) {
        const struct drm_mode_modeinfo *m = &o->modes[i];
        uint64_t demand = mode_cost(m);
        if (!m->hdisplay || !m->vdisplay || monitor_mode_same(m, failed) || demand >= below) continue;
        if (best < 0 || demand > cost) { best = (int)i; cost = demand; }
    }
    return best;
}

static const char *enable_output(int fd, struct output *o)
{
    struct drm_mode_modeinfo requested = o->mode;
    uint64_t below = mode_cost(&requested);
    const char *error = NULL;
    int active = 1;
    for (uint32_t attempt = 0; attempt <= o->nmodes + 1; attempt++) {
        error = extended ? surface_for_output(o) : NULL;
        if (!error) error = create_output(fd, o);
        if (error) { destroy_output(fd, o); return error; }
        if (!error) {
            /* Never enable an unpopulated framebuffer, including fallback. */
            struct output_surface *s = extended ? o->surface : NULL;
            struct drm_clip_rect clip = s ? (struct drm_clip_rect){ 0, 0,
                (unsigned short)s->width, (unsigned short)s->height } :
                (struct drm_clip_rect){ 0, 0, (unsigned short)width, (unsigned short)height };
            copy_picture(o->map, o->pitch, o->mode.hdisplay, o->mode.vdisplay,
                         s ? s->pixels : shadow, s ? s->width : width, s ? s->height : height, clip);
            if (set_crtc(fd, o) == 0) {
                if (!monitor_mode_same(&o->mode, &requested)) {
                    say("screen: %s: requested %ux%u@%d mHz unavailable; using %ux%u@%d mHz (saved setting kept)",
                        o->name, requested.hdisplay, requested.vdisplay, monitor_mode_millihz(&requested),
                        o->mode.hdisplay, o->mode.vdisplay, monitor_mode_millihz(&o->mode));
                }
                return NULL;
            }
            error = "DRM_IOCTL_MODE_SETCRTC failed";
        }
        destroy_output(fd, o);
        int next = fallback_mode(o, &o->mode, below, active);
        active = 0;
        if (next < 0) break;
        o->mode = o->modes[next];
        uint64_t demand = mode_cost(&o->mode);
        if (demand < below) below = demand;
    }
    return error;
}

/* Show the current picture on the outputs of this fd (native per-output
 * canvases in extended mode, one fitted canvas otherwise). */
static const char *show(int fd)
{
    const char *why = "no usable output buffers";
    uint32_t orig_noutputs = noutputs;
    uint32_t used = 0;
    int resized_fallback = 0;
    int any_mode_changed = 0;
    for (uint32_t i = 0; i < noutputs; i++) {
        struct output *o = &outputs[i];
        int old_w = o->mode.hdisplay, old_h = o->mode.vdisplay;
        const char *error = enable_output(fd, o);
        if (error != NULL) {
            why = error;
            say("screen: connector %u unavailable: %s", o->conn_id, error);
            destroy_output(fd, o);
            free(o->modes);
            o->modes = NULL;
            continue;
        }
        if (old_w != o->mode.hdisplay || old_h != o->mode.vdisplay) {
            any_mode_changed = 1;
            if (used == 0) resized_fallback = 1;
        }
        outputs[used++] = *o;
    }
    noutputs = used;
    if (used == 0) {
        clear_outputs(fd);
        card = -1;
        return why;
    }
    card = fd;
    lent = 0;
    if (!extended && resized_fallback && (width != outputs[0].mode.hdisplay || height != outputs[0].mode.vdisplay)) {
        uint32_t w = outputs[0].mode.hdisplay, h = outputs[0].mode.vdisplay;
        uint32_t *picture = (uint64_t)w * h > SIZE_MAX / sizeof *picture ? NULL :
                            calloc((size_t)w * h, sizeof *picture);
        if (picture) {
            copy_picture((uint8_t *)picture, w * 4u, w, h, shadow, width, height,
                         (struct drm_clip_rect){ 0, 0, (unsigned short)width, (unsigned short)height });
            free(shadow);
            shadow = picture; width = w; height = h;
            drop_presented(&presented, &presented_w, &presented_h, &presented_valid);
            layout();
        } else {
            say("screen: fallback canvas allocation failed; fitting the previous canvas");
        }
    }
    if (extended) {
        if (any_mode_changed || used != orig_noutputs) {
            const char *layout_error = extended_layout();
            if (layout_error) say("screen: fallback layout: %s", layout_error);
        }
        select_available_surface();
    }
    mark_all();
    screen_flush();
    cursor_put(fd);
    return NULL;
}

/* Refresh the outputs and match the drawing size to the first selected one.
 * A take is followed by K_SHOWN, so programs lay out and draw it again. */
static const char *prepare(int fd)
{
    monitor_profiles_select_sysfs();
    const char *why = find_outputs(fd);
    if (why != NULL) {
        return why;
    }
    if (extended) {
        /* Never an output without a canvas: one whose canvas cannot be made
         * is dropped, as show drops one without a buffer; other errors leave
         * none. */
        uint32_t used = 0;
        for (uint32_t i = 0; i < noutputs; i++) {
            const char *error = surface_for_output(&outputs[i]);
            if (error != NULL) {
                why = error;
                say("screen: %s unavailable: %s", outputs[i].name, error);
                free(outputs[i].modes);
                outputs[i].modes = NULL;
                continue;
            }
            outputs[used++] = outputs[i];
        }
        noutputs = used;
        if (used > 0) why = extended_layout();
        if (why != NULL) clear_outputs(fd);
        return why;
    }
    uint32_t w = outputs[0].mode.hdisplay, h = outputs[0].mode.vdisplay;
    if (shadow == NULL || width != w || height != h) {
        uint32_t *picture = (uint64_t)w * h > SIZE_MAX / sizeof *picture ? NULL :
                            calloc((size_t)w * h, sizeof *picture);
        if (picture == NULL) {
            clear_outputs(fd);
            return "no memory for the shadow buffer";
        }
        double s = output_scale();
        if ((why = scaled_font(s)) != NULL) {
            free(picture);
            clear_outputs(fd);
            return why;
        }
        free(shadow);
        shadow = picture;
        width = w;
        height = h;
        scale = s;
        drop_presented(&presented, &presented_w, &presented_h, &presented_valid);
        layout();
    }
    return NULL;
}

static double screen_time(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* A window on the shared display (LOCACHY_BACKEND=wayland, wayland-backend.c)
 * instead of screens: one canvas, the window's size in pixels at its scale.
 * No DRM, lease or process-1 control is used; a new size is K_SHOWN. The
 * desktop has its own bar, so a window has none: the canvas keeps the top
 * bar's rows above the window (programs lay out as on a screen, their first
 * row of content at the window's top), screen_bar draws nothing, and those
 * rows are never shown. */
static int window_mode;
static uint32_t window_hidden; /* the top bar's pixel rows, above the window */

int screen_window_top(void)
{
    return window_mode ? (int)window_hidden : 0;
}

#ifdef CONFIG_WAYLAND
static int window_new_size;

static int window_resize(int w, int h, double s)
{
    if (w < 1 || h < 1 || w > 16384 || h > 16384) return -1;
    if (s < 0.5 || s > 8) s = 1.0;
    /* The text at this scale first: the hidden bar's height follows it. */
    if ((font == NULL || s < scale - 0.001 || s > scale + 0.001) && scaled_font(s) != NULL && font == NULL) {
        return -1; /* no font at all; one that cannot be scaled stays */
    }
    uint32_t hidden = bar_height();
    uint32_t *picture = calloc((size_t)w * ((size_t)h + hidden), sizeof *picture);
    if (picture == NULL) {
        layout(); /* the old canvas, with the text as it is now */
        return -1;
    }
    free(shadow);
    shadow = picture;
    width = (uint32_t)w;
    height = (uint32_t)h + hidden;
    window_hidden = hidden;
    scale = s;
    drop_presented(&presented, &presented_w, &presented_h, &presented_valid);
    layout();
    mark_all();
    window_new_size = 1;
    return 0;
}

static void window_frame(void)
{
    screen_flush();
}

int screen_window_shown(void)
{
    int shown = window_new_size;
    window_new_size = 0;
    return shown;
}
#endif

/* Pure-sysfs DRM card enumeration and selection helpers. None of these ever
 * open /dev/dri/card* so a discrete GPU kept suspended for compute workloads
 * is never woken during discovery or matching. */
static const char *drm_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int drm_valid_card_name(const char *name)
{
    if (!name || strncmp(name, "card", 4) != 0) return 0;
    const char *p = name + 4;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    return *p == '\0' && (size_t)(p - name) < 16;
}

static int drm_extract_card_name(const char *name_or_path, char *out, size_t size)
{
    if (!name_or_path || !*name_or_path || !out || size < 6) return 0;
    const char *card_name = name_or_path;
    if (strncmp(name_or_path, "/dev/dri/", 9) == 0) {
        card_name = name_or_path + 9;
    } else if (strchr(name_or_path, '/') != NULL) {
        return 0;
    }
    if (!drm_valid_card_name(card_name)) return 0;
    int n = snprintf(out, size, "%s", card_name);
    return n > 0 && (size_t)n < size;
}

static int drm_selector_is_auto(const char *selector)
{
    return !selector || !*selector || strcmp(selector, "auto") == 0;
}

static int drm_selector_valid(const char *selector)
{
    if (drm_selector_is_auto(selector)) return 1;
    size_t len = strlen(selector);
    if (len >= 64) return 0;
    if (strcmp(selector, ".") == 0 || strcmp(selector, "..") == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)selector[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.')) {
            return 0;
        }
    }
    return 1;
}

static int drm_readlink_base(const char *path, char *out, size_t size)
{
    char target[PATH_MAX];
    ssize_t n = readlink(path, target, sizeof target - 1);
    if (n <= 0) {
        out[0] = '\0';
        return 0;
    }
    target[n] = '\0';
    while (n > 1 && target[n - 1] == '/') target[--n] = '\0';
    const char *base = drm_basename(target);
    if (!*base || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        out[0] = '\0';
        return 0;
    }
    int w = snprintf(out, size, "%s", base);
    if (w < 0 || (size_t)w >= size) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

static int drm_read_card_info(const char *sysfs_drm_dir, const char *card_name, struct screen_card_info *out)
{
    if (!sysfs_drm_dir || !*sysfs_drm_dir || !drm_valid_card_name(card_name) || !out) return 0;
    char card_dir[PATH_MAX];
    int n = snprintf(card_dir, sizeof card_dir, "%s/%s", sysfs_drm_dir, card_name);
    if (n <= 0 || (size_t)n >= sizeof card_dir) return 0;
    struct stat st;
    if (stat(card_dir, &st) < 0 || !S_ISDIR(st.st_mode)) return 0;

    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", card_name);

    char link_path[PATH_MAX];
    if (snprintf(link_path, sizeof link_path, "%s/device/driver", card_dir) < (int)sizeof link_path) {
        drm_readlink_base(link_path, out->driver, sizeof out->driver);
    }
    if (snprintf(link_path, sizeof link_path, "%s/device", card_dir) < (int)sizeof link_path) {
        char resolved[PATH_MAX];
        if (realpath(link_path, resolved) != NULL) {
            const char *base = drm_basename(resolved);
            size_t blen = strlen(base);
            if (blen > 0 && blen < sizeof out->pci && strcmp(base, "device") != 0) {
                memcpy(out->pci, base, blen + 1);
            }
        }
        if (!out->pci[0]) {
            drm_readlink_base(link_path, out->pci, sizeof out->pci);
        }
    }

    DIR *d = opendir(sysfs_drm_dir);
    if (d) {
        char prefix[24];
        int plen = snprintf(prefix, sizeof prefix, "%s-", card_name);
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (plen <= 0 || strncmp(e->d_name, prefix, (size_t)plen) != 0) continue;
            char status_path[PATH_MAX];
            if (snprintf(status_path, sizeof status_path, "%s/%s/status", sysfs_drm_dir, e->d_name) >=
                (int)sizeof status_path) {
                continue;
            }
            FILE *f = fopen(status_path, "r");
            if (!f) continue;
            char line[32] = "";
            if (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (strcmp(line, "connected") == 0) out->connected_outputs++;
            }
            fclose(f);
        }
        closedir(d);
    }
    return 1;
}

int screen_list_cards_from(const char *sysfs_drm_dir, struct screen_card_info *out, int max)
{
    if (!sysfs_drm_dir || !*sysfs_drm_dir || !out || max <= 0) return 0;
    int count = 0;
    for (int n = 0; n < 8 && count < max; n++) {
        char name[16];
        snprintf(name, sizeof name, "card%d", n);
        if (drm_read_card_info(sysfs_drm_dir, name, &out[count])) {
            count++;
        }
    }
    return count;
}

int screen_list_cards(struct screen_card_info *out, int max)
{
    return screen_list_cards_from("/sys/class/drm", out, max);
}

int screen_card_matches_from(const char *sysfs_drm_dir, const char *card_name_or_path, const char *selector)
{
    char card_name[16];
    if (!drm_extract_card_name(card_name_or_path, card_name, sizeof card_name)) return 0;
    if (drm_selector_is_auto(selector)) return 1;
    if (!drm_selector_valid(selector)) return 0;
    if (strcmp(card_name, selector) == 0) return 1;
    struct screen_card_info info;
    if (!drm_read_card_info(sysfs_drm_dir, card_name, &info)) return 0;
    if (info.driver[0] && strcmp(info.driver, selector) == 0) return 1;
    if (info.pci[0] && strcmp(info.pci, selector) == 0) return 1;
    return 0;
}

int screen_card_matches(const char *card_name_or_path, const char *selector)
{
    return screen_card_matches_from("/sys/class/drm", card_name_or_path, selector);
}

int screen_select_card_path_from(const char *sysfs_drm_dir, const char *selector, char *out, size_t size)
{
    if (!out || size == 0 || drm_selector_is_auto(selector) || !drm_selector_valid(selector)) return -1;
    struct screen_card_info cards[8];
    int count = screen_list_cards_from(sysfs_drm_dir, cards, 8);
    int best = -1;
    for (int i = 0; i < count; i++) {
        int matched = strcmp(cards[i].name, selector) == 0 ||
                      (cards[i].driver[0] && strcmp(cards[i].driver, selector) == 0) ||
                      (cards[i].pci[0] && strcmp(cards[i].pci, selector) == 0);
        if (!matched) continue;
        if (best < 0 || (cards[best].connected_outputs == 0 && cards[i].connected_outputs > 0)) {
            best = i;
        }
    }
    if (best < 0) return -1;
    int n = snprintf(out, size, "/dev/dri/%s", cards[best].name);
    return (n > 0 && (size_t)n < size) ? 0 : -1;
}

int screen_select_card_path(const char *selector, char *out, size_t size)
{
    return screen_select_card_path_from("/sys/class/drm", selector, out, size);
}

static void on_rc_display_card(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "display_card") == 0 && value) {
        snprintf((char *)arg, 64, "%s", value);
    }
}

static void read_display_card_selector(char *out, size_t size)
{
    out[0] = '\0';
    const char *env = getenv("LOCACHY_DRM_CARD");
    if (env && *env) {
        snprintf(out, size, "%s", env);
        return;
    }
    char rc_val[64] = "";
    if (rc_read) {
        rc_read(on_rc_display_card, rc_val);
    }
    if (rc_val[0]) {
        snprintf(out, size, "%s", rc_val);
    }
}

/* Only cached sysfs fields: PCI vendor/device/class are cached in pci_dev;
 * connector status_show in drm_sysfs.c reports connector->status. */
static int drm_read_attr(const char *dir, const char *attr, char *out, size_t size)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s", dir, attr);
    if (n < 0 || (size_t)n >= sizeof path) return 0;
    FILE *f = fopen(path, "re");
    if (!f) return 0;
    int ok = fgets(out, size, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\n")] = '\0';
    return ok;
}

static int drm_pci_number(const char *dir, const char *attr, int base)
{
    char buf[24], *end;
    if (!drm_read_attr(dir, attr, buf, sizeof buf)) return -1;
    long n = strtol(buf, &end, base);
    return end != buf && !*end && n >= 0 && n <= INT_MAX ? (int)n : -1;
}

static int drm_amd_apu(const char *pci)
{
    /* PCI IDs flagged AMD_IS_APU in amdgpu_drv.c, for older chips which
     * may not publish IP discovery. Unknown IDs need positive GC evidence. */
    int id = drm_pci_number(pci, "device", 16);
    switch (id) {
    case 0x15dd: case 0x15d8: /* Raven, Picasso */
    case 0x15e7: case 0x1636: case 0x1638: case 0x164c: /* Renoir, Lucienne */
    case 0x164d: case 0x1681: /* Rembrandt */
        return 1;
    }
    /* Only GC versions marked unconditionally AMD_IS_APU by upstream
     * amdgpu_discovery.c. These sysfs show handlers read cached metadata. */
    static const unsigned int apu_gc[] = {
        0x090100, 0x090202, 0x090300, 0x0a0103, 0x0a0104, 0x0a0301,
        0x0a0303, 0x0a0306, 0x0a0307, 0x0b0001, 0x0b0004, 0x0b0500,
        0x0b0501, 0x0b0502, 0x0b0503, 0x0b0504, 0x0b0506, 0x0b0700, 0x0b0701,
    };
    int major_v = drm_pci_number(pci, "ip_discovery/die/0/GC/0/major", 10);
    int minor_v = drm_pci_number(pci, "ip_discovery/die/0/GC/0/minor", 10);
    int rev = drm_pci_number(pci, "ip_discovery/die/0/GC/0/revision", 10);
    if (major_v < 0 || major_v > 255 || minor_v < 0 || minor_v > 255 || rev < 0 || rev > 255) return 0;
    for (size_t i = 0; i < sizeof apu_gc / sizeof *apu_gc; i++) {
        if ((unsigned int)((major_v << 16) | (minor_v << 8) | rev) == apu_gc[i]) return 1;
    }
    return 0;
}

/* The canonical PCI device path contains a host-bridge component, e.g.
 * .../pci0000:00/0000:00:01.0/0000:01:00.0. */
static int drm_hex_field(const char *s, size_t len)
{
    return strspn(s, "0123456789abcdef") >= len;
}

static size_t drm_pci_root_len(const char *pci)
{
    const char *slot = drm_basename(pci);
    if (strlen(slot) != 12 || !drm_hex_field(slot, 4) || slot[4] != ':' ||
        !drm_hex_field(slot + 5, 2) || slot[7] != ':' || !drm_hex_field(slot + 8, 2) ||
        slot[10] != '.' || slot[11] < '0' || slot[11] > '7') return 0;
    for (const char *p = pci, *end; (end = strchr(p, '/')) != NULL; p = end + 1) {
        if (end - p == 10 && strncmp(p, "pci", 3) == 0 && drm_hex_field(p + 3, 4) &&
            p[7] == ':' && drm_hex_field(p + 8, 2) && memcmp(p + 3, slot, 4) == 0) {
            return (size_t)(end - pci);
        }
    }
    return 0;
}

static int drm_fixed_pci(const char *pci, size_t root_len)
{
    /* Linux does not mark every internal PCI dev fixed. In that case
     * require the endpoint itself to be described by platform firmware
     * (ACPI _ADR, as exported by pci-acpi.c and ACPI glue), not merely an
     * upstream bridge. Reject removable or ejectable ancestors as well. */
    char path[PATH_MAX], value[24];
    snprintf(path, sizeof path, "%s", pci);
    int fixed = drm_read_attr(path, "removable", value, sizeof value) && strcmp(value, "fixed") == 0;
    if (!fixed) {
        struct stat st;
        int n = snprintf(path, sizeof path, "%s/firmware_node/adr", pci);
        if (n < 0 || (size_t)n >= sizeof path || stat(path, &st) < 0) return 0;
        snprintf(path, sizeof path, "%s", pci);
    }
    while (strlen(path) > root_len) {
        if (drm_read_attr(path, "removable", value, sizeof value) && strcmp(value, "removable") == 0) return 0;
        char eject[PATH_MAX];
        int n = snprintf(eject, sizeof eject, "%s/firmware_node/eject", path);
        if (n < 0 || (size_t)n >= sizeof eject || access(eject, F_OK) == 0) return 0;
        *strrchr(path, '/') = '\0';
    }
    return 1;
}

static int drm_integrated_pci(const char *pci, size_t root_len, int vendor, const char *driver)
{
    const char *slot = drm_basename(pci);
    /* Intel integrated display is the host-bus IGD function, not an Arc
     * endpoint behind a PCIe bridge (the driver name alone is shared). */
    if (vendor == 0x8086 && (strcmp(driver, "i915") == 0 || strcmp(driver, "xe") == 0)) {
        return slot == pci + root_len + 1 && strcmp(slot + 5, "00:02.0") == 0;
    }
    return vendor == 0x1002 && strcmp(driver, "amdgpu") == 0 && drm_amd_apu(pci);
}

static int drm_panel_connected(const char *sysfs, const char *card_name)
{
    DIR *dir = opendir(sysfs);
    if (!dir) return 0;
    size_t len = strlen(card_name);
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, card_name, len) != 0 || e->d_name[len] != '-') continue;
        const char *conn = e->d_name + len + 1;
        if (strncmp(conn, "eDP-", 4) && strncmp(conn, "LVDS-", 5) && strncmp(conn, "DSI-", 4)) continue;
        char path[PATH_MAX], value[24];
        int n = snprintf(path, sizeof path, "%s/%s", sysfs, e->d_name);
        found = n > 0 && (size_t)n < sizeof path &&
                drm_read_attr(path, "status", value, sizeof value) && strcmp(value, "connected") == 0;
    }
    closedir(dir);
    return found;
}

/* Match the current character node to the sysfs object without opening it. */
static int drm_route_devnode(const char *sys_card, const char *dev_dri, const char *name,
                             char *out, size_t size)
{
    int n = snprintf(out, size, "%s/%s", dev_dri, name);
    struct stat st;
    char info[32], tail;
    unsigned int maj, min;
    return n > 0 && (size_t)n < size && lstat(out, &st) == 0 && S_ISCHR(st.st_mode) &&
           access(out, R_OK | W_OK) == 0 && drm_read_attr(sys_card, "dev", info, sizeof info) &&
           sscanf(info, "%u:%u%c", &maj, &min, &tail) == 2 && maj == 226 &&
           major(st.st_rdev) == maj && minor(st.st_rdev) == min;
}

int screen_hybrid_route_from(const char *sysfs_drm_dir, const char *dev_dri_dir,
                             struct screen_hybrid_route *out)
{
    if (!out) return 0;
    *out = (struct screen_hybrid_route){ 0 };
    if (!sysfs_drm_dir || !*sysfs_drm_dir || !dev_dri_dir || !*dev_dri_dir) return 0;
    DIR *dir = opendir(sysfs_drm_dir);
    if (!dir) return 0;
    struct screen_hybrid_route route = { 0 };
    char pci_panel[PATH_MAX] = "", pci_nvidia[PATH_MAX] = "";
    struct dirent *e;
    int count = 0;
    while ((e = readdir(dir)) != NULL) {
        const char *name = e->d_name;
        if (!drm_valid_card_name(name)) continue;
        /* An extra GPU must not disappear from Wayland's fixed list. */
        if (++count > 2) break;
        char sys_card[PATH_MAX], attr[PATH_MAX], pci[PATH_MAX], driver[32], bus[16];
        int n = snprintf(sys_card, sizeof sys_card, "%s/%s", sysfs_drm_dir, name);
        if (n < 0 || (size_t)n >= sizeof sys_card) break;
        n = snprintf(attr, sizeof attr, "%s/device", sys_card);
        if (n < 0 || (size_t)n >= sizeof attr || !realpath(attr, pci)) break;
        size_t root_len = drm_pci_root_len(pci);
        n = snprintf(attr, sizeof attr, "%s/subsystem", pci);
        if (!root_len || n < 0 || (size_t)n >= sizeof attr ||
            !drm_readlink_base(attr, bus, sizeof bus) || strcmp(bus, "pci") != 0) break;
        n = snprintf(attr, sizeof attr, "%s/driver", pci);
        if (n < 0 || (size_t)n >= sizeof attr || !drm_readlink_base(attr, driver, sizeof driver)) break;
        int vendor = drm_pci_number(pci, "vendor", 16);
        int class = drm_pci_number(pci, "class", 16);
        if (class < 0 || class >> 16 != 0x03) break;
        if (vendor == 0x10de && (strcmp(driver, "nvidia") == 0 || strcmp(driver, "nouveau") == 0) &&
            !pci_nvidia[0] && drm_fixed_pci(pci, root_len) &&
            drm_route_devnode(sys_card, dev_dri_dir, name, route.nvidia_card, sizeof route.nvidia_card)) {
            strcpy(pci_nvidia, pci);
        } else if (!pci_panel[0] && drm_integrated_pci(pci, root_len, vendor, driver) &&
                   drm_panel_connected(sysfs_drm_dir, name) &&
                   drm_route_devnode(sys_card, dev_dri_dir, name, route.panel_card, sizeof route.panel_card)) {
            strcpy(pci_panel, pci);
        } else {
            break;
        }
    }
    closedir(dir);
    size_t root_len = drm_pci_root_len(pci_panel);
    if (count != 2 || !root_len || !pci_nvidia[0] || root_len != drm_pci_root_len(pci_nvidia) ||
        strncmp(pci_panel, pci_nvidia, root_len) != 0 || strcmp(pci_panel, pci_nvidia) == 0) return 0;
    *out = route;
    return 1;
}

static void on_rc_gpu_graphics_policy(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "gpu_graphics_policy") == 0) {
        /* off (and any unknown value) leaves the old selection in place. */
        *(int *)arg = value && strcmp(value, "auto") == 0;
    }
}

int screen_hybrid_route(struct screen_hybrid_route *out)
{
    if (!out) return 0;
    *out = (struct screen_hybrid_route){ 0 };
    /* make.sh writes this only to the installed root. The same screen.c
     * is linked into PID 1 (before and after switch_root) and live tools. */
    struct stat st;
    if (stat("/usr/local/lib/locachy/level", &st) < 0 || !S_ISREG(st.st_mode) || !rc_read) return 0;
    char selector[64];
    read_display_card_selector(selector, sizeof selector);
    if (!drm_selector_is_auto(selector)) return 0;
    int enabled = 1;
    rc_read(on_rc_gpu_graphics_policy, &enabled);
    return enabled && screen_hybrid_route_from("/sys/class/drm", "/dev/dri", out);
}

static int drm_render_driver_allowed(const char *driver)
{
    return driver && (strcmp(driver, "i915") == 0 || strcmp(driver, "xe") == 0 ||
                      strcmp(driver, "amdgpu") == 0 || strcmp(driver, "radeon") == 0);
}

static int drm_render_node_in_dir(const char *drm_dir, const char *dev_dri_dir, char *path, size_t size)
{
    DIR *d = opendir(drm_dir);
    if (!d) return -1;
    int found = -1;
    char best[32] = "";
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "renderD", 7) != 0 || strlen(e->d_name) >= sizeof best) continue;
        char cand[PATH_MAX];
        int n = snprintf(cand, sizeof cand, "%s/%s", dev_dri_dir, e->d_name);
        if (n <= 0 || (size_t)n >= sizeof cand || access(cand, R_OK | W_OK) != 0) continue;
        if (!best[0] || strcmp(e->d_name, best) < 0) {
            snprintf(best, sizeof best, "%.31s", e->d_name);
        }
    }
    closedir(d);
    if (best[0]) {
        int n = snprintf(path, size, "%s/%s", dev_dri_dir, best);
        if (n > 0 && (size_t)n < size) found = 0;
    }
    return found;
}

static int drm_open_card_fd(const char *path, int want_master)
{
    double opened_at = screen_time();
    int fd = open(path, O_RDWR | O_CLOEXEC);
    double opening = screen_time() - opened_at;
    if (opening >= 0.1) {
        say("screen: %s: open %.0f ms (fd %d)", path, opening * 1000, fd);
    }
    if (fd < 0) return -1;
    if (want_master && ioctl(fd, DRM_IOCTL_SET_MASTER, 0) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* One try using cached connector state. A failed preference must not stop
 * the existing probe (nor turn it into a retry loop of its own). */
static int screen_try_hybrid_panel(const struct screen_hybrid_route *route, int want_master)
{
    int fd = drm_open_card_fd(route->panel_card, want_master);
    if (fd >= 0) {
        probe_connectors = -1;
        const char *error = prepare(fd);
        probe_connectors = 1;
        if (!error) error = show(fd);
        if (!error) {
            for (uint32_t i = 0; i < noutputs; i++) {
                if (outputs[i].internal) {
                    say("screen: on %s (hybrid graphics)", route->panel_card);
                    return 1;
                }
            }
        }
        clear_outputs(fd);
        card = -1;
        close(fd);
    }
    say("screen: hybrid panel unavailable, using legacy card search");
    return 0;
}

int screen_open(const char *font_path)
{
    active_bar.valid = 0;
    if (theme_reload) theme_reload();
    const char *why = NULL;
    snprintf(font_fallback, sizeof font_fallback, "%s", font_path);
#ifdef CONFIG_FREETYPE
    ft_init();
#endif

    /* An explicit window request is met or ends the program: it never falls
     * back to opening DRM or the input devices itself. */
    int backend = wl_backend_requested();
    if (backend != 0) {
#ifdef CONFIG_WAYLAND
        if (backend > 0) {
            if (wl_backend_open(window_resize, window_frame) == 0) {
                window_mode = 1;
                say("screen: a window of %ux%u pixels, scale %.2f, font %ux%u, %dx%d cells", width, height,
                    scale, glyph_w, glyph_h, cols, rows);
                return 1;
            }
            say("screen: LOCACHY_BACKEND=wayland, but no window from the compositor (WAYLAND_DISPLAY=%s)",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "unset");
            exit(WL_EXIT_UNAVAILABLE);
        }
#endif
        if (backend > 0) say("screen: LOCACHY_BACKEND=wayland, but this program is built without the window backend");
        else say("screen: LOCACHY_BACKEND=%s is not a backend (direct or wayland)", getenv("LOCACHY_BACKEND"));
        exit(WL_EXIT_CONFIG);
    }

    if (why == NULL && control_fd() >= 0) {
        control_wait_take(); /* screen_take does the rest */
        return shadow != NULL;
    }
    char selector[64] = "";
    read_display_card_selector(selector, sizeof selector);
    int explicit_card = !drm_selector_is_auto(selector);
    if (explicit_card && !drm_selector_valid(selector)) {
        say("screen: invalid display_card=%s; using auto", selector);
        explicit_card = 0;
    }
    /* Only an initial preference, not a replacement for the bounded legacy
     * search: cached panel state may be stale or this card may fail to show. */
    struct screen_hybrid_route route;
    if (drm_selector_is_auto(selector) && screen_hybrid_route(&route) && screen_try_hybrid_panel(&route, 0)) {
        return 1;
    }
    int fallback_logged = 0;
    for (int attempt = 0; why == NULL && attempt <= 50; attempt++) {
        int strict = explicit_card && attempt < 20;
        if (explicit_card && !strict && !fallback_logged) {
            say("screen: display_card=%s matched no connected output; falling back to auto", selector);
            fallback_logged = 1;
        }
        for (int n = 0; why == NULL && n < 8; n++) {
            char path[32];
            snprintf(path, sizeof path, "/dev/dri/card%d", n);
            if (strict && !screen_card_matches(path, selector)) {
                continue;
            }
            int fd = drm_open_card_fd(path, 0);
            if (fd < 0) {
                continue;
            }
            if (attempt == 50) {
                describe_card(fd, path);
            } else {
                double t0 = screen_time();
                const char *prepared = prepare(fd);
                double ready = screen_time();
                if (prepared == NULL) {
                    why = show(fd);
                    double done = screen_time();
                    if (ready - t0 >= 0.1 || done - ready >= 0.1) {
                        say("screen: %s: probe %.0f ms, modeset %.0f ms", path,
                            (ready - t0) * 1000, (done - ready) * 1000);
                    }
                    if (why == NULL) {
                        say("screen: on %s after %d tries", path, attempt + 1);
                        return 1;
                    }
                } else if (ready - t0 >= 0.1) {
                    say("screen: %s: probe %.0f ms: %s", path, (ready - t0) * 1000, prepared);
                }
            }
            close(fd);
        }
        if (why == NULL && attempt < 50) {
            usleep(100 * 1000);
        }
    }
    say("screen: none: %s", why != NULL ? why : "no card with a connected output and a readable font");
    return 0;
}

int screen_take(int lease)
{
    screen_release();
    active_bar.valid = 0;
    probe_connectors = 0;
    const char *why = prepare(lease);
    probe_connectors = 1;

#ifdef CONFIG_FREETYPE
    ft_init();
#endif
    if (why == NULL) {
        if (extended) {
            base_language_reload();
            if (theme_reload && theme_reload()) mark_all();
            for (uint32_t i = 0; i < noutputs; i++) {
                if (outputs[i].surface) outputs[i].surface->redraw = 0;
            }
        } else {
            screen_rescale();
        }
        why = show(lease);
    }
    if (why != NULL) {
        say("screen: cannot use the lease: %s", why);
        close(lease);
        return 0;
    }
    return 1;
}

/* Output power (session.h's base_screen_power: authorized, temporary, never
 * a brightness setting): the DPMS property of each connector this process
 * shows. Nothing here sets a mode, disables a CRTC or touches a framebuffer
 * or the backlight; an atomic driver marks the CRTC inactive, keeping its
 * mode and framebuffer, and active again on DPMS on, with no mode set from
 * here.
 *
 * What this process turned off it turns on again when it gives the screen
 * up: screen_release (process 1's "release", a new take) and exit. SIGKILL
 * leaves the outputs off: the kernel ends the lease without touching them
 * (drm_lease.c). The next mode set on them turns them on, as it sets the
 * CRTC active (atomic drivers then report DPMS on; the legacy CRTC helpers
 * make a full mode set when a connector's DPMS is not on), and process 1
 * makes one for the outputs it takes back (screen_take_back). */
#ifndef SCREEN_POWER_IOCTL
#define SCREEN_POWER_IOCTL ioctl /* tools/test-screen-power.c: a recording fake */
#endif
static int dark, dark_exit; /* DPMS off set by this process; the exit hook */
static pid_t dark_pid;      /* not a child forked since */

/* The id of the connector's DPMS property (0: it has none), by name from the
 * kernel's list of its properties. Not GETCONNECTOR: asked for no modes, it
 * probes the connector (an EDID read). -1: the list cannot be read. */
static int dpms_property(int fd, uint32_t connector, uint32_t *id, uint64_t *mode)
{
    *id = 0;
    for (int retry = 0; retry < 4; retry++) {
        struct drm_mode_obj_get_properties list = { .obj_id = connector,
                                                    .obj_type = DRM_MODE_OBJECT_CONNECTOR };
        if (SCREEN_POWER_IOCTL(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &list) < 0) return -1;
        uint32_t n = list.count_props;
        uint32_t *ids = array(n, sizeof *ids);
        uint64_t *values = array(n, sizeof *values);
        int ok = ids != NULL && values != NULL;
        if (ok) {
            list.props_ptr = (uintptr_t)ids;
            list.prop_values_ptr = (uintptr_t)values;
            ok = SCREEN_POWER_IOCTL(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &list) == 0;
        }
        for (uint32_t i = 0; ok && list.count_props <= n && i < list.count_props && !*id; i++) {
            struct drm_mode_get_property p = { .prop_id = ids[i] };
            if (SCREEN_POWER_IOCTL(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) == 0 &&
                strncmp(p.name, "DPMS", sizeof p.name) == 0) {
                *id = ids[i];
                if (mode) *mode = values[i];
            }
        }
        int error = errno;
        free(ids);
        free(values);
        errno = error;
        if (!ok) return -1;
        if (list.count_props <= n) return 0;
    }
    errno = EAGAIN; /* the list kept growing */
    return -1;
}

/* When changed is supplied, leave an already-dark connector alone. */
static int connector_power_change(int fd, const struct output *o, int on, int *changed)
{
    uint32_t prop;
    uint64_t mode = DRM_MODE_DPMS_ON;
    if (changed) *changed = 0;
    if (dpms_property(fd, o->conn_id, &prop, &mode) < 0) {
        say("screen: %s: cannot list its properties: %m", o->name);
    } else if (prop == 0) {
        say("screen: %s has no DPMS property", o->name);
    } else {
        if (changed && !on && mode != DRM_MODE_DPMS_ON) return 0;
        struct drm_mode_obj_set_property set = { .value = on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF,
            .prop_id = prop, .obj_id = o->conn_id, .obj_type = DRM_MODE_OBJECT_CONNECTOR };
        if (SCREEN_POWER_IOCTL(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &set) == 0) {
            if (changed) *changed = 1;
            return 0;
        }
        say("screen: %s: DPMS %s refused: %m", o->name, on ? "on" : "off");
    }
    return -1;
}

static int connector_power(int fd, const struct output *o, int on)
{
    return connector_power_change(fd, o, on, NULL);
}

int screen_idle_power(int on)
{
    if (window_mode || card < 0 || !noutputs) return -1;
    if (!on) {
        for (uint32_t i = 0; i < noutputs; i++) {
            int changed;
            if (connector_power_change(card, &outputs[i], 0, &changed) < 0) {
                screen_idle_power(1);
                return -1;
            }
            outputs[i].idle_off |= changed;
        }
        return 0;
    }
    int result = 0;
    for (uint32_t i = 0; i < noutputs; i++) {
        if (!outputs[i].idle_off) continue;
        if (connector_power(card, &outputs[i], 1) == 0) outputs[i].idle_off = 0;
        else result = -1;
    }
    return result;
}

/* Once: a lease about to end cannot be asked again. */
static void power_restore(void)
{
    if (!dark || dark_pid != getpid()) return;
    dark = 0;
    for (uint32_t i = 0; card >= 0 && i < noutputs; i++) connector_power(card, &outputs[i], 1);
}

int base_screen_power(int on)
{
    if (window_mode || card < 0 || noutputs == 0 || lent) return -1;
    if (!on && !dark_exit) {
        if (atexit(power_restore) != 0) return -1;
        dark_exit = 1;
    }
    int was_dark = dark;
    uint32_t i = 0;
    while (i < noutputs && connector_power(card, &outputs[i], on) == 0) {
        i++;
        if (!on) { dark = 1; dark_pid = getpid(); }
    }
    if (i == noutputs) {
        if (on) dark = 0;
        return 0;
    }
    /* Never half off: what this call turned off is turned on again. */
    if (!on && !was_dark) {
        int back = 1;
        while (i > 0) back &= connector_power(card, &outputs[--i], 1) == 0;
        if (back) dark = 0;
    }
    return -1;
}

void screen_release(void)
{
    hover_output = -1;
    power_restore(); /* while the connectors are still this process's */
    clear_outputs(card);
    if (card >= 0) {
        close(card); /* ends the lease; shadow remains for the next take */
        card = -1;
    }
    lent = 0;
    active_lessee = 0;
    presented_valid = 0;
    mirrored.presented_valid = 0;
    for (struct output_surface *s = surfaces; s; s = s->next) {
        s->presented_valid = 0;
    }
}

static int any_lent(void)
{
    for (uint32_t i = 0; i < noutputs; i++) {
        if (outputs[i].lessee) return 1;
    }
    return 0;
}

/* A lease of the outputs named (names NULL: all of them) not lent already. */
int screen_lend_outputs(const char *const *names, int n, uint32_t *lessee)
{
    *lessee = 0;
    if (card < 0 || noutputs == 0) {
        errno = ENODEV;
        return -1;
    }
    uint32_t *objects = array(noutputs, 2 * sizeof *objects), count = 0;
    uint32_t *which = array(noutputs, sizeof *which);
    if (objects == NULL || which == NULL) {
        free(objects);
        free(which);
        say("screen: no memory for lease objects");
        return -1;
    }
    for (uint32_t i = 0; i < noutputs; i++) {
        int wanted = names == NULL;
        for (int k = 0; !wanted && k < n; k++) wanted = strcmp(names[k], outputs[i].name) == 0;
        if (!wanted || outputs[i].lessee) continue;
        which[count / 2] = i;
        objects[count++] = outputs[i].conn_id;
        objects[count++] = outputs[i].crtc_id;
    }
    if (count == 0) {
        free(objects);
        free(which);
        errno = ENOENT;
        return -1;
    }
    for (int retry = 0; retry < 15; retry++) {
        if (ioctl(card, DRM_IOCTL_SET_MASTER, 0) == 0) {
            struct drm_mode_create_lease l = { .object_ids = (uintptr_t)objects,
                .object_count = count, .flags = O_CLOEXEC };
            int ok = ioctl(card, DRM_IOCTL_MODE_CREATE_LEASE, &l);
            int fallback = ok < 0 && count > 2;
            if (fallback) {
                l = (struct drm_mode_create_lease){ .object_ids = (uintptr_t)objects,
                    .object_count = 2, .flags = O_CLOEXEC };
                ok = ioctl(card, DRM_IOCTL_MODE_CREATE_LEASE, &l);
            }
            if (ok == 0) {
                lent = 1;
                active_lessee = l.lessee_id;
                *lessee = l.lessee_id;
                for (uint32_t k = 0; k < (fallback ? 1 : count / 2); k++) outputs[which[k]].lessee = l.lessee_id;
                if (fallback) {
                    say("screen: multi-output lease unavailable; lending connector %u only", objects[0]);
                }
                free(objects);
                free(which);
                return (int)l.fd;
            }
        }
        usleep(20000);
    }
    int error = errno;
    free(objects);
    free(which);
    errno = error;
    say("screen: cannot create a lease: %m");
    return -1;
}

/* All of them, the outputs rescanned first. */
int screen_lend(uint32_t *lessee)
{
    *lessee = 0;
    if (!any_lent() && (screen_refresh() <= 0 || noutputs == 0)) {
        return -1;
    }
    return screen_lend_outputs(NULL, 0, lessee);
}

int screen_places(struct screen_output *out, int max)
{
    struct monitor_rect rects[32];
    if (card < 0 || noutputs == 0 || noutputs > 32) return 0;
    for (uint32_t i = 0; i < noutputs; i++) {
        struct output *o = &outputs[i];
        struct monitor_conf conf;
        double s = monitor_conf(o->name, &conf) && conf.scale > 0 ? conf.scale : 1.0;
        o->logical_width = (int)(o->mode.hdisplay / s);
        o->logical_height = (int)(o->mode.vdisplay / s);
    }
    if (place_outputs(rects) != NULL) return 0;
    int n = 0;
    for (uint32_t i = 0; i < noutputs && n < max; i++, n++) {
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].name, sizeof out[n].name, "%s", outputs[i].name);
        out[n].x = rects[i].x;
        out[n].y = rects[i].y;
        out[n].width = rects[i].width;
        out[n].height = rects[i].height;
        out[n].pixel_width = outputs[i].mode.hdisplay;
        out[n].pixel_height = outputs[i].mode.vdisplay;
        out[n].internal = outputs[i].internal;
    }
    return n;
}

int screen_drop_master(uint32_t lessee)
{
    struct drm_mode_revoke_lease r = { .lessee_id = lessee };

    if (card < 0) {
        errno = ENODEV;
        return -1;
    }
    if (lessee != 0 && ioctl(card, DRM_IOCTL_MODE_REVOKE_LEASE, &r) < 0 &&
        errno != ENOENT && errno != EINVAL) {
        return -1;
    }
    for (uint32_t i = 0; i < noutputs; i++) {
        if (outputs[i].lessee == lessee) outputs[i].lessee = 0;
    }
    active_lessee = 0;
    for (int retry = 0; retry < 15; retry++) {
        if (ioctl(card, DRM_IOCTL_DROP_MASTER, 0) == 0) {
            return 0;
        }
        usleep(15000);
    }
    return -1;
}

int screen_take_back(uint32_t lessee)
{
    struct drm_mode_revoke_lease r = { .lessee_id = lessee };

    if (card < 0) {
        errno = ENODEV;
        return -1;
    }
    if (lessee != 0 && ioctl(card, DRM_IOCTL_MODE_REVOKE_LEASE, &r) < 0 &&
        errno != ENOENT && errno != EINVAL) {
        return -1;
    }
    /* Its outputs come back (lessee 0: those of a raw master handoff, all). */
    int back[32] = { 0 };
    for (uint32_t i = 0; i < noutputs && i < 32; i++) {
        back[i] = lessee == 0 ? !outputs[i].lessee : outputs[i].lessee == lessee;
        if (back[i]) outputs[i].lessee = 0;
    }
    active_lessee = 0;
    /* Revocation ended the lease even if raw master is returned later. A
     * timeout must not leave a stale lent bit preventing future retries. */
    lent = any_lent();
    int master = 0;
    for (int retry = 0; retry < 15; retry++) {
        if (ioctl(card, DRM_IOCTL_SET_MASTER, 0) == 0) {
            master = 1;
            break;
        }
        usleep(20000);
    }
    if (!master) {
        return -1;
    }
    int result = 0;
    for (uint32_t i = 0; i < noutputs; i++) {
        if (i < 32 && !back[i]) continue; /* still lent to another program */
        clear_dmabuf_fbs(card, &outputs[i], 0);
        if (set_crtc(card, &outputs[i]) < 0) {
            say("screen: master restored; connector %u needs rescan: %m", outputs[i].conn_id);
            result = 1;
        }
        if (extended && outputs[i].surface) {
            outputs[i].surface->presented_valid = 0;
            if (drawing_surface == outputs[i].surface) presented_valid = 0;
        }
    }
    if (!extended) presented_valid = 0;
    if (!lent) {
        mark_all();
        screen_flush();
    }
    return result;
}

/* Called by the owner only after reclaiming master and revoking its lease.
 * Keep the card open even with no connected outputs, for a later hotplug. */
int screen_refresh(void)
{
    if (active_lessee != 0 || any_lent()) {
        errno = EBUSY;
        return -1;
    }
    if (card < 0) {
        /* Booting without a connected monitor is recoverable: no earlier
         * scanout fd exists, so probe cards again on the hotplug event. */
        char selector[64] = "";
        read_display_card_selector(selector, sizeof selector);
        int explicit_card = !drm_selector_is_auto(selector);
        if (explicit_card && !drm_selector_valid(selector)) {
            say("screen: invalid display_card=%s; using auto", selector);
            explicit_card = 0;
        }
        struct screen_hybrid_route route;
        if (drm_selector_is_auto(selector) && screen_hybrid_route(&route) && screen_try_hybrid_panel(&route, 1)) {
            return 1;
        }
        int passes = explicit_card ? 2 : 1;
        for (int pass = 0; pass < passes; pass++) {
            int strict = explicit_card && pass == 0;
            if (explicit_card && pass == 1) {
                say("screen: display_card=%s matched no connected output; falling back to auto", selector);
            }
            for (int n = 0; n < 8; n++) {
                char path[32];
                snprintf(path, sizeof path, "/dev/dri/card%d", n);
                if (strict && !screen_card_matches(path, selector)) continue;
                int fd = drm_open_card_fd(path, 1);
                if (fd < 0) continue;
                if (prepare(fd) == NULL) {
                    const char *why = show(fd);
                    if (why == NULL) return 1;
                    say("screen: cannot enable reconnected %s: %s", path, why);
                }
                clear_outputs(fd);
                close(fd);
                card = -1;
            }
        }
        return 0;
    }
    if (ioctl(card, DRM_IOCTL_SET_MASTER, 0) < 0) {
        return -1;
    }
    lent = 0;
    int fd = card;
    clear_outputs(fd);
    const char *why = prepare(fd);
    if (why != NULL) {
        if (strcmp(why, "no connected output with a compatible CRTC") == 0) {
            return 0;
        }
        say("screen: cannot refresh outputs: %s", why);
        return -1;
    }
    why = show(fd);
    card = fd; /* show cleans failed buffers, but the owner retains its fd */
    if (why != NULL) {
        say("screen: cannot refresh outputs: %s", why);
        return -1;
    }
    return 1;
}

int screen_extended(int on)
{
    on = !!on;
    if (window_mode) return on ? -1 : 0; /* a window is one canvas */
    if (on == extended) return 0;
    for (uint32_t i = 0; i < noutputs; i++) soft_cursor_flush(&outputs[i], 0);
    if (on) {
        save_surface(&mirrored);
        const char *why = extended_layout();
        if (why) { say("screen: cannot extend outputs: %s", why); return -1; }
        extended = 1;
        selected_output = noutputs ? 0 : -1;
        drawing_surface = noutputs ? outputs[0].surface : NULL;
        if (drawing_surface) use_surface(drawing_surface);
    } else {
        save_drawing_surface();
        extended = 0;
        selected_output = -1;
        drawing_surface = NULL;
        use_surface(&mirrored);
    }
    mark_all();
    cursor_put(card);
    screen_flush();
    return 1;
}

int screen_is_extended(void)
{
    return extended;
}

int screen_output_current(void)
{
    if (window_mode) return 0;
    return noutputs ? extended ? selected_output : 0 : -1;
}

int screen_output_count(void)
{
    if (window_mode) return 1;
    return extended ? (int)noutputs : noutputs ? 1 : 0;
}

int screen_output_get(int index, struct screen_output *out)
{
    if (!out || index < 0 || index >= screen_output_count()) return -1;
    if (window_mode) {
        memset(out, 0, sizeof *out);
        snprintf(out->name, sizeof out->name, "window");
        out->width = out->pixel_width = (int)width;
        out->height = out->pixel_height = (int)height;
        out->scale = scale;
        out->cols = cols;
        out->rows = rows;
        return 0;
    }
    const struct output *o = &outputs[index];
    const struct output_surface *s = extended ? o->surface : NULL;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", o->name);
    out->x = extended ? o->x : 0; out->y = extended ? o->y : 0;
    out->width = extended ? o->logical_width : (int)width;
    out->height = extended ? o->logical_height : (int)height;
    out->pixel_width = s ? (int)s->width : (int)width;
    out->pixel_height = s ? (int)s->height : (int)height;
    out->scale = s ? s->scale : scale;
    out->cols = s ? s->cols : cols; out->rows = s ? s->rows : rows;
    return 0;
}

int screen_output_select(int index)
{
    if (index < 0 || index >= screen_output_count()) return -1;
    int previous = extended ? selected_output : 0;
    if (extended) {
        if (index == selected_output && drawing_surface == outputs[index].surface) {
            return previous;
        }
        save_drawing_surface();
        selected_output = index;
        drawing_surface = outputs[index].surface;
        use_surface(drawing_surface);
    }
    return previous;
}

int screen_output_cell_at(int output, int lx, int ly, int *col, int *row)
{
    if (!col || !row) return -1;
    if (!extended) {
        if (output != 0) return -1;
        screen_cell_at(lx, ly, col, row);
        return 0;
    }
    if (output < 0 || (uint32_t)output >= noutputs || !outputs[output].surface) return -1;
    const struct output_surface *s = outputs[output].surface;
    if (output == selected_output && drawing_surface == s) {
        *col = cell_index(lx, glyph_w);
        *row = cell_index((int64_t)ly - top, glyph_h);
    } else {
        *col = cell_index(lx, s->glyph_w);
        *row = cell_index((int64_t)ly - s->top, s->glyph_h);
    }
    return 0;
}

int screen_output_at(int x, int y, int *local_x, int *local_y)
{
    if (!extended) {
        if (x < 0 || y < 0 || x >= (int)width || y >= (int)height) return -1;
        if (local_x) *local_x = x;
        if (local_y) *local_y = y;
        return noutputs || window_mode ? 0 : -1;
    }
    for (uint32_t i = 0; i < noutputs; i++) {
        struct output *o = &outputs[i];
        if (x < o->x || y < o->y || (int64_t)x >= (int64_t)o->x + o->logical_width ||
            (int64_t)y >= (int64_t)o->y + o->logical_height) continue;
        if (local_x) *local_x = (int)((int64_t)(x - o->x) * o->surface->width / o->logical_width);
        if (local_y) *local_y = (int)((int64_t)(y - o->y) * o->surface->height / o->logical_height);
        return (int)i;
    }
    return -1;
}

void screen_output_project(int *x, int *y)
{
    if (!x || !y || screen_output_at(*x, *y, NULL, NULL) >= 0 || !noutputs) return;
    int64_t best = INT64_MAX;
    int nx = *x, ny = *y;
    for (uint32_t i = 0; i < (extended ? noutputs : 1); i++) {
        struct output *o = &outputs[i];
        int ox = extended ? o->x : 0, oy = extended ? o->y : 0;
        int ow = extended ? o->logical_width : (int)width, oh = extended ? o->logical_height : (int)height;
        int64_t right = (int64_t)ox + ow - 1, bottom = (int64_t)oy + oh - 1;
        int px = *x < ox ? ox : (int64_t)*x > right ? (int)right : *x;
        int py = *y < oy ? oy : (int64_t)*y > bottom ? (int)bottom : *y;
        int64_t dx = (int64_t)*x - px, dy = (int64_t)*y - py;
        uint64_t ax = (uint64_t)(dx < 0 ? -dx : dx), ay = (uint64_t)(dy < 0 ? -dy : dy);
        int64_t distance = ax > INT32_MAX || ay > INT32_MAX ? INT64_MAX : dx * dx + dy * dy;
        if (distance < best) { best = distance; nx = px; ny = py; }
    }
    *x = nx; *y = ny;
}

int screen_cols(void)
{
    return cols;
}

int screen_rows(void)
{
    return rows;
}

void screen_describe(void)
{
    say("screen: %ux%u canvas (%s), scale %.2f, font %ux%u, %dx%d cells, %u %s outputs",
        width, height, noutputs ? outputs[0].name : "-", scale, glyph_w, glyph_h, cols, rows, noutputs,
        extended ? "extended" : "mirrored");
}

int screen_rescale(void)
{
    base_language_reload(); /* settings changed the language, or anything else */
    active_bar.valid = 0;
    if (extended) {
        for (uint32_t i = 0; i < noutputs; i++) {
            if (outputs[i].surface) outputs[i].surface->bar.valid = 0;
        }
    }
    if (theme_reload && theme_reload()) mark_all();
    if (shadow == NULL || noutputs == 0) {
        return 0;
    }
    if (extended) {
        save_drawing_surface();
        if (extended_layout() != NULL) return 0;
        int changed = 0;
        for (uint32_t i = 0; i < noutputs; i++) {
            changed |= outputs[i].surface->redraw;
            outputs[i].surface->redraw = 0;
        }
        select_available_surface();
        if (changed) mark_all();
        return changed;
    }
    double s = output_scale();
    if (s > scale - 0.001 && s < scale + 0.001) {
        return 0;
    }
    if (scaled_font(s) != NULL) {
        return 0; /* the current font stays */
    }
    scale = s;
    layout();
    memset(shadow, 0, (size_t)width * height * sizeof *shadow);
    mark_all();
    cursor_put(card); /* its size follows the scale */
    return 1;
}

static void touch(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (dirty.x1 == dirty.x2) {
        dirty = (struct drm_clip_rect){ (unsigned short)x0, (unsigned short)y0,
                                        (unsigned short)x1, (unsigned short)y1 };
        return;
    }
    dirty.x1 = x0 < dirty.x1 ? (unsigned short)x0 : dirty.x1;
    dirty.y1 = y0 < dirty.y1 ? (unsigned short)y0 : dirty.y1;
    dirty.x2 = x1 > dirty.x2 ? (unsigned short)x1 : dirty.x2;
    dirty.y2 = y1 > dirty.y2 ? (unsigned short)y1 : dirty.y2;
}

/* Name physical damage to drivers that show a copy (simpledrm); direct
 * scanout drivers ignore DIRTYFB. Keep logical damage while lent/released. */
void screen_flush(void)
{
#ifdef CONFIG_WAYLAND
    if (window_mode) {
        /* Held back while the compositor shows the last one: drawn again
         * from the canvas when it is done (window_frame), changes kept.
         * The window shows the canvas below the hidden bar's rows. */
        if (shadow == NULL || dirty.x1 == dirty.x2 || height <= window_hidden) return;
        uint32_t vh = height - window_hidden;
        uint32_t wy0 = dirty.y1 > window_hidden ? dirty.y1 - window_hidden : 0;
        uint32_t wy1 = dirty.y2 > window_hidden ? dirty.y2 - window_hidden : 0;
        if (wy1 > vh) wy1 = vh;
        if (wy1 <= wy0) {
            dirty.x2 = dirty.x1; /* only the hidden rows changed */
            return;
        }
        const uint32_t *vis = shadow + (size_t)window_hidden * width;
        struct drm_clip_rect want = { dirty.x1, (unsigned short)wy0, dirty.x2, (unsigned short)wy1 };
        struct drm_clip_rect diff;
        if (!present_diff(vis, width, vh, want, presented, presented_w, presented_h, presented_valid, &diff)) {
            dirty.x2 = dirty.x1;
            return;
        }
        if (wl_backend_flush(vis, (int)width, (int)vh, diff.x1, diff.y1, diff.x2, diff.y2) == 0) {
            present_commit(vis, width, vh, diff, &presented, &presented_w, &presented_h, &presented_valid);
            dirty.x2 = dirty.x1;
        }
        return;
    }
#endif
    if (extended) {
        save_drawing_surface();
        if (card < 0 || !noutputs || lent) return;
        for (uint32_t i = 0; i < noutputs; i++) {
            struct output *o = &outputs[i];
            struct output_surface *s = o->surface;
            if (!o->map || !s || s->dirty.x1 == s->dirty.x2) continue;
            struct drm_clip_rect diff;
            if (!present_diff(s->pixels, s->width, s->height, s->dirty,
                              s->presented, s->presented_w, s->presented_h, s->presented_valid, &diff)) {
                s->dirty.x2 = s->dirty.x1;
                continue;
            }
            soft_cursor_flush(o, 0);
            struct drm_clip_rect clip = copy_picture(o->map, o->pitch, o->mode.hdisplay,
                o->mode.vdisplay, s->pixels, s->width, s->height, diff);
            if (clip.x1 < clip.x2 && clip.y1 < clip.y2) {
                struct drm_mode_fb_dirty_cmd cmd = { .fb_id = o->fb_id, .num_clips = 1,
                    .clips_ptr = (uintptr_t)&clip };
                ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &cmd);
            }
            present_commit(s->pixels, s->width, s->height, diff,
                           &s->presented, &s->presented_w, &s->presented_h, &s->presented_valid);
            s->dirty.x2 = s->dirty.x1;
            if (ndesk >= 2 && hover_output >= 0) hover_frame(o);
            soft_cursor_flush(o, 1);
        }
        if (drawing_surface) {
            dirty = drawing_surface->dirty;
            presented = drawing_surface->presented;
            presented_w = drawing_surface->presented_w;
            presented_h = drawing_surface->presented_h;
            presented_valid = drawing_surface->presented_valid;
        }
        return;
    }
    if (card < 0 || noutputs == 0 || shadow == NULL || lent || dirty.x1 == dirty.x2) {
        return;
    }
    if (noutputs == 1 && outputs[0].back_map != NULL && !outputs[0].soft_cursor && ndesk < 2) {
        struct output *o = &outputs[0];
        /* Wait briefly for an in-flight SHM flip before writing the back buffer,
         * so a synchronous TUI redraw (extdisplay returning to setup_loop) never
         * drops a frame and never writes into the live front buffer. */
        if (o->shm_flip_pending && screen_flip_complete() <= 0 && o->shm_flip_pending) {
            struct pollfd p = { .fd = card, .events = POLLIN };
            if (poll(&p, 1, 50) > 0 && (p.revents & POLLIN)) (void)screen_flip_complete();
        }
        if (o->shm_flip_pending) shm_settle_target(o);
        if (screen_present_shm_flip(shadow, width, height) >= 0) {
            dirty.x2 = dirty.x1;
            return;
        }
    }
    int dmabuf_active = 0;
    for (uint32_t i = 0; i < noutputs; i++) {
        if (has_any_dmabuf_fb(&outputs[i]) || outputs[i].shm_flip_pending) dmabuf_active = 1;
    }
    if (dmabuf_active) {
        presented_valid = 0;
    }
    struct drm_clip_rect diff;
    if (!present_diff(shadow, width, height, dirty,
                      presented, presented_w, presented_h, presented_valid, &diff)) {
        dirty.x2 = dirty.x1;
        return;
    }
    int flushed_any = 0;
    for (uint32_t i = 0; i < noutputs; i++) {
        struct output *o = &outputs[i];
        if (o->map == NULL) {
            continue;
        }
        if (o->shm_flip_pending) {
            /* Wait/drain or restore front FB before direct single-buffer TUI
             * drawing so screen_flush never writes into an in-flight flip. */
            (void)screen_flip_complete();
            if (o->shm_flip_pending) {
                o->shm_flip_pending = 0;
                (void)set_crtc(card, o);
            }
        }
        if (has_any_dmabuf_fb(o)) {
            clear_dmabuf_fbs(card, o, 1);
        }
        soft_cursor_flush(o, 0);
        struct drm_clip_rect clip = copy_picture(o->map, o->pitch,
            o->mode.hdisplay, o->mode.vdisplay, shadow, width, height, diff);
        if (clip.x1 < clip.x2 && clip.y1 < clip.y2) {
            struct drm_mode_fb_dirty_cmd cmd = { .fb_id = o->fb_id, .num_clips = 1,
                                                 .clips_ptr = (uintptr_t)&clip };
            ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &cmd);
        }
        if (ndesk >= 2 && hover_output >= 0) hover_frame(o);
        soft_cursor_flush(o, 1);
        flushed_any = 1;
    }
    if (flushed_any) {
        present_commit(shadow, width, height, diff,
                       &presented, &presented_w, &presented_h, &presented_valid);
    }
    dirty.x2 = dirty.x1;
}

/* The green frame around output o, when the pointer is on it and the desktop
 * has more screens than one: drawn into its scanout over the picture, as the
 * cursor is, after every copy of the picture there. */
#define HOVER_GREEN 0x30C850u
static uint32_t hover_thickness(const struct output *o)
{
    double s = extended && o->surface ? o->surface->scale : scale > 0 ? scale : 1.0;
    uint32_t w = o->mode.hdisplay, h = o->mode.vdisplay, t = (uint32_t)(3 * s + 0.5);
    if (t < 2) t = 2;
    return (2 * t < w && 2 * t < h) ? t : 0;
}

static void hover_frame(struct output *o)
{
    if (ndesk < 2 || hover_output < 0 || (uint32_t)hover_output >= noutputs || o != &outputs[hover_output] ||
        o->map == NULL || lent) {
        return;
    }
    uint32_t w = o->mode.hdisplay, h = o->mode.vdisplay, t = hover_thickness(o);
    if (t == 0) return;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *line = (uint32_t *)(o->map + (uint64_t)y * o->pitch);
        if (y < t || y >= h - t) {
            for (uint32_t x = 0; x < w; x++) line[x] = HOVER_GREEN;
            continue;
        }
        for (uint32_t x = 0; x < t; x++) line[x] = line[w - 1 - x] = HOVER_GREEN;
    }
    struct drm_clip_rect clips[4] = {
        { 0, 0, (unsigned short)w, (unsigned short)t },
        { 0, (unsigned short)(h - t), (unsigned short)w, (unsigned short)h },
        { 0, (unsigned short)t, (unsigned short)t, (unsigned short)(h - t) },
        { (unsigned short)(w - t), (unsigned short)t, (unsigned short)w, (unsigned short)(h - t) },
    };
    struct drm_mode_fb_dirty_cmd cmd = { .fb_id = o->fb_id, .num_clips = 4, .clips_ptr = (uintptr_t)clips };
    ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &cmd);
}

/* Restore only the 4 thin border strips of output i from its shadow buffer. */
static void hover_clear(int i)
{
    if (i < 0 || (uint32_t)i >= noutputs || card < 0 || lent) return;
    struct output *o = &outputs[i];
    if (o->map == NULL) return;
    uint32_t w = o->mode.hdisplay, h = o->mode.vdisplay, t = hover_thickness(o);
    if (t == 0) return;
    struct output_surface *s = extended ? o->surface : NULL;
    const uint32_t *src = s ? s->pixels : shadow;
    uint32_t sw = s ? s->width : width, sh = s ? s->height : height;
    if (src == NULL || sw == 0 || sh == 0) return;
    soft_cursor_flush(o, 0);
    struct drm_clip_rect clips[4] = {
        { 0, 0, (unsigned short)w, (unsigned short)t },
        { 0, (unsigned short)(h - t), (unsigned short)w, (unsigned short)h },
        { 0, (unsigned short)t, (unsigned short)t, (unsigned short)(h - t) },
        { (unsigned short)(w - t), (unsigned short)t, (unsigned short)w, (unsigned short)(h - t) },
    };
    if (w == sw && h == sh) {
        for (int k = 0; k < 4; k++) {
            copy_picture(o->map, o->pitch, w, h, src, sw, sh, clips[k]);
        }
        struct drm_mode_fb_dirty_cmd cmd = { .fb_id = o->fb_id, .num_clips = 4, .clips_ptr = (uintptr_t)clips };
        ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &cmd);
    } else {
        struct drm_clip_rect fit = fit_rect(sw, sh, w, h);
        uint32_t fw = fit.x2 - fit.x1, fh = fit.y2 - fit.y1;
        for (int k = 0; k < 4; k++) {
            for (uint32_t y = clips[k].y1; y < clips[k].y2; y++) {
                uint32_t *line = (uint32_t *)(o->map + (uint64_t)y * o->pitch);
                if (y < fit.y1 || y >= fit.y2) {
                    memset(line + clips[k].x1, 0, (size_t)(clips[k].x2 - clips[k].x1) * 4);
                } else {
                    const uint32_t *srow = src + (uint64_t)((uint64_t)(y - fit.y1) * sh / fh) * sw;
                    for (uint32_t x = clips[k].x1; x < clips[k].x2; x++) {
                        line[x] = (x >= fit.x1 && x < fit.x2) ? srow[(uint64_t)(x - fit.x1) * sw / fw] : 0;
                    }
                }
            }
        }
        struct drm_mode_fb_dirty_cmd cmd = { .fb_id = o->fb_id, .num_clips = 4, .clips_ptr = (uintptr_t)clips };
        ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &cmd);
    }
    soft_cursor_flush(o, 1);
}

void screen_hover(int output)
{
    if (output == hover_output) return;
    int was = hover_output;
    hover_output = output;
    if (ndesk < 2 || card < 0 || lent) return;
    if (was >= 0) hover_clear(was);
    if (output >= 0 && (uint32_t)output < noutputs) {
        struct output *o = &outputs[output];
        soft_cursor_flush(o, 0);
        hover_frame(o);
        soft_cursor_flush(o, 1);
    }
}

void screen_desk(char *const *fields, int n)
{
    ndesk = 0;
    for (int i = 0; i + 4 < n && ndesk < MAX_DESK; i += 5) {
        snprintf(desk[ndesk].name, sizeof desk[ndesk].name, "%s", fields[i]);
        desk[ndesk].x = atoi(fields[i + 1]);
        desk[ndesk].y = atoi(fields[i + 2]);
        desk[ndesk].w = atoi(fields[i + 3]);
        desk[ndesk].h = atoi(fields[i + 4]);
        if (desk[ndesk].w > 0 && desk[ndesk].h > 0) ndesk++;
    }
}

int screen_desk_count(void)
{
    return ndesk;
}

/* This program's picture, outside extended mode, is its first output's. */
static const struct desk *first_desk(void)
{
    for (int i = 0; noutputs && i < ndesk; i++) {
        if (strcmp(desk[i].name, outputs[0].name) == 0) return &desk[i];
    }
    return NULL;
}

int screen_to_desk(int x, int y, int *gx, int *gy)
{
    if (extended) {
        *gx = x;
        *gy = y;
        return 0;
    }
    const struct desk *d = first_desk();
    if (!d || !width || !height) return -1;
    *gx = d->x + (int)((int64_t)x * d->w / (int64_t)width);
    *gy = d->y + (int)((int64_t)y * d->h / (int64_t)height);
    return 0;
}

int screen_from_desk(int gx, int gy, int *x, int *y)
{
    if (extended) {
        *x = gx;
        *y = gy;
        return 0;
    }
    const struct desk *d = first_desk();
    if (!d) return -1;
    *x = (int)((int64_t)(gx - d->x) * (int64_t)width / d->w);
    *y = (int)((int64_t)(gy - d->y) * (int64_t)height / d->h);
    return 0;
}

int screen_desk_foreign(int gx, int gy)
{
    for (int i = 0; i < ndesk; i++) {
        const struct desk *d = &desk[i];
        if (gx < d->x || gy < d->y || (int64_t)gx >= (int64_t)d->x + d->w || (int64_t)gy >= (int64_t)d->y + d->h) {
            continue;
        }
        for (uint32_t k = 0; k < noutputs; k++) {
            if (strcmp(outputs[k].name, d->name) == 0) return 0;
        }
        return 1;
    }
    return 0;
}

void screen_fill(int col, int row, int ncols, int nrows, uint32_t rgb)
{
    if (shadow == NULL || col < 0 || row < 0 || col >= cols || row >= rows || ncols <= 0 || nrows <= 0) {
        return;
    }
    if (col + ncols > cols) {
        ncols = cols - col;
    }
    if (row + nrows > rows) {
        nrows = rows - row;
    }
    uint32_t x0 = (uint32_t)col * glyph_w, x1 = x0 + (uint32_t)ncols * glyph_w;
    uint32_t y0 = top + (uint32_t)row * glyph_h, y1 = y0 + (uint32_t)nrows * glyph_h;
    screen_fill_px((int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0), rgb);
}

/* A rectangle of pixels clipped to the picture, *sx and *sy what was cut
 * from its left and top; 0 if nothing is left. */
static int clip_px(int *x, int *y, int *w, int *h, int *sx, int *sy)
{
    *sx = *x < 0 ? -*x : 0;
    *sy = *y < 0 ? -*y : 0;
    int x0 = *x + *sx, y0 = *y + *sy;
    int x1 = *x + *w > (int)width ? (int)width : *x + *w;
    int y1 = *y + *h > (int)height ? (int)height : *y + *h;
    if (shadow == NULL || x0 >= x1 || y0 >= y1) {
        return 0;
    }
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return 1;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O3")))
#endif
void screen_blit(int x, int y, int w, int h, const uint32_t *px, int stride)
{
    int sx, sy;
    if (px == NULL || stride < w || !clip_px(&x, &y, &w, &h, &sx, &sy)) {
        return;
    }
    if (y < (int)bar_h) active_bar.valid = 0;
    touch((uint32_t)x, (uint32_t)y, (uint32_t)(x + w), (uint32_t)(y + h));
#if defined(__AVX2__)
    const __m256i mask = _mm256_set1_epi32(0x00FFFFFF);
#elif defined(__SSE2__)
    const __m128i mask = _mm_set1_epi32(0x00FFFFFF);
#endif
    for (int r = 0; r < h; r++) {
        const uint32_t *from = px + (uint64_t)(sy + r) * (uint64_t)stride + sx;
        uint32_t *to = shadow + (uint64_t)(y + r) * width + (uint64_t)x;
        int c = 0;
#if defined(__AVX2__)
        for (; c + 8 <= w; c += 8) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(from + c));
            _mm256_storeu_si256((__m256i *)(to + c), _mm256_and_si256(v, mask));
        }
#elif defined(__SSE2__)
        for (; c + 4 <= w; c += 4) {
            __m128i v = _mm_loadu_si128((const __m128i *)(from + c));
            _mm_storeu_si128((__m128i *)(to + c), _mm_and_si128(v, mask));
        }
#endif
        for (; c < w; c++) {
            to[c] = from[c] & 0xFFFFFF;
        }
    }
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O3")))
#endif
void screen_fill_px(int x, int y, int w, int h, uint32_t rgb)
{
    int sx, sy;
    if (!clip_px(&x, &y, &w, &h, &sx, &sy)) {
        return;
    }
    if (y < (int)bar_h) active_bar.valid = 0;
    touch((uint32_t)x, (uint32_t)y, (uint32_t)(x + w), (uint32_t)(y + h));
    rgb &= 0xFFFFFF;
#if defined(__AVX2__)
    const __m256i fill = _mm256_set1_epi32((int)rgb);
#elif defined(__SSE2__)
    const __m128i fill = _mm_set1_epi32((int)rgb);
#endif
    for (int r = 0; r < h; r++) {
        uint32_t *to = shadow + (uint64_t)(y + r) * width + (uint64_t)x;
        int c = 0;
#if defined(__AVX2__)
        for (; c + 8 <= w; c += 8) {
            _mm256_storeu_si256((__m256i *)(to + c), fill);
        }
#elif defined(__SSE2__)
        for (; c + 4 <= w; c += 4) {
            _mm_storeu_si128((__m128i *)(to + c), fill);
        }
#endif
        for (; c < w; c++) {
            to[c] = rgb;
        }
    }
}

int screen_render_node(char *path, size_t size)
{
    struct stat st;
    if (!path || size == 0 || window_mode || card < 0 || fstat(card, &st) < 0) return -1;
    char drvlink[128], drv[64];
    snprintf(drvlink, sizeof drvlink, "/sys/dev/char/%u:%u/device/driver",
             (unsigned)major(st.st_rdev), (unsigned)minor(st.st_rdev));
    if (!drm_readlink_base(drvlink, drv, sizeof drv)) return -1;
    /* Only expose VA-API + DRM PRIME scanout on drivers verified to export
     * direct BGRX scanout surfaces cleanly; skip nouveau/nvidia-drm/simpledrm. */
    if (!drm_render_driver_allowed(drv)) return -1;
    char dir[128];
    snprintf(dir, sizeof dir, "/sys/dev/char/%u:%u/device/drm",
             (unsigned)major(st.st_rdev), (unsigned)minor(st.st_rdev));
    return drm_render_node_in_dir(dir, "/dev/dri", path, size);
}

int screen_render_node_for_from(const char *sysfs_drm_dir, const char *dev_dri_dir,
                                const char *selector, char *path, size_t size)
{
    if (!sysfs_drm_dir || !*sysfs_drm_dir || !dev_dri_dir || !*dev_dri_dir || !path || size == 0) {
        return -1;
    }
    if (!drm_selector_valid(selector)) return -1;
    struct screen_card_info cards[8];
    int count = screen_list_cards_from(sysfs_drm_dir, cards, 8);
    int is_auto = drm_selector_is_auto(selector);
    int best_connected = -1, best_any = -1;
    for (int i = 0; i < count; i++) {
        if (!is_auto) {
            int matched = strcmp(cards[i].name, selector) == 0 ||
                          (cards[i].driver[0] && strcmp(cards[i].driver, selector) == 0) ||
                          (cards[i].pci[0] && strcmp(cards[i].pci, selector) == 0);
            if (!matched) continue;
        }
        if (!drm_render_driver_allowed(cards[i].driver)) continue;
        char drm_dir[PATH_MAX], cand[PATH_MAX];
        if (snprintf(drm_dir, sizeof drm_dir, "%s/%s/device/drm", sysfs_drm_dir, cards[i].name) >=
            (int)sizeof drm_dir) {
            continue;
        }
        if (drm_render_node_in_dir(drm_dir, dev_dri_dir, cand, sizeof cand) != 0) continue;
        if (cards[i].connected_outputs > 0 && best_connected < 0) best_connected = i;
        if (best_any < 0) best_any = i;
    }
    int pick = best_connected >= 0 ? best_connected : best_any;
    if (pick < 0) return -1;
    char drm_dir[PATH_MAX];
    if (snprintf(drm_dir, sizeof drm_dir, "%s/%s/device/drm", sysfs_drm_dir, cards[pick].name) >=
        (int)sizeof drm_dir) {
        return -1;
    }
    return drm_render_node_in_dir(drm_dir, dev_dri_dir, path, size);
}

/* The open scanout card's name ("card0"), from its device number. */
static int open_card_name(char *out, size_t size)
{
    struct stat st;
    if (card < 0 || fstat(card, &st) < 0) return 0;
    char link[128];
    snprintf(link, sizeof link, "/sys/dev/char/%u:%u",
             (unsigned)major(st.st_rdev), (unsigned)minor(st.st_rdev));
    return drm_readlink_base(link, out, size);
}

int screen_render_node_for(const char *selector, char *path, size_t size)
{
    if (!path || size == 0 || window_mode) return -1;
    if (card >= 0) {
        /* A PRIME dma-buf imports only on the device it scans out from: with a
         * scanout card open, its own render node is the only one used. Another
         * selected card gives -1 (software decode), never a cross-device import. */
        char name[16];
        if (drm_selector_is_auto(selector) || (open_card_name(name, sizeof name) &&
                                               screen_card_matches(name, selector))) {
            return screen_render_node(path, size);
        }
        char matched[64];
        if (drm_selector_valid(selector) && screen_select_card_path(selector, matched, sizeof matched) != 0) {
            say("screen: %s matched no DRM card; using the scanout's render node", selector);
            return screen_render_node(path, size);
        }
        return -1;
    }
    if (drm_selector_is_auto(selector)) {
        return screen_render_node_for_from("/sys/class/drm", "/dev/dri", NULL, path, size);
    }
    if (!drm_selector_valid(selector)) return -1;
    return screen_render_node_for_from("/sys/class/drm", "/dev/dri", selector, path, size);
}

int screen_flip_fd(void)
{
    if (card < 0 || noutputs != 1 || lent || window_mode || extended) return -1;
    return (outputs[0].dmabuf_flip_pending || outputs[0].shm_flip_pending) ? card : -1;
}

int screen_flip_complete(void)
{
    if (card < 0 || noutputs != 1 || lent || window_mode || extended) return 0;
    struct output *o = &outputs[0];
    int flags = fcntl(card, F_GETFL);
    if (flags >= 0 && !(flags & O_NONBLOCK)) (void)fcntl(card, F_SETFL, flags | O_NONBLOCK);
    int completed = 0;
    unsigned char buf[1024];
    for (;;) {
        ssize_t n = read(card, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        if (n == 0) break;
        size_t pos = 0;
        while (pos + sizeof(struct drm_event) <= (size_t)n) {
            struct drm_event ev;
            memcpy(&ev, buf + pos, sizeof ev);
            if (ev.length < sizeof ev || pos + ev.length > (size_t)n) break;
            if (ev.type == DRM_EVENT_FLIP_COMPLETE && ev.length >= sizeof(struct drm_event_vblank)) {
                struct drm_event_vblank vb;
                memcpy(&vb, buf + pos, sizeof vb);
                if (o->dmabuf_flip_pending &&
                    (vb.user_data == 0 || vb.user_data == (uint64_t)o->dmabuf_pending_fb_id)) {
                    if (o->dmabuf_fb_id != 0 && o->dmabuf_fb_id != o->dmabuf_pending_fb_id &&
                        !is_cached_dmabuf_fb(o, o->dmabuf_fb_id)) {
                        ioctl(card, DRM_IOCTL_MODE_RMFB, &o->dmabuf_fb_id);
                    }
                    o->dmabuf_fb_id = o->dmabuf_pending_fb_id;
                    o->dmabuf_pending_fb_id = 0;
                    o->dmabuf_active_slot = o->dmabuf_pending_slot;
                    o->dmabuf_pending_slot = -1;
                    o->dmabuf_flip_pending = 0;
                    completed = 1;
                } else if (o->shm_flip_pending &&
                           (vb.user_data == 0 || vb.user_data == (uint64_t)o->back_fb_id)) {
                    /* The back dumb buffer is now actively scanning out; swap so
                     * map/fb_id always tracks the front buffer and back_* is safe
                     * to render the next frame into. */
                    swap_dumb_buffers(o);
                    o->shm_flip_pending = 0;
                    completed = 1;
                }
            }
            pos += ev.length;
        }
    }
    return completed;
}

void screen_flip_reset(void)
{
    if (card < 0 || noutputs != 1 || lent || window_mode || extended) return;
    struct output *o = &outputs[0];
    if (!o->dmabuf_flip_pending && !o->shm_flip_pending) return;
    if (screen_flip_complete() > 0) return;
    o->dmabuf_flip_pending = 0;
    o->dmabuf_pending_fb_id = 0;
    o->dmabuf_pending_slot = -1;
    if (o->shm_flip_pending) shm_settle_target(o);
}

int screen_present_dmabuf_planes(const struct screen_dmabuf_planes *p, uint32_t w, uint32_t h)
{
    if (!p || window_mode || extended) {
        errno = EINVAL;
        return -1;
    }
    int use_cache = p->generation != 0 && p->slot < SCREEN_DMABUF_SLOTS;
    if (p->num_planes > SCREEN_DMABUF_MAX_PLANES ||
        (p->num_planes == 0 && !use_cache) ||
        (p->generation != 0 && p->slot >= SCREEN_DMABUF_SLOTS)) {
        errno = EINVAL;
        return -1;
    }
    if (card < 0 || noutputs != 1 || lent) { errno = EAGAIN; return -1; }
    struct output *o = &outputs[0];
    if (o->mode.hdisplay != w || o->mode.vdisplay != h) { errno = EINVAL; return -1; }
    if (o->dmabuf_flip_pending || o->shm_flip_pending) {
        (void)screen_flip_complete();
        if (o->dmabuf_flip_pending || o->shm_flip_pending) { errno = EBUSY; return -1; }
    }
    uint32_t fb_id = 0;
    if (use_cache && p->num_planes == 0) {
        if (o->dmabuf_cache_gen != p->generation || o->dmabuf_cache_fb[p->slot] == 0) {
            errno = ENOENT;
            return -1;
        }
        fb_id = o->dmabuf_cache_fb[p->slot];
    } else {
        struct drm_mode_fb_cmd2 fb = {
            .width = w,
            .height = h,
            .pixel_format = p->fourcc,
        };
        uint32_t handles[SCREEN_DMABUF_MAX_PLANES] = { 0 };
        uint32_t unique_handles[SCREEN_DMABUF_MAX_PLANES] = { 0 };
        uint32_t nunique = 0;
        int use_modifiers = 0;
        for (uint32_t i = 0; i < p->num_planes; i++) {
            if (p->fds[i] < 0) { errno = EINVAL; goto fail_handles; }
            /* Deduplicate imports when multiple planes share the same dmabuf fd. */
            uint32_t hnd = 0;
            for (uint32_t k = 0; k < i; k++) {
                if (p->fds[k] == p->fds[i]) { hnd = handles[k]; break; }
            }
            if (hnd == 0) {
                struct drm_prime_handle prime = { .fd = (uint32_t)p->fds[i] };
                if (ioctl(card, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0) goto fail_handles;
                hnd = prime.handle;
                int seen = 0;
                for (uint32_t u = 0; u < nunique; u++) {
                    if (unique_handles[u] == hnd) { seen = 1; break; }
                }
                if (!seen) unique_handles[nunique++] = hnd;
            }
            handles[i] = hnd;
            fb.handles[i] = hnd;
            fb.pitches[i] = p->pitches[i];
            fb.offsets[i] = p->offsets[i];
            if (p->modifiers[i] != 0 && p->modifiers[i] != DRM_FORMAT_MOD_INVALID) use_modifiers = 1;
        }
        if (use_modifiers) {
            fb.flags = DRM_MODE_FB_MODIFIERS;
            for (uint32_t i = 0; i < p->num_planes; i++) fb.modifier[i] = p->modifiers[i];
        }
        int added = ioctl(card, DRM_IOCTL_MODE_ADDFB2, &fb);
        int add_err = errno;
        /* ADDFB2 keeps its own kernel reference on each GEM object; drop handles now. */
        for (uint32_t u = 0; u < nunique; u++) {
            struct drm_gem_close gc = { .handle = unique_handles[u] };
            ioctl(card, DRM_IOCTL_GEM_CLOSE, &gc);
        }
        if (added < 0 || fb.fb_id == 0) { errno = add_err ? add_err : EINVAL; return -1; }
        fb_id = fb.fb_id;
        if (use_cache) {
            if (o->dmabuf_cache_gen != p->generation) {
                for (int k = 0; k < SCREEN_DMABUF_SLOTS; k++) {
                    uint32_t old = o->dmabuf_cache_fb[k];
                    o->dmabuf_cache_fb[k] = 0;
                    if (old != 0 && old != o->dmabuf_fb_id && old != o->dmabuf_pending_fb_id) {
                        (void)ioctl(card, DRM_IOCTL_MODE_RMFB, &old);
                    }
                }
                o->dmabuf_cache_gen = p->generation;
            }
            uint32_t prev = o->dmabuf_cache_fb[p->slot];
            o->dmabuf_cache_fb[p->slot] = fb_id;
            if (prev != 0 && prev != fb_id && prev != o->dmabuf_fb_id && prev != o->dmabuf_pending_fb_id) {
                (void)ioctl(card, DRM_IOCTL_MODE_RMFB, &prev);
            }
        }
        goto flip_now;

    fail_handles: {
            int err = errno;
            for (uint32_t u = 0; u < nunique; u++) {
                struct drm_gem_close gc = { .handle = unique_handles[u] };
                ioctl(card, DRM_IOCTL_GEM_CLOSE, &gc);
            }
            errno = err;
            return -1;
        }
    }

flip_now:
    /* Non-blocking DRM page flip avoids blocking the main loop on vblank; the
     * previous FB is retired when DRM_EVENT_FLIP_COMPLETE arrives. */
    if (!o->dmabuf_flip_broken) {
        struct drm_mode_crtc_page_flip flip = {
            .crtc_id = o->crtc_id,
            .fb_id = fb_id,
            .flags = DRM_MODE_PAGE_FLIP_EVENT,
            .user_data = (uint64_t)fb_id,
        };
        if (ioctl(card, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0) {
            o->dmabuf_pending_fb_id = fb_id;
            o->dmabuf_pending_slot = use_cache ? (int)p->slot : -1;
            o->dmabuf_flip_pending = 1;
            presented_valid = 0;
            dirty.x2 = dirty.x1;
            return 1;
        }
        int flip_err = errno;
        if (flip_err == EBUSY) {
            if (!is_cached_dmabuf_fb(o, fb_id)) ioctl(card, DRM_IOCTL_MODE_RMFB, &fb_id);
            errno = EBUSY;
            return -1;
        }
        if (flip_err == EACCES || flip_err == EPERM) {
            if (!is_cached_dmabuf_fb(o, fb_id)) ioctl(card, DRM_IOCTL_MODE_RMFB, &fb_id);
            errno = flip_err;
            return -1;
        }
        /* EINVAL/ENOTSUP (e.g. driver cannot flip between dumb FB and tiled/modifier
         * FB on the first frame, or lacks async page flip): fall back to SETCRTC.
         * Only latch dmabuf_flip_broken if already scanning out a dmabuf FB, because
         * on Intel i915 transitioning from linear dumb FB -> Y-tiled dmabuf FB
         * requires one SETCRTC before subsequent same-modifier PAGE_FLIPs work! */
        if (o->dmabuf_fb_id != 0) {
            o->dmabuf_flip_broken = 1;
            say("screen: DRM page flip refused (%s); using synchronous SETCRTC", strerror(flip_err));
        }
    }
    struct drm_mode_crtc set = {
        .crtc_id = o->crtc_id,
        .fb_id = fb_id,
        .set_connectors_ptr = (uintptr_t)&o->conn_id,
        .count_connectors = 1,
        .mode = o->mode,
        .mode_valid = 1,
    };
    if (ioctl(card, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
        int set_err = errno;
        if (use_cache && o->dmabuf_cache_fb[p->slot] == fb_id) {
            o->dmabuf_cache_fb[p->slot] = 0;
        }
        if (fb_id != o->dmabuf_fb_id && fb_id != o->dmabuf_pending_fb_id && !is_cached_dmabuf_fb(o, fb_id)) {
            ioctl(card, DRM_IOCTL_MODE_RMFB, &fb_id);
        }
        errno = set_err;
        return -1;
    }
    if (o->dmabuf_fb_id != 0 && o->dmabuf_fb_id != fb_id && !is_cached_dmabuf_fb(o, o->dmabuf_fb_id)) {
        ioctl(card, DRM_IOCTL_MODE_RMFB, &o->dmabuf_fb_id);
    }
    o->dmabuf_fb_id = fb_id;
    o->dmabuf_active_slot = use_cache ? (int)p->slot : -1;
    o->dmabuf_pending_slot = -1;
    presented_valid = 0;
    dirty.x2 = dirty.x1;
    return 0;
}

int screen_present_dmabuf(int dmabuf_fd, uint32_t w, uint32_t h, uint32_t fourcc,
                          uint32_t pitch, uint32_t offset, uint64_t modifier)
{
    struct screen_dmabuf_planes p = {
        .fourcc = fourcc,
        .num_planes = 1,
        .pitches = { pitch },
        .offsets = { offset },
        .modifiers = { modifier },
        .fds = { dmabuf_fd, -1, -1, -1 },
    };
    int r = screen_present_dmabuf_planes(&p, w, h);
    return r < 0 ? -1 : 0;
}

/* Tear-free double-buffered CPU presentation for single-output mirrored mode.
 * Writes directly into the inactive back dumb buffer and flips via
 * DRM_IOCTL_MODE_PAGE_FLIP + DRM_MODE_PAGE_FLIP_EVENT, bypassing shadow,
 * present_diff, and present_commit: for a 60 Hz full-screen video stream,
 * full-frame memcmp + memcpy against a shadow snapshot costs ~29 MB/frame of
 * extra memory bandwidth at 2560x1440 with no partial-damage benefit, because
 * every back buffer must hold a complete frame before the CRTC scans it out.
 * Never writes into the currently scanned-out front buffer. */
int screen_present_shm_flip(const uint32_t *pixels, uint32_t w, uint32_t h)
{
    if (!pixels || w == 0 || h == 0 || window_mode || extended) {
        errno = EINVAL;
        return -1;
    }
    if (card < 0 || noutputs != 1 || lent) {
        errno = EAGAIN;
        return -1;
    }
    struct output *o = &outputs[0];
    if (o->map == NULL || o->fb_id == 0) {
        errno = ENOTSUP;
        return -1;
    }
    if (o->dmabuf_flip_pending || o->shm_flip_pending) {
        (void)screen_flip_complete();
        if (o->dmabuf_flip_pending || o->shm_flip_pending) {
            errno = EBUSY;
            return -1;
        }
    }
    if (ensure_back_buffer(card, o) < 0) {
        return -1;
    }
    /* Copy the full frame (aspect-preserving fit with black borders when sizes
     * differ, or direct row memcpy when 1:1) into the inactive back buffer. */
    struct drm_clip_rect full = { 0, 0, (unsigned short)w, (unsigned short)h };
    struct drm_clip_rect clip = copy_picture(o->back_map, o->back_pitch,
        o->mode.hdisplay, o->mode.vdisplay, pixels, w, h, full);
    if (clip.x1 < clip.x2 && clip.y1 < clip.y2) {
        struct drm_mode_fb_dirty_cmd cmd = {
            .fb_id = o->back_fb_id,
            .num_clips = 1,
            .clips_ptr = (uintptr_t)&clip,
        };
        (void)ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &cmd);
    }
    /* Leaving dma-buf scanout: transition onto the populated back dumb FB with
     * SETCRTC first so cached/active dma-buf FBs can be safely removed without
     * blanking the CRTC or racing with an in-flight flip off a dma-buf FB. */
    int leaving_dmabuf = has_any_dmabuf_fb(o);
    if (!leaving_dmabuf && !o->shm_flip_broken) {
        struct drm_mode_crtc_page_flip flip = {
            .crtc_id = o->crtc_id,
            .fb_id = o->back_fb_id,
            .flags = DRM_MODE_PAGE_FLIP_EVENT,
            .user_data = (uint64_t)o->back_fb_id,
        };
        if (ioctl(card, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0) {
            o->shm_flip_pending = 1;
            presented_valid = 0;
            dirty.x2 = dirty.x1;
            return 1;
        }
        int flip_err = errno;
        if (flip_err == EBUSY || flip_err == EACCES || flip_err == EPERM) {
            errno = flip_err;
            return -1;
        }
        o->shm_flip_broken = 1;
        say("screen: SHM DRM page flip refused (%s); using synchronous SETCRTC", strerror(flip_err));
    }
    struct drm_mode_crtc set = {
        .crtc_id = o->crtc_id,
        .fb_id = o->back_fb_id,
        .set_connectors_ptr = (uintptr_t)&o->conn_id,
        .count_connectors = 1,
        .mode = o->mode,
        .mode_valid = 1,
    };
    if (ioctl(card, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
        return -1;
    }
    swap_dumb_buffers(o);
    if (leaving_dmabuf) {
        clear_dmabuf_fbs(card, o, 0);
    }
    presented_valid = 0;
    dirty.x2 = dirty.x1;
    return 0;
}

void screen_cells(int *w, int *h, int *t)
{
    *w = (int)glyph_w;
    *h = (int)glyph_h;
    *t = (int)top;
}

static uint32_t glyph_of(uint32_t cp)
{
    if (cp == 0x2018 || cp == 0x2019) {
        cp = '\'';
    } else if (cp == 0x201C || cp == 0x201D) {
        cp = '"';
    } else if (cp >= 0x2010 && cp <= 0x2015) {
        cp = '-';
    } else if (cp == 0x00A0) {
        cp = ' ';
    }
    if (cp < 128) {
        return ascii_has[cp] ? (uint32_t)ascii_map[cp] : '?';
    }
    for (int i = 0; i < nutab; i++) {
        if (utab[i].cp == cp) {
            return utab[i].g;
        }
    }
    return nutab == 0 && cp < glyph_count ? cp : '?';
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O3")))
#endif
static void glyph_draw(int col, int row, uint32_t gi, uint32_t fg, uint32_t bg)
{
    const uint8_t *g = glyphs + (uint64_t)gi * glyph_bytes;
    uint32_t *line = shadow + ((uint64_t)top + (uint64_t)row * glyph_h) * width + (uint64_t)col * glyph_w;

    if (glyph_mag == 1) {
        for (uint32_t y = 0; y < glyph_h; y++, line += width) {
            const uint8_t *bits = g + y * row_bytes;
            for (uint32_t x = 0; x < glyph_w; x++) {
                line[x] = (bits[x >> 3] >> (7 - (x & 7))) & 1 ? fg : bg;
            }
        }
    } else if (glyph_mag == 2) {
        for (uint32_t y = 0; y < glyph_h; y++, line += width) {
            const uint8_t *bits = g + (y >> 1) * row_bytes;
            for (uint32_t x = 0; x < glyph_w; x++) {
                uint32_t fx = x >> 1;
                line[x] = (bits[fx >> 3] >> (7 - (fx & 7))) & 1 ? fg : bg;
            }
        }
    } else {
        for (uint32_t y = 0; y < glyph_h; y++, line += width) {
            const uint8_t *bits = g + (y / glyph_mag) * row_bytes;
            for (uint32_t x = 0; x < glyph_w; x++) {
                uint32_t fx = x / glyph_mag;
                line[x] = (bits[fx >> 3] >> (7 - (fx & 7))) & 1 ? fg : bg;
            }
        }
    }
}

/* Kept for standalone unit tests (test-screen-mirror.c) that call glyph() directly. */
__attribute__((unused))
static void glyph(int col, int row, uint32_t cp, uint32_t fg, uint32_t bg)
{
    uint32_t y0 = top + (uint32_t)row * glyph_h;
    if (y0 < bar_h) active_bar.valid = 0;
    touch((uint32_t)col * glyph_w, y0, (uint32_t)(col + 1) * glyph_w, y0 + glyph_h);
    glyph_draw(col, row, glyph_of(cp), fg, bg);
}

void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    if (shadow == NULL || row < 0 || row >= rows || s == NULL) {
        return;
    }
    uint32_t fg_rgb = fg & 0xFFFFFFu, bg_rgb = bg & 0xFFFFFFu;
    const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s);
    int span_start = -1, span_end = -1;
    for (; p < end && col < cols; col++) {
        uint32_t cp = utf8(&p, end);
        if (col < 0) {
            continue;
        }
#ifdef CONFIG_FREETYPE
        if (is_wide_cjk(cp) || cp >= 0x2500) {
            int wide = is_wide_cjk(cp);
            if (!wide || col + 1 < cols) {
                if (span_start < 0) span_start = col;
                draw_ft_glyph(col, row, cp, wide, fg_rgb, bg_rgb);
                if (wide) {
                    col++;
                }
                span_end = col + 1;
            }
            continue;
        }
        uint32_t g = glyph_of(cp);
        if (span_start < 0) span_start = col;
        if (g != '?' || cp == '?') {
            glyph_draw(col, row, g, fg_rgb, bg_rgb);
        } else {
            draw_ft_glyph(col, row, cp, 0, fg_rgb, bg_rgb);
        }
        span_end = col + 1;
#else
        if (span_start < 0) span_start = col;
        glyph_draw(col, row, glyph_of(cp), fg_rgb, bg_rgb);
        span_end = col + 1;
#endif
    }
    if (span_start >= 0 && span_end > span_start) {
        uint32_t y0 = top + (uint32_t)row * glyph_h;
        if (y0 < bar_h) active_bar.valid = 0;
        touch((uint32_t)span_start * glyph_w, y0, (uint32_t)span_end * glyph_w, y0 + glyph_h);
    }
}

int screen_char_cells(uint32_t cp)
{
#ifdef CONFIG_FREETYPE
    return is_wide_cjk(cp) ? 2 : 1;
#else
    (void)cp;
    return 1;
#endif
}

void screen_pixels(int *w, int *h)
{
    *w = (int)width;
    *h = (int)height;
}

static int cell_index(int64_t px, uint32_t step)
{
    if (!step) return 0;
    return (int)(px / step - (px % step < 0));
}

void screen_cell_at(int x, int y, int *col, int *row)
{
    *col = cell_index(x, glyph_w);
    *row = cell_index((int64_t)y - top, glyph_h);
}

void screen_bar(const char *title, const char *ime)
{
    unsigned long long ram;
    char sub[32] = "";
    int x0, x1;
    if (shadow == NULL || height < bar_h || window_mode) {
        return; /* a window: the desktop's bar shows all this */
    }
    int frozen = panel_containers(NULL, &ram);
    const char *tstr = title ? title : "";
    const char *istr = ime ? ime : "";
    struct status_fields sf;
    memset(&sf, 0, sizeof sf);
    int have_sf = 0;
    if (base_status_fields) {
        base_status_fields(&sf);
        have_sf = 1;
    }
    uint32_t t_bg = theme_rgb(THEME_PANEL, PANEL_BG & 0xFFFFFFu);
    uint32_t t_fg = theme_rgb(THEME_FG, 0xD0D0D0u);
    uint32_t t_acc = theme_rgb(THEME_ACCENT, 0x3A4C68u);
    if (active_bar.valid && have_sf &&
        active_bar.bar_w == width && active_bar.bar_h == bar_h &&
        active_bar.frozen == frozen &&
        active_bar.theme_bg == t_bg && active_bar.theme_fg == t_fg &&
        active_bar.theme_accent == t_acc &&
        strcmp(active_bar.title, tstr) == 0 &&
        strcmp(active_bar.ime, istr) == 0 &&
        memcmp(&active_bar.fields, &sf, sizeof sf) == 0) {
        return;
    }
    if (frozen) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(sub, sizeof sub, P_("%d frozen", "%d frozen", frozen), frozen);
    }
    panel_draw(shadow, (int)width, (int)bar_h, (int)width, title, sub, ime, 0, &x0, &x1);
    touch(0, 0, width, bar_h);
    if (have_sf && strlen(tstr) < sizeof active_bar.title && strlen(istr) < sizeof active_bar.ime) {
        active_bar.valid = 1;
        snprintf(active_bar.title, sizeof active_bar.title, "%s", tstr);
        snprintf(active_bar.ime, sizeof active_bar.ime, "%s", istr);
        active_bar.fields = sf;
        active_bar.frozen = frozen;
        active_bar.bar_w = width;
        active_bar.bar_h = bar_h;
        active_bar.theme_bg = t_bg;
        active_bar.theme_fg = t_fg;
        active_bar.theme_accent = t_acc;
    } else {
        active_bar.valid = 0;
    }
}

double screen_scale(void)
{
    return scale > 0 ? scale : 1.0;
}

/*
 * The cursor: one ARGB buffer on the card, the cursor plane of every output's
 * CRTC (a lease brings them along, as process 1 does not ask for universal
 * planes). Where a driver has none (simpledrm), it is blended into that
 * output's scanout and the picture put back under it when it moves. Its
 * image is the cursor theme's (the default theme, as on the shared display),
 * 24 pixels at scale 1; or a drawn arrow.
 */
#define CURSOR_MAX 128
static int cursor_shape = CURSOR_ARROW, cursor_on, cursor_x, cursor_y;
static uint32_t cursor_image[CURSOR_MAX * CURSOR_MAX]; /* premultiplied ARGB */
static int cursor_w, cursor_h, hot_x, hot_y, cursor_loaded = -1;
static uint32_t cursor_handle, cursor_side;
static uint32_t *cursor_map;
static uint64_t cursor_map_size;
static int cursor_put_on; /* on the outputs of card: to take off again */
static int cursor_output = -1; /* extended mode: output currently holding the cursor */

static const char *const arrow[] = {
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ", "X....X     ",
    "X.....X    ", "X......X   ", "X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ",
    "X.X X..X   ", "XX  X..X   ", "X    X..X  ", "     X..X  ", "      XX   ",
};
static const char *const beam[] = {
    "XXX XXX", "X..X..X", "XXX.XXX", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ",
    "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "XXX.XXX", "X..X..X", "XXX XXX",
};

static void drawn_cursor(int text)
{
    const char *const *rows_of = text ? beam : arrow;
    int n = text ? (int)(sizeof beam / sizeof beam[0]) : (int)(sizeof arrow / sizeof arrow[0]);
    int w = (int)strlen(rows_of[0]), k = (int)(screen_scale() * 1.4 + 0.5);
    if (k < 1) k = 1;
    if (w * k > CURSOR_MAX || n * k > CURSOR_MAX) k = CURSOR_MAX / (n > w ? n : w);
    cursor_w = w * k;
    cursor_h = n * k;
    hot_x = text ? cursor_w / 2 : 0;
    hot_y = text ? cursor_h / 2 : 0;
    for (int y = 0; y < cursor_h; y++) {
        for (int x = 0; x < cursor_w; x++) {
            char c = rows_of[y / k][x / k];
            cursor_image[y * cursor_w + x] = c == 'X' ? 0xFF000000u : c == '.' ? 0xFFFFFFFFu : 0;
        }
    }
}

/* The theme directory of the default cursor theme: default, or what it
 * inherits (default-cursors: Adwaita). */
static int theme_file(const char *name, char *path, size_t size)
{
    snprintf(path, size, "/usr/share/icons/default/cursors/%s", name);
    if (access(path, R_OK) == 0) return 1;
    FILE *f = fopen("/usr/share/icons/default/index.theme", "r");
    char line[256], theme[128] = "";
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, "Inherits=", 9) == 0) {
            sscanf(line + 9, "%127[^,\n ]", theme);
        }
    }
    if (f) fclose(f);
    if (!theme[0] || strchr(theme, '/')) return 0;
    snprintf(path, size, "/usr/share/icons/%s/cursors/%s", theme, name);
    return access(path, R_OK) == 0;
}

/* An Xcursor file's image of the nominal size nearest want. */
static int xcursor(const char *name, int want)
{
    char path[256];
    if (!theme_file(name, path, sizeof path)) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    off_t size = fd < 0 ? -1 : lseek(fd, 0, SEEK_END);
    uint8_t *buf = size >= 16 && size <= (4 << 20) ? malloc((size_t)size) : NULL;
    int ok = buf && pread(fd, buf, (size_t)size, 0) == size;
    if (fd >= 0) close(fd);
    uint32_t h[4], best = 0, best_size = 0;
    if (ok) memcpy(h, buf, sizeof h);
    if (!ok || h[0] != 0x72756358u || h[3] > 4096 || h[1] + (uint64_t)h[3] * 12 > (uint64_t)size) {
        free(buf);
        return 0;
    }
    for (uint32_t i = 0; i < h[3]; i++) {
        uint32_t toc[3];
        memcpy(toc, buf + h[1] + i * 12, sizeof toc);
        if (toc[0] != 0xFFFD0002u) continue; /* an image */
        if (!best || abs((int)toc[1] - want) < abs((int)best_size - want)) {
            best = toc[2];
            best_size = toc[1];
        }
    }
    uint32_t c[9];
    if (!best || best + sizeof c > (uint64_t)size) { free(buf); return 0; }
    memcpy(c, buf + best, sizeof c); /* header, type, size, version, w, h, xhot, yhot, delay */
    if (c[4] == 0 || c[5] == 0 || c[4] > CURSOR_MAX || c[5] > CURSOR_MAX || c[6] >= c[4] || c[7] >= c[5] ||
        best + c[0] + (uint64_t)c[4] * c[5] * 4 > (uint64_t)size) {
        free(buf);
        return 0;
    }
    cursor_w = (int)c[4];
    cursor_h = (int)c[5];
    hot_x = (int)c[6];
    hot_y = (int)c[7];
    memcpy(cursor_image, buf + best + c[0], (size_t)cursor_w * cursor_h * 4);
    free(buf);
    return 1;
}

static void load_cursor(void)
{
    int key = cursor_shape * 1000 + (int)(screen_scale() * 100);
    if (key == cursor_loaded) return;
    cursor_loaded = key;
    int want = (int)(24 * screen_scale() + 0.5), text = cursor_shape == CURSOR_TEXT;
    if (!xcursor(text ? "xterm" : "left_ptr", want) && !xcursor(text ? "text" : "default", want)) {
        drawn_cursor(text);
    }
}

/* Where the cursor's hot spot is on output o, in its pixels. */
static void cursor_place(const struct output *o, int *x, int *y)
{
    uint32_t dw = o->mode.hdisplay, dh = o->mode.vdisplay;
    if (extended) {
        *x = (int)((int64_t)(cursor_x - o->x) * dw / o->logical_width);
        *y = (int)((int64_t)(cursor_y - o->y) * dh / o->logical_height);
        return;
    }
    if (dw == width && dh == height) {
        *x = cursor_x;
        *y = cursor_y;
        return;
    }
    struct drm_clip_rect fit = fit_rect(width, height, dw, dh);
    *x = fit.x1 + (int)((int64_t)cursor_x * (fit.x2 - fit.x1) / (int64_t)width);
    *y = fit.y1 + (int)((int64_t)cursor_y * (fit.y2 - fit.y1) / (int64_t)height);
}

/* Draw (or only take away, draw 0) the cursor in o's scanout, reading
 * background pixels from cached RAM (never WC VRAM) and writing sequentially. */
static void soft_cursor_flush(struct output *o, int draw)
{
    if (!o->soft_cursor || o->map == NULL) return;
    struct drm_clip_rect *r = &o->cursor_at;
    uint32_t dw = o->mode.hdisplay, dh = o->mode.vdisplay;
    struct output_surface *surf = extended ? o->surface : NULL;
    const uint32_t *src = surf ? surf->pixels : shadow;
    uint32_t sw = surf ? surf->width : width, sh = surf ? surf->height : height;
    if (r->x1 != r->x2) {
        if (src != NULL && sw > 0 && sh > 0) {
            struct drm_clip_rect back = copy_picture(o->map, o->pitch, dw, dh, src, sw, sh, *r);
            if (back.x1 < back.x2 && back.y1 < back.y2) {
                struct drm_mode_fb_dirty_cmd cmd = { .fb_id = o->fb_id, .num_clips = 1, .clips_ptr = (uintptr_t)&back };
                ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &cmd);
            }
        }
        *r = (struct drm_clip_rect){ 0 };
    }
    if (!draw || !cursor_on || lent) return;
    if (extended) {
        if (screen_output_at(cursor_x, cursor_y, NULL, NULL) != (int)(o - outputs)) return;
        double saved_scale = scale;
        scale = o->surface->scale;
        load_cursor();
        scale = saved_scale;
    }
    int ox, oy;
    cursor_place(o, &ox, &oy);
    int x0 = ox - hot_x, y0 = oy - hot_y;
    int x1 = x0 < 0 ? 0 : x0, y1 = y0 < 0 ? 0 : y0;
    int x2 = x0 + cursor_w > (int)dw ? (int)dw : x0 + cursor_w;
    int y2 = y0 + cursor_h > (int)dh ? (int)dh : y0 + cursor_h;
    if (x1 >= x2 || y1 >= y2) return;

    int same_size = (dw == sw && dh == sh);
    struct drm_clip_rect fit = same_size ? (struct drm_clip_rect){ 0, 0, (unsigned short)dw, (unsigned short)dh }
                                         : (sw > 0 && sh > 0 ? fit_rect(sw, sh, dw, dh) : (struct drm_clip_rect){ 0 });
    uint32_t fw = fit.x2 - fit.x1, fh = fit.y2 - fit.y1;
    uint32_t hborder = (ndesk >= 2 && hover_output >= 0 && (uint32_t)hover_output < noutputs &&
                        o == &outputs[hover_output]) ? hover_thickness(o) : 0;
    uint32_t row_buf[CURSOR_MAX];
    int span = x2 - x1;
    for (int y = y1; y < y2; y++) {
        const uint32_t *srow = NULL;
        if (src != NULL && sw > 0 && sh > 0) {
            if (same_size) {
                srow = src + (uint64_t)y * sw;
            } else if ((uint32_t)y >= fit.y1 && (uint32_t)y < fit.y2 && fh > 0) {
                srow = src + (uint64_t)((uint64_t)((uint32_t)y - fit.y1) * sh / fh) * sw;
            }
        }
        int y_border = hborder > 0 && ((uint32_t)y < hborder || (uint32_t)y >= dh - hborder);
        const uint32_t *crow = cursor_image + (y - y0) * cursor_w + (x1 - x0);
        for (int i = 0; i < span; i++) {
            int x = x1 + i;
            uint32_t d;
            if (y_border || (hborder > 0 && ((uint32_t)x < hborder || (uint32_t)x >= dw - hborder))) {
                d = HOVER_GREEN;
            } else if (srow == NULL) {
                d = 0;
            } else if (same_size) {
                d = srow[x];
            } else if ((uint32_t)x >= fit.x1 && (uint32_t)x < fit.x2 && fw > 0) {
                d = srow[(uint64_t)((uint32_t)x - fit.x1) * sw / fw];
            } else {
                d = 0;
            }
            uint32_t sp = crow[i], a = sp >> 24;
            if (a == 0) {
                row_buf[i] = d;
            } else if (a == 255) {
                row_buf[i] = sp & 0xFFFFFFu;
            } else {
                uint32_t inv = 255u - a;
                uint32_t dr = ((d >> 16) & 0xFFu) * inv + 128u;
                uint32_t dg = ((d >> 8) & 0xFFu) * inv + 128u;
                uint32_t db = (d & 0xFFu) * inv + 128u;
                uint32_t r_ch = ((sp >> 16) & 0xFFu) + ((dr + (dr >> 8)) >> 8);
                uint32_t g_ch = ((sp >> 8) & 0xFFu) + ((dg + (dg >> 8)) >> 8);
                uint32_t b_ch = (sp & 0xFFu) + ((db + (db >> 8)) >> 8);
                if (r_ch > 255u) r_ch = 255u;
                if (g_ch > 255u) g_ch = 255u;
                if (b_ch > 255u) b_ch = 255u;
                row_buf[i] = (r_ch << 16) | (g_ch << 8) | b_ch;
            }
        }
        memcpy(o->map + (uint64_t)y * o->pitch + (uint64_t)x1 * 4u, row_buf, (size_t)span * 4);
    }
    /* Logical damage for the scaled copy is the whole frame to preserve borders. */
    *r = extended || same_size ? (struct drm_clip_rect){ (unsigned short)x1, (unsigned short)y1,
                                                         (unsigned short)x2, (unsigned short)y2 }
                               : (struct drm_clip_rect){ 0, 0, (unsigned short)width, (unsigned short)height };
    struct drm_clip_rect on = { (unsigned short)x1, (unsigned short)y1, (unsigned short)x2, (unsigned short)y2 };
    struct drm_mode_fb_dirty_cmd cmd = { .fb_id = o->fb_id, .num_clips = 1, .clips_ptr = (uintptr_t)&on };
    ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &cmd);
}

static void cursor_drop(int fd)
{
    for (uint32_t i = 0; i < noutputs; i++) {
        outputs[i].cursor_at = (struct drm_clip_rect){ 0 };
    }
    if (cursor_map != NULL) {
        munmap(cursor_map, (size_t)cursor_map_size);
        cursor_map = NULL;
    }
    if (cursor_handle != 0 && fd >= 0) {
        struct drm_mode_destroy_dumb d = { .handle = cursor_handle };
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    cursor_handle = 0;
    cursor_put_on = 0;
    cursor_output = -1;
}

static void cursor_prepare_bo(int fd)
{
    uint32_t side = cursor_w > 64 || cursor_h > 64 ? CURSOR_MAX : 64;
    if (cursor_handle == 0 || side != cursor_side) {
        cursor_drop(fd);
        struct drm_mode_create_dumb dumb = { .width = side, .height = side, .bpp = 32 };
        struct drm_mode_map_dumb m = { 0 };
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) == 0) {
            cursor_handle = dumb.handle;
            m.handle = dumb.handle;
            void *p = dumb.pitch == side * 4 && ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) == 0 ?
                      mmap(NULL, (size_t)dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset) :
                      MAP_FAILED;
            if (p != MAP_FAILED) {
                cursor_map = p;
                cursor_map_size = dumb.size;
                cursor_side = side;
            } else {
                cursor_drop(fd);
            }
        }
    }
    if (cursor_map != NULL) {
        memset(cursor_map, 0, (size_t)cursor_side * cursor_side * 4);
        for (int y = 0; y < cursor_h; y++) {
            memcpy(cursor_map + (size_t)y * cursor_side, cursor_image + y * cursor_w, (size_t)cursor_w * 4);
        }
    }
}

/* The cursor as it should be on every output of fd: its image, or none. */
static void cursor_put(int fd)
{
    if (fd < 0 || lent || noutputs == 0) return;
    if (!cursor_on || cursor_shape == CURSOR_NONE) {
        if (!cursor_put_on) return;
        cursor_put_on = 0;
        cursor_output = -1;
        for (uint32_t i = 0; i < noutputs; i++) {
            struct output *o = &outputs[i];
            struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = o->crtc_id };
            if (!o->soft_cursor) ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &c);
            soft_cursor_flush(o, 0);
        }
        return;
    }
    if (extended) {
        int target = screen_output_at(cursor_x, cursor_y, NULL, NULL);
        double saved_scale = scale;
        if (target >= 0 && (uint32_t)target < noutputs && outputs[target].surface) {
            scale = outputs[target].surface->scale;
        }
        load_cursor();
        scale = saved_scale;
        cursor_prepare_bo(fd);
        for (uint32_t i = 0; i < noutputs; i++) {
            struct output *o = &outputs[i];
            soft_cursor_flush(o, 0);
            if ((int)i == target) {
                int x, y;
                cursor_place(o, &x, &y);
                struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE,
                    .crtc_id = o->crtc_id, .x = x - hot_x, .y = y - hot_y, .width = cursor_side,
                    .height = cursor_side, .handle = cursor_handle, .hot_x = hot_x, .hot_y = hot_y };
                o->soft_cursor = cursor_map == NULL || ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &c) < 0;
                soft_cursor_flush(o, 1);
            } else {
                struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = o->crtc_id };
                o->soft_cursor = cursor_map == NULL || ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &c) < 0;
            }
        }
        cursor_output = target;
        cursor_put_on = 1;
        return;
    }
    load_cursor();
    cursor_prepare_bo(fd);
    for (uint32_t i = 0; i < noutputs; i++) {
        struct output *o = &outputs[i];
        int x, y;
        cursor_place(o, &x, &y);
        struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE,
            .crtc_id = o->crtc_id, .x = x - hot_x, .y = y - hot_y, .width = cursor_side,
            .height = cursor_side, .handle = cursor_handle, .hot_x = hot_x, .hot_y = hot_y };
        o->soft_cursor = cursor_map == NULL || ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &c) < 0;
        soft_cursor_flush(o, 1);
    }
    cursor_put_on = 1;
}

void screen_cursor_move(int x, int y)
{
    cursor_x = x;
    cursor_y = y;
    if (card < 0 || lent || !cursor_on || cursor_shape == CURSOR_NONE) return;
    if (extended) {
        int target = screen_output_at(cursor_x, cursor_y, NULL, NULL);
        if (target != cursor_output) {
            cursor_put(card);
            return;
        }
        if (target >= 0 && (uint32_t)target < noutputs) {
            struct output *o = &outputs[target];
            if (o->soft_cursor) {
                soft_cursor_flush(o, 1);
            } else {
                int ox, oy;
                cursor_place(o, &ox, &oy);
                struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_MOVE, .crtc_id = o->crtc_id,
                                              .x = ox - hot_x, .y = oy - hot_y };
                ioctl(card, DRM_IOCTL_MODE_CURSOR2, &c);
            }
        }
        return;
    }
    for (uint32_t i = 0; i < noutputs; i++) {
        struct output *o = &outputs[i];
        if (o->soft_cursor) {
            soft_cursor_flush(o, 1);
            continue;
        }
        int ox, oy;
        cursor_place(o, &ox, &oy);
        struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_MOVE, .crtc_id = o->crtc_id,
                                      .x = ox - hot_x, .y = oy - hot_y };
        ioctl(card, DRM_IOCTL_MODE_CURSOR2, &c);
    }
}

void screen_cursor_visible(int on)
{
    if (on == cursor_on) return;
    cursor_on = on;
    cursor_put(card);
}

void screen_cursor(int shape)
{
    if (shape == cursor_shape) return;
    cursor_shape = shape;
#ifdef CONFIG_WAYLAND
    if (window_mode) wl_backend_cursor(shape); /* the compositor draws a window's cursor */
#endif
    cursor_put(card);
}
