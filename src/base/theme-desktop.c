/* Only the palette of the shared compositor; its geometry and opacity stay
 * in the native theme and user configuration. */
#include "base.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef LABWC_THEMERC_DIR
#define LABWC_THEMERC_DIR "/etc/locachy/labwc"
#endif
#ifndef LABWC_EXECUTABLE
#define LABWC_EXECUTABLE "/usr/bin/labwc"
#endif

static const struct {
    const char *key;
    enum theme_role role;
} colors[] = {
    { "window.active.border.color", THEME_ACCENT },
    { "window.inactive.border.color", THEME_BORDER },
    { "window.active.title.bg.color", THEME_PANEL },
    { "window.inactive.title.bg.color", THEME_INACTIVE },
    { "window.active.label.text.color", THEME_FG },
    { "window.inactive.label.text.color", THEME_DIM },
    { "window.active.button.unpressed.image.color", THEME_FG },
    { "window.inactive.button.unpressed.image.color", THEME_DIM },
    { "window.active.indicator.toggled-keybind.color", THEME_ERROR },
    { "menu.border.color", THEME_BORDER },
    { "menu.items.bg.color", THEME_SURFACE },
    { "menu.items.text.color", THEME_FG },
    { "menu.items.active.bg.color", THEME_SELECTION },
    { "menu.items.active.text.color", THEME_FG },
    { "menu.separator.color", THEME_BORDER },
    { "menu.title.bg.color", THEME_ACCENT },
    { "menu.title.text.color", THEME_ON_ACCENT },
    { "osd.bg.color", THEME_PANEL },
    { "osd.border.color", THEME_BORDER },
    { "osd.label.text.color", THEME_FG },
};

#define PALETTE_BEGIN "# BEGIN LOCachy palette (generated)\n"
#define PALETTE_END   "# END LOCachy palette\n"

static int palette_line(const char *line, size_t length, const char *marker)
{
    /* Also recognize a marker lacking its final newline. */
    size_t n = strlen(marker);
    return (length == n || length == n - 1) && !memcmp(line, marker, length);
}

static int replace_palette(const char *old, size_t size, int exists, char **out,
                           size_t *out_size, void *arg)
{
    (void)exists;
    const struct theme_palette *palette = arg;
    size_t begin = 0, end = 0;
    int found_begin = 0;
    for (size_t i = 0; i < size;) {
        const char *nl = memchr(old + i, '\n', size - i);
        size_t next = nl ? (size_t)(nl - old) + 1 : size;
        if (palette_line(old + i, next - i, PALETTE_BEGIN)) {
            if (found_begin) { errno = EINVAL; return -1; }
            found_begin = 1;
            begin = i;
        } else if (palette_line(old + i, next - i, PALETTE_END)) {
            if (!found_begin || end) { errno = EINVAL; return -1; }
            end = next;
        }
        i = next;
    }
    if (found_begin && !end) { errno = EINVAL; return -1; }

    char block[2048] = PALETTE_BEGIN;
    size_t used = sizeof PALETTE_BEGIN - 1;
    for (size_t i = 0; i < sizeof colors / sizeof colors[0]; i++) {
        int n = snprintf(block + used, sizeof block - used, "%s: #%06x\n",
                         colors[i].key, (unsigned)palette->rgb[colors[i].role]);
        if (n < 0 || (size_t)n >= sizeof block - used) { errno = EOVERFLOW; return -1; }
        used += (size_t)n;
    }
    if (sizeof PALETTE_END > sizeof block - used) { errno = EOVERFLOW; return -1; }
    memcpy(block + used, PALETTE_END, sizeof PALETTE_END - 1);
    used += sizeof PALETTE_END - 1;
    if (!begin && end == used && !memcmp(old, block, used)) return 1;

    /* Keep the user's bytes, including wildcard colors and alpha. Labwc
     * processes the lines in order, so explicit overrides still win. */
    size_t keep = size - (end - begin);
    if (keep > SIZE_MAX - used) { errno = EOVERFLOW; return -1; }
    char *text = malloc(used + keep);
    if (!text) return -1;
    memcpy(text, block, used);
    memcpy(text + used, old, begin);
    memcpy(text + used + begin, old + end, size - end);
    *out = text;
    *out_size = used + keep;
    return 0;
}

int theme_apply_desktop(void)
{
    /* make.sh installs the configuration directory even without labwc. */
    if (access(LABWC_EXECUTABLE, X_OK) < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (stat(LABWC_THEMERC_DIR, &st) < 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    const struct theme_palette *palette = theme_get();
    return conf_transaction(LABWC_THEMERC_DIR "/themerc-override", replace_palette, (void *)palette);
}
