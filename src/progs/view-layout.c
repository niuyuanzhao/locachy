/*
 * view-layout.c: see view-layout.h. Okular's numbers are from its
 * part/pageview.cpp (slotRelayoutPages, updateItemSize, updateZoom).
 */
#include <stdlib.h>
#include <math.h>

#include "view-layout.h"

const double vl_zoom_values[16] = { 0.12, 0.25, 0.33, 0.50, 0.66, 0.75, 1.00, 1.25,
                                    1.50, 2.00, 4.00, 8.00, 16.00, 25.00, 50.00, 100.00 };

void vl_rotated(int rotate, float pw, float ph, float *w, float *h)
{
    int turned = rotate == 90 || rotate == 270;
    *w = turned ? ph : pw;
    *h = turned ? pw : ph;
}

struct vl_crop vl_crop_box(struct vl_crop b, int rotate, int margins)
{
    if (!isfinite(b.x0) || !isfinite(b.x1) || !isfinite(b.y0) || !isfinite(b.y1) ||
        b.x1 <= b.x0 || b.y1 <= b.y0) return (struct vl_crop){ 0, 0, 1, 1 };
    for (int r = 0; r < rotate; r += 90) {
        struct vl_crop a = b;
        b = (struct vl_crop){ 1 - a.y1, a.x0, 1 - a.y0, a.x1 };
    }
    double e = margins ? .04 * ((b.x1 - b.x0) + (b.y1 - b.y0)) / 2 : 0;
    b.x0 = fmax(0, b.x0 - e); b.x1 = fmin(1, b.x1 + e);
    b.y0 = fmax(0, b.y0 - e); b.y1 = fmin(1, b.y1 + e);
    double min = margins ? .5 : .2;
    if (b.x1 - b.x0 < min) {
        b.x0 = fmax(0, fmin(1 - min, (b.x0 + b.x1 - min) / 2));
        b.x1 = b.x0 + min;
    }
    if (b.y1 - b.y0 < min) {
        b.y0 = fmax(0, fmin(1 - min, (b.y0 + b.y1 - min) / 2));
        b.y1 = b.y0 + min;
    }
    return b;
}

void vl_layout(const struct vl_params *p, int n, const float *pw, const float *ph,
               struct vl_item *items, int *full_w, int *full_h)
{
    vl_layout_crop(p, n, pw, ph, NULL, items, full_w, full_h);
}

