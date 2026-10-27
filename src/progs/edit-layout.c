#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit-layout.h"

static struct edit_split *leaf(int view)
{
    struct edit_split *s = calloc(1, sizeof *s);
    if (s) { s->view = view; s->ratio = 5000; }
    return s;
}

int edit_layout_init(struct edit_layout *layout)
{
    layout->root = leaf(0);
    return layout->root ? 0 : -1;
}

static void free_split(struct edit_split *s)
{
    if (!s) return;
    free_split(s->first);
    free_split(s->second);
    free(s);
}

void edit_layout_free(struct edit_layout *layout)
{
    free_split(layout->root);
    layout->root = NULL;
}

static struct edit_split *find(struct edit_split *s, int view)
{
    if (!s || s->view == view) return s;
    struct edit_split *f = find(s->first, view);
    return f ? f : find(s->second, view);
}

static void reindex(struct edit_split *s, int at, int delta)
{
    if (!s) return;
    if (s->view >= at) s->view += delta;
    reindex(s->first, at, delta);
    reindex(s->second, at, delta);
}

int edit_layout_split(struct edit_layout *layout, int view, int inserted, int vertical)
{
    struct edit_split *s = find(layout->root, view);
    if (!s || inserted < 0) return -1;
    struct edit_split *a = leaf(view), *b = leaf(inserted);
    if (!a || !b) { free(a); free(b); return -1; }
    reindex(layout->root, inserted, 1);
    a->view = s->view;
    *s = (struct edit_split){ .view = -1, .vertical = vertical != 0, .ratio = 5000,
                              .first = a, .second = b };
    return 0;
}

static int remove_view(struct edit_split **slot, int view)
{
    struct edit_split *s = *slot;
    if (!s || s->view >= 0) return 0;
    if (s->first->view == view || s->second->view == view) {
        struct edit_split *gone = s->first->view == view ? s->first : s->second;
        struct edit_split *keep = gone == s->first ? s->second : s->first;
        free(gone);
        free(s);
        *slot = keep;
        return 1;
    }
    return remove_view(&s->first, view) || remove_view(&s->second, view);
}

int edit_layout_remove(struct edit_layout *layout, int view)
{
    if (!remove_view(&layout->root, view)) return -1;
    reindex(layout->root, view + 1, -1);
    return 0;
}

static void halves(const struct edit_split *s, struct edit_rect area,
                   struct edit_rect *a, struct edit_rect *b)
{
    int full = s->vertical ? area.w : area.h;
    int usable = full > 0 ? full - 1 : 0;
    int first = (int)((long long)usable * s->ratio / 10000);
    if (usable >= 2) {
        if (first < 1) first = 1;
        if (first > usable - 1) first = usable - 1;
    }
    *a = *b = area;
    if (s->vertical) {
        a->w = first;
        b->x += first + (full > 0);
        b->w = usable - first;
    } else {
        a->h = first;
        b->y += first + (full > 0);
        b->h = usable - first;
    }
}

static int rect_of(const struct edit_split *s, int view, struct edit_rect area, struct edit_rect *out)
{
    if (!s) return 0;
    if (s->view == view) { *out = area; return 1; }
    if (s->view >= 0) return 0;
    struct edit_rect a, b;
    halves(s, area, &a, &b);
    return rect_of(s->first, view, a, out) || rect_of(s->second, view, b, out);
}

int edit_layout_rect(const struct edit_layout *layout, int view, struct edit_rect area, struct edit_rect *out)
{
    return rect_of(layout->root, view, area, out);
}

static int contains(struct edit_rect a, int x, int y)
{
    return x >= a.x && y >= a.y && x < a.x + a.w && y < a.y + a.h;
}

static struct edit_split *separator(struct edit_split *s, struct edit_rect area, int x, int y,
                                   struct edit_rect *split_area)
{
    if (!s || s->view >= 0 || !contains(area, x, y)) return NULL;
    struct edit_rect a, b;
    halves(s, area, &a, &b);
    if ((s->vertical && x == a.x + a.w) || (!s->vertical && y == a.y + a.h)) {
        if (split_area) *split_area = area;
        return s;
    }
    struct edit_split *r = separator(s->first, a, x, y, split_area);
    return r ? r : separator(s->second, b, x, y, split_area);
}

struct edit_split *edit_layout_separator(struct edit_layout *layout, struct edit_rect area, int x, int y,
                                         struct edit_rect *split_area)
{
    return separator(layout->root, area, x, y, split_area);
}

void edit_layout_resize(struct edit_split *s, struct edit_rect area, int x, int y, int minimum)
{
    if (!s || s->view >= 0) return;
    int available = (s->vertical ? area.w : area.h) - 1;
    if (available < 2) return;
    int size = s->vertical ? x - area.x : y - area.y;
    int min = minimum > 0 ? minimum : 1;
    if (min > available / 2) min = available / 2;
    if (size < min) size = min;
    if (size > available - min) size = available - min;
    s->ratio = (int)((long long)size * 10000 / available);
}

static struct edit_split *parent_of(struct edit_split *s, int view)
{
    if (!s || s->view >= 0) return NULL;
    if (s->first->view == view || s->second->view == view) return s;
    struct edit_split *p = parent_of(s->first, view);
    return p ? p : parent_of(s->second, view);
}

