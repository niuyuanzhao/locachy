/*
 * test-fonts CASE: fontmatch.c, and the bar's wide characters (panel.c),
 * against the fixture configuration FONTCONFIG_FILE names (font-fixtures.sh
 * makes them, each with a fonts directory of its own): no font file's path
 * is known here. PRIMARY is the family the configuration makes monospace,
 * CJK part of the name of the family U+4E2D should come from ("-": none).
 * CASE reload: see reload().
 */
#include "../base/fontmatch.c"
#include "../base/panel.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void base_status(const char *ime, char *out, size_t size)
{
    (void)ime;
    snprintf(out, size, "status");
}

void base_status_fields(struct status_fields *s)
{
    memset(s, 0, sizeof *s);
}

static const char *env(const char *name)
{
    const char *value = getenv(name);
    if (!value || !*value) {
        fprintf(stderr, "test-fonts: %s is not set\n", name);
        exit(2);
    }
    return value;
}

static int seen_font(const struct font_set *fs, uint32_t cp)
{
    unsigned slot = cp % SEEN;
    return fs->seen[slot].key == cp + 1 ? fs->seen[slot].font : -2;
}

/* A bar font as panel_font loads one (blank PSF glyphs), with the FreeType
 * part panel_font sets up. */
static struct panel_font *bar_font(void)
{
    static uint8_t blank[256 * 2 * 16];
    struct panel_font *f = &fonts[0];
    memset(f, 0, sizeof *f);
    f->loaded = 1;
    f->size = 16;
    f->mag = 1;
    f->w = 8;
    f->h = 16;
    f->row_bytes = 1;
    f->bytes = 16;
    f->count = 256;
    f->glyphs = blank;
    ft_tried = 1;
    assert(FT_Init_FreeType(&ft_lib) == 0);
    ft_fonts = font_set_new(ft_lib, "monospace", 0, 0);
    assert(ft_fonts);
    f->ft_px = (int)f->h - 1;
    return f;
}

/* The pixels U+4E2D's two cells have inked. */
static int bar_ink(const struct panel_font *f)
{
    static uint32_t px[64 * 16];
    for (size_t i = 0; i < sizeof px / sizeof *px; i++) px[i] = 0xFF000000u;
    panel_text(px, 64, 16, 64, 0, 0, "\xE4\xB8\xAD" "A", 0xFFFFFFFFu, 0xFF000000u, f);
    int ink = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) ink += px[y * 64 + x] != 0xFF000000u;
    assert(panel_text_width("\xE4\xB8\xAD" "A", f) == 3 * 8);
    return ink;
}

static void bar_done(void)
{
    font_set_free(ft_fonts);
    ft_fonts = NULL;
    FT_Done_FreeType(ft_lib);
}

/* The bar draws a wide character from fontconfig's fonts at its rows'
 * height, or the PSF '?' (blank here) when no font has it. */
static void bar_text(const char *name, int want_cjk)
{
    int ink = bar_ink(bar_font());
    assert(want_cjk ? ink > 0 : ink == 0);
    printf("PASS %s: the bar draws U+4E2D in two cells %s\n", name,
           want_cjk ? "from fontconfig's CJK fallback" : "as its PSF '?' (no CJK font)");
    bar_done();
}

/* test-fonts reload: the configuration's fonts directory FONT_DIR has no CJK
 * font; once FONT_ADD is linked into it, the bar's SIGHUP (panel_fonts_reload)
 * reads fontconfig again and draws CJK, as after settings installed the CJK
 * fonts with Chinese Pinyin. */
