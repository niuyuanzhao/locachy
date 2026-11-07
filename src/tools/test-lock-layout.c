/* Lock painting on native outputs of different sizes/scales, without PAM,
 * DRM, input devices or a running lock. Build with gc-sections: only draw
 * and the shared state it reads are linked from progs/lock.c. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define main lock_main
#include "../progs/lock.c"
#include "i18n-stub.h"
#undef main

static int extended_fixture = 1, selected, count = 2, fills[2], flushes;
static int columns[] = { 151, 160 }, row_count[] = { 42, 44 };
static int pixels_w[] = { 1366, 1920 }, pixels_h[] = { 768, 1080 };
static char picture[2][80][200];

int screen_cols(void) { return columns[selected]; }
int screen_rows(void) { return row_count[selected]; }
void screen_pixels(int *w, int *h) { *w = pixels_w[selected]; *h = pixels_h[selected]; }
int screen_is_extended(void) { return extended_fixture; }
int screen_output_count(void) { return count; }
int screen_output_current(void) { return selected; }
int screen_output_select(int i) {
    if (i < 0 || i >= count) return -1;
    int was = selected; selected = i; return was;
}
int ui_cells(const char *s) { return (int)strlen(s); }
void screen_fill_px(int x, int y, int w, int h, uint32_t color) {
    assert(x == 0 && y == 0 && w == pixels_w[selected] && h == pixels_h[selected] && color == BG);
    memset(picture[selected], ' ', sizeof picture[selected]);
    fills[selected]++;
}
void screen_text(int col, int row, const char *text, uint32_t fg, uint32_t bg) {
    (void)fg; (void)bg;
    assert(row >= 0 && row < row_count[selected]);
    for (int i = 0; text[i] && col + i < columns[selected]; i++) picture[selected][row][col + i] = text[i];
}
void screen_bar(const char *title, const char *ime) { (void)ime; assert(!strcmp(title, "locked")); }
void screen_flush(void) { flushes++; }

static int line_of(int output, const char *text) {
    for (int row = 0; row < row_count[output]; row++) {
        char line[201];
        memcpy(line, picture[output][row], (size_t)columns[output]);
        line[columns[output]] = '\0';
        if (strstr(line, text)) return row;
    }
    return -1;
}
static void check(void) {
    for (int i = 0; i < (extended_fixture ? count : 1); i++) {
        assert(fills[i] > 0);
        int prompt = line_of(i, "Password: ***_");
        int failure = line_of(i, "Wrong password.");
        int footer = line_of(i, "Enter: unlock");
        assert(prompt >= 0 && failure > prompt && failure < footer && footer == row_count[i] - 2);
        assert(line_of(i, "Fingerprint: waiting") > prompt);
        assert(line_of(i, "Face: waiting") > prompt);
    }
    assert(!strcmp(typed, "abc") && ntyped == 3 && ways[0].a.pid == 123 && ways[0].a.asking);
}
int main(void) {
    snprintf(user, sizeof user, "example");
    snprintf(typed, sizeof typed, "abc"); ntyped = 3;
    snprintf(status_line, sizeof status_line, "Wrong password.");
    ways[0].a.pid = 123; ways[0].a.asking = 1;
    snprintf(ways[0].a.prompt, sizeof ways[0].a.prompt, "Password:");
    ways[1].on = ways[2].on = 1;
    ways[1].a.pid = 124; ways[2].a.pid = 125;
    selected = 1;
    draw();
    check();
    assert(selected == 1 && flushes == 1);
    puts("PASS lock lays out one shared authentication state natively on 1366x768 and 1920x1080 outputs");
    /* A short, high-scale screen and a portrait canvas: native rows, no
     * fitted shared image, and no password/error pushed below the footer. */
    row_count[0] = 13; columns[0] = 48;
    pixels_w[1] = 1080; pixels_h[1] = 1920; row_count[1] = 79; columns[1] = 90;
    draw(); check();
    assert(selected == 1);
    puts("PASS lock compacts short screens independently while retaining a portrait output's native layout");
    count = 1; selected = 0;
    draw(); check();
    count = 2; selected = 1;
    draw(); check();
    assert(selected == 1 && ways[0].a.pid == 123 && !strcmp(typed, "abc"));
    puts("PASS lock redraw after output removal/reappearance preserves the password and PAM transaction");
    extended_fixture = 0; selected = 0;
    draw(); check();
    puts("PASS lock retains one safely mirrored layout when native per-output mode is unavailable");
    return 0;
}
