/* Real packaged fonts and an in-memory screen, without DRM, input or a UI. */
#include "../base/font.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_widths(void)
{
    assert(content_font_char_cells('A') == 1);
    assert(content_font_char_cells(0x0301) == 0);
    assert(content_font_char_cells(0x093E) == 0);
    assert(content_font_char_cells(0x1AB0) == 0);
    assert(content_font_char_cells(0x200D) == 0);
    assert(content_font_char_cells(0xE0100) == 0);
    assert(content_font_char_cells(0x1161) == 0);
    assert(content_font_char_cells(0x1F3FD) == 0);
    assert(content_font_char_cells(0x4E2D) == 2);
    assert(content_font_char_cells(0xFF0C) == 2);
    assert(content_font_char_cells(0x1F642) == 2);
    assert(content_font_char_cells(0xFF66) == 1);
    assert(content_font_text_cells(NULL) == 0);
    assert(content_font_text_cells("") == 0);
    assert(content_font_text_cells("A\xE4\xB8\xAD" "e\xCC\x81\xF0\x9F\x99\x82") == 6);
    assert(content_font_text_cells("\xCC\x81" "a") == 1);
    assert(content_font_text_cells("\xF0") == 1);
    assert(content_font_text_cells("\xE4\xB8") == 2);
    assert(content_font_text_cells("\xC0\x80") == 2);
    assert(content_font_text_cells("\xED\xA0\x80") == 3);
    assert(content_font_text_cells("\xF4\x90\x80\x80") == 4);
    puts("PASS locale-independent Unicode widths, combining marks and malformed UTF-8");
}

#ifdef CONFIG_FREETYPE
#define WIDTH 1024
#define HEIGHT 400
#define SENTINEL 0x173B59u
#define FG 0xEAD9C2u
#define BG 0x203145u
static uint32_t picture[WIDTH * HEIGHT];
static int allow_x, allow_y, allow_w = WIDTH, allow_h = HEIGHT;
static int fills, blits;

void screen_pixels(int *width, int *height) { *width = WIDTH; *height = HEIGHT; }

static void check_rect(int x, int y, int w, int h)
{
    assert(w > 0 && h > 0 && x >= 0 && y >= 0 && x + w <= WIDTH && y + h <= HEIGHT);
    assert(x >= allow_x && y >= allow_y && x + w <= allow_x + allow_w && y + h <= allow_y + allow_h);
}

void screen_fill_px(int x, int y, int w, int h, uint32_t rgb)
{
    check_rect(x, y, w, h);
    fills++;
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++) picture[py * WIDTH + px] = rgb & 0xFFFFFF;
}

void screen_blit(int x, int y, int w, int h, const uint32_t *px, int stride)
{
    check_rect(x, y, w, h);
    assert(stride >= w);
    blits++;
    for (int py = 0; py < h; py++)
        for (int ix = 0; ix < w; ix++) picture[(y + py) * WIDTH + x + ix] = px[py * stride + ix] & 0xFFFFFF;
}

static void reset_screen(int x, int y, int w, int h)
{
    for (size_t i = 0; i < sizeof picture / sizeof *picture; i++) picture[i] = SENTINEL;
    allow_x = x; allow_y = y; allow_w = w; allow_h = h;
    fills = blits = 0;
}

static uint64_t image_hash(void)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < sizeof picture / sizeof *picture; i++)
        hash = (hash ^ picture[i]) * UINT64_C(1099511628211);
    return hash;
}

static int ink_count(void)
{
    int count = 0;
    for (size_t i = 0; i < sizeof picture / sizeof *picture; i++)
        if (picture[i] != SENTINEL && picture[i] != BG) count++;
    return count;
}

static void draw(struct content_font *font, int x, int y, const char *text, unsigned style)
{
    content_font_draw(font, x, y, text, allow_x, allow_y, allow_w, allow_h, FG, BG, style);
}

