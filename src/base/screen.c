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
 * Process 1 opens the first card with a connected output itself (it is the
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
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

#include "internal.h"
#include "monitor-layout.h"

#define PSF1_MAGIC 0x0436u     /* bytes 36 04 */
#define PSF2_MAGIC 0x864AB572u /* bytes 72 b5 4a 86 */

struct output_surface;
static uint32_t *shadow;             /* the current drawing target */
static uint32_t width, height;
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
    uint8_t *map;
    uint64_t map_size;
    uint32_t pitch, fb_id, handle;
    int soft_cursor;                 /* no cursor plane: drawn into the scanout */
    struct drm_clip_rect cursor_at;  /* where it was drawn there; empty if x1 == x2 */
    uint32_t lessee;                 /* process 1: the lease it is lent in; 0 if none */
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
static int cols, rows;
static uint32_t top;                 /* pixels above row 0: bar_h - glyph_h, row 0 within the bar */
static uint32_t bar_h = PANEL_H;     /* the top bar: one and a half text rows */
static double scale;                 /* the first output's; 0 until it is known */
static char font_fallback[256];      /* when no Terminus of the size can be read */

/* A connector keeps its canvas while its lease is released or it disconnects. */
struct output_surface {
    char name[32];
    uint32_t *pixels, width, height;
    double scale;
    int redraw;
    struct drm_clip_rect dirty;
    uint8_t *font;
    const uint8_t *glyphs;
    uint32_t glyph_count, glyph_w, glyph_h, glyph_bytes, row_bytes, glyph_mag;
    struct { uint32_t cp, g; } utab[1024];
    int nutab, cols, rows;
    uint32_t top, bar_h;
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
static void hover_frame(struct output *o);
static double screen_time(void);
static void cursor_put(int fd);
static void cursor_drop(int fd);
static void soft_cursor_flush(struct output *o, int draw);

#ifdef CONFIG_FREETYPE
static FT_Library ft_lib;
static FT_Face ft_face, ft_cjk; /* symbols (DejaVu Sans Mono), and CJK (Noto Sans CJK SC) */
static int ft_ready;

static void ft_size(void)
{
    FT_UInt px = glyph_h > 2 ? glyph_h - 2 : glyph_h;
    if (px == 0) {
        return;
    }
    if (ft_face) {
        FT_Set_Pixel_Sizes(ft_face, 0, px);
    }
    if (ft_cjk) {
        FT_Set_Pixel_Sizes(ft_cjk, 0, px);
    }
}

static void ft_init(void)
{
    if (ft_ready || FT_Init_FreeType(&ft_lib) != 0) {
        return;
    }
    if (FT_New_Face(ft_lib, "/usr/share/fonts/TTF/DejaVuSansMono.ttf", 0, &ft_face) != 0) {
        ft_face = NULL;
    }
    /* The collection's simplified Chinese face; its first face is Japanese. */
    const char *cjk = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";
    FT_Face f;
    if (FT_New_Face(ft_lib, cjk, 0, &f) == 0) {
        FT_Long n = f->num_faces;
        ft_cjk = f;
        for (FT_Long i = 1; i < n; i++) {
            if (FT_New_Face(ft_lib, cjk, i, &f) != 0) {
                continue;
            }
            if (f->family_name && strstr(f->family_name, " SC")) {
                FT_Done_Face(ft_cjk);
                ft_cjk = f;
                break;
            }
            FT_Done_Face(f);
        }
    }
    ft_ready = ft_face != NULL || ft_cjk != NULL;
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
    FT_Face f = !ft_ready ? NULL : ft_face && FT_Get_Char_Index(ft_face, cp) ? ft_face :
                ft_cjk && FT_Get_Char_Index(ft_cjk, cp) ? ft_cjk : NULL;
    screen_fill(col, row, wide ? 2 : 1, 1, bg);
    if (f == NULL || FT_Load_Char(f, cp, FT_LOAD_RENDER) != 0) {
        return;
    }
    FT_Bitmap *bm = &f->glyph->bitmap;
    int cell_w = wide ? (int)glyph_w * 2 : (int)glyph_w;
    int bx0 = col * (int)glyph_w;
    int by0 = (int)top + row * (int)glyph_h;

    int x_off = bx0 + (wide ? (cell_w - (int)bm->width) / 2 : f->glyph->bitmap_left);
    int baseline = by0 + (int)glyph_h - (int)glyph_h / 5;
    int y_off = baseline - f->glyph->bitmap_top;
    if (x_off < bx0) {
        x_off = bx0;
    }

    uint32_t fg_r = (fg >> 16) & 0xFF, fg_g = (fg >> 8) & 0xFF, fg_b = fg & 0xFF;
    uint32_t bg_r = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF, bg_b = bg & 0xFF;

    for (int y = 0; y < (int)bm->rows; y++) {
        int py = y_off + y;
        if (py < 0 || py >= (int)height) {
            continue;
        }
        for (int x = 0; x < (int)bm->width; x++) {
            int px = x_off + x;
            if (px < 0 || px >= (int)width) {
                continue;
            }
            uint8_t a = bm->buffer[y * bm->pitch + x];
            if (a == 0) {
                continue;
            }
            if (a == 255) {
                shadow[(uint64_t)py * width + (uint32_t)px] = fg & 0xFFFFFF;
            } else {
                uint32_t r = (fg_r * a + bg_r * (255 - a)) / 255;
                uint32_t g = (fg_g * a + bg_g * (255 - a)) / 255;
                uint32_t b = (fg_b * a + bg_b * (255 - a)) / 255;
                shadow[(uint64_t)py * width + (uint32_t)px] = (r << 16) | (g << 8) | b;
            }
        }
    }
    touch((uint32_t)bx0, (uint32_t)by0, (uint32_t)(bx0 + cell_w), (uint32_t)(by0 + glyph_h));
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
                }
            }
            if (u < end) {
                u++;
            }
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

static void layout(void)
{
    bar_h = glyph_h * 3 / 2;
    top = bar_h > glyph_h ? bar_h - glyph_h : 0;
    cols = (int)(width / glyph_w);
    rows = height > top ? (int)((height - top) / glyph_h) : 0;
#ifdef CONFIG_FREETYPE
    ft_size();
#endif
}

static void save_surface(struct output_surface *s)
{
    s->pixels = shadow;
    s->width = width; s->height = height; s->scale = scale; s->dirty = dirty;
    s->font = font; s->glyphs = glyphs;
    s->glyph_count = glyph_count; s->glyph_w = glyph_w; s->glyph_h = glyph_h;
    s->glyph_bytes = glyph_bytes; s->row_bytes = row_bytes; s->glyph_mag = glyph_mag;
    memcpy(s->utab, utab, sizeof utab);
    s->nutab = nutab; s->cols = cols; s->rows = rows; s->top = top; s->bar_h = bar_h;
}

static void use_surface(struct output_surface *s)
{
    shadow = s->pixels;
    width = s->width; height = s->height; scale = s->scale; dirty = s->dirty;
    font = s->font; glyphs = s->glyphs;
    glyph_count = s->glyph_count; glyph_w = s->glyph_w; glyph_h = s->glyph_h;
    glyph_bytes = s->glyph_bytes; row_bytes = s->row_bytes; glyph_mag = s->glyph_mag;
    memcpy(utab, s->utab, sizeof utab);
    nutab = s->nutab; cols = s->cols; rows = s->rows; top = s->top; bar_h = s->bar_h;
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
        free(surface->pixels); free(surface->font);
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

/* Each successful step owns its resources immediately, including on failure. */
static void destroy_output(int fd, struct output *o)
{
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
 * On a lease, GETENCODER filters possible_crtcs to the visible CRTC list. */
static int read_output(int fd, uint32_t id, struct output *o)
{
    for (int retry = 0; retry < 4; retry++) {
        struct drm_mode_get_connector c = { .connector_id = id };
        double t0 = screen_time();
        int got = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
        double elapsed = screen_time() - t0;
        if (elapsed >= 0.1) {
            say("screen: connector %u: initial query %.0f ms (result %d, state %u)",
                id, elapsed * 1000, got, c.connection);
        }
        if (got < 0 || c.connection != 1 || c.count_modes == 0) {
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
        *o = (struct output){ .conn_id = id };
        snprintf(o->name, sizeof o->name, "%s-%u", c.connector_type < NCONNECTOR_TYPES ?
                 connector_types[c.connector_type] : "Unknown", c.connector_type_id);
        o->internal = monitor_internal(o->name);
        struct monitor_conf conf;
        int configured = monitor_conf(o->name, &conf);
        for (uint32_t m = 0; m < c.count_modes; m++) {
            if (modes[m].hdisplay == 0 || modes[m].vdisplay == 0) {
                continue;
            }
            if (o->mode.hdisplay == 0 || (modes[m].type & DRM_MODE_TYPE_PREFERRED)) {
                o->mode = modes[m];
                if (modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                    break;
                }
            }
        }
        if (configured && conf.width > 0 && conf.height > 0) {
            for (uint32_t m = 0; m < c.count_modes; m++) {
                if (modes[m].hdisplay == conf.width && modes[m].vdisplay == conf.height) {
                    o->mode = modes[m];
                    break;
                }
            }
        }
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
                }
            }
        }
        free(modes);
        free(encs);
        return o->mode.hdisplay != 0 && o->possible_crtcs != 0;
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
    if (extended) {
        for (uint32_t i = 0; i < noutputs; i++) {
            struct output_surface *s = outputs[i].surface;
            if (s) s->dirty = (struct drm_clip_rect){ 0, 0, (unsigned short)s->width, (unsigned short)s->height };
        }
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

/* Pure memory copy: identical sizes keep dirty rows; scaled outputs redraw
 * the entire frame, including borders, on any logical damage. Pitch is bytes. */
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
    for (uint32_t y = 0; y < dh; y++) {
        uint32_t *line = (uint32_t *)(dst + (uint64_t)y * pitch);
        memset(line, 0, (size_t)dw * 4);
        if (y >= fit.y1 && y < fit.y2) {
            const uint32_t *s = src + (uint64_t)((uint64_t)(y - fit.y1) * sh / fh) * sw;
            for (uint32_t x = 0; x < fw; x++) {
                line[fit.x1 + x] = s[(uint64_t)x * sw / fw];
            }
        }
    }
    return (struct drm_clip_rect){ 0, 0, (unsigned short)dw, (unsigned short)dh };
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

/* Show the current picture on the outputs of this fd (native per-output
 * canvases in extended mode, one fitted canvas otherwise). */
static const char *show(int fd)
{
    const char *why = "no usable output buffers";
    uint32_t used = 0;
    struct drm_clip_rect full = { 0, 0, (unsigned short)width, (unsigned short)height };
    for (uint32_t i = 0; i < noutputs; i++) {
        struct output *o = &outputs[i];
        const char *error = create_output(fd, o);
        if (error == NULL) {
            /* Populate scanout and black borders before enabling its CRTC. */
            struct output_surface *s = extended ? o->surface : NULL;
            struct drm_clip_rect clip = s ? (struct drm_clip_rect){ 0, 0,
                (unsigned short)s->width, (unsigned short)s->height } : full;
            copy_picture(o->map, o->pitch, o->mode.hdisplay, o->mode.vdisplay,
                         s ? s->pixels : shadow, s ? s->width : width, s ? s->height : height, clip);
            if (set_crtc(fd, o) < 0) error = "DRM_IOCTL_MODE_SETCRTC failed";
        }
        if (error != NULL) {
            why = error;
            say("screen: connector %u unavailable: %s", o->conn_id, error);
            destroy_output(fd, o);
            continue;
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
    if (extended) select_available_surface();
    mark_all();
    screen_flush();
    cursor_put(fd);
    return NULL;
}

/* Refresh the outputs and match the drawing size to the first selected one.
 * A take is followed by K_SHOWN, so programs lay out and draw it again. */
static const char *prepare(int fd)
{
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

int screen_open(const char *font_path)
{
    const char *why = NULL;
    snprintf(font_fallback, sizeof font_fallback, "%s", font_path);
#ifdef CONFIG_FREETYPE
    ft_init();
#endif

    if (why == NULL && control_fd() >= 0) {
        control_wait_take(); /* screen_take does the rest */
        return shadow != NULL;
    }
    for (int attempt = 0; why == NULL && attempt <= 50; attempt++) {
        for (int n = 0; why == NULL && n < 8; n++) {
            char path[24];
            snprintf(path, sizeof path, "/dev/dri/card%d", n);
            double opened_at = screen_time();
            int fd = open(path, O_RDWR | O_CLOEXEC);
            double opening = screen_time() - opened_at;
            if (opening >= 0.1) {
                say("screen: %s: open %.0f ms (fd %d)", path, opening * 1000, fd);
            }
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
    const char *why = prepare(lease);

#ifdef CONFIG_FREETYPE
    ft_init();
#endif
    if (why == NULL) {
        screen_rescale();
        why = show(lease);
    }
    if (why != NULL) {
        say("screen: cannot use the lease: %s", why);
        close(lease);
        return 0;
    }
    return 1;
}

void screen_release(void)
{
    hover_output = -1;
    clear_outputs(card);
    if (card >= 0) {
        close(card); /* ends the lease; shadow remains for the next take */
        card = -1;
    }
    lent = 0;
    active_lessee = 0;
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
        if (set_crtc(card, &outputs[i]) < 0) {
            say("screen: cannot restore connector %u: %m", outputs[i].conn_id);
            result = -1;
        }
    }
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
        for (int n = 0; n < 8; n++) {
            char path[24];
            snprintf(path, sizeof path, "/dev/dri/card%d", n);
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) == 0 && prepare(fd) == NULL) {
                const char *why = show(fd);
                if (why == NULL) return 1;
                say("screen: cannot enable reconnected %s: %s", path, why);
            }
            clear_outputs(fd);
            close(fd);
            card = -1;
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
    return noutputs ? extended ? selected_output : 0 : -1;
}

int screen_output_count(void)
{
    return extended ? (int)noutputs : noutputs ? 1 : 0;
}

int screen_output_get(int index, struct screen_output *out)
{
    if (!out || index < 0 || index >= screen_output_count()) return -1;
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
        save_drawing_surface();
        selected_output = index;
        drawing_surface = outputs[index].surface;
        use_surface(drawing_surface);
    }
    return previous;
}

int screen_output_at(int x, int y, int *local_x, int *local_y)
{
    if (!extended) {
        if (x < 0 || y < 0 || x >= (int)width || y >= (int)height) return -1;
        if (local_x) *local_x = x;
        if (local_y) *local_y = y;
        return noutputs ? 0 : -1;
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
    if (extended) {
        save_drawing_surface();
        if (card < 0 || !noutputs || lent) return;
        for (uint32_t i = 0; i < noutputs; i++) {
            struct output *o = &outputs[i];
            struct output_surface *s = o->surface;
            if (!o->map || !s || s->dirty.x1 == s->dirty.x2) continue;
            soft_cursor_flush(o, 0);
            struct drm_clip_rect clip = copy_picture(o->map, o->pitch, o->mode.hdisplay,
                o->mode.vdisplay, s->pixels, s->width, s->height, s->dirty);
            struct drm_mode_fb_dirty_cmd cmd = { .fb_id = o->fb_id, .num_clips = 1,
                .clips_ptr = (uintptr_t)&clip };
            ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &cmd);
            s->dirty.x2 = s->dirty.x1;
            hover_frame(o);
            soft_cursor_flush(o, 1);
        }
        if (drawing_surface) dirty = drawing_surface->dirty;
        return;
    }
    if (card < 0 || noutputs == 0 || shadow == NULL || lent || dirty.x1 == dirty.x2) {
        return;
    }
    for (uint32_t i = 0; i < noutputs; i++) {
        struct output *o = &outputs[i];
        if (o->map == NULL) {
            continue;
        }
        soft_cursor_flush(o, 0);
        struct drm_clip_rect clip = copy_picture(o->map, o->pitch,
            o->mode.hdisplay, o->mode.vdisplay, shadow, width, height, dirty);
        struct drm_mode_fb_dirty_cmd cmd = { .fb_id = o->fb_id, .num_clips = 1,
                                             .clips_ptr = (uintptr_t)&clip };
        ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &cmd);
        hover_frame(o);
        soft_cursor_flush(o, 1);
    }
    dirty.x2 = dirty.x1;
}

/* The green frame around output o, when the pointer is on it and the desktop
 * has more screens than one: drawn into its scanout over the picture, as the
 * cursor is, after every copy of the picture there. */
#define HOVER_GREEN 0x30C850u
static void hover_frame(struct output *o)
{
    if (ndesk < 2 || hover_output < 0 || (uint32_t)hover_output >= noutputs || o != &outputs[hover_output] ||
        o->map == NULL || lent) {
        return;
    }
    double s = extended && o->surface ? o->surface->scale : scale > 0 ? scale : 1.0;
    uint32_t w = o->mode.hdisplay, h = o->mode.vdisplay, t = (uint32_t)(3 * s + 0.5);
    if (t < 2) t = 2;
    if (2 * t >= w || 2 * t >= h) return;
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

/* Output i shown again from its picture (the frame taken off, or put on). */
static void output_again(int i)
{
    if (i < 0 || (uint32_t)i >= noutputs) return;
    struct output_surface *s = extended ? outputs[i].surface : NULL;
    if (s) s->dirty = (struct drm_clip_rect){ 0, 0, (unsigned short)s->width, (unsigned short)s->height };
    else dirty = (struct drm_clip_rect){ 0, 0, (unsigned short)width, (unsigned short)height };
}

void screen_hover(int output)
{
    if (output == hover_output) return;
    int was = hover_output;
    hover_output = output;
    if (ndesk < 2) return;
    output_again(was);
    output_again(output);
    screen_flush();
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
    if (shadow == NULL || col < 0 || row < 0 || col >= cols || row >= rows) {
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
    touch(x0, y0, x1, y1);
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            shadow[(uint64_t)y * width + x] = rgb & 0xFFFFFF;
        }
    }
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

void screen_blit(int x, int y, int w, int h, const uint32_t *px, int stride)
{
    int sx, sy;
    if (px == NULL || stride < w || !clip_px(&x, &y, &w, &h, &sx, &sy)) {
        return;
    }
    touch((uint32_t)x, (uint32_t)y, (uint32_t)(x + w), (uint32_t)(y + h));
    for (int r = 0; r < h; r++) {
        const uint32_t *from = px + (uint64_t)(sy + r) * (uint64_t)stride + sx;
        uint32_t *to = shadow + (uint64_t)(y + r) * width + (uint64_t)x;
        for (int c = 0; c < w; c++) {
            to[c] = from[c] & 0xFFFFFF;
        }
    }
}

void screen_fill_px(int x, int y, int w, int h, uint32_t rgb)
{
    int sx, sy;
    if (!clip_px(&x, &y, &w, &h, &sx, &sy)) {
        return;
    }
    touch((uint32_t)x, (uint32_t)y, (uint32_t)(x + w), (uint32_t)(y + h));
    for (int r = 0; r < h; r++) {
        uint32_t *to = shadow + (uint64_t)(y + r) * width + (uint64_t)x;
        for (int c = 0; c < w; c++) {
            to[c] = rgb & 0xFFFFFF;
        }
    }
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
    for (int i = 0; i < nutab; i++) {
        if (utab[i].cp == cp) {
            return utab[i].g;
        }
    }
    return nutab == 0 && cp < glyph_count ? cp : '?';
}

static void glyph(int col, int row, uint32_t cp, uint32_t fg, uint32_t bg)
{
    const uint8_t *g = glyphs + (uint64_t)glyph_of(cp) * glyph_bytes;
    uint32_t *line = shadow + ((uint64_t)top + (uint64_t)row * glyph_h) * width + (uint64_t)col * glyph_w;

    touch((uint32_t)col * glyph_w, top + (uint32_t)row * glyph_h,
          (uint32_t)(col + 1) * glyph_w, top + (uint32_t)(row + 1) * glyph_h);
    for (uint32_t y = 0; y < glyph_h; y++, line += width) {
        const uint8_t *bits = g + (y / glyph_mag) * row_bytes;
        for (uint32_t x = 0; x < glyph_w; x++) {
            uint32_t fx = x / glyph_mag;
            line[x] = (bits[fx / 8] >> (7 - fx % 8)) & 1 ? fg : bg;
        }
    }
}

void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    if (shadow == NULL || row < 0 || row >= rows || s == NULL) {
        return;
    }
    const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s);
    for (; p < end && col < cols; col++) {
        uint32_t cp = utf8(&p, end);
        if (col < 0) {
            continue;
        }
#ifdef CONFIG_FREETYPE
        if (is_wide_cjk(cp) || cp >= 0x2500) {
            int wide = is_wide_cjk(cp);
            if (!wide || col + 1 < cols) {
                draw_ft_glyph(col, row, cp, wide, fg & 0xFFFFFF, bg & 0xFFFFFF);
                if (wide) {
                    col++;
                }
            }
            continue;
        }
        uint32_t g = glyph_of(cp);
        if (g != '?' || cp == '?') {
            glyph(col, row, cp, fg & 0xFFFFFF, bg & 0xFFFFFF);
        } else {
            draw_ft_glyph(col, row, cp, 0, fg & 0xFFFFFF, bg & 0xFFFFFF);
        }
#else
        glyph(col, row, cp, fg & 0xFFFFFF, bg & 0xFFFFFF);
#endif
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

void screen_cell_at(int x, int y, int *col, int *row)
{
    *col = glyph_w ? x / (int)glyph_w : 0;
    *row = glyph_h ? (y - (int)top) / (int)glyph_h : 0;
    if (y < (int)top) *row = -1;
}

void screen_bar(const char *title, const char *ime)
{
    unsigned long long ram;
    char sub[32] = "";
    int x0, x1;
    if (shadow == NULL || height < bar_h) {
        return;
    }
    int frozen = panel_containers(NULL, &ram);
    if (frozen) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(sub, sizeof sub, P_("%d frozen", "%d frozen", frozen), frozen);
    }
    panel_draw(shadow, (int)width, (int)bar_h, (int)width, title, sub, ime, 0, &x0, &x1);
    touch(0, 0, width, bar_h);
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

/* Draw (or only take away, draw 0) the cursor in o's scanout. */
static void soft_cursor_flush(struct output *o, int draw)
{
    if (!o->soft_cursor || o->map == NULL) return;
    struct drm_clip_rect *r = &o->cursor_at;
    uint32_t dw = o->mode.hdisplay, dh = o->mode.vdisplay;
    if (r->x1 != r->x2) {
        struct output_surface *s = extended ? o->surface : NULL;
        struct drm_clip_rect back = copy_picture(o->map, o->pitch, dw, dh,
            s ? s->pixels : shadow, s ? s->width : width, s ? s->height : height, *r);
        struct drm_mode_fb_dirty_cmd cmd = { .fb_id = o->fb_id, .num_clips = 1, .clips_ptr = (uintptr_t)&back };
        ioctl(card, DRM_IOCTL_MODE_DIRTYFB, &cmd);
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
    for (int y = y1; y < y2; y++) {
        uint32_t *line = (uint32_t *)(o->map + (uint64_t)y * o->pitch);
        for (int x = x1; x < x2; x++) {
            uint32_t s = cursor_image[(y - y0) * cursor_w + (x - x0)], a = s >> 24, d = line[x];
            if (a == 0) continue;
            uint32_t out = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                uint32_t v = ((s >> sh) & 0xFF) + ((d >> sh) & 0xFF) * (255 - a) / 255;
                out |= (v > 255 ? 255 : v) << sh;
            }
            line[x] = out;
        }
    }
    /* Logical damage for the scaled copy is the whole frame anyway. */
    *r = extended || (dw == width && dh == height) ? (struct drm_clip_rect){ (unsigned short)x1, (unsigned short)y1,
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
}

/* The cursor as it should be on every output of fd: its image, or none. */
static void cursor_put(int fd)
{
    if (fd < 0 || lent || noutputs == 0) return;
    if (extended) {
        for (uint32_t i = 0; i < noutputs; i++) soft_cursor_flush(&outputs[i], 0);
        cursor_drop(fd);
        for (uint32_t i = 0; i < noutputs; i++) {
            struct output *o = &outputs[i];
            struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = o->crtc_id };
            ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &c);
            o->soft_cursor = 1;
            soft_cursor_flush(o, cursor_on && cursor_shape != CURSOR_NONE);
        }
        cursor_put_on = cursor_on && cursor_shape != CURSOR_NONE;
        return;
    }
    if (!cursor_on || cursor_shape == CURSOR_NONE) {
        if (!cursor_put_on) return;
        cursor_put_on = 0;
        for (uint32_t i = 0; i < noutputs; i++) {
            struct output *o = &outputs[i];
            struct drm_mode_cursor2 c = { .flags = DRM_MODE_CURSOR_BO, .crtc_id = o->crtc_id };
            if (!o->soft_cursor) ioctl(fd, DRM_IOCTL_MODE_CURSOR2, &c);
            soft_cursor_flush(o, 0);
        }
        return;
    }
    load_cursor();
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
    cursor_put(card);
}
