/* One pair of system preferences, with derived roles for the UI and portals. */
#define LOCACHY_THEME_IMPLEMENTATION
#include "base.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/stat.h>
#include <sys/inotify.h>
#endif

#ifndef THEME_APPEARANCE_DIR
#define THEME_APPEARANCE_DIR "/run/locachy/appearance"
#endif

static const uint32_t dark[THEME_ROLE_COUNT] = {
    [THEME_BG] = 0x101820, [THEME_FG] = 0xd0d0d0,
    [THEME_SURFACE] = 0x1c2836, [THEME_PANEL] = 0x203048,
    [THEME_CONTROL] = 0x304860, [THEME_BORDER] = 0x50647b, [THEME_DIM] = 0x9baab8,
    [THEME_ERROR] = 0xff9090, [THEME_WARNING] = 0xffd17a, [THEME_SUCCESS] = 0x81d697,
    [THEME_INACTIVE] = 0x141e28, [THEME_OUTPUT] = 0x05080c, [THEME_DESKTOP] = 0x2e3238,
};
static const uint32_t light[THEME_ROLE_COUNT] = {
    [THEME_BG] = 0xf6f8fb, [THEME_FG] = 0x202934,
    [THEME_SURFACE] = 0xe9eef5, [THEME_PANEL] = 0xe4ebf4,
    [THEME_CONTROL] = 0xd4dfed, [THEME_BORDER] = 0x6c7e96, [THEME_DIM] = 0x4a596c,
    [THEME_ERROR] = 0x9c2234, [THEME_WARNING] = 0x704b00, [THEME_SUCCESS] = 0x235d3b,
    [THEME_INACTIVE] = 0xeef2f7, [THEME_OUTPUT] = 0xeef2f6, [THEME_DESKTOP] = 0xd6dfe9,
};
static const uint32_t ansi_dark[16] = {
    0x101820, 0xcc4444, 0x44b866, 0xccaa44, 0x4488cc, 0xb855b8, 0x44b8b8, 0xd0d0d0,
    0x606870, 0xff6666, 0x66e088, 0xffe066, 0x66b0ff, 0xe080e0, 0x66e0e0, 0xffffff,
};
static const uint32_t ansi_light[16] = {
    0x222c39, 0x912733, 0x27623b, 0x674700, 0x245290, 0x782e77, 0x1b5968, 0xe4eaf2,
    0x526279, 0xa22943, 0x276a38, 0x6e5000, 0x255da0, 0x8b3378, 0x15606a, 0xfcfdff,
};
static struct theme_palette current;
static int loaded;

/* IEC sRGB transfer curve in a fixed 0..1000000 scale; keeping it as a
 * table avoids libm in the static init build. See test-theme for the formula. */
static const uint32_t linear_srgb[256] = {
    0,304,607,911,1214,1518,1821,2125,2428,2732,3035,3347,3677,4025,4391,4777,
    5182,5605,6049,6512,6995,7499,8023,8568,9134,9721,10330,10960,11612,12286,12983,13702,
    14444,15209,15996,16807,17642,18500,19382,20289,21219,22174,23153,24158,25187,26241,27321,28426,
    29557,30713,31896,33105,34340,35601,36889,38204,39546,40915,42311,43735,45186,46665,48172,49707,
    51269,52861,54480,56128,57805,59511,61246,63010,64803,66626,68478,70360,72272,74214,76185,78187,
    80220,82283,84376,86500,88656,90842,93059,95307,97587,99899,102242,104616,107023,109462,111932,114435,
    116971,119538,122139,124772,127438,130136,132868,135633,138432,141263,144128,147027,149960,152926,155926,158961,
    162029,165132,168269,171441,174647,177888,181164,184475,187821,191202,194618,198069,201556,205079,208637,212231,
    215861,219526,223228,226966,230740,234551,238398,242281,246201,250158,254152,258183,262251,266356,270498,274677,
    278894,283149,287441,291771,296138,300544,304987,309469,313989,318547,323143,327778,332452,337164,341914,346704,
    351533,356400,361307,366253,371238,376262,381326,386429,391572,396755,401978,407240,412543,417885,423268,428690,
    434154,439657,445201,450786,456411,462077,467784,473531,479320,485150,491021,496933,502886,508881,514918,520996,
    527115,533276,539479,545724,552011,558340,564712,571125,577580,584078,590619,597202,603827,610496,617207,623960,
    630757,637597,644480,651406,658375,665387,672443,679542,686685,693872,701102,708376,715694,723055,730461,737910,
    745404,752942,760525,768151,775822,783538,791298,799103,806952,814847,822786,830770,838799,846873,854993,863157,
    871367,879622,887923,896269,904661,913099,921582,930111,938686,947307,955973,964686,973445,982251,991102,1000000,
};

static uint32_t luminance(uint32_t rgb)
{
    return (2126 * linear_srgb[rgb >> 16 & 255] + (uint64_t)7152 * linear_srgb[rgb >> 8 & 255] +
            722 * linear_srgb[rgb & 255]) / 10000;
}

static int readable(uint32_t a, uint32_t b)
{
    uint32_t x = luminance(a) + 50000, y = luminance(b) + 50000;
    return (x > y ? x : y) * 2 >= (x < y ? x : y) * 9;
}

static uint32_t blend(uint32_t from, uint32_t to, unsigned percent)
{
    uint32_t rgb = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned a = (from >> shift) & 255, b = (to >> shift) & 255;
        rgb |= ((a * (100 - percent) + b * percent + 50) / 100) << shift;
    }
    return rgb;
}