static void test_sizes_and_ownership(void)
{
    struct content_font *font = content_font_new(NULL, 18);
    struct content_font *other = content_font_new(NULL, 18);
    assert(font && other && font != other && font->library != other->library);
    for (int i = 0; i < 4; i++) assert(font->faces[i] && font->faces[i] != other->faces[i]);
    assert(font->cjk[0] && font->cjk[1]);
    assert(strstr(font->cjk[0]->family_name, " SC"));
    int w0, h0, b0, w1, h1, b1;
    content_font_metrics(font, &w0, &h0, &b0);
    int old_w = w0, old_h = h0, old_baseline = b0;
    assert(w0 > 0 && h0 >= 18 && b0 > 0 && b0 < h0);
    reset_screen(0, 0, WIDTH, HEIGHT);
    draw(font, 20, 20, "Ag 123", 0);
    uint64_t small = image_hash();
    int small_ink = ink_count();
    assert(small_ink > 0 && fills == 1 && blits == 1);
    assert(content_font_set_size_px(font, 36) == 0);
    content_font_metrics(font, &w1, &h1, &b1);
    assert(w1 > w0 && h1 > h0 && b1 > b0);
    assert(other->cell_w == w0 && other->cell_h == h0 && other->size_px == 18);
    for (int i = 0; i < 4; i++) {
        assert(font->faces[i]->size->metrics.y_ppem == 36);
        assert(other->faces[i]->size->metrics.y_ppem == 18);
    }
    for (int i = 0; i < 2; i++) assert(font->cjk[i]->size->metrics.y_ppem == 36);
    reset_screen(0, 0, WIDTH, HEIGHT);
    draw(font, 20, 20, "Ag 123", 0);
    assert(image_hash() != small && ink_count() > small_ink);
    assert(content_font_set_size_px(font, INT_MIN) == 0 && content_font_size_px(font) == 12);
    assert(content_font_set_size_px(font, INT_MAX) == 0 && content_font_size_px(font) == 96);
    assert(content_font_set_size_px(NULL, 18) == -1);
    assert(!content_font_new("/nonexistent/DejaVuSansMono.ttf", 18));
    struct content_font *explicit_font = content_font_new(regular_paths[0], 24);
    if (!explicit_font) explicit_font = content_font_new(regular_paths[1], 24);
    assert(explicit_font);
    content_font_free(explicit_font);
    content_font_free(font);
    content_font_free(other);
    content_font_free(NULL);
    content_font_metrics(NULL, &w0, &h0, &b0);
    assert(w0 == 0 && h0 == 0 && b0 == 0 && content_font_size_px(NULL) == 0);
    printf("PASS independent real FT_Faces and resizing: 18px %dx%d baseline %d; 36px %dx%d baseline %d\n",
           old_w, old_h, old_baseline, w1, h1, b1);
}

