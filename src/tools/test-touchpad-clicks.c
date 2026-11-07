/* Touchpad Clicks through base/pointer.c and the real libinput, without
 * devices or root (test-touchpad-clicks.sh runs it in a user and mount
 * namespace of its own). A clickpad, a TrackPoint and a touchpad with two
 * buttons of its own (Panasonic's round one) are described as udev and sysfs
 * describe input devices, on tmpfs over /sys/devices/virtual/mem, /sys/dev/char
 * and /run/udev; /dev/null, /dev/zero and /dev/full are bound at their nodes.
 * Their events come through pipes whose evdev ioctls this program answers:
 * ioctl, fstat and fstatfs here come before the C library's (the last only
 * so that systemd's sd-device takes the tmpfs for sysfs). libinput reads its
 * quirks from the file named first, settings' section in it as settings
 * writes it. Usage: test-touchpad-clicks QUIRKS_FILE */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define CONFIG_LIBINPUT
#include "../base/pointer.c"
#include <assert.h>
#include <linux/magic.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

static int cursor_x, cursor_y;
static const char *clicks_value; /* rc.conf's touchpad_clicks; NULL: unset */
int control_fd(void) { return -1; }
int control_send(int sock, const char *msg, size_t len, const int *fds, int n)
{ (void)sock; (void)msg; (void)len; (void)fds; (void)n; abort(); }
int control_send_fields(int sock, const char *const *fields) { (void)sock; (void)fields; abort(); }
void screen_pixels(int *w, int *h) { *w = 1000; *h = 800; }
void screen_cell_at(int x, int y, int *col, int *row) { *col = x / 10; *row = y / 20; }
void screen_cursor_move(int x, int y) { cursor_x = x; cursor_y = y; }
void screen_cursor_visible(int on) { (void)on; }
double screen_scale(void) { return 1.0; }
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    fn("pointer_accel", "0", arg);
    if (clicks_value) fn("touchpad_clicks", clicks_value, arg);
    return 0;
}

#define LONGS(n) (((n) + 8 * (int)sizeof(long) - 1) / (8 * (int)sizeof(long)))
struct fake_axis { int code, min, max, res; };
struct fake {
    const char *node, *sys, *name, *udev;
    struct input_id id;
    int keys[12], rels[4], props[4];
    struct fake_axis axes[8];
    dev_t rdev, pipe_dev;
    ino_t pipe_ino;
    int rfd, wfd;
};
#define TOUCHPAD_AXES(w, h, res) { { ABS_X, 0, w, res }, { ABS_Y, 0, h, res }, { ABS_MT_SLOT, 0, 4, 0 }, \
    { ABS_MT_POSITION_X, 0, w, res }, { ABS_MT_POSITION_Y, 0, h, res }, { ABS_MT_TRACKING_ID, 0, 65535, 0 }, \
    { -1, 0, 0, 0 } }
static struct fake pad = {
    "/dev/input/event100", "input100", "SynPS/2 Synaptics TouchPad",
    "E:ID_INPUT=1\nE:ID_INPUT_TOUCHPAD=1\nE:ID_INPUT_TOUCHPAD_INTEGRATION=internal\n",
    { BUS_I8042, 0x0002, 0x0007, 0x01b1 },
    { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, -1 }, { -1 },
    { INPUT_PROP_POINTER, INPUT_PROP_BUTTONPAD, -1 }, TOUCHPAD_AXES(4000, 1890, 42), 0, 0, 0, -1, -1
};
static struct fake stick = {
    "/dev/input/event101", "input101", "TPPS/2 IBM TrackPoint",
    "E:ID_INPUT=1\nE:ID_INPUT_MOUSE=1\nE:ID_INPUT_POINTINGSTICK=1\n",
    { BUS_I8042, 0x0002, 0x000a, 0x0063 }, { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, -1 }, { REL_X, REL_Y, -1 },
    { INPUT_PROP_POINTER, INPUT_PROP_POINTING_STICK, -1 }, { { -1, 0, 0, 0 } }, 0, 0, 0, -1, -1
};
static struct fake round_pad = {
    "/dev/input/event102", "input102", "SYNA0103:00 06CB:CE53 Touchpad",
    "E:ID_INPUT=1\nE:ID_INPUT_TOUCHPAD=1\nE:ID_INPUT_TOUCHPAD_INTEGRATION=internal\n",
    { BUS_I2C, 0x06cb, 0xce53, 0x0100 },
    { BTN_LEFT, BTN_RIGHT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, -1 }, { -1 },
    { INPUT_PROP_POINTER, -1 }, TOUCHPAD_AXES(771, 771, 12), 0, 0, 0, -1, -1
};
static struct fake *const fakes[] = { &pad, &stick, &round_pad };
#define NFAKES ((int)(sizeof fakes / sizeof fakes[0]))