static int reload(void)
{
    const char *dir = env("FONT_DIR"), *add = env("FONT_ADD");
    const struct panel_font *f = bar_font();
    assert(bar_ink(f) == 0);
    assert(panel_fonts_reload() == 0); /* nothing changed: the same fonts */
    /* fontconfig compares modification times in seconds with the time it
     * read its configuration. */
    time_t read_at = time(NULL);
    while (time(NULL) <= read_at + 1) usleep(100 * 1000);
    char link[4096];
    snprintf(link, sizeof link, "%s/cjk-added", dir);
    assert(symlink(add, link) == 0);
    assert(panel_fonts_reload() == 1);
    assert(bar_ink(f) > 0);
    assert(panel_fonts_reload() == 0);
    bar_done();
    puts("PASS reload: fonts installed after the bar started are drawn after its reload (SIGHUP)");
    puts("ALL PASSED");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: test-fonts CASE\n");
        return 2;
    }
    if (strcmp(argv[1], "reload") == 0) return reload();
    const char *name = argv[1], *primary_family = env("PRIMARY"), *cjk_family = env("CJK");
    int want_cjk = strcmp(cjk_family, "-") != 0;
    FT_Library library;
    assert(FT_Init_FreeType(&library) == 0);

    struct font_set *mono = font_set_new(library, "monospace", 0, 0);
    assert(mono && !mono->sorted); /* nothing is read until a face is wanted */
    FT_Face primary = font_set_primary(mono);
    assert(primary && primary->family_name);
    printf("%s: monospace is %s %s\n", name, primary->family_name, primary->style_name);
    assert(strcmp(primary->family_name, primary_family) == 0);
    FT_UInt glyph;
    assert(font_set_face(mono, 'A', 18, &glyph) == primary && glyph);
    assert(primary->size->metrics.y_ppem == 18);
    printf("PASS %s: the primary face is the configuration's monospace (%s)\n", name, primary_family);

    FT_Face cjk = font_set_face(mono, 0x4E2D, 20, &glyph);
    if (want_cjk) {
        assert(cjk && cjk != primary && glyph && cjk->family_name);
        printf("%s: U+4E2D from %s %s\n", name, cjk->family_name, cjk->style_name);
        assert(strstr(cjk->family_name, cjk_family));
        assert(cjk->size->metrics.y_ppem == 20);
        int font = seen_font(mono, 0x4E2D);
        assert(font >= 0 && mono->faces[font] == cjk); /* remembered */
        assert(font_set_face(mono, 0x6587, 20, &glyph) == cjk && glyph);
        assert(font_set_face(mono, 0x4E2D, 0, &glyph) == cjk && glyph);
        struct font_set *bold = font_set_new(library, "monospace", 1, 0);
        FT_Face bold_cjk = font_set_face(bold, 0x4E2D, 20, &glyph);
        assert(bold_cjk && glyph && strstr(bold_cjk->family_name, cjk_family));
        printf("%s: bold U+4E2D from %s %s\n", name, bold_cjk->family_name, bold_cjk->style_name);
        font_set_free(bold);
        printf("PASS %s: CJK comes from fontconfig's fallback (%s), remembered per character\n",
               name, cjk->family_name);
    } else {
        assert(!cjk && glyph == 0);
        assert(seen_font(mono, 0x4E2D) == -1); /* none has it: not looked up again */
        assert(!font_set_face(mono, 0x4E2D, 20, &glyph) && glyph == 0);
        printf("PASS %s: without a CJK font a CJK character has no face, and that is remembered\n", name);
    }
    /* No font has a noncharacter: looked up once, then remembered. */
    assert(!font_set_face(mono, 0x10FFFE, 20, &glyph) && glyph == 0 && seen_font(mono, 0x10FFFE) == -1);
    /* An unknown family still gives fontconfig's best match, never nothing. */
    struct font_set *unknown = font_set_new(library, "No Such Family 7f3a", 0, 0);
    assert(font_set_primary(unknown));
    font_set_free(unknown);
    font_set_free(mono);
    FT_Done_FreeType(library);

    assert(font_lang_present("zh") == want_cjk);
    if (!want_cjk || strstr(cjk_family, "CJK")) assert(font_lang_present("zh-cn") == want_cjk);
    printf("PASS %s: fontconfig has a font for zh: %s\n", name, want_cjk ? "yes" : "no");
    bar_text(name, want_cjk);
    puts("ALL PASSED");
    return 0;
}
