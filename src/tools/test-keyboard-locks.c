/* One Caps Lock and Num Lock across process 1, a direct program and the
 * shared display, without devices, DRM, PID 1 or privileges. Each open of a
 * fixture keyboard is a queue of its own (a pipe), as each evdev client has
 * one: a key reaches every open copy, or only the one that grabbed the
 * device (EVIOCGRAB). The LEDs are the device's (dev->led), written through
 * a writable open only. Keyboard 0 is the X230's built-in one as its patched
 * embedded controller has it: it has Caps and Num Lock LEDs (EVIOCGBIT),
 * but the controller drives them: a write does not stick, and EVIOCGLED
 * reads 0 whatever was written. The others are USB keyboards whose LEDs
 * show what was written. Process 1 is the real init.c/keys.c/control.c; the
 * direct program is a child using the real control_service and keys.c; the
 * test stands in for the shared display's seat on its control socket.
 * Build with -ffunction-sections -fdata-sections -Wl,--gc-sections -lm. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int fixture_open(const char *, int, ...);
static int fixture_ioctl(int, unsigned long, ...);
static ssize_t fixture_write(int, const void *, size_t);
static int fixture_fstat(int, struct stat *);
#define open fixture_open
#define fstat fixture_fstat
#define ioctl fixture_ioctl
#define write fixture_write
#define main init_main
#include "../init/init.c"
#undef main
#include "../base/keys.c"
#include "../base/pointer.c"
#include "../base/control.c"
#undef open
#undef ioctl
#undef write
#undef fstat

#define NDEV 3
#define NOPEN 128
/* Shared with the program (MAP_SHARED): the devices and every open of them. */
static struct fixtures {
    int present[NDEV], grab[NDEV]; /* grab: the open holding the device, -1 none */
    int caps[NDEV], num[NDEV];     /* the LEDs as they are lit */
    int firmware[NDEV];            /* its controller drives them: writes do not stick, EVIOCGLED reads 0 */
    int wrote_caps[NDEV], wrote_num[NDEV], writes[NDEV]; /* what was written last, how often */
    struct { dev_t dev; ino_t ino; int device, writable, revoked, wfd; } open[NOPEN];
    int nopen;
} *fx;
static int ime_caps = -1; /* the program's input method, as keys.c tells it */
void ime_caps_key(int pressed, int locked) { (void)pressed; (void)locked; }
void ime_set_caps(int locked) { ime_caps = locked; }

static int entry_of(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    for (int k = 0; k < fx->nopen; k++) {
        if (fx->open[k].dev == st.st_dev && fx->open[k].ino == st.st_ino) return k;
    }
    return -1;
}

/* Every open of a device is the same device file (input_same): its node. */
static int fixture_fstat(int fd, struct stat *st)
{
    int k = entry_of(fd);
    if (k < 0) return fstat(fd, st);
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR | 0600;
    st->st_ino = 1000 + (ino_t)fx->open[k].device;
    st->st_rdev = makedev(13, 64 + fx->open[k].device);
    return 0;
}

static int fixture_open(const char *path, int flags, ...)
{
    int i, end = 0;
    if (sscanf(path, "/dev/input/event%d%n", &i, &end) == 1 && !path[end]) {
        if (i < 0 || i >= NDEV || !fx->present[i]) { errno = ENOENT; return -1; }
        int q[2];
        struct stat st;
        assert(pipe2(q, O_NONBLOCK | O_CLOEXEC) == 0 && fstat(q[0], &st) == 0 && fx->nopen < NOPEN);
        fx->open[fx->nopen].dev = st.st_dev;
        fx->open[fx->nopen].ino = st.st_ino;
        fx->open[fx->nopen].device = i;
        fx->open[fx->nopen].writable = (flags & O_ACCMODE) == O_RDWR;
        fx->open[fx->nopen].revoked = 0;
        fx->open[fx->nopen++].wfd = q[1];
        return q[0];
    }
    return open(path, flags, 0600);
}

static void setbit(unsigned long *bits, int code)
{ bits[code / (8 * sizeof *bits)] |= 1ul << (code % (8 * sizeof *bits)); }

