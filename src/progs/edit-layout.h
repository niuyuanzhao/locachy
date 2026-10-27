/* Kate's recursive view splitter layout, independent of the screen API.
 * Coordinates are character cells; splitting/resizing never changes documents. */
#ifndef LOCACHY_EDIT_LAYOUT_H
#define LOCACHY_EDIT_LAYOUT_H

struct edit_rect { int x, y, w, h; };
struct edit_split {
    int view;                    /* leaf >= 0; -1: splitter */
    int vertical;                /* left/right, otherwise top/bottom */
    int ratio;                   /* first child, ten-thousandths */
    struct edit_split *first, *second;
};
struct edit_layout { struct edit_split *root; };

int edit_layout_init(struct edit_layout *layout);
void edit_layout_free(struct edit_layout *layout);
/* Insert a view beside the existing one. Existing indices >= inserted shift
 * up, matching the view-space array. 0, or -1 with the tree left unchanged. */
int edit_layout_split(struct edit_layout *layout, int view, int inserted, int vertical);
/* Drop a view; greater indices shift down. The last view cannot be removed. */
int edit_layout_remove(struct edit_layout *layout, int view);
int edit_layout_rect(const struct edit_layout *layout, int view, struct edit_rect area, struct edit_rect *out);
/* The separator under a point (one cell wide), or NULL. */
struct edit_split *edit_layout_separator(struct edit_layout *layout, struct edit_rect area, int x, int y,
                                         struct edit_rect *split_area);
/* Resize from a point in the full split area; keep both sides usable. */
void edit_layout_resize(struct edit_split *split, struct edit_rect area, int x, int y, int minimum);
int edit_layout_toggle(struct edit_layout *layout, int view);
/* Closest view in a cardinal direction; -1 when no view lies there. */
int edit_layout_neighbor(const struct edit_layout *layout, int view, struct edit_rect area, int dx, int dy);
/* Strict session encoding: leaves are indices, splits V(ratio,a,b)/H(...).
 * Decoder validates exactly views distinct leaves and leaves old state intact
 * on malformed input. Caller owns the encoded string. */
char *edit_layout_encode(const struct edit_layout *layout);
int edit_layout_decode(struct edit_layout *layout, const char *text, int views);

#endif
