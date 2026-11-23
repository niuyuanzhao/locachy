/*
 * Fonts as fontconfig chooses them, as GTK, Qt and foot (fcft) find theirs:
 * a font set is fontconfig's sorted list for a family, a weight and a slant
 * (FcFontSort, after FcConfigSubstitute and FcDefaultSubstitute, so the
 * system's and the user's fonts.conf and the locale's language decide). Its
 * first font is the match; a character that font lacks comes from the first
 * font after it whose charset has it. fontconfig is initialised once per
 * process (FcInit), a set is sorted once, at its first use, a face opens when
 * a character first needs it, and which font has a character is remembered:
 * drawing looks nothing up again. Which fonts, and which of a CJK family's
 * variants a language gets, is fontconfig's configuration (tools/FONTS.md).
 */
#include "fontmatch.h"

#ifdef CONFIG_FREETYPE
#include <fontconfig/fontconfig.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define SEEN 4096        /* code points remembered per set (direct mapped by cp % SEEN) */
#define GLYPH_CACHE 2048 /* rendered glyph bitmaps cached per set (4-way bucketed) */

struct glyph_entry {
    FT_Face face;
    FT_UInt glyph_id;
    int px;
    uint32_t stamp;
    uint8_t *alpha_buf;
    struct font_glyph glyph;
};

struct font_set {
    FT_Library library;
    FcPattern *pattern;  /* until sorted */
    FcFontSet *fonts;    /* sorted: NULL when there are none */
    FT_Face *faces;      /* each font's, once opened */
    unsigned char *tried;
    int sorted;
    struct { uint32_t key; int font; FT_UInt glyph; } seen[SEEN]; /* key: cp + 1 (0: empty); font -1: none has it */
    struct glyph_entry *glyphs;
    uint32_t glyph_clock;
};

struct font_set *font_set_new(FT_Library library, const char *family, int bold, int italic)
{
    struct font_set *set = calloc(1, sizeof *set);
    if (!set) return NULL;
    set->library = library;
    set->pattern = FcPatternCreate();
    if (!set->pattern || !FcPatternAddString(set->pattern, FC_FAMILY, (const FcChar8 *)family) ||
        (bold && !FcPatternAddInteger(set->pattern, FC_WEIGHT, FC_WEIGHT_BOLD)) ||
        (italic && !FcPatternAddInteger(set->pattern, FC_SLANT, FC_SLANT_ITALIC))) {
        font_set_free(set);
        return NULL;
    }
    return set;
}

void font_set_free(struct font_set *set)
{
    if (!set) return;
    if (set->glyphs) {
        for (int i = 0; i < GLYPH_CACHE; i++) {
            free(set->glyphs[i].alpha_buf);
        }
        free(set->glyphs);
    }
    for (int i = 0; set->faces && i < set->fonts->nfont; i++) {
        if (set->faces[i]) FT_Done_Face(set->faces[i]);
    }
    if (set->fonts) FcFontSetDestroy(set->fonts);
    if (set->pattern) FcPatternDestroy(set->pattern);
    free(set->faces);
    free(set->tried);
    free(set);
}

static int initialised; /* FcInit has read the configuration */

int font_config_reload(void)
{
    if (!initialised || FcConfigUptoDate(NULL)) return 0;
    return FcInitReinitialize() ? 1 : -1;
}

static int sorted(struct font_set *set)
{
    if (set->sorted) return set->fonts != NULL;
    set->sorted = 1;
    FcResult result;
    if (!initialised) {
        if (!FcInit()) return 0;
        initialised = 1;
    }
    if (!FcConfigSubstitute(NULL, set->pattern, FcMatchPattern)) return 0;
    FcDefaultSubstitute(set->pattern);
    set->fonts = FcFontSort(NULL, set->pattern, FcTrue, NULL, &result);
    FcPatternDestroy(set->pattern);
    set->pattern = NULL;
    if (set->fonts && set->fonts->nfont > 0) {
        set->faces = calloc((size_t)set->fonts->nfont, sizeof *set->faces);
        set->tried = calloc((size_t)set->fonts->nfont, 1);
    }
    if (!set->faces || !set->tried) {
        free(set->faces);
        free(set->tried);
        set->faces = NULL;
        set->tried = NULL;
        if (set->fonts) FcFontSetDestroy(set->fonts);
        set->fonts = NULL;
    }
    return set->fonts != NULL;
}

