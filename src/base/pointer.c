/* The pointer for direct-screen frontends. Process 1 owns the original input
 * devices and revokes every copy on release; only the program on the screen
 * reads its copies. It reads them through libinput, the library the shared
 * display uses, set up the same way: acceleration from rc.conf, tap to click,
 * two-finger and TrackPoint scrolling, the touchpad ignored while typing.
 * Motions move the screen's cursor at once and become K_POINTER events
 * (merged while unread). Touch screens: a tap is a click, holding still a
 * right click, dragging scrolls, two fingers pinch and scroll (libinput
 * gives only the touches; the canvas is the internal panel's, so a touch's
 * place on it is its place in the picture). Touchpad pinches are pinches;
 * swipes of three or more fingers go home (up) or to the next or previous
 * task (left, right). A pen points, its tip is the left button. No uinput,
 * daemon, or shared read of a device. With several screens, each program
 * has its own: the pointer leaving this program's for another's is told to
 * process 1, which gives the devices to that program (pointer_came there);
 * the screen it is on has a green frame.
 * Process 1 (musl) is built without libinput: it only lends and revokes. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#ifdef CONFIG_LIBINPUT
#include <libinput.h>
#endif

#include "internal.h"

#define QUEUE 64
#define CLICK_S 0.4        /* a second press within this is a double click */
#define SCROLL_PIXELS 15.0 /* touchpad and TrackPoint scrolling per wheel step */
#define TOUCHES 10
#define HOLD_S 0.5         /* a touch held this long, unmoved, is a right click */
#define SLOP_MM 3.0        /* a touch that moved less than this has not moved */
#define DOUBLE_TAP_MM 6.0  /* a second tap within this of the first: a double click */
#define PINCH_STEP 0.03    /* pinches are reported in steps of this much */
#define SWIPE_UNITS 100.0  /* a swipe this long switches (libinput's unaccelerated units: ~10 a mm) */

static int bit(const unsigned long *bits, int n)
{
    return (int)((bits[n / (8 * sizeof *bits)] >> (n % (8 * sizeof *bits))) & 1);
}

int *pointer_lend(size_t *count, char paths[][POINTER_PATH], size_t max)
{
    int *fds = NULL;
    *count = 0;
    for (int i = 0; i < 64 && *count < max; i++) {
        char path[POINTER_PATH];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
        unsigned long rel[REL_MAX / (8 * sizeof(long)) + 1] = { 0 };
        unsigned long abs[ABS_MAX / (8 * sizeof(long)) + 1] = { 0 };
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        int mouse = bit(keys, BTN_LEFT) && bit(rel, REL_X) && bit(rel, REL_Y);
        int touch = bit(keys, BTN_TOUCH) && bit(abs, ABS_X) && bit(abs, ABS_Y);
        int keyboard = bit(keys, KEY_A) && bit(keys, KEY_ENTER); /* to pair with a touchpad */
        if (!mouse && !touch && !keyboard) { close(fd); continue; }
        int *p = realloc(fds, (*count + 1) * sizeof *p);
        if (!p) { close(fd); pointer_take_back(fds, *count); *count = 0; return NULL; }
        fds = p;
        snprintf(paths[*count], POINTER_PATH, "%s", path);
        fds[(*count)++] = fd;
    }
    return fds;
}

void pointer_take_back(int *fds, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        ioctl(fds[i], EVIOCREVOKE, NULL);
        close(fds[i]);
    }
    free(fds);
}

