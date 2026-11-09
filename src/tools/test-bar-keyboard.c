/* Real bar keyboard event loop with pipe queues and XKB, no display/input. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdarg.h>
#include <unistd.h>
#include <poll.h>
static int fake_audio(int, char *, size_t);
static int fake_brightness(int, char *, size_t);
static int keyboard_ioctl(int fd, unsigned long request, ...);
#define ioctl keyboard_ioctl
#define keys_device_layout test_device_layout
#define keys_reload test_keys_reload
static int signalled; /* the last signal the bar sent its seat */
static int fake_signal(pid_t pid, int sig) { (void)pid; signalled = sig; return 0; }
#define kill fake_signal
#define base_audio_key fake_audio
#define base_brightness_key fake_brightness
static int fake_open_in_home(const char *with, const char *target); /* below */
#define open_in_home fake_open_in_home
static void fake_seat_spawn(char *const argv[]);
#define BAR_SEAT_SPAWN fake_seat_spawn
static char app_command[256];
#define BAR_APP_COMMAND app_command
#define BAR_CONTROL_SOCKET "/no-such-bar-test-control.sock"
static char launch_bin_dir[128], launch_cg_dir[128], launch_run_dir[128], launch_conf_dir[128];
static char launch_mixer_path[256], launch_bt_path[256], launch_net_path[256], launch_shell_path[256];
#define LAUNCH_BIN launch_bin_dir
#define LAUNCH_MIXER launch_mixer_path
#define LAUNCH_BLUETOOTH launch_bt_path
#define LAUNCH_NETWORK launch_net_path
#define LAUNCH_USER_SHELL launch_shell_path
#define LAUNCH_CGROUP_ROOT launch_cg_dir
#define LAUNCH_APP_RUN launch_run_dir
#define LAUNCH_CONF_DIR launch_conf_dir
#ifdef CONFIG_IME
#define ime_get_commit typed_commit /* the bar's commits, seen here */
#endif
#define main bar_main
/* The bar's sockets, relative: in the exec test's own directory. */
#define OPEN_DIR "bar-exec-open"
#define BAR_SOCKET "bar-exec.sock"
#include "../progs/bar.c"
#undef main
#ifdef CONFIG_IME
#undef ime_get_commit
#endif
#undef base_audio_key
#undef base_brightness_key
#undef open_in_home
#undef kill
#undef keys_device_layout
#undef keys_reload
#undef ioctl
#include "i18n-stub.h"
#ifndef KEYS_LOCK_CAPS /* internal.h's, for a bar from before the locks */
#define KEYS_LOCK_CAPS 1
#define KEYS_LOCK_NUM 2
#endif

static int own_us, own_jis, reload_count, grabs[2], release_during_grab;
static int keyboard_index(int fd);
static char system_table[PATH_MAX];
void keys_reload(void);
void test_keys_reload(void)
{
    reload_count++;
    keys_reload();
    if (system_table[0]) assert(keys_layout_load(system_table) == 0);
}
static int revoked_fd = -1; /* EVIOCGVERSION: ENODEV, as after EVIOCREVOKE */
static int keyboard_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == EVIOCGVERSION) {
        if (fd == revoked_fd) { errno = ENODEV; return -1; }
        *(int *)arg = EV_VERSION;
        return 0;
    }
    if (request == EVIOCGKEY(sizeof(unsigned long) * (KEY_MAX / (8 * sizeof(long)) + 1))) {
        memset(arg, 0, _IOC_SIZE(request)); /* every key up in the kernel */
        return 0;
    }
    assert(request == EVIOCGRAB);
    int i = keyboard_index(fd);
    if (i >= 0) grabs[i] = arg != NULL;
    if (arg && release_during_grab) {
        release_during_grab = 0;
        assert(raise(SIGUSR2) == 0);
    }
    return 0;
}

#ifdef CONFIG_IME
/* The last text Rime committed, as the bar typed it. */
static char committed[64];
const char *ime_get_commit(void);
const char *typed_commit(void)
{
    const char *c = ime_get_commit();
    if (c && c[0]) snprintf(committed, sizeof committed, "%s", c);
    return c;
}
#endif

/* What the bar asked home for (open_in_home's request): with and target. */
static char asked_with[128], asked_target[128];
static int asked;
static int fake_open_in_home(const char *with, const char *target)
{
    asked++;
    snprintf(asked_with, sizeof asked_with, "%.127s", with ? with : "");
    snprintf(asked_target, sizeof asked_target, "%.127s", target);
    return 0;
}

static char win_prog[64], win_target[128], opened_container[64];
static int win_calls, opened_calls, opened_own_tor;
static void fake_seat_spawn(char *const argv[])
{
    if (argv[1] && strcmp(argv[1], "--window") == 0) {
        win_calls++;
        snprintf(win_prog, sizeof win_prog, "%s", argv[2] ? argv[2] : "");
        snprintf(win_target, sizeof win_target, "%s", argv[3] ? argv[3] : "");
    } else if (argv[1] && strcmp(argv[1], "--open") == 0) {
        opened_calls++;
        opened_own_tor = argv[2] && strcmp(argv[2], "--own-tor") == 0;
        const char *c = opened_own_tor ? argv[3] : argv[2];
        snprintf(opened_container, sizeof opened_container, "%s", c ? c : "");
    }
}

static int audio_action, brightness_action;
static int fake_audio(int key, char *out, size_t size)
{
    audio_action = key;
    (void)out;
    (void)size;
    return -1;
}
static int fake_brightness(int key, char *out, size_t size)
{
    brightness_action = key;
    (void)out;
    (void)size;
    return -1;
}
void say(const char *fmt, ...) { (void)fmt; }
static const char *rc_width_cn = NULL; /* rc.conf's key_ime_width_cn, when it has one */
/* rc.conf's program_NAME settings (home's list), name and value pairs. */
static const char *const *rc_programs;
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    if (rc_width_cn) fn("key_ime_width_cn", rc_width_cn, arg);
    for (const char *const *p = rc_programs; p && *p; p += 2) fn(p[0], p[1], arg);
    return 0;
}
static double test_time = 1.0;
double uptime(void) { return test_time; }
int text_px(double scale) { (void)scale; return 18; }
const struct panel_font *panel_font(int height, int bold) { (void)height; (void)bold; return NULL; }
static int font_reloads;
int panel_fonts_reload(void) { font_reloads++; return 0; }
void panel_font_size(const struct panel_font *font, int *w, int *h) { (void)font; *w = 9; *h = 18; }
void panel_fill(uint32_t *px, int w, int h, int stride, int x, int y, int bw, int bh, uint32_t color)
{ (void)px;(void)w;(void)h;(void)stride;(void)x;(void)y;(void)bw;(void)bh;(void)color; }
void panel_text(uint32_t *px, int w, int h, int stride, int x, int y, const char *text,
                uint32_t fg, uint32_t bg, const struct panel_font *font)
{ (void)px;(void)w;(void)h;(void)stride;(void)x;(void)y;(void)text;(void)fg;(void)bg;(void)font; }
int panel_text_width(const char *text, const struct panel_font *font) { (void)font; return (int)strlen(text) * 9; }
size_t panel_text_cut(const char *text, int width, const struct panel_font *font)
{ (void)font; size_t n = (size_t)(width > 0 ? width / 9 : 0); return strlen(text) < n ? strlen(text) : n; }
void panel_text_fit(char *out, size_t size, const char *text, int width, const struct panel_font *font)
{ (void)width;(void)font;snprintf(out,size,"%.*s",(int)(size ? size-1 : 0),text); }
void panel_draw(uint32_t *px, int w, int h, int stride, const char *title, const char *sub,
                const char *ime, int reserve, int *x0, int *x1)
{ (void)px;(void)w;(void)h;(void)stride;(void)title;(void)sub;(void)ime;(void)reserve;*x0=0;*x1=0; }
int panel_hits(struct panel_hit *out, int max) { (void)out;(void)max;return 0; }
int panel_containers(const char *name, unsigned long long *ram) { (void)name;*ram=0;return 0; }
static const char *const *test_containers;
int open_containers(char names[][OPEN_NAME], int max)
{
    int n = 0;
    for (const char *const *p = test_containers; p && *p && n < max; p++) {
        snprintf(names[n++], OPEN_NAME, "%s", *p);
    }
    return n;
}
int open_window_ids(const char *name, char ids[][OPEN_WINDOW_ID], int max)
{ (void)name; (void)ids; (void)max; return 0; }
int open_candidates(const char *type, char names[][OPEN_NAME], int max) { (void)type;(void)names;(void)max;return 0; }
int open_rule(const char *type, char *name, size_t size) { (void)type;(void)name;(void)size;return OPEN_NO_RULE; }
int open_set_rule(const char *type, const char *value) { (void)type;(void)value;return 0; }
void open_label(const char *name, const char *type, char *out, size_t size)
{ (void)type;snprintf(out,size,"%s",name); }
int open_program(const char *name, const char *target, char *spec, size_t size, char *argv[], int max)
{ (void)name;(void)target;(void)spec;(void)size;(void)argv;(void)max;return 0; }
int open_shared(const char *path, char *why, size_t size) { (void)path;(void)why;(void)size;return 0; }
int open_host(const char *name) { (void)name;return 0; }
void open_user_dirs(char *home, size_t hsize, char *dirs, size_t dsize)
{ snprintf(home, hsize, "/nonexistent/home"); snprintf(dirs, dsize, "Documents"); }
/* open_in_home: the bar's calls go to fake_open_in_home (control.c is linked
 * for the seat's link, control_send/control_recv). */

