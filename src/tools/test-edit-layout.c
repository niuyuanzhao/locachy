/* Recursive editor split layout: no screen, files or input devices. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base.h"
#include "i18n-stub.h"
#include "../progs/edit-layout.h"

static void rect_is(struct edit_layout *l, int view, int x, int y, int w, int h)
{
    struct edit_rect r;
    assert(edit_layout_rect(l, view, (struct edit_rect){0, 0, 101, 41}, &r));
    assert(r.x == x && r.y == y && r.w == w && r.h == h);
}

int main(void)
{
    struct edit_layout l;
    assert(edit_layout_init(&l) == 0);
    rect_is(&l, 0, 0, 0, 101, 41);
    assert(edit_layout_split(&l, 0, 1, 1) == 0);
    rect_is(&l, 0, 0, 0, 50, 41);
    rect_is(&l, 1, 51, 0, 50, 41);
    assert(edit_layout_split(&l, 1, 2, 0) == 0);
    rect_is(&l, 1, 51, 0, 50, 20);
    rect_is(&l, 2, 51, 21, 50, 20);
    char *encoded = edit_layout_encode(&l);
    assert(encoded && strcmp(encoded, "V(5000,0,H(5000,1,2))") == 0);
    struct edit_layout restored = { 0 };
    assert(edit_layout_decode(&restored, encoded, 3) == 0);
    rect_is(&restored, 2, 51, 21, 50, 20);
    assert(edit_layout_decode(&restored, "V(5000,0,0)", 2) == -1);
    assert(edit_layout_decode(&restored, "V(5000,0,1)", 3) == -1);
    assert(edit_layout_decode(&restored, "H(999999999999999999999,0,1)", 2) == -1);
    assert(edit_layout_decode(&restored, "H(0,0,1)", 2) == -1);
    rect_is(&restored, 2, 51, 21, 50, 20);
    edit_layout_free(&restored);
    free(encoded);
    struct edit_rect area, full = {0, 0, 101, 41};
    struct edit_split *s = edit_layout_separator(&l, full, 70, 20, &area);
    assert(s && !s->vertical && area.x == 51 && area.w == 50);
    edit_layout_resize(s, area, 70, 30, 4);
    rect_is(&l, 1, 51, 0, 50, 30);
    rect_is(&l, 2, 51, 31, 50, 10);
    assert(edit_layout_neighbor(&l, 1, full, 0, 1) == 2);
    assert(edit_layout_neighbor(&l, 2, full, -1, 0) == 0);
    assert(edit_layout_neighbor(&l, 0, full, -1, 0) == -1);
    assert(edit_layout_toggle(&l, 1) == 1);
    assert(s->vertical);
    assert(edit_layout_remove(&l, 1) == 0);
    rect_is(&l, 1, 51, 0, 50, 41);
    assert(edit_layout_remove(&l, 0) == 0);
    rect_is(&l, 0, 0, 0, 101, 41);
    assert(edit_layout_remove(&l, 0) == -1);
    assert(edit_layout_split(&l, 12, 1, 1) == -1);
    for (int i = 0; i < 20; i++) {
        assert(edit_layout_split(&l, 0, 1, i & 1) == 0);
        assert(edit_layout_remove(&l, 0) == 0);
    }
    edit_layout_free(&l);
    assert(!l.root);
    puts("PASS editor recursive horizontal/vertical splits, resize, neighbors and close/reindex");
}