/* The device a descriptor (or a copy of it) reads, if it is one of these. */
static struct fake *fake_of(int fd)
{
    struct stat st;
    if (fstatat(fd, "", &st, AT_EMPTY_PATH) != 0) return NULL;
    for (int i = 0; i < NFAKES; i++) {
        if (fakes[i]->rfd >= 0 && fakes[i]->pipe_ino == st.st_ino && fakes[i]->pipe_dev == st.st_dev) return fakes[i];
    }
    return NULL;
}

static void set_bit(unsigned long *b, int n) { b[n / (8 * sizeof *b)] |= 1UL << (n % (8 * sizeof *b)); }

static int copy_out(void *arg, size_t size, const void *src, size_t len)
{
    memset(arg, 0, size);
    memcpy(arg, src, len < size ? len : size);
    return (int)(len < size ? len : size);
}

/* The evdev ioctls libevdev and libinput ask, as the kernel answers them. */
int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    struct fake *f = fake_of(fd);
    if (!f) return (int)syscall(SYS_ioctl, fd, req, arg);
    unsigned nr = _IOC_NR(req), size = _IOC_SIZE(req);
    if (_IOC_TYPE(req) != 'E') { errno = ENOTTY; return -1; }
    if (_IOC_DIR(req) == _IOC_WRITE) return 0; /* clock, grab, revoke */
    if (nr == 0x01) { *(int *)arg = EV_VERSION; return 0; }
    if (nr == 0x02) { memcpy(arg, &f->id, sizeof f->id); return 0; }
    if (nr == 0x06) return copy_out(arg, size, f->name, strlen(f->name) + 1);
    if (nr == 0x07 || nr == 0x08) { errno = ENOENT; return -1; } /* no phys, no uniq */
    if (nr == 0x09) {
        unsigned long b[LONGS(INPUT_PROP_CNT)] = { 0 };
        for (const int *p = f->props; *p >= 0; p++) set_bit(b, *p);
        return copy_out(arg, size, b, sizeof b);
    }
    if (nr == 0x0a) { /* EVIOCGMTSLOTS: the code, then a value for each slot */
        int32_t *v = arg;
        for (unsigned i = 1; i < size / 4; i++) v[i] = v[0] == ABS_MT_TRACKING_ID ? -1 : 0;
        return 0;
    }
    if (nr >= 0x18 && nr <= 0x1b) { memset(arg, 0, size); return (int)size; } /* key, LED, sound, switch state */
    if (nr >= 0x20 && nr < 0x20 + EV_CNT) {
        unsigned long b[LONGS(KEY_CNT)] = { 0 };
        int ev = (int)nr - 0x20;
        if (ev == 0) {
            set_bit(b, EV_SYN);
            if (f->keys[0] >= 0) set_bit(b, EV_KEY);
            if (f->rels[0] >= 0) set_bit(b, EV_REL);
            if (f->axes[0].code >= 0) set_bit(b, EV_ABS);
        }
        for (const int *k = f->keys; ev == EV_KEY && *k >= 0; k++) set_bit(b, *k);
        for (const int *r = f->rels; ev == EV_REL && *r >= 0; r++) set_bit(b, *r);
        for (const struct fake_axis *a = f->axes; ev == EV_ABS && a->code >= 0; a++) set_bit(b, a->code);
        return copy_out(arg, size, b, sizeof b);
    }
    if (nr >= 0x40 && nr < 0x40 + ABS_CNT) {
        struct input_absinfo info = { 0 };
        for (const struct fake_axis *a = f->axes; a->code >= 0; a++) {
            if (a->code == (int)nr - 0x40) info = (struct input_absinfo){ .minimum = a->min, .maximum = a->max, .resolution = a->res };
        }
        copy_out(arg, size, &info, sizeof info);
        return 0;
    }
    if (nr == 0x84) { *(int *)arg = 0; return 0; } /* no force feedback */
    errno = EINVAL;
    return -1;
}

