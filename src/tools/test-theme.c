/* Verify the shared palette independently of a renderer or compositor. */
#include "base.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static const char *scheme;
static const char *accent;

int rc_read(void (*visit)(const char *, const char *, void *), void *arg)
{
    if (scheme) visit("color_scheme", scheme, arg);
    if (accent) visit("accent_color", accent, arg);
    return 0;
}

static double linear(unsigned n)
{
    double c = n / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t c)
{
    return 0.2126 * linear(c >> 16 & 255) + 0.7152 * linear(c >> 8 & 255) + 0.0722 * linear(c & 255);
}

static void contrast(uint32_t text, uint32_t background)
{
    double a = luminance(text) + 0.05, b = luminance(background) + 0.05;
    double ratio = a > b ? a / b : b / a;
    if (ratio < 4.4999) fprintf(stderr, "Unreadable pair #%06X on #%06X: %.5f:1\n", text, background, ratio);
    assert(ratio >= 4.4999); /* Allow rounding of the fixed-point luminance table. */
}

static void check_palette(enum theme_scheme s, uint32_t rgb)
{
    struct theme_config config = { .scheme = s, .custom_accent = 1, .accent = rgb };
    struct theme_palette p;
    theme_resolve(&config, &p);
    const uint32_t *c = p.rgb;
    assert(p.config.accent == rgb && c[THEME_ACCENT] == rgb);
    contrast(c[THEME_ON_ACCENT], c[THEME_ACCENT]);
    contrast(c[THEME_FG], c[THEME_SELECTION]);
    static const enum theme_role grounds[] = {
        THEME_BG, THEME_SURFACE, THEME_PANEL, THEME_CONTROL, THEME_INACTIVE, THEME_OUTPUT, THEME_DESKTOP,
    };
    for (unsigned i = 0; i < sizeof grounds / sizeof *grounds; i++) {
        contrast(c[THEME_FG], c[grounds[i]]);
        contrast(c[THEME_ACCENT_TEXT], c[grounds[i]]);
    }
}

int main(void)
{
    uint32_t rgb = 0;
    assert(theme_parse_accent("#00AbFF", &rgb) == 0 && rgb == 0x00abff);
    assert(theme_parse_accent("#000000", NULL) == 0);
    const char *invalid[] = { NULL, "", "default", "#abc", "#00000G", "#00000000", "000000", " #000000", "#123456\n" };
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        assert(theme_parse_accent(invalid[i], &rgb) < 0 && rgb == 0x00abff);
    }
    assert(theme_reload() == 1 && theme_reload() == 0);
    assert(!theme_is_light() && !theme_get()->config.custom_accent && theme_get()->config.accent == 0x80c0ff);
    assert(theme_rgb(THEME_BG, 0x123456) == 0x123456);
    assert(theme_argb(THEME_ACCENT, 0x99123456) == 0x99123456);
    scheme = "light";
    assert(theme_reload() == 1 && theme_is_light() && theme_get()->config.accent == 0x275c9f);
    assert(theme_argb(THEME_BG, 0x99123456) == (0x99000000u | theme_get()->rgb[THEME_BG]));
    assert(theme_ansi(2, 0x123456) == theme_get()->ansi[2] && theme_ansi(16, 0x123456) == 0x123456);
    accent = "#000000";
    assert(theme_reload() == 1 && theme_get()->config.custom_accent && theme_get()->config.accent == 0);
    scheme = "dark";
    assert(theme_reload() == 1 && theme_reload() == 0 && theme_get()->config.custom_accent);
    assert(theme_rgb(THEME_BG, 0x123456) == 0x123456);
    assert(theme_argb(THEME_ACCENT, 0x99123456) == 0x99000000);
    assert(theme_ansi(2, 0x123456) == 0x123456);
    scheme = "unknown";
    accent = "#bad";
    assert(theme_reload() == 1 && !theme_is_light() && !theme_get()->config.custom_accent);
    assert(theme_get()->config.accent == 0x80c0ff);
    for (unsigned s = THEME_DARK; s <= THEME_LIGHT; s++) {
        for (unsigned i = 0; i < 256; i++) check_palette(s, i * 0x010101u);
        /* Sample the RGB cube and the extremes of each individual channel. */
        for (unsigned r = 0; r <= 255; r += 17)
            for (unsigned g = 0; g <= 255; g += 17)
                for (unsigned b = 0; b <= 255; b += 17)
                    check_palette(s, r << 16 | g << 8 | b);
    }
    puts("PASS: theme defaults, exact accents, role contrast and ARGB alpha");
}
