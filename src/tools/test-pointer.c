/* The pointer layer without libinput, real input devices or process 1: the
 * device lending/revocation bookkeeping on pipes, and the events libinput's
 * motions, buttons and scrolling become (merging, clamping, click counts,
 * scroll steps, cells, the cursor), touch screens' taps, holds, drags and
 * two-finger pinches, touchpad pinches and swipes. Built without
 * CONFIG_LIBINPUT (link -lm). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../base/pointer.c"
#include <assert.h>
#include <math.h>
#include <sys/socket.h>

static int channel[2], opened, cursor_x = -1, cursor_y = -1, cursor_shown = -1;
static char sent[8][32];
static int nsent;
int control_fd(void) { return channel[0]; }
int control_send(int sock, const char *msg, size_t len, const int *fds, int n)
{ (void)fds; assert(n == 0); return send(sock, msg, len, 0) == (ssize_t)len ? 0 : -1; }
/* What the swipes asked process 1 for. */
int control_send_fields(int sock, const char *const *fields)
{
    assert(sock == channel[0] && nsent < 8);
    snprintf(sent[nsent++], sizeof sent[0], "%s%s%s", fields[0], fields[1] ? " " : "", fields[1] ? fields[1] : "");
    return 0;
}
static int extended_fixture, selected_fixture;
/* Two outputs side by side, as screen.c lays them out: desktop rectangles,
 * native pixels. At first the second has twice the pixels. */
static struct screen_output outs[2] = {
    { .x = 0, .width = 1000, .height = 800, .pixel_width = 1000, .pixel_height = 800, .scale = 1 },
    { .x = 1000, .width = 800, .height = 800, .pixel_width = 1600, .pixel_height = 1600, .scale = 2 },
};
int screen_is_extended(void) { return extended_fixture; }
int screen_output_select(int index) { int old=selected_fixture;selected_fixture=index;return old; }
int screen_output_get(int index, struct screen_output *out)
{ if (index < 0 || index > 1) return -1; *out = outs[index]; return 0; }
int screen_output_at(int x, int y, int *lx, int *ly)
{
    for (int i = 0; i < 2; i++) {
        const struct screen_output *o = &outs[i];
        if (x < o->x || x >= o->x + o->width || y < o->y || y >= o->y + o->height) continue;
        if (lx) *lx = (x - o->x) * o->pixel_width / o->width;
        if (ly) *ly = (y - o->y) * o->pixel_height / o->height;
        return i;
    }
    return -1;
}
/* The nearest point of either rectangle. */
void screen_output_project(int *x, int *y)
{
    long best = -1;
    int nx = *x, ny = *y;
    for (int i = 0; i < 2; i++) {
        const struct screen_output *o = &outs[i];
        int cx = *x < o->x ? o->x : *x > o->x + o->width - 1 ? o->x + o->width - 1 : *x;
        int cy = *y < o->y ? o->y : *y > o->y + o->height - 1 ? o->y + o->height - 1 : *y;
        long d = (long)(cx - *x) * (cx - *x) + (long)(cy - *y) * (cy - *y);
        if (best < 0 || d < best) { best = d; nx = cx; ny = cy; }
    }
    *x = nx; *y = ny;
}
/* Extended, the canvas drawn on: the selected output's native pixels. */
void screen_pixels(int *w, int *h)
{
    if (extended_fixture) { *w = outs[selected_fixture].pixel_width; *h = outs[selected_fixture].pixel_height; }
    else { *w = 1000; *h = 800; }
}
void screen_cell_at(int x, int y, int *col, int *row) { *col = x / 10; *row = y / 20 - 1; }
void screen_cursor_move(int x, int y) { cursor_x = x; cursor_y = y; }
void screen_cursor_visible(int on) { cursor_shown = on; }
double screen_scale(void) { return 1.0; }
/* The desktop of several screens (desk_fixture of them when 2): this
 * program's picture at its origin, another program's screen right of it. */
static int desk_fixture, hovered = -2;
int screen_desk_count(void) { return desk_fixture; }
int screen_to_desk(int x, int y, int *gx, int *gy) { *gx = x; *gy = y; return 0; }
int screen_from_desk(int gx, int gy, int *x, int *y) { *x = gx; *y = gy; return 0; }
int screen_desk_foreign(int gx, int gy) { return gx >= 1000 && gx < 2000 && gy >= 0 && gy < 800; }
void screen_hover(int output) { hovered = output; }
int rc_read(void (*fn)(const char *, const char *, void *), void *arg) { (void)fn; (void)arg; return -1; }

int ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    if (request == EVIOCREVOKE) { opened--; return 0; }
    errno = ENOTTY;
    return -1;
}

static struct pointer_event next(void)
{
    assert(pointer_next());
    return *pointer_last();
}

int main(void)
{
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel) == 0);
    assert(pointer_open() == 0 && px == 500 && py == 400 && cursor_x == 500 && cursor_y == 400);
    char request[32] = "";
    assert(recv(channel[1], request, sizeof request, 0) == 8 && strcmp(request, "pointer?") == 0);
    enum { DEVICES = 17 };
    int pipes[DEVICES][2];
    int *originals = calloc(DEVICES, sizeof *originals);
    assert(originals);
    for (int i = 0; i < DEVICES; i++) {
        char path[POINTER_PATH];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        assert(pipe2(pipes[i], O_NONBLOCK | O_CLOEXEC) == 0);
        originals[i] = dup(pipes[i][0]);
        assert(originals[i] >= 0);
        pointer_take(pipes[i][0], path); /* kept, though no libinput takes it here */
        opened++;
    }
    assert(ndevices == DEVICES && strcmp(devices[16].path, "/dev/input/event16") == 0);
    assert(pointer_fd() == -1 && !pointer_present());
    puts("PASS pointer request and dynamic device list with paths");

    px += 20; py -= 10; moved();
    px += 5; moved(); /* unread motions merge */
    assert(cursor_x == 525 && cursor_y == 390);
    struct pointer_event e = next();
    assert(e.x == 525 && e.y == 390 && !e.button && e.col == 52 && e.row == 18 && !pointer_next());
    px = 5000; py = -7; moved();
    e = next();
    assert(e.x == 999 && e.y == 0 && cursor_x == 999 && e.row == -1); /* kept on the picture, the bar */
    puts("PASS motions merge while unread, move the cursor at once, stay on the picture, give cells");

    px = 100; py = 100; moved();
    button(BTN_LEFT, 1);
    button(BTN_LEFT, 0);
    button(BTN_LEFT, 1);
    e = next();
    assert(!e.button);
    e = next();
    assert(e.button == BTN_LEFT && e.pressed && e.clicks == 1 && e.buttons == 1);
    e = next();
    assert(e.button == BTN_LEFT && !e.pressed && e.buttons == 0);
    e = next();
    assert(e.clicks == 2);
    button(BTN_LEFT, 0); button(BTN_LEFT, 1);
    next(); e = next();
    assert(e.clicks == 3);
    button(BTN_LEFT, 0); button(BTN_LEFT, 1);
    next(); e = next();
    assert(e.clicks == 1); /* after a triple click, counting starts again */
    button(BTN_LEFT, 0);
    next();
    px = 300; moved(); next();
    button(BTN_LEFT, 1);
    e = next();
    assert(e.clicks == 1); /* far away: not a double click */
    button(BTN_LEFT, 0); next();
    button(BTN_RIGHT, 1);
    e = next();
    assert(e.button == BTN_RIGHT && e.clicks == 1 && e.buttons == 2);
    button(BTN_RIGHT, 0); next();
    button(BTN_SIDE, 1);
    assert(!pointer_next()); /* buttons other than these three are not reported */
    puts("PASS presses and releases, held buttons, double and triple clicks by time and place");

    scroll(0.5, 0);
    assert(!pointer_next());
    scroll(0.6, 0);
    e = next();
    assert(e.wheel == -1 && e.hwheel == 0); /* down, as libinput gives it positive */
    scroll(-3, 2);
    e = next();
    assert(e.wheel == 2 && e.hwheel == 2); /* the 0.1 left over counts */
    puts("PASS scrolling becomes whole wheel steps, up positive, fractions carried");

    for (int i = 0; i < QUEUE + 5; i++) button(BTN_LEFT, i % 2 == 0);
    int count = 0;
    while (pointer_next()) count++;
    assert(count == QUEUE);
    puts("PASS a full queue drops its oldest events");
    button(BTN_LEFT, 0);
    pointer_next();

    /* A touch screen, 10 pixels of the picture to a millimetre. */
    touch_ppmm = 10;
    nmice = 1;
    usleep(450000); /* no double click with the presses above */
    touch_down(0, 200, 300, 20, 30);
    assert(cursor_shown == 0 && !pointer_next()); /* touching hides the cursor; nothing yet */
    touch_up(0);
    e = next();
    assert(e.button == BTN_LEFT && e.pressed && e.clicks == 1 && e.x == 200 && e.y == 300);
    e = next();
    assert(e.button == BTN_LEFT && !e.pressed && !pointer_next());
    touch_down(0, 240, 300, 24, 30); /* 4 mm away: a double tap */
    touch_up(0);
    e = next();
    assert(e.clicks == 2);
    next();
    px = 10;
    moved();
    assert(cursor_shown == 1); /* a mouse moving shows it again */
    next();
    puts("PASS touch: a tap clicks where it went down, a near second one double-clicks, the cursor hides");

    touch_down(1, 400, 400, 40, 40);
    touch_motion(1, 405, 402, 40.5, 40.2); /* within 3 mm: still */
    touch_timer();
    e = next();
    assert(e.button == BTN_RIGHT && e.pressed && e.x == 400 && e.y == 400);
    e = next();
    assert(e.button == BTN_RIGHT && !e.pressed);
    touch_up(1);
    assert(!pointer_next());
    puts("PASS touch: holding still is a right click, lifting after it nothing");

    touch_down(0, 500, 400, 50, 40);
    touch_motion(0, 500, 420, 50, 42);
    assert(!pointer_next()); /* 2 mm: not yet moving */
    touch_motion(0, 500, 460, 50, 46);
    e = next();
    assert(e.wheel == 3 && e.hwheel == 0 && e.x == 500 && e.y == 400 && !e.button); /* rows 19 to 22 */
    touch_motion(0, 480, 440, 48, 44);
    e = next();
    assert(e.wheel == -1 && e.hwheel == 2);
    touch_timer(); /* a hold timer after moving: nothing */
    touch_up(0);
    assert(!pointer_next());
    puts("PASS touch: dragging scrolls a wheel step a row and a column, the content following the finger");

    touch_down(0, 300, 300, 30, 30);
    touch_down(1, 400, 300, 40, 30);
    touch_motion(1, 410, 300, 41, 30);
    px = 50; moved(); /* a pinch is no motion to merge with */
    e = next();
    assert(e.gesture == GESTURE_PINCH && fabs(e.scale - 1.1) < 1e-9 && !e.button && !e.wheel);
    e = next();
    assert(!e.gesture && e.x == 50);
    touch_motion(0, 300, 340, 30, 30.1);
    touch_motion(1, 410, 340, 41, 30.1);
    int wheel = 0;
    while (pointer_next()) {
        assert(!pointer_last()->gesture && !pointer_last()->button);
        wheel += pointer_last()->wheel;
    }
    assert(wheel == 2);
    touch_up(0);
    touch_up(1);
    assert(!pointer_next());
    puts("PASS touch: two fingers pinch by their distance and scroll by their middle, no tap on lifting");

    pinch_last = 1;
    pinch(1.02);
    assert(!pointer_next());
    pinch(1.05);
    e = next();
    assert(e.gesture == GESTURE_PINCH && fabs(e.scale - 1.05) < 1e-9);
    pinch(0.9);
    e = next();
    assert(fabs(e.scale - 0.9 / 1.05) < 1e-9);
    swipe(3, 0, -150);
    swipe(4, -200, 30);
    swipe(3, 150, 0);
    swipe(2, 0, -300); /* two fingers: scrolling, not this */
    swipe(3, 30, -60);  /* 6 mm: too short */
    assert(nsent == 3 && !strcmp(sent[0], "home") && !strcmp(sent[1], "switch next") &&
           !strcmp(sent[2], "switch prev"));
    puts("PASS touchpad: pinches in steps; three or more fingers swipe home, to the next and previous task");

    scroll_input(SCROLL_FINGER, 2, 0, 1, &pipes[0]);
    e = next();
    assert(e.wheel == -2 && e.scroll_source == SCROLL_FINGER && e.scroll_phase == SCROLL_UPDATE &&
           e.scroll_fingers == 0 && e.gesture == GESTURE_NONE && !pointer_next());
    scroll_input(SCROLL_FINGER, 0, 0, 1, &pipes[0]);
    assert(!pointer_next());
    scroll_input(SCROLL_CONTINUOUS, -1, 2, 3, &pipes[0]);
    e = next();
    assert(e.wheel == 1 && e.hwheel == 2 && e.scroll_source == SCROLL_CONTINUOUS);
    scroll_input(SCROLL_CONTINUOUS, 0, 0, 3, &pipes[0]);
    assert(!pointer_next());
    puts("PASS scrolling sources tag existing updates without adding unrequested boundary events");

    assert(pointer_scroll_events(1) == 0);
    px = 400; py = 300;
    scroll_input(SCROLL_FINGER, -0.2, 0, 1, &pipes[0]);
    e = next();
    assert(e.gesture == GESTURE_SCROLL && e.scroll_phase == SCROLL_BEGIN && e.scroll_source == SCROLL_FINGER &&
           e.scroll_fingers == 0 && e.x == 400 && e.y == 300 && !e.wheel && !e.hwheel && !pointer_next());
    scroll_input(SCROLL_FINGER, -0.9, 0.1, 3, &pipes[0]);
    px = 410; moved();
    e = next();
    assert(e.wheel == 1 && e.scroll_phase == SCROLL_UPDATE && e.scroll_source == SCROLL_FINGER);
    e = next();
    assert(e.x == 410 && !e.gesture && !e.scroll_phase);
    scroll_input(SCROLL_FINGER, 0, 0, 1, &pipes[0]);
    assert(!pointer_next()); /* the horizontal axis is still active */
    scroll_input(SCROLL_FINGER, 0, 0, 2, &pipes[0]);
    e = next();
    assert(e.gesture == GESTURE_SCROLL && e.scroll_phase == SCROLL_END && e.x == 400 && e.y == 300 &&
           !e.wheel && !e.hwheel && !pointer_next());
    scroll_input(SCROLL_FINGER, 0, 0, 3, &pipes[0]);
    assert(!pointer_next()); /* a second stop does not end the gesture again */
    puts("PASS opted-in finger scrolling starts before a full step and ends once after both axes stop");

    scroll_input(SCROLL_CONTINUOUS, 1, 0, 1, &pipes[0]);
    e = next(); assert(e.scroll_phase == SCROLL_BEGIN && e.scroll_source == SCROLL_CONTINUOUS);
    e = next(); assert(e.wheel == -1 && e.scroll_source == SCROLL_CONTINUOUS);
    scroll_input(SCROLL_WHEEL, 1, 0, 1, &pipes[0]);
    e = next(); assert(e.scroll_phase == SCROLL_CANCEL && e.scroll_source == SCROLL_CONTINUOUS);
    e = next();
    assert(e.wheel == -1 && e.scroll_source == SCROLL_WHEEL && e.scroll_phase == SCROLL_UPDATE && !pointer_next());
    scroll_input(SCROLL_FINGER, 1, 0, 1, &pipes[0]);
    next(); next();
    scroll_input(SCROLL_FINGER, 1, 0, 1, &pipes[1]);
    e = next(); assert(e.scroll_phase == SCROLL_CANCEL);
    e = next(); assert(e.scroll_phase == SCROLL_BEGIN);
    e = next(); assert(e.scroll_phase == SCROLL_UPDATE);
    pointer_went();
    e = next(); assert(e.scroll_phase == SCROLL_CANCEL && !pointer_next());
    scroll_input(SCROLL_FINGER, 0, 0, 1, &pipes[1]);
    assert(!pointer_next());
    away = 0;
    puts("PASS wheel and TrackPoint sources stay distinct; source, device and ownership changes cancel scrolling");

    scroll_input(SCROLL_FINGER, -1, 0, 1, &pipes[0]);
    next(); next();
    pinch_last = 1;
    pinch(1.2);
    e = next(); assert(e.scroll_phase == SCROLL_CANCEL && e.scroll_source == SCROLL_FINGER);
    e = next(); assert(e.gesture == GESTURE_PINCH && !e.scroll_phase && !pointer_next());
    scroll_input(SCROLL_FINGER, 0, 0, 1, &pipes[0]);
    assert(!pointer_next());
    puts("PASS pinching cancels a pending finger scroll before its zoom event");

    touch_down(0, 200, 200, 20, 20);
    touch_motion(0, 200, 260, 20, 26);
    e = next();
    assert(e.gesture == GESTURE_SCROLL && e.scroll_source == SCROLL_TOUCH && e.scroll_phase == SCROLL_BEGIN &&
           e.scroll_fingers == 1 && e.x == 200 && e.y == 200);
    e = next();
    assert(e.wheel == 3 && e.scroll_source == SCROLL_TOUCH && e.scroll_phase == SCROLL_UPDATE && e.scroll_fingers == 1);
    touch_motion(0, 200, 220, 20, 22);
    e = next(); assert(e.wheel == -2 && e.scroll_fingers == 1);
    touch_up(0);
    e = next(); assert(e.scroll_phase == SCROLL_END && e.scroll_fingers == 1 && !pointer_next());
    touch_up(0);
    assert(!pointer_next());
    puts("PASS single-finger touch scrolling reports direction reversal and exactly one release at its origin");

    touch_down(0, 300, 300, 30, 30);
    touch_motion(0, 300, 360, 30, 36);
    next(); next();
    touch_down(1, 400, 360, 40, 36);
    e = next(); assert(e.scroll_phase == SCROLL_CANCEL && e.scroll_fingers == 1);
    touch_motion(0, 300, 400, 30, 40);
    e = next(); assert(e.gesture == GESTURE_PINCH && !e.scroll_phase);
    e = next(); assert(e.scroll_phase == SCROLL_BEGIN && e.scroll_fingers == 2);
    e = next(); assert(e.scroll_phase == SCROLL_UPDATE && e.scroll_fingers == 2);
    touch_up(1);
    e = next(); assert(e.scroll_phase == SCROLL_END && e.scroll_fingers == 2);
    touch_up(0);
    assert(!pointer_next());
    touch_down(0, 300, 300, 30, 30);
    touch_motion(0, 300, 360, 30, 36);
    next(); next();
    touch_reset();
    e = next(); assert(e.scroll_phase == SCROLL_CANCEL && e.scroll_source == SCROLL_TOUCH && !pointer_next());
    touch_up(0);
    assert(!pointer_next());
    puts("PASS adding a second touch cancels the single-finger gesture; cancellation is not a release");

    touch_down(0, 300, 300, 30, 30);
    touch_down(1, 400, 300, 40, 30);
    touch_motion(0, 300, 340, 30, 30.1);
    next(); next();
    touch_down(2, 500, 300, 50, 30);
    e = next(); assert(e.scroll_phase == SCROLL_CANCEL && e.scroll_fingers == 2);
    touch_motion(0, 300, 400, 30, 30.2);
    touch_up(0); touch_up(1); touch_up(2);
    assert(!pointer_next());
    touch_down(0, 200, 200, 20, 20);
    touch_up(0);
    e = next(); assert(e.button == BTN_LEFT && e.pressed && !e.scroll_phase);
    e = next(); assert(e.button == BTN_LEFT && !e.pressed && !e.scroll_phase && !pointer_next());
    touch_down(0, 200, 200, 20, 20);
    touch_timer();
    e = next(); assert(e.button == BTN_RIGHT && e.pressed && !e.scroll_phase);
    next(); touch_up(0);
    assert(!pointer_next());
    puts("PASS third-finger arrival cancels scrolling; opted-in taps and holds retain their button events");

    touch_down(0, 300, 300, 30, 30);
    touch_motion(0, 300, 360, 30, 36);
    assert(pointer_scroll_events(0) == 1); /* already queued boundaries disappear too */
    e = next(); assert(e.wheel == 3 && e.scroll_phase == SCROLL_UPDATE && !pointer_next());
    touch_up(0);
    assert(!pointer_next());
    assert(pointer_scroll_events(0) == 0);
    puts("PASS disabling lifecycle delivery preserves updates and removes queued boundaries");

    extended_fixture = 1;
    px = 1100; py = 200; moved();
    e = next();
    assert(e.output == 1 && e.x == 1100 && e.col == 20 && e.row == 19 && selected_fixture == 0);
    px = 2000; py = 900; moved();
    e = next();
    assert(e.output == 1 && e.x == 1799 && e.y == 799 && cursor_x == 1799 && cursor_y == 799);
    extended_fixture = 0;
    puts("PASS extended pointer crosses outputs, maps local scaled cells and clamps desktop projection");
    /* Motions of less than a pixel add up when extended too. */
    extended_fixture = 1;
    px = 1100; py = 200; moved(); next();
    for (int i = 1; i <= 8; i++) {
        px += 0.25; py -= 0.25; moved();
        assert(cursor_x == 1100 + i / 4 && cursor_y == 200 - (i + 3) / 4);
    }
    e = next();
    assert(e.output == 1 && e.x == 1102 && e.y == 198 && !pointer_next());
    puts("PASS extended pointer keeps sub-pixel motions inside the outputs");
    /* Absolute devices point at the internal panel, the first output, in its
     * desktop coordinates, whatever output is drawn on: 1920x1080 at scale 2
     * (960 wide), the other output from 960 on. */
    outs[0] = (struct screen_output){ .width = 960, .height = 540, .pixel_width = 1920,
                                      .pixel_height = 1080, .scale = 2 };
    outs[1] = (struct screen_output){ .x = 960, .width = 1000, .height = 800, .pixel_width = 1000,
                                      .pixel_height = 800, .scale = 1 };
    selected_fixture = 1;
    int ax, ay, aw, ah;
    absolute_area(&ax, &ay, &aw, &ah);
    assert(ax == 0 && ay == 0 && aw == 960 && ah == 540);
    touch_ppmm = 10;
    touch_down(0, ax + 0.5 * aw, ay + 0.5 * ah, 50, 50); /* the panel's centre */
    touch_up(0);
    e = next();
    assert(e.button == BTN_LEFT && e.pressed && e.output == 0 && e.x == 480 && e.y == 270);
    assert(e.col == 96 && e.row == 26 && cursor_x == 480 && cursor_y == 270 && selected_fixture == 1);
    next();
    touch_down(0, 100, 100, 10, 10);
    touch_motion(0, 100, 140, 10, 14); /* 80 of the panel's pixels: 4 of its rows */
    e = next();
    assert(e.wheel == 4 && e.hwheel == 0 && e.output == 0 && e.x == 100 && e.y == 100);
    touch_up(0);
    assert(!pointer_next() && selected_fixture == 1);
    px = ax + 0.75 * aw; py = ay + 0.5 * ah; moved(); /* an absolute mouse or pen */
    e = next();
    assert(e.output == 0 && e.x == 720 && e.y == 270 && cursor_x == 720);
    extended_fixture = 0;
    selected_fixture = 0;
    puts("PASS extended touch, pen and absolute mice stay on the internal output with its own cells");

    desk_fixture = 2;
    nmice = 1;
    touch_hid = 0;
    px = 900; py = 100; moved(); next();
    assert(hovered == 0 && cursor_shown == 1);
    usleep(450000); /* no double click with the taps above */
    button(BTN_LEFT, 1);
    assert(recv(channel[1], request, sizeof request, MSG_DONTWAIT) == 13 && !strcmp(request, "pointer-press"));
    button(BTN_LEFT, 0); next(); next();
    px = 1200; moved(); /* onto the other program's screen */
    assert(!pointer_next() && away && hovered == -1 && cursor_shown == 0);
    assert(nsent == 4 && !strcmp(sent[3], "pointer-leave 1200"));
    px += 10; moved();
    assert(!pointer_next()); /* not ours until process 1 gives it back */
    pointer_came(950, 100);
    e = next();
    assert(!away && e.x == 950 && e.y == 100 && cursor_x == 950 && hovered == 0 && cursor_shown == 1);
    button(BTN_LEFT, 1); /* a drag stays on its screen */
    assert(recv(channel[1], request, sizeof request, MSG_DONTWAIT) == 13);
    px = 1300; moved();
    next(); e = next();
    assert(e.x == 999 && !away && nsent == 4);
    button(BTN_LEFT, 0); next();
    pointer_went();
    assert(away && hovered == -1 && cursor_shown == 0);
    desk_fixture = 0;
    puts("PASS several screens: presses are told, leaving onto another's screen stops here, enter resumes");

    assert(pointer_scroll_events(1) == 0);
    away = 0;
    scroll_input(SCROLL_FINGER, -1, 0, 1, &pipes[0]);
    next(); next();
    pointer_release();
    e = next(); assert(e.scroll_phase == SCROLL_CANCEL && e.scroll_source == SCROLL_FINGER && !pointer_next());
    scroll_input(SCROLL_FINGER, 0, 0, 1, &pipes[0]);
    assert(!pointer_next());
    assert(pointer_scroll_events(0) == 1);
    assert(ndevices == 0 && cursor_shown == 0);
    for (int i = 0; i < DEVICES; i++) {
        assert(fcntl(pipes[i][0], F_GETFD) < 0 && errno == EBADF);
        close(pipes[i][1]);
    }
    pointer_take_back(originals, DEVICES);
    assert(opened == 0);
    pointer_resume();
    assert(recv(channel[1], request, sizeof request, 0) == 8);
    close(channel[0]); close(channel[1]);
    puts("PASS all frontend copies close, every original is revoked, resume requests fresh descriptors");
    puts("ALL PASSED");
    return 0;
}
