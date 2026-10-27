/*
 * test-screen-blit: screen.c's pictures without a screen: screen_blit and
 * screen_fill_px clipped to its shadow buffer at every edge, the alpha byte
 * dropped, and screen_char_cells as screen_text draws (2 for CJK, else 1).
 * Build with -DCONFIG_FREETYPE as make.sh does.
 */
#include "../base/screen.c"
#include <assert.h>

int monitor_conf(const char *name, struct monitor_conf *out)
{
    (void)name;
    memset(out, 0, sizeof *out);
    return 0;
}
int monitor_internal(const char *name) { (void)name; return 0; }

static uint32_t at(int x, int y) { return shadow[(size_t)y * width + (size_t)x]; }

int main(void)
{
    width = 64;
    height = 48;
    shadow = calloc((size_t)width * height, 4);
    uint32_t px[10 * 8];
    for (int i = 0; i < 10 * 8; i++) px[i] = 0xAB000000u | (uint32_t)(i + 1);

    screen_blit(5, 6, 10, 8, px, 10);
    assert(at(5, 6) == 1 && at(14, 6) == 10 && at(5, 13) == 71 && at(14, 13) == 80);
    assert(at(4, 6) == 0 && at(15, 6) == 0 && at(5, 5) == 0 && at(5, 14) == 0);
    puts("PASS a picture is drawn where it is put, without its alpha byte");

    memset(shadow, 0, (size_t)width * height * 4);
    screen_blit(-3, -2, 10, 8, px, 10);
    assert(at(0, 0) == 2 * 10 + 4 && at(6, 5) == 7 * 10 + 10 && at(7, 0) == 0 && at(0, 6) == 0);
    screen_blit(60, 44, 10, 8, px, 10);
    assert(at(63, 47) == 3 * 10 + 4 && at(60, 44) == 1);
    screen_blit(-20, 0, 10, 8, px, 10);
    screen_blit(64, 0, 10, 8, px, 10);
    screen_blit(0, 48, 10, 8, px, 10);
    screen_blit(0, 0, 0, 8, px, 10);
    puts("PASS pictures cut at the left, top, right and bottom; wholly outside or empty: nothing");

    uint32_t wide[4 * 2] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    memset(shadow, 0, (size_t)width * height * 4);
    screen_blit(0, 0, 2, 2, wide, 4);
    assert(at(0, 0) == 1 && at(1, 0) == 2 && at(0, 1) == 5 && at(1, 1) == 6 && at(2, 0) == 0);
    puts("PASS the stride: a part of a wider picture");

    screen_fill_px(-5, 40, 10, 20, 0xFF123456u);
    assert(at(0, 40) == 0x123456 && at(4, 47) == 0x123456 && at(5, 40) == 0 && at(0, 39) == 0);
    screen_fill_px(10, 10, -1, 5, 0x00FFFFFF);
    assert(at(10, 10) == 0);
    puts("PASS filled rectangles of pixels, clipped");

#ifdef CONFIG_FREETYPE
    assert(screen_char_cells(0x4E2D) == 2 && screen_char_cells(0xFF0C) == 2 && screen_char_cells(0x1F600) == 2);
#endif
    assert(screen_char_cells('a') == 1 && screen_char_cells(0xE9) == 1 && screen_char_cells(0x2500) == 1);
    puts("PASS character cells as screen_text draws them");
    puts("ALL PASSED");
    return 0;
}