static int fake_fstat(int fd, struct stat *st)
{
    int r = fstatat(fd, "", st, AT_EMPTY_PATH);
    struct fake *f = r == 0 ? fake_of(fd) : NULL;
    if (f) {
        st->st_mode = S_IFCHR | 0660;
        st->st_rdev = f->rdev;
    }
    return r;
}
int fstat(int fd, struct stat *st) { return fake_fstat(fd, st); }
int fstat64(int fd, struct stat64 *st) { return fake_fstat(fd, (struct stat *)st); }

static void sysfs_type(int fd, struct statfs *s)
{
    char link[64], path[256];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n > 0 && s->f_type == TMPFS_MAGIC) {
        path[n] = '\0';
        if (strncmp(path, "/sys/", 5) == 0) s->f_type = SYSFS_MAGIC;
    }
}
int fstatfs(int fd, struct statfs *s)
{
    int r = (int)syscall(SYS_fstatfs, fd, s);
    if (r == 0) sysfs_type(fd, s);
    return r;
}
int fstatfs64(int fd, struct statfs64 *s) { return fstatfs(fd, (struct statfs *)s); }

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

/* The device as sysfs and udev have it, and its pipe. */
static void make(struct fake *f)
{
    char dir[128], path[256], text[512];
    const char *event = f->node + strlen("/dev/input/");
    struct stat st;
    assert(stat(f->node, &st) == 0 && S_ISCHR(st.st_mode));
    f->rdev = st.st_rdev;
    unsigned long props = 0;
    for (const int *p = f->props; *p >= 0; p++) props |= 1UL << *p;
    snprintf(dir, sizeof dir, "/sys/devices/virtual/mem/%s", f->sys);
    assert(mkdir(dir, 0755) == 0);
    snprintf(path, sizeof path, "%s/uevent", dir);
    snprintf(text, sizeof text, "PRODUCT=%x/%x/%x/%x\nNAME=\"%s\"\nPROP=%lx\n", f->id.bustype, f->id.vendor,
             f->id.product, f->id.version, f->name, props);
    put(path, text);
    snprintf(path, sizeof path, "%s/subsystem", dir);
    assert(symlink("../../../../class/input", path) == 0);
    snprintf(path, sizeof path, "%s/%s", dir, event);
    assert(mkdir(path, 0755) == 0);
    snprintf(path, sizeof path, "%s/%s/uevent", dir, event);
    snprintf(text, sizeof text, "MAJOR=%u\nMINOR=%u\nDEVNAME=%s\n", major(f->rdev), minor(f->rdev), f->node + 5);
    put(path, text);
    snprintf(path, sizeof path, "%s/%s/subsystem", dir, event);
    assert(symlink("../../../../../class/input", path) == 0);
    snprintf(path, sizeof path, "/sys/dev/char/%u:%u", major(f->rdev), minor(f->rdev));
    snprintf(text, sizeof text, "../../devices/virtual/mem/%s/%s", f->sys, event);
    assert(symlink(text, path) == 0);
    snprintf(path, sizeof path, "/run/udev/data/c%u:%u", major(f->rdev), minor(f->rdev));
    snprintf(text, sizeof text, "I:1\n%sG:seat\nQ:seat\nV:1\n", f->udev);
    put(path, text);
    int p[2];
    assert(pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0);
    assert(fstatat(p[0], "", &st, AT_EMPTY_PATH) == 0);
    f->rfd = p[0];
    f->wfd = p[1];
    f->pipe_dev = st.st_dev;
    f->pipe_ino = st.st_ino;
}