int edit_layout_toggle(struct edit_layout *layout, int view)
{
    struct edit_split *s = parent_of(layout->root, view);
    if (!s) return 0;
    s->vertical = !s->vertical;
    return 1;
}

static void neighbor(const struct edit_split *s, const struct edit_layout *layout, int view,
                     struct edit_rect area, struct edit_rect from, int dx, int dy,
                     long long *distance, int *best)
{
    if (!s) return;
    if (s->view >= 0) {
        if (s->view == view) return;
        struct edit_rect r;
        if (!edit_layout_rect(layout, s->view, area, &r)) return;
        int x = 2 * r.x + r.w, y = 2 * r.y + r.h;
        int fx = 2 * from.x + from.w, fy = 2 * from.y + from.h;
        if ((dx < 0 && r.x + r.w > from.x) || (dx > 0 && r.x < from.x + from.w) ||
            (dy < 0 && r.y + r.h > from.y) || (dy > 0 && r.y < from.y + from.h)) return;
        long long along = dx ? llabs((long long)x - fx) : llabs((long long)y - fy);
        long long cross = dx ? llabs((long long)y - fy) : llabs((long long)x - fx);
        int overlap = dx ? r.y < from.y + from.h && r.y + r.h > from.y :
                           r.x < from.x + from.w && r.x + r.w > from.x;
        long long score = (overlap ? 0 : 1000000000LL) + along * 10000 + cross;
        if (score < *distance) { *distance = score; *best = s->view; }
        return;
    }
    neighbor(s->first, layout, view, area, from, dx, dy, distance, best);
    neighbor(s->second, layout, view, area, from, dx, dy, distance, best);
}

int edit_layout_neighbor(const struct edit_layout *layout, int view, struct edit_rect area, int dx, int dy)
{
    if ((!dx && !dy) || (dx && dy)) return -1;
    struct edit_rect from;
    if (!edit_layout_rect(layout, view, area, &from)) return -1;
    int best = -1;
    long long distance = LLONG_MAX;
    neighbor(layout->root, layout, view, area, from, dx, dy, &distance, &best);
    return best;
}

static int encode_split(const struct edit_split *s, char *buf, size_t size, size_t *at)
{
    if (!s || *at >= size) return -1;
    int n = s->view >= 0 ? snprintf(buf + *at, size - *at, "%d", s->view) :
            snprintf(buf + *at, size - *at, "%c(%d,", s->vertical ? 'V' : 'H', s->ratio);
    if (n < 0 || (size_t)n >= size - *at) return -1;
    *at += (size_t)n;
    if (s->view >= 0) return 0;
    if (encode_split(s->first, buf, size, at) < 0 || *at + 1 >= size) return -1;
    buf[(*at)++] = ',';
    if (encode_split(s->second, buf, size, at) < 0 || *at + 1 >= size) return -1;
    buf[(*at)++] = ')';
    buf[*at] = '\0';
    return 0;
}

char *edit_layout_encode(const struct edit_layout *layout)
{
    size_t size = 65536, at = 0;
    char *text = malloc(size);
    if (!text) return NULL;
    if (encode_split(layout->root, text, size, &at) < 0) { free(text); return NULL; }
    return text;
}

static int number(const char **text, int *out)
{
    const char *p = *text;
    if (*p < '0' || *p > '9') return -1;
    unsigned value = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (value > 100000) return -1;
        value = value * 10 + (unsigned)(*p - '0');
    }
    if (value > 1000000) return -1;
    *out = (int)value;
    *text = p;
    return 0;
}

static struct edit_split *decode_split(const char **text, int views, unsigned char *seen, int depth)
{
    if (depth > 64) return NULL;
    const char *p = *text;
    if (*p >= '0' && *p <= '9') {
        int view;
        if (number(&p, &view) < 0 || view >= views || seen[view]) return NULL;
        struct edit_split *s = leaf(view);
        if (!s) return NULL;
        seen[view] = 1;
        *text = p;
        return s;
    }
    int vertical = *p == 'V';
    if ((*p != 'V' && *p != 'H') || p[1] != '(') return NULL;
    p += 2;
    int ratio;
    if (number(&p, &ratio) < 0 || ratio < 1 || ratio >= 10000 || *p++ != ',') return NULL;
    struct edit_split *s = leaf(-1);
    if (!s) return NULL;
    s->vertical = vertical;
    s->ratio = ratio;
    s->first = decode_split(&p, views, seen, depth + 1);
    if (!s->first || *p++ != ',') { free_split(s); return NULL; }
    s->second = decode_split(&p, views, seen, depth + 1);
    if (!s->second || *p++ != ')') { free_split(s); return NULL; }
    *text = p;
    return s;
}

int edit_layout_decode(struct edit_layout *layout, const char *text, int views)
{
    if (!text || views < 1 || views > 1024 || strlen(text) > 65535) return -1;
    unsigned char *seen = calloc((size_t)views, 1);
    if (!seen) return -1;
    const char *p = text;
    struct edit_split *root = decode_split(&p, views, seen, 0);
    int valid = root && !*p;
    for (int i = 0; i < views; i++) valid &= seen[i] != 0;
    free(seen);
    if (!valid) { free_split(root); return -1; }
    free_split(layout->root);
    layout->root = root;
    return 0;
}
