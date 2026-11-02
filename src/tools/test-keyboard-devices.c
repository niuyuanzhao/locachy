/* Keyboard discovery and layout modifiers through pipes, no devices. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int fixture_open(const char *, int, ...);
static int fixture_ioctl(int, unsigned long, ...);
static ssize_t fixture_write(int, const void *, size_t);
#define open fixture_open
#define ioctl fixture_ioctl
#define write fixture_write
#define KEYS_TABLE "/nonexistent/keymap.table"
#include "../base/keys.c"
#undef open
#undef ioctl
#undef write

static int queues[3][2], present[3], owner[1024], mode[1024];
/* Device 0's LEDs, as the input core keeps them (dev->led); the others have none. */
static int led[LED_MAX + 1], led_writes, ime_caps = -1;
static int kdown[KEY_MAX + 1]; /* device 0's keys down, as EVIOCGKEY gives them */
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    (void)fn;
    (void)arg;
    return 0;
}
void say(const char *fmt, ...) { (void)fmt; }
void ime_caps_key(int pressed, int locked) { (void)pressed; (void)locked; }
void ime_set_caps(int locked) { ime_caps = locked; }

static int fixture_open(const char *path, int flags, ...)
{
    int i, end = 0;
    if (sscanf(path, "/dev/input/event%d%n", &i, &end) != 1 || path[end] || i > 2 || !present[i]) {
        errno = ENOENT;
        return -1;
    }
    int fd = fcntl(queues[i][0], F_DUPFD_CLOEXEC, 0);
    assert(fd >= 0 && fd < 1024);
    owner[fd] = i + 1;
    mode[fd] = flags & O_ACCMODE;
    return fd;
}

/* evdev_write: EBADF without write access; EV_LED through the input core,
 * which keeps only LEDs the device has. */
static ssize_t fixture_write(int fd, const void *buf, size_t size)
{
    assert(fd >= 0 && fd < 1024 && owner[fd] && size % sizeof(struct input_event) == 0);
    if (mode[fd] == O_RDONLY) { errno = EBADF; return -1; }
    const struct input_event *ev = buf;
    for (size_t k = 0; k < size / sizeof *ev; k++) {
        if (ev[k].type == EV_LED && owner[fd] == 1 && ev[k].code <= LED_MAX) led[ev[k].code] = ev[k].value;
    }
    led_writes++;
    return (ssize_t)size;
}

static void set_bit(unsigned long *bits, int n)
{
    bits[n / (8 * sizeof *bits)] |= 1ul << (n % (8 * sizeof *bits));
}

static int fixture_ioctl(int fd, unsigned long request, ...)
{
    assert(fd >= 0 && fd < 1024 && owner[fd]);
    int i = owner[fd] - 1;
    if (!present[i]) { errno = ENODEV; return -1; }
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == EVIOCGVERSION) { *(int *)arg = EV_VERSION; return 0; }
    if (request == EVIOCGBIT(EV_KEY, sizeof(unsigned long) * (KEY_MAX / (8 * sizeof(long)) + 1))) {
        memset(arg, 0, _IOC_SIZE(request));
        if (i == 0) { set_bit(arg, KEY_A); set_bit(arg, KEY_ENTER); }
        else if (i == 1) { set_bit(arg, KEY_BRIGHTNESSUP); set_bit(arg, KEY_BRIGHTNESSDOWN); }
        else { set_bit(arg, KEY_VOLUMEUP); set_bit(arg, KEY_MUTE); }
        return _IOC_SIZE(request);
    }
    if (request == EVIOCGBIT(EV_LED, sizeof(unsigned long) * (LED_MAX / (8 * sizeof(long)) + 1)) ||
        request == EVIOCGLED(sizeof(unsigned long) * (LED_MAX / (8 * sizeof(long)) + 1))) {
        memset(arg, 0, _IOC_SIZE(request));
        int state = request == EVIOCGLED(_IOC_SIZE(request)); /* else which LEDs it has */
        if (i == 0 && (!state || led[LED_CAPSL])) set_bit(arg, LED_CAPSL);
        if (i == 0 && (!state || led[LED_NUML])) set_bit(arg, LED_NUML);
        return _IOC_SIZE(request);
    }
    if (request == EVIOCGKEY(sizeof(unsigned long) * (KEY_MAX / (8 * sizeof(long)) + 1))) {
        memset(arg, 0, _IOC_SIZE(request));
        for (int code = 0; i == 0 && code <= KEY_MAX; code++) if (kdown[code]) set_bit(arg, code);
        return _IOC_SIZE(request);
    }
    errno = EINVAL;
    return -1;
}

