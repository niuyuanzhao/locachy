/*
 * test-screen-bar OUT.ppm [SCALE]: screen.c's picture without a screen: its
 * shadow buffer, drawn as home draws (the background, the bar, text in rows 1
 * and 2) at that scale (1 if none), written as a PPM to look at.
 */
#include "../base/screen.c"

int monitor_conf(const char *name, struct monitor_conf *out)
{
    (void)name;
    memset(out, 0, sizeof *out);
    return 0;
}
int monitor_internal(const char *name) { (void)name; return 0; }

int main(int argc, char **argv)
{
    snprintf(font_fallback, sizeof font_fallback, "%s", BASE_FONT);
    scale = argc == 3 ? strtod(argv[2], NULL) : 1.0;
    if (argc < 2 || argc > 3 || scale < 0.5 || scale > 4 || scaled_font(scale) != NULL) {
        fprintf(stderr, "usage: test-screen-bar OUT.ppm [SCALE] (with %s)\n", BASE_FONT_DIR);
        return 2;
    }
#ifdef CONFIG_FREETYPE
    ft_init();
#endif
    width = 1280;
    height = 200;
    shadow = calloc((size_t)width * height, 4);
    layout();
    screen_fill(0, 0, cols, rows, 0x101820);
    screen_bar("home", NULL);
    screen_text(1, 1, "1 containers   0 marked (operation targets only)", 0x80C0FF, 0x101820);
    screen_text(1, 2, "    Container        State     RAM MiB  Swap MiB", 0xD0D0D0, 0x101820);
    screen_text(1, 3, "中文字幕 (CJK face)  ─│┌┐ (box drawing)  … ✓", 0xD0D0D0, 0x101820);
    FILE *out = fopen(argv[1], "w");
    if (!out) return 1;
    fprintf(out, "P6\n%u %u\n255\n", width, height);
    for (uint32_t i = 0; i < width * height; i++) {
        uint8_t rgb[3] = { (uint8_t)(shadow[i] >> 16), (uint8_t)(shadow[i] >> 8), (uint8_t)shadow[i] };
        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
    printf("scale %.2f: font %ux%u, cells %dx%d, row 0 at y=%u, the bar %u px\n",
           scale, glyph_w, glyph_h, cols, rows, top, bar_h);
    return 0;
}