struct device {
    int fd;
    char path[POINTER_PATH];
#ifdef CONFIG_LIBINPUT
    struct libinput_device *input;
#endif
};
static struct device *devices;
static size_t ndevices;
static int wanted, npointers;
static double px, py;                /* where the pointer is, in pixels */
static int held;                     /* buttons held: 1 left, 2 right, 4 middle */
static double scroll_v, scroll_h;    /* scrolling not yet a whole step */
static double click_at;
static int click_button, click_count, click_x, click_y;
static struct pointer_event queue[QUEUE], last;
static int qhead, qlen;
static int nmice, touch_hid, pen_near, timer_fd = -1;
static int away;                     /* on another program's screen: not ours to move */
static double touch_ppmm = 4.0;      /* pixels of the picture per millimetre of the touch screen */
static struct touch { int down; double x, y, xmm, ymm; } touches[TOUCHES];
static enum { TOUCH_IDLE, TOUCH_PENDING, TOUCH_SCROLL, TOUCH_TWO, TOUCH_DONE } touch_state;
static int touch_first, touch_second; /* their slots */
static double touch_x0, touch_y0, touch_xmm0, touch_ymm0; /* where the first went down */
static int touch_col, touch_row;     /* the cell its scrolling has reached */
static double pinch_last;            /* finger distance (mm), or libinput's scale, last reported */

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int extended_screen(void)
{
    return screen_is_extended && screen_is_extended();
}

/* The cell at (x, y) and its output: when extended, the output's own cells
 * at its native pixels, whatever output is drawn on; -1 off every output. */
static int cell_at(int x, int y, int *col, int *row)
{
    if (!extended_screen()) {
        screen_cell_at(x, y, col, row);
        return 0;
    }
    int lx, ly, output = screen_output_at(x, y, &lx, &ly);
    if (output < 0) return -1;
    int previous = screen_output_select(output);
    screen_cell_at(lx, ly, col, row);
    if (previous >= 0) screen_output_select(previous);
    return output;
}

/* Kept on the outputs when extended: moved only if its pixel is off them, so
 * motions of less than a pixel add up. */
static void keep_on_outputs(void)
{
    int x = (int)px, y = (int)py;
    screen_output_project(&x, &y);
    if (x != (int)px) px = x;
    if (y != (int)py) py = y;
}

static void push(struct pointer_event e)
{
    e.output = cell_at(e.x, e.y, &e.col, &e.row);
    if (e.output < 0) return;
    e.buttons = held;
    if (qlen > 0) {
        struct pointer_event *tail = &queue[(qhead + qlen - 1) % QUEUE];
        if (!e.button && !e.wheel && !e.hwheel && !e.gesture &&
            !tail->button && !tail->wheel && !tail->hwheel && !tail->gesture) {
            *tail = e; /* motions merge while unread */
            return;
        }
    }
    if (qlen == QUEUE) {
        qhead = (qhead + 1) % QUEUE; /* the oldest goes */
        qlen--;
    }
    queue[(qhead + qlen++) % QUEUE] = e;
}

/* What libinput's events become (unused where it is not built in: process 1). */
#define EVENTS __attribute__((unused))

/* The cursor is shown while a mouse is there or a pen near, but not after a
 * touch until a mouse moves again. */
static void show_cursor(void)
{
    screen_cursor_visible(!away && !touch_hid && (nmice > 0 || pen_near));
}

static int several_screens(void)
{
    return screen_desk_count && screen_desk_count() > 1;
}

/* The pointer went onto another program's screen, at (gx, gy) on the
 * desktop: process 1 gives it the devices; here it is gone until it comes
 * back (pointer_came) or the screen is taken again. */
static void leave(int gx, int gy)
{
    char x[16], y[16];
    snprintf(x, sizeof x, "%d", gx);
    snprintf(y, sizeof y, "%d", gy);
    const char *const fields[] = { "pointer-leave", x, y, NULL };
    if (control_fd() < 0 || control_send_fields(control_fd(), fields) < 0) return;
    away = 1;
    show_cursor();
    if (screen_hover) screen_hover(-1);
}

/* The screen the pointer is on, framed when there are several. */
static void hover(void)
{
    if (!screen_hover || !several_screens()) return;
    screen_hover(away || nmice == 0 ? -1 : extended_screen() ? screen_output_at((int)px, (int)py, NULL, NULL) : 0);
}