static int events[2];
#ifdef CONFIG_IME
static void write_schema(const char *directory)
{
    char path[PATH_MAX];
    assert(mkdir(directory, 0700) == 0);
    snprintf(path, sizeof path, "%s/default.yaml", directory);
    FILE *f = fopen(path, "w");
    assert(f && fputs("config_version: '1'\nschema_list:\n  - schema: easy_en\n  - schema: rime_frost\n"
                      "ascii_composer:\n  good_old_caps_lock: true\n  switch_key:\n    Caps_Lock: clear\n    Shift_R: commit_text\n", f) >= 0 && fclose(f) == 0);
    static const char *const schemas[] = { "easy_en", "rime_frost" };
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s/%s.schema.yaml", directory, schemas[i]);
        f = fopen(path, "w");
        assert(f);
        fprintf(f, "schema:\n  schema_id: %s\n  name: Test\n  version: '1'\n"
                "switches:\n  - name: ascii_mode\n    reset: 0\n"
                "engine:\n  processors: [ascii_composer, recognizer, speller, punctuator, selector, express_editor]\n"
                "  segmentors: [ascii_segmentor, matcher, abc_segmentor, punct_segmentor, fallback_segmentor]\n"
                "  translators: [punct_translator, table_translator]\n"
                "punctuator:\n  half_shape:\n    ',': {commit: '\xeF\xbc\x8c'}\n"
                "speller:\n  alphabet: abcdefghijklmnopqrstuvwxyz\n  delimiter: \" '\"\n"
                "translator:\n  dictionary: test\n  enable_user_dict: false\n", schemas[i]);
        assert(fclose(f) == 0);
    }
    snprintf(path, sizeof path, "%s/test.dict.yaml", directory);
    f = fopen(path, "w");
    assert(f && fputs("---\nname: test\nversion: '1'\nsort: by_weight\n...\n\xe4\xbd\xa0\xe5\xa5\xbd\tnihao\t100\nhello\thello\t100\n", f) >= 0 && fclose(f) == 0);
}
#endif

static void integration_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    (void)data;
    if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "zwlr_layer_shell_v1")) layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    else if (!strcmp(iface, "wl_output")) {
        struct bar *b = calloc(1, sizeof *b);
        assert(b);
        b->name = name;
        b->output = wl_registry_bind(reg, name, &wl_output_interface, 1);
        b->next = bars; bars = b;
    }
    else if (!strcmp(iface, "wl_seat")) wl_seat_obj = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
    else if (!strcmp(iface, "zwp_virtual_keyboard_manager_v1"))
        vkbd_mgr = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void integration_remove(void *data, struct wl_registry *reg, uint32_t name)
{ (void)data; (void)reg; (void)name; }

static void key(int code, int value)
{
    struct input_event e = { .type = EV_KEY, .code = (unsigned short)code, .value = value };
    assert(write(events[1], &e, sizeof e) == sizeof e);
    handle_kbd_event(events[0]);
}

/* A key of the second keyboard (other: its pipe). */
static int other[2];
static int keyboard_index(int fd)
{
    return fd == events[0] ? 0 : fd == other[0] ? 1 : -1;
}
const char *test_device_layout(int fd, const char *field)
{
    if (own_jis && fd == events[0]) {
        if (!strcmp(field, "layout")) return "jp";
        if (!strcmp(field, "model")) return "jp106";
        return NULL;
    }
    if (own_us && fd == other[0]) {
        if (!strcmp(field, "layout")) return "us";
        if (!strcmp(field, "model")) return "pc105";
        return NULL;
    }
    return keys_layout_name(field);
}
static void okey(int code, int value)
{
    struct input_event e = { .type = EV_KEY, .code = (unsigned short)code, .value = value };
    assert(write(other[1], &e, sizeof e) == sizeof e);
    handle_kbd_event(other[0]);
}

static int sym(int l, int code)
{
    return (int)xkb_state_key_get_one_sym(layouts[l].st, (xkb_keycode_t)code + 8);
}

static int mod(int l, const char *name, enum xkb_state_component kind)
{
    return xkb_state_mod_name_is_active(layouts[l].st, name, kind) > 0;
}

static void layout_transition(void)
{
    assert(ime_mode() == IME_MODE_RAW);
    register_keyboard_signals(); /* the same registrations production main uses */
    set_grab(0);
    assert(raise(SIGUSR2) == 0);
    keyboard_signals();
    assert(!active && !grabbed && !grabs[0] && !grabs[1]);
    own_us = 1;
    int before = reload_count, fonts_before = font_reloads;
    assert(raise(SIGHUP) == 0); /* settings owns the screen while changing keyboards.conf */
    keyboard_signals();
    assert(reload_count == before + 1 && nlayouts == 2 && kbd_layout[1] == 1 && want_grab);
    assert(font_reloads == fonts_before + 1); /* fonts installed meanwhile (Chinese Pinyin's) are seen */
    assert(!active && !grabbed && !grabs[0] && !grabs[1]);
    /* Neither queue is monopolised: the direct program can still read y. */
    struct input_event e = { .type = EV_KEY, .code = KEY_Y, .value = 1 }, got;
    assert(write(events[1], &e, sizeof e) == sizeof e && read(events[0], &got, sizeof got) == sizeof got);
    assert(got.code == KEY_Y);
    assert(write(other[1], &e, sizeof e) == sizeof e && read(other[0], &got, sizeof got) == sizeof got);
    assert(got.code == KEY_Y);
    set_grab(1); /* a delayed UI callback must not reacquire a released seat */
    assert(!grabbed && !grabs[0] && !grabs[1]);
    on_sigusr1(SIGUSR1);
    keyboard_signals();
    assert(active && grabbed && grabs[0] && grabs[1] && nlayouts == 2);
    key(KEY_F5, 1); key(KEY_F5, 0);
    okey(KEY_F6, 1); okey(KEY_F6, 0);
    on_sigusr2(SIGUSR2);
    own_us = 0;
    on_sighup(SIGHUP);
    keyboard_signals();
    assert(!active && !grabbed && !grabs[0] && !grabs[1] && nlayouts == 1);
    own_us = 1;
    on_sighup(SIGHUP);
    keyboard_signals();
    assert(!active && !grabbed && !grabs[0] && !grabs[1] && nlayouts == 2);
    on_sigusr1(SIGUSR1);
    keyboard_signals();
    assert(active && grabbed && grabs[0] && grabs[1]);
    key(KEY_F7, 1); key(KEY_F7, 0);
    okey(KEY_F8, 1); okey(KEY_F8, 0);
    set_grab(0);
    void (*previous)(int) = signal(SIGUSR2, on_sigusr2);
    assert(previous != SIG_ERR);
    release_during_grab = 1;
    set_grab(1);
    assert(!release_during_grab && !active && !grabbed && !grabs[0] && !grabs[1]);
    assert(signal(SIGUSR2, previous) != SIG_ERR);
    keyboard_signals();
    on_sigusr1(SIGUSR1);
    keyboard_signals();
    assert(active && grabbed && grabs[0] && grabs[1]);
    own_us = 0;
    on_sighup(SIGHUP);
    keyboard_signals();
    assert(active && !grabbed && !grabs[0] && !grabs[1] && nlayouts == 1);
    puts("PASS first per-keyboard setup, clear and re-enable never grab the direct program's keyboards; "
         "only the seat take enables forwarding");
}

static int nothing_fed(void)
{
    for (int l = 0; l < nlayouts; l++) {
        for (int c = 0; c <= KEY_MAX; c++) if (layouts[l].fed[c]) return 0;
    }
    return 1;
}

/* The launcher (LOCachy's block, or Alt+F1) from the keyboard: the keyboard
 * the bar's once no key is held (the compositor has the presses of those),
 * the headings passed over, closed by Esc after its release, by running an
 * entry, by another window's focus and by the seat's release; none while the
 * seat is released (home or the lock on the screen). */
static void make_exec_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    assert(fd >= 0 && close(fd) == 0);
}

static void reply_is(int fd, const char *expected)
{
    char reply[256];
    struct pollfd p = { .fd = fd, .events = POLLIN };
    assert(poll(&p, 1, 1000) > 0);
    ssize_t n = recv(fd, reply, sizeof reply - 1, 0);
    assert(n > 0);
    reply[n] = '\0';
    assert(strcmp(reply, expected) == 0);
    p.revents = 0;
    assert(poll(&p, 1, 1000) > 0); /* a just-forked launcher closes CLOEXEC copies */
    assert(recv(fd, reply, sizeof reply, MSG_DONTWAIT) == 0);
    close(fd);
}

static void warning_targets(int base)
{
    int pair[2], replacement[2];
    char log[512];
    snprintf(app_command, sizeof app_command, "%s/app", launch_bin_dir);
    FILE *script = fopen(app_command, "w");
    assert(script && fputs("#!/bin/sh\nprintf '%s\\n' \"$@\"\n", script) >= 0 && fclose(script) == 0 &&
           chmod(app_command, 0755) == 0);

    /* The Enter that chose a target is not a confirmation, nor are repeats
     * or a second Enter from another keyboard while the first remains held. */
    key(KEY_ENTER, 1);
    assert(keys_down != 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    ask_own_tor_target(pair[0], "tor-browser", "https://example.org/a b");
    again_tick();
    assert(again.shown && again.wait_release && opened_calls == 1);
    key(KEY_ENTER, 2);
    assert(again.shown && again.client == pair[0]);
    key(KEY_ENTER, 0);
    again_tick();
    assert(again.shown && grabbed && want_grab && !again.wait_release);
    key(KEY_ESC, 1);
    assert(!again.shown && !want_grab && grabbed); /* dismissal key still ours */
    key(KEY_ESC, 0);
    assert(grabbed == base && !layouts[0].down[KEY_ESC] && !layouts[0].down[KEY_ENTER]);
    reply_is(pair[1], "error cancelled");

    /* A new target cancels and closes exactly the old client, never loses the
     * new target when the picker closes or touches choose.target later. */
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, replacement) == 0);
    ask_own_tor_target(pair[0], "tor-browser", "https://old.invalid/");
    again_tick();
    ask_own_tor_target(replacement[0], "tor-browser", "https://example.org/a b");
    reply_is(pair[1], "error cancelled");
    snprintf(choose.target, sizeof choose.target, "https://changed.invalid/");
    key(KEY_ENTER, 1);
    assert(again.client == -1 && !again.pending && !again.shown && grabbed);
    key(KEY_ENTER, 0);
    assert(grabbed == base && !layouts[0].down[KEY_ENTER]);
    reply_is(replacement[1], "ok tor-browser");
    int status;
    assert(waitpid(-1, &status, 0) > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    snprintf(log, sizeof log, "%s/tor-browser.log", launch_run_dir);
    char text[512] = "";
    FILE *f = fopen(log, "r");
    assert(f && fread(text, 1, sizeof text - 1, f) > 0 && fclose(f) == 0);
    assert(strcmp(text, "open\n--own-tor\ntor-browser\nhttps://example.org/a b\n") == 0);
    unlink(log);

    /* Seat release/lock cancels both pending and displayed warnings. */
    for (int shown = 0; shown < 2; shown++) {
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
        ask_own_tor_target(pair[0], "tor-browser", "https://locked.invalid/");
        if (shown) again_tick();
        on_sigusr2(SIGUSR2);
        keyboard_signals();
        again_tick();
        assert(!again.pending && !again.shown && again.client == -1 && !grabbed);
        reply_is(pair[1], "error cancelled");
        on_sigusr1(SIGUSR1);
        keyboard_signals();
        assert(active && grabbed == base && !question_active());
    }
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    ask_own_tor_target(pair[0], "tor-browser", "https://timeout.invalid/");
    again_tick();
    test_time += 61.0;
    again_tick();
    assert(!question_active() && grabbed == base);
    reply_is(pair[1], "error timed out");
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    ask_own_tor_target(pair[0], "tor-browser", "https://gone.invalid/");
    close(pair[1]);
    again_tick();
    assert(!question_active() && again.client == -1 && grabbed == base);
    unlink(app_command);
    puts("PASS own-Tor targets: held Enter/repeats cannot confirm; cancel/replacement/lock/timeout/disconnect close clients; confirmed target retained");
}

