/* The system color scheme: RGB for a direct canvas, explicit alpha for SHM. */
#ifndef LOCACHY_THEME_H
#define LOCACHY_THEME_H

#include <stdint.h>

enum theme_scheme { THEME_DARK, THEME_LIGHT };
enum theme_role {
    THEME_BG, THEME_FG, THEME_SURFACE, THEME_PANEL, THEME_CONTROL,
    THEME_BORDER, THEME_DIM, THEME_ACCENT, THEME_ON_ACCENT,
    THEME_ACCENT_TEXT, THEME_SELECTION, THEME_ERROR, THEME_WARNING,
    THEME_SUCCESS, THEME_INACTIVE, THEME_OUTPUT, THEME_DESKTOP,
    THEME_ROLE_COUNT
};

struct theme_config {
    enum theme_scheme scheme;
    int custom_accent;
    uint32_t accent; /* sRGB, 0xRRGGBB; resolved even for the scheme default */
};

struct theme_palette {
    struct theme_config config;
    uint32_t rgb[THEME_ROLE_COUNT];
    uint32_t ansi[16];
};

void theme_read(struct theme_config *config);
int theme_parse_accent(const char *value, uint32_t *rgb); /* 0 valid RGB, -1 invalid */
void theme_resolve(const struct theme_config *config, struct theme_palette *palette);
const struct theme_palette *theme_get(void);
int theme_reload(void); /* 1 if the selected scheme or accent changed */
int theme_watch_open(void); /* nonblocking fd owned by the caller; -1 if unavailable */
int theme_watch_read(int fd); /* drain notifications; 1 for a changed palette */
int theme_apply_desktop(void); /* labwc's derived configuration */
int theme_publish_appearance(void); /* the two read-only portal preferences */

/* Most standalone program fixtures include one C file without the rest of
 * the base; keep its historical colors when the palette is not linked. */
#ifndef LOCACHY_THEME_IMPLEMENTATION
extern const struct theme_palette *theme_get(void) __attribute__((weak));
extern int theme_reload(void) __attribute__((weak));
extern int theme_watch_open(void) __attribute__((weak));
extern int theme_watch_read(int fd) __attribute__((weak));
#endif

static inline int theme_is_light(void)
{
#ifndef LOCACHY_THEME_IMPLEMENTATION
    if (!theme_get) return 0;
#endif
    return theme_get()->config.scheme == THEME_LIGHT;
}

static inline uint32_t theme_rgb(enum theme_role role, uint32_t legacy)
{
#ifndef LOCACHY_THEME_IMPLEMENTATION
    if (!theme_get) return legacy;
#endif
    const struct theme_palette *p = theme_get();
    if (p->config.scheme == THEME_DARK) {
        if (!p->config.custom_accent ||
            (role != THEME_ACCENT && role != THEME_ON_ACCENT &&
             role != THEME_ACCENT_TEXT && role != THEME_SELECTION)) return legacy;
    }
    return p->rgb[role];
}

static inline uint32_t theme_argb(enum theme_role role, uint32_t legacy)
{
    return (legacy & 0xff000000u) | theme_rgb(role, legacy & 0xffffffu);
}

static inline uint32_t theme_ansi(unsigned index, uint32_t legacy)
{
#ifndef LOCACHY_THEME_IMPLEMENTATION
    if (!theme_get) return legacy;
#endif
    return index < 16 && theme_is_light() ? theme_get()->ansi[index] : legacy;
}

#endif
