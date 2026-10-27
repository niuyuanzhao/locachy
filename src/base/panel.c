/*
 * The status bar, drawn the same way on the shared display (locachy-bar, a
 * layer surface per screen, at that screen's scale) and at the top of the
 * framebuffer programs (screen_bar): LOCachy and blocks of two rows. At the
 * left the title block (the container on the screen, or the program), memory
 * and CPU (dropped, in this order from the right, where the middle needs the
 * room); at the right the input method, volume, network, battery and clock.
 * What is between them is left to the caller (locachy-bar puts the window
 * list there).
 *
 * Fonts are Terminus (BASE_FONT_DIR/ter-uNN[nb].psf, 12 to 32 pixels high,
 * and above that one of half the size drawn twice as large), the largest
 * that fits, so that the bar is as sharp at 1.5 or 3 as at 1: PANEL_H (27)
 * pixels at scale 1 has rows of 12. Wide characters (CJK) come
 * from NotoSansCJK through FreeType at the same height; without FreeType
 * (process 1) they show as '?'.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef CONFIG_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SIZES_H
#endif

#include "base.h"

#define PSF2_MAGIC 0x864AB572u
#define FG    0xFFD0D0D0u
#define DIM   0xFF98A8C0u /* the second row */
#define LINE  0xFF3A4C68u /* between blocks */

struct panel_font {
    int size, bold, loaded, mag; /* w and h: drawn, each font pixel mag x mag */
    uint8_t *data;
    const uint8_t *glyphs;
    uint32_t count, w, h, bytes, row_bytes;
    struct { uint32_t cp, g; } utab[2048];
    int nutab;
#ifdef CONFIG_FREETYPE
    FT_Size ft_size;
    int ft_ready;
#endif
};

static const int sizes[] = { 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 56, 64 };
#define NSIZES ((int)(sizeof sizes / sizeof sizes[0]))
static struct panel_font fonts[2 * NSIZES]; /* normal and bold of each size */

#ifdef CONFIG_FREETYPE
static FT_Library ft_lib;
static FT_Face ft_face;
static int ft_tried;
#endif