static void launcher_keys(void)
{
    char fixdir[] = "/tmp/bar-launch-XXXXXX", cg[256], path[512];
    assert(mkdtemp(fixdir));
    snprintf(launch_bin_dir, sizeof launch_bin_dir, "%s/bin", fixdir);
    snprintf(launch_cg_dir, sizeof launch_cg_dir, "%s/cgroup", fixdir);
    snprintf(launch_run_dir, sizeof launch_run_dir, "%s/apps", fixdir);
    snprintf(launch_conf_dir, sizeof launch_conf_dir, "%s/conf", fixdir);
    assert(mkdir(launch_bin_dir, 0755) == 0 && mkdir(launch_cg_dir, 0755) == 0 &&
           mkdir(launch_run_dir, 0755) == 0 && mkdir(launch_conf_dir, 0755) == 0);
    snprintf(launch_mixer_path, sizeof launch_mixer_path, "%s/wiremix", launch_bin_dir);
    snprintf(launch_bt_path, sizeof launch_bt_path, "%s/bluetui", launch_bin_dir);
    snprintf(launch_net_path, sizeof launch_net_path, "%s/nmtui", launch_bin_dir);
    snprintf(launch_shell_path, sizeof launch_shell_path, "%s/user-shell", launch_bin_dir);
    static const char *const bins[] = { "files", "view", "term", "settings", "wiremix", "bluetui", "nmtui", "user-shell" };
    for (size_t i = 0; i < sizeof bins / sizeof *bins; i++) {
        snprintf(path, sizeof path, "%s/%s", launch_bin_dir, bins[i]);
        make_exec_file(path);
    }
    /* Containers: edge (running), wechat (frozen), foot (closed), tor-browser (closed + own Tor). */
    static const char *const conts[] = { "edge", "foot", "tor-browser", "wechat", NULL };
    test_containers = conts;
    snprintf(cg, sizeof cg, "%s/app_edge", launch_cg_dir); assert(mkdir(cg, 0755) == 0);
    snprintf(path, sizeof path, "%s/cgroup.events", cg);
    FILE *f = fopen(path, "w"); assert(f && fputs("populated 1\nfrozen 0\n", f) >= 0 && fclose(f) == 0);
    snprintf(path, sizeof path, "%s/edge.ready", launch_run_dir); make_exec_file(path);
    snprintf(cg, sizeof cg, "%s/app_wechat", launch_cg_dir); assert(mkdir(cg, 0755) == 0);
    snprintf(path, sizeof path, "%s/cgroup.events", cg);
    f = fopen(path, "w"); assert(f && fputs("populated 1\nfrozen 1\n", f) >= 0 && fclose(f) == 0);
    snprintf(path, sizeof path, "%s/tor-browser.conf", launch_conf_dir);
    f = fopen(path, "w"); assert(f && fputs("network=\"1\"\ntor=\"1\"\npackage=\"tor-browser-bin\"\ncommand=\"/usr/bin/tor-browser\"\narguments=\"\"\n", f) >= 0 && fclose(f) == 0);

    static const char *const full_rc[] = {
        "program_view", "/usr/local/bin/view",
        "program_edit", "/usr/local/bin/edit",
        "program_archive", "/usr/local/bin/archive",
        "program_term", "/usr/local/bin/term /usr/local/lib/locachy/user-shell",
        "program_admin", "/usr/local/bin/term",
        "program_network", "/usr/local/bin/term /usr/bin/nmtui",
        "program_settings", "/usr/local/bin/settings",
        "program_packages", "/usr/local/bin/packages",
        "program_firmware", "/usr/local/bin/firmware",
        "program_bluetooth", "/usr/local/bin/term /usr/bin/bluetui",
        "program_mixer", "/usr/local/bin/term /usr/bin/wiremix",
        "service_bluetooth", "/usr/local/lib/locachy/bluetooth-client",
        NULL
    };
    rc_programs = full_rc;
    /* own-Tor is not a package-name shortcut: canonical command form and
     * last duplicate key are required; generic tor=1 remains system-only. */
    snprintf(path, sizeof path, "%s/tor-browser.conf", launch_conf_dir);
    f = fopen(path, "w");
    assert(f && fputs("network=\"1\"\ntor=\"1\"\npackage=\"tor-browser-bin\"\ncommand=\"/usr/bin/tor-browser\"\narguments=\"--private-window\"\n", f) >= 0 && fclose(f) == 0);
    assert(bar_needs_own_tor("tor-browser"));
    f = fopen(path, "w");
    assert(f && fputs("network=\"1\"\ntor=\"1\"\npackage=\"tor-browser-bin\"\ncommand=\"/usr/bin/env\"\narguments=\"MOZ_APP_REMOTINGNAME=tor-browser /usr/bin/tor-browser --private-window\"\n", f) >= 0 && fclose(f) == 0);
    assert(bar_needs_own_tor("tor-browser"));
    f = fopen(path, "w");
    assert(f && fputs("network=\"1\"\ntor=\"1\"\npackage=\"other\"\ncommand=\"/usr/bin/tor-browser\"\n", f) >= 0 && fclose(f) == 0);
    assert(!bar_needs_own_tor("tor-browser"));
    f = fopen(path, "w");
    assert(f && fputs("network=\"1\"\ntor=\"1\"\npackage=\"other\"\npackage=\"tor-browser-bin\"\ncommand=\"/usr/bin/tor-browser\"\n", f) >= 0 && fclose(f) == 0);
    assert(bar_needs_own_tor("tor-browser"));
    f = fopen(path, "w");
    assert(f && fputs("network=\"1\"\ntor=\"1\"\npackage=\"tor-browser-bin\"\npackage=\"other\"\ncommand=\"/usr/bin/tor-browser\"\n", f) >= 0 && fclose(f) == 0);
    assert(!bar_needs_own_tor("tor-browser"));
    f = fopen(path, "w");
    assert(f && fputs("network=\"1\"\ntor=\"1\"\npackage=\"tor-browser-bin\"\ncommand=\"/usr/bin/env\"\narguments=\"OTHER=1 MOZ_APP_REMOTINGNAME=tor-browser /usr/bin/tor-browser\"\n", f) >= 0 && fclose(f) == 0);
    assert(!bar_needs_own_tor("tor-browser"));
    f = fopen(path, "w");
    assert(f && fputs("network=\"1\"\ntor=\"1\"\npackage=\"tor-browser-bin\"\ncommand=\"/usr/bin/tor-browser\"\narguments=\"\"\n", f) >= 0 && fclose(f) == 0);
    puts("PASS own-Tor eligibility uses canonical package/argv and last duplicate key; generic tor=1 stays system-only");

    /* Status bar Bluetooth click: bluetui directly when service_bluetooth + bluetui exist, else settings devices. */
    win_calls = 0;
    status_open(PANEL_BLOCK_BLUETOOTH);
    assert(win_calls == 1 && !strcmp(win_prog, "bluetooth") && win_target[0] == '\0');
    status_open(PANEL_BLOCK_VOLUME);
    assert(win_calls == 2 && !strcmp(win_prog, "mixer") && win_target[0] == '\0');
    status_open(PANEL_BLOCK_NETWORK);
    assert(win_calls == 3 && !strcmp(win_prog, "settings") && !strcmp(win_target, "network"));
    rc_programs = NULL;
    status_open(PANEL_BLOCK_BLUETOOTH);
    assert(win_calls == 4 && !strcmp(win_prog, "settings") && !strcmp(win_target, "devices"));
    rc_programs = full_rc;
    puts("PASS top-bar Bluetooth click opens bluetui directly when enabled, falling back to settings devices");

    assert(active && keys_down == 0 && !launcher_shown);
    set_grab(grab_wanted());
    int base = grabbed; /* the keyboard's owner without the launcher */
    key(KEY_LEFTALT, 1); key(KEY_F1, 1);
    assert(launcher_shown && grabbed == base && grabs[0] == base);
    key(KEY_F1, 0); key(KEY_LEFTALT, 0);
    assert(launcher_shown && grabbed && grabs[0] && keys_down == 0);
    assert(launcher.sel == 0 && launcher.items[0].kind == LAUNCH_HOME && launcher.items[1].kind == LAUNCH_HEADER);
    /* Verify sections & exact order mirror home: Containers, Programs, System and hardware. */
    int hdr_cont = -1, hdr_prog = -1, hdr_sys = -1;
    for (int i = 0; i < launcher.n; i++) {
        if (launcher.items[i].kind == LAUNCH_HEADER) {
            if (launcher.items[i].section == SECTION_CONTAINERS) hdr_cont = i;
            if (launcher.items[i].section == SECTION_PROGRAMS) hdr_prog = i;
            if (launcher.items[i].section == SECTION_SYSTEM) hdr_sys = i;
        }
    }
    assert(hdr_cont == 1 && hdr_prog > hdr_cont && hdr_sys > hdr_prog);
    assert(!strcmp(launcher.items[hdr_cont + 1].name, "edge") && !strcmp(launch_detail(&launcher.items[hdr_cont + 1]), "running"));
    assert(!strcmp(launcher.items[hdr_cont + 2].name, "foot") && !strcmp(launch_detail(&launcher.items[hdr_cont + 2]), "closed"));
    assert(!strcmp(launcher.items[hdr_cont + 4].name, "wechat") && !strcmp(launch_detail(&launcher.items[hdr_cont + 4]), "frozen"));
    assert(!strcmp(launcher.items[hdr_prog + 1].name, "files") && !strcmp(launch_detail(&launcher.items[hdr_prog + 1]), "window"));
    assert(!strcmp(launcher.items[hdr_prog + 2].name, "view") && !strcmp(launch_detail(&launcher.items[hdr_prog + 2]), "window"));
    assert(!strcmp(launcher.items[hdr_prog + 3].name, "edit") && !strcmp(launch_detail(&launcher.items[hdr_prog + 3]), "full screen"));
    assert(!strcmp(launcher.items[hdr_prog + 5].name, "term") && !strcmp(launch_detail(&launcher.items[hdr_prog + 5]), "window"));
    assert(!strcmp(launcher.items[hdr_sys + 1].name, "admin") && !strcmp(launch_detail(&launcher.items[hdr_sys + 1]), "full screen"));
    assert(!strcmp(launcher.items[hdr_sys + 2].name, "network") && !strcmp(launch_detail(&launcher.items[hdr_sys + 2]), "window"));
    assert(!strcmp(launcher.items[hdr_sys + 3].name, "settings") && !strcmp(launch_detail(&launcher.items[hdr_sys + 3]), "window"));
    assert(!strcmp(launcher.items[hdr_sys + 6].name, "bluetooth") && !strcmp(launch_detail(&launcher.items[hdr_sys + 6]), "window"));
    assert(!strcmp(launcher.items[hdr_sys + 7].name, "mixer") && !strcmp(launch_detail(&launcher.items[hdr_sys + 7]), "window"));
    assert(launcher.items[hdr_sys + 8].kind == LAUNCH_REBOOT &&
           launcher.items[hdr_sys + 9].kind == LAUNCH_POWEROFF && hdr_sys + 10 == launcher.n);

    key(KEY_UP, 1); key(KEY_UP, 0);
    assert(launcher.sel == 0);
    key(KEY_DOWN, 1); key(KEY_DOWN, 0);
    assert(launcher.sel > 1 && launcher.items[launcher.sel].kind != LAUNCH_HEADER);
    for (int i = 0; i < 2 * LAUNCH_MAX; i++) {
        key(KEY_DOWN, 1); key(KEY_DOWN, 0);
        assert(launcher.items[launcher.sel].kind != LAUNCH_HEADER);
    }
    int last = launcher.n - 1;
    while (launcher.items[last].kind == LAUNCH_HEADER) last--;
    assert(launcher.sel == last);
    for (int i = 0; i < 2 * LAUNCH_MAX; i++) {
        key(KEY_UP, 1); key(KEY_UP, 0);
    }
    assert(launcher.sel == 0 && !layouts[0].down[KEY_DOWN] && !layouts[0].down[KEY_UP]);
    key(KEY_ESC, 1);
    assert(!launcher_shown && grabbed); /* Esc held: the window never had its press */
    key(KEY_ESC, 0);
    assert(grabbed == base && grabs[0] == base && !layouts[0].down[KEY_ESC]);
    /* Power rows are requests to home, never direct signals; home owns Y/N. */
    launcher_open(NULL);
    while (launcher.items[launcher.sel].kind != LAUNCH_REBOOT) { key(KEY_DOWN, 1); key(KEY_DOWN, 0); }
    signalled = asked = 0;
    key(KEY_ENTER, 1); key(KEY_ENTER, 0);
    assert(!launcher_shown && signalled == SIGUSR1 && asked == 1 &&
           !strcmp(asked_with, "program:" HOME_REQUEST_REBOOT));
    launcher_open(NULL);
    while (launcher.items[launcher.sel].kind != LAUNCH_POWEROFF) { key(KEY_DOWN, 1); key(KEY_DOWN, 0); }
    signalled = asked = 0;
    key(KEY_ENTER, 1); key(KEY_ENTER, 0);
    assert(!launcher_shown && signalled == SIGUSR1 && asked == 1 &&
           !strcmp(asked_with, "program:" HOME_REQUEST_POWEROFF));
    puts("PASS desktop power entries ask home through reserved requests, never perform power actions directly");
    puts("PASS Alt+F1 opens launcher mirroring home's 3 sections, live container states and uniform details");

    signalled = 0;
    key(KEY_LEFTALT, 1); key(KEY_F1, 1); key(KEY_F1, 0); key(KEY_LEFTALT, 0);
    key(KEY_ENTER, 1);
    assert(!launcher_shown && signalled == SIGUSR1);
    key(KEY_ENTER, 0);
    assert(grabbed == base && !layouts[0].down[KEY_ENTER]);
    key(KEY_LEFTALT, 1); key(KEY_F1, 1); key(KEY_F1, 0); key(KEY_LEFTALT, 0);
    assert(launcher_shown);
    key(KEY_LEFTALT, 1); key(KEY_F1, 1); key(KEY_F1, 0); key(KEY_LEFTALT, 0);
    assert(!launcher_shown && grabbed == base);
    puts("PASS Enter on home closes launcher and asks seat for home; Alt+F1 again toggles it closed");

    /* Enter on container opens it on desktop; Enter on window program opens window;
     * F on window or full-screen program opens full screen in home. */
    key(KEY_LEFTALT, 1); key(KEY_F1, 1); key(KEY_F1, 0); key(KEY_LEFTALT, 0);
    while (strcmp(launcher.items[launcher.sel].name, "edge") != 0) { key(KEY_DOWN, 1); key(KEY_DOWN, 0); }
    opened_calls = 0;
    key(KEY_ENTER, 1); key(KEY_ENTER, 0);
    assert(!launcher_shown && opened_calls == 1 && !strcmp(opened_container, "edge") && !opened_own_tor);

    /* Closed tor="1" container with service_tor off asks own-Tor confirmation on desktop. */
    key(KEY_LEFTALT, 1); key(KEY_F1, 1); key(KEY_F1, 0); key(KEY_LEFTALT, 0);
    while (strcmp(launcher.items[launcher.sel].name, "tor-browser") != 0) { key(KEY_DOWN, 1); key(KEY_DOWN, 0); }
    opened_calls = 0;
    key(KEY_ENTER, 1); key(KEY_ENTER, 0);
    assert(!launcher_shown && opened_calls == 0 && again.pending && again.own_tor && !strcmp(again.name, "tor-browser"));
    again_tick();
    assert(again.shown && grabbed);
    key(KEY_ENTER, 1); key(KEY_ENTER, 0);
    assert(!again.shown && opened_calls == 1 && !strcmp(opened_container, "tor-browser") && opened_own_tor);
    assert(grabbed == base && !want_grab);
    warning_targets(base);

    static const char *const win_names[] = { "files", "view", "term", "network", "settings", "bluetooth", "mixer" };
    for (size_t k = 0; k < sizeof win_names / sizeof *win_names; k++) {
        key(KEY_LEFTALT, 1); key(KEY_F1, 1); key(KEY_F1, 0); key(KEY_LEFTALT, 0);
        while (strcmp(launcher.items[launcher.sel].name, win_names[k]) != 0) { key(KEY_DOWN, 1); key(KEY_DOWN, 0); }
        win_calls = 0;
        key(KEY_ENTER, 1); key(KEY_ENTER, 0);
        assert(!launcher_shown && win_calls == 1 && !strcmp(win_prog, win_names[k]));

        key(KEY_LEFTALT, 1); key(KEY_F1, 1); key(KEY_F1, 0); key(KEY_LEFTALT, 0);
        while (strcmp(launcher.items[launcher.sel].name, win_names[k]) != 0) { key(KEY_DOWN, 1); key(KEY_DOWN, 0); }
        signalled = asked = 0;
        key(KEY_F, 1); key(KEY_F, 0);
        assert(!launcher_shown && signalled == SIGUSR1 && asked == 1);
        if (!strcmp(win_names[k], "files")) {
            assert(asked_with[0] == '\0' && !strcmp(asked_target, "/nonexistent/home"));
        } else {
            char want[32];
            snprintf(want, sizeof want, "program:%s", win_names[k]);
            assert(!strcmp(asked_with, want) && asked_target[0] == '\0');
        }
    }
    key(KEY_LEFTALT, 1); key(KEY_F1, 1); key(KEY_F1, 0); key(KEY_LEFTALT, 0);
    while (strcmp(launcher.items[launcher.sel].name, "admin") != 0) { key(KEY_DOWN, 1); key(KEY_DOWN, 0); }
    signalled = asked = win_calls = 0;
    key(KEY_ENTER, 1); key(KEY_ENTER, 0);
    assert(!launcher_shown && win_calls == 0 && signalled == SIGUSR1 && asked == 1 && !strcmp(asked_with, "program:admin"));

    /* Scrolling on a short screen keeps selection + section header in view. */
    launcher_open(NULL);
    launcher.vis = 6;
    launcher.scroll = 0;
    launcher.sel = launcher.n - 1;
    launcher_keep_visible(6);
    assert(launcher.scroll == launcher.n - 6 && launcher.sel >= launcher.scroll);
    launcher.sel = hdr_prog + 1;
    launcher_keep_visible(6);
    assert(launcher.scroll == hdr_prog);
    launcher.sel = 0;
    launcher.scroll = 0;
    launcher_wheel(25.0); /* two wheel steps down without snapping back to sel=0 */
    assert(launcher.scroll == 2);
    launcher_wheel(-15.0);
    assert(launcher.scroll == 1);
    launcher_close();
    puts("PASS Enter opens containers & desktop windows; F opens full screen in home; scrolling keeps header & item visible");

    test_containers = NULL;
    rc_programs = NULL;
    launcher_toggle(NULL); /* a click on LOCachy */
    assert(launcher_shown && grabbed);
    launcher_tick();
    assert(launcher_shown);
    focus_changes++; /* another window got the focus */
    launcher_tick();
    assert(!launcher_shown && grabbed == base);
    launcher_toggle(NULL);
    assert(launcher_shown);
    on_sigusr2(SIGUSR2); /* home or the lock comes to the front */
    keyboard_signals();
    launcher_tick();
    assert(!launcher_shown && !active && !grabbed && !grabs[0]);
    launcher_toggle(NULL);
    key(KEY_LEFTALT, 1); key(KEY_F1, 1); key(KEY_F1, 0); key(KEY_LEFTALT, 0);
    assert(!launcher_shown && !grabbed && !grabs[0]);
    on_sigusr1(SIGUSR1);
    keyboard_signals();
    assert(active && !launcher_shown);
    puts("PASS the launcher closes when another window gets the focus or the seat is released (home, the lock), "
         "and opens neither from a click nor from Alt+F1 while it is released");

    /* Cleanup temp fixture */
    for (size_t i = 0; i < sizeof bins / sizeof *bins; i++) {
        snprintf(path, sizeof path, "%s/%s", launch_bin_dir, bins[i]);
        unlink(path);
    }
    snprintf(path, sizeof path, "%s/app_edge/cgroup.events", launch_cg_dir); unlink(path);
    snprintf(path, sizeof path, "%s/app_edge", launch_cg_dir); rmdir(path);
    snprintf(path, sizeof path, "%s/app_wechat/cgroup.events", launch_cg_dir); unlink(path);
    snprintf(path, sizeof path, "%s/app_wechat", launch_cg_dir); rmdir(path);
    snprintf(path, sizeof path, "%s/edge.ready", launch_run_dir); unlink(path);
    snprintf(path, sizeof path, "%s/tor-browser.conf", launch_conf_dir); unlink(path);
    rmdir(launch_bin_dir); rmdir(launch_cg_dir); rmdir(launch_run_dir); rmdir(launch_conf_dir); rmdir(fixdir);
}