void vl_layout_crop(const struct vl_params *p, int n, const float *pw, const float *ph,
                    const struct vl_crop *crop, struct vl_item *items, int *full_w, int *full_h)
{
    int centered = p->facing == VL_FIRST_CENTERED && n >= 3;
    int cols = p->facing == VL_OVERVIEW ? (p->overview_cols > 0 ? p->overview_cols : 3) :
        (p->facing == VL_FACING && n > 1) || centered ? 2 : 1;
    if (cols > 8) cols = 8;
    if (p->facing == VL_OVERVIEW && n > 0 && n < cols) cols = n; /* Okular's viewColumns() */
    int offset = centered ? cols - 1 : 0, rows = (n + offset + cols - 1) / cols;
    int *rh = calloc((size_t)(rows > 0 ? rows : 1), sizeof *rh);
    int colw[8], avail_w = p->view_w / cols - p->col_margin, avail_h = p->view_h - p->row_margin;
    if (!rh) { *full_w = *full_h = 0; return; }
    if (avail_w < 1) avail_w = 1;
    if (avail_h < 1) avail_h = 1;
    for (int c = 0; c < cols; c++) colw[c] = p->view_w / cols;
    for (int i = 0; i < n; i++) {
        float w, h;
        vl_rotated(p->rotate, pw[i] > 0 ? pw[i] : 1, ph[i] > 0 ? ph[i] : 1, &w, &h);
        float fullw = w, fullh = h;
        struct vl_crop b = crop ? crop[i] : (struct vl_crop){ 0, 0, 1, 1 };
        w *= b.x1 - b.x0; h *= b.y1 - b.y0;
        double z = p->zoom, zw = avail_w / (double)w, zh = avail_h / (double)h;
        if (p->mode == VL_FIT_WIDTH) z = zw;
        else if (p->mode == VL_FIT_PAGE) z = fmin(zw, zh);
        else if (p->mode == VL_FIT_AUTO) {
            double rel = ((double)avail_h / avail_w) / ((double)h / w);
            z = p->paged && rel > 1.25 ? zh : rel < 1 / 1.25 ? zw : fmin(zw, zh);
        }
        if (!(z > 1e-4)) z = 1e-4;
        float zf = (float)z;
        items[i].zoom = z;
        items[i].ox = (int)lround(b.x0 * fullw * zf);
        items[i].oy = (int)lround(b.y0 * fullh * zf);
        items[i].w = p->mode == VL_FIT_WIDTH ? avail_w : (int)(w * zf + .5f);
        items[i].h = (int)(h * zf + .5f);
        if (items[i].w < 1) items[i].w = 1;
        if (items[i].h < 1) items[i].h = 1;
        int rendered_w = (int)(fullw * zf + .5f), rendered_h = (int)(fullh * zf + .5f);
        if (items[i].ox + items[i].w > rendered_w) items[i].ox = rendered_w - items[i].w;
        if (items[i].oy + items[i].h > rendered_h) items[i].oy = rendered_h - items[i].h;
        if (items[i].ox < 0) items[i].ox = 0;
        if (items[i].oy < 0) items[i].oy = 0;
        int c = (i + offset) % cols, r = (i + offset) / cols;
        if (items[i].w + p->col_margin > colw[c]) colw[c] = items[i].w + p->col_margin;
        if (items[i].h + p->row_margin > rh[r]) rh[r] = items[i].h + p->row_margin;
    }
    *full_w = 0;
    for (int c = 0; c < cols; c++) *full_w += colw[c];
    int current = p->current < 0 ? 0 : p->current >= n ? n - 1 : p->current;
    int selected_row = (current + offset) / cols, total = 0;
    if (p->paged) total = rh[selected_row];
    else for (int r = 0; r < rows; r++) total += rh[r];
    *full_h = total;
    int y0 = total < p->view_h ? (p->view_h - total) / 2 : 0, y = y0;
    for (int i = 0; i < n; i++) {
        int c = (i + offset) % cols, r = (i + offset) / cols;
        items[i].hidden = p->paged && r != selected_row;
        int x = 0;
        for (int k = 0; k < c; k++) x += colw[k];
        /* Right to left (Okular's slotRelayoutPages): the facing pages swap
         * sides, the other columns are mirrored. */
        if (centered && (i == 0 || (i == n - 1 && n % 2 == 0)))
            items[i].x = (*full_w - items[i].w) / 2;
        else if (cols == 2 && p->facing != VL_OVERVIEW)
            items[i].x = (c == 0) != (p->rtl != 0) ? *full_w / 2 - items[i].w - 1 : *full_w / 2 + 1;
        else if (p->rtl) items[i].x = *full_w - x - colw[c] + (colw[c] - items[i].w) / 2;
        else items[i].x = x + (colw[c] - items[i].w) / 2;
        items[i].y = (p->paged ? y0 : y) + (rh[r] - items[i].h) / 2;
        if (c == cols - 1) y += rh[r];
    }
    free(rh);
}

void vl_point_to_item(int rotate, float pw, float ph, double z, double px, double py, double *x, double *y)
{
    switch (rotate) {
    case 90:
        *x = (ph - py) * z;
        *y = px * z;
        break;
    case 180:
        *x = (pw - px) * z;
        *y = (ph - py) * z;
        break;
    case 270:
        *x = py * z;
        *y = (pw - px) * z;
        break;
    default:
        *x = px * z;
        *y = py * z;
    }
}

void vl_item_to_point(int rotate, float pw, float ph, double z, double x, double y, double *px, double *py)
{
    switch (rotate) {
    case 90:
        *px = y / z;
        *py = ph - x / z;
        break;
    case 180:
        *px = pw - x / z;
        *py = ph - y / z;
        break;
    case 270:
        *px = pw - y / z;
        *py = x / z;
        break;
    default:
        *px = x / z;
        *py = y / z;
    }
}

/* How far v is outside [a, a + len). */
static long long outside(int v, int a, int len)
{
    return v < a ? (long long)a - v : v >= a + len ? (long long)v - (a + len - 1) : 0;
}