/* What absolute devices (touch screens, pens, absolute mice) point at, at
 * (*x, *y) and *w by *h: the picture, or when extended the first output (the
 * internal panel) in desktop coordinates, its native pixels over its scale,
 * whatever output is drawn on. */
EVENTS static void absolute_area(int *x, int *y, int *w, int *h)
{
    struct screen_output o;
    if (extended_screen() && screen_output_get(0, &o) == 0) {
        *x = o.x;
        *y = o.y;
        *w = o.width;
        *h = o.height;
        return;
    }
    *x = *y = 0;
    screen_pixels(w, h);
}

/* The pointer at (px, py), kept on the picture; onto another program's
 * screen, unless a button is held (a drag stays). */
EVENTS static void moved(void)
{
    int w, h, gx, gy;
    if (away) return;
    if (touch_hid) {
        touch_hid = 0;
        show_cursor();
    }
    int crossing = !held && several_screens() && screen_to_desk((int)px, (int)py, &gx, &gy) == 0 &&
                   screen_desk_foreign(gx, gy);
    if (extended_screen()) {
        keep_on_outputs();
    } else {
        screen_pixels(&w, &h);
        px = px < 0 ? 0 : px > w - 1 ? w - 1 : px;
        py = py < 0 ? 0 : py > h - 1 ? h - 1 : py;
    }
    if (crossing) {
        leave(gx, gy);
        return;
    }
    screen_cursor_move((int)px, (int)py);
    hover();
    push((struct pointer_event){ .x = (int)px, .y = (int)py });
}

/* A press or release of an evdev button code; a second press within slack
 * pixels of the first (and soon) is a double click. */
EVENTS static void press(int code, int pressed, int slack)
{
    int mask = code == BTN_LEFT ? 1 : code == BTN_RIGHT ? 2 : code == BTN_MIDDLE ? 4 : 0;
    if (!mask) return;
    held = pressed ? held | mask : held & ~mask;
    struct pointer_event e = { .x = (int)px, .y = (int)py, .button = code, .pressed = pressed };
    if (pressed) {
        double t = now();
        int near = abs(e.x - click_x) <= slack && abs(e.y - click_y) <= slack;
        click_count = code == click_button && near && t - click_at < CLICK_S && click_count < 3 ?
                      click_count + 1 : 1;
        click_at = t;
        click_button = code;
        click_x = e.x;
        click_y = e.y;
        e.clicks = click_count;
        /* With several screens: the keyboard comes to the one pressed on. */
        if (several_screens() && control_fd() >= 0) control_send(control_fd(), "pointer-press", 13, NULL, 0);
    }
    push(e);
}

EVENTS static void button(int code, int pressed)
{
    press(code, pressed, 4 * (int)(screen_scale() + 0.5));
}

/* Scrolling, in wheel steps positive down, as libinput gives it. */
EVENTS static void scroll(double v, double h)
{
    scroll_v += v;
    scroll_h += h;
    int sv = (int)scroll_v, sh = (int)scroll_h;
    scroll_v -= sv;
    scroll_h -= sh;
    if (sv || sh) {
        push((struct pointer_event){ .x = (int)px, .y = (int)py, .wheel = -sv, .hwheel = sh });
    }
}

/* The touch screen's hold timer (none without libinput: tests call
 * touch_timer themselves); 0 disarms it. */
EVENTS static void arm(double seconds)
{
    if (timer_fd < 0) return;
    struct itimerspec t = { .it_value = { (time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9) } };
    timerfd_settime(timer_fd, 0, &t, NULL);
}

/* A click where the touch went down; double taps by millimetres. */
EVENTS static void tap(int code)
{
    int slack = (int)(DOUBLE_TAP_MM * touch_ppmm);
    px = touch_x0;
    py = touch_y0;
    screen_cursor_move((int)px, (int)py);
    press(code, 1, slack);
    press(code, 0, slack);
}