/*
 * Updating in place (bar_exec), in a process of its own: test-bar-keyboard
 * exec-child, started from a copy of this program that the parent then
 * replaces with a new file, as a rebuild does. The child runs the bar's own
 * steps: exe_record, bar_inherit, the listening sockets, the seat's
 * signals, keyboard_signals on SIGHUP, exec_ready and bar_exec in its loop.
 * It reports on TEST_REPORT and takes commands on TEST_COMMANDS (fds):
 * "lend" (a keyboard: a pipe), "busy N" (N notifications on the screen),
 * "quit".
 */
static int report_fd = -1;
int kill(pid_t pid, int sig); /* the real one: the bar's was fake_signal, declared under that name */

static void report(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n] = '\n';
    assert(write(report_fd, line, (size_t)n + 1) == n + 1);
}

static int exec_child(char **argv)
{
    report_fd = atoi(getenv("TEST_REPORT"));
    int commands = atoi(getenv("TEST_COMMANDS"));
    sigset_t mask;
    sigprocmask(SIG_SETMASK, NULL, &mask);
    int held = sigismember(&mask, SIGHUP) && sigismember(&mask, SIGUSR1) && sigismember(&mask, SIGUSR2);
    exe_record(argv);
    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(xkb_ctx);
    init_xkb();
    bar_inherit();
    open_listen();
    bar_listen();
    register_keyboard_signals();
    sigprocmask(SIG_SETMASK, NULL, &mask);
    assert(!sigismember(&mask, SIGHUP) && !sigismember(&mask, SIGUSR2));
    /* A request made while the previous image ran waits in the backlog. */
    int waiting = accept4(bar_fd, NULL, NULL, SOCK_CLOEXEC);
    report("start pid=%d inherited=%d keys=%d kbds=%d active=%d held=%d waiting=%d", (int)getpid(),
           bar_inherited, keys_link >= 0, nkbds, (int)active, held, waiting >= 0);
    if (waiting >= 0) close(waiting);
    for (;;) {
        struct pollfd p[2] = { { .fd = wake_pipe[0], .events = POLLIN }, { .fd = commands, .events = POLLIN } };
        poll(p, 2, exec_wanted ? 100 : -1);
        char drain[64];
        while (read(wake_pipe[0], drain, sizeof drain) > 0) {
        }
        int hup = sig_reload;
        keyboard_signals();
        if (hup) report("hup wanted=%d", exec_wanted);
        if (p[1].revents & POLLIN) {
            char line[64];
            ssize_t n = read(commands, line, sizeof line - 1);
            assert(n > 0);
            line[n] = '\0';
            if (!strncmp(line, "lend", 4)) {
                int kbd[2];
                assert(pipe2(kbd, O_NONBLOCK | O_CLOEXEC) == 0);
                take_keyboards(&kbd[0], 1);
                report("lent kbds=%d", nkbds);
            } else if (!strncmp(line, "busy ", 5)) {
                nnotes = atoi(line + 5);
                report("busy %d", nnotes);
            } else {
                return 0;
            }
        }
        if (exec_wanted && exec_ready()) bar_exec();
    }
}

