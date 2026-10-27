/*
 * view's arithmetic (progs/view-layout.c) on its own: where pages go in
 * continuous Single and Facing modes, the fit modes at several screen
 * sizes, rotation, screen pixels to a page's points and back, what is seen
 * and in what order it is rendered, Okular's zoom steps, and the page
 * cache's choice of what to drop. PASS lines; an assert ends it otherwise.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "view-layout.h"

#define N 10
static float pw[N], ph[N];
static struct vl_item it[N];

static struct vl_params params(int w, int h, int facing, int rotate, int mode, double zoom)
{
    return (struct vl_params){ w, h, facing, rotate, mode, zoom, 6, 12, 0, 0, 3, 0 };
}

static void letter(void)
{
    for (int i = 0; i < N; i++) {
        pw[i] = 612;
        ph[i] = 792;
    }
}

static int near(double a, double b, double eps)
{
    return fabs(a - b) <= eps;
}

int main(void)
{
    int fw, fh;
    letter();
    struct vl_crop b = vl_crop_box((struct vl_crop){ .2, .1, .8, .6 }, 0, 1);
    assert(near(b.x0, .178, 1e-12) && near(b.x1, .822, 1e-12));
    assert(near(b.y0, .078, 1e-12) && near(b.y1, .622, 1e-12));
    struct vl_crop rot = vl_crop_box((struct vl_crop){ .2, .1, .8, .6 }, 90, 1);
    assert(near(rot.x0, 1 - b.y1, 1e-12) && near(rot.y0, b.x0, 1e-12));
    b = vl_crop_box((struct vl_crop){ .95, .95, .99, .99 }, 0, 1);
    assert(near(b.x0, .5, 1e-12) && b.x1 == 1 && near(b.y1 - b.y0, .5, 1e-12));
    b = vl_crop_box((struct vl_crop){ .95, .95, .99, .99 }, 0, 0);
    assert(near(b.x0, .8, 1e-12) && b.x1 == 1 && near(b.y1 - b.y0, .2, 1e-12));
    b = vl_crop_box((struct vl_crop){ 0, 0, 0, 0 }, 270, 1);
    assert(b.x0 == 0 && b.y0 == 0 && b.x1 == 1 && b.y1 == 1);
    for (int r = 0; r < 360; r += 90) {
        struct vl_crop crops[2] = { vl_crop_box((struct vl_crop){ .15, .1, .7, .6 }, r, 1),
            vl_crop_box((struct vl_crop){ .4, .2, .6, .4 }, r, 1) };
        struct vl_params cp = params(1000, 700, 1, r, VL_FIT_PAGE, 1);
        ph[1] = 500;
        vl_layout_crop(&cp, 2, pw, ph, crops, it, &fw, &fh);
        for (int i = 0; i < 2; i++) {
            double X, Y, px, py;
            vl_page_to_screen(&cp, &it[i], pw[i], ph[i], 120, 40, 0, 0, 300, 200, &X, &Y);
            assert(vl_screen_to_page(&cp, 2, it, pw, ph, 120, 40, 0, 0, (int)X, (int)Y, 1, &px, &py) == i);
            assert(fabs(px - 300) <= 2 / it[i].zoom && fabs(py - 200) <= 2 / it[i].zoom);
        }
    }
    letter();
    puts("PASS exact Okular crop expansion, minimum dimensions, blank bounds, rotated unequal-page coordinate round trips");

    /* Continuous, Single Page, 100% of a point: pages in one column, each
     * centred, rows as high as the page and Okular's 12-pixel margin. */
    struct vl_params p = params(800, 600, 0, 0, VL_FIXED, 1.0);
    vl_layout(&p, 3, pw, ph, it, &fw, &fh);
    assert(fw == 800 && fh == 3 * (792 + 12));
    for (int i = 0; i < 3; i++) {
        assert(it[i].w == 612 && it[i].h == 792 && it[i].x == (800 - 612) / 2);
        assert(it[i].y == i * 804 + 6 && it[i].zoom == 1.0);
    }
    /* Wider than the screen: the content is the page and its margin. */
    p = params(500, 600, 0, 0, VL_FIXED, 1.0);
    vl_layout(&p, 3, pw, ph, it, &fw, &fh);
    assert(fw == 618 && it[0].x == 3);
    /* Lower than the screen: centred (Okular's insertY). */
    p = params(800, 2000, 0, 0, VL_FIXED, 1.0);
    vl_layout(&p, 1, pw, ph, it, &fw, &fh);
    assert(fh == 804 && it[0].y == (2000 - 804) / 2 + 6);
    puts("PASS continuous single page: centred columns, rows with margins, short content centred");

    /* Facing Pages: two to a row, meeting at the middle, the first on the left. */
    p = params(800, 600, 1, 0, VL_FIXED, 1.0);
    vl_layout(&p, 4, pw, ph, it, &fw, &fh);
    assert(fw == 2 * 618 && fh == 2 * 804);
    assert(it[0].x == fw / 2 - 612 - 1 && it[1].x == fw / 2 + 1);
    assert(it[0].y == it[1].y && it[2].y == it[0].y + 804 && it[2].x == it[0].x && it[3].x == it[1].x);
    /* A wide screen: the cells are half of it each. */
    p = params(2000, 900, 1, 0, VL_FIXED, 1.0);
    vl_layout(&p, 3, pw, ph, it, &fw, &fh);
    assert(fw == 2000 && it[0].x == 1000 - 612 - 1 && it[1].x == 1001 && it[2].x == it[0].x);
    assert(fh == 2 * 804 && it[2].y == 804 + 6);
    /* One page: as Okular's Facing of one page, it is centred alone. */
    vl_layout(&p, 1, pw, ph, it, &fw, &fh);
    assert(fw == 2000 && it[0].x == (2000 - 612) / 2);
    /* A row of two different heights: as high as the highest, each centred in it. */
    ph[1] = 500;
    p = params(1400, 700, 1, 0, VL_FIXED, 1.0);
    vl_layout(&p, 2, pw, ph, it, &fw, &fh);
    assert(fh == 804 && it[0].y == 6 && it[1].y == (804 - 500) / 2);
    letter();
    puts("PASS facing pages: pairs meeting at the middle, odd last page, a single page centred");

    /* Fit Width at several screen sizes: each page as wide as its column less 6. */
    int widths[] = { 1366, 1920, 800, 1024 };
    for (int k = 0; k < 4; k++) {
        for (int facing = 0; facing < 2; facing++) {
            p = params(widths[k], 700, facing, 0, VL_FIT_WIDTH, 0);
            vl_layout(&p, 4, pw, ph, it, &fw, &fh);
            int avail = widths[k] / (facing ? 2 : 1) - 6;
            assert(it[0].w == avail && near(it[0].zoom, avail / 612.0, 1e-9));
            assert(it[0].h == (int)(792 * it[0].zoom + 0.5));
            assert(fw == widths[k] - (facing ? widths[k] % 2 : 0));
            assert(it[0].x >= 0 && it[facing].x + it[facing].w <= fw);
        }
    }
    /* Fit Page: the whole page in sight, whichever side limits it. */
    p = params(1000, 700, 0, 0, VL_FIT_PAGE, 0);
    vl_layout(&p, 2, pw, ph, it, &fw, &fh);
    assert(near(it[0].zoom, 688 / 792.0, 1e-9) && it[0].h == 688 && it[0].w == (int)(612 * 688 / 792.0 + 0.5));
    assert(it[0].h + 12 <= 700);
    p = params(500, 1400, 0, 0, VL_FIT_PAGE, 0);
    vl_layout(&p, 2, pw, ph, it, &fw, &fh);
    assert(near(it[0].zoom, 494 / 612.0, 1e-9) && it[0].w == 494);
    p = params(1366, 680, 1, 0, VL_FIT_PAGE, 0);
    vl_layout(&p, 2, pw, ph, it, &fw, &fh);
    assert(near(it[0].zoom, fmin(677 / 612.0, 668 / 792.0), 1e-9) && it[0].y >= 0 && fh <= 680);
    puts("PASS fit width and fit page at 1366, 1920, 800 and 1024 pixels wide, single and facing");
    p = params(1000, 700, VL_FIRST_CENTERED, 0, VL_FIT_PAGE, 1);
    vl_layout(&p, 4, pw, ph, it, &fw, &fh);
    assert(it[0].x == (fw - it[0].w) / 2 && it[3].x == (fw - it[3].w) / 2);
    assert(it[1].y == it[2].y && it[1].x < it[2].x && it[0].y < it[1].y && it[3].y > it[2].y);
    p.paged = 1; p.current = 2;
    vl_layout(&p, 4, pw, ph, it, &fw, &fh);
    assert(it[0].hidden && !it[1].hidden && !it[2].hidden && it[3].hidden && fh <= 700);
    p = params(1200, 700, VL_OVERVIEW, 0, VL_FIT_WIDTH, 1);
    vl_layout(&p, 5, pw, ph, it, &fw, &fh);
    assert(it[0].y == it[2].y && it[0].x < it[1].x && it[1].x < it[2].x && it[3].y > it[0].y);
    p = params(1200, 500, VL_SINGLE, 0, VL_FIT_AUTO, 1);
    vl_layout(&p, 2, pw, ph, it, &fw, &fh);
    assert(near(it[0].zoom, 1194 / 612.0, 1e-9));
    p = params(500, 1200, VL_SINGLE, 0, VL_FIT_AUTO, 1); p.paged = 1;
    vl_layout(&p, 2, pw, ph, it, &fw, &fh);
    assert(near(it[0].zoom, 1188 / 792.0, 1e-9));
    puts("PASS first-centered facing, overview three-column grid, independent continuous and exact 1.25-ratio auto fit");

    /* Overview columns (okular.kcfg ViewColumns 1..8; fewer pages than
     * columns: as many columns as pages) and right to left reading. */
    for (int cols = 1; cols <= 8; cols++) {
        p = params(1600, 700, VL_OVERVIEW, 0, VL_FIT_WIDTH, 1);
        p.overview_cols = cols;
        vl_layout(&p, N, pw, ph, it, &fw, &fh);
        for (int i = 1; i < N; i++) assert(i % cols ? it[i].y == it[i - 1].y && it[i].x > it[i - 1].x : it[i].y > it[i - 1].y);
        assert(it[0].w == 1600 / cols - 6);
        p.rtl = 1;
        struct vl_item ltr[N];
        for (int i = 0; i < N; i++) ltr[i] = it[i];
        vl_layout(&p, N, pw, ph, it, &fw, &fh);
        for (int i = 0; i < N; i++) assert(it[i].y == ltr[i].y && it[i].x == fw - ltr[i].x - ltr[i].w);
    }
    p = params(1600, 700, VL_OVERVIEW, 0, VL_FIXED, 0.5);
    p.overview_cols = 8;
    vl_layout(&p, 3, pw, ph, it, &fw, &fh);
    assert(it[0].y == it[2].y && fw == 3 * (1600 / 3) && it[2].x > it[1].x);
    p = params(1400, 700, VL_FACING, 0, VL_FIXED, 1.0);
    p.rtl = 1;
    vl_layout(&p, 4, pw, ph, it, &fw, &fh);
    assert(it[0].x == fw / 2 + 1 && it[1].x == fw / 2 - 612 - 1 && it[2].x == it[0].x);
    p.facing = VL_FIRST_CENTERED;
    vl_layout(&p, 5, pw, ph, it, &fw, &fh);
    assert(it[0].x == (fw - 612) / 2 && it[1].x == fw / 2 + 1 && it[2].x == fw / 2 - 612 - 1);
    puts("PASS overview columns 1 to 8 (pages fewer than columns: that many), right to left: facing sides swapped, columns mirrored");

    /* Rotation: 90 and 270 swap the width and the height; 180 does not. */
    for (int r = 0; r < 360; r += 90) {
        p = params(2000, 600, 0, r, VL_FIXED, 1.5);
        vl_layout(&p, 1, pw, ph, it, &fw, &fh);
        int turned = r == 90 || r == 270;
        assert(it[0].w == (int)((turned ? 792 : 612) * 1.5 + 0.5) && it[0].h == (int)((turned ? 612 : 792) * 1.5 + 0.5));
        p = params(1000, 600, 0, r, VL_FIT_WIDTH, 0);
        vl_layout(&p, 1, pw, ph, it, &fw, &fh);
        assert(near(it[0].zoom, 994.0 / (turned ? 792 : 612), 1e-9));
    }
    float w, h;
    vl_rotated(270, 612, 792, &w, &h);
    assert(w == 792 && h == 612);
    puts("PASS rotation: 90 and 270 swap width and height, also in fit width");

    /* A page's point to its item and back, each rotation: the page's top
     * left corner goes where a clockwise turn takes it. */
    double x, y, px, py;
    vl_point_to_item(0, 612, 792, 2, 0, 0, &x, &y);
    assert(x == 0 && y == 0);
    vl_point_to_item(90, 612, 792, 2, 0, 0, &x, &y);
    assert(x == 792 * 2 && y == 0); /* the top right */
    vl_point_to_item(180, 612, 792, 2, 0, 0, &x, &y);
    assert(x == 612 * 2 && y == 792 * 2); /* the bottom right */
    vl_point_to_item(270, 612, 792, 2, 0, 0, &x, &y);
    assert(x == 0 && y == 612 * 2); /* the bottom left */
    vl_point_to_item(90, 612, 792, 2, 100, 50, &x, &y);
    assert(x == (792 - 50) * 2 && y == 100 * 2);
    for (int r = 0; r < 360; r += 90) {
        vl_point_to_item(r, 612, 792, 1.75, 123.5, 456.25, &x, &y);
        vl_item_to_point(r, 612, 792, 1.75, x, y, &px, &py);
        assert(near(px, 123.5, 1e-9) && near(py, 456.25, 1e-9));
    }
    /* Screen pixels to a page and back: the area at (300, 50), scrolled. */
    for (int facing = 0; facing < 2; facing++) {
        for (int r = 0; r < 360; r += 90) {
            p = params(1200, 700, facing, r, VL_FIXED, 1.3);
            vl_layout(&p, 6, pw, ph, it, &fw, &fh);
            int sx = fw > 1200 ? 40 : 0, sy = 900;
            for (int i = 0; i < 6; i++) {
                double X, Y;
                vl_page_to_screen(&p, &it[i], pw[i], ph[i], 300, 50, sx, sy, 200.25, 333.5, &X, &Y);
                int page = vl_screen_to_page(&p, 6, it, pw, ph, 300, 50, sx, sy, (int)floor(X), (int)floor(Y), 0,
                                             &px, &py);
                assert(page == i && near(px, 200.25, 1 / 1.3) && near(py, 333.5, 1 / 1.3));
            }
            /* Between the pages: on none, or (nearest) on the nearest one. */
            int gap_y = 50 - sy + it[2].y - 3;
            assert(vl_screen_to_page(&p, 6, it, pw, ph, 300, 50, sx, sy, 300 - sx + it[2].x + 5, gap_y, 0, &px, &py) < 0);
            int page = vl_screen_to_page(&p, 6, it, pw, ph, 300, 50, sx, sy, 300 - sx + it[2].x + 5, gap_y, 1, &px, &py);
            assert(page == 2 && py >= 0);
        }
    }
    puts("PASS screen pixel to page point and back: rotations 0, 90, 180, 270, single and facing, gaps");

    /* What is seen, nearest the middle first, and the render order. */
    int out[16], nvis, n;
    p = params(800, 600, 0, 0, VL_FIXED, 1.0);
    vl_layout(&p, N, pw, ph, it, &fw, &fh);
    n = vl_visible(N, it, 0, 804, 800, 600, out, 16);
    assert(n == 1 && out[0] == 1);
    n = vl_visible(N, it, 0, 1300, 800, 600, out, 16);
    assert(n == 2 && out[0] == 1 && out[1] == 2); /* centre 1600: page 1's 1206, page 2's 2010 */
    n = vl_visible(N, it, 0, 1500, 800, 600, out, 16);
    assert(n == 2 && out[0] == 2 && out[1] == 1);
    n = vl_priority(N, it, 1, 0, 1300, 800, 600, out, 16, &nvis);
    assert(nvis == 2 && n == 4 && out[2] == 0 && out[3] == 3);
    n = vl_priority(N, it, 1, 0, 0, 800, 600, out, 16, &nvis);
    assert(nvis == 1 && out[0] == 0 && n == 2 && out[1] == 1); /* nothing before the first */
    p = params(800, 600, 1, 0, VL_FIXED, 1.0);
    vl_layout(&p, N, pw, ph, it, &fw, &fh);
    n = vl_priority(N, it, 2, 0, 804 + 100, 800, 600, out, 16, &nvis);
    assert(nvis == 2 && (out[0] == 2 || out[0] == 3) && n == 6);
    assert(out[2] + out[3] == 0 + 1 && out[4] + out[5] == 4 + 5); /* the rows before and after */
    puts("PASS visible pages nearest the middle first; then a row before and after (single and facing)");

    /* Okular's zoom steps, with fit width 1.7 and fit page 0.85 among them. */
    int mode;
    assert(vl_zoom_step(1.0, 1.7, 0.85, 1, &mode) == 1.25 && mode == VL_FIXED);
    assert(vl_zoom_step(1.0, 1.7, 0.85, 0, &mode) == 0.85 && mode == VL_FIT_PAGE);
    assert(vl_zoom_step(1.6, 1.7, 0.85, 1, &mode) == 1.7 && mode == VL_FIT_WIDTH);
    assert(vl_zoom_step(1.7, 1.7, 0.85, 1, &mode) == 2.0 && mode == VL_FIXED);
    assert(vl_zoom_step(0.12, 1.7, 0.85, 0, &mode) == 0.12 && mode == -1);
    assert(vl_zoom_step(100, 1.7, 0.85, 1, &mode) == 100 && mode == -1);
    assert(vl_zoom_step(0.5, 0.5, 0.5, 1, &mode) == 0.66 && mode == VL_FIXED);
    puts("PASS zoom steps: Okular's factors with the fit factors among them, none past the ends");

    /* The cache: within the limit nothing goes; else the least recently
     * used first, never what the screen shows (used at or after keep). */
    struct vl_lru e[6] = { { 30, 5 }, { 20, 2 }, { 10, 9 }, { 0, 0 }, { 25, 3 }, { 15, 12 } };
    int drop[6];
    assert(vl_evict(e, 6, 200, 50, 10, drop) == 0);          /* 100 + 50 within 200 */
    n = vl_evict(e, 6, 120, 50, 10, drop);                   /* 30 to go */
    assert(n == 2 && drop[0] == 1 && drop[1] == 4);          /* used 2, then 3 */
    n = vl_evict(e, 6, 100, 50, 10, drop);                   /* 50 to go */
    assert(n == 3 && drop[0] == 1 && drop[1] == 4 && drop[2] == 0);
    assert(vl_evict(e, 6, 60, 50, 10, drop) == -1);          /* 15 (used 12) must stay: no room */
    assert(vl_evict(e, 6, 65, 50, 13, drop) == 4);           /* keep later: then it may go too */
    uint64_t total = 0;
    for (int k = 0; k < 6; k++) total += e[k].bytes;
    for (int k = 0; k < 4; k++) total -= e[drop[k]].bytes;
    assert(total + 50 <= 65 && drop[3] == 2);
    puts("PASS cache: the byte bound kept, least recently used dropped first, what is on the screen kept");
    puts("ALL PASSED");
    return 0;
}