static double fingers_apart(void)
{
    const struct touch *a = &touches[touch_first], *b = &touches[touch_second];
    return sqrt((a->xmm - b->xmm) * (a->xmm - b->xmm) + (a->ymm - b->ymm) * (a->ymm - b->ymm));
}

/* Scrolling by a finger at (x, y): a wheel step for each row (a horizontal
 * one for each column) it went, the content following the finger, reported
 * where the touch began. */
EVENTS static void drag(double x, double y)
{
    int col, row;
    if (cell_at((int)x, (int)y, &col, &row) < 0) return;
    if (row != touch_row || col != touch_col) {
        push((struct pointer_event){ .x = (int)touch_x0, .y = (int)touch_y0,
                                     .wheel = row - touch_row, .hwheel = touch_col - col });
        touch_row = row;
        touch_col = col;
    }
}

EVENTS static void touch_down(int slot, double x, double y, double xmm, double ymm)
{
    if (slot < 0 || slot >= TOUCHES) return;
    touches[slot] = (struct touch){ 1, x, y, xmm, ymm };
    if (!touch_hid) {
        touch_hid = 1;
        show_cursor();
    }
    if (touch_state == TOUCH_IDLE) {
        touch_state = TOUCH_PENDING;
        touch_first = slot;
        touch_x0 = x;
        touch_y0 = y;
        touch_xmm0 = xmm;
        touch_ymm0 = ymm;
        cell_at((int)x, (int)y, &touch_col, &touch_row);
        arm(HOLD_S);
    } else if ((touch_state == TOUCH_PENDING || touch_state == TOUCH_SCROLL) && slot != touch_first) {
        touch_state = TOUCH_TWO;
        touch_second = slot;
        pinch_last = fingers_apart();
        cell_at((int)((touches[touch_first].x + x) / 2), (int)((touches[touch_first].y + y) / 2),
                &touch_col, &touch_row);
        arm(0);
    }
}

EVENTS static void touch_motion(int slot, double x, double y, double xmm, double ymm)
{
    if (slot < 0 || slot >= TOUCHES || !touches[slot].down) return;
    touches[slot] = (struct touch){ 1, x, y, xmm, ymm };
    if (touch_state == TOUCH_PENDING && slot == touch_first &&
        sqrt((xmm - touch_xmm0) * (xmm - touch_xmm0) + (ymm - touch_ymm0) * (ymm - touch_ymm0)) >= SLOP_MM) {
        touch_state = TOUCH_SCROLL;
        arm(0);
    }
    if (touch_state == TOUCH_SCROLL && slot == touch_first) {
        drag(x, y);
    } else if (touch_state == TOUCH_TWO && (slot == touch_first || slot == touch_second)) {
        double mx = (touches[touch_first].x + touches[touch_second].x) / 2;
        double my = (touches[touch_first].y + touches[touch_second].y) / 2, d = fingers_apart();
        if (pinch_last > 0 && fabs(d / pinch_last - 1) >= PINCH_STEP) {
            push((struct pointer_event){ .x = (int)mx, .y = (int)my, .gesture = GESTURE_PINCH,
                                         .scale = d / pinch_last });
            pinch_last = d;
        }
        drag(mx, my);
    }
}

EVENTS static void touch_up(int slot)
{
    if (slot < 0 || slot >= TOUCHES || !touches[slot].down) return;
    touches[slot].down = 0;
    if (touch_state == TOUCH_PENDING && slot == touch_first) {
        arm(0);
        tap(BTN_LEFT);
    } else if (touch_state == TOUCH_TWO) {
        touch_state = TOUCH_DONE; /* the other finger lifting is no tap */
    }
    int left = 0;
    for (int i = 0; i < TOUCHES; i++) left += touches[i].down;
    if (left == 0) touch_state = TOUCH_IDLE;
}

