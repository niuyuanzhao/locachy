/* The production bar hot-corner state machine, no compositor or devices. */
#define main bar_main
#include "../progs/bar.c"
#undef main
#include <assert.h>

static void push(struct bar *b, long long start)
{
    assert(!corner_push(b, .25, .25, -1, -1, start));
    assert(!corner_push(b, .25, .25, -1, 0, start + 74));
    assert(corner_push(b, .25, .25, 0, -1, start + 75));
}

int main(void)
{
    struct bar left = { 0 }, right = { 0 };
    push(&left, 1000);
    assert(!right.corner_attempt && !right.corner_triggered);
    push(&right, 1010);
    puts("PASS each output has its own 75 ms hot-corner attempt and cooldown");

    for (long long t = 1100; t < 2500; t += 20) {
        assert(!corner_push(&left, 0, 0, -2, -2, t));
    }
    assert(!corner_push(&left, 0, 0, -1, -1, 3000));
    assert(corner_push(&left, 0, 0, -1, -1, 3075));
    puts("PASS continued pushing triggers once; quiet cooldown permits another attempt");

    struct bar b = { 0 };
    assert(!corner_push(&b, 0, 0, -1, -1, 1000));
    assert(!corner_push(&b, 32, 0, 1, 0, 1050));
    assert(!b.corner_attempt);
    assert(!corner_push(&b, 0, 0, -1, -1, 1075));
    assert(corner_push(&b, 0, 0, -1, -1, 1150));
    puts("PASS leaving the 30 logical pixel area resets an incomplete attempt");

    memset(&b, 0, sizeof b);
    assert(!corner_push(&b, 0, 0, -1, -1, 1000));
    assert(!corner_push(&b, 0, 0, -1, -1, 1400));
    assert(!corner_push(&b, 0, 0, -1, -1, 1474));
    assert(corner_push(&b, 0, 0, -1, -1, 1475));
    puts("PASS an abandoned attempt expires at 350 ms");

    memset(&b, 0, sizeof b);
    assert(!corner_push(&b, 0, 0, -1, -1, 1000));
    assert(!corner_push(&b, 0, 0, 1, 0, 1040));
    assert(!b.corner_attempt);
    assert(!corner_push(&b, -1, 0, -1, 0, 1075));
    assert(!corner_push(&b, 1, 0, -1, 0, 1150));
    assert(!corner_push(&b, 0, 1, 0, -1, 1225));
    puts("PASS retreat and coordinates outside the one logical pixel do not trigger");

    struct wl_pointer *p = NULL;
    pointer_bar = &left;
    left.corner_attempt = 4000;
    pointer_leave(NULL, p, 0, NULL);
    assert(!pointer_bar && !left.corner_attempt);
    left.corner_attempt = left.corner_triggered = 5000;
    destroy_bar_surface(&left);
    assert(!left.corner_attempt && !left.corner_triggered);
    puts("PASS leaving or destroying an output cannot carry an attempt to another output");
    return 0;
}
