/* libinput end to end through base/pointer.c, in a disposable VM only
 * (test-pointer-libinput.sh): uinput makes a mouse, a clickpad, a TrackPoint,
 * a touchscreen and a screen pen, the real udevd tags them, and pointer.c
 * reads them as the program on the screen reads the copies process 1 lends
 * it. Never run it where a real display or real input is in use. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define CONFIG_LIBINPUT
#include "../base/pointer.c"
#include <assert.h>
#include <dirent.h>
#include <linux/netlink.h>
#include <math.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <linux/uinput.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static int cursor_x, cursor_y, cursor_shown;
static const char *accel = "0";
static char asked[8][32]; /* what swipes asked process 1 for */
static int nasked;
static int wait_for_tags = 1;
int control_fd(void) { return 99; }
int control_send(int sock, const char *msg, size_t len, const int *fds, int n)
{ (void)sock; (void)msg; (void)len; (void)fds; (void)n; abort(); }
int control_send_fields(int sock, const char *const *fields)
{
    assert(sock == 99 && nasked < 8);
    snprintf(asked[nasked++], sizeof asked[0], "%s%s%s", fields[0], fields[1] ? " " : "", fields[1] ? fields[1] : "");
    return 0;
}
void screen_pixels(int *w, int *h) { *w = 1000; *h = 800; }
void screen_cell_at(int x, int y, int *col, int *row) { *col = x / 10; *row = y / 20; }
void screen_cursor_move(int x, int y) { cursor_x = x; cursor_y = y; }
void screen_cursor_visible(int on) { cursor_shown = on; }
double screen_scale(void) { return 1.0; }
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{ fn("pointer_accel", accel, arg); return 0; }

struct axis { int code, min, max, res; };

/* Wait until udevd has tagged node as input, and show its tags. */
static void tagged(const char *node)
{
    char db[64], buf[4096];
    for (int i = 0; i < 500; i++) {
        struct stat st;
        ssize_t n = -1;
        if (stat(node, &st) == 0) {
            snprintf(db, sizeof db, "/run/udev/data/c%u:%u", major(st.st_rdev), minor(st.st_rdev));
            int fd = open(db, O_RDONLY | O_CLOEXEC);
            n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
            if (fd >= 0) close(fd);
        }
        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "E:ID_INPUT=1\n")) {
                printf("%s:", node);
                for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
                    if (strncmp(line, "E:ID_INPUT_", 11) == 0) printf(" %s", line + 2);
                putchar('\n');
                return;
            }
        }
        usleep(10000);
    }
    assert(!"udevd tagged the device");
}

/* A uinput device; its event node into node. Lists end with -1. */
static int device_ids(const char *name, int bus, int vendor, int product, const int *keys, const int *rels,
                      const struct axis *axes, const int *props, char *node, size_t size)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    assert(fd >= 0);
    if (keys) assert(ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0);
    for (; keys && *keys >= 0; keys++) assert(ioctl(fd, UI_SET_KEYBIT, *keys) == 0);
    if (rels) assert(ioctl(fd, UI_SET_EVBIT, EV_REL) == 0);
    for (; rels && *rels >= 0; rels++) assert(ioctl(fd, UI_SET_RELBIT, *rels) == 0);
    if (axes) assert(ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0);
    for (; axes && axes->code >= 0; axes++) {
        struct uinput_abs_setup a = { .code = (uint16_t)axes->code,
            .absinfo = { .minimum = axes->min, .maximum = axes->max, .resolution = axes->res } };
        assert(ioctl(fd, UI_SET_ABSBIT, axes->code) == 0 && ioctl(fd, UI_ABS_SETUP, &a) == 0);
    }
    for (; props && *props >= 0; props++) assert(ioctl(fd, UI_SET_PROPBIT, *props) == 0);
    struct uinput_setup s = { .id = { .bustype = (uint16_t)bus, .vendor = (uint16_t)vendor, .product = (uint16_t)product,
                                      .version = 1 } };
    snprintf(s.name, sizeof s.name, "%s", name);
    assert(ioctl(fd, UI_DEV_SETUP, &s) == 0 && ioctl(fd, UI_DEV_CREATE) == 0);
    char sys[64] = "", path[160];
    assert(ioctl(fd, UI_GET_SYSNAME(sizeof sys), sys) >= 0);
    snprintf(path, sizeof path, "/sys/devices/virtual/input/%s", sys);
    DIR *d = opendir(path);
    assert(d);
    node[0] = '\0';
    for (struct dirent *e; (e = readdir(d)) != NULL;)
        if (strncmp(e->d_name, "event", 5) == 0) snprintf(node, size, "/dev/input/%.40s", e->d_name);
    closedir(d);
    assert(node[0]);
    if (wait_for_tags) tagged(node);
    return fd;
}

static int device(const char *name, int bus, const int *keys, const int *rels, const struct axis *axes,
                  const int *props, char *node, size_t size)
{
    return device_ids(name, bus, 0x1d6b, 0x0104, keys, rels, axes, props, node, size);
}