static int fixture_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    int k = entry_of(fd);
    if (k < 0) { errno = ENOTTY; return -1; }
    int i = fx->open[k].device;
    if (request == EVIOCREVOKE) {
        fx->open[k].revoked = 1;
        if (fx->grab[i] == k) fx->grab[i] = -1;
        return 0;
    }
    if (!fx->present[i] || fx->open[k].revoked) { errno = ENODEV; return -1; }
    if (request == EVIOCGVERSION) { *(int *)arg = EV_VERSION; return 0; }
    if (request == EVIOCGRAB) {
        if (arg) fx->grab[i] = k;
        else if (fx->grab[i] == k) fx->grab[i] = -1;
        return 0;
    }
    /* No names or ids: every keyboard types with the system's (US) table. */
    if (_IOC_TYPE(request) == 'E' && _IOC_NR(request) >= 0x20 && _IOC_NR(request) <= 0x20 + EV_MAX) {
        memset(arg, 0, _IOC_SIZE(request));
        int type = (int)_IOC_NR(request) - 0x20;
        if (type == EV_KEY) { setbit(arg, KEY_A); setbit(arg, KEY_ENTER); setbit(arg, KEY_CAPSLOCK); }
        if (type == EV_LED) { setbit(arg, LED_CAPSL); setbit(arg, LED_NUML); }
        return (int)_IOC_SIZE(request);
    }
    if (_IOC_TYPE(request) == 'E' && _IOC_NR(request) == _IOC_NR(EVIOCGLED(0))) {
        memset(arg, 0, _IOC_SIZE(request));
        if (fx->caps[i] && !fx->firmware[i]) setbit(arg, LED_CAPSL);
        if (fx->num[i] && !fx->firmware[i]) setbit(arg, LED_NUML);
        return (int)_IOC_SIZE(request);
    }
    if (_IOC_TYPE(request) == 'E' && _IOC_NR(request) == 0x18) { /* EVIOCGKEY: every key up */
        memset(arg, 0, _IOC_SIZE(request));
        return (int)_IOC_SIZE(request);
    }
    errno = EINVAL;
    return -1;
}

/* evdev_write: EBADF without write access; EV_LED into the device. */
static ssize_t fixture_write(int fd, const void *buf, size_t size)
{
    int k = entry_of(fd);
    if (k < 0) return write(fd, buf, size);
    if (!fx->open[k].writable) { errno = EBADF; return -1; }
    const struct input_event *ev = buf;
    int i = fx->open[k].device;
    for (size_t n = 0; n < size / sizeof *ev; n++) {
        if (ev[n].type == EV_LED && ev[n].code == LED_CAPSL) fx->wrote_caps[i] = ev[n].value != 0;
        if (ev[n].type == EV_LED && ev[n].code == LED_NUML) fx->wrote_num[i] = ev[n].value != 0;
    }
    fx->writes[i]++;
    if (!fx->firmware[i]) {
        fx->caps[i] = fx->wrote_caps[i];
        fx->num[i] = fx->wrote_num[i];
    }
    return (ssize_t)size;
}

void say(const char *fmt, ...) { (void)fmt; }
int rc_read(void (*fn)(const char *, const char *, void *), void *arg) { (void)fn; (void)arg; return 0; }
void screen_pixels(int *w, int *h) { *w = 1000; *h = 800; }
void screen_cell_at(int x, int y, int *col, int *row) { *col = x / 10; *row = y / 20; }
void screen_cursor_move(int x, int y) { (void)x; (void)y; }
void screen_cursor_visible(int on) { (void)on; }
int screen_take(int fd) { close(fd); return 1; }
void screen_release(void) {}
int base_audio_key(int action, char *toast, size_t size) { (void)action; (void)toast; (void)size; return 0; }
int base_brightness_key(int action, char *toast, size_t size) { (void)action; (void)toast; (void)size; return 0; }
/* message() reaches these; the cases here never call them. */
void apps_tick(void) { abort(); }
void apps_display_ready(void) { abort(); }
void apps_list(char *buf, size_t size) { (void)buf; (void)size; abort(); }
int apps_action(const char *action, const char *name, char *why, size_t size)
{ (void)action; (void)name; (void)why; (void)size; abort(); }
int apps_open(const char *name, const char *target, char *why, size_t size)
{ (void)name; (void)target; (void)why; (void)size; abort(); }
int apps_window_open(const char *program, const char *target, char *why, size_t size)
{ (void)program; (void)target; (void)why; (void)size; abort(); }
int screen_lend(uint32_t *lessee) { (void)lessee; abort(); }
int screen_lend_outputs(const char *const *names, int n, uint32_t *lessee) { (void)names; (void)n; (void)lessee; abort(); }
int screen_places(struct screen_output *out, int max) { (void)out; (void)max; abort(); }
int screen_drop_master(uint32_t lessee) { (void)lessee; abort(); }
int screen_take_back(uint32_t lessee) { (void)lessee; abort(); }
int screen_refresh(void) { abort(); }

/* A key on device i, to every open copy, or the grabbing one only. */
static void emit(int i, int code, int value)
{
    struct input_event ev = { .type = EV_KEY, .code = (unsigned short)code, .value = value };
    for (int k = 0; k < fx->nopen; k++) {
        if (fx->open[k].device != i || fx->open[k].revoked || (fx->grab[i] >= 0 && fx->grab[i] != k)) continue;
        if (write(fx->open[k].wfd, &ev, sizeof ev) < 0) {
        } /* EPIPE: that copy is closed */
    }
}

