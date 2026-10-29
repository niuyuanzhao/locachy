/* Input hotplug without physical inputs, uinput, DRM, PID 1, or privileges.
 * Pipes stand in for evdev queues; only discovery/capability/liveness ioctls
 * are doubled. Init and the frontend use the real lease control channel.
 * Build with -ffunction-sections -fdata-sections -Wl,--gc-sections -lm. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int input_open(const char *, int, ...);
static int input_ioctl(int, unsigned long, ...);
#define open input_open
#define ioctl input_ioctl
#define main init_main
#include "../init/init.c"
#undef main
#include "../base/keys.c"
#include "../base/pointer.c"
#include "../base/control.c"
#undef open
#undef ioctl

#define NINPUT 4
/* button: a device with only that key (ThinkPad Extra Buttons' ThinkVantage) */
static struct fixture { int read, write, present, mouse, button; } input[NINPUT];
static int owner[1024]; /* fd -> fixture index + 1 */

static int input_open(const char *path, int flags, ...)
{
    int i, end = 0;
    if (sscanf(path, "/dev/input/event%d%n", &i, &end) == 1 && !path[end]) {
        if (i < 0 || i >= NINPUT || !input[i].present) { errno = ENOENT; return -1; }
        int fd = fcntl(input[i].read, F_DUPFD_CLOEXEC, 0);
        assert(fd >= 0 && fd < (int)(sizeof owner / sizeof *owner));
        owner[fd] = i + 1;
        return fd;
    }
    return open(path, flags);
}

static void setbit(unsigned long *bits, int code)
{ bits[code / (8 * sizeof *bits)] |= 1ul << (code % (8 * sizeof *bits)); }

static int input_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    /* No names or ids: each keyboard types with the system's key table. */
    if (request == EVIOCGID || (_IOC_TYPE(request) == 'E' && _IOC_NR(request) == _IOC_NR(EVIOCGNAME(0)))) {
        errno = EINVAL;
        return -1;
    }
    assert(fd >= 0 && fd < (int)(sizeof owner / sizeof *owner) && owner[fd]);
    struct fixture *f = &input[owner[fd] - 1];
    if (request == EVIOCREVOKE) return 0;
    if (!f->present || !input_same(fd, f->read)) { errno = ENODEV; return -1; }
    if (request == EVIOCGVERSION) { *(int *)arg = EV_VERSION; return 0; }
    if (_IOC_TYPE(request) == 'E' && _IOC_NR(request) >= 0x20 && _IOC_NR(request) <= 0x20 + EV_MAX) {
        memset(arg, 0, _IOC_SIZE(request));
        int type = (int)_IOC_NR(request) - 0x20;
        if (type == EV_KEY) {
            if (f->button) setbit(arg, f->button);
            else if (f->mouse) setbit(arg, BTN_LEFT);
            else { setbit(arg, KEY_A); setbit(arg, KEY_ENTER); }
        } else if (type == EV_REL && f->mouse) { setbit(arg, REL_X); setbit(arg, REL_Y); }
        return (int)_IOC_SIZE(request);
    }
    if (_IOC_TYPE(request) == 'E' && _IOC_NR(request) == 0x18) {
        memset(arg, 0, _IOC_SIZE(request));
        return (int)_IOC_SIZE(request);
    }
    errno = EINVAL;
    return -1;
}

void say(const char *fmt, ...) { (void)fmt; }
static const char *rc[3]; /* rc.conf's NAME, VALUE; none: the defaults */
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{ for (int i = 0; rc[i]; i += 2) fn(rc[i], rc[i + 1], arg); return 0; }
void screen_pixels(int *w, int *h) { *w = 1000; *h = 800; }
void screen_cell_at(int x, int y, int *col, int *row) { *col = x / 10; *row = y / 20; }
void screen_cursor_move(int x, int y) { (void)x; (void)y; }
void screen_cursor_visible(int on) { (void)on; }
int screen_take(int fd) { close(fd); return 1; }
void screen_release(void) {}
int base_audio_key(int action, char *toast, size_t size)
{ (void)action; (void)toast; (void)size; return 0; }
int base_brightness_key(int action, char *toast, size_t size)
{ (void)action; (void)toast; (void)size; return 0; }

static void add(int i, int mouse)
{
    int pair[2];
    assert(pipe2(pair, O_NONBLOCK | O_CLOEXEC) == 0);
    input[i] = (struct fixture){ pair[0], pair[1], 1, mouse, 0 };
}