/* Held still: a right click (a menu), and nothing more from this touch. */
EVENTS static void touch_timer(void)
{
    if (touch_state != TOUCH_PENDING) return;
    touch_state = TOUCH_DONE;
    tap(BTN_RIGHT);
}

static void touch_reset(void)
{
    memset(touches, 0, sizeof touches);
    touch_state = TOUCH_IDLE;
    arm(0);
}

/* A touchpad pinch: libinput's scale since it began, reported in steps. */
EVENTS static void pinch(double scale_now)
{
    if (pinch_last > 0 && fabs(scale_now / pinch_last - 1) >= PINCH_STEP) {
        push((struct pointer_event){ .x = (int)px, .y = (int)py, .gesture = GESTURE_PINCH,
                                     .scale = scale_now / pinch_last });
        pinch_last = scale_now;
    }
}

/* A swipe of three or more fingers ended: up goes home (as Super+Esc), left
 * and right to the next and previous task; process 1 does it. */
EVENTS static void swipe(int fingers, double dx, double dy)
{
    static const char *const home[] = { "home", NULL };
    static const char *const next[] = { "switch", "next", NULL }, *const prev[] = { "switch", "prev", NULL };
    if (fingers < 3 || control_fd() < 0) return;
    if (-dy > SWIPE_UNITS && -dy > fabs(dx)) {
        control_send_fields(control_fd(), home);
    } else if (fabs(dx) > SWIPE_UNITS && fabs(dx) > fabs(dy)) {
        control_send_fields(control_fd(), dx < 0 ? next : prev);
    }
}

#ifdef CONFIG_LIBINPUT
static struct libinput *li;
static int epoll_fd = -1;             /* libinput's fd and the hold timer */
static double swipe_dx, swipe_dy;

static int open_restricted(const char *path, int flags, void *data)
{
    (void)flags;
    (void)data;
    for (size_t i = 0; i < ndevices; i++) {
        if (devices[i].fd >= 0 && strcmp(devices[i].path, path) == 0) {
            /* libinput may close a rejected device before udev tags it.
             * Keep our lease for the next processed-device event to retry. */
            int fd = fcntl(devices[i].fd, F_DUPFD_CLOEXEC, 0);
            return fd < 0 ? -errno : fd;
        }
    }
    return -ENOENT;
}

static void close_restricted(int fd, void *data)
{
    (void)data;
    close(fd); /* the duplicate returned by open_restricted, not our lease */
}

static const struct libinput_interface interface = { open_restricted, close_restricted };

static void on_setting(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "pointer_accel") == 0) {
        *(double *)arg = strtod(value, NULL);
    }
}

static int is_pointer(struct libinput_device *d)
{
    return libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_POINTER) ||
           libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_TOUCH) ||
           libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_TABLET_TOOL);
}

/* As labwc sets it up: its defaults, and rc.conf's acceleration. */
static void configure(struct libinput_device *d)
{
    double accel = 0;
    rc_read(on_setting, &accel);
    if (libinput_device_config_accel_is_available(d)) {
        libinput_device_config_accel_set_speed(d, accel < -1 ? -1 : accel > 1 ? 1 : accel);
    }
    if (libinput_device_config_tap_get_finger_count(d) > 0) {
        libinput_device_config_tap_set_enabled(d, LIBINPUT_CONFIG_TAP_ENABLED);
    }
    if (libinput_device_config_scroll_has_natural_scroll(d)) {
        libinput_device_config_scroll_set_natural_scroll_enabled(d, 0);
    }
}

static double axis(struct libinput_event_pointer *p, enum libinput_pointer_axis a, int v120, double per_step)
{
    if (!libinput_event_pointer_has_axis(p, a)) return 0;
    return v120 ? libinput_event_pointer_get_scroll_value_v120(p, a) / 120.0 :
                  libinput_event_pointer_get_scroll_value(p, a) / per_step;
}

