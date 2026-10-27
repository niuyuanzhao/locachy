/* Independent, per-program content fonts; the system UI font is unchanged. */
#ifndef CONTENT_FONT_H
#define CONTENT_FONT_H

#include <stdint.h>

struct content_font;

enum {
    CONTENT_FONT_BOLD = 1u << 0,
    CONTENT_FONT_ITALIC = 1u << 1,
    CONTENT_FONT_UNDERLINE = 1u << 2
};

#define CONTENT_FONT_MIN_PX 12
#define CONTENT_FONT_MAX_PX 96

/* NULL or empty path selects the packaged DejaVu Sans Mono. An explicit path
 * selects a Unicode, fixed-width regular face; matching packaged DejaVu style
 * faces in its directory are used when available. Noto Sans CJK is a fallback.
 * NULL means unavailable (including builds without CONFIG_FREETYPE). */
struct content_font *content_font_new(const char *path, int size_px);
void content_font_free(struct content_font *font);
/* Clamp to 12..96; 0 on success, -1 on failure (the old size is retained). */
int content_font_set_size_px(struct content_font *font, int size_px);
int content_font_size_px(const struct content_font *font); /* 0 if unavailable */
/* Pixel cell size and baseline measured down from the line's top. Any output
 * may be NULL; an unavailable font gives zeroes. Wide characters use two cells. */
void content_font_metrics(const struct content_font *font, int *cell_w, int *cell_h, int *baseline);
/* Locale-independent Unicode widths: combining/format characters 0, wide 2,
 * others 1. Invalid UTF-8 consumes one byte and displays U+FFFD (one cell).
 * Text is an ordinary NUL-terminated string; counts saturate at INT_MAX. */
int content_font_char_cells(uint32_t cp);
int content_font_text_cells(const char *utf8);
/* Draw a single line with its top at pixel (x, y), in 0xRRGGBB colours.
 * The run's cells are filled with bg; combining marks overlay the preceding
 * character without advancing. Controls display U+FFFD. The run, its line
 * height, the screen and clip rectangle all bound drawing, including italics
 * and underlines. Uses screen_blit/fill_px, never screen/device ownership.
 * Bold/italic use real faces, falling back to an available face, not synthesis.
 * This is a cell renderer, not a shaping engine for complex scripts. */
void content_font_draw(struct content_font *font, int x, int y, const char *utf8,
                       int clip_x, int clip_y, int clip_w, int clip_h,
                       uint32_t fg, uint32_t bg, unsigned style);

#endif