static FT_Face open_font(struct font_set *set, int i)
{
    if (set->tried[i]) return set->faces[i];
    set->tried[i] = 1;
    FcPattern *font = set->fonts->fonts[i];
    FcChar8 *file;
    FcBool flag;
    int index = 0;
    if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch ||
        (FcPatternGetBool(font, FC_SCALABLE, 0, &flag) == FcResultMatch && !flag) ||
        (FcPatternGetBool(font, FC_COLOR, 0, &flag) == FcResultMatch && flag)) {
        return NULL;
    }
    FcPatternGetInteger(font, FC_INDEX, 0, &index);
    FT_Face face;
    if (FT_New_Face(set->library, (const char *)file, index, &face) != 0) return NULL;
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
        FT_Done_Face(face);
        return NULL;
    }
    return set->faces[i] = face;
}

FT_Face font_set_primary(struct font_set *set)
{
    if (!set || !sorted(set)) return NULL;
    for (int i = 0; i < set->fonts->nfont; i++) {
        FT_Face face = open_font(set, i);
        if (face) return face;
    }
    return NULL;
}

FT_Face font_set_face(struct font_set *set, uint32_t cp, int px, FT_UInt *glyph)
{
    *glyph = 0;
    if (!set || !sorted(set)) return NULL;
    uint32_t key = cp + 1u;
    unsigned slot = cp % SEEN;
    int found = -1;
    FT_UInt gid = 0;
    if (set->seen[slot].key == key) {
        found = set->seen[slot].font;
        gid = set->seen[slot].glyph;
    } else {
        for (int i = 0; i < set->fonts->nfont && found < 0; i++) {
            FcCharSet *chars;
            if (FcPatternGetCharSet(set->fonts->fonts[i], FC_CHARSET, 0, &chars) != FcResultMatch ||
                !FcCharSetHasChar(chars, cp)) {
                continue;
            }
            FT_Face face = open_font(set, i);
            if (face) {
                FT_UInt g = FT_Get_Char_Index(face, cp);
                if (g) {
                    found = i;
                    gid = g;
                }
            }
        }
        set->seen[slot].key = key;
        set->seen[slot].font = found;
        set->seen[slot].glyph = gid;
    }
    if (found < 0) return NULL;
    FT_Face face = set->faces[found];
    if (px > 0 && face->size->metrics.y_ppem != px && FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px) != 0) return NULL;
    *glyph = gid;
    return face;
}

static unsigned glyph_hash(FT_Face face, FT_UInt glyph, int px)
{
    uintptr_t fp = (uintptr_t)face;
    uint64_t x = (uint64_t)(fp >> 3) ^ ((uint64_t)glyph * 2654435761ull) ^ ((uint64_t)(uint32_t)px * 40503ull);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (unsigned)x & (GLYPH_CACHE - 1);
}

static int normalize_bitmap(const FT_Bitmap *bm, uint8_t **out_buf)
{
    *out_buf = NULL;
    if (bm->width == 0 || bm->rows == 0 || !bm->buffer) return 0;
    if (bm->width > UINT16_MAX || bm->rows > UINT16_MAX) return -1;
    size_t w = (size_t)bm->width;
    size_t r = (size_t)bm->rows;
    if (w > SIZE_MAX / r) return -1;
    uint8_t *buf = malloc(w * r);
    if (!buf) return -1;
    size_t pitch = bm->pitch < 0 ? (size_t)-(int64_t)bm->pitch : (size_t)bm->pitch;
    if (bm->pixel_mode == FT_PIXEL_MODE_GRAY && bm->num_grays == 256) {
        if (bm->pitch >= 0 && pitch == w) {
            memcpy(buf, bm->buffer, w * r);
        } else {
            for (size_t y = 0; y < r; y++) {
                size_t sy = bm->pitch < 0 ? r - 1 - y : y;
                memcpy(buf + y * w, bm->buffer + sy * pitch, w);
            }
        }
    } else if (bm->pixel_mode == FT_PIXEL_MODE_GRAY && bm->num_grays > 1) {
        unsigned denom = (unsigned)(bm->num_grays - 1);
        for (size_t y = 0; y < r; y++) {
            size_t sy = bm->pitch < 0 ? r - 1 - y : y;
            const unsigned char *src = bm->buffer + sy * pitch;
            uint8_t *dst = buf + y * w;
            for (size_t x = 0; x < w; x++) {
                dst[x] = (uint8_t)((unsigned)src[x] * 255u / denom);
            }
        }
    } else if (bm->pixel_mode == FT_PIXEL_MODE_MONO) {
        for (size_t y = 0; y < r; y++) {
            size_t sy = bm->pitch < 0 ? r - 1 - y : y;
            const unsigned char *src = bm->buffer + sy * pitch;
            uint8_t *dst = buf + y * w;
            for (size_t x = 0; x < w; x++) {
                dst[x] = (src[x >> 3] & (0x80u >> (x & 7))) ? 255u : 0u;
            }
        }
    } else {
        memset(buf, 0, w * r);
    }
    *out_buf = buf;
    return 0;
}