/* A pen over the picture (a screen's pen) or a tablet (all the picture). */
static void pen_moved(struct libinput_event_tablet_tool *pen, int x, int y, int w, int h)
{
    px = x + libinput_event_tablet_tool_get_x_transformed(pen, (uint32_t)w);
    py = y + libinput_event_tablet_tool_get_y_transformed(pen, (uint32_t)h);
    moved();
}

static void handle(struct libinput_event *ev)
{
    enum libinput_event_type type = libinput_event_get_type(ev);
    /* Each is asked of its own kind only: libinput reports any other ask. */
    int pointer = type >= LIBINPUT_EVENT_POINTER_MOTION && type < LIBINPUT_EVENT_TOUCH_DOWN;
    int touch = type >= LIBINPUT_EVENT_TOUCH_DOWN && type < LIBINPUT_EVENT_TABLET_TOOL_AXIS;
    int tool = type >= LIBINPUT_EVENT_TABLET_TOOL_AXIS && type < LIBINPUT_EVENT_TABLET_PAD_BUTTON;
    int gesture = type >= LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN && type < LIBINPUT_EVENT_SWITCH_TOGGLE;
    struct libinput_event_pointer *p = pointer ? libinput_event_get_pointer_event(ev) : NULL;
    struct libinput_event_touch *t = touch ? libinput_event_get_touch_event(ev) : NULL;
    struct libinput_event_tablet_tool *pen = tool ? libinput_event_get_tablet_tool_event(ev) : NULL;
    struct libinput_event_gesture *g = gesture ? libinput_event_get_gesture_event(ev) : NULL;
    struct libinput_device *d = libinput_event_get_device(ev);
    enum libinput_pointer_axis v = LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL;
    enum libinput_pointer_axis h = LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL;
    int ax, ay, w, hgt;
    absolute_area(&ax, &ay, &w, &hgt);
    switch (type) {
    case LIBINPUT_EVENT_DEVICE_ADDED:
    case LIBINPUT_EVENT_DEVICE_REMOVED: {
        int sign = type == LIBINPUT_EVENT_DEVICE_ADDED ? 1 : -1;
        double w_mm, h_mm;
        if (sign > 0) configure(d);
        if (sign > 0 && libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_TOUCH) &&
            libinput_device_get_size(d, &w_mm, &h_mm) == 0 && w_mm > 0) {
            touch_ppmm = w / w_mm;
        }
        npointers += sign * is_pointer(d);
        nmice += sign * libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_POINTER);
        show_cursor();
        hover(); /* the frame, once the pointer that came over has its devices */
        break;
    }
    case LIBINPUT_EVENT_POINTER_MOTION:
        /* Deltas are for a picture at scale 1, as on the shared display. */
        px += libinput_event_pointer_get_dx(p) * (extended_screen() ? 1.0 : screen_scale());
        py += libinput_event_pointer_get_dy(p) * (extended_screen() ? 1.0 : screen_scale());
        moved();
        break;
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
        px = ax + libinput_event_pointer_get_absolute_x_transformed(p, (uint32_t)w);
        py = ay + libinput_event_pointer_get_absolute_y_transformed(p, (uint32_t)hgt);
        moved();
        break;
    case LIBINPUT_EVENT_POINTER_BUTTON:
        button((int)libinput_event_pointer_get_button(p),
               libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED);
        break;
    case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
        scroll(axis(p, v, 1, 1), axis(p, h, 1, 1));
        break;
    case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
    case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
        scroll(axis(p, v, 0, SCROLL_PIXELS), axis(p, h, 0, SCROLL_PIXELS));
        break;
    case LIBINPUT_EVENT_TOUCH_DOWN:
    case LIBINPUT_EVENT_TOUCH_MOTION:
        (type == LIBINPUT_EVENT_TOUCH_DOWN ? touch_down : touch_motion)(
            libinput_event_touch_get_seat_slot(t), ax + libinput_event_touch_get_x_transformed(t, (uint32_t)w),
            ay + libinput_event_touch_get_y_transformed(t, (uint32_t)hgt), libinput_event_touch_get_x(t),
            libinput_event_touch_get_y(t));
        break;
    case LIBINPUT_EVENT_TOUCH_UP:
        touch_up(libinput_event_touch_get_seat_slot(t));
        break;
    case LIBINPUT_EVENT_TOUCH_CANCEL:
        touch_reset();
        break;
    case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN:
        pinch_last = 1;
        break;
    case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE:
        pinch(libinput_event_gesture_get_scale(g));
        break;
    case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
        swipe_dx = swipe_dy = 0;
        break;
    case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
        swipe_dx += libinput_event_gesture_get_dx_unaccelerated(g);
        swipe_dy += libinput_event_gesture_get_dy_unaccelerated(g);
        break;
    case LIBINPUT_EVENT_GESTURE_SWIPE_END:
        if (!libinput_event_gesture_get_cancelled(g)) {
            swipe(libinput_event_gesture_get_finger_count(g), swipe_dx, swipe_dy);
        }
        break;
    case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
        pen_near = libinput_event_tablet_tool_get_proximity_state(pen) ==
                   LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_IN;
        touch_hid = 0;
        show_cursor();
        pen_moved(pen, ax, ay, w, hgt);
        break;
    case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
    case LIBINPUT_EVENT_TABLET_TOOL_TIP:
        pen_moved(pen, ax, ay, w, hgt);
        if (type == LIBINPUT_EVENT_TABLET_TOOL_TIP) {
            button(BTN_LEFT, libinput_event_tablet_tool_get_tip_state(pen) == LIBINPUT_TABLET_TOOL_TIP_DOWN);
        }
        break;
    case LIBINPUT_EVENT_TABLET_TOOL_BUTTON: {
        uint32_t code = libinput_event_tablet_tool_get_button(pen);
        if (code == BTN_STYLUS || code == BTN_STYLUS2) {
            button(code == BTN_STYLUS ? BTN_RIGHT : BTN_MIDDLE,
                   libinput_event_tablet_tool_get_button_state(pen) == LIBINPUT_BUTTON_STATE_PRESSED);
        }
        break;
    }
    default:
        break;
    }
}
#endif

