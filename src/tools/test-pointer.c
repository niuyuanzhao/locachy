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
void screen_pixels(int *w, int *h) { *w = 1000; *h = 800; }
void screen_cell_at(int x, int y, int *col, int *row) { *col = x / 10; *row = y / 20 - 1; }
void screen_cursor_move(int x, int y) { cursor_x = x; cursor_y = y; }
void screen_cursor_visible(int on) { cursor_shown = on; }
double screen_scale(void) { return 1.0; }
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

    pointer_release();
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