static void test_clips(struct content_font *font)
{
    int cw = font->cell_w, ch = font->cell_h;
    reset_screen(53, 37, 61, 19);
    draw(font, 40, 30, "jAg\xE4\xB8\xAD" "g\xCC\x81", CONTENT_FONT_BOLD | CONTENT_FONT_ITALIC | CONTENT_FONT_UNDERLINE);
    assert(blits == 1 && fills == 1);
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++)
            if (x < 53 || x >= 114 || y < 37 || y >= 56) assert(picture[y * WIDTH + x] == SENTINEL);
    reset_screen(0, 0, WIDTH, HEIGHT);
    draw(font, -cw, -ch / 2, "AB\xE4\xB8\xAD", CONTENT_FONT_ITALIC);
    assert(blits == 1);
    reset_screen(0, 0, WIDTH, HEIGHT);
    draw(font, WIDTH - cw / 2, HEIGHT - ch / 2, "AB", CONTENT_FONT_UNDERLINE);
    assert(blits == 1);
    reset_screen(0, 0, WIDTH, HEIGHT);
    draw(font, INT_MAX, INT_MAX, "AA", 7);
    draw(font, INT_MIN, INT_MIN, "AA", 7);
    content_font_draw(font, 0, 0, "AA", INT_MAX, 0, INT_MAX, HEIGHT, FG, BG, 7);
    content_font_draw(font, 0, 0, "AA", 0, 0, -1, HEIGHT, FG, BG, 7);
    draw(font, 0, 0, "", 7);
    draw(font, 0, 0, "\xCC\x81", 7);
    draw(NULL, 0, 0, "A", 0);
    draw(font, 0, 0, NULL, 0);
    assert(!fills && !blits);
    reset_screen(0, 0, WIDTH, HEIGHT);
    draw(font, 50, 30, "jA", CONTENT_FONT_ITALIC | CONTENT_FONT_UNDERLINE);
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++)
            if (x < 50 || x >= 50 + cw * 2 || y < 30 || y >= 30 + ch)
                assert(picture[y * WIDTH + x] == SENTINEL);
    /* Every clipped picture is exactly the same crop of an unclipped run. */
    uint32_t *full = malloc(sizeof picture);
    assert(full);
    reset_screen(0, 0, WIDTH, HEIGHT);
    draw(font, 50, 70, "jA\xE4\xB8\xAD" "e\xCC\x81" "fj", 7);
    memcpy(full, picture, sizeof picture);
    for (int offset = 0; offset < cw * 8; offset++) {
        int cx = 45 + offset, cy = 65 + offset % ch;
        reset_screen(cx, cy, 17, 11);
        draw(font, 50, 70, "jA\xE4\xB8\xAD" "e\xCC\x81" "fj", 7);
        for (int py = 0; py < HEIGHT; py++)
            for (int px = 0; px < WIDTH; px++)
                assert(picture[py * WIDTH + px] ==
                       (px >= cx && px < cx + 17 && py >= cy && py < cy + 11 ?
                        full[py * WIDTH + px] : SENTINEL));
    }
    free(full);
    puts("PASS glyphs, italic overhang and underline stay inside line/run/clip/screen; crops match full render");
}

static void test_unicode(struct content_font *font)
{
    int cw = font->cell_w, ch = font->cell_h;
    reset_screen(0, 0, WIDTH, HEIGHT);
    draw(font, 20, 20, "eB", 0);
    uint32_t *plain = malloc(sizeof picture);
    assert(plain);
    memcpy(plain, picture, sizeof picture);
    reset_screen(0, 0, WIDTH, HEIGHT);
    draw(font, 20, 20, "e\xCC\x81" "B", 0);
    int accent = 0;
    for (int y = 20; y < 20 + ch; y++) {
        for (int x = 20; x < 20 + cw; x++) if (plain[y * WIDTH + x] != picture[y * WIDTH + x]) accent++;
        for (int x = 20 + cw; x < 20 + 2 * cw; x++) assert(plain[y * WIDTH + x] == picture[y * WIDTH + x]);
    }
    assert(accent > 0);
    reset_screen(0, 0, WIDTH, HEIGHT);
    draw(font, 20, 20, "\xE4\xB8\xAD" "B", 0);
    assert(ink_count() > 0);
    FT_UInt glyph;
    FT_Face cjk = glyph_face(font, font->faces[0], 0x4E2D, 0, &glyph);
    assert(cjk == font->cjk[0] && glyph);
    assert(glyph_face(font, font->faces[1], 0x4E2D, CONTENT_FONT_BOLD, &glyph) == font->cjk[1] && glyph);
    for (int y = 20; y < 20 + ch; y++)
        for (int x = 20 + 2 * cw; x < 20 + 3 * cw; x++)
            assert(picture[y * WIDTH + x] == plain[y * WIDTH + x - cw]);
    /* A missing character uses the real .notdef, not uninitialized pixels. */
    draw(font, 100, 20, "\xF4\x8F\xBF\xBF", 0);
    const char *bad[] = { "\xF0", "\xE4\xB8", "\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\t\n" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) draw(font, 100, 80, bad[i], 0);
    free(plain);
    puts("PASS CJK regular/bold fallback, two-cell advance, combining accent overlay and invalid UTF-8 drawing");
}