int theme_parse_accent(const char *s, uint32_t *rgb)
{
    if (!s || s[0] != '#' || strlen(s) != 7) return -1;
    uint32_t value = 0;
    for (int i = 1; i < 7; i++) {
        unsigned digit = (unsigned char)s[i];
        if (digit >= '0' && digit <= '9') digit -= '0';
        else if ((digit | 32u) >= 'a' && (digit | 32u) <= 'f') digit = (digit | 32u) - 'a' + 10;
        else return -1;
        value = value * 16 + digit;
    }
    if (rgb) *rgb = value;
    return 0;
}

static uint32_t default_accent(enum theme_scheme scheme)
{
    return scheme == THEME_LIGHT ? 0x275c9f : 0x80c0ff;
}

static void read_setting(const char *name, const char *value, void *data)
{
    struct theme_config *p = data;
    if (strcmp(name, "color_scheme") == 0)
        p->scheme = strcmp(value, "light") == 0 ? THEME_LIGHT : THEME_DARK;
    else if (strcmp(name, "accent_color") == 0)
        p->custom_accent = theme_parse_accent(value, &p->accent) == 0;
}

void theme_read(struct theme_config *config)
{
    *config = (struct theme_config){ .scheme = THEME_DARK };
    rc_read(read_setting, config);
    if (!config->custom_accent) config->accent = default_accent(config->scheme);
}

void theme_resolve(const struct theme_config *config, struct theme_palette *p)
{
    p->config = *config;
    int is_light = config->scheme == THEME_LIGHT;
    uint32_t *c = p->rgb;
    memcpy(c, is_light ? light : dark, sizeof p->rgb);
    memcpy(p->ansi, is_light ? ansi_light : ansi_dark, sizeof p->ansi);
    uint32_t accent = config->custom_accent ? config->accent & 0xffffffu : default_accent(config->scheme);
    p->config.accent = c[THEME_ACCENT] = accent;
    /* Use the better of black and white on an exact user-selected fill. */
    c[THEME_ON_ACCENT] = luminance(accent) >= 179129 ? 0 : 0xffffff;
    /* For the foreground accent, tint toward the theme's text until it is
     * readable on both the canvas and the stronger UI surfaces. */
    static const enum theme_role grounds[] = { THEME_BG, THEME_SURFACE, THEME_PANEL, THEME_CONTROL,
                                               THEME_INACTIVE, THEME_OUTPUT, THEME_DESKTOP };
    c[THEME_ACCENT_TEXT] = c[THEME_FG];
    for (unsigned i = 0; i < 100; i += 4) {
        uint32_t text = blend(accent, c[THEME_FG], i);
        int all = 1;
        for (size_t j = 0; j < sizeof grounds / sizeof *grounds; j++)
            all &= readable(text, c[grounds[j]]);
        if (all) { c[THEME_ACCENT_TEXT] = text; break; }
    }
    c[THEME_SELECTION] = c[THEME_SURFACE];
    for (unsigned i = 32; i > 0; i -= 4) {
        uint32_t bg = blend(c[THEME_SURFACE], accent, i);
        if (readable(c[THEME_FG], bg)) { c[THEME_SELECTION] = bg; break; }
    }
}

int theme_reload(void)
{
    struct theme_config config;
    theme_read(&config);
    int changed = !loaded || config.scheme != current.config.scheme ||
                  config.custom_accent != current.config.custom_accent || config.accent != current.config.accent;
    if (changed) theme_resolve(&config, &current);
    loaded = 1;
    return changed;
}

#if defined(__linux__)
int theme_watch_open(void)
{
    static const char path[] = RC_CONF;
    char directory[sizeof path];
    memcpy(directory, path, sizeof path);
    char *slash = strrchr(directory, '/');
    if (!slash || slash == directory) return -1;
    *slash = '\0';
    int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (fd >= 0 && inotify_add_watch(fd, directory, IN_MOVED_TO | IN_MOVED_FROM | IN_CLOSE_WRITE | IN_DELETE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int theme_watch_read(int fd)
{
    union { struct inotify_event alignment; char bytes[4096]; } events;
    const char *name = strrchr(RC_CONF, '/') + 1;
    int changed = 0;
    ssize_t n;
    while ((n = read(fd, events.bytes, sizeof events.bytes)) > 0) {
        for (size_t i = 0; i < (size_t)n;) {
            const struct inotify_event *ev = (const void *)(events.bytes + i);
            if ((ev->len && !strcmp(ev->name, name)) || (ev->mask & (IN_Q_OVERFLOW | IN_IGNORED))) changed = 1;
            i += sizeof *ev + ev->len;
        }
    }
    return changed && theme_reload();
}
#else
int theme_watch_open(void) { return -1; }
int theme_watch_read(int fd) { (void)fd; return 0; }
#endif

const struct theme_palette *theme_get(void)
{
    if (!loaded) theme_reload();
    return &current;
}

static int appearance_file(const char *old, size_t size, int exists, char **new, size_t *n, void *data)
{
    const struct theme_config *config = data;
    char text[96];
    int len = snprintf(text, sizeof text, "color_scheme=\"%s\"\naccent_color=\"#%06X\"\n",
                       config->scheme == THEME_LIGHT ? "light" : "dark", (unsigned)config->accent);
    if (exists && size == (size_t)len && memcmp(old, text, size) == 0) return 1;
    *new = strdup(text);
    if (!*new) return -1;
    *n = (size_t)len;
    return 0;
}

int theme_publish_appearance(void)
{
#if defined(__linux__)
    const struct theme_palette *p = theme_get();
    if (mkdir("/run/locachy", 0755) < 0 && errno != EEXIST) return -1;
    if (mkdir(THEME_APPEARANCE_DIR, 0755) < 0 && errno != EEXIST) return -1;
    return conf_transaction(THEME_APPEARANCE_DIR "/settings", appearance_file, (void *)&p->config);
#else
    (void)appearance_file;
    return -1;
#endif
}