static uint32_t decode_utf8(const uint8_t **p, const uint8_t *end)
{
    uint8_t b = *(*p)++;
    if (b < 0x80) return b;
    int n = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
    if (n == 0 || *p + n > end) return '?';
    uint32_t cp = b & (0x3Fu >> n);
    for (int i = 0; i < n; i++) {
        if (((*p)[i] & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | ((*p)[i] & 0x3F);
    }
    *p += n;
    return cp;
}

static int load_psf(struct panel_font *f, const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    off_t size = lseek(fd, 0, SEEK_END);
    uint32_t h[8];
    if (size < (off_t)sizeof h || (f->data = malloc((size_t)size)) == NULL ||
        pread(fd, f->data, (size_t)size, 0) != size) {
        close(fd);
        return -1;
    }
    close(fd);
    memcpy(h, f->data, sizeof h);
    if (h[0] != PSF2_MAGIC || h[7] == 0 || h[7] > 32 || h[6] == 0) return -1;
    f->glyphs = f->data + h[2];
    f->count = h[4];
    f->bytes = h[5];
    f->h = h[6];
    f->w = h[7];
    f->row_bytes = (f->w + 7) / 8;
    if (h[3] & 1) {
        const uint8_t *u = f->glyphs + (uint64_t)f->count * f->bytes;
        const uint8_t *end = f->data + size;
        for (uint32_t g = 0; g < f->count && u < end; g++) {
            while (u < end && *u != 0xFF) {
                if (*u == 0xFE) {
                    while (u < end && *u != 0xFF) u++;
                    break;
                }
                uint32_t cp = decode_utf8(&u, end);
                if (f->nutab < (int)(sizeof f->utab / sizeof f->utab[0])) {
                    f->utab[f->nutab].cp = cp;
                    f->utab[f->nutab++].g = g;
                }
            }
            if (u < end) u++;
        }
    }
    return 0;
}

/* The largest size that fits, bold if asked and there is one (Terminus has
 * none at 12), else the normal one. */
const struct panel_font *panel_font(int height, int bold)
{
    int i = NSIZES - 1;
    while (i > 0 && sizes[i] > height) {
        i--;
    }
    for (int k = 2 * i + (bold ? 1 : 0); k >= 0; k--) {
        i = k / 2;
        bold = k % 2;
        struct panel_font *f = &fonts[k];
        if (!f->loaded) {
            char path[256];
            f->loaded = 1;
            f->size = sizes[i];
            f->bold = bold;
            f->mag = sizes[i] > 32 ? 2 : 1;
            snprintf(path, sizeof path, BASE_FONT_DIR "/ter-u%02d%c.psf", sizes[i] / f->mag, bold ? 'b' : 'n');
            if (load_psf(f, path) < 0) {
                f->glyphs = NULL;
            } else {
                f->w *= (uint32_t)f->mag;
                f->h *= (uint32_t)f->mag;
            }
#ifdef CONFIG_FREETYPE
            if (!ft_tried) {
                ft_tried = 1;
                if (FT_Init_FreeType(&ft_lib) != 0 ||
                    FT_New_Face(ft_lib, "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 0, &ft_face) != 0) {
                    ft_face = NULL;
                }
            }
            if (f->glyphs && ft_face && FT_New_Size(ft_face, &f->ft_size) == 0 &&
                FT_Activate_Size(f->ft_size) == 0 &&
                FT_Set_Pixel_Sizes(ft_face, 0, f->h > 16 ? f->h - 2 : f->h - 1) == 0) {
                f->ft_ready = 1;
            }
#endif
        }
        if (f->glyphs) {
            return f;
        }
    }
    return NULL;
}

void panel_font_size(const struct panel_font *f, int *w, int *h)
{
    *w = f ? (int)f->w : 0;
    *h = f ? (int)f->h : 0;
}

static int is_wide(uint32_t cp)
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

static uint32_t psf_glyph_of(const struct panel_font *f, uint32_t cp)
{
    for (int i = 0; i < f->nutab; i++) {
        if (f->utab[i].cp == cp) return f->utab[i].g;
    }
    return cp < 128 ? cp : '?';
}

void panel_fill(uint32_t *px, int w, int h, int stride, int x0, int y0, int bw, int bh, uint32_t argb)
{
    for (int y = y0 < 0 ? 0 : y0; y < y0 + bh && y < h; y++) {
        for (int x = x0 < 0 ? 0 : x0; x < x0 + bw && x < w; x++) {
            px[(size_t)y * (size_t)stride + (size_t)x] = argb;
        }
    }
}

size_t panel_text_cut(const char *s, int width, const struct panel_font *f)
{
    const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s), *cut = p;
    int cells = f ? width / (int)f->w : 0, used = 0;
    while (p < end) {
        int w = is_wide(decode_utf8(&p, end)) ? 2 : 1;
        if (used + w > cells) {
            break;
        }
        used += w;
        cut = p;
    }
    return (size_t)(cut - (const uint8_t *)s);
}

void panel_text_fit(char *out, size_t size, const char *s, int width, const struct panel_font *f)
{
    snprintf(out, size, "%s", s);
    if (!f || panel_text_width(s, f) <= width) {
        return;
    }
    const char *more = psf_glyph_of(f, 0x2026) != '?' ? "\xe2\x80\xa6" : "..";
    int room = width - (more[0] == '.' ? 2 : 1) * (int)f->w;
    snprintf(out, size, "%.*s%s", (int)panel_text_cut(s, room, f), s, room > 0 ? more : "");
}

int panel_text_width(const char *s, const struct panel_font *f)
{
    int cells = 0;
    const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s);
    if (!f) return 0;
    while (p < end) {
        cells += is_wide(decode_utf8(&p, end)) ? 2 : 1;
    }
    return cells * (int)f->w;
}

void panel_text(uint32_t *px, int w, int h, int stride, int x, int y, const char *s,
                uint32_t fg, uint32_t bg, const struct panel_font *f)
{
    const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s);
    if (!f) return;
    while (p < end && x < w) {
        uint32_t cp = decode_utf8(&p, end);
        int wide = is_wide(cp);
        int cell_w = wide ? (int)f->w * 2 : (int)f->w;
        if (x < 0) {
            x += cell_w;
            continue;
        }
        panel_fill(px, w, h, stride, x, y, cell_w, (int)f->h, bg);
#ifdef CONFIG_FREETYPE
        if (wide && f->ft_ready && FT_Activate_Size(f->ft_size) == 0 &&
            FT_Load_Char(ft_face, cp, FT_LOAD_RENDER) == 0) {
            FT_Bitmap *bm = &ft_face->glyph->bitmap;
            int x_off = x + (cell_w - (int)bm->width) / 2;
            int y_off = y + (int)f->h - (int)f->h / 6 - ft_face->glyph->bitmap_top;
            uint32_t fg_r = (fg >> 16) & 0xFF, fg_g = (fg >> 8) & 0xFF, fg_b = fg & 0xFF;
            uint32_t bg_r = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF, bg_b = bg & 0xFF;
            for (int gy = 0; gy < (int)bm->rows; gy++) {
                int py = y_off + gy;
                if (py < 0 || py >= h) continue;
                for (int gx = 0; gx < (int)bm->width; gx++) {
                    int pxx = x_off + gx;
                    uint8_t a = bm->buffer[gy * bm->pitch + gx];
                    if (pxx < 0 || pxx >= w || a == 0) continue;
                    uint32_t r = (fg_r * a + bg_r * (255 - a)) / 255;
                    uint32_t g = (fg_g * a + bg_g * (255 - a)) / 255;
                    uint32_t b = (fg_b * a + bg_b * (255 - a)) / 255;
                    px[(size_t)py * (size_t)stride + (size_t)pxx] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
            x += cell_w;
            continue;
        }
#endif
        const uint8_t *g = f->glyphs + (uint64_t)psf_glyph_of(f, wide ? '?' : cp) * f->bytes;
        for (uint32_t gy = 0; gy < f->h && y + (int)gy < h; gy++) {
            if (y + (int)gy < 0) continue;
            const uint8_t *bits = g + (gy / (uint32_t)f->mag) * f->row_bytes;
            for (uint32_t gx = 0; gx < f->w && x + (int)gx < w; gx++) {
                uint32_t fx = gx / (uint32_t)f->mag;
                px[(size_t)(y + (int)gy) * (size_t)stride + (size_t)(x + (int)gx)] =
                    ((bits[fx / 8] >> (7 - fx % 8)) & 1) ? fg : bg;
            }
        }
        x += (int)f->w; /* a wide character without FreeType: one '?' */
    }
}

int panel_containers(const char *name, unsigned long long *ram)
{
    int frozen = 0;
    DIR *dir = opendir("/sys/fs/cgroup");
    struct dirent *e;
    *ram = 0;
    while (dir && (e = readdir(dir)) != NULL) {
        char path[320], text[128];
        if (strncmp(e->d_name, "app_", 4) != 0) {
            continue;
        }
        snprintf(path, sizeof path, "/sys/fs/cgroup/%.200s/cgroup.events", e->d_name);
        int in = open(path, O_RDONLY | O_CLOEXEC);
        ssize_t n = in >= 0 ? read(in, text, sizeof text - 1) : -1;
        if (in >= 0) close(in);
        text[n > 0 ? n : 0] = '\0';
        frozen += strstr(text, "populated 1") && strstr(text, "frozen 1");
        if (name && name[0] && strcmp(e->d_name + 4, name) == 0) {
            snprintf(path, sizeof path, "/sys/fs/cgroup/%.200s/memory.current", e->d_name);
            in = open(path, O_RDONLY | O_CLOEXEC);
            n = in >= 0 ? read(in, text, sizeof text - 1) : -1;
            if (in >= 0) close(in);
            text[n > 0 ? n : 0] = '\0';
            *ram = strtoull(text, NULL, 10);
        }
    }
    if (dir) closedir(dir);
    return frozen;
}

struct block {
    char top[96], bottom[96];
    int min; /* cells, so that it stays put as numbers change */
};

static int block_width(const struct block *b, const struct panel_font *f)
{
    int w = panel_text_width(b->top, f), wb = panel_text_width(b->bottom, f);
    w = wb > w ? wb : w;
    return w > b->min * (int)f->w ? w : b->min * (int)f->w;
}

static void set(struct block *b, const char *top, const char *bottom, int min)
{
    snprintf(b->top, sizeof b->top, "%s", top);
    snprintf(b->bottom, sizeof b->bottom, "%s", bottom);
    b->min = min;
}

void panel_draw(uint32_t *px, int w, int h, int stride, const char *title, const char *sub,
                const char *ime, int reserve, int *mid_x0, int *mid_x1)
{
    const struct panel_font *row = panel_font(h / 2, 0), *big = panel_font(h * 2 / 3, 1);
    int gap = h * 5 / 9, inset = h / 9;
    panel_fill(px, w, h, stride, 0, 0, w, h, PANEL_BG);
    *mid_x0 = *mid_x1 = 0;
    if (!big) {
        return;
    }
    int bw, bh;
    panel_font_size(big, &bw, &bh);
    panel_text(px, w, h, stride, bw, (h - bh) / 2, "LOCachy", FG, PANEL_BG, big);
    int lx = panel_text_width("LOCachy", big) + 2 * bw;
    *mid_x0 = *mid_x1 = lx;
    if (!row) {
        char line[200];
        base_status(ime, line, sizeof line);
        int x = w - panel_text_width(line, big) - 2 * bw;
        if (x > lx) {
            panel_text(px, w, h, stride, x, (h - bh) / 2, line, FG, PANEL_BG, big);
            *mid_x1 = x;
        }
        return;
    }
    int rw, rh;
    panel_font_size(row, &rw, &rh);
    int y0 = (h - 2 * rh) / 2;
    struct status_fields s;
    base_status_fields(&s);
    struct block left[3], right[6];
    int nright = 0;
    set(&left[0], title, sub, 12);
    set(&left[1], s.memory, s.swap, 14);
    set(&left[2], s.cpu, s.temp, 7); /* "CPU 99%" */
    if (ime && ime[0]) {
        set(&right[nright++], "IME", ime, 3);
    }
    set(&right[nright++], s.volume, s.mic, 9);
    if (s.bluetooth[0]) {
        set(&right[nright++], s.bluetooth, s.bt_device, 7);
    }
    set(&right[nright++], s.network, s.address, 14);
    set(&right[nright++], s.battery, s.power, 12);
    set(&right[nright++], s.time, s.date, 14);

    /* The right ones from the right edge; the left ones after LOCachy, as
     * many as fit before them. */
    int x = w - rw;
    for (int i = nright - 1; i >= 0; i--) {
        x -= block_width(&right[i], row);
        if (x < lx) {
            break;
        }
        panel_text(px, w, h, stride, x, y0, right[i].top, FG, PANEL_BG, row);
        panel_text(px, w, h, stride, x, y0 + rh, right[i].bottom, DIM, PANEL_BG, row);
        *mid_x1 = x;
        x -= gap;
        panel_fill(px, w, h, stride, x + gap / 2, inset, 1, h - 2 * inset, LINE);
    }
    for (int i = 0; i < 3; i++) {
        int width = block_width(&left[i], row);
        if (lx + width + gap > *mid_x1 - (i > 0 ? reserve : 0)) {
            break;
        }
        if (i > 0) {
            panel_fill(px, w, h, stride, lx - gap / 2, inset, 1, h - 2 * inset, LINE);
        }
        panel_text(px, w, h, stride, lx, y0, left[i].top, FG, PANEL_BG, row);
        panel_text(px, w, h, stride, lx, y0 + rh, left[i].bottom, DIM, PANEL_BG, row);
        lx += width + gap;
        *mid_x0 = lx;
    }
}
