/* Logical output geometry shared by settings and the compositor controller. */
#ifndef LOCACHY_MONITOR_LAYOUT_H
#define LOCACHY_MONITOR_LAYOUT_H

#include <stddef.h>
#include <string.h>

enum monitor_place {
    MONITOR_RIGHT, MONITOR_LEFT, MONITOR_ABOVE, MONITOR_BELOW, MONITOR_ORIGIN
};

struct monitor_rect {
    const char *name;
    int width, height;
    int place;
    int x, y;
    int has_position;
};

static inline int monitor_before(const struct monitor_rect *r, size_t a, size_t b)
{
    int cmp = strcmp(r[a].name, r[b].name);
    return cmp < 0 || (cmp == 0 && a < b);
}

/* Connector names determine same-side order, not discovery or Wayland order.
 * A missing origin falls back to the first name; extra origins go right. */
static inline size_t monitor_layout(struct monitor_rect *r, size_t n)
{
    if (!n) return n;
    size_t anchor = 0;
    for (size_t i = 1; i < n; i++) {
        if ((r[i].place == MONITOR_ORIGIN && r[anchor].place != MONITOR_ORIGIN) ||
            ((r[i].place == MONITOR_ORIGIN) == (r[anchor].place == MONITOR_ORIGIN) &&
             monitor_before(r, i, anchor))) anchor = i;
    }
    int min_x = 0, min_y = 0, explicit = 0;
    for (size_t i = 0; i < n; i++) explicit |= r[i].has_position;
    for (size_t i = 0; i < n; i++) {
        if (r[i].has_position) {
            if (r[i].x < min_x) min_x = r[i].x;
            if (r[i].y < min_y) min_y = r[i].y;
            continue;
        }
        r[i].x = r[i].y = 0;
        if (i == anchor) continue;
        int side = r[i].place;
        if (side < MONITOR_RIGHT || side > MONITOR_BELOW) side = MONITOR_RIGHT;
        if (side == MONITOR_LEFT) r[i].x = -r[i].width;
        else if (side == MONITOR_ABOVE) r[i].y = -r[i].height;
        else if (side == MONITOR_BELOW) r[i].y = r[anchor].height;
        else r[i].x = r[anchor].width;
        if (r[anchor].has_position) { r[i].x += r[anchor].x; r[i].y += r[anchor].y; }
        for (size_t j = 0; j < n; j++) {
            if (j == anchor || r[j].has_position || !monitor_before(r, j, i)) continue;
            int other = r[j].place;
            if (other < MONITOR_RIGHT || other > MONITOR_BELOW) other = MONITOR_RIGHT;
            if (other != side) continue;
            if (side == MONITOR_LEFT) r[i].x -= r[j].width;
            else if (side == MONITOR_ABOVE) r[i].y -= r[j].height;
            else if (side == MONITOR_BELOW) r[i].y += r[j].height;
            else r[i].x += r[j].width;
        }
        if (r[i].x < min_x) min_x = r[i].x;
        if (r[i].y < min_y) min_y = r[i].y;
    }
    if (!explicit) for (size_t i = 0; i < n; i++) {
        r[i].x -= min_x;
        r[i].y -= min_y;
    }
    return anchor;
}

#endif