static int read_line(int fd, char *line, size_t size, int timeout_ms)
{
    size_t n = 0;
    while (n + 1 < size) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, timeout_ms) <= 0 || read(fd, &line[n], 1) != 1) return 0;
        if (line[n] == '\n') break;
        n++;
    }
    line[n] = '\0';
    return 1;
}

/* from written as a new file at to (a new inode, as the linker writes it),
 * its time age seconds ago (settled). */
static void install_copy(const char *from, const char *to, int age)
{
    char temporary[PATH_MAX], buffer[65536];
    snprintf(temporary, sizeof temporary, "%s.new", to);
    int in = open(from, O_RDONLY | O_CLOEXEC), out = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    assert(in >= 0 && out >= 0);
    ssize_t n;
    while ((n = read(in, buffer, sizeof buffer)) > 0) assert(write(out, buffer, (size_t)n) == n);
    assert(n == 0 && close(in) == 0 && close(out) == 0);
    struct timespec times[2] = { { .tv_sec = time(NULL) - age }, { .tv_sec = time(NULL) - age } };
    assert(utimensat(AT_FDCWD, temporary, times, 0) == 0 && rename(temporary, to) == 0);
}

static void expect(int fd, const char *prefix, char *line, size_t size)
{
    if (!read_line(fd, line, size, 10000) || strncmp(line, prefix, strlen(prefix)) != 0) {
        fprintf(stderr, "FAIL bar exec: wanted \"%s...\", got \"%s\"\n", prefix, line);
        abort();
    }
}

static void test_exec_in_place(void)
{
    char dir[] = "/tmp/bar-exec-XXXXXX", cwd[PATH_MAX], copy[PATH_MAX], line[256];
    assert(mkdtemp(dir) && getcwd(cwd, sizeof cwd) && chdir(dir) == 0);
    snprintf(copy, sizeof copy, "%s/locachy-bar", dir);
    char self[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
    assert(len > 0);
    self[len] = '\0';
    install_copy(self, copy, 60);
    int reports[2], commands[2], keys[2];
    assert(pipe2(reports, O_CLOEXEC) == 0 && pipe2(commands, O_CLOEXEC) == 0 &&
           socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, keys) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char number[16];
        fcntl(reports[1], F_SETFD, 0);
        fcntl(commands[0], F_SETFD, 0);
        fcntl(keys[1], F_SETFD, 0); /* as locachy-seat starts the bar */
        snprintf(number, sizeof number, "%d", reports[1]);
        setenv("TEST_REPORT", number, 1);
        snprintf(number, sizeof number, "%d", commands[0]);
        setenv("TEST_COMMANDS", number, 1);
        snprintf(number, sizeof number, "%d", keys[1]);
        setenv("LOCACHY_BAR_KEYS", number, 1);
        execl(copy, copy, "exec-child", (char *)NULL);
        _exit(127);
    }
    close(reports[1]);
    close(commands[0]);
    close(keys[1]);
    int in = reports[0], out = commands[1];
    char want[128];
    snprintf(want, sizeof want, "start pid=%d inherited=0 keys=1 kbds=0 active=1 held=0 waiting=0", (int)pid);
    expect(in, want, line, sizeof line);
    assert(write(out, "lend", 4) == 4);
    expect(in, "lent kbds=1", line, sizeof line);
    /* The executable unchanged: a SIGHUP only reloads. */
    assert(kill(pid, SIGHUP) == 0);
    expect(in, "hup wanted=0", line, sizeof line);
    assert(!read_line(in, line, sizeof line, 300));
    puts("PASS bar exec: an unchanged executable is not run again; SIGHUP only reloads");
    /* A new build while a notification is on the screen: it waits. */
    assert(write(out, "busy 1", 6) == 6);
    expect(in, "busy 1", line, sizeof line);
    install_copy(self, copy, 60);
    int waiting = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = "bar-exec.sock" };
    assert(waiting >= 0 && connect(waiting, (struct sockaddr *)&address, sizeof address) == 0);
    assert(kill(pid, SIGUSR2) == 0); /* the seat released (home on the screen) */
    assert(kill(pid, SIGHUP) == 0);
    expect(in, "hup wanted=1", line, sizeof line);
    assert(!read_line(in, line, sizeof line, 500));
    puts("PASS bar exec: a new build waits while something is on the screen to answer or read");
    assert(write(out, "busy 0", 6) == 6);
    expect(in, "busy 0", line, sizeof line);
    snprintf(want, sizeof want, "start pid=%d inherited=2 keys=1 kbds=1 active=0 held=1 waiting=1", (int)pid);
    expect(in, want, line, sizeof line);
    close(waiting);
    puts("PASS bar exec: the new build runs in place: same PID, seat link, keyboard, sockets (a waiting request taken), released state, the seat's signals held until handled");
    /* The new image knows its own file: no second exec. */
    assert(kill(pid, SIGHUP) == 0);
    expect(in, "hup wanted=0", line, sizeof line);
    assert(write(out, "quit", 4) == 4);
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(in);
    close(out);
    close(keys[0]);
    unlink(copy);
    unlink("bar-exec.sock");
    unlink("bar-exec-open/socket");
    rmdir("bar-exec-open");
    assert(chdir(cwd) == 0 && rmdir(dir) == 0);
    puts("PASS bar exec: after the exec the new image reloads on SIGHUP and is not run again");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "exec-child")) return exec_child(argv);
    int integration = argc >= 2 && !strcmp(argv[1], "--headless");
    assert(argc == 1 || integration);
#ifdef CONFIG_IME
    if (integration) {
        assert(argc == 4);
        write_schema(argv[2]);
        assert(mkdir(argv[3], 0700) == 0);
        assert(ime_init(argv[2], argv[3]) == 0);
    }
