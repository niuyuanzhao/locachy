/*
 * Fonts as fontconfig chooses them (tools/FONTS.md), for the programs built
 * with FreeType (CONFIG_FREETYPE: glibc). Process 1 and the static musl
 * programs draw PSF fonts only and never link fontconfig.
 */
#ifndef FONTMATCH_H
#define FONTMATCH_H

/* Whether fontconfig has a font for the language lang (a tag as fontconfig
 * has them: "zh", "zh-cn", "ja"), as fc-list :lang=LANG lists them, with the
 * configuration read afresh, so fonts installed since are seen: 1 yes, 0 no,
 * -1 no fontconfig (a build without FreeType, or it cannot be read). */
int font_lang_present(const char *lang);

#ifdef CONFIG_FREETYPE
#include <stdint.h>
#include <ft2build.h>
#include FT_FREETYPE_H

struct font_set;

/* fontconfig's configuration read again when its files or font directories
 * changed since it was read (FcConfigUptoDate; fonts installed or removed):
 * 1 read again, 0 unchanged or never read yet, -1 cannot be read. The sets
 * made before still hold the old fonts: their owners make them again. */
int font_config_reload(void);

/* fontconfig's fonts for family ("monospace", or a family's name) of that
 * weight and slant, sorted as FcFontSort sorts them after the system's and
 * the user's configuration (fonts.conf) and the locale's language (FC_LANG,
 * LC_ALL, LC_CTYPE, LANG), trimmed to those that add characters. Sorted at
 * its first use, once. Its faces open when first needed, in library, and stay
 * open until font_set_free (before the library is done). NULL: no memory. */
struct font_set *font_set_new(FT_Library library, const char *family, int bold, int italic);
void font_set_free(struct font_set *set);

/* The first of the set's fonts that opens: fontconfig's match. NULL if none.
 * Bitmap-only and colour fonts are skipped: these programs blend grey
 * coverage at any pixel size. */
FT_Face font_set_primary(struct font_set *set);

/* The face for cp: the first of the set's fonts whose charset has it (the
 * match, then its fallbacks), at px pixels (0: as it is), *glyph its glyph;
 * NULL and 0 when none has it. Which font has cp is remembered, also that
 * none has, so that drawing it again looks nothing up. */
FT_Face font_set_face(struct font_set *set, uint32_t cp, int px, FT_UInt *glyph);
#endif

#endif