static void emit(struct fake *f, int type, int code, int value)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    struct input_event e = { .input_event_sec = t.tv_sec, .input_event_usec = t.tv_nsec / 1000,
                             .type = (uint16_t)type, .code = (uint16_t)code, .value = value };
    assert(write(f->wfd, &e, sizeof e) == (ssize_t)sizeof e);
}
static void syn(struct fake *f) { emit(f, EV_SYN, SYN_REPORT, 0); }

static int tracking = 1;
static void finger(struct fake *f, int x, int y, int down)
{
    emit(f, EV_ABS, ABS_MT_SLOT, 0);
    if (down) emit(f, EV_ABS, ABS_MT_TRACKING_ID, tracking++);
    emit(f, EV_ABS, ABS_MT_POSITION_X, x);
    emit(f, EV_ABS, ABS_MT_POSITION_Y, y);
    if (down) { emit(f, EV_KEY, BTN_TOUCH, 1); emit(f, EV_KEY, BTN_TOOL_FINGER, 1); }
    emit(f, EV_ABS, ABS_X, x);
    emit(f, EV_ABS, ABS_Y, y);
    syn(f);
}
static void lift(struct fake *f)
{
    emit(f, EV_ABS, ABS_MT_SLOT, 0);
    emit(f, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(f, EV_KEY, BTN_TOUCH, 0);
    emit(f, EV_KEY, BTN_TOOL_FINGER, 0);
    syn(f);
}

/* What pointer.c made of the input, dispatching as a program on the screen
 * does (libinput's timers run meanwhile). */
static struct pointer_event got[256];
static int ngot;
static void collect(int ms)
{
    for (double end = now() + ms / 1000.0; now() < end;) {
        struct pollfd p = { .fd = pointer_fd(), .events = POLLIN };
        poll(&p, 1, 5);
        pointer_dispatch();
        while (pointer_next()) {
            assert(ngot < 256);
            got[ngot++] = *pointer_last();
        }
    }
}
static int presses(int code)
{
    int n = 0;
    for (int i = 0; i < ngot; i++) n += got[i].button == code && got[i].pressed;
    return n;
}
static int buttons(void)
{
    int n = 0;
    for (int i = 0; i < ngot; i++) n += got[i].button != 0;
    return n;
}

static const char *quirks_path;
static void local_quirks(const char *text)
{
    FILE *f = fopen(quirks_path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

/* What the program gets as it next takes the pointer: all three. */
static struct libinput_device *take_all(void)
{
    for (int i = 0; i < NFAKES; i++) {
        int fd = fcntl(fakes[i]->rfd, F_DUPFD_CLOEXEC, 0);
        assert(fd >= 0 && pointer_take(fd, fakes[i]->node) == 0);
    }
    assert(ndevices == (size_t)NFAKES);
    for (size_t i = 0; i < ndevices; i++) assert(devices[i].input);
    collect(100);
    ngot = 0;
    return devices[0].input; /* the clickpad */
}

/* A tap, a press in the middle, a press in the bottom right corner (the
 * right soft button), all on the clickpad; the TrackPoint's three buttons;
 * the round touchpad's own two, and a tap on it. */
struct result { int tap, press, press_right, stick_l, stick_r, stick_m, round_l, round_r, round_tap; };
static struct result clicks_through(void)
{
    struct result r;
    ngot = 0;
    finger(&pad, 2000, 1300, 1); collect(40);
    lift(&pad); collect(500);
    r.tap = presses(BTN_LEFT);
    ngot = 0;
    finger(&pad, 2000, 1300, 1); collect(40);
    emit(&pad, EV_KEY, BTN_LEFT, 1); syn(&pad); collect(60);
    emit(&pad, EV_KEY, BTN_LEFT, 0); syn(&pad); collect(40);
    lift(&pad); collect(300);
    r.press = presses(BTN_LEFT) * 10 + presses(BTN_RIGHT);
    ngot = 0;
    finger(&pad, 3800, 1850, 1); collect(40);
    emit(&pad, EV_KEY, BTN_LEFT, 1); syn(&pad); collect(60);
    emit(&pad, EV_KEY, BTN_LEFT, 0); syn(&pad); collect(40);
    lift(&pad); collect(300);
    r.press_right = presses(BTN_LEFT) * 10 + presses(BTN_RIGHT);
    int stick_codes[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE }, *stick_got[3] = { &r.stick_l, &r.stick_r, &r.stick_m };
    for (int i = 0; i < 3; i++) {
        ngot = 0;
        emit(&stick, EV_KEY, stick_codes[i], 1); syn(&stick); collect(50);
        emit(&stick, EV_KEY, stick_codes[i], 0); syn(&stick); collect(300);
        *stick_got[i] = presses(stick_codes[i]) == 1 && buttons() == 2;
    }
    ngot = 0;
    emit(&round_pad, EV_KEY, BTN_LEFT, 1); syn(&round_pad); collect(50);
    emit(&round_pad, EV_KEY, BTN_LEFT, 0); syn(&round_pad); collect(100);
    r.round_l = presses(BTN_LEFT) == 1 && buttons() == 2;
    ngot = 0;
    emit(&round_pad, EV_KEY, BTN_RIGHT, 1); syn(&round_pad); collect(50);
    emit(&round_pad, EV_KEY, BTN_RIGHT, 0); syn(&round_pad); collect(100);
    r.round_r = presses(BTN_RIGHT) == 1 && buttons() == 2;
    ngot = 0;
    finger(&round_pad, 385, 385, 1); collect(40);
    lift(&round_pad); collect(500);
    r.round_tap = presses(BTN_LEFT);
    return r;
}

/* How far the cursor goes for a finger moving 3 mm along the clickpad's
 * bottom strip (libinput's soft button area, 85 to 100% of its height; the
 * pad, 45 mm tall, is too small for libinput's thumb detection there). */
static int bottom_strip_travel(void)
{
    px = 500;
    py = 400;
    cursor_x = 500;
    ngot = 0;
    finger(&pad, 1800, 1700, 1); collect(30);
    for (int x = 1810; x <= 1930; x += 10) {
        finger(&pad, x, 1700, 0);
        collect(10);
    }
    lift(&pad); collect(300);
    return cursor_x - 500;
}

static void show(const char *mode, struct libinput_device *d, struct result r, int travel)
{
    printf("%s: tap %s, click method %d; clickpad tap %d, middle press %d left %d right, corner press %d left %d right; "
           "TrackPoint left/right/middle %d/%d/%d; round touchpad buttons %d/%d, tap %d; bottom strip travel %d px\n",
           mode, libinput_device_config_tap_get_enabled(d) ? "on" : "off", libinput_device_config_click_get_method(d),
           r.tap, r.press / 10, r.press % 10, r.press_right / 10, r.press_right % 10, r.stick_l, r.stick_r, r.stick_m,
           r.round_l, r.round_r, r.round_tap, travel);
}

/* settings' section for the clickpad (progs/settings.c; test-settings-monitors
 * checks the text), after a section of someone else's. */
#define THEIRS "[A tablet of theirs]\nMatchName=*Tablet*\nAttrPressureRange=10:8\n"
#define CLICKPAD_QUIRK "[LOCachy touchpad clicks: SynPS/2 Synaptics TouchPad]\n" \
    "# settings: Touchpad Clicks, pointing only (rc.conf touchpad_clicks): this clickpad's own press is no click\n" \
    "MatchUdevType=touchpad\nMatchBus=ps2\nMatchVendor=0x0002\nMatchProduct=0x0007\n" \
    "MatchName=SynPS/2 Synaptics TouchPad\nAttrEventCode=-BTN_LEFT\n"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc == 2);
    quirks_path = argv[1];
    for (int i = 0; i < NFAKES; i++) make(fakes[i]);
    struct libinput_device *d;
    struct result r;
    int travel;

    local_quirks(THEIRS);
    clicks_value = NULL;
    d = take_all();
    r = clicks_through();
    travel = bottom_strip_travel();
    show("tap and press (unset)", d, r, travel);
    assert(libinput_device_config_tap_get_enabled(d) == LIBINPUT_CONFIG_TAP_ENABLED &&
           libinput_device_config_click_get_method(d) == LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS);
    assert(r.tap == 1 && r.press == 10 && r.press_right == 1 && r.stick_l && r.stick_r && r.stick_m &&
           r.round_l && r.round_r && r.round_tap == 1 && travel == 0);
    pointer_release();
    puts("PASS tap and press: a tap and a press click, the corner is the right button; the TrackPoint's and the "
         "round touchpad's own buttons click");

    clicks_value = "press";
    d = take_all();
    r = clicks_through();
    travel = bottom_strip_travel();
    show("press only", d, r, travel);
    assert(libinput_device_config_tap_get_enabled(d) == LIBINPUT_CONFIG_TAP_DISABLED &&
           libinput_device_config_click_get_method(d) == LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS);
    assert(r.tap == 0 && r.press == 10 && r.press_right == 1 && r.stick_l && r.stick_r && r.stick_m &&
           r.round_l && r.round_r && r.round_tap == 0 && travel == 0);
    pointer_release();
    puts("PASS press only: taps click nothing on either touchpad; presses and separate buttons click as before");

    /* Pointing only as the program takes the pointer before libinput has
     * the section (the shared display until it starts again): tap off and
     * click method none; the press still clicks, the left button wherever:
     * click method none alone does not stop it. */
    clicks_value = "pointing";
    d = take_all();
    r = clicks_through();
    travel = bottom_strip_travel();
    show("pointing only, no quirk read yet", d, r, travel);
    assert(libinput_device_config_tap_get_enabled(d) == LIBINPUT_CONFIG_TAP_DISABLED &&
           libinput_device_config_click_get_method(d) == LIBINPUT_CONFIG_CLICK_METHOD_NONE);
    assert(r.tap == 0 && r.press == 10 && r.press_right == 10 && r.stick_l && r.stick_r && r.stick_m &&
           r.round_l && r.round_r && r.round_tap == 0 && travel > 0);
    pointer_release();
    puts("PASS pointing only without the quirk: click method none takes the soft buttons away, not the press");

    local_quirks(THEIRS "\n" CLICKPAD_QUIRK);
    d = take_all();
    r = clicks_through();
    travel = bottom_strip_travel();
    show("pointing only", d, r, travel);
    assert(libinput_device_config_tap_get_enabled(d) == LIBINPUT_CONFIG_TAP_DISABLED &&
           libinput_device_config_click_get_method(d) == LIBINPUT_CONFIG_CLICK_METHOD_NONE &&
           !libinput_device_pointer_has_button(d, BTN_LEFT));
    assert(r.tap == 0 && r.press == 0 && r.press_right == 0 && r.stick_l && r.stick_r && r.stick_m &&
           r.round_l && r.round_r && r.round_tap == 0 && travel > 0);
    ngot = 0;
    px = 500;
    py = 400;
    cursor_x = 500;
    finger(&pad, 1500, 1000, 1); collect(30);
    for (int x = 1520; x <= 1800; x += 20) {
        finger(&pad, x, 1000, 0);
        collect(10);
    }
    lift(&pad); collect(200);
    printf("pointing only, clickpad finger 6.7 mm right: cursor %d px right\n", cursor_x - 500);
    assert(cursor_x > 500 && !buttons());
    pointer_release();
    puts("PASS pointing only: settings' section takes the clickpad's press away (libinput's AttrEventCode); it "
         "still points, all of it; the TrackPoint's and the round touchpad's own buttons still click");

    local_quirks(THEIRS);
    clicks_value = NULL;
    d = take_all();
    r = clicks_through();
    assert(libinput_device_config_tap_get_enabled(d) == LIBINPUT_CONFIG_TAP_ENABLED &&
           libinput_device_pointer_has_button(d, BTN_LEFT) && r.tap == 1 && r.press == 10 && r.press_right == 1);
    pointer_release();
    puts("PASS back to tap and press: the next context has the press and tapping again");
    return 0;
}