static void emit(int i, int code, int value)
{
    struct input_event ev = { .type = EV_KEY, .code = (unsigned short)code, .value = value };
    assert(write(input[i].write, &ev, sizeof ev) == sizeof ev);
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

struct expected { int keys, pointers, key, mods, shown; };
static void frontend(int control, int sync)
{
    keys_release(); /* inherited process-1 fds are not frontend leases */
    for (size_t i = 0; i < nafd; i++) close(afd[i]);
    for (int i = 0; i < NINPUT; i++) {
        if (input[i].present) { close(input[i].read); close(input[i].write); }
    }
    ctl = control;
    int empty_fds = -1;
    for (;;) {
        struct expected e;
        assert(recv(sync, &e, sizeof e, 0) == sizeof e);
        if (e.keys < 0) break;
        struct pollfd p = { .fd = control, .events = POLLIN };
        while (poll(&p, 1, 0) == 1) assert(control_service(NULL, 0, NULL) == 1);
        assert(count == e.keys && (int)ndevices == e.pointers);
        assert(control_shown() == e.shown);
        assert(key_poll_timeout(-1, 0) == e.key);
        assert(keys_modifiers() == e.mods);
        for (int i = 0; i < count; i++) assert(fcntl(keyboards[i], F_GETFD) >= 0);
        for (size_t i = 0; i < ndevices; i++) assert(fcntl(devices[i].fd, F_GETFD) >= 0);
        if (!count && !ndevices) {
            if (empty_fds < 0) empty_fds = open_fds();
            assert(open_fds() == empty_fds);
        }
        assert(send(sync, "ok", 2, 0) == 2);
    }
    keys_release();
    pointer_release();
    _exit(0);
}

static void check(int sync, int keys, int pointers, int key, int mods, int shown)
{
    struct expected e = { keys, pointers, key, mods, shown };
    assert(send(sync, &e, sizeof e, 0) == sizeof e);
    struct pollfd p = { .fd = sync, .events = POLLIN };
    char reply[8];
    assert(poll(&p, 1, 2000) == 1 && recv(sync, reply, sizeof reply, 0) == 2);
}

int main(void)
{
    ctl = -1; /* process 1 owns discovery, not the frontend control endpoint */
    add(0, 0);
    assert(keys_open() == 1);
    add(1, 0); /* a Bluetooth keyboard connects after home/lock started */
    input_refresh(); /* the module event path, originally activity_open only */
    int fds[MAX_KBD];
    int n = keys_fds(fds, MAX_KBD);
    printf("keyboards after module event: %d (expected 2); activity devices: %zu\n", n, nafd);
    assert(n == 2 && nafd == 2);
    puts("PASS the baseline regression: module events refresh process-1 keyboard discovery");

    int pair[2], sync[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sync) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) { close(pair[0]); close(sync[0]); frontend(pair[1], sync[1]); }
    close(pair[1]); close(sync[1]);
    home.ctl = pair[0]; home.pid = 123; home.lessee = 42;
    home.screens = 1; /* home on the screen, the keyboard and the pointer its */
    int take[1 + MAX_KBD];
    take[0] = open("/dev/null", O_RDONLY);
    home.nkbd = keys_lend(home.kbd, MAX_KBD);
    memcpy(take + 1, home.kbd, (size_t)home.nkbd * sizeof *take);
    assert(control_send(home.ctl, "take", 4, take, home.nkbd + 1) == 0);
    close(take[0]);
    home.pointer_wanted = 1;
    pointer_refresh(&home, POINTER_MOVING | POINTER_DIRECT);
    check(sync[0], 2, 2, 0, 0, 1);

    int original_key = fds[0], original_lease = home.kbd[0], original_pointer = home.pointer[0];
    emit(0, KEY_A, 1); /* queued before the uevent: must not be reopened/drained */
    input_refresh();
    assert(keys_fds(fds, MAX_KBD) == 2);
    assert(fds[0] == original_key && home.kbd[0] == original_lease && home.pointer[0] == original_pointer);
    check(sync[0], 2, 2, 'a', 0, 0);
    add(2, 1);
    emit(0, KEY_LEFTSHIFT, 1);
    input_refresh();
    check(sync[0], 2, 3, 0, KEYS_SHIFT, 0);
    emit(0, KEY_A, 1);
    check(sync[0], 2, 3, 'A', KEYS_SHIFT, 0);
    puts("PASS unrelated events and pointer additions retain live queues/modifiers without K_SHOWN");

    emit(1, KEY_LEFTCTRL, 1);
    check(sync[0], 2, 3, 0, KEYS_SHIFT | KEYS_CTRL, 0);
    input[1].present = 0;
    input_refresh();
    assert(count == 1 && home.nkbd == 1 && home.npointer == 2);
    check(sync[0], 1, 2, 0, KEYS_SHIFT, 0);
    emit(0, KEY_LEFTSHIFT, 0);
    check(sync[0], 1, 2, 0, 0, 0);
    puts("PASS disconnect closes/revokes only removed leases and clears that device's modifiers");

    close(input[1].read); close(input[1].write);
    add(1, 0); /* same event path, new inode/queue */
    input_refresh();
    emit(1, KEY_A, 1);
    check(sync[0], 2, 3, 'a', 0, 0);
    puts("PASS reconnect at the same event path replaces stale descriptors in the running frontend");
    close(input[1].read); close(input[1].write);
    add(1, 0); /* remove and re-add can be drained together in one uevent batch */
    input_refresh();
    emit(1, KEY_A, 1);
    check(sync[0], 2, 3, 'a', 0, 0);
    emit(0, KEY_LEFTSHIFT, 1);
    emit(0, KEY_RIGHTSHIFT, 1);
    check(sync[0], 2, 3, 0, KEYS_SHIFT, 0);
    emit(0, KEY_LEFTSHIFT, 0);
    check(sync[0], 2, 3, 0, KEYS_SHIFT, 0);
    emit(1, KEY_LEFTSHIFT, 1);
    check(sync[0], 2, 3, 0, KEYS_SHIFT, 0);
    emit(0, KEY_RIGHTSHIFT, 0);
    emit(1, KEY_A, 1);
    check(sync[0], 2, 3, 'A', KEYS_SHIFT, 0);
    emit(1, KEY_LEFTSHIFT, 0);
    check(sync[0], 2, 3, 0, 0, 0);
    puts("PASS batched same-path reconnect and independently held left/right/device modifiers");

    /* Ownership only: no lock/auth code is replaced or exercised here. */
    locker = home;
    memset(&home, 0, sizeof home);
    locked = 1;
    input[2].present = 0;
    input_refresh();
    check(sync[0], 2, 2, 0, 0, 0);
    emit(1, KEY_A, 1);
    check(sync[0], 2, 2, 'a', 0, 0);
    assert(focused() == &locker && locked && !home.pointer && home.nkbd == 0);
    puts("PASS an already-running lock receives input-only hotplug, not a background frame");

    /* The shared display in front: the lock, not on a screen, loses the
     * pointer; the compositor gets no framebuffer input leases. */
    struct frame *display = &tasks[0].f[0];
    *display = (struct frame){ .shared_display = 1, .pid = 1, .ctl = -1, .screens = 1 };
    tasks[0].depth = 1;
    display_front = &tasks[0];
    locked = 0;
    add(3, 0);
    input_refresh();
    assert(count == 3 && !display->nkbd && !display->pointer && focused() == display);
    check(sync[0], 2, 0, 0, 0, 0);
    display_front = NULL;
    tasks[0].depth = 0;
    locked = 1;
    input_refresh();
    check(sync[0], 3, 3, 0, 0, 0);
    puts("PASS the shared compositor is not sent framebuffer input leases");

    for (int i = 0; i < NINPUT; i++) input[i].present = 0;
    input_refresh();
    assert(!count && !locker.nkbd && !locker.npointer && !nafd);
    check(sync[0], 0, 0, 0, 0, 0);
    int empty_fds = open_fds();
    for (int i = 0; i < 12; i++) {
        close(input[1].read); close(input[1].write);
        add(1, 0);
        input_refresh();
        emit(1, KEY_A, 1);
        check(sync[0], 1, 1, 'a', 0, 0);
        input[1].present = 0;
        input_refresh();
        check(sync[0], 0, 0, 0, 0, 0);
        assert(open_fds() == empty_fds);
    }
    puts("PASS zero-keyboard updates and repeated reconnects leave no stale devices or fd growth");
    struct expected end = { .keys = -1 };
    assert(send(sync[0], &end, sizeof end, 0) == sizeof end);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    free(locker.pointer);
    close(pair[0]); close(sync[0]);
    for (int i = 0; i < NINPUT; i++) { close(input[i].read); close(input[i].write); }

    /* The keys that type nothing which process 1 watches on its own fds
     * (system_inputs) and goes home on, whatever has the keyboard: key_home's
     * alone, ThinkVantage unless rc.conf names others; then those, and
     * ThinkVantage nothing. MicMute alone is key_mic_mute's, not home's. */
    add(0, 0); input[0].button = KEY_PROG1;
    add(1, 0); input[1].button = KEY_MICMUTE;
    int sys[8], nsys = system_inputs(sys, 8);
    assert(nsys == 1 && input_same(sys[0], input[0].read));
    struct input_event ev;
    emit(0, KEY_PROG1, 1);
    assert(read(sys[0], &ev, sizeof ev) == sizeof ev && keys_alone_action(ev.code) == K_HOME);
    assert(keys_alone_action(KEY_VENDOR) == K_HOME && keys_alone_action(KEY_MICMUTE) != K_HOME);
    close(sys[0]);
    rc[0] = "key_home", rc[1] = "Super+Esc MicMute";
    keys_reload(); /* as at SIGHUP */
    nsys = system_inputs(sys, 8);
    assert(nsys == 1 && input_same(sys[0], input[1].read));
    emit(1, KEY_MICMUTE, 1);
    assert(read(sys[0], &ev, sizeof ev) == sizeof ev && keys_alone_action(ev.code) == K_HOME);
    assert(keys_alone_action(KEY_PROG1) == 0 && keys_alone_action(KEY_VENDOR) == 0);
    close(sys[0]);
    for (int i = 0; i < 2; i++) { close(input[i].read); close(input[i].write); }
    puts("PASS process 1 watches and goes home on key_home's keys: ThinkVantage by default, "
         "MicMute when rc.conf names it, ThinkVantage then nothing");
    puts("ALL PASSED");
    return 0;
}