static int16_t clamp_i16(int64_t v)
{
    if (v < INT16_MIN) return INT16_MIN;
    if (v > INT16_MAX) return INT16_MAX;
    return (int16_t)v;
}

const struct font_glyph *font_face_glyph(struct font_set *set, FT_Face face, FT_UInt glyph, int px)
{
    if (!set || !face) return NULL;
    int eff_px = px > 0 ? px : (face->size ? (int)face->size->metrics.y_ppem : 0);
    if (!set->glyphs) {
        set->glyphs = calloc(GLYPH_CACHE, sizeof *set->glyphs);
        if (!set->glyphs) return NULL;
    }
    unsigned h = glyph_hash(face, glyph, eff_px);
    struct glyph_entry *victim = &set->glyphs[h];
    for (unsigned step = 0; step < 4; step++) {
        struct glyph_entry *e = &set->glyphs[(h + step) & (GLYPH_CACHE - 1)];
        if (e->face == face && e->glyph_id == glyph && e->px == eff_px) {
            e->stamp = ++set->glyph_clock;
            return &e->glyph;
        }
        if (!e->face) {
            victim = e;
            break;
        }
        if (victim->face && (int32_t)(e->stamp - victim->stamp) < 0) {
            victim = e;
        }
    }
    if (px > 0 && (!face->size || face->size->metrics.y_ppem != px)) {
        if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px) != 0) return NULL;
    }
    if (FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT) != 0 ||
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
        return NULL;
    }
    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap *bm = &slot->bitmap;
    uint8_t *buf = NULL;
    if (normalize_bitmap(bm, &buf) != 0) return NULL;
    free(victim->alpha_buf);
    victim->face = face;
    victim->glyph_id = glyph;
    victim->px = eff_px;
    victim->stamp = ++set->glyph_clock;
    victim->alpha_buf = buf;
    victim->glyph.width = buf ? (uint16_t)bm->width : 0;
    victim->glyph.rows = buf ? (uint16_t)bm->rows : 0;
    victim->glyph.left = clamp_i16(slot->bitmap_left);
    victim->glyph.top = clamp_i16(slot->bitmap_top);
    victim->glyph.advance_x = clamp_i16((slot->advance.x + 63) >> 6);
    victim->glyph.has_advance = slot->advance.x != 0 ? 1 : 0;
    victim->glyph.alpha = buf;
    return &victim->glyph;
}

const struct font_glyph *font_set_glyph(struct font_set *set, uint32_t cp, int px)
{
    FT_UInt glyph = 0;
    FT_Face face = font_set_face(set, cp, px, &glyph);
    if (!face) return NULL;
    return font_face_glyph(set, face, glyph, px);
}

int font_lang_present(const char *lang)
{
    FcConfig *config = FcInitLoadConfigAndFonts();
    if (!config) return -1;
    FcPattern *pattern = FcPatternCreate();
    FcLangSet *langs = FcLangSetCreate();
    FcObjectSet *objects = FcObjectSetBuild(FC_FILE, (char *)NULL);
    int present = -1;
    if (pattern && langs && objects && FcLangSetAdd(langs, (const FcChar8 *)lang) &&
        FcPatternAddLangSet(pattern, FC_LANG, langs)) {
        FcFontSet *fonts = FcFontList(config, pattern, objects);
        present = fonts && fonts->nfont > 0;
        if (fonts) FcFontSetDestroy(fonts);
    }
    if (objects) FcObjectSetDestroy(objects);
    if (langs) FcLangSetDestroy(langs);
    if (pattern) FcPatternDestroy(pattern);
    FcConfigDestroy(config);
    return present;
}

#else

int font_lang_present(const char *lang)
{
    (void)lang;
    return -1;
}

#endif