void pointer_remove(const char *path)
{
    for (size_t i = 0; i < ndevices; i++) {
        if (strcmp(devices[i].path, path) != 0) continue;
#ifdef CONFIG_LIBINPUT
        if (devices[i].input) {
            libinput_path_remove_device(devices[i].input);
            libinput_device_unref(devices[i].input);
        }
#endif
        if (devices[i].fd >= 0) close(devices[i].fd);
        ndevices--;
        memmove(devices + i, devices + i + 1, (ndevices - i) * sizeof *devices);
        pointer_dispatch(); /* device-removed and any releases generated by libinput */
        return;
    }
}

void pointer_retry(void)
{
#ifdef CONFIG_LIBINPUT
    if (!li) return;
    for (size_t i = 0; i < ndevices; i++) {
        if (!devices[i].input) {
            struct libinput_device *input = libinput_path_add_device(li, devices[i].path);
            if (input) devices[i].input = libinput_device_ref(input);
        }
    }
    pointer_dispatch();
#endif
}

int pointer_take(int fd, const char *path)
{
    for (size_t i = 0; i < ndevices; i++) {
        if (strcmp(devices[i].path, path) != 0) continue;
        if (devices[i].fd >= 0 && input_same(devices[i].fd, fd)) { close(fd); return 0; }
        pointer_remove(path);
        break;
    }
    struct device *d = realloc(devices, (ndevices + 1) * sizeof *d);
    if (!d) { close(fd); return -1; }
    devices = d;
    devices[ndevices].fd = fd;
    snprintf(devices[ndevices].path, sizeof devices[ndevices].path, "%s", path);
    ndevices++;
#ifdef CONFIG_LIBINPUT
    devices[ndevices - 1].input = NULL;
    if (!li) {
        li = libinput_path_create_context(&interface, NULL);
        timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        int watched[2] = { li ? libinput_get_fd(li) : -1, timer_fd };
        for (int i = 0; i < 2 && epoll_fd >= 0; i++) {
            struct epoll_event e = { .events = EPOLLIN, .data.fd = watched[i] };
            if (watched[i] >= 0) epoll_ctl(epoll_fd, EPOLL_CTL_ADD, watched[i], &e);
        }
    }
    struct libinput_device *input = li ? libinput_path_add_device(li, path) : NULL;
    if (input) {
        devices[ndevices - 1].input = libinput_device_ref(input);
        pointer_dispatch(); /* its "added" event: set it up */
        return 0;
    }
#endif
    return -1; /* kept until release; libinput did not take it */
}