int vl_item_at(int n, const struct vl_item *items, int x, int y, int nearest)
{
    int best = -1;
    long long best_d = 0;
    for (int i = 0; i < n; i++) {
        if (items[i].hidden) continue;
        long long dx = outside(x, items[i].x, items[i].w), dy = outside(y, items[i].y, items[i].h);
        if (dx == 0 && dy == 0) return i;
        long long d = dy * 1000000 + dx; /* the row first */
        if (nearest && (best < 0 || d < best_d)) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

int vl_screen_to_page(const struct vl_params *p, int n, const struct vl_item *items, const float *pw,
                      const float *ph, int ax, int ay, int sx, int sy, int X, int Y, int nearest,
                      double *px, double *py)
{
    int cx = X - ax + sx, cy = Y - ay + sy;
    int i = vl_item_at(n, items, cx, cy, nearest);
    if (i < 0) return -1;
    double lx = cx - items[i].x, ly = cy - items[i].y;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx > items[i].w) lx = items[i].w;
    if (ly > items[i].h) ly = items[i].h;
    vl_item_to_point(p->rotate, pw[i], ph[i], items[i].zoom, lx + items[i].ox, ly + items[i].oy, px, py);
    return i;
}

void vl_page_to_screen(const struct vl_params *p, const struct vl_item *it, float pw, float ph, int ax, int ay,
                       int sx, int sy, double px, double py, double *X, double *Y)
{
    double x, y;
    vl_point_to_item(p->rotate, pw, ph, it->zoom, px, py, &x, &y);
    *X = ax - sx + it->x + x - it->ox;
    *Y = ay - sy + it->y + y - it->oy;
}

int vl_visible(int n, const struct vl_item *items, int sx, int sy, int vw, int vh, int *out, int max)
{
    long long dist[256];
    int count = 0;
    if (max > 256) max = 256;
    for (int i = 0; i < n; i++) {
        const struct vl_item *it = &items[i];
        if (it->hidden) continue;
        if (it->x >= sx + vw || it->x + it->w <= sx || it->y >= sy + vh || it->y + it->h <= sy) continue;
        long long dx = (long long)(it->x + it->w / 2) - (sx + vw / 2);
        long long dy = (long long)(it->y + it->h / 2) - (sy + vh / 2);
        long long d = dx * dx + dy * dy;
        int k = count < max ? count++ : max;
        if (k == max) {
            if (d >= dist[max - 1]) continue;
            k = max - 1;
        }
        while (k > 0 && dist[k - 1] > d) {
            dist[k] = dist[k - 1];
            out[k] = out[k - 1];
            k--;
        }
        dist[k] = d;
        out[k] = i;
    }
    return count;
}

int vl_priority(int n, const struct vl_item *items, int cols, int sx, int sy, int vw, int vh, int *out,
                int max, int *nvisible)
{
    int count = vl_visible(n, items, sx, sy, vw, vh, out, max);
    *nvisible = count;
    if (count == 0) return 0;
    int lo = out[0], hi = out[0];
    for (int i = 1; i < count; i++) {
        if (out[i] < lo) lo = out[i];
        if (out[i] > hi) hi = out[i];
    }
    if (cols < 1) cols = 1;
    int before = lo / cols * cols - cols, after = (hi / cols + 1) * cols;
    for (int pass = 0; pass < 2; pass++) {
        int from = pass == 0 ? before : after;
        for (int i = from; i < from + cols && count < max; i++) {
            if (i < 0 || i >= n) continue;
            int seen = 0;
            for (int k = 0; k < count; k++) seen |= out[k] == i;
            if (!seen) out[count++] = i;
        }
    }
    return count;
}

static int by_value(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

double vl_zoom_step(double cur, double fit_width, double fit_page, int in, int *mode)
{
    double v[18];
    for (int i = 0; i < 16; i++) v[i] = vl_zoom_values[i];
    v[16] = fit_width;
    v[17] = fit_page;
    qsort(v, 18, sizeof v[0], by_value);
    double next;
    if (!in) {
        if (cur <= v[0]) {
            *mode = -1;
            return cur;
        }
        int i = 0;
        while (i < 18 && v[i] < cur) i++; /* lower_bound */
        next = v[i - 1];
    } else {
        if (cur >= v[17]) {
            *mode = -1;
            return cur;
        }
        int i = 0;
        while (i < 18 && v[i] <= cur) i++; /* upper_bound */
        next = v[i];
    }
    if (next == fit_width) {
        *mode = VL_FIT_WIDTH;
    } else if (next == fit_page) {
        *mode = VL_FIT_PAGE;
    } else {
        *mode = VL_FIXED;
        next = next < VL_ZOOM_MIN ? VL_ZOOM_MIN : next > VL_ZOOM_MAX ? VL_ZOOM_MAX : next;
    }
    return next;
}

int vl_evict(const struct vl_lru *e, int n, uint64_t limit, uint64_t need, uint64_t keep, int *out)
{
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        if (e[i].used) total += e[i].bytes;
    }
    int count = 0;
    while (total + need > limit) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (!e[i].used || e[i].used >= keep) continue;
            int taken = 0;
            for (int k = 0; k < count; k++) taken |= out[k] == i;
            if (!taken && (best < 0 || e[i].used < e[best].used)) best = i;
        }
        if (best < 0) return -1;
        out[count++] = best;
        total -= e[best].bytes;
    }
    return count;
}