static void stroke(int i, int code) { emit(i, code, 1); emit(i, code, 0); }

/* Process 1's loop over its own keyboards: every queued event. */
static void process1_reads(void)
{
    int fds[MAX_KBD], n = keys_fds(fds, MAX_KBD);
    for (int i = 0; i < n; i++) {
        struct pollfd p = { .fd = fds[i], .events = POLLIN };
        while (poll(&p, 1, 0) == 1 && (p.revents & POLLIN)) keys_event(fds[i]);
    }
}

/* The direct program: messages from process 1, then what it types. */
struct reply { int keyboards, key, ime, caps, num; };
static void program(int control, int sync)
{
    keys_release(); /* process 1's copies are not its own */
    ctl = control;
    for (;;) {
        int op;
        if (recv(sync, &op, sizeof op, 0) != sizeof op || op < 0) break;
        struct pollfd p = { .fd = control, .events = POLLIN };
        while (poll(&p, 1, 0) == 1) assert(control_service(NULL, 0, NULL) == 1);
        struct reply r = { count, 0, 0, 0, 0 };
        r.key = op ? key_poll_timeout(-1, 0) : 0;
        r.ime = ime_caps;
        r.caps = caps;
        r.num = num;
        assert(send(sync, &r, sizeof r, 0) == sizeof r);
    }
    _exit(0);
}

static struct reply program_does(int sync, int op)
{
    struct reply r;
    assert(send(sync, &op, sizeof op, 0) == sizeof op);
    struct pollfd p = { .fd = sync, .events = POLLIN };
    assert(poll(&p, 1, 2000) == 1 && recv(sync, &r, sizeof r, 0) == sizeof r);
    printf("  program: %d keyboards, typed %d, Caps %d Num %d (input method %d); USB LEDs caps %d%d num %d%d\n",
           r.keyboards, r.key, r.caps, r.num, r.ime, fx->caps[1], fx->caps[2], fx->num[1], fx->num[2]);
    return r;
}

/* What the shared display's seat got: its last "keys" message with
 * keyboards, that message's locks field (-1: none; -2: no such message). */
static int seat_locks(int seat)
{
    int locks = -2;
    char msg[CONTROL_MAX];
    int fds[CONTROL_MAX_FDS], n;
    struct pollfd p = { .fd = seat, .events = POLLIN };
    while (poll(&p, 1, 0) == 1) {
        int len = control_recv(seat, msg, sizeof msg, fds, &n);
        assert(len > 0);
        for (int i = 0; i < n; i++) close(fds[i]);
        if (strcmp(msg, "keys") == 0 && n > 0) locks = len > 5 ? atoi(msg + 5) : -1;
    }
    return locks;
}

