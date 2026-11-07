/* Keyboard discovery and layout modifiers through pipes, no devices. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
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
/* sysfs's input devices, keyboards.conf and the keymaps' tables: a fixture
 * directory (empty until the keyboards' list is tested). */
static char sysfs_input[256], keyboards_conf[256], keymap_tables[256];
#define KEYS_SYSFS_INPUT sysfs_input
#define KEYS_DEVICES keyboards_conf
#define KEYS_TABLES keymap_tables
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

/* A fixture file (its directories made). */
static void put(const char *root, const char *relative, const char *text)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", root, relative);
    for (char *p = path + 1; (p = strchr(p, '/')) != NULL; p++) {
        *p = '\0';
        assert(mkdir(path, 0755) == 0 || errno == EEXIST);
        *p = '/';
    }
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

/* An input device as sysfs has it: eventN/device's name, ids (hex) and key
 * bitmap (hex words, the highest first). */
static void sysfs_device(const char *root, int n, const char *name, const char *vendor, const char *product,
                         const char *keys)
{
    char file[64], text[300];
    static const char *const files[] = { "name", "id/vendor", "id/product", "capabilities/key" };
    const char *values[] = { name, vendor, product, keys };
    for (int i = 0; i < 4; i++) {
        snprintf(file, sizeof file, "event%d/device/%s", n, files[i]);
        snprintf(text, sizeof text, "%s\n", values[i]);
        put(root, file, text);
    }
}

/* The keyboards' rows of a program that holds no keyboard (settings in a
 * window on the shared display): sysfs's keyboards, each id once, with the
 * keymap each types in; nothing opened in /dev/input. */
static void test_keyboards_without_lend(void)
{
    char root[] = "/tmp/keyboard-devices-XXXXXX";
    assert(mkdtemp(root));
    snprintf(sysfs_input, sizeof sysfs_input, "%s/class/input", root);
    snprintf(keyboards_conf, sizeof keyboards_conf, "%s/keyboards.conf", root);
    snprintf(keymap_tables, sizeof keymap_tables, "%s/keymaps", root);
    /* letters and Enter (KEY_A 30, KEY_ENTER 28) in the last word */
    sysfs_device(sysfs_input, 0, "AT Translated Set 2 keyboard", "0001", "0001",
                 "402000000 3803078f800d001 feffffdfffefffff fffffffffffffffe");
    sysfs_device(sysfs_input, 1, "Power Button", "0000", "0001", "10000000000000 0");
    sysfs_device(sysfs_input, 2, "USB Keyboard", "046d", "c31c", "ffffffffffffffff fffffffffffffffe");
    sysfs_device(sysfs_input, 3, "USB Keyboard", "046d", "c31c", "1f 0 ffffffffffffffff 50000000"); /* its other interface */
    sysfs_device(sysfs_input, 5, "Mini Keyboard", "1234", "0005", "fffffffe");
    put(sysfs_input, "input7/name", "not an event device\n");
    put(root, "keyboards.conf", "# fixture\n1133:49948:USB_Keyboard us\n4660:5:Mini_Keyboard de\n");
    put(root, "keymaps/us.table", "console us\n");
    keys_release();
    for (int i = 0; i < 3; i++) present[i] = 0; /* no lent keyboard, no device opened */
    struct keys_keyboard kb[MAX_KEYBOARDS];
    int n = keys_keyboards(kb, MAX_KEYBOARDS);
    assert(count == 0);
    assert(n == 3);
    assert(!strcmp(kb[0].id, "1:1:AT_Translated_Set_2_keyboard") && !strcmp(kb[0].name, "AT Translated Set 2 keyboard"));
    assert(!kb[0].own && !kb[0].missing);
    assert(!strcmp(kb[1].id, "1133:49948:USB_Keyboard") && kb[1].own && !strcmp(kb[1].keymap, "us") && !kb[1].missing);
    assert(!strcmp(kb[2].id, "4660:5:Mini_Keyboard") && kb[2].own && !strcmp(kb[2].keymap, "de") && kb[2].missing);
    assert(keys_keyboards(kb, 1) == 1);
    /* Set from there as the direct program sets them: keyboards.conf. */
    assert(keys_set_keyboard(kb[0].id, "jp106") == 0);
    n = keys_keyboards(kb, MAX_KEYBOARDS);
    assert(n == 3 && kb[0].own && !strcmp(kb[0].keymap, "jp106") && kb[0].missing);
    assert(keys_set_keyboard(kb[0].id, NULL) == 0);
    n = keys_keyboards(kb, MAX_KEYBOARDS);
    assert(n == 3 && !kb[0].own);
    assert(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    sysfs_input[0] = keyboards_conf[0] = keymap_tables[0] = '\0';
    puts("PASS keyboards without a lent one (a window's settings): sysfs's keyboards, each once, with the keymap "
         "each types in; keyboards.conf set from there");
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

    led[LED_NUML] = 1; /* as another owner left them: never taken from there */
    caps = 0;
    keys_leds(0);
    keys_refresh();
    assert(count == 3 && mode[keyboards[0]] == O_RDWR && led_writes == 0);
    keys_leds(1);
    assert(!caps && !num && !led[LED_CAPSL] && !led[LED_NUML] && led_writes == 3);
    assert(key(0, KEY_CAPSLOCK, 1) == 0 && caps && led[LED_CAPSL] && ime_caps == 1);
    assert(key(0, KEY_CAPSLOCK, 0) == 0 && key(0, KEY_A, 1) == 'A');
    assert(key(0, KEY_NUMLOCK, 1) == 0 && num && led[LED_NUML] && led[LED_CAPSL]);
    puts("PASS process 1 shows its own locks, never the LEDs' (output only), then each change on them");

    /* The shared display's locks (its seat's "locks" report): on every
     * keyboard; shown again over another writer's. */
    keys_set_locks(KEYS_LOCK_CAPS | KEYS_LOCK_NUM);
    assert(caps && num && led[LED_CAPSL] && led[LED_NUML] && ime_caps == 1);
    led[LED_CAPSL] = 0; /* another writer cleared it */
    keys_leds(1);
    assert(caps && led[LED_CAPSL]);
    puts("PASS process 1 shows the shared display's reported locks, its own state again over another writer's");

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
    assert(caps && num);    /* never from one keyboard's LEDs */
    keys_set_locks(KEYS_LOCK_NUM); /* the locks that came with them (control.c) */
    assert(!caps && num && ime_caps == 0);
    int writes = led_writes;
    assert(key(0, KEY_CAPSLOCK, 1) == 0 && caps && !led[LED_CAPSL] && led_writes == writes);
    keys_release();
    for (int i = 0; i < 3; i++) { close(queues[i][0]); close(queues[i][1]); }
    puts("PASS lent keyboards are read-only; a program takes the locks process 1 gives with them");

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
    test_keyboards_without_lend();
    return 0;
}
