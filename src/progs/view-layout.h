/*
 * view-layout.h: view's arithmetic, apart from the screen and the renderer:
 * where the pages go (as Okular's PageView::slotRelayoutPages places them in
 * its continuous and paged modes), which are seen and in what order they are rendered,
 * Okular's zoom steps, and which rendered pictures the page cache drops.
 *
 * Pages are given in points (1/72 inch), unrotated. Items are the pages as
 * laid out: content pixels, rotated. Rotation is clockwise.
 */
#ifndef LOCACHY_VIEW_LAYOUT_H
#define LOCACHY_VIEW_LAYOUT_H

#include <stdint.h>

/* Okular's fixed, fit width/page and aspect-ratio-dependent auto fit modes. */
enum { VL_FIXED, VL_FIT_WIDTH, VL_FIT_PAGE, VL_FIT_AUTO };
enum { VL_SINGLE, VL_FACING, VL_FIRST_CENTERED, VL_OVERVIEW };

struct vl_params {
    int view_w, view_h;          /* the document area, pixels */
    int facing;                  /* VL_SINGLE, VL_FACING, VL_FIRST_CENTERED, VL_OVERVIEW */
    int rotate;                  /* 0, 90, 180 or 270 */
    int mode;                    /* VL_FIXED ... */
    double zoom;                 /* VL_FIXED: pixels a point */
    int col_margin, row_margin;  /* Okular's kcolWidthMargin and krowHeightMargin, in pixels */
    int paged, current, overview_cols; /* Continuous off; selected page; default overview columns 3 */
    int rtl;                     /* Okular's right to left reading direction */
};

struct vl_item {
    int x, y, w, h;              /* in the content, pixels */
    int ox, oy;                  /* crop origin in the full rotated rendered page */
    int hidden;                  /* another row in non-continuous mode */
    double zoom;                 /* pixels a point it is drawn at */
};

/* The n pages (pw[i] by ph[i] points) laid out into items, and the size of
 * the content; columns and rows as Okular's: a page fits in its cell, the
 * cells are at least the view's width (divided by the columns), facing
 * pages meet at the middle, and content lower than the view is centred. */
void vl_layout(const struct vl_params *p, int n, const float *pw, const float *ph,
               struct vl_item *items, int *full_w, int *full_h);

struct vl_crop { double x0, y0, x1, y1; };
/* Okular updateItemSize: rotate the normalized bounds, expand margins by
 * .04 * average dimension, clip, then enforce .5/.2 minimum dimensions. */
struct vl_crop vl_crop_box(struct vl_crop box, int rotate, int margins);
void vl_layout_crop(const struct vl_params *p, int n, const float *pw, const float *ph,
                    const struct vl_crop *crop, struct vl_item *items, int *full_w, int *full_h);

/* A page's size turned by rotate: *w by *h points. */
void vl_rotated(int rotate, float pw, float ph, float *w, float *h);

/* A point of a page (pw by ph points, unrotated) and the pixels of its item
 * drawn at zoom z turned by rotate, both ways. */
void vl_point_to_item(int rotate, float pw, float ph, double z, double px, double py, double *x, double *y);
void vl_item_to_point(int rotate, float pw, float ph, double z, double x, double y, double *px, double *py);

/* The item content pixel (x, y) is on, or -1; nearest: the nearest item
 * instead (in rows, then columns). */
int vl_item_at(int n, const struct vl_item *items, int x, int y, int nearest);

/* A screen pixel (X, Y) to a page and its point: the document area starts
 * at (ax, ay) on the screen and shows the content from (sx, sy). The page,
 * or -1. And a page's point to the screen. */
int vl_screen_to_page(const struct vl_params *p, int n, const struct vl_item *items, const float *pw,
                      const float *ph, int ax, int ay, int sx, int sy, int X, int Y, int nearest,
                      double *px, double *py);
void vl_page_to_screen(const struct vl_params *p, const struct vl_item *it, float pw, float ph, int ax, int ay,
                       int sx, int sy, double px, double py, double *X, double *Y);

/* The pages with a pixel in the view's rectangle (sx, sy, vw, vh), nearest
 * its middle first: up to max into out, how many. */
int vl_visible(int n, const struct vl_item *items, int sx, int sy, int vw, int vh, int *out, int max);
/* The order they are rendered in: the visible ones (as vl_visible), then
 * the row before and the row after them (cols pages a row), each once. *nvisible:
 * how many of out are visible. */
int vl_priority(int n, const struct vl_item *items, int cols, int sx, int sy, int vw, int vh, int *out,
                int max, int *nvisible);

/* Okular's zoom steps (PageView::updateZoom ZoomIn and ZoomOut): from the
 * factor cur (1: 100%), the next of its fixed factors and the fit-width and
 * fit-page factors, in or out. The new factor, *mode what it is (a fit
 * mode when it is one of those); cur itself at either end. */
double vl_zoom_step(double cur, double fit_width, double fit_page, int in, int *mode);
#define VL_ZOOM_MIN 0.12
#define VL_ZOOM_MAX 100.0
extern const double vl_zoom_values[16];

/* The page cache's choice: entries of bytes, used when last drawn or made
 * (a counter; 0: an empty slot). To add need bytes within limit: which to
 * drop, least recently used first, never one used at or after keep (what
 * the screen shows now): their indexes into out (room for n), how many;
 * -1 if even dropping all the others would not make room. */
struct vl_lru {
    uint64_t bytes, used;
};
int vl_evict(const struct vl_lru *e, int n, uint64_t limit, uint64_t need, uint64_t keep, int *out);

#endif
