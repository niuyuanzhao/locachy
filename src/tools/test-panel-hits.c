/* The status blocks' click areas from the real panel_draw. Fonts are made
 * in memory (no font files are read); status text is fixed.
 * gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc/base/include -Isrc/base src/tools/test-panel-hits.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../base/panel.c"

static int with_bluetooth = 1;

void base_status(const char *ime, char *out, size_t size)
{
    (void)ime;
    snprintf(out, size, "status");
}

void base_status_fields(struct status_fields *s)
{
    memset(s, 0, sizeof *s);
    snprintf(s->volume, sizeof s->volume, "Vol 50%%");
    snprintf(s->mic, sizeof s->mic, "Mic 100%%");
    if (with_bluetooth) {
        snprintf(s->bluetooth, sizeof s->bluetooth, "BT on");
        snprintf(s->bt_device, sizeof s->bt_device, "idle");
    }
    snprintf(s->network, sizeof s->network, "Wi-Fi Home");
    snprintf(s->address, sizeof s->address, "192.168.1.23");
    snprintf(s->battery, sizeof s->battery, "Bat 80%%");
    snprintf(s->power, sizeof s->power, "7.5 W, 3h20m");
    snprintf(s->memory, sizeof s->memory, "RAM 3.1/15.4G");
    snprintf(s->swap, sizeof s->swap, "Swap 0.0/20.0G");
    snprintf(s->cpu, sizeof s->cpu, "CPU 12%%");
    snprintf(s->temp, sizeof s->temp, "52C");
    snprintf(s->time, sizeof s->time, "03:18:51");
    snprintf(s->date, sizeof s->date, "Tue 2026-10-20");
}

/* Every size loaded, half as wide as high, with blank glyphs. */
static uint8_t blank[256 * 4 * 64];

static void memory_fonts(void)
{
    for (int k = 0; k < 2 * NSIZES; k++) {
        struct panel_font *f = &fonts[k];
        memset(f, 0, sizeof *f);
        f->loaded = 1;
        f->size = sizes[k / 2];
        f->bold = k % 2;
        f->mag = 1;
        f->w = (uint32_t)sizes[k / 2] / 2;
        f->h = (uint32_t)sizes[k / 2];
        f->row_bytes = (f->w + 7) / 8;
        f->bytes = f->row_bytes * f->h;
        f->count = 256;
        f->glyphs = blank;
    }
}

static int find(const struct panel_hit *h, int n, enum panel_block id)
{
    for (int i = 0; i < n; i++) {
        if (h[i].id == id) return i;
    }
    return -1;
}

static uint32_t px[3000 * 64];

static int draw(int w, int h, int *x0, int *x1, struct panel_hit *hit)
{
    panel_draw(px, w, h, w, "home", "", "RAW", 0, x0, x1);
    return panel_hits(hit, 16);
}

static void ordered(int w, int h)
{
    struct panel_hit hit[16];
    int x0, x1, n = draw(w, h, &x0, &x1, hit);
    const enum panel_block right[] = { PANEL_BLOCK_VOLUME, PANEL_BLOCK_BLUETOOTH, PANEL_BLOCK_NETWORK,
                                       PANEL_BLOCK_BATTERY, PANEL_BLOCK_TIME };
    assert(n == 6);
    int previous = x1;
    for (size_t i = 0; i < sizeof right / sizeof *right; i++) {
        int k = find(hit, n, right[i]);
        assert(k >= 0 && hit[k].x0 < hit[k].x1 && hit[k].x0 >= previous && hit[k].x1 <= w);
        previous = hit[k].x1;
    }
    /* LOCachy, from the left edge; the title, memory and CPU blocks after it
     * are readouts only and have no click area. */
    int logo = find(hit, n, PANEL_BLOCK_LOCACHY);
    assert(logo >= 0 && hit[logo].x0 == 0 && hit[logo].x1 > 0 && hit[logo].x1 < x0);
    for (int i = 0; i < n; i++) {
        if (i != logo) assert(hit[i].x0 >= x1);
        assert(!(hit[i].x0 <= x1 && x1 < hit[i].x1));
    }
}

int main(void)
{
    memory_fonts();
    ordered(1920, 36);
    ordered(2880, 54); /* scale 1.5: the same blocks in buffer pixels */
    puts("PASS each status block's area in drawing order, LOCachy's at the left edge, the input method's own block "
         "excluded, at scale 1 and 1.5");

    struct panel_hit hit[16];
    int x0, x1;
    /* The launcher remains on LOCachy's block, not a foreground program's
     * title: view, settings and mixer use the same panel full screen. */
    static const char *const titles[] = { "view", "settings", "mixer" };
    for (int scale = 1; scale <= 2; scale++) {
        int w = scale == 1 ? 1920 : 2880, h = scale == 1 ? 36 : 54;
        int n = draw(w, h, &x0, &x1, hit);
        int logo = find(hit, n, PANEL_BLOCK_LOCACHY), end = hit[logo].x1;
        for (size_t i = 0; i < sizeof titles / sizeof *titles; i++) {
            panel_draw(px, w, h, w, titles[i], "", "RAW", 0, &x0, &x1);
            n = panel_hits(hit, 16);
            logo = find(hit, n, PANEL_BLOCK_LOCACHY);
            assert(n == 6 && logo >= 0 && hit[logo].x0 == 0 && hit[logo].x1 == end);
        }
    }
    puts("PASS LOCachy's launcher area stays separate from view, settings and mixer titles at scale 1 and 1.5");
    with_bluetooth = 0;
    int n = draw(1920, 36, &x0, &x1, hit);
    assert(n == 5 && find(hit, n, PANEL_BLOCK_BLUETOOTH) < 0);
    with_bluetooth = 1;
    puts("PASS no Bluetooth block, no Bluetooth area");

    n = draw(420, 36, &x0, &x1, hit);
    assert(n > 0 && n < 6 && find(hit, n, PANEL_BLOCK_TIME) >= 0 && find(hit, n, PANEL_BLOCK_LOCACHY) >= 0);
    for (int i = 0; i < n; i++) {
        assert(hit[i].x0 >= 0 && hit[i].x1 <= 420 && hit[i].x0 < hit[i].x1);
    }
    n = draw(1920, 36, &x0, &x1, hit);
    assert(n == 6);
    puts("PASS a narrow bar has areas only for the blocks it draws, and the next draw replaces them");
    return 0;
}
