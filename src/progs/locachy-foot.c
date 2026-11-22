/* Launch one managed window of foot with the current system palette. Foot
 * still loads the user's normal configuration; only these colors and the
 * initial scheme are overridden. */
#include "base.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef FOOT_EXECUTABLE
#define FOOT_EXECUTABLE "/usr/bin/foot"
#endif

int main(int argc, char **argv)
{
    const struct theme_palette *p = theme_get();
    const char *scheme = p->config.scheme == THEME_LIGHT ? "light" : "dark";
    const uint32_t *c = p->rgb;
    const struct {
        const char *key;
        uint32_t color;
    } colors[] = {
        { "background", c[THEME_BG] },
        { "foreground", c[THEME_FG] },
        { "selection-background", c[THEME_SELECTION] },
        { "selection-foreground", c[THEME_FG] },
    };
    enum { COLOR_COUNT = sizeof colors / sizeof colors[0], ANSI_COUNT = 16,
           OVERRIDE_COUNT = COLOR_COUNT + ANSI_COUNT + 2 };
    char overrides[OVERRIDE_COUNT][64];
    size_t used = 0;
    snprintf(overrides[used++], sizeof overrides[0], "--override=main.initial-color-theme=%s", scheme);
    /* Foot's cursor value is text color first, then the cursor itself. */
    snprintf(overrides[used++], sizeof overrides[0], "--override=colors-%s.cursor=%06x %06x",
             scheme, (unsigned)c[THEME_ON_ACCENT], (unsigned)c[THEME_ACCENT]);
    for (size_t i = 0; i < COLOR_COUNT; i++)
        snprintf(overrides[used++], sizeof overrides[0], "--override=colors-%s.%s=%06x",
                 scheme, colors[i].key, (unsigned)colors[i].color);
    for (unsigned i = 0; i < ANSI_COUNT; i++)
        snprintf(overrides[used++], sizeof overrides[0], "--override=colors-%s.%s%u=%06x",
                 scheme, i < 8 ? "regular" : "bright", i % 8, (unsigned)p->ansi[i]);

    char **args = calloc((size_t)argc + OVERRIDE_COUNT + 1, sizeof *args);
    if (!args) { perror("locachy-foot"); return 1; }
    args[0] = FOOT_EXECUTABLE;
    for (size_t i = 0; i < used; i++) args[i + 1] = overrides[i];
    /* Keep the recipe's options and command arguments in their original
     * order, including any -- delimiter and explicit color overrides. */
    for (int i = 1; i < argc; i++) args[used + (size_t)i] = argv[i];
    execv(FOOT_EXECUTABLE, args);
    int err = errno;
    fprintf(stderr, "locachy-foot: %s: %s\n", FOOT_EXECUTABLE, strerror(err));
    return err == ENOENT ? 127 : 126;
}
