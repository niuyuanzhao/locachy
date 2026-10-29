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
#define open fixture_open
#define ioctl fixture_ioctl
#define KEYS_TABLE "/nonexistent/keymap.table"
#include "../base/keys.c"
#undef open
#undef ioctl

static int queues[3][2], present[3], owner[1024];
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    (void)fn;
    (void)arg;
    return 0;
}
void say(const char *fmt, ...) { (void)fmt; }
void ime_caps_key(int pressed, int locked) { (void)pressed; (void)locked; }
void ime_set_caps(int locked) { (void)locked; }

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
    (void)flags;
    return fd;
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
    errno = EINVAL;
    return -1;
}

static int key(int i, int code, int value)
{
    struct input_event e = { .type = EV_KEY, .code = (unsigned short)code, .value = value };
    assert(write(queues[i][1], &e, sizeof e) == sizeof e);
    for (int n = 0; n < count; n++) {
        if (owner[keyboards[n]] == i + 1) return keys_event(keyboards[n]);
    }
    assert(0);
    return 0;
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
    return 0;
}