static int fails;
static void check(const char *what, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* What the frame on the other end of sock was given last with keyboards:
 * that message's locks field (-1: none; -2: no such message). */
static int given_locks(int sock)
{
    return seat_locks(sock);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    fx = mmap(NULL, sizeof *fx, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(fx != MAP_FAILED);
    memset(fx, 0, sizeof *fx);
    for (int i = 0; i < NDEV; i++) fx->grab[i] = -1;
    ctl = -1;

    /* Boot: the built-in keyboard (its controller's LEDs) first, a USB one
     * lit by its previous owner. The LEDs are output only: process 1 starts
     * with every lock off and shows that on both. */
    fx->firmware[0] = 1;
    fx->present[0] = fx->present[1] = 1;
    fx->caps[1] = fx->num[1] = 1;
    keys_leds(1);
    assert(keys_open() == 2);
    check("boot: process 1 shows its own locks (off) on both keyboards, the USB one's lit LEDs not taken",
          !fx->caps[1] && !fx->num[1] && fx->writes[0] > 0 && !fx->wrote_caps[0]);

    int pair[2], sync[2], seat[2], lock[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sync) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, seat) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, lock) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(pair[0]); close(sync[0]); close(seat[0]); close(seat[1]); close(lock[0]); close(lock[1]);
        program(pair[1], sync[1]);
    }
    close(pair[1]); close(sync[1]);
    home.ctl = pair[0]; home.pid = 123; home.screens = 1;
    locker.ctl = lock[0]; locker.pid = 124; locker.screens = 1; /* the lock screen: a direct program too */
    struct frame *display = &tasks[0].f[0];
    *display = (struct frame){ .shared_display = 1, .pid = 2, .ctl = seat[0], .screens = 1 };
    tasks[0].depth = 1;

    /* Home (a direct program) is given the keyboards. */
    keys_place();
    stroke(1, KEY_A);
    process1_reads();
    struct reply r = program_does(sync[0], 1);
    check("a direct program given the keyboards types a", r.keyboards == 2 && r.key == 'a' && !r.caps && !r.num);

    /* Caps Lock and Num Lock on the USB keyboard in the direct program:
     * process 1 sees the same keys and writes every keyboard's LEDs. */
    stroke(1, KEY_CAPSLOCK);
    stroke(1, KEY_NUMLOCK);
    stroke(1, KEY_A);
    process1_reads();
    r = program_does(sync[0], 1);
    check("Caps and Num Lock on the USB keyboard: the program types A; the USB LEDs lit, the built-in one's written",
          r.key == 'A' && r.caps && r.num && fx->caps[1] && fx->num[1] && fx->wrote_caps[0] && fx->wrote_num[0]);

    /* An input-only update: a USB keyboard plugged in, then gone. */
    fx->present[2] = 1;
    input_refresh();
    stroke(2, KEY_A);
    process1_reads();
    r = program_does(sync[0], 1);
    check("a keyboard plugged in: the program keeps Caps and Num Lock and types A; its LEDs lit",
          r.keyboards == 3 && r.key == 'A' && r.caps && r.num && fx->caps[2] && fx->num[2]);
    fx->present[2] = 0;
    input_refresh();
    stroke(0, KEY_A);
    process1_reads();
    r = program_does(sync[0], 1);
    check("that keyboard gone (an input-only update): Caps and Num Lock stay, the program types A",
          r.keyboards == 2 && r.key == 'A' && r.caps && r.num);

    /* Home and another direct program in turn (the lock screen, then home). */
    locked = 1;
    keys_place();
    check("the lock screen given the keyboards gets the locks with them (keys 3)", given_locks(lock[1]) == 3);
    r = program_does(sync[0], 0);
    assert(r.keyboards == 0);
    locked = 0;
    keys_place();
    given_locks(lock[1]);
    stroke(0, KEY_A);
    process1_reads();
    r = program_does(sync[0], 1);
    check("home given the keyboards again: Caps and Num Lock persist, it types A on the built-in keyboard",
          r.keyboards == 2 && r.key == 'A' && r.caps && r.num && r.ime == 1);

    /* The shared display in front, direct input (the bar holds nothing):
     * process 1 sees its Num Lock, and home takes the state back. */
    display_front = &tasks[0];
    keys_place();
    check("the shared display is given the keyboards with the locks (keys 3)", seat_locks(seat[1]) == 3);
    stroke(1, KEY_NUMLOCK);
    process1_reads();
    display_front = NULL;
    keys_place();
    seat_locks(seat[1]);
    stroke(1, KEY_A);
    process1_reads();
    r = program_does(sync[0], 1);
    check("Num Lock off on the desktop, then home: Caps Lock stays on (A), Num Lock off, USB LEDs to match",
          r.key == 'A' && r.caps && !r.num && fx->caps[1] && !fx->num[1]);

    /* The bar holding the keyboards (CN mode): process 1 does not see its
     * lock keys; the seat reports the locks. labwc may leave the LEDs lit
     * otherwise as it lets go. */
    display_front = &tasks[0];
    keys_place();
    seat_locks(seat[1]);
    int bar[MAX_KBD], nbar = display->nkbd;
    memcpy(bar, display->kbd, sizeof(int) * (size_t)nbar);
    for (int i = 0; i < nbar; i++) ioctl(bar[i], EVIOCGRAB, (void *)1);
    stroke(1, KEY_CAPSLOCK); /* the bar's alone */
    stroke(1, KEY_NUMLOCK);
    process1_reads();
    const char *const num_only[] = { "locks", "2", NULL };
    assert(control_send_fields(seat[1], num_only) == 0);
    message(display);
    check("Caps off and Num on in the desktop with the keyboards grabbed: the seat's report sets the USB LEDs",
          !fx->caps[1] && fx->num[1]);
    fx->caps[1] = 1; /* labwc's last write as it let go */
    for (int i = 0; i < nbar; i++) ioctl(bar[i], EVIOCGRAB, NULL);
    display_front = NULL;
    keys_place();
    seat_locks(seat[1]);
    stroke(1, KEY_A);
    process1_reads();
    r = program_does(sync[0], 1);
    check("then home: the program types a with Num Lock on; process 1 shows its state again over labwc's",
          r.key == 'a' && !r.caps && r.num && r.ime == 0 && !fx->caps[1] && fx->num[1]);

    /* Only the shared display holding the keyboards reports its locks. */
    const char *const late[] = { "locks", "1", NULL };
    assert(control_send_fields(seat[1], late) == 0);
    message(display);
    check("a report from the shared display without the keyboards changes nothing", !fx->caps[1] && fx->num[1]);

    int end = -1;
    assert(send(sync[0], &end, sizeof end, 0) == sizeof end);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    printf("%s keyboard locks: %d failed\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