static int event(int i, int type, int code, int value)
{
    struct input_event e = { .type = (unsigned short)type, .code = (unsigned short)code, .value = value };
    assert(write(queues[i][1], &e, sizeof e) == sizeof e);
    for (int n = 0; n < count; n++) {
        if (owner[keyboards[n]] == i + 1) return keys_event(keyboards[n]);
    }
    assert(0);
    return 0;
}

static int key(int i, int code, int value)
{
    return event(i, EV_KEY, code, value);
}

int main(void)
{
    for (int i = 0; i < 3; i++) {
        assert(pipe2(queues[i], O_CLOEXEC | O_NONBLOCK) == 0);
        present[i] = 1;
    }
    keys_refresh();
    assert(count == 3);
    assert(key(1, KEY_BRIGHTNESSUP, 1) == K_BRI_UP);
    assert(key(1, KEY_BRIGHTNESSDOWN, 1) == K_BRI_DOWN);
    assert(key(2, KEY_VOLUMEUP, 1) == K_VOL_UP && key(2, KEY_MUTE, 1) == K_VOL_MUTE);
    puts("PASS standalone Fn brightness and audio devices join keyboard discovery");
    assert(key(0, KEY_LEFTCTRL, 1) == 0 && key(0, KEY_RIGHTCTRL, 1) == 0);
    assert(key(0, KEY_LEFTCTRL, 0) == 0 && (keys_modifiers() & KEYS_CTRL));
    assert(key(0, KEY_C, 1) == 3);
    keys_refresh();
    assert(key(0, KEY_RIGHTCTRL, 0) == 0 && keys_modifiers() == 0);
    present[1] = 0;
    keys_refresh();
    assert(count == 2);
    present[1] = 1;
    keys_refresh();
    assert(count == 3 && key(1, KEY_BRIGHTNESSUP, 1) == K_BRI_UP);
    keys_release();
    for (int i = 0; i < 3; i++) { close(queues[i][0]); close(queues[i][1]); }
    puts("PASS Fn device reconnect and independent simultaneous modifier keys");
    for (int s = 0; s < NSTATES; s++) {
        tables[0].chars[s][KEY_CAPSLOCK] = tables[0].chars[s][KEY_LEFTCTRL] = 0;
        tables[0].syms[s][KEY_CAPSLOCK] = 0xffe3;
        tables[0].syms[s][KEY_LEFTCTRL] = 0xffe5;
    }
    tables[0].modifier_role[KEY_CAPSLOCK] = KEYS_CTRL;
    tables[0].modifier_role[KEY_LEFTCTRL] = KEYS_CAPS;
    assert(keys_modifier(KEY_CAPSLOCK) == KEYS_CTRL && keys_modifier(KEY_LEFTCTRL) == KEYS_CAPS);
    int events[2];
    assert(pipe2(events, O_CLOEXEC | O_NONBLOCK) == 0);
    struct input_event e = { .type = EV_KEY, .code = KEY_CAPSLOCK, .value = 1 };
    assert(write(events[1], &e, sizeof e) == sizeof e && keys_event(events[0]) == 0);
    assert((keys_modifiers() & KEYS_CTRL) && !caps);
    e.code = KEY_C;
    assert(write(events[1], &e, sizeof e) == sizeof e && keys_event(events[0]) == 3);
    e.code = KEY_CAPSLOCK; e.value = 0;
    assert(write(events[1], &e, sizeof e) == sizeof e && keys_event(events[0]) == 0);
    assert(keys_modifiers() == 0);
    e.code = KEY_LEFTCTRL; e.value = 1;
    assert(write(events[1], &e, sizeof e) == sizeof e && keys_event(events[0]) == 0 && caps);
    close(events[0]); close(events[1]);
    puts("PASS direct input uses the layout modifier role, not physical Ctrl/Caps positions");

    /* Caps and Num Lock on the LEDs, with the console's keyboard off. */
    tables[0].modifier_role[KEY_CAPSLOCK] = KEYS_CAPS;
    tables[0].modifier_role[KEY_LEFTCTRL] = KEYS_CTRL;
    tables[0].modifier_role[KEY_NUMLOCK] = ROLE_NUM;
    caps = num = 0;
    for (int i = 0; i < 3; i++) {
        assert(pipe2(queues[i], O_CLOEXEC | O_NONBLOCK) == 0);
        present[i] = 1;
    }
    keys_refresh();
    assert(count == 3 && mode[keyboards[0]] == O_RDONLY);
    assert(key(0, KEY_CAPSLOCK, 1) == 0 && caps && led_writes == 0 && !led[LED_CAPSL]);
    key(0, KEY_CAPSLOCK, 0);
    keys_release();
    puts("PASS a program that does not show the locks never writes a keyboard");

    led[LED_NUML] = 1; /* as another owner left them */
    keys_leds(0);
    keys_refresh();
    assert(count == 3 && mode[keyboards[0]] == O_RDWR && led_writes == 0);
    keys_leds(1);
    assert(!caps && num && ime_caps == 0 && !led[LED_CAPSL] && led[LED_NUML] && led_writes == 3);
    assert(key(0, KEY_CAPSLOCK, 1) == 0 && caps && led[LED_CAPSL] && ime_caps == 1);
    assert(key(0, KEY_CAPSLOCK, 0) == 0 && key(0, KEY_A, 1) == 'A');
    assert(key(0, KEY_NUMLOCK, 1) == 0 && !num && !led[LED_NUML] && led[LED_CAPSL]);
    puts("PASS process 1 takes the locks from the LEDs, then shows each change on them");

    keys_leds(0);
    int writes = led_writes;
    assert(key(0, KEY_CAPSLOCK, 1) == 0 && !caps && led[LED_CAPSL] && led_writes == writes);
    keys_leds(1);
    assert(caps && led[LED_CAPSL]);
    puts("PASS while the shared display shows its locks, process 1 leaves the LEDs and takes them back");

    present[0] = 0;
    keys_refresh();
    led[LED_CAPSL] = 0; /* a keyboard connected again starts as the console's (kbd_start) */
    present[0] = 1;
    keys_refresh();
    assert(count == 3 && led[LED_CAPSL]);
    puts("PASS a keyboard found again shows the locks");

    int lent[MAX_KEYBOARDS], nlent = keys_lend(lent, MAX_KEYBOARDS);
    assert(nlent == 3);
    for (int i = 0; i < nlent; i++) assert(mode[lent[i]] == O_RDONLY);
    led[LED_CAPSL] = 0;
    led[LED_NUML] = 1;
    keys_take(lent, nlent); /* as a program takes them: the same code, its own state */
    assert(!caps && num);
    writes = led_writes;
    assert(key(0, KEY_CAPSLOCK, 1) == 0 && caps && !led[LED_CAPSL] && led_writes == writes);
    keys_release();
    for (int i = 0; i < 3; i++) { close(queues[i][0]); close(queues[i][1]); }
    puts("PASS lent keyboards are read-only; a program takes the locks as the LEDs show them");

    /* A queue overflowed while Ctrl was held: its release lost. */
    for (int i = 0; i < 3; i++) assert(pipe2(queues[i], O_CLOEXEC | O_NONBLOCK) == 0);
    keys_refresh();
    caps = 0;
    assert(key(0, KEY_LEFTCTRL, 1) == 0 && (keys_modifiers() & KEYS_CTRL));
    kdown[KEY_LEFTCTRL] = 0; /* released meanwhile */
    assert(event(0, EV_SYN, SYN_DROPPED, 0) == 0);
    assert(key(0, KEY_A, 1) == 0 && key(0, KEY_LEFTCTRL, 0) == 0); /* the rest of a dropped frame */
    assert(event(0, EV_SYN, SYN_REPORT, 0) == 0 && keys_modifiers() == 0);
    assert(key(0, KEY_C, 1) == 'c');
    kdown[KEY_LEFTSHIFT] = 1; /* pressed during a loss: held, nothing typed */
    assert(event(0, EV_SYN, SYN_DROPPED, 0) == 0 && event(0, EV_SYN, SYN_REPORT, 0) == 0);
    assert(keys_modifiers() == KEYS_SHIFT && key(0, KEY_C, 1) == 'C');
    keys_release();
    for (int i = 0; i < 3; i++) { close(queues[i][0]); close(queues[i][1]); }
    puts("PASS after SYN_DROPPED the keys are read again: a lost release clears, a lost press holds, nothing typed");
    return 0;
}
