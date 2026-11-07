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
#include <stdlib.h>

#define SEEN 256 /* code points remembered per set (direct mapped) */

struct font_set {
    FT_Library library;
    FcPattern *pattern;  /* until sorted */
    FcFontSet *fonts;    /* sorted: NULL when there are none */
    FT_Face *faces;      /* each font's, once opened */
    unsigned char *tried;
    int sorted;
    struct { uint32_t key; int font; } seen[SEEN]; /* key: cp + 1 (0: empty); font -1: none has it */
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
    initialised = 1;
    if (!FcInit() || !FcConfigSubstitute(NULL, set->pattern, FcMatchPattern)) return 0;
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
    unsigned slot = cp % SEEN;
    int found = -1;
    if (set->seen[slot].key == cp + 1) {
        found = set->seen[slot].font;
    } else {
        for (int i = 0; i < set->fonts->nfont && found < 0; i++) {
            FcCharSet *chars;
            if (FcPatternGetCharSet(set->fonts->fonts[i], FC_CHARSET, 0, &chars) != FcResultMatch ||
                !FcCharSetHasChar(chars, cp)) {
                continue;
            }
            FT_Face face = open_font(set, i);
            if (face && FT_Get_Char_Index(face, cp)) found = i;
        }
        set->seen[slot].key = cp + 1;
        set->seen[slot].font = found;
    }
    if (found < 0) return NULL;
    FT_Face face = set->faces[found];
    if (px > 0 && face->size->metrics.y_ppem != px && FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px) != 0) return NULL;
    *glyph = FT_Get_Char_Index(face, cp);
    return face;
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