#endif
    if (integration) {
        display = wl_display_connect(NULL);
        assert(display);
        registry = wl_display_get_registry(display);
        static const struct wl_registry_listener listener = { integration_global, integration_remove };
        wl_registry_add_listener(registry, &listener, NULL);
        assert(wl_display_roundtrip(display) >= 0 && wl_display_roundtrip(display) >= 0);
        assert(vkbd_mgr && wl_seat_obj);
        text_vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkbd_mgr, wl_seat_obj);
    }
    assert(pipe2(events, O_NONBLOCK | O_CLOEXEC) == 0);
    nkbds = 1;
    kbds[0] = events[0];
    active = 1;
    want_grab = grabbed = 0;
    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(xkb_ctx);
    /* The system's key table names that layout (keys.c), as keytable writes
     * them, with its letters, digits and space: the keyboards without one of
     * their own type with it, the input method gets them through it. */
    char table[] = "/tmp/bar-keyboard-table-XXXXXX";
    int tfd = mkstemp(table);
    FILE *tf = tfd >= 0 ? fdopen(tfd, "w") : NULL;
    assert(tf);
    fputs("console jp106\nlayout jp\nmodel jp106\nvariant -\noptions ctrl:swapcaps\n", tf);
    static const struct { int first; const char *keys; } rows[] = {
        { KEY_1, "1234567890" }, { KEY_Q, "qwertyuiop" }, { KEY_A, "asdfghjkl" }, { KEY_Z, "zxcvbnm" }, { KEY_SPACE, " " },
    };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        for (int i = 0; rows[r].keys[i]; i++) {
            int c = rows[r].keys[i], upper = c >= 'a' && c <= 'z' ? c - 32 : c;
            char name[8];
            snprintf(name, sizeof name, "%s", c == ' ' ? "space" : (char[]){ (char)c, 0 });
            fprintf(tf, "key %d %s", rows[r].first + i, name);
            for (int st = 0; st < 8; st++) {
                int ch = !!(st & 1) != !!(st & 2) ? upper : c; /* Shift, Caps Lock: letters only */
                fprintf(tf, " %x:%x", ch, ch);
            }
            fputc('\n', tf);
        }
    }
    assert(fclose(tf) == 0);
    snprintf(system_table, sizeof system_table, "%s", table);
    assert(keys_layout_load(table) == 0);
    assert(keys_char(KEY_N, 0) == 'n' && keys_char(KEY_SPACE, 0) == ' ');
    const char *const jp_swap[4] = { "jp", "jp106", NULL, "ctrl:swapcaps" };
    assert(layout_init(&layouts[0], jp_swap) == 0);
    nlayouts = 1;
    key(KEY_CAPSLOCK, 1);
    assert(ctrl && !caps_locked());
    key(KEY_C, 1);
    assert(ctrl);
    key(KEY_C, 0);
    key(KEY_CAPSLOCK, 0);
    assert(!ctrl && !caps_locked());
    key(KEY_LEFTCTRL, 1);
    key(KEY_LEFTCTRL, 0);
    assert(!ctrl && caps_locked());
    key(KEY_LEFTCTRL, 1);
    key(KEY_LEFTCTRL, 0);
    assert(!caps_locked());
    puts("PASS graphical shortcuts track XKB Ctrl/Caps roles, not physical positions");
    assert(pipe2(other, O_NONBLOCK | O_CLOEXEC) == 0);
    nkbds = 2; kbds[1] = other[0];
    key(KEY_CAPSLOCK, 1);
    struct input_event both = { .type = EV_KEY, .code = KEY_CAPSLOCK, .value = 1 };
    assert(write(other[1], &both, sizeof both) == sizeof both);
    handle_kbd_event(other[0]);
    key(KEY_CAPSLOCK, 0);
    assert(ctrl && layouts[0].fed[KEY_CAPSLOCK] == 1);
    both.value = 0;
    assert(write(other[1], &both, sizeof both) == sizeof both);
    handle_kbd_event(other[0]);
    assert(!ctrl && keys_down == 0);
    puts("PASS identical modifier keycodes on two devices remain held until both release");
    key(KEY_CAPSLOCK, 1);
    both.value = 1;
    assert(write(other[1], &both, sizeof both) == sizeof both); handle_kbd_event(other[0]);
    reconcile_keyboards(&other[0], 1);
    assert(nkbds == 1 && kbds[0] == other[0] && ctrl);
    both.value = 0;
    assert(write(other[1], &both, sizeof both) == sizeof both); handle_kbd_event(other[0]);
    assert(!ctrl);
    int reconnect[] = { events[0], other[0] };
    reconcile_keyboards(reconnect, 2);
    assert(nkbds == 2 && keys_down == 0);
    puts("PASS removing one keyboard preserves another device's held modifier; reconnect needs no desktop restart");

    /* The second keyboard with a layout of its own (us) beside the system's
     * (jp, Ctrl and Caps Lock swapped): each key as its keyboard's layout
     * has it, the modifiers of both keyboards in both, one Caps Lock. */
    const char *const us[4] = { "us", "pc105", NULL, NULL };
    assert(layout_init(&layouts[1], us) == 0);
    nlayouts = 2;
    kbd_layout[1] = 1;
    assert(grab_wanted()); /* in direct input too: the compositor has one layout */
    key(KEY_LEFTSHIFT, 1);
    assert(sym(1, KEY_2) == XKB_KEY_at && sym(0, KEY_2) == XKB_KEY_quotedbl);
    okey(KEY_2, 1); okey(KEY_2, 0);
    key(KEY_LEFTSHIFT, 0);
    assert(sym(1, KEY_2) == XKB_KEY_2 && !shift);
    key(KEY_CAPSLOCK, 1); /* jp's Ctrl here */
    okey(KEY_C, 1);
    assert(ctrl && mod(1, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE));
    okey(KEY_C, 0);
    key(KEY_CAPSLOCK, 0);
    assert(!mod(1, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) && !mod(0, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE));
    key(KEY_LEFTCTRL, 1); key(KEY_LEFTCTRL, 0); /* jp's Caps Lock */
    assert(mod(1, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) && mod(0, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED));
    assert(sym(1, KEY_A) == XKB_KEY_A);
    okey(KEY_CAPSLOCK, 1); okey(KEY_CAPSLOCK, 0); /* us's Caps Lock unlocks both */
    assert(!mod(1, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) && !mod(0, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED));
    /* jp's own Caps Lock is Shift+Eisu: its lock is the others' too. */
    const char *const jp_plain[4] = { "jp", "jp106", NULL, NULL };
    assert(layout_init(&layouts[2], jp_plain) == 0);
    nlayouts = 3;
    kbd_layout[1] = 2;
    okey(KEY_LEFTSHIFT, 1); okey(KEY_CAPSLOCK, 1); okey(KEY_CAPSLOCK, 0); okey(KEY_LEFTSHIFT, 0);
    assert(mod(2, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) && mod(0, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) &&
           mod(1, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED));
    key(KEY_LEFTCTRL, 1); key(KEY_LEFTCTRL, 0); /* unlocked by the other keyboard's */
    assert(!mod(2, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) && !mod(0, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED));
    kbd_layout[1] = 1;
    okey(KEY_LEFTSHIFT, 1);
    assert(shift && sym(0, KEY_2) == XKB_KEY_quotedbl);
    int jp_only[] = { events[0] };
    reconcile_keyboards(jp_only, 1); /* the us keyboard gone with Shift held */
    assert(!mod(0, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) && nothing_fed() && !grab_wanted());
    reconcile_keyboards(reconnect, 2);
    kbd_layout[1] = 1;
    puts("PASS a keyboard of its own layout: its keys by it, modifiers of both keyboards in both, one Caps Lock, "
         "unplugged held keys released");
    /* Chinese mode's width keys, rc.conf's key_ime_width_cn: Ctrl+. and
     * Shift+Space unless set, each key by its symbol in its keyboard's
     * layout (jp's @ is a key of its own, us's is Shift+2). */
    load_width_keys();
    key(KEY_CAPSLOCK, 1); /* jp's Ctrl here */
    assert(width_key(0, KEY_DOT) && width_key(1, KEY_DOT) && !width_key(0, KEY_SPACE));
    key(KEY_CAPSLOCK, 0);
    okey(KEY_LEFTSHIFT, 1);
    assert(width_key(1, KEY_SPACE) && width_key(0, KEY_SPACE) && !width_key(1, KEY_DOT));
    okey(KEY_LEFTSHIFT, 0);
    assert(!width_key(0, KEY_DOT) && !width_key(0, KEY_SPACE) && !width_key(1, KEY_SPACE));
    rc_width_cn = "Ctrl+@,Hiragana_Katakana";
    load_width_keys();
    okey(KEY_LEFTCTRL, 1);
    assert(!width_key(0, KEY_DOT) && width_key(0, KEY_LEFTBRACE) && !width_key(1, KEY_LEFTBRACE) &&
           !width_key(1, KEY_2));
    okey(KEY_LEFTCTRL, 0);
    assert(width_key(0, KEY_KATAKANAHIRAGANA) && !width_key(0, KEY_LEFTBRACE));
    rc_width_cn = "";
    load_width_keys();
    assert(nwidth_keys == 0);
    rc_width_cn = NULL;
    load_width_keys();
    assert(nwidth_keys == 2 && keys_down == 0);
    puts("PASS Chinese mode's width keys from rc.conf's key_ime_width_cn (Ctrl+. and Shift+Space unless set), "
         "by each keyboard's layout");
    kbd_layout[1] = 0;
    layout_transition();
    if (integration) {
        assert(wl_display_roundtrip(display) >= 0);
        init_xkb();
        assert(nlayouts == 1 && layouts[0].keymap_fd >= 0 && !strcmp(layouts[0].names[3], "ctrl:swapcaps"));
        restore_passthrough_keymap();
        assert(wl_display_roundtrip(display) >= 0);
        grabbed = want_grab = 1;
#ifdef CONFIG_IME
        ime_set_mode(IME_MODE_CN);
        static const int pinyin[] = { KEY_N, KEY_I, KEY_H, KEY_A, KEY_O, KEY_SPACE };
        for (size_t i = 0; i < sizeof pinyin / sizeof pinyin[0]; i++) {
            key(pinyin[i], 1); key(pinyin[i], 0);
        }
        assert(!strcmp(committed, "\xe4\xbd\xa0\xe5\xa5\xbd"));
        /* The same on the second keyboard: every keyboard's keys reach Rime. */
        committed[0] = '\0';
        for (size_t i = 0; i < sizeof pinyin / sizeof pinyin[0]; i++) {
            okey(pinyin[i], 1); okey(pinyin[i], 0);
        }
        assert(!strcmp(committed, "\xe4\xbd\xa0\xe5\xa5\xbd"));
        /* Chinese mode's width by key_ime_width_cn's keys, on either
         * keyboard; the keys are the bar's, not the application's. */
        char full[40];
        snprintf(full, sizeof full, "%s", ime_mode_label());
        key(KEY_CAPSLOCK, 1); key(KEY_DOT, 1); /* jp's Ctrl here */
        assert(strcmp(ime_mode_label(), full) != 0 && !layouts[0].down[KEY_DOT]);
        key(KEY_DOT, 0); key(KEY_CAPSLOCK, 0);
        okey(KEY_LEFTSHIFT, 1); okey(KEY_SPACE, 1);
        assert(strcmp(ime_mode_label(), full) == 0 && !layouts[0].down[KEY_SPACE]);
        okey(KEY_SPACE, 0); okey(KEY_LEFTSHIFT, 0);
        ime_set_mode(IME_MODE_EN);
        static const int english[] = { KEY_H, KEY_E, KEY_L, KEY_L, KEY_O, KEY_SPACE };
        for (size_t i = 0; i < sizeof english / sizeof english[0]; i++) {
            key(english[i], 1); key(english[i], 0);
        }
        ime_set_mode(IME_MODE_RAW);
        puts("PASS real Rime CN/EN composition and commit executed from pipe keyboard input");
        puts("PASS real Rime pinyin on a second keyboard commits Chinese text; its width keys switch the width");
#endif
        key(KEY_A, 1);
        assert(layouts[0].down[KEY_A] == 1);
        vkbd_type_utf8("\xe4\xbd\xa0\xe5\xa5\xbd");
        assert(layouts[0].down[KEY_A] == 1);
        key(KEY_A, 0);
        key(KEY_C, 1);
        both.code = KEY_C; both.value = 1;
        assert(write(other[1], &both, sizeof both) == sizeof both); handle_kbd_event(other[0]);
        assert(layouts[0].down[KEY_C] == 2);
        key(KEY_C, 0);
        assert(layouts[0].down[KEY_C] == 1);
        both.value = 0;
        assert(write(other[1], &both, sizeof both) == sizeof both); handle_kbd_event(other[0]);
        assert(!layouts[0].down[KEY_C]);
        key(KEY_RIGHT, 1);
        assert(layouts[0].down[KEY_RIGHT]);
        assert(bars);
        uint32_t removed_name = bars->name;
        reg_remove(NULL, NULL, removed_name);
        assert(!layouts[0].down[KEY_RIGHT] && !keys_down);
        key(KEY_RIGHT, 0);
        key(KEY_D, 1); key(KEY_D, 0);
        both.code = KEY_E; both.value = 1;
        assert(write(other[1], &both, sizeof both) == sizeof both); handle_kbd_event(other[0]);
        assert(layouts[0].down[KEY_E] == 1);
        int remaining[] = { events[0] };
        reconcile_keyboards(remaining, 1);
        assert(!layouts[0].down[KEY_E]);
        reconcile_keyboards(reconnect, 2);
        assert(wl_display_roundtrip(display) >= 0);
        puts("PASS real virtual-keyboard delivery keeps text commit separate from held passthrough keys");
        /* The second keyboard us, its own, beside the system's (jp, Ctrl and
         * Caps Lock swapped): the focused client gets each key in its
         * keyboard's keymap, with the modifiers of both. */
        const char *const us_own[4] = { "us", "pc105", NULL, NULL };
        assert(layout_init(&layouts[nlayouts], us_own) == 0);
        kbd_layout[1] = nlayouts++;
        grabbed = want_grab = 1;
        key(KEY_LEFTSHIFT, 1);
        key(KEY_2, 1); key(KEY_2, 0);   /* the system's, jp: " */
        okey(KEY_2, 1); okey(KEY_2, 0); /* us: @ */
        key(KEY_LEFTSHIFT, 0);
        okey(KEY_LEFTCTRL, 1);
        key(KEY_C, 1); key(KEY_C, 0);   /* Ctrl of the us keyboard, c of the jp one */
        okey(KEY_LEFTCTRL, 0);
        key(KEY_LEFTCTRL, 1); key(KEY_LEFTCTRL, 0); /* jp's Caps Lock */
        okey(KEY_A, 1); okey(KEY_A, 0); /* A on the us keyboard: one Caps Lock */
        okey(KEY_CAPSLOCK, 1); okey(KEY_CAPSLOCK, 0);
        okey(KEY_A, 1); okey(KEY_A, 0);
#ifdef CONFIG_IME
        /* Pinyin on that keyboard, by its own layout, commits too. */
        ime_set_mode(IME_MODE_CN);
        committed[0] = '\0';
        for (size_t i = 0; i < sizeof pinyin / sizeof pinyin[0]; i++) {
            okey(pinyin[i], 1); okey(pinyin[i], 0);
        }
        assert(!strcmp(committed, "\xe4\xbd\xa0\xe5\xa5\xbd"));
        ime_set_mode(IME_MODE_RAW);
        puts("PASS real Rime pinyin on a keyboard of its own layout commits Chinese text");
#endif
        assert(wl_display_roundtrip(display) >= 0);
        for (int l = 0; l < nlayouts; l++) {
            for (int c = 0; c <= KEY_MAX; c++) assert(!layouts[l].down[c]);
        }
        int one[] = { events[0] };
        reconcile_keyboards(one, 1);
        reconcile_keyboards(reconnect, 2);
        assert(wl_display_roundtrip(display) >= 0);
        puts("PASS real delivery through a second layout's virtual keyboard: its keymap, both keyboards' modifiers");
#ifdef CONFIG_IME
        /* Real XKB JIS Shift+Eisu, not a hand-assigned Caps role, while Rime
         * is composing: both layouts must leave composition and type capitals. */
        const char *const plain_us[4] = { "us", "pc105", NULL, NULL };
        const char *const plain_jp[4] = { "jp", "jp106", NULL, NULL };
        reset_keyboard_state();
        layout_free(&layouts[0]);
        layout_free(&layouts[1]);
        assert(layout_init(&layouts[0], plain_us) == 0 && layout_init(&layouts[1], plain_jp) == 0);
        nlayouts = 2;
        kbd_layout[0] = 1;
        kbd_layout[1] = 0;
        own_us = 0;
        own_jis = 1;
        grabbed = want_grab = 1;
        for (int mode = IME_MODE_RAW; mode <= IME_MODE_EN; mode++) {
            ime_set_mode(mode);
            /* Distinct marker per mode, read back from the real client. */
            int marker = mode == IME_MODE_RAW ? KEY_F9 : mode == IME_MODE_CN ? KEY_F10 : KEY_F11;
            okey(marker, 1); okey(marker, 0);
            if (mode != IME_MODE_RAW) { key(KEY_N, 1); key(KEY_N, 0); }
            char preedit[256], candidates[256];
            if (mode != IME_MODE_RAW) assert(ime_get_overlay(preedit, candidates, sizeof preedit));
            key(KEY_LEFTSHIFT, 1);
            key(KEY_CAPSLOCK, 1); key(KEY_CAPSLOCK, 0);
            key(KEY_LEFTSHIFT, 0);
            assert(caps_locked());
            assert(!ime_get_overlay(preedit, candidates, sizeof preedit));
            key(KEY_A, 1); key(KEY_A, 0);
            okey(KEY_B, 1); okey(KEY_B, 0);
            key(KEY_LEFTSHIFT, 1);
            key(KEY_CAPSLOCK, 1); key(KEY_CAPSLOCK, 0);
            key(KEY_LEFTSHIFT, 0);
            assert(!caps_locked());
            if (mode != IME_MODE_RAW) {
                committed[0] = '\0';
                const int *word = mode == IME_MODE_CN ? pinyin : english;
                size_t nword = mode == IME_MODE_CN ? sizeof pinyin / sizeof pinyin[0] : sizeof english / sizeof english[0];
                for (size_t i = 0; i < nword; i++) { key(word[i], 1); key(word[i], 0); }
                assert(mode != IME_MODE_CN || !strcmp(committed, "\xe4\xbd\xa0\xe5\xa5\xbd"));
            }
            /* The other keyboard's plain Caps is the same lock for both. */
            okey(KEY_CAPSLOCK, 1); okey(KEY_CAPSLOCK, 0);
            assert(caps_locked());
            key(KEY_C, 1); key(KEY_C, 0);
            okey(KEY_D, 1); okey(KEY_D, 0);
            okey(KEY_CAPSLOCK, 1); okey(KEY_CAPSLOCK, 0);
            assert(!caps_locked());
            okey(KEY_NUMLOCK, 1); okey(KEY_NUMLOCK, 0);
            key(KEY_KP1, 1); key(KEY_KP1, 0);
            okey(KEY_KP2, 1); okey(KEY_KP2, 0);
            key(KEY_NUMLOCK, 1); key(KEY_NUMLOCK, 0);
            key(KEY_KP1, 1); key(KEY_KP1, 0);
            okey(KEY_KP2, 1); okey(KEY_KP2, 0);
            okey(KEY_F12, 1); okey(KEY_F12, 0);
        }
        puts("PASS real Rime Caps in RAW/CN/EN: JIS Shift+Eisu and US Caps, composition cleared, shared capitals, input resumed");
        puts("PASS real Rime Num Lock in RAW/CN/EN: shared keypad digits and unlocked navigation");
        ime_set_mode(IME_MODE_CN);
        committed[0] = '\0';
        okey(KEY_COMMA, 1); okey(KEY_COMMA, 0);
        assert(!strcmp(committed, "\xef\xbc\x8c")); /* this schema would transform it if EN fed it */
        ime_set_mode(IME_MODE_EN);
        committed[0] = '\0';
        okey(KEY_F4, 1); okey(KEY_F4, 0);
        okey(KEY_COMMA, 1); okey(KEY_COMMA, 0);
        okey(KEY_DOT, 1); okey(KEY_DOT, 0);
        okey(KEY_SLASH, 1); okey(KEY_SLASH, 0);
        okey(KEY_F12, 1); okey(KEY_F12, 0);
        assert(!committed[0]);
        ime_set_mode(IME_MODE_CN);
        committed[0] = '\0';
        okey(KEY_F3, 1); okey(KEY_F3, 0);
        key(KEY_N, 1); key(KEY_N, 0);
        key(KEY_I, 1); key(KEY_I, 0);
        assert(!committed[0]);
        okey(KEY_RIGHTSHIFT, 1);
        okey(KEY_RIGHTSHIFT, 0);
        assert(!strcmp(committed, "\xe4\xbd\xa0\xe5\xa5\xbd"));
        assert(ime_get_commit() == NULL); /* delivered immediately, not retained for the next key */
        /* Even when a third-party schema configures Shift_R: commit_text in
         * ascii_composer, Rime's internal ascii_mode stays clamped off so the
         * very next key in CN still composes and maps Chinese punctuation. */
        committed[0] = '\0';
        okey(KEY_COMMA, 1); okey(KEY_COMMA, 0);
        assert(!strcmp(committed, "\xef\xbc\x8c"));
        puts("PASS native Rime modifier/release commits are delivered immediately");
        /* English mode through the bar's own event path (#291): the test
         * dictionary's "hello" is a completion of "hel" and "hell". Space,
         * a main-row digit, -, Return and a keypad digit (Num Lock on) end the
         * word as typed and are typed; Tab moves to the completion, Space
         * commits it and is typed. Between F1 and F12 (test-input-delivery.sh). */
        ime_set_mode(IME_MODE_EN);
        okey(KEY_F1, 1); okey(KEY_F1, 0);
        static const int en_probe[] = { KEY_H, KEY_E, KEY_L, KEY_SPACE, KEY_H, KEY_E, KEY_L, KEY_3,
                                        KEY_H, KEY_E, KEY_L, KEY_MINUS, KEY_H, KEY_E, KEY_L, KEY_ENTER };
        for (size_t i = 0; i < sizeof en_probe / sizeof en_probe[0]; i++) {
            okey(en_probe[i], 1); okey(en_probe[i], 0);
        }
        okey(KEY_NUMLOCK, 1); okey(KEY_NUMLOCK, 0);
        static const int en_keypad[] = { KEY_H, KEY_E, KEY_L, KEY_KP1, KEY_H, KEY_E, KEY_L, KEY_L, KEY_TAB, KEY_SPACE };
        for (size_t i = 0; i < sizeof en_keypad / sizeof en_keypad[0]; i++) {
            okey(en_keypad[i], 1); okey(en_keypad[i], 0);
        }
        okey(KEY_NUMLOCK, 1); okey(KEY_NUMLOCK, 0);
        okey(KEY_F12, 1); okey(KEY_F12, 0);
        char row1_en[256], row2_en[256];
        assert(!ime_get_overlay(row1_en, row2_en, sizeof row1_en));
        puts("PASS real Rime English through the bar: Space, digits, -, Return and keypad digits typed as typed; "
             "Tab chooses the completion");
        own_jis = 0;
        ime_set_mode(IME_MODE_RAW);
#endif
        grabbed = want_grab = 0;
        text_vkbd = NULL;
        for (int l = 0; l < MAX_LAYOUTS; l++) layouts[l].vkbd = NULL;
    }
    key(KEY_CAPSLOCK, 1);
    layouts[0].down[KEY_RIGHT] = 1;
    kbd_forwarded[0][KEY_RIGHT] = 1;
    reset_keyboard_state();
    assert(!ctrl && !keys_down && !layouts[0].down[KEY_RIGHT] && !kbd_forwarded[0][KEY_RIGHT]);
    assert(nothing_fed());
    close(other[0]); close(other[1]); nkbds = 1;
    puts("PASS output/seat state reset clears depressed modifiers and forwarded key ownership");
    key(KEY_BRIGHTNESSUP, 1);
    key(KEY_BRIGHTNESSUP, 0);
    assert(brightness_action == K_BRI_UP);
    key(KEY_VOLUMEDOWN, 1);
    key(KEY_VOLUMEDOWN, 0);
    assert(audio_action == K_VOL_DOWN);
    key(KEY_MUTE, 1);
    key(KEY_MUTE, 0);
    assert(audio_action == K_VOL_MUTE && keys_down == 0);
    puts("PASS graphical application mode dispatches Fn brightness, volume and mute");
    launcher_keys();
    layouts[0].down[KEY_LEFTMETA] = 1;
    grabbed = 0;
    set_grab(0);
    assert(!layouts[0].down[KEY_LEFTMETA]);
    sig_release = 0;
    on_sigusr2(SIGUSR2);
    assert(sig_release && !active && !grabbed);
    clear_virtual_keys();
    assert(!layouts[0].down[KEY_LEFTMETA]);
    puts("PASS seat release schedules balanced virtual-key cleanup even after the physical grab ended");

    /* The keyboards seat passes on: the same device again keeps its queue
     * and held keys, a new one starts drained, a revoked one goes. */
    active = 1;
    key(KEY_LEFTSHIFT, 1);
    assert(shift && keys_down == 1);
    int again = dup(events[0]), fresh[2];
    assert(again >= 0 && pipe2(fresh, O_NONBLOCK | O_CLOEXEC) == 0);
    take_keyboards(&again, 1);
    assert(nkbds == 1 && kbds[0] == events[0] && shift && fcntl(again, F_GETFD) < 0);
    struct input_event stale = { .type = EV_KEY, .code = KEY_A, .value = 1 };
    assert(write(fresh[1], &stale, sizeof stale) == sizeof stale);
    int lent[] = { dup(events[0]), fresh[0] };
    take_keyboards(lent, 2);
    assert(nkbds == 2 && kbds[0] == events[0] && kbds[1] == fresh[0] && shift);
    assert(read(fresh[0], &stale, sizeof stale) < 0 && errno == EAGAIN);
    puts("PASS keyboards from seat: a device lent again keeps its state, a new one starts drained");
    struct input_event lost[] = {
        { .type = EV_SYN, .code = SYN_DROPPED }, { .type = EV_KEY, .code = KEY_LEFTSHIFT, .value = 0 },
        { .type = EV_SYN, .code = SYN_REPORT },
    };
    assert(write(events[1], lost, sizeof lost) == sizeof lost);
    handle_kbd_event(events[0]);
    assert(!shift && keys_down == 0 && !kbd_down[0][KEY_LEFTSHIFT]);
    puts("PASS after SYN_DROPPED a key the kernel has up is released, none left down");
    revoked_fd = fresh[0];
    drop_dead_keyboards();
    revoked_fd = -1;
    assert(nkbds == 1 && kbds[0] == events[0] && fcntl(fresh[0], F_GETFD) < 0);
    close(fresh[1]);
    take_keyboards(NULL, 0);
    assert(nkbds == 0 && fcntl(events[0], F_GETFD) < 0);
    events[0] = -1;
    puts("PASS a revoked keyboard is closed; none lent, none held");

    /* The locks process 1 gives with the keyboards ("keys LOCKS", through
     * the seat): the bar starts from them after holding none, keeps them
     * across its layouts made again, reports its own changes on the link,
     * and keeps its state when a keyboard is added. */
    int link[2], board[2], extra[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, link) == 0);
    assert(pipe2(board, O_NONBLOCK | O_CLOEXEC) == 0 && pipe2(extra, O_NONBLOCK | O_CLOEXEC) == 0);
    keys_link = link[0];
    char msg[64];
    int len = snprintf(msg, sizeof msg, "keys%c%d", 0, KEYS_LOCK_CAPS);
    assert(control_send(link[1], msg, (size_t)len, &board[0], 1) == 0);
    keyboards_message();
    assert(nkbds == 1 && active);
    assert(caps_locked() && !mod(0, XKB_MOD_NAME_NUM, XKB_STATE_MODS_LOCKED));
    puts("PASS the bar given the keyboards after none starts from process 1's locks (Caps Lock on)");
    struct input_event tap = { .type = EV_KEY, .code = KEY_LEFTCTRL, .value = 1 }; /* jp, ctrl:swapcaps: Caps Lock */
    assert(write(board[1], &tap, sizeof tap) == sizeof tap);
    handle_kbd_event(kbds[0]);
    tap.value = 0;
    assert(write(board[1], &tap, sizeof tap) == sizeof tap);
    handle_kbd_event(kbds[0]);
    int rfds[CONTROL_MAX_FDS], nfds = 0, got = control_recv(link[1], msg, sizeof msg, rfds, &nfds);
    assert(!caps_locked() && got == 7 && !strcmp(msg, "locks") && !strcmp(msg + 6, "0") && nfds == 0);
    tap.value = 1;
    assert(write(board[1], &tap, sizeof tap) == sizeof tap);
    handle_kbd_event(kbds[0]);
    tap.value = 0;
    assert(write(board[1], &tap, sizeof tap) == sizeof tap);
    handle_kbd_event(kbds[0]);
    got = control_recv(link[1], msg, sizeof msg, rfds, &nfds);
    assert(caps_locked() && got == 7 && !strcmp(msg + 6, "1"));
    assert(control_recv(link[1], msg, sizeof msg, rfds, &nfds) < 0 && errno == EAGAIN); /* once per change */
    puts("PASS each Caps Lock the bar sees is reported on the seat's link (locks 0, locks 1)");
    on_sighup(SIGHUP);
    keyboard_signals();
    assert(caps_locked());
    puts("PASS the layouts made again (settings' reload) keep the locks");
    int two[2] = { dup(kbds[0]), extra[0] };
    len = snprintf(msg, sizeof msg, "keys%c%d", 0, 0);
    assert(control_send(link[1], msg, (size_t)len, two, 2) == 0);
    close(two[0]);
    keyboards_message();
    assert(nkbds == 2 && caps_locked());
    puts("PASS a keyboard added while the bar holds the keyboards: its own Caps Lock stands");
    assert(control_send(link[1], "keys", 4, NULL, 0) == 0);
    keyboards_message();
    len = snprintf(msg, sizeof msg, "keys%c%d", 0, KEYS_LOCK_NUM);
    assert(control_send(link[1], msg, (size_t)len, &board[0], 1) == 0);
    keyboards_message();
    assert(nkbds == 1 && !caps_locked() && mod(0, XKB_MOD_NAME_NUM, XKB_STATE_MODS_LOCKED));
    puts("PASS the seat taken back: the bar starts again from the locks given (Num Lock only)");
    take_keyboards(NULL, 0);
    keys_link = -1;
    close(link[0]); close(link[1]);
    close(board[0]); close(board[1]); close(extra[0]); close(extra[1]);
    ime_done();
    for (int l = 0; l < nlayouts; l++) layout_free(&layouts[l]);
    xkb_context_unref(xkb_ctx);
    close(events[1]); /* events[0]: closed by take_keyboards */
    assert(unlink(system_table) == 0);
    if (!integration) test_exec_in_place();
    return 0;
}