static void emit(int fd, int type, int code, int value)
{
    struct input_event e = { .type = (uint16_t)type, .code = (uint16_t)code, .value = value };
    assert(write(fd, &e, sizeof e) == (ssize_t)sizeof e);
}
static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

static int tracking = 1;
static void touch(int fd, int slot, int x, int y, int down)
{
    emit(fd, EV_ABS, ABS_MT_SLOT, slot);
    if (down) emit(fd, EV_ABS, ABS_MT_TRACKING_ID, tracking++);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, x);
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, y);
}
static void lift(int fd, int slot)
{
    emit(fd, EV_ABS, ABS_MT_SLOT, slot);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
}

/* What pointer.c made of the input, dispatching as a program on the screen
 * does (libinput's timers run meanwhile). */
static struct pointer_event got[512];
static int ngot;
static void collect(int ms)
{
    for (double end = now() + ms / 1000.0; now() < end;) {
        struct pollfd p = { .fd = pointer_fd(), .events = POLLIN };
        poll(&p, 1, 5);
        pointer_dispatch();
        while (pointer_next()) {
            assert(ngot < 512);
            got[ngot++] = *pointer_last();
        }
    }
}
static int presses(int button, int pressed)
{
    int n = 0;
    for (int i = 0; i < ngot; i++) n += got[i].button == button && got[i].pressed == pressed;
    return n;
}
static int buttons(void)
{
    int n = 0;
    for (int i = 0; i < ngot; i++) n += got[i].button != 0;
    return n;
}
static int wheel(void)
{
    int n = 0;
    for (int i = 0; i < ngot; i++) n += got[i].wheel;
    return n;
}

/* The sections settings writes for Touchpad Circular Scrolling on and off
 * (progs/settings.c; test-settings-monitors checks the text). */
#define CIRCULAR_QUIRK(value) "[LOCachy touchpad circular scrolling]\n" \
    "# settings: Touchpad Circular Scrolling (rc.conf touchpad_circular)\n" \
    "MatchUdevType=touchpad\nModelCircularTouchpad=" value "\n"