static void test_styles_and_raster(struct content_font *font)
{
    uint64_t hashes[8];
    for (unsigned style = 0; style < 8; style++) {
        FT_Face face = style_face(font, style);
        assert(face == font->faces[style & 3]);
        assert(((face->style_flags & FT_STYLE_FLAG_BOLD) != 0) == ((style & CONTENT_FONT_BOLD) != 0));
        assert(((face->style_flags & FT_STYLE_FLAG_ITALIC) != 0) == ((style & CONTENT_FONT_ITALIC) != 0));
        reset_screen(0, 0, WIDTH, HEIGHT);
        draw(font, 30, 30, "Afgj", style);
        hashes[style] = image_hash();
        for (unsigned other = 0; other < style; other++) assert(hashes[style] != hashes[other]);
    }
    assert(blend(0xFF0000, 0x0000FF, 128) == 0x80007F);
    assert(blend(FG, BG, 0) == BG && blend(FG, BG, 255) == FG);
    reset_screen(0, 0, WIDTH, HEIGHT);
    draw(font, 20, 20, "A", 0);
    FT_Face face = font->faces[0];
    assert(!FT_Load_Char(face, 'A', FT_LOAD_RENDER));
    FT_GlyphSlot slot = face->glyph;
    int gray = 0;
    for (unsigned y = 0; y < slot->bitmap.rows; y++) {
        for (unsigned x = 0; x < slot->bitmap.width; x++) {
            unsigned alpha = coverage(&slot->bitmap, x, y);
            int px = 20 + slot->bitmap_left + (int)x;
            int py = 20 + font->baseline - slot->bitmap_top + (int)y;
            if (px < 20 || px >= 20 + font->cell_w || py < 20 || py >= 20 + font->cell_h) continue;
            assert(picture[py * WIDTH + px] == blend(FG, BG, alpha));
            if (alpha && alpha != 255) gray++;
        }
    }
    assert(gray > 0);
    assert(!FT_Load_Char(face, 'A', FT_LOAD_TARGET_MONO));
    assert(!FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO));
    assert(face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO);
    int mono_ink = 0;
    for (unsigned y = 0; y < face->glyph->bitmap.rows; y++)
        for (unsigned x = 0; x < face->glyph->bitmap.width; x++) {
            unsigned alpha = coverage(&face->glyph->bitmap, x, y);
            assert(alpha == 0 || alpha == 255);
            mono_ink += alpha != 0;
        }
    assert(mono_ink > 0);
    unsigned char reverse[] = { 255, 0, 0, 255 };
    FT_Bitmap bitmap = { .rows = 2, .width = 2, .pitch = -2, .buffer = reverse,
                         .num_grays = 256, .pixel_mode = FT_PIXEL_MODE_GRAY };
    assert(coverage(&bitmap, 0, 0) == 0 && coverage(&bitmap, 1, 0) == 255);
    assert(coverage(&bitmap, 0, 1) == 255 && coverage(&bitmap, 1, 1) == 0);
    puts("PASS actual regular/bold/italic/bold-italic faces, underline, exact alpha blend, gray/mono and negative pitch");
}
#endif

int main(void)
{
    test_widths();
#ifdef CONFIG_FREETYPE
    test_sizes_and_ownership();
    struct content_font *font = content_font_new(NULL, 24);
    assert(font);
    test_clips(font);
    test_unicode(font);
    test_styles_and_raster(font);
    content_font_free(font);
#else
    assert(!content_font_new(NULL, 24));
    assert(content_font_set_size_px(NULL, 24) == -1);
    assert(content_font_size_px(NULL) == 0);
    int w = -1, h = -1, b = -1;
    content_font_metrics(NULL, &w, &h, &b);
    assert(w == 0 && h == 0 && b == 0);
    content_font_free(NULL);
    content_font_draw(NULL, 0, 0, "hello", 0, 0, 100, 100, 0, 0, 0);
    puts("PASS unavailable API without FreeType, fonts or screen dependencies");
#endif
    puts("ALL PASSED");
    return 0;
}