/* From process 1: the pointer is ours again, at (gx, gy) on the desktop. */
void pointer_came(int gx, int gy)
{
    int x, y;
    away = 0;
    if (screen_from_desk && screen_from_desk(gx, gy, &x, &y) == 0) {
        px = x;
        py = y;
    }
    show_cursor();
    moved();
}

/* From process 1: the pointer went to another program (it took the
 * devices), not across an edge of ours. */
void pointer_went(void)
{
    away = 1;
    show_cursor();
    if (screen_hover) screen_hover(-1);
}

void pointer_release(void)
{
    away = 0;
#ifdef CONFIG_LIBINPUT
    if (li) {
        for (size_t i = 0; i < ndevices; i++) {
            if (devices[i].input) libinput_device_unref(devices[i].input);
        }
        libinput_unref(li); /* closes its devices */
        li = NULL;
    }
    if (epoll_fd >= 0) close(epoll_fd);
    epoll_fd = -1;
#endif
    touch_reset();
    if (timer_fd >= 0) close(timer_fd);
    timer_fd = -1;
    for (size_t i = 0; i < ndevices; i++) {
        if (devices[i].fd >= 0) close(devices[i].fd);
    }
    free(devices);
    devices = NULL;
    ndevices = 0;
    npointers = nmice = 0;
    touch_hid = pen_near = 0;
    held = 0;
    qlen = 0;
    scroll_v = scroll_h = 0;
    screen_cursor_visible(0);
}

void pointer_resume(void)
{
    if (extended_screen()) keep_on_outputs();
    if (wanted && control_fd() >= 0) control_send(control_fd(), "pointer?", 8, NULL, 0);
}

/* keys_open asks for it: every program on the screen has the pointer. */
int pointer_open(void)
{
    if (!wanted) {
        int w, h;
        screen_pixels(&w, &h);
        px = w / 2;
        py = h / 2;
        screen_cursor_move((int)px, (int)py); /* shown there before any motion */
    }
    wanted = 1;
    if (control_fd() < 0) return -1;
    pointer_resume();
    return 0;
}

int pointer_fd(void)
{
#ifdef CONFIG_LIBINPUT
    return !li ? -1 : epoll_fd >= 0 ? epoll_fd : libinput_get_fd(li);
#else
    return -1;
#endif
}

void pointer_dispatch(void)
{
#ifdef CONFIG_LIBINPUT
    uint64_t expired;
    if (timer_fd >= 0 && read(timer_fd, &expired, sizeof expired) == (ssize_t)sizeof expired) {
        touch_timer();
    }
    if (!li || libinput_dispatch(li) != 0) return;
    struct libinput_event *ev;
    while ((ev = libinput_get_event(li)) != NULL) {
        handle(ev);
        libinput_event_destroy(ev);
    }
#endif
}

int pointer_next(void)
{
    if (qlen == 0) return 0;
    last = queue[qhead];
    qhead = (qhead + 1) % QUEUE;
    qlen--;
    return 1;
}

const struct pointer_event *pointer_last(void)
{
    return &last;
}

int pointer_present(void)
{
    return npointers > 0;
}