static void local_quirks(const char *text)
{
    if (!text) {
        assert(unlink("/etc/libinput/local-overrides.quirks") == 0 || errno == ENOENT);
        return;
    }
    assert(mkdir("/etc/libinput", 0755) == 0 || errno == EEXIST);
    FILE *f = fopen("/etc/libinput/local-overrides.quirks", "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static int two_finger_scroll(int fd, int horizontal, int direction)
{
    ngot = 0;
    touch(fd, 0, 280, 260, 1);
    touch(fd, 1, 450, 260, 1);
    emit(fd, EV_KEY, BTN_TOUCH, 1); emit(fd, EV_KEY, BTN_TOOL_DOUBLETAP, 1);
    emit(fd, EV_ABS, ABS_X, 280); emit(fd, EV_ABS, ABS_Y, 260); syn(fd);
    collect(20);
    for (int i = 1; i <= 12; i++) {
        int offset = direction * 12 * i;
        int x = horizontal ? offset : 0, y = horizontal ? 0 : offset;
        touch(fd, 0, 280 + x, 260 + y, 0);
        touch(fd, 1, 450 + x, 260 + y, 0);
        emit(fd, EV_ABS, ABS_X, 280 + x); emit(fd, EV_ABS, ABS_Y, 260 + y); syn(fd);
        collect(10);
    }
    lift(fd, 0); lift(fd, 1);
    emit(fd, EV_KEY, BTN_TOUCH, 0); emit(fd, EV_KEY, BTN_TOOL_DOUBLETAP, 0); syn(fd);
    collect(300);
    int amount = 0;
    for (int i = 0; i < ngot; i++) amount += horizontal ? got[i].hwheel : got[i].wheel;
    assert(!buttons());
    return amount;
}

/* A finger once round a square pad's middle c (in device units), r from it,
 * clockwise or not, with tap to click on as configure() sets it; the wheel
 * steps it gave. */
static int circle(int fd, int c, int r, int clockwise)
{
    ngot = 0;
    for (int i = 0; i <= 72; i++) {
        double a = (clockwise ? 1 : -1) * i * 5 * M_PI / 180;
        int x = c + (int)lround(r * cos(a)), y = c + (int)lround(r * sin(a)); /* y down: clockwise */
        touch(fd, 0, x, y, i == 0);
        if (i == 0) {
            emit(fd, EV_KEY, BTN_TOUCH, 1);
            emit(fd, EV_KEY, BTN_TOOL_FINGER, 1);
        }
        emit(fd, EV_ABS, ABS_X, x); emit(fd, EV_ABS, ABS_Y, y); syn(fd);
        collect(i == 0 ? 350 : 10);
    }
    lift(fd, 0);
    emit(fd, EV_KEY, BTN_TOUCH, 0); emit(fd, EV_KEY, BTN_TOOL_FINGER, 0); syn(fd);
    collect(300);
    return wheel();
}

static uint32_t scroll_methods(void)
{
    assert(ndevices == 1 && devices[0].input);
    return libinput_device_config_scroll_get_methods(devices[0].input);
}

/* A pen hovering for ms, as real ones keep reporting (libinput takes a
 * silent one for gone); x wobbles a unit so that no event is a repeat. */
static void hover(int fd, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        emit(fd, EV_ABS, ABS_X, 10000 + (i & 1));
        syn(fd);
        collect(10);
    }
}

static void take(const char *node)
{
    int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    assert(fd >= 0);
    assert(pointer_take(fd, node) == 0); /* libinput takes it; its added event set it up */
    ngot = 0;
    collect(100);
    ngot = 0;
}

/* How far the pointer goes for the same slow mouse motion at a setting. */
static int travel(const char *setting, const char *node, int fd)
{
    accel = setting;
    take(node);
    px = 100;
    py = 400;
    for (int i = 0; i < 10; i++) {
        emit(fd, EV_REL, REL_X, 4);
        syn(fd);
        collect(8);
    }
    collect(50);
    assert(ngot >= 1 && got[ngot - 1].y == 400 && cursor_x == got[ngot - 1].x && !buttons());
    pointer_release();
    return got[ngot - 1].x - 100;
}

static int open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    assert(d);
    int n = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

/* Real path-device refs and real evdev revoke/removal, in this VM only.
 * Keep the context and original pointer live throughout the reconnects. */
static void expected_rejection(struct libinput *context, enum libinput_log_priority priority,
                               const char *format, va_list args)
{
    (void)context;
    (void)priority;
    fputs("EXPECTED pre-udev rejection: ", stderr);
    vfprintf(stderr, format, args);
}

static void hotplug(void)
{
    char anchor_node[64], node[64];
    int anchor = device("LOCachy hotplug anchor", BUS_USB, (const int[]){ BTN_LEFT, BTN_RIGHT, -1 },
                        (const int[]){ REL_X, REL_Y, -1 }, NULL, NULL, anchor_node, sizeof anchor_node);
    take(anchor_node);
    assert(ndevices == 1 && devices[0].input && npointers == 1);
    struct libinput *context = li;
    int anchor_lease = devices[0].fd;
    int idle_fds = open_fds();
    int events = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_groups = 1 | 2 };
    assert(events >= 0 && bind(events, (struct sockaddr *)&addr, sizeof addr) == 0);
    assert(system("/usr/bin/udevadm control --stop-exec-queue") == 0);
    wait_for_tags = 0;
    int mouse = device("LOCachy hotplug mouse", BUS_BLUETOOTH, (const int[]){ BTN_LEFT, BTN_RIGHT, -1 },
                       (const int[]){ REL_X, REL_Y, -1 }, NULL, NULL, node, sizeof node);
    int owner = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    assert(owner >= 0);
    libinput_log_set_handler(context, expected_rejection);
    assert(pointer_take(dup(owner), node) < 0);
    libinput_log_set_handler(context, NULL);
    assert(li == context && ndevices == 2 && !devices[1].input && fcntl(devices[1].fd, F_GETFD) >= 0);
    assert(system("/usr/bin/udevadm control --start-exec-queue") == 0);
    int processed = 0;
    for (double end = now() + 5; !processed && now() < end;) {
        struct pollfd p = { .fd = events, .events = POLLIN };
        poll(&p, 1, 20);
        char message[8192];
        ssize_t len;
        while ((len = recv(events, message, sizeof message, MSG_DONTWAIT)) > 0) {
            if (len < 8 || memcmp(message, "libudev", 8) != 0 ||
                !memmem(message, (size_t)len, node + 5, strlen(node + 5))) continue;
            processed = 1;
            pointer_retry(); /* the real processed-device event, no sleep retry */
        }
    }
    close(events);
    wait_for_tags = 1;
    assert(processed && devices[1].input && npointers == 2 && li == context);
    assert(fcntl(devices[1].fd, F_GETFD) >= 0 && devices[0].fd == anchor_lease);
    struct libinput_device *accepted = devices[1].input;
    pointer_retry();
    assert(devices[1].input == accepted && npointers == 2 && ndevices == 2);
    collect(200);
    px = 400; py = 300;
    emit(mouse, EV_REL, REL_X, 20); syn(mouse); collect(40);
    assert(cursor_x > 400);
    ngot = 0;
    emit(mouse, EV_KEY, BTN_LEFT, 0); syn(mouse); collect(30);
    emit(mouse, EV_KEY, BTN_LEFT, 1); syn(mouse); collect(200);
    printf("hotplug held button: held=%d presses=%d events=%d\n", held, presses(BTN_LEFT, 1), ngot);
    assert(held == 1 && presses(BTN_LEFT, 1) == 1);
    assert(ioctl(owner, EVIOCREVOKE, NULL) == 0);
    close(owner);
    pointer_remove(node);
    assert(li == context && ndevices == 1 && devices[0].fd == anchor_lease && npointers == 1);
    collect(200);
    printf("hotplug removed button: held=%d releases=%d\n", held, presses(BTN_LEFT, 0));
    assert(held == 0 && presses(BTN_LEFT, 0) == 1);
    assert(ioctl(mouse, UI_DEV_DESTROY) == 0);
    close(mouse);
    collect(100);
    assert(open_fds() == idle_fds);
    puts("PASS hotplug: real pre-udev rejection retries on processed netlink; revoke/remove releases held buttons");

    for (int i = 0; i < 8; i++) {
        mouse = device("LOCachy reconnect mouse", BUS_BLUETOOTH, (const int[]){ BTN_LEFT, BTN_RIGHT, -1 },
                       (const int[]){ REL_X, REL_Y, -1 }, NULL, NULL, node, sizeof node);
        take(node);
        assert(li == context && ndevices == 2 && npointers == 2 && devices[0].fd == anchor_lease);
        int before = cursor_x;
        emit(mouse, EV_REL, REL_X, 15); syn(mouse); collect(30);
        assert(cursor_x > before);
        if (i & 1) {
            assert(ioctl(mouse, UI_DEV_DESTROY) == 0);
            close(mouse);
            collect(60); /* libinput can observe unplug before our remove packet */
        }
        pointer_remove(node);
        if (!(i & 1)) { assert(ioctl(mouse, UI_DEV_DESTROY) == 0); close(mouse); }
        collect(60);
        assert(li == context && ndevices == 1 && npointers == 1 && open_fds() == idle_fds);
    }
    pointer_remove(anchor_node);
    assert(!ndevices && !npointers && !pointer_present() && !cursor_shown);
    pointer_release();
    assert(ioctl(anchor, UI_DEV_DESTROY) == 0);
    close(anchor);
    assert(!li && epoll_fd < 0 && timer_fd < 0);
    puts("PASS hotplug: 8 real Bluetooth-bus mouse reconnects preserve the live context/anchor with no fd growth");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    hotplug();
    char mouse_node[64], pad_node[64], stick_node[64], touch_node[64], pen_node[64];
    int mouse = device("LOCachy test mouse", BUS_USB, (const int[]){ BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, -1 },
                       (const int[]){ REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES, -1 },
                       NULL, NULL, mouse_node, sizeof mouse_node);
    int pad = device("LOCachy test clickpad", BUS_I8042,
                     (const int[]){ BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, -1 },
                     NULL,
                     (const struct axis[]){ { ABS_X, 0, 4000, 42 }, { ABS_Y, 0, 2600, 42 }, { ABS_MT_SLOT, 0, 4, 0 },
                                            { ABS_MT_TRACKING_ID, 0, 65535, 0 }, { ABS_MT_POSITION_X, 0, 4000, 42 },
                                            { ABS_MT_POSITION_Y, 0, 2600, 42 }, { -1, 0, 0, 0 } },
                     (const int[]){ INPUT_PROP_POINTER, INPUT_PROP_BUTTONPAD, -1 }, pad_node, sizeof pad_node);
    int stick = device("LOCachy test TrackPoint", BUS_I8042, (const int[]){ BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, -1 },
                       (const int[]){ REL_X, REL_Y, -1 }, NULL,
                       (const int[]){ INPUT_PROP_POINTER, INPUT_PROP_POINTING_STICK, -1 }, stick_node, sizeof stick_node);
    int screen = device("LOCachy test touchscreen", BUS_I2C, (const int[]){ BTN_TOUCH, -1 }, NULL,
                        (const struct axis[]){ { ABS_X, 0, 9999, 40 }, { ABS_Y, 0, 9999, 40 }, { ABS_MT_SLOT, 0, 9, 0 },
                                               { ABS_MT_TRACKING_ID, 0, 65535, 0 }, { ABS_MT_POSITION_X, 0, 9999, 40 },
                                               { ABS_MT_POSITION_Y, 0, 9999, 40 }, { -1, 0, 0, 0 } },
                        (const int[]){ INPUT_PROP_DIRECT, -1 }, touch_node, sizeof touch_node);
    int pen = device("LOCachy test pen", BUS_I2C,
                     (const int[]){ BTN_TOOL_PEN, BTN_TOUCH, BTN_STYLUS, BTN_STYLUS2, -1 }, NULL,
                     (const struct axis[]){ { ABS_X, 0, 20000, 80 }, { ABS_Y, 0, 12500, 80 },
                                            { ABS_PRESSURE, 0, 4095, 0 }, { -1, 0, 0, 0 } },
                     (const int[]){ INPUT_PROP_DIRECT, -1 }, pen_node, sizeof pen_node);

    /* What process 1 lends: all four, by path. */
    size_t nlent;
    char paths[64][POINTER_PATH];
    int *lent = pointer_lend(&nlent, paths, 64);
    int found = 0;
    for (size_t i = 0; i < nlent; i++) {
        found += !strcmp(paths[i], mouse_node) + !strcmp(paths[i], pad_node) + !strcmp(paths[i], stick_node) +
                 !strcmp(paths[i], touch_node) + !strcmp(paths[i], pen_node);
    }
    assert(lent && found == 5);
    pointer_take_back(lent, nlent);
    puts("PASS process 1 lends the mouse, clickpad, TrackPoint, touchscreen and pen with their paths");

    int slow = travel("-1", mouse_node, mouse), normal = travel("0", mouse_node, mouse);
    int fast = travel("1", mouse_node, mouse);
    printf("mouse travel at -1/0/1: %d %d %d pixels\n", slow, normal, fast);
    assert(0 < slow && slow < normal && normal < fast);
    puts("PASS mouse motion moves the cursor and follows rc.conf pointer_accel as the shared display does");

    take(mouse_node);
    assert(pointer_present() && cursor_shown);
    for (int i = 0; i < 2; i++) {
        emit(mouse, EV_KEY, BTN_LEFT, 1); syn(mouse); collect(30);
        emit(mouse, EV_KEY, BTN_LEFT, 0); syn(mouse); collect(30);
    }
    int k = 0, clicks[2] = { 0, 0 };
    for (int i = 0; i < ngot; i++)
        if (got[i].button == BTN_LEFT && got[i].pressed && k < 2) clicks[k++] = got[i].clicks;
    assert(k == 2 && clicks[0] == 1 && clicks[1] == 2 && presses(BTN_LEFT, 0) == 2);
    ngot = 0;
    emit(mouse, EV_REL, REL_WHEEL, -1); emit(mouse, EV_REL, REL_WHEEL_HI_RES, -120); syn(mouse);
    collect(30);
    emit(mouse, EV_REL, REL_HWHEEL, 1); emit(mouse, EV_REL, REL_HWHEEL_HI_RES, 120); syn(mouse);
    collect(30);
    assert(wheel() == -1 && ngot == 2 && got[1].hwheel == 1);
    ioctl(mouse, UI_DEV_DESTROY);
    close(mouse);
    collect(200);
    assert(!pointer_present() && !cursor_shown);
    pointer_release();
    puts("PASS mouse buttons with a double click, wheel steps both ways, unplugging hides the cursor");

    take(pad_node);
    touch(pad, 0, 2000, 1300, 1);
    emit(pad, EV_KEY, BTN_TOUCH, 1); emit(pad, EV_KEY, BTN_TOOL_FINGER, 1);
    emit(pad, EV_ABS, ABS_X, 2000); emit(pad, EV_ABS, ABS_Y, 1300); syn(pad);
    collect(40);
    lift(pad, 0);
    emit(pad, EV_KEY, BTN_TOUCH, 0); emit(pad, EV_KEY, BTN_TOOL_FINGER, 0); syn(pad);
    collect(600);
    assert(presses(BTN_LEFT, 1) == 1 && presses(BTN_LEFT, 0) == 1);
    ngot = 0;
    touch(pad, 0, 1700, 900, 1);
    touch(pad, 1, 2300, 900, 1);
    emit(pad, EV_KEY, BTN_TOUCH, 1); emit(pad, EV_KEY, BTN_TOOL_DOUBLETAP, 1);
    emit(pad, EV_ABS, ABS_X, 1700); emit(pad, EV_ABS, ABS_Y, 900); syn(pad);
    collect(10);
    for (int i = 1; i <= 20; i++) {
        touch(pad, 0, 1700, 900 + 40 * i, 0);
        touch(pad, 1, 2300, 900 + 40 * i, 0);
        emit(pad, EV_ABS, ABS_Y, 900 + 40 * i); syn(pad);
        collect(10);
    }
    lift(pad, 0);
    lift(pad, 1);
    emit(pad, EV_KEY, BTN_TOUCH, 0); emit(pad, EV_KEY, BTN_TOOL_DOUBLETAP, 0); syn(pad);
    collect(300);
    printf("two-finger scroll down: %d wheel steps\n", wheel());
    assert(wheel() < 0 && !buttons());
    pointer_release();
    puts("PASS clickpad: a tap clicks, two fingers scroll (down is down), no clicks while scrolling");

    take(stick_node);
    emit(stick, EV_KEY, BTN_MIDDLE, 1); syn(stick); collect(50);
    emit(stick, EV_KEY, BTN_MIDDLE, 0); syn(stick); collect(300);
    assert(presses(BTN_MIDDLE, 1) == 1 && presses(BTN_MIDDLE, 0) == 1);
    ngot = 0;
    px = 500;
    py = 400;
    emit(stick, EV_KEY, BTN_MIDDLE, 1); syn(stick); collect(250);
    for (int i = 0; i < 40; i++) {
        emit(stick, EV_REL, REL_Y, 5); syn(stick);
        collect(10);
    }
    emit(stick, EV_KEY, BTN_MIDDLE, 0); syn(stick); collect(300);
    printf("TrackPoint middle-button scroll down: %d wheel steps\n", wheel());
    assert(wheel() < 0 && !buttons() && (int)py == 400);
    pointer_release();
    puts("PASS TrackPoint: a middle click alone, middle held with motion scrolls and does not move or click");

    /* The touchscreen: 250 mm wide over a picture 1000 pixels wide, 4 px/mm. */
    take(touch_node);
    touch(screen, 0, 2500, 5000, 1);
    emit(screen, EV_KEY, BTN_TOUCH, 1); emit(screen, EV_ABS, ABS_X, 2500); emit(screen, EV_ABS, ABS_Y, 5000);
    syn(screen);
    collect(50);
    assert(!buttons() && !cursor_shown); /* nothing until it is lifted; no cursor for fingers */
    lift(screen, 0);
    emit(screen, EV_KEY, BTN_TOUCH, 0); syn(screen);
    collect(50);
    assert(ngot == 2 && got[0].button == BTN_LEFT && got[0].pressed && got[0].clicks == 1 && !got[1].pressed);
    assert(abs(got[0].x - 250) <= 1 && abs(got[0].y - 400) <= 1);
    ngot = 0;
    touch(screen, 0, 5000, 5000, 1);
    emit(screen, EV_KEY, BTN_TOUCH, 1); syn(screen);
    collect(700); /* the hold timer, through the epoll fd */
    assert(ngot == 2 && got[0].button == BTN_RIGHT && got[0].pressed && abs(got[0].x - 500) <= 1);
    lift(screen, 0);
    emit(screen, EV_KEY, BTN_TOUCH, 0); syn(screen);
    collect(50);
    assert(ngot == 2);
    ngot = 0;
    touch(screen, 0, 5000, 2000, 1);
    emit(screen, EV_KEY, BTN_TOUCH, 1); syn(screen);
    collect(20);
    for (int y = 2100; y <= 4000; y += 100) {
        touch(screen, 0, 5000, y, 0);
        syn(screen);
        collect(10);
    }
    lift(screen, 0);
    emit(screen, EV_KEY, BTN_TOUCH, 0); syn(screen);
    collect(600);
    printf("touchscreen drag down 190 px: %d wheel steps\n", wheel());
    assert(wheel() == 8 && !buttons()); /* rows 8 to 16: the content follows the finger */
    ngot = 0;
    touch(screen, 0, 4000, 5000, 1);
    touch(screen, 1, 6000, 5000, 1);
    emit(screen, EV_KEY, BTN_TOUCH, 1); syn(screen);
    collect(20);
    for (int x = 6100; x <= 7000; x += 100) {
        touch(screen, 1, x, 5000, 0);
        syn(screen);
        collect(10);
    }
    lift(screen, 0);
    lift(screen, 1);
    emit(screen, EV_KEY, BTN_TOUCH, 0); syn(screen);
    collect(600);
    double zoom = 1;
    for (int i = 0; i < ngot; i++) if (got[i].gesture == GESTURE_PINCH) zoom *= got[i].scale;
    printf("touchscreen pinch 50 to 75 mm: zoom %.3f\n", zoom);
    assert(fabs(zoom - 1.5) < 0.05 && !buttons());
    pointer_release();
    puts("PASS touchscreen: a tap clicks, holding is a right click, dragging scrolls, two fingers pinch");

    /* The clickpad: three fingers swipe, two pinch. */
    take(pad_node);
    for (int dir = 0; dir < 2; dir++) {
        int x[3] = { 1500, 2000, 2500 }, y = 1500;
        for (int f = 0; f < 3; f++) touch(pad, f, x[f], y, 1);
        emit(pad, EV_KEY, BTN_TOUCH, 1); emit(pad, EV_KEY, BTN_TOOL_TRIPLETAP, 1); syn(pad);
        collect(20);
        for (int i = 1; i <= 20; i++) {
            for (int f = 0; f < 3; f++) touch(pad, f, dir == 0 ? x[f] - 50 * i : x[f], dir == 0 ? y : y - 40 * i, 0);
            syn(pad);
            collect(10);
        }
        for (int f = 0; f < 3; f++) lift(pad, f);
        emit(pad, EV_KEY, BTN_TOUCH, 0); emit(pad, EV_KEY, BTN_TOOL_TRIPLETAP, 0); syn(pad);
        collect(300);
    }
    printf("three-finger swipes asked for: %s, %s\n", nasked > 0 ? asked[0] : "-", nasked > 1 ? asked[1] : "-");
    assert(nasked == 2 && !strcmp(asked[0], "switch next") && !strcmp(asked[1], "home"));
    ngot = 0;
    touch(pad, 0, 1800, 1300, 1);
    touch(pad, 1, 2200, 1300, 1);
    emit(pad, EV_KEY, BTN_TOUCH, 1); emit(pad, EV_KEY, BTN_TOOL_DOUBLETAP, 1); syn(pad);
    collect(20);
    for (int i = 1; i <= 20; i++) {
        touch(pad, 0, 1800 - 40 * i, 1300, 0);
        touch(pad, 1, 2200 + 40 * i, 1300, 0);
        syn(pad);
        collect(10);
    }
    lift(pad, 0);
    lift(pad, 1);
    emit(pad, EV_KEY, BTN_TOUCH, 0); emit(pad, EV_KEY, BTN_TOOL_DOUBLETAP, 0); syn(pad);
    collect(300);
    zoom = 1;
    for (int i = 0; i < ngot; i++) if (got[i].gesture == GESTURE_PINCH) zoom *= got[i].scale;
    printf("clickpad pinch apart: zoom %.3f\n", zoom);
    assert(zoom > 1.3 && !buttons());
    pointer_release();
    puts("PASS clickpad: three fingers swipe to the next task and home, two fingers pinch");

    /* The screen's pen: hovering points, the tip is the left button, the
     * barrel button the right. */
    take(pen_node);
    emit(pen, EV_KEY, BTN_TOOL_PEN, 1); emit(pen, EV_ABS, ABS_X, 10000); emit(pen, EV_ABS, ABS_Y, 5000); syn(pen);
    hover(pen, 50);
    assert(cursor_shown && abs(cursor_x - 500) <= 1 && abs(cursor_y - 320) <= 1 && !buttons());
    emit(pen, EV_KEY, BTN_TOUCH, 1); emit(pen, EV_ABS, ABS_PRESSURE, 1500); syn(pen);
    hover(pen, 30);
    emit(pen, EV_KEY, BTN_TOUCH, 0); emit(pen, EV_ABS, ABS_PRESSURE, 0); syn(pen);
    hover(pen, 30);
    emit(pen, EV_KEY, BTN_STYLUS, 1); syn(pen);
    hover(pen, 30);
    emit(pen, EV_KEY, BTN_STYLUS, 0); syn(pen);
    hover(pen, 30);
    emit(pen, EV_KEY, BTN_TOOL_PEN, 0); syn(pen);
    collect(50);
    assert(presses(BTN_LEFT, 1) == 1 && presses(BTN_LEFT, 0) == 1 && presses(BTN_RIGHT, 1) == 1 &&
           presses(BTN_RIGHT, 0) == 1 && !cursor_shown);
    pointer_release();
    puts("PASS pen: hovering points and shows the cursor, the tip clicks, the barrel button right-clicks");

    /* A round touchpad libinput does not know (60 mm across). Without a
     * quirk a finger round its edge points; with settings' section on
     * (libinput reads it as the program next takes the pointer) clockwise
     * scrolls down and counterclockwise up, tap to click on, and its middle
     * still points. */
    char wheelpad_node[64];
    int wheelpad = device("LOCachy test round touchpad", BUS_I2C,
                          (const int[]){ BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, -1 }, NULL,
                          (const struct axis[]){ { ABS_X, 0, 1200, 20 }, { ABS_Y, 0, 1200, 20 }, { ABS_MT_SLOT, 0, 1, 0 },
                                                 { ABS_MT_TRACKING_ID, 0, 65535, 0 }, { ABS_MT_POSITION_X, 0, 1200, 20 },
                                                 { ABS_MT_POSITION_Y, 0, 1200, 20 }, { -1, 0, 0, 0 } },
                          (const int[]){ INPUT_PROP_POINTER, INPUT_PROP_BUTTONPAD, -1 }, wheelpad_node,
                          sizeof wheelpad_node);
    take(wheelpad_node);
    int plain = circle(wheelpad, 600, 500, 1);
    printf("round touchpad without the quirk, once round clockwise: %d wheel steps\n", plain);
    assert(!(scroll_methods() & LIBINPUT_CONFIG_SCROLL_CIRCULAR) && plain == 0 && ngot > 0 && !buttons());
    pointer_release();
    local_quirks(CIRCULAR_QUIRK("1"));
    take(wheelpad_node);
    assert((scroll_methods() & (LIBINPUT_CONFIG_SCROLL_CIRCULAR | LIBINPUT_CONFIG_SCROLL_2FG)) ==
           (LIBINPUT_CONFIG_SCROLL_CIRCULAR | LIBINPUT_CONFIG_SCROLL_2FG));
    int down = circle(wheelpad, 600, 500, 1), up = circle(wheelpad, 600, 500, 0);
    printf("round touchpad with settings' section on, once round clockwise: %d, counterclockwise: %d wheel steps\n",
           down, up);
    assert(down < 0 && up > 0 && !buttons()); /* down is negative, as the mouse wheel's */
    ngot = 0;
    px = 500;
    py = 400;
    touch(wheelpad, 0, 560, 600, 1);
    emit(wheelpad, EV_KEY, BTN_TOUCH, 1); emit(wheelpad, EV_KEY, BTN_TOOL_FINGER, 1); syn(wheelpad);
    collect(20);
    for (int x = 570; x <= 660; x += 10) {
        touch(wheelpad, 0, x, 600, 0);
        emit(wheelpad, EV_ABS, ABS_X, x); syn(wheelpad);
        collect(10);
    }
    lift(wheelpad, 0);
    emit(wheelpad, EV_KEY, BTN_TOUCH, 0); emit(wheelpad, EV_KEY, BTN_TOOL_FINGER, 0); syn(wheelpad);
    collect(300);
    printf("round touchpad's middle, 5 mm right: cursor at %d\n", cursor_x);
    assert(cursor_x > 500 && !wheel() && !buttons());
    pointer_release();
    local_quirks(NULL);
    ioctl(wheelpad, UI_DEV_DESTROY);
    close(wheelpad);
    puts("PASS round touchpad: settings' section on gives it circular scrolling with tap to click on; "
         "clockwise down, counterclockwise up, the middle points");

    /* Panasonic's CF-FV1 (libinput's model of it: Synaptics 06CB:CE53 on I2C,
     * 64 mm across, two buttons, no clickpad); the VM's DMI says CFFV1.
     * libinput's own quirk makes it round: circular scrolling with nothing
     * written. Settings' section off gives it two-finger scrolling back. */
    char fv1_node[64];
    int fv1 = device_ids("SYNA0103:00 06CB:CE53 Touchpad", BUS_I2C, 0x06cb, 0xce53,
                         (const int[]){ BTN_LEFT, BTN_RIGHT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP,
                                        BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP, -1 }, NULL,
                         (const struct axis[]){ { ABS_X, 0, 771, 12 }, { ABS_Y, 0, 771, 12 }, { ABS_MT_SLOT, 0, 4, 0 },
                                                { ABS_MT_POSITION_X, 0, 771, 12 }, { ABS_MT_POSITION_Y, 0, 771, 12 },
                                                { ABS_MT_TOOL_TYPE, 0, 2, 0 }, { ABS_MT_TRACKING_ID, 0, 65535, 0 },
                                                { -1, 0, 0, 0 } },
                         (const int[]){ INPUT_PROP_POINTER, -1 }, fv1_node, sizeof fv1_node);
    take(fv1_node);
    assert((scroll_methods() & (LIBINPUT_CONFIG_SCROLL_CIRCULAR | LIBINPUT_CONFIG_SCROLL_2FG)) ==
           (LIBINPUT_CONFIG_SCROLL_CIRCULAR | LIBINPUT_CONFIG_SCROLL_2FG));
    down = circle(fv1, 385, 330, 1);
    up = circle(fv1, 385, 330, 0);
    printf("CF-FV1 as libinput knows it, once round clockwise: %d, counterclockwise: %d wheel steps\n", down, up);
    assert(down < 0 && up > 0 && !buttons());
    int vertical_down = two_finger_scroll(fv1, 0, 1), vertical_up = two_finger_scroll(fv1, 0, -1);
    int horizontal_right = two_finger_scroll(fv1, 1, 1), horizontal_left = two_finger_scroll(fv1, 1, -1);
    printf("CF-FV1 circular mode two-finger: vertical %d/%d horizontal %d/%d\n",
           vertical_down, vertical_up, horizontal_right, horizontal_left);
    assert(vertical_down < 0 && vertical_up > 0 && horizontal_right > 0 && horizontal_left < 0);
    pointer_release();
    local_quirks(CIRCULAR_QUIRK("0"));
    take(fv1_node);
    assert(!(scroll_methods() & LIBINPUT_CONFIG_SCROLL_CIRCULAR) && (scroll_methods() & LIBINPUT_CONFIG_SCROLL_2FG));
    plain = circle(fv1, 385, 330, 1);
    ngot = 0;
    touch(fv1, 0, 300, 250, 1);
    touch(fv1, 1, 470, 250, 1);
    emit(fv1, EV_KEY, BTN_TOUCH, 1); emit(fv1, EV_KEY, BTN_TOOL_DOUBLETAP, 1);
    emit(fv1, EV_ABS, ABS_X, 300); emit(fv1, EV_ABS, ABS_Y, 250); syn(fv1);
    collect(10);
    for (int i = 1; i <= 15; i++) {
        touch(fv1, 0, 300, 250 + 20 * i, 0);
        touch(fv1, 1, 470, 250 + 20 * i, 0);
        emit(fv1, EV_ABS, ABS_Y, 250 + 20 * i); syn(fv1);
        collect(10);
    }
    lift(fv1, 0);
    lift(fv1, 1);
    emit(fv1, EV_KEY, BTN_TOUCH, 0); emit(fv1, EV_KEY, BTN_TOOL_DOUBLETAP, 0); syn(fv1);
    collect(300);
    printf("CF-FV1 with settings' section off: round clockwise %d, two fingers down %d wheel steps\n", plain, wheel());
    assert(plain == 0 && wheel() < 0 && !buttons());
    pointer_release();
    local_quirks(NULL);
    ioctl(fv1, UI_DEV_DESTROY);
    close(fv1);
    puts("PASS CF-FV1: round as libinput knows it (circular scrolling, tap to click on); settings' section off "
         "gives two-finger scrolling");

    close(pad); close(stick); close(screen); close(pen);
    puts("PASS pointer-libinput: real libinput and udevd through base/pointer.c");
    return 0;
}
