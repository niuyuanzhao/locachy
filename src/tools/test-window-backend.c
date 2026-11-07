/* A program as the base's programs are, opened as a window of a headless
 * labwc (LOCACHY_BACKEND=wayland): one line on stdout for each thing it
 * gets, for test-window-backend.sh. Its whole canvas is one colour and it
 * draws the top bar as programs do, so a screenshot finds the window and
 * shows whether the bar is there. k: answer the next closes with
 * key_close_kept(); u: no longer; q: end. */
#include <stdio.h>

#include "base.h"
#ifdef CONFIG_WAYLAND
#include "include/wayland-backend.h"
#endif

#define COLOUR 0x2A6FDBu

int main(void)
{
    int keep = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    screen_open(BASE_FONT);
    keys_open();
    for (;;) {
        int key = key_get();
        if (key == K_SHOWN) {
            int w, h, gw, gh, top, decoration = 0;
            screen_pixels(&w, &h);
            screen_cells(&gw, &gh, &top);
#ifdef CONFIG_WAYLAND
            decoration = wl_backend_decoration();
#endif
            screen_fill_px(0, 0, w, h, COLOUR);
            screen_bar("window test", NULL);
            screen_text(0, 2, "window backend", 0xFFFFFF, COLOUR);
            screen_flush();
            printf("SHOWN %d %d %d %d %d %d %d\n", w, h, screen_cols(), screen_rows(), top, gh, decoration);
        } else if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (e->button) printf("BUTTON %d %d %d %d %d\n", e->button, e->pressed, e->clicks, e->x, e->y);
            else if (e->wheel || e->hwheel) printf("WHEEL %d %d\n", e->wheel, e->hwheel);
            else printf("MOTION %d %d\n", e->x, e->y);
        } else if (key == K_CLOSE) {
            printf("CLOSE\n");
            if (keep) {
                key_close_kept();
                printf("KEPT\n");
            }
        } else if (key == K_TEXT) {
            printf("TEXT %s\n", keys_text());
        } else if (key != 0) {
            printf("KEY %d %d\n", key, keys_modifiers());
            if (key == 'k') keep = 1;
            if (key == 'u') keep = 0;
            if (key == 'q') return 0;
        }
    }
}
