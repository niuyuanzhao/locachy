/* Isolated settings UI: fixture sysfs, synthetic keys/pointer, no devices.
 * Build with -ffunction-sections -fdata-sections -Wl,--gc-sections.
 * test-settings-monitors DIR
 * test-settings-monitors --fixture-dir DIR --place right|left|above|below
 * The latter writes DIR/monitors.conf through the actual UI event loop. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static FILE *fixture_fopen(const char *, const char *);
static int fixture_access(const char *, int);
static pid_t fixture_fork(void);
static pid_t fixture_waitpid(pid_t, int *, int);
static DIR *fixture_opendir(const char *);
static int fixture_rename(const char *, const char *);
static int fixture_unlink(const char *);
static int fixture_fclose(FILE *);
static int fixture_kill(pid_t, int);
static int fixture_sethostname(const char *, size_t);
static struct passwd *fixture_getpwuid(uid_t);
#define getpwuid fixture_getpwuid
#define fopen fixture_fopen
#define opendir fixture_opendir
#define rename fixture_rename
#define unlink fixture_unlink
#define fclose fixture_fclose
#define kill fixture_kill
#define sethostname fixture_sethostname
#define access fixture_access
#define fork fixture_fork
#define waitpid fixture_waitpid
#define main settings_main
#define SETTINGS_COMMAND_OUTPUT
#define PACMAN_LOCAL "/fixture/pacman-local"
#include "../progs/settings.c"
#include "../base/ui.c"
#include "i18n-stub.h"
#undef main
#undef waitpid
#undef fork
#undef access
#undef sethostname
#undef kill
#undef fclose
#undef unlink
#undef rename
#undef opendir
#undef fopen
#undef getpwuid

static struct passwd *fixture_getpwuid(uid_t uid)
{
    static struct passwd user = { .pw_name = "testuser", .pw_uid = 1000, .pw_gid = 1000, .pw_dir = "/home/testuser" };
    return uid == 1000 ? &user : NULL;
}

static char root[1024];
static int fail_open, fail_close, fail_rename;
static FILE *writing;
static int test_cols = 100, test_rows = 35, frames, settings_page;
int screen_is_extended(void) { return 0; }
int screen_output_current(void) { return 0; }
static char canvas[100][256];
static uint32_t colors[100][256];
static int keys[64], nkeys, keypos;
static struct pointer_event mouse;
static char snapshot[32];
static const char *camera_listing = "", *pending_command;
static int camera_commands;

static void command_output(char *const argv[], char *buf, size_t size)
{
    buf[0] = '\0';
    if (strcmp(argv[0], FACE_CAMERA_TOOL) == 0) {
        assert(argv[1] && !strcmp(argv[1], "list") && argv[2] == NULL);
        camera_commands++;
        snprintf(buf, size, "%s", camera_listing);
    } else {
        pending_command = argv[0];
        pid_t pid = fixture_fork();
        if (pid > 0) {
            fixture_kill(pid, SIGKILL);
            fixture_waitpid(pid, NULL, 0);
        }
        pending_command = NULL;
    }
}

static const char *map_path(const char *path, char *out, size_t size)
{
    if (strcmp(path, "/sys/class/drm") == 0)
        snprintf(out, size, "%s/drm", root);
    else if (strncmp(path, "/sys/class/drm/", 15) == 0)
        snprintf(out, size, "%s/drm/%s", root, path + 15);
    else if (strcmp(path, MONITORS_CONF) == 0)
        snprintf(out, size, "%s/monitors.conf", root);
    else if (strcmp(path, MONITORS_CONF ".tmp") == 0)
        snprintf(out, size, "%s/monitors.conf.tmp", root);
    else if (strcmp(path, SUDO_PAM) == 0 || strcmp(path, SUDO_PAM ".tmp") == 0)
        snprintf(out, size, "%s/sudo%s", root, path + strlen(SUDO_PAM));
    else if (strcmp(path, PACMAN_LOCAL) == 0)
        snprintf(out, size, "%s/pacman-local", root);
    else if (strcmp(path, KERNEL_MANIFEST) == 0)
        snprintf(out, size, "%s/packages", root);
    else if (strncmp(path, "/home/", 6) == 0 && strstr(path, "/.config/rclone/rclone.conf"))
        snprintf(out, size, "%s/rclone.conf", root);
    else if (strcmp(path, HOWDY_CONF) == 0)
        snprintf(out, size, "%s/howdy.ini", root);
    else if (strcmp(path, "/etc/locachy/labwc/rc.xml") == 0)
        snprintf(out, size, "%s/absent-labwc.xml", root);
    else if (strcmp(path, CLASH_STATE "/subscription") == 0)
        snprintf(out, size, "%s/subscription", root);
    else if (strcmp(path, LOCALE_DIR) == 0) /* the Language row: no catalog here, English only */
        snprintf(out, size, "%s/locale", root);
    else if (strcmp(path, MACHINES_DIR) == 0)
        snprintf(out, size, "%s/machines", root);
    else if (strncmp(path, MACHINES_DIR "/", sizeof MACHINES_DIR) == 0)
        snprintf(out, size, "%s/machines/%s", root, path + sizeof MACHINES_DIR);
    else if (strcmp(path, "/etc/hostname") == 0 || strcmp(path, "/etc/hostname.tmp") == 0)
        snprintf(out, size, "%s/hostname%s", root, path + 13);
    else if (strcmp(path, "/etc/hosts") == 0 || strcmp(path, "/etc/hosts.tmp") == 0)
        snprintf(out, size, "%s/hosts%s", root, path + 10);
    else if (strcmp(path, NM_MAC_CONF) == 0 || strcmp(path, NM_MAC_CONF ".new") == 0)
        snprintf(out, size, "%s/nm-mac.conf%s", root, path + strlen(NM_MAC_CONF));
    else if (strcmp(path, "/etc/locale.conf") == 0 || strcmp(path, "/etc/locale.conf.tmp") == 0)
        snprintf(out, size, "%s/locale.conf%s", root, path + 16);
    else if (strcmp(path, LOCAL_QUIRKS) == 0 || strcmp(path, LOCAL_QUIRKS ".tmp") == 0)
        snprintf(out, size, "%s/local-overrides.quirks%s", root, path + strlen(LOCAL_QUIRKS));
    else if (strcmp(path, "/proc") == 0 || strncmp(path, "/proc/", 6) == 0) /* the programs told */
        snprintf(out, size, "%s/proc%s", root, path + 5);
    else { fprintf(stderr, "Unexpected file access: %s\n", path); abort(); }
    return out;
}

static FILE *fixture_fopen(const char *path, const char *mode)
{
    char mapped[1400];
    map_path(path, mapped, sizeof mapped);
    if (mode[0] == 'w' && fail_open) { errno = EACCES; return NULL; }
    FILE *f = fopen(mapped, mode);
    if (mode[0] == 'w') writing = f;
    return f;
}

static DIR *fixture_opendir(const char *path)
{
    char mapped[1400];
    return opendir(map_path(path, mapped, sizeof mapped));
}

static int fixture_rename(const char *from, const char *to)
{
    char a[1400], b[1400];
    map_path(from, a, sizeof a);
    map_path(to, b, sizeof b);
    if (fail_rename) { errno = EACCES; return -1; }
    return rename(a, b);
}

static int fixture_unlink(const char *path)
{
    char mapped[1400];
    return unlink(map_path(path, mapped, sizeof mapped));
}

static int fixture_fclose(FILE *f)
{
    int output = f == writing;
    if (output) writing = NULL;
    int result = fclose(f);
    return output && fail_close ? EOF : result;
}

/* Service rows (allow_rc): rc.conf's lines written, process 1 told,
 * packages "installed" by flags, locachy-pkg and nmcli not run. */
static int allow_rc, hups, bar_hups, forks, fake_tailscale, fake_mihomo, fake_fprintd, fake_nvidia;
static int fake_bluetooth, fake_rime, fake_rime_zh, fake_opt, fake_howdy, fail_rc, term_calls, term_status, track_events;
static int face_term_calls, tailscale_term_calls;
static char face_term_args[512];
static char rc_log[1024], events[32];

static void event(char what)
{
    if (!track_events) return;
    size_t n = strlen(events);
    assert(n + 1 < sizeof events);
    events[n] = what;
    events[n + 1] = '\0';
}

static int fixture_sethostname(const char *name, size_t len)
{
    (void)name;
    (void)len;
    return 0;
}

static int fixture_kill(pid_t pid, int signal)
{
    if (allow_rc && pending_command && signal == SIGKILL) return 0;
    if (allow_rc && pid == 4242 && signal == SIGHUP) { /* the fixture's locachy-bar */
        bar_hups++;
        return 0;
    }
    if (!allow_rc || pid != 1 || signal != SIGHUP) abort();
    event('H');
    hups++;
    return 0;
}

static int fixture_access(const char *path, int mode)
{
    if (!strcmp(path, "/usr/bin/tailscaled") || !strcmp(path, "/usr/bin/tailscale")) return fake_tailscale ? 0 : -1;
    if (!strcmp(path, "/usr/bin/mihomo")) return fake_mihomo ? 0 : -1;
    if (!strcmp(path, "/usr/lib/bluetooth/bluetoothd")) return fake_bluetooth & 1 ? 0 : -1;
    if (!strcmp(path, "/usr/bin/bluetoothctl")) return fake_bluetooth & 2 ? 0 : -1;
    if (!strcmp(path, "/usr/bin/bluetui")) return fake_bluetooth & 4 ? 0 : -1;
    if (!strcmp(path, "/usr/lib/fprintd") || !strcmp(path, "/usr/bin/fprintd-enroll")) return fake_fprintd ? 0 : -1;
    if (!strcmp(path, "/usr/lib/librime.so") || !strcmp(path, "/usr/bin/rime_deployer")) return fake_rime ? 0 : -1;
    if (!strcmp(path, "/usr/share/rime-data/rime_frost.schema.yaml")) return fake_rime_zh ? 0 : -1;
    if (!strcmp(path, HOWDY) || !strcmp(path, FACE_CAMERA_TOOL)) return fake_howdy ? 0 : -1;
    if (!strcmp(path, "/usr/local/bin/locachy-tpm")) return -1;
    if (!strcmp(path, NVIDIA_DRIVER)) return fake_nvidia ? 0 : -1;
    if (!strcmp(path, "/etc/libinput")) return 0; /* its file is the fixture's */
    for (int i = 0; i < NOPT_MODULES; i++) {
        if (!strcmp(path, opt_modules[i].probe)) return (fake_opt & (1 << i)) ? 0 : -1;
    }
    return access(path, mode);
}

/* Elsewhere (load_nets' nmcli in the UI loop): no process, no networks. */
static pid_t fixture_fork(void)
{
    if (!allow_rc && pending_command) return -1;
    event('F');
    return allow_rc ? 4000 + ++forks : -1;
}

static pid_t fixture_waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    if (status) *status = 0;
    return pid;
}

static int rescales;
int screen_rescale(void) { rescales++; return 0; }
/* rc.conf: the X230's panel is on DP-3 (monitors.c, the real one, reads it). */
static const char *rc_internal = "DP-3";
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    if (rc_internal[0]) fn("internal_screen", rc_internal, arg);
    return 0;
}
#include "../base/monitors.c"
/* adjust() reaches these for other rows only; the screen rows must not. */
int rc_set(const char *name, const char *value)
{
    if (!allow_rc) abort();
    event('R');
    if (fail_rc) return -1;
    size_t n = strlen(rc_log);
    snprintf(rc_log + n, sizeof rc_log - n, "%s=%s;", name, value ? value : "(none)");
    return 0;
}
int base_call(char *const argv[])
{
    assert(!strcmp(argv[0], "/usr/local/bin/term"));
    if (!strcmp(argv[1], FACE_CAMERA_TOOL)) {
        face_term_args[0] = '\0';
        for (int i = 2; argv[i]; i++) {
            size_t n = strlen(face_term_args);
            snprintf(face_term_args + n, sizeof face_term_args - n, "%s%s", n ? " " : "", argv[i]);
        }
        face_term_calls++;
        return term_status;
    }
    if (!strcmp(argv[1], "/usr/bin/tailscale")) {
        assert(!strcmp(argv[2], "up") && !strcmp(argv[3], "--qr") && argv[4] == NULL);
        tailscale_term_calls++;
        return term_status;
    }
    assert(!strcmp(argv[1], "/usr/bin/bluetui") && argv[2] == NULL);
    term_calls++;
    return term_status;
}
double uptime(void) { abort(); }
int open_candidates(const char *type, char names[][OPEN_NAME], int max)
{ (void)type; (void)names; (void)max; abort(); }
int open_set_rule(const char *type, const char *value) { (void)type; (void)value; abort(); }
int open_containers(char names[][OPEN_NAME], int max) { (void)names; (void)max; abort(); }
int open_host(const char *name) { return strcmp(name, "foot") == 0; }
void open_label(const char *name, const char *type, char *label, size_t size)
{ (void)name; (void)type; (void)label; (void)size; abort(); }
int screen_cols(void) { return test_cols; }
int screen_char_cells(uint32_t cp) { return cp >= 0x2E80 && cp <= 0xA4CF ? 2 : 1; }
int screen_rows(void) { return test_rows; }
void screen_cell_at(int x, int y, int *col, int *row) { *col = x; *row = y; }
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
void screen_fill(int x, int y, int w, int h, uint32_t color)
{
    for (int row = y; row < y + h; row++) for (int col = x; col < x + w; col++) {
        if (row < 0 || row >= test_rows || col < 0 || col >= test_cols) continue;
        canvas[row][col] = ' ';
        colors[row][col] = color;
    }
}
void screen_text(int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    (void)fg;
    if (y < 0 || y >= test_rows) return;
    for (int i = 0; text[i] && x + i < test_cols; i++) {
        if (x + i < 0) continue;
        canvas[y][x + i] = text[i];
        colors[y][x + i] = bg;
    }
}
static int canvas_has(const char *text)
{
    size_t len = strlen(text);
    for (int row = 0; row < test_rows; row++) {
        canvas[row][test_cols] = '\0';
        for (int col = 0; col + (int)len <= test_cols; col++) {
            size_t i = 0;
            for (; i < len; i++) {
                if (canvas[row][col + i] != text[i]) break;
            }
            if (i == len) return 1;
        }
    }
    return 0;
}
void screen_flush(void)
{
    frames++;
    if (!settings_page) {
        assert(canvas_has("[Left]") && canvas_has("[Right]"));
        assert(canvas_has("[Above]") && canvas_has("[Below]"));
        assert(canvas_has("Esc: back"));
    }
    if (snapshot[0]) {
        char path[1400];
        snprintf(path, sizeof path, "%s/%s.txt", root, snapshot);
        FILE *f = fopen(path, "w");
        assert(f);
        for (int i = 0; i < test_rows; i++) {
            fwrite(canvas[i], 1, (size_t)test_cols, f);
            fputc('\n', f);
        }
        assert(fclose(f) == 0);
    }
}
/* The next key of keys[]; -2 is a pointer event: mouse, as K_POINTER. */
const struct pointer_event *pointer_last(void) { return &mouse; }
int key_poll_timeout(int fd, int timeout)
{
    (void)fd; (void)timeout;
    assert(keypos < nkeys);
    int key = keys[keypos++];
    return key == -2 ? K_POINTER : key;
}

static void file_text(const char *relative, const char *text)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", root, relative);
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0 && fclose(f) == 0);
}

static void connector(const char *name, const char *status, const char *modes)
{
    char path[1400], relative[128];
    snprintf(path, sizeof path, "%s/drm/card9-%s", root, name);
    assert(mkdir(path, 0700) == 0 || errno == EEXIST);
    snprintf(relative, sizeof relative, "drm/card9-%s/status", name);
    file_text(relative, status);
    snprintf(relative, sizeof relative, "drm/card9-%s/modes", name);
    file_text(relative, modes);
}

static int monitor_index(const char *name)
{
    for (int i = 0; i < nmonitors; i++) if (!strcmp(name, monitors[i].name)) return i;
    return -1;
}

/* The screen row of setting idx on the page last drawn, or -1 when it is not shown. */
static int item_row(int idx)
{
    int k = entry_of(idx);
    return k < page_scroll || k >= page_scroll + page_rows() ? -1 : PAGE_TOP + k - page_scroll;
}

static void check_geometry(int place)
{
    struct monitor_rect rects[MAX_MONITORS];
    int indices[MAX_MONITORS], n = arrangement_rects(rects, indices), a = -1, b = -1;
    for (int i = 0; i < n; i++) {
        if (!strcmp(rects[i].name, "HEADLESS-1")) a = i;
        if (!strcmp(rects[i].name, "HEADLESS-2")) b = i;
    }
    assert(a >= 0 && b >= 0);
    assert(rects[a].width == 1024 && rects[a].height == 576);
    assert(rects[b].width == 853 && rects[b].height == 480);
    if (place == MONITOR_RIGHT) assert(rects[b].x == rects[a].width && rects[b].y == 0);
    if (place == MONITOR_LEFT) assert(rects[a].x == rects[b].width && rects[b].x == 0);
    if (place == MONITOR_ABOVE) assert(rects[a].y == rects[b].height && rects[b].y == 0);
    if (place == MONITOR_BELOW) assert(rects[b].y == rects[a].height && rects[b].x == 0);
}

static void enter_direction(int place)
{
    int b = monitor_index("HEADLESS-2");
    assert(b >= 0);
    nkeys = keypos = 0;
    int current = monitors[b].place;
    for (int i = 0; i < (place - current + 4) % 4; i++) keys[nkeys++] = K_RIGHT;
    keys[nkeys++] = '\n';
    keys[nkeys++] = 27;
    selected = num_hw_items();
    int before = frames;
    assert(screen_settings_key('\n') == 1);
    assert(keypos == nkeys && frames > before);
}

static void prepare(void)
{
    char path[1400];
    assert(mkdir(root, 0700) == 0 || errno == EEXIST);
    snprintf(path, sizeof path, "%s/drm", root);
    assert(mkdir(path, 0700) == 0 || errno == EEXIST);
    connector("HEADLESS-1", "connected\n", "1280x720\n1920x1080\n");
    connector("HEADLESS-2", "connected\n", "1280x720\n1920x1080\n");
    file_text("monitors.conf", "HEADLESS-1=origin,1280x720,1.25\nHEADLESS-2=right,1280x720,1.5\n");
    load_monitors();
    assert(nmonitors == 2 && monitors[monitor_anchor()].place == MONITOR_ORIGIN);
}

static void quirks_text(char *text, size_t size)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/local-overrides.quirks", root);
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(text, 1, size - 1, f) : 0;
    text[n] = '\0';
    if (f) fclose(f);
}

/* Touchpad Circular Scrolling: rc.conf's line and a section of libinput's file
 * for local quirks, the rest of the file kept; round touchpads (none written),
 * on, off, round again; the file goes when only the section was in it. */
static void circular_step(int dir, const char *rc, const char *note, const char *theirs, const char *section)
{
    char text[1024], expected[1024];
    rc_log[0] = '\0';
    adjust(dir);
    quirks_text(text, sizeof text);
    snprintf(expected, sizeof expected, "%s\n%s", theirs, section);
    assert(strstr(rc_log, rc) && strstr(notice, note) && !strcmp(text, expected));
}

static void test_circular(void)
{
    static const char theirs[] = "[A tablet of theirs]\nMatchName=*Tablet*\nAttrPressureRange=10:8\n";
#define CIRCULAR_TEXT(value) "[LOCachy touchpad circular scrolling]\n" \
    "# settings: Touchpad Circular Scrolling (rc.conf touchpad_circular)\n" \
    "MatchUdevType=touchpad\nModelCircularTouchpad=" value "\n"
    static const char on[] = CIRCULAR_TEXT("1"), off[] = CIRCULAR_TEXT("0");
    char text[1024], path[1400];
    snprintf(path, sizeof path, "%s/local-overrides.quirks", root);
    assert(circ_index() == tnet_index() + 1 && imek_index() == circ_index() + 3 &&
           kb_index() == imek_index() + NIME_KEYS && num_hw_items() == kb_index() + nkeyboards_seen);
    on_rc("touchpad_circular", "0", NULL);
    assert(circular == 0);
    on_rc("touchpad_circular", "yes", NULL);
    assert(circular == -1);
    assert(apply_circular() == 0); /* as at boot (settings --apply) */
    quirks_text(text, sizeof text);
    assert(!text[0]); /* round touchpads: no file made */
    file_text("local-overrides.quirks", theirs);
    selected = circ_index();
    circular_step(+1, "touchpad_circular=1;", "Every touchpad scrolls around", theirs, on);
    assert(apply_circular() == 0); /* at boot: the same file */
    quirks_text(text, sizeof text);
    assert(strstr(text, on));
    circular_step(+1, "touchpad_circular=0;", "two fingers", theirs, off);
    circular_step(+1, "touchpad_circular=(none);", "Round touchpads", theirs, "");
    circular_step(-1, "touchpad_circular=0;", "two fingers", theirs, off); /* no second blank line */
    circular_step(-1, "touchpad_circular=1;", "Every touchpad", theirs, on);
    file_text("local-overrides.quirks", on);
    adjust(-1);
    assert(circular == -1 && access(path, F_OK) != 0);
    int page_before = settings_page;
    settings_page = 1;
    in_page = 1;
    draw();
    assert(category == CAT_INPUT && canvas_has("Mouse & Touchpad") && canvas_has("Touchpad Circular Scrolling") &&
           canvas_has("< round touchpads >"));
    in_page = 0;
    settings_page = page_before;
    assert(circular_width == 25 && circular_delay == 300);
    selected = circ_index() + 1;
    adjust(-1);
    assert(circular_width == 24 && circular_width_set);
    quirks_text(text, sizeof text);
    assert(strstr(text, "AttrCircularScrollRingWidth=24") && !strstr(text, "ModelCircularTouchpad="));
    selected = circ_index() + 2;
    adjust(+1);
    assert(circular_delay == 350 && circular_delay_set);
    quirks_text(text, sizeof text);
    assert(strstr(text, "AttrCircularScrollDwell=350") && strstr(text, "AttrCircularScrollRingWidth=24"));
    selected = circ_index() + 1;
    fail_open = 1;
    adjust(+1);
    fail_open = 0;
    assert(circular_width == 24 && circular_width_set && strstr(notice, "Cannot write"));
    circular_width = 25; circular_delay = 300;
    circular_width_set = circular_delay_set = 0;
    assert(apply_circular() == 0);
    puts("PASS touchpad circular scrolling: round touchpads (none written), on, off; a section of libinput's "
         "local quirks, the rest kept; width and dwell independently adjustable");
}

/* key_get for choose_key: keys[] with the modifiers held at each and the
 * name keys_last_name gives it (NULL: none), as keys.c has them; the notice
 * as each key is asked for (what the one before it got). */
static int key_mods[64], key_at = -1, name_any;
static const char *key_names[64];
static char notices[64][sizeof notice];
int key_get(void)
{
    key_at = keypos;
    snprintf(notices[keypos], sizeof notices[0], "%s", notice);
    return key_poll_timeout(-1, -1);
}
int keys_modifiers(void) { return key_at >= 0 ? key_mods[key_at] : 0; }
int keys_last_name(char *out, size_t size)
{
    if (key_at < 0 || !key_names[key_at]) return -1;
    snprintf(out, size, "%s", key_names[key_at]);
    return 0;
}
void keys_name_any(int on) { name_any = on; }
/* K_TEXT's text: a character beyond ASCII (keys.c's, from a de or fr table). */
static const char *key_text = "é";
const char *keys_text(void) { return key_text; }
static void press(int key, int mods, const char *name)
{
    assert(nkeys < 64);
    key_mods[nkeys] = mods;
    key_names[nkeys] = name;
    keys[nkeys++] = key;
}

/* The input method's keys: rc.conf's or keys.c's defaults; recorded alone
 * when they type nothing (one bound to nothing too), not another setting's,
 * not a letter alone; saved, process 1 and the shared display's bar told. */
static void test_ime_keys(void)
{
    char dir[1400];
    snprintf(dir, sizeof dir, "%s/proc", root);
    assert(mkdir(dir, 0700) == 0);
    snprintf(dir, sizeof dir, "%s/proc/4242", root);
    assert(mkdir(dir, 0700) == 0);
    snprintf(dir, sizeof dir, "%s/proc/4243", root);
    assert(mkdir(dir, 0700) == 0);
    file_text("proc/4242/comm", "locachy-bar\n");
    file_text("proc/4243/comm", "labwc\n"); /* not told: rc.conf's keys are not its */
    assert(!strcmp(ime_keys[0], "Ctrl+Space") && !strcmp(ime_keys[1], "Henkan_Mode") &&
           !strcmp(ime_keys[2], "Muhenkan") && !strcmp(ime_keys[3], "Zenkaku_Hankaku"));
    on_rc("key_ime_width", "Shift+Space", NULL);
    assert(!strcmp(ime_keys[3], "Shift+Space"));
    on_rc("key_ime_width", "Zenkaku_Hankaku", NULL);
    int page_before = settings_page, hups_before = hups;
    settings_page = 1;
    rc_log[0] = '\0';
    nkeys = keypos = 0;
    press('a', 0, "A");                              /* a letter alone */
    press(K_ACT_IME_RAW, 0, "Muhenkan");             /* direct input's */
    press(K_ACT_LOCK, KEYS_META, "Super+L");         /* the lock's */
    press(K_NAMED, 0, "Hiragana_Katakana");          /* bound to nothing */
    choose_key(_(ime_key_labels[1]), ime_key_names[1], ime_keys[1], sizeof ime_keys[1], 1, reread_ime_keys);
    assert(keypos == nkeys && !name_any);
    assert(strstr(notices[0], "Chinese input key: press it, alone if it types nothing (Henkan_Mode)"));
    assert(strstr(notices[1], "A key that types nothing (Henkan_Mode), or Ctrl, Alt or Super"));
    assert(strstr(notices[2], "That key is another of rc.conf's") && strstr(notices[3], "another of rc.conf's"));
    assert(!strcmp(notice, "Chinese input key: Hiragana_Katakana, saved and applied."));
    assert(!strcmp(ime_keys[1], "Hiragana_Katakana") && !strcmp(rc_log, "key_ime_cn=Hiragana_Katakana;"));
    assert(hups == hups_before + 1 && bar_hups == 1);

    /* It is the Chinese key's now; the lock's row takes no bare key. */
    nkeys = keypos = 0;
    press(K_NAMED, 0, "Hiragana_Katakana");
    press(K_ACT_IME_RAW, 0, "Muhenkan");             /* its own */
    choose_key(_(ime_key_labels[2]), ime_key_names[2], ime_keys[2], sizeof ime_keys[2], 1, reread_ime_keys);
    assert(strstr(notices[1], "another of rc.conf's") && !strcmp(ime_keys[2], "Muhenkan"));
    nkeys = keypos = 0;
    press(K_NAMED, 0, "Henkan_Mode");
    press(27, 0, NULL);
    choose_key(_("Lock key"), "key_lock", lock_key, sizeof lock_key, 0, reread_keys);
    assert(strstr(notices[1], "Ctrl, Alt or Super with a letter, digit, Space, Enter or Tab") &&
           !strstr(notices[1], "types nothing"));
    assert(!strcmp(notice, "Lock key left as it is.") && !strcmp(lock_key, "Super+L") && !name_any);
    assert(!strcmp(rc_log, "key_ime_cn=Hiragana_Katakana;key_ime_raw=Muhenkan;") && bar_hups == 2);

    selected = imek_index() + 1;
    in_page = 1;
    draw();
    assert(category == CAT_INPUT && canvas_has("Chinese input key") && canvas_has("Hiragana_Katakana") && canvas_has("Punctuation width key"));
    in_page = 0;
    settings_page = page_before;
    snprintf(ime_keys[1], sizeof ime_keys[1], "Henkan_Mode");
    key_at = -1;
    nkeys = keypos = 0;
    puts("PASS input method keys: Henkan_Mode, Muhenkan, Zenkaku_Hankaku unless rc.conf says; a key that types "
         "nothing alone (bound to nothing too), not another setting's, not a letter alone; the bar told");
}

/* keys.c's keyboards (doubles): the built-in one types the system's keymap,
 * a USB one its own; what keyboards.conf was given. */
static struct keys_keyboard fake_keyboards[2] = {
    { .id = "1:1:AT_Translated_Set_2_keyboard", .name = "AT Translated Set 2 keyboard", .keymap = "jp106" },
    { .id = "1133:49948:USB_Keyboard", .name = "USB Keyboard", .keymap = "us", .own = 1 },
};
static char keyboard_log[256];
static int keys_reloads;
int keys_keyboards(struct keys_keyboard *out, int max)
{
    int n = max < 2 ? max : 2;
    memcpy(out, fake_keyboards, (size_t)n * sizeof *out);
    return n;
}
int keys_set_keyboard(const char *id, const char *keymap)
{
    size_t n = strlen(keyboard_log);
    snprintf(keyboard_log + n, sizeof keyboard_log - n, "%s=%s;", id, keymap ? keymap : "");
    for (int i = 0; i < 2; i++) {
        if (strcmp(fake_keyboards[i].id, id) != 0) continue;
        fake_keyboards[i].own = keymap != NULL;
        snprintf(fake_keyboards[i].keymap, sizeof fake_keyboards[i].keymap, "%s", keymap ? keymap : "jp106");
    }
    return 0;
}
void keys_reload(void) { keys_reloads++; }
const char *keys_layout_name(const char *field) { return strcmp(field, "console") == 0 ? "jp106" : NULL; }

/* A row for each keyboard with the keymap it types in; one of its own, or
 * the system's again (Delete, or the system's chosen); process 1 and the
 * shared display's bar told, this program's keys read again. */
static void test_keyboards(void)
{
    int page_before = settings_page, hups_before = hups, bar_before = bar_hups;
    settings_page = 1;
    selected = kb_index() + 1;
    in_page = 1;
    draw();
    assert(nkeyboards_seen == 2 && category == CAT_INPUT);
    assert(canvas_has("AT Translated Set 2 keyboard") && canvas_has("jp106, the system's"));
    assert(canvas_has("USB Keyboard") && canvas_has("us, its own"));
    set_keyboard_keymap(&keyboards_seen[1], NULL);
    assert(!strcmp(keyboard_log, "1133:49948:USB_Keyboard=;") && keys_reloads == 1);
    assert(hups == hups_before + 1 && bar_hups == bar_before + 1);
    assert(!strcmp(notice, "USB Keyboard: the system's layout again."));
    draw();
    assert(canvas_has("USB Keyboard") && !canvas_has("us, its own"));
    set_keyboard_keymap(&keyboards_seen[1], "us"); /* its table made (keytable), then keyboards.conf */
    assert(strstr(keyboard_log, "1133:49948:USB_Keyboard=us;") && keys_reloads == 2);
    assert(!strcmp(notice, "USB Keyboard: layout us from now on; the boot unlock takes it from the next boot image."));
    set_keyboard_keymap(&keyboards_seen[0], "jp106"); /* the system's: none of its own */
    assert(strstr(keyboard_log, "1:1:AT_Translated_Set_2_keyboard=;"));
    fake_keyboards[1].missing = 1;
    test_cols = 160;
    draw();
    assert(canvas_has("us, its own: no key table, types as the system's"));
    test_cols = 100;
    fake_keyboards[1].missing = 0;
    in_page = 0;
    settings_page = page_before;
    puts("PASS keyboards: a row each with the keymap it types in (the system's, its own, its own without a "
         "table); chosen or the system's again; process 1 and the bar told");
}

static void test_bluetooth(void)
{
    /* All three packages' required executables, not merely bluetui. */
    for (fake_bluetooth = 0; fake_bluetooth < 7; fake_bluetooth++) assert(!bluetooth_installed());
    assert(bluetooth_installed());
    bluetooth_on = 0;
    on_rc("bluetooth_enabled", "1", NULL);
    assert(!bluetooth_on);
    on_rc("service_bluetooth", BLUETOOTH_SERVICE, NULL);
    assert(bluetooth_on);
    bluetooth_on = 0;
    fake_bluetooth = fake_tailscale = fake_mihomo = 0;
    tailscale_enabled = clash_on = 0;
    settings_page = 1;
    /* Every row's placement, hit testing and highlight use the same indices,
     * both with and without the three extra TrackPoint rows. */
    for (tp_present = 0; tp_present < 2; tp_present++) {
        assert(tr_index() == cl_index() + 4 && bt_index() == tr_index() + 4 && nw_index() == bt_index() + 3);
        selected = bt_index();
        in_page = 1;
        notice[0] = '\0';
        draw();
        /* Its category's page (Connected Devices), under its group's title. */
        assert(category == CAT_DEVICES && canvas_has("Connected Devices") && canvas_has("Bluetooth"));
        assert(canvas_has("Bluetooth tools installed") && canvas_has("Bluetooth service"));
        assert(canvas_has("Bluetooth devices") && canvas_has("not installed") && !canvas_has("Tailscale"));
        assert(item_row(bt_index()) > PAGE_TOP && colors[item_row(bt_index())][text_x()] == ACCENT);
        assert(colors[item_row(bt_index())][0] != ACCENT); /* the sidebar is not selected: the page is */
        for (int i = 0; i < 3; i++) {
            mouse = (struct pointer_event){ .col = text_x() + 38, .row = item_row(bt_index() + i),
                                            .button = BTN_LEFT, .pressed = 1, .clicks = 1 };
            assert(item_at(mouse.row) == bt_index() + i);
            assert(settings_pointer() == (i == 2 ? -1 : K_RIGHT));
            mouse.clicks = 2;
            assert(settings_pointer() == (i == 2 ? '\n' : K_RIGHT));
        }
    }
    tp_present = 0;
    int forks_before = forks, hups_before = hups;
    track_events = 1;
    events[0] = rc_log[0] = '\0';
    selected = bt_index() + 1;
    adjust(+1);
    assert(!bluetooth_on && !events[0] && strstr(notice, "not installed"));
    selected = bt_index() + 2;
    assert(!bluetooth_settings_key(K_RIGHT) && bluetooth_settings_key('\n'));
    assert(term_calls == 0 && !bluetooth_on && strstr(notice, "not installed"));
    selected = bt_index();
    adjust(+1);
    assert(forks == forks_before + 1 && hups == hups_before && !bluetooth_on && !strcmp(events, "F"));
    assert(!strcmp(pkg_action, "install") && !strcmp(pkg_name, BLUETOOTH_PACKAGES));
    assert(!strcmp(pkg_title(), "Bluetooth tools") && !rc_log[0]);
    selected = bt_index() + 1;
    adjust(+1);
    assert(!bluetooth_on && strstr(notice, "Bluetooth tools is still being installed"));
    pkg_check();
    assert(!pkg_job && strstr(notice, "Bluetooth tools installed") && !strstr(notice, "Tailscale"));
    fake_bluetooth = 7;
    selected = bt_index() + 2;
    assert(bluetooth_settings_key('\n') && term_calls == 0 && !bluetooth_on && strstr(notice, "service is off"));
    draw();
    assert(canvas_has("service off"));
    selected = bt_index() + 1;
    fail_rc = 1;
    events[0] = '\0';
    adjust(+1);
    assert(!bluetooth_on && !strcmp(events, "R") && hups == hups_before && strstr(notice, "Cannot write"));
    fail_rc = 0;
    events[0] = '\0';
    adjust(+1);
    assert(bluetooth_on && !strcmp(events, "RH") && !strcmp(rc_log, "service_bluetooth=" BLUETOOTH_SERVICE ";"));
    selected = bt_index() + 2;
    draw();
    assert(canvas_has("Enter: open bluetui"));
    mouse = (struct pointer_event){ .col = text_x() + 38, .row = item_row(selected),
                                    .button = BTN_LEFT, .pressed = 1, .clicks = 2 };
    assert(bluetooth_settings_key(settings_pointer()) && term_calls == 1 && strstr(notice, "bluetui closed"));
    term_status = 7;
    assert(bluetooth_settings_key('\n') && term_calls == 2 && strstr(notice, "did not finish"));
    term_status = 0;
    events[0] = '\0';
    adjust(+1);
    assert(!events[0] && strstr(notice, "Enter: open Bluetooth"));
    /* Turning off also fails without changing memory or notifying process 1. */
    selected = bt_index() + 1;
    fail_rc = 1;
    hups_before = hups;
    adjust(-1);
    assert(bluetooth_on && hups == hups_before && !strcmp(events, "R"));
    fail_rc = 0;
    events[0] = rc_log[0] = '\0';
    adjust(-1);
    assert(!bluetooth_on && !strcmp(events, "RH") && !strcmp(rc_log, "service_bluetooth=(none);"));
    /* Removing always writes off, then notifies process 1, then starts the
     * package job. A failed write must abort all later steps. */
    for (int enabled = 0; enabled < 2; enabled++) {
        bluetooth_on = enabled;
        selected = bt_index();
        forks_before = forks;
        hups_before = hups;
        fail_rc = 1;
        events[0] = rc_log[0] = '\0';
        adjust(+1);
        assert(bluetooth_on == enabled && !pkg_job && forks == forks_before && hups == hups_before);
        assert(!strcmp(events, "R") && !rc_log[0] && strstr(notice, "Cannot write"));
        fail_rc = 0;
        events[0] = '\0';
        adjust(+1);
        assert(!bluetooth_on && !strcmp(events, "RHF") && !strcmp(rc_log, "service_bluetooth=(none);"));
        assert(forks == forks_before + 1 && hups == hups_before + 1);
        assert(!strcmp(pkg_action, "remove") && !strcmp(pkg_name, BLUETOOTH_PACKAGES));
        pkg_check();
        assert(!pkg_job && strstr(notice, "Bluetooth tools removed"));
    }
    /* A stale service can still be switched off if a tool has gone missing. */
    fake_bluetooth = 0;
    bluetooth_on = 1;
    selected = bt_index() + 1;
    adjust(-1);
    assert(!bluetooth_on);

    /* Optional modular packages (Audio, fwupd, flashrom, etc.): install/remove + rc sync. */
    fake_opt = 0;
    selected = opt_index();
    events[0] = rc_log[0] = '\0';
    adjust(+1);
    assert(!strcmp(pkg_action, "install") && strstr(pkg_name, "pipewire"));
    assert(strstr(rc_log, "service_audio=" AUDIO_SERVICE) && strstr(rc_log, "program_mixer=" MIXER_PROGRAM));
    pkg_check();
    fake_opt = 1;
    events[0] = rc_log[0] = '\0';
    adjust(+1);
    assert(!strcmp(pkg_action, "remove") && strstr(rc_log, "service_audio=(none)") && strstr(rc_log, "program_mixer=(none)"));
    pkg_check();
    fake_opt = 0;

    tailscale_login();
    assert(tailscale_term_calls == 1 && strstr(notice, "logged in"));
    term_status = 7;
    tailscale_login();
    assert(tailscale_term_calls == 2 && strstr(notice, "failed (status 7)"));
    term_status = 0;
    puts("PASS Tailscale login runs the original CLI in term, no QR output parsing or layout");

    /* Source updates: the repository and branch rows, as locachy-pkg takes them. */
    assert(valid_update_repo("https://example.org/me/locachy.git") && valid_update_repo("git@example.org:me/l.git"));
    assert(valid_update_repo("ssh://git@example.org/me/l.git"));
    assert(!valid_update_repo("file:///x") && !valid_update_repo("https://x y") && !valid_update_repo("https://x\"y"));
    assert(!valid_update_repo("https://x$(id)") && !valid_update_repo("-uhttps://x"));
    assert(valid_update_branch("main") && valid_update_branch("release/1.0") && valid_update_branch("x230_fix-2"));
    assert(!valid_update_branch("-x") && !valid_update_branch("a..b") && !valid_update_branch("x.lock"));
    assert(!valid_update_branch("x/") && !valid_update_branch("x//y") && !valid_update_branch("") &&
           !valid_update_branch("a b"));
    update_repo[0] = update_branch[0] = '\0';
    assert(upd_index() == mach_index() + 1);
    selected = upd_index() + 1; /* scrolled down to it: both rows shown */
    draw();
    assert(canvas_has("Update repository") && canvas_has("github.com/niuyuanzhao/locachy.git"));
    assert(canvas_has("Update branch") && canvas_has("main"));
    assert(enter_row(upd_index()) && enter_row(upd_index() + 1));
    on_rc("update_repo", "off", NULL);
    on_rc("update_branch", "next", NULL);
    selected = upd_index() + 1;
    draw();
    assert(canvas_has("next") && canvas_has("(updates off)"));
    events[0] = rc_log[0] = '\0';
    adjust(+1);
    assert(!rc_log[0] && strstr(notice, "Enter: the branch"));
    update_repo[0] = update_branch[0] = '\0';

    /* Boot: the kernel's messages and options in rc.conf, and the image made
     * at once (locachy-boot), as pacman's hooks make it after updates. */
    assert(boot_index() == upd_index() + 2 && opt_index() == boot_index() + 5);
    assert(valid_boot_cmdline("i915.enable_psr=0 mitigations=off") && valid_boot_cmdline(""));
    assert(!valid_boot_cmdline("a\"b") && !valid_boot_cmdline("$(id)") && !valid_boot_cmdline("a;b") &&
           !valid_boot_cmdline("a\nb") && !valid_boot_cmdline("a\\b"));
    boot_verbose = 0;
    boot_cmdline[0] = notice[0] = '\0';
    selected = boot_index() + 2;
    draw();
    assert(category == CAT_SYSTEM);
    char boot_line[512];
    item_text(boot_index(), boot_line, sizeof boot_line);
    assert(strstr(boot_line, "Boot messages") && strstr(boot_line, "hidden (quiet)"));
    item_text(boot_index() + 1, boot_line, sizeof boot_line);
    assert(strstr(boot_line, "Kernel options"));
    item_text(boot_index() + 2, boot_line, sizeof boot_line);
    assert(strstr(boot_line, "Boot image") && strstr(boot_line, "Enter: make it again"));
    assert(enter_row(boot_index() + 1) && enter_row(boot_index() + 2) && !enter_row(boot_index()));
    int forks_boot = forks;
    events[0] = rc_log[0] = '\0';
    selected = boot_index();
    adjust(+1);
    assert(boot_verbose && strstr(rc_log, "boot_verbose=1;") && forks == forks_boot + 1);
    assert(strstr(notice, "Boot image made"));
    adjust(-1);
    assert(!boot_verbose && strstr(rc_log, "boot_verbose=(none);") && forks == forks_boot + 2);
    on_rc("boot_cmdline", "i915.enable_psr=0", NULL);
    selected = boot_index() + 1;
    draw();
    assert(canvas_has("i915.enable_psr=0"));
    boot_cmdline[0] = '\0';

    /* Headers are declared in the manifest independently of installed state. */
    file_text("packages", "base\nlinux-cachyos-bore\n");
    load_kernel_state();
    assert(!kernel_headers_declared && !kernel_headers_installed);
    nkernel_choices = 2;
    kernel_choice = 0;
    snprintf(kernel_choices[0], sizeof kernel_choices[0], "linux-cachyos-bore");
    snprintf(kernel_choices[1], sizeof kernel_choices[1], "linux-lts");
    selected = boot_index() + 4;
    draw();
    assert(canvas_has("Kernel headers") && canvas_has("[OFF]"));
    forks_boot = forks;
    adjust(+1);
    assert(forks == forks_boot + 1 && !strcmp(pkg_action, "kernel") && !strcmp(pkg_name, "headers on"));
    pkg_job = 0;
    file_text("packages", "base\nlinux-cachyos-bore\nlinux-cachyos-bore-headers\n");
    load_kernel_state();
    assert(kernel_headers_declared && !kernel_headers_installed);
    draw();
    assert(canvas_has("[ON ]") && canvas_has("not installed"));
    adjust(-1);
    assert(!strcmp(pkg_action, "kernel") && !strcmp(pkg_name, "headers off"));
    pkg_job = 0;
    selected = boot_index() + 3;
    adjust(+1);
    assert(kernel_choice == 1 && !strcmp(kernel_choices[kernel_choice], "linux-lts"));
    assert(enter_row(selected));
    file_text("packages", "base\nlinux-cachyos-bore\n");
    puts("PASS kernel headers ON/OFF follows manifest, jobs use normal package control; kernel choice is separate");

    char machine_dir[1400];
    snprintf(machine_dir, sizeof machine_dir, "%s/machines", root);
    assert(mkdir(machine_dir, 0700) == 0 || errno == EEXIST);
    file_text("machines/lenovo-test.conf", "# Brand: Lenovo\ndescription=\"ThinkPad fixture\"\n");
    file_text("machines/panasonic-test.conf", "# Brand: Panasonic\ndescription=\"Let us note fixture\"\n");
    find_machines();
    assert(nmachines == 3 && !machine_profiles[0][0] && !strcmp(machine_brands[0], "Generic"));
    assert(!strcmp(machine_brands[1], "Lenovo") && !strcmp(machine_labels[1], "ThinkPad fixture"));
    selected = mach_index(); rc_log[0] = '\0';
    adjust(+1);
    assert(!rc_log[0] && enter_row(selected));
    int matches[] = { 0, 1, 2 };
    test_cols = 160; machine_grid(matches, 3); assert(machine_columns() == 5);
    assert(machine_cells[0].row < machine_cells[1].row && machine_cells[1].row < machine_cells[2].row);
    test_cols = 110; assert(machine_columns() == 4);
    test_cols = 24; assert(machine_columns() == 1);
    test_cols = 100;
    nkeys = keypos = 0; keys[nkeys++] = 'P'; keys[nkeys++] = 27;
    pick_machine();
    assert(!rc_log[0] && canvas_has("Panasonic") && !canvas_has("ThinkPad fixture"));
    nkeys = keypos = 0; keys[nkeys++] = 27;
    pick_machine();
    assert(canvas_has("generic PC") && canvas_has("[*]"));
    nkeys = keypos = 0; keys[nkeys++] = 'P'; keys[nkeys++] = 'a'; keys[nkeys++] = 'n'; keys[nkeys++] = '\n';
    pick_machine();
    assert(!strcmp(active_machine, "panasonic-test") && strstr(rc_log, "machine=panasonic-test;"));
    active_machine[0] = rc_log[0] = '\0';
    mouse = (struct pointer_event){ .col = 4, .row = 8, .button = BTN_LEFT, .pressed = 1, .clicks = 2 };
    nkeys = keypos = 0; keys[nkeys++] = -2;
    pick_machine();
    assert(!strcmp(active_machine, "lenovo-test") && strstr(rc_log, "machine=lenovo-test;"));
    active_machine[0] = '\0';
    puts("PASS machine profile Enter picker: generic retained, brand groups, search, responsive columns, pointer and apply/cancel");

    track_events = settings_page = 0;
    puts("PASS Bluetooth rows, clicks and Enter; all tools required, install separate from service, bluetui in term "
         "without arguments; remove writes off and reloads before packages, failed writes abort");
}

int main(int argc, char **argv)
{
    int generated = argc == 5 && strcmp(argv[1], "--fixture-dir") == 0 && strcmp(argv[3], "--place") == 0;
    assert(generated || argc == 2);
    snprintf(root, sizeof root, "%s", argv[generated ? 2 : 1]);
    assert(root[0] == '/');
    prepare();
    if (generated) {
        int place = place_index(argv[4]);
        assert(place <= MONITOR_BELOW && !strcmp(places[place], argv[4]));
        snprintf(snapshot, sizeof snapshot, "%s", argv[4]);
        enter_direction(place);
        load_monitors();
        assert(monitors[monitor_index("HEADLESS-2")].place == place);
        check_geometry(place);
        printf("PASS UI generated %s/monitors.conf (%s)\n", root, argv[4]);
        return 0;
    }
    for (int p = MONITOR_RIGHT; p <= MONITOR_BELOW; p++) {
        snprintf(snapshot, sizeof snapshot, "%s", places[p]);
        enter_direction(p);
        load_monitors();
        assert(monitors[monitor_index("HEADLESS-2")].place == p);
        check_geometry(p);
    }
    int frame_before = frames;
    keys[0] = 27; nkeys = 1; keypos = 0;
    selected = num_hw_items();
    assert(screen_settings_key('\n') && frames > frame_before);
    assert(arrangement_button == MONITOR_BELOW && canvas_has("Below of HEADLESS-1"));
    puts("PASS key entry, four direction buttons, saved reload, reopen and fractional geometry");
    int b = monitor_index("HEADLESS-2");
    int previous = monitors[b].place;
    assert(canvas_has("[Below]"));
    int highlighted = 0;
    for (int y = 0; y < test_rows; y++) for (int x = 0; x < test_cols; x++)
        highlighted += colors[y][x] == ACCENT;
    assert(highlighted > 100);
    for (int failure = 0; failure < 3; failure++) {
        fail_open = failure == 0; fail_close = failure == 1; fail_rename = failure == 2;
        enter_direction(MONITOR_RIGHT);
        assert(monitors[b].place == previous && strstr(notice, "Cannot save"));
        fail_open = fail_close = fail_rename = 0;
        load_monitors();
        assert(monitors[monitor_index("HEADLESS-2")].place == previous);
    }
    puts("PASS open, close and rename failures preserve configuration and in-memory placement");
    arrangement_selected = b;
    arrangement_button = MONITOR_LEFT;
    arrangement_draw();
    for (int i = 0; i < narrangement_hits; i++) {
        struct arrangement_hit h = arrangement_hits[i];
        if (h.button != MONITOR_LEFT) continue;
        mouse = (struct pointer_event){ .col = h.x, .row = h.y, .button = BTN_LEFT, .pressed = 1, .clicks = 1 };
    }
    nkeys = 2; keypos = 0; keys[0] = -2; keys[1] = 27;
    selected = num_hw_items();
    assert(screen_settings_key('\n'));
    load_monitors();
    assert(monitors[monitor_index("HEADLESS-2")].place == MONITOR_LEFT);
    assert(arrangement_pointer_seen);
    assert(!canvas_has("+")); /* the screen draws the cursor, not the page */
    struct monitor_rect hit_rects[MAX_MONITORS];
    int hit_indices[MAX_MONITORS], hit_n = arrangement_rects(hit_rects, hit_indices);
    arrangement_selected = monitor_index("HEADLESS-2");
    arrangement_draw();
    struct arrangement_hit external_hit = {0};
    for (int i = 0; i < narrangement_hits; i++)
        if (arrangement_hits[i].monitor == arrangement_selected) external_hit = arrangement_hits[i];
    arrangement_selected = monitor_anchor();
    struct pointer_event select_event = { .col = external_hit.x, .row = external_hit.y,
                                         .button = BTN_LEFT, .pressed = 1, .clicks = 1 };
    arrangement_pointer(&select_event);
    assert(arrangement_selected == monitor_index("HEADLESS-2") && hit_n == 2);
    select_event.pressed = 0;
    arrangement_pointer(&select_event);
    assert(arrangement_drag < 0);
    puts("PASS synthetic pointer clicks through live loop, screen-box selection and visible pointer");
    arrangement_selected = monitor_index("HEADLESS-2");
    arrangement_draw();
    for (int i = 0; i < narrangement_hits; i++)
        if (arrangement_hits[i].monitor == arrangement_selected) external_hit = arrangement_hits[i];
    select_event = (struct pointer_event){ .col = external_hit.x + external_hit.width / 2,
        .row = external_hit.y + external_hit.height / 2, .button = BTN_LEFT, .pressed = 1 };
    arrangement_pointer(&select_event);
    assert(arrangement_drag == arrangement_selected);
    int original_x = monitors[arrangement_selected].x, original_y = monitors[arrangement_selected].y;
    select_event.button = 0; select_event.pressed = 0;
    select_event.col += 1; select_event.row += 6;
    arrangement_pointer(&select_event);
    /* Down, beyond the snapping distance; still touching the other screen. */
    assert(monitors[arrangement_selected].x == original_x &&
           monitors[arrangement_selected].y > original_y + ARRANGEMENT_SNAP);
    int saved_x = monitors[arrangement_selected].x, saved_y = monitors[arrangement_selected].y;
    select_event.button = BTN_LEFT;
    arrangement_pointer(&select_event);
    assert(arrangement_drag < 0);
    load_monitors();
    b = monitor_index("HEADLESS-2");
    assert(monitors[b].has_position && monitors[b].x == saved_x && monitors[b].y == saved_y);
    arrangement_selected = b;
    fail_open = 1;
    arrangement_move(10, -10);
    fail_open = 0;
    assert(monitors[b].x == saved_x && monitors[b].y == saved_y);
    file_text("monitors.conf", "HEADLESS-1=origin,1280x720,1.25,0,0\nHEADLESS-2=left,1280x720,1.5,-853,-120\n");
    load_monitors();
    b = monitor_index("HEADLESS-2");
    assert(monitors[b].has_position && monitors[b].x == -853 && monitors[b].y == -120);
    file_text("monitors.conf", "HEADLESS-1=origin,1280x720,1.25\nHEADLESS-2=left,1280x720,1.5\n");
    load_monitors();
    arrangement_selected = monitor_index("HEADLESS-2");
    puts("PASS monitor drag saves independent XY, negative positions persist and failed moves roll back");
    /* Snapping as kscreen's: two screens always touch (the nearer side, then
     * edges or centres within 80 lined up); only a corner keeps the screen
     * where it was; with more screens, edges near a neighbour meet or line up. */
    {
        struct monitor_rect two[2] = { { .name = "A", .width = 1024, .height = 576 },
                                       { .name = "B", .width = 853, .height = 480, .x = -853 } };
        int x = -900, y = 300;
        arrangement_snap(two, 2, 1, -853, 0, &x, &y);
        assert(x == -853 && y == 300);
        x = -900; y = -30;
        arrangement_snap(two, 2, 1, -853, 0, &x, &y);
        assert(x == -853 && y == 0); /* tops */
        x = -900; y = 30;
        arrangement_snap(two, 2, 1, -853, 0, &x, &y);
        assert(x == -853 && y == 96); /* bottoms, tried first */
        struct monitor_rect tall[2] = { { .name = "A", .width = 1920, .height = 1080 },
                                        { .name = "B", .width = 800, .height = 480, .x = -800 } };
        x = -850; y = 330;
        arrangement_snap(tall, 2, 1, -800, 0, &x, &y);
        assert(x == -800 && y == 300); /* centres */
        x = 100; y = 600;
        arrangement_snap(two, 2, 1, -853, 0, &x, &y);
        assert(x == 171 && y == 576); /* below, right edges lined up */
        x = -2000; y = 2000;
        arrangement_snap(two, 2, 1, -853, 0, &x, &y);
        assert(x == -853 && y == 0);
        struct monitor_rect three[3] = { { .name = "A", .width = 1024, .height = 576 },
                                         { .name = "B", .width = 1920, .height = 1080, .x = 1024 },
                                         { .name = "C", .width = 800, .height = 600, .x = 5000, .y = 5000 } };
        x = 3000; y = 20;
        arrangement_snap(three, 3, 2, 5000, 5000, &x, &y);
        assert(x == 2944 && y == 0); /* right of B, tops lined up */
        x = 4000; y = 2000;
        arrangement_snap(three, 3, 2, 5000, 5000, &x, &y);
        assert(x == 4000 && y == 2000); /* far from all: free */
    }
    puts("PASS dragging snaps as KDE's display settings: two screens touch, edges meet and line up within 80");
    keys[0] = K_DOWN; keys[1] = 0; keys[2] = 27; nkeys = 3; keypos = 0;
    arrange_screens();
    assert(arrangement_selected == monitor_anchor());
    puts("PASS origin selection survives the arrangement page refresh");
    /* The main page: a click selects a row, on its value it changes it, the
     * wheel moves; a double click is Enter only where Enter acts. */
    build_row_maps();
    int scale_item = -1;
    for (int i = 0; i < nrow_maps; i++)
        if (row_maps[i].field == 2) scale_item = num_hw_items() + 1 + i;
    assert(scale_item > 0);
    settings_page = 1;
    selected = scale_item; /* the page of its category: Input & Output */
    in_page = 1;
    draw();
    assert(category == CAT_INPUT && canvas_has("Display & Monitor") && canvas_has("Mouse & Touchpad"));
    int phys = item_row(scale_item);
    selected = bri_index();
    mouse = (struct pointer_event){ .col = text_x() + 3, .row = phys, .button = BTN_LEFT, .pressed = 1, .clicks = 1 };
    assert(settings_pointer() == -1 && selected == scale_item);
    mouse.col = text_x() + 38;
    assert(settings_pointer() == K_RIGHT);
    mouse.col = text_x() + 32;
    assert(settings_pointer() == K_LEFT);
    mouse.clicks = 2;
    assert(settings_pointer() == K_LEFT); /* not Enter: Enter here would leave */
    mouse = (struct pointer_event){ .col = text_x() + 3, .row = item_row(num_hw_items()),
                                    .button = BTN_LEFT, .pressed = 1, .clicks = 2 };
    assert(settings_pointer() == '\n' && selected == num_hw_items());
    mouse = (struct pointer_event){ .col = text_x() + 3, .row = item_row(num_hw_items()), .wheel = -2 };
    assert(settings_pointer() == -1 && selected == num_hw_items() + 2);
    mouse = (struct pointer_event){ .col = 5, .row = 7 }; /* a motion */
    assert(settings_pointer() == 0);
    puts("PASS main page clicks select and change rows, double click enters, wheel moves");
    /* The sidebar: every setting is in exactly one category; a click on a
     * category opens its page; keys go to the sidebar or the page. */
    int counted[512] = { 0 }, total = num_hw_items() + 1 + nrow_maps + nrules;
    for (int c = 0; c < NCATS; c++) {
        nentries = 0;
        category_entries(c);
        for (int k = 0; k < nentries; k++) if (entries[k].idx >= 0) counted[entries[k].idx]++;
    }
    for (int i = 0; i < total; i++) assert(counted[i] == 1);
    mouse = (struct pointer_event){ .col = 1, .row = PAGE_TOP + CAT_SYSTEM, .button = BTN_LEFT, .pressed = 1,
                                    .clicks = 1 };
    assert(settings_pointer() == -1 && category == CAT_SYSTEM && in_page && selected == host_index());
    draw();
    assert(canvas_has("About This System") && canvas_has("Hostname") && !canvas_has("Mouse & Touchpad"));
    assert(colors[PAGE_TOP + CAT_SYSTEM][1] != BG); /* the category shown is marked */
    in_page = 0;
    sidebar_key(K_UP);
    assert(category == CAT_TIME && !in_page && selected == lang_index());
    draw();
    assert(colors[PAGE_TOP + CAT_TIME][1] == ACCENT && canvas_has("Region & Language") && canvas_has("Date & Time"));
    sidebar_key('\n');
    assert(in_page);
    page_move(1);
    assert(selected == tz_index());
    page_move(100);
    assert(selected == tz_index() + 2);
    page_move(-100);
    assert(selected == lang_index());
    /* / : every category's settings with the text, under "Category: Group". */
    snprintf(search, sizeof search, "%s", "bluetooth");
    build_entries();
    assert(first_setting() == bt_index() && entry_of(bt_index() + 2) >= 0 && entry_of(ts_index()) < 0);
    assert(entries[0].idx == E_TITLE && strstr(entries[0].text, "Connected Devices: Bluetooth"));
    snprintf(search, sizeof search, "%s", "MAC on");
    build_entries();
    assert(first_setting() == nw_index()); /* a saved network's, else new networks' */
    search[0] = '\0';
    selected = fc_index() + 4;
    draw();
    assert(category == CAT_SYSTEM && item_row(fc_index() + 4) >= PAGE_TOP && canvas_has("Camera for faces"));
    for (int c = 0; c < NCATS; c++) { /* each page as it is shown: ROOT/page-N.txt */
        snprintf(snapshot, sizeof snapshot, "page-%d", c);
        show_category(c, c % 2);
        draw();
    }
    snapshot[0] = '\0';
    settings_page = 0;
    puts("PASS sidebar categories hold every setting once; a click opens a page; Up/Down go through the "
         "categories, Enter into the page; the page moves within its settings; / finds across categories");
    test_cols = 24; test_rows = 10;
    snapshot[0] = '\0';
    arrangement_draw();
    test_cols = 100; test_rows = 35;
    puts("PASS narrow and short screen keeps every button and back action visible");
    connector("HEADLESS-2", "disconnected\n", "1280x720\n");
    load_monitors();
    arrangement_selected = monitor_index("HEADLESS-2");
    arrangement_button = MONITOR_BELOW;
    arrangement_apply();
    assert(monitors[arrangement_selected].place == MONITOR_LEFT);
    arrangement_selected = monitor_anchor();
    arrangement_apply();
    assert(monitors[arrangement_selected].place == MONITOR_ORIGIN);
    arrangement_selected = -1; arrangement_select(1); arrangement_draw();
    assert(arrangement_selected == monitor_anchor() && canvas_has("Selected:"));
    keys[0] = '\n'; keys[1] = 27; nkeys = 2; keypos = 0;
    selected = num_hw_items();
    assert(screen_settings_key('\n'));
    puts("PASS disconnected and origin protection; single screen page opens and returns");
    connector("HEADLESS-1", "disconnected\n", "1280x720\n");
    load_monitors();
    arrangement_selected = -1; arrangement_draw();
    assert(canvas_has("No screens connected"));
    connector("HEADLESS-1", "connected\n", "1280x720\n");
    connector("HEADLESS-2", "connected\n", "1280x720\n");
    connector("HEADLESS-3", "connected\n", "1920x1080\n");
    file_text("monitors.conf", "HEADLESS-1=origin,1280x720,1.25\nHEADLESS-3=right,1920x1080,1\nHEADLESS-2=right,1280x720,1.5\n");
    load_monitors();
    struct monitor_rect rects[MAX_MONITORS];
    int indices[MAX_MONITORS], n = arrangement_rects(rects, indices);
    int first = -1, second = -1;
    for (int i = 0; i < n; i++) {
        if (!strcmp(rects[i].name, "HEADLESS-2")) first = i;
        if (!strcmp(rects[i].name, "HEADLESS-3")) second = i;
    }
    assert(first >= 0 && second >= 0 && rects[second].x == rects[first].x + rects[first].width);
    char names[MAX_MONITORS][32];
    for (int i = 0; i < n; i++) {
        snprintf(names[i], sizeof names[i], "%s", rects[i].name);
        rects[i].name = names[i];
    }
    struct monitor swap = monitors[0]; monitors[0] = monitors[2]; monitors[2] = swap;
    struct monitor_rect reordered[MAX_MONITORS];
    int reordered_indices[MAX_MONITORS];
    assert(arrangement_rects(reordered, reordered_indices) == n);
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) if (!strcmp(rects[i].name, reordered[j].name))
        assert(rects[i].x == reordered[j].x && rects[i].y == reordered[j].y);
    puts("PASS no-screen state and stable three-screen layout independent of discovery order");
    file_text("monitors.conf", "HEADLESS-3=right,1920x1080,1\nHEADLESS-2=right,1280x720,1.5\n");
    load_monitors();
    assert(!strcmp(monitors[monitor_anchor()].name, "HEADLESS-1"));
    assert(save_monitors() == 0);
    load_monitors();
    assert(monitors[monitor_index("HEADLESS-1")].place == MONITOR_ORIGIN);
    puts("PASS no-internal-screen fallback origin is persisted");
    file_text("monitors.conf", "HEADLESS-1=origin,1280x720,1.25\nHEADLESS-2=right,1280x720,1.5\nHEADLESS-3=above,1920x1080,1\n");
    load_monitors();
    n = arrangement_rects(rects, indices);
    for (int i = 0; i < n; i++) if (!strcmp(rects[i].name, "HEADLESS-3"))
        assert(rects[i].width == 1920 && rects[i].height == 1080 && rects[i].y == 0);
    puts("PASS mixed native resolutions in shared logical geometry");
    build_row_maps();
    int scale_row = -1, h1 = monitor_index("HEADLESS-1");
    for (int i = 0; i < nrow_maps; i++)
        if (row_maps[i].mon_idx == h1 && row_maps[i].field == 2) scale_row = i;
    assert(scale_row >= 0 && monitors[h1].scale == 1.25);
    selected = num_hw_items() + 1 + scale_row;
    int before_rescales = rescales;
    adjust(+1);
    assert(rescales == before_rescales + 1 && strstr(notice, "Saved"));
    load_monitors();
    assert(monitors[monitor_index("HEADLESS-1")].scale == 1.5);
    fail_rename = 1;
    adjust(+1);
    fail_rename = 0;
    assert(rescales == before_rescales + 1 && strstr(notice, "Cannot save"));
    load_monitors();
    assert(monitors[monitor_index("HEADLESS-1")].scale == 1.5);
    puts("PASS a saved scale resizes this screen's text at once; a failed save does not");
    connector("DP-3", "connected\n", "2560x1600\n");
    file_text("monitors.conf", "HEADLESS-2=right,1280x720,1.5\n");
    load_monitors();
    assert(!strcmp(monitors[monitor_anchor()].name, "DP-3"));
    assert(save_monitors() == 0);
    load_monitors();
    assert(monitors[monitor_index("DP-3")].place == MONITOR_ORIGIN);
    build_row_maps();
    test_rows = 10;
    settings_page = 1;
    selected = num_hw_items() + nrow_maps;
    draw();
    int physical_row = item_row(selected);
    assert(physical_row >= 2 && physical_row < test_rows - 2 && page_scroll > 0);
    selected = 0;
    draw();
    assert(page_scroll == 0 && item_row(0) == PAGE_TOP + 1); /* under its group's title */
    settings_page = 0;
    test_rows = 35;
    puts("PASS internal-screen anchor preference and main-page selected-row scrolling");
    /* The labwc configuration: touch screens and pens on the internal screen. */
    char *text = NULL;
    size_t size = 0;
    FILE *written = open_memstream(&text, &size);
    assert(written);
    labwc_line(written, "    <mapToOutput></mapToOutput>\n"); /* DP-3, the internal one, is connected */
    connector("DP-3", "disconnected\n", "2560x1600\n");
    load_monitors();
    labwc_line(written, "    <mapToOutput></mapToOutput>\n"); /* two external ones: left out */
    connector("HEADLESS-2", "disconnected\n", "1280x720\n");
    connector("HEADLESS-3", "disconnected\n", "1920x1080\n");
    load_monitors();
    labwc_line(written, "    <mapToOutput></mapToOutput>\n"); /* the only one */
    labwc_line(written, "  <touch>\n");
    assert(fclose(written) == 0);
    assert(strcmp(text, "    <mapToOutput>DP-3</mapToOutput>\n    <mapToOutput>HEADLESS-1</mapToOutput>\n"
                        "  <touch>\n") == 0);
    free(text);
    puts("PASS labwc maps touch screens and pens to the internal screen, else the only one, else all");
    /* The terminal keys: a keybind each in labwc's names (one it has no name
     * for here, none); no key, no line. */
#define RUN "><action name=\"Execute\" command=\"/usr/local/bin/locachy-app new foot\" /></keybind>\n"
    const char *bind = "    <keybind key=\"TERMINAL-KEY\"" RUN;
    written = open_memstream(&text, &size);
    assert(written);
    labwc_line(written, bind); /* rc.conf has none: Ctrl+Alt+T */
    snprintf(terminal_key, sizeof terminal_key, "Super+7,Ctrl+Shift+Enter Alt+Space Ctrl+VolumeUp Ctrl+Alt+T+Y");
    labwc_line(written, bind);
    terminal_key[0] = '\0';
    labwc_line(written, bind);
    assert(fclose(written) == 0);
    assert(strcmp(text, "    <keybind key=\"C-A-t\"" RUN "    <keybind key=\"W-7\"" RUN
                        "    <keybind key=\"C-S-Return\"" RUN "    <keybind key=\"A-space\"" RUN) == 0);
    free(text);
    puts("PASS labwc opens a window of foot for each of rc.conf's terminal keys, in its names");
    written = open_memstream(&text, &size);
    assert(written);
    labwc_line(written, "<keybind key=\"HOME-KEY\"><action name=\"Execute\" command=\"/usr/local/lib/locachy/labwc-control --action home\" /></keybind>\n");
    labwc_line(written, "<keybind key=\"LOCK-KEY\"><action name=\"Execute\" command=\"/usr/local/lib/locachy/labwc-control --action lock\" /></keybind>\n");
    labwc_line(written, "<keybind key=\"IME-KEY\"><action name=\"Execute\" command=\"/usr/bin/true\" /></keybind>\n");
    assert(fclose(written) == 0);
    assert(strstr(text, "key=\"W-Escape\"") && strstr(text, "key=\"C-A-Escape\"") &&
           strstr(text, "key=\"C-A-Super_L\"") && strstr(text, "key=\"W-l\""));
    assert(strstr(text, "key=\"C-space\"><action name=\"Execute\" command=\"/usr/bin/true\"") &&
           strstr(text, "key=\"Henkan_Mode\"") && strstr(text, "key=\"Muhenkan\"") &&
           strstr(text, "key=\"Zenkaku_Hankaku\""));
    assert(!strstr(text, "HOME-KEY") && !strstr(text, "LOCK-KEY") && !strstr(text, "IME-KEY"));
    free(text);
    puts("PASS compositor home, lock and every input method key are consuming bindings, not observer-only actions");
    rc_internal = "";
    assert(!monitor_internal("DP-3") && monitor_internal("eDP-1") && monitor_internal("LVDS-1"));
    assert(monitor_internal("DSI-1") && !monitor_internal("HDMI-A-1"));
    rc_internal = "DP-3";
    assert(monitor_internal("DP-3") && !monitor_internal("eDP-1"));
    puts("PASS the internal screen: eDP, LVDS or DSI, or only the one rc.conf's internal_screen names");
    /* Installed and on are two rows: on only once installed (off: the network
     * goes direct); removing switches it off first. Then a network's MAC. */
    allow_rc = 1;
    selected = ts_index() + 1;
    adjust(+1);
    assert(!tailscale_enabled && strstr(notice, "not installed") && forks == 0);
    selected = ts_index();
    adjust(+1);
    assert(forks == 1 && !strcmp(pkg_action, "install") && !strcmp(pkg_name, "tailscale") && !tailscale_enabled);
    pkg_job = 0;
    fake_tailscale = 1;
    selected = ts_index() + 1;
    adjust(+1);
    assert(tailscale_enabled && strstr(rc_log, "service_tailscale=" TAILSCALE_SERVICE ";") && hups == 1);
    rc_log[0] = '\0';
    selected = ts_index();
    adjust(+1);
    assert(!tailscale_enabled && strstr(rc_log, "service_tailscale=(none);") && hups == 2);
    assert(forks == 2 && !strcmp(pkg_action, "remove"));
    pkg_job = 0;
    selected = cl_index() + 1;
    adjust(+1);
    assert(!clash_on && strstr(notice, "not installed"));
    fake_mihomo = 1;
    adjust(+1);
    assert(clash_on && !clash_proxy && strstr(rc_log, "service_clash=" CLASH_SERVICE ";"));
    adjust(-1);
    assert(!clash_on && strstr(rc_log, "service_clash=(none);") && strstr(notice, "direct"));
    adjust(+1);
    rc_log[0] = '\0';
    selected = cl_index();
    adjust(+1);
    assert(!clash_on && strstr(rc_log, "service_clash=(none);") && forks == 3 && !strcmp(pkg_action, "remove"));
    pkg_job = 0;
    puts("PASS installed and on are two rows; on only once installed, off goes direct, removing switches off");
    nnets = 1;
    snprintf(nets[0].name, sizeof nets[0].name, "Home");
    selected = nw_index();
    adjust(+1);
    assert(nets[0].mac == 1 && forks == 4 && strstr(notice, "a new MAC address each time"));
    adjust(+1);
    assert(nets[0].mac == 2 && forks == 5 && strstr(notice, "the card's real MAC address"));
    adjust(+1);
    assert(nets[0].mac == 0 && forks == 6 && strstr(notice, "the same each time"));
    adjust(-1);
    assert(nets[0].mac == 2 && forks == 7);
    adjust(+1);
    assert(wp_index() == nw_index() + 2);
    puts("PASS a saved network's MAC: its own, a new one each time or the card's real one, through nmcli");
    /* New networks (and saved ones that name none): NetworkManager's drop-in. */
    char nmconf[1400], nmtext[512] = "";
    snprintf(nmconf, sizeof nmconf, "%s/nm-mac.conf", root);
    selected = nw_index() + nnets;
    adjust(+1);
    int nmfd = open(nmconf, O_RDONLY);
    ssize_t got = nmfd >= 0 ? read(nmfd, nmtext, sizeof nmtext - 1) : 0;
    nmtext[got > 0 ? got : 0] = '\0';
    if (nmfd >= 0) close(nmfd);
    assert(mac_default == 1 && strstr(nmtext, "wifi.cloned-mac-address=random\nethernet.cloned-mac-address=random\n") &&
           strstr(notice, "a new one each time"));
    selected = nw_index() + nnets;
    adjust(-1);
    assert(mac_default == 0 && access(nmconf, F_OK) != 0 && strstr(notice, "the same each time"));
    nnets = 0;
    puts("PASS new networks' MAC: a drop-in for NetworkManager, none for the default");
    int hups_before = hups;
    rc_log[0] = '\0';
    selected = sec_index() + 1;
    adjust(+1);
    assert(login_skip && !strcmp(rc_log, "login_skip=1;") && hups == hups_before + 1 && strstr(notice, "home at once"));
    adjust(-1);
    assert(!login_skip && strstr(rc_log, "login_skip=(none);") && hups == hups_before + 2);
    assert(strstr(notice, "the lock screen first"));
    assert(sec_index() == link_index() + 1 && fp_index() == sec_index() + 4 && fc_index() == fp_index() + 4);
    puts("PASS Password at boot: off writes login_skip=\"1\", on takes it out; process 1 reads it again");
    rc_log[0] = '\0';
    selected = sec_index() + 3;
    assert(lock_after == 10);
    adjust(+1);
    assert(lock_after == 15 && !strcmp(rc_log, "lock_after=15;") && hups == hups_before + 3);
    for (int i = 0; i < 5; i++) adjust(-1);
    assert(lock_after == 0 && strstr(rc_log, "lock_after=0;") && strstr(notice, "Never"));
    adjust(-1);
    assert(lock_after == 60 && strstr(rc_log, "lock_after=60;") && hups == hups_before + 9);
    puts("PASS Lock when idle: 10 min unless set, round never 1 2 5 10 15 30 60, written as lock_after");
    /* Fingerprints: installed and on are two rows, as for Tailscale. */
    rc_log[0] = '\0';
    int forks_before = forks;
    selected = fp_index() + 1;
    adjust(+1);
    assert(!fingerprint_on && strstr(notice, "not installed") && !rc_log[0]);
    selected = fp_index();
    adjust(+1);
    assert(forks == forks_before + 1 && !strcmp(pkg_action, "install") && !strcmp(pkg_name, "fprintd"));
    selected = fp_index() + 1;
    adjust(+1);
    assert(strstr(notice, "still being installed") && !fingerprint_on);
    pkg_job = 0;
    fake_fprintd = 1;
    file_text("sudo", "#%PAM-1.0\nauth\t\tinclude\t\tsystem-auth\naccount\t\tinclude\t\tsystem-auth\n");
    adjust(+1);
    assert(fingerprint_on && !strcmp(rc_log, "unlock_fingerprint=1;"));
    char sudo_text[512], sudo_path[1400];
    snprintf(sudo_path, sizeof sudo_path, "%s/sudo", root);
    FILE *sudo_file = fopen(sudo_path, "r");
    size_t sudo_len = sudo_file ? fread(sudo_text, 1, sizeof sudo_text - 1, sudo_file) : 0;
    sudo_text[sudo_len] = '\0';
    if (sudo_file) fclose(sudo_file);
    assert(!strcmp(sudo_text, "#%PAM-1.0\nauth\t\tsufficient\tpam_locachy_any.so locachy-lock-fingerprint\n"
                              "auth\t\tinclude\t\tsystem-auth\naccount\t\tinclude\t\tsystem-auth\n"));
    selected = fp_index() + 3;
    assert(fp_next == 0);
    adjust(-1);
    assert(fp_next == NFINGERS - 1 && !strcmp(fingers[fp_next], "left-little-finger"));
    rc_log[0] = '\0';
    selected = fp_index();
    adjust(+1);
    assert(!fingerprint_on && !strcmp(rc_log, "unlock_fingerprint=(none);") && !strcmp(pkg_action, "remove"));
    sudo_file = fopen(sudo_path, "r");
    sudo_len = sudo_file ? fread(sudo_text, 1, sizeof sudo_text - 1, sudo_file) : 0;
    sudo_text[sudo_len] = '\0';
    if (sudo_file) fclose(sudo_file);
    assert(!strcmp(sudo_text, "#%PAM-1.0\nauth\t\tinclude\t\tsystem-auth\naccount\t\tinclude\t\tsystem-auth\n"));
    pkg_job = 0;
    fake_fprintd = 0;
    /* Faces use capture capabilities; enumeration never starts streaming. */
    struct face_cam candidates[8];
    camera_listing = "/dev/video0\t0\tColour Camera\n/dev/video128\t1\tIR Camera\n/dev/videobad\t1\tInvalid\n";
    int ncam = face_cameras(candidates, 8);
    assert(ncam == 2 && !candidates[0].ir && candidates[1].ir);
    assert(!strcmp(candidates[1].path, "/dev/video128"));
    face_choice[0] = '\0';
    assert(next_face_camera(+1) == 0 && !strcmp(face_choice, "/dev/video0"));
    assert(next_face_camera(+1) == 0 && !strcmp(face_choice, "/dev/video128"));
    assert(next_face_camera(+1) == 0 && !face_choice[0]);
    assert(next_face_camera(-1) == 0 && !strcmp(face_choice, "/dev/video128"));
    assert(enter_row(fc_index() + 4));
    file_text("howdy.ini", "[video]\ndevice_path=/dev/missing\n");
    face_choice_loaded = 0;
    load_face_camera();
    assert(!face_choice[0]);
    file_text("howdy.ini", "[video]\ndevice_path=/dev/video128\n");
    face_choice_loaded = 0;
    load_face_camera();
    assert(!strcmp(face_choice, "/dev/video128") && camera_commands > 0);
    fake_howdy = 1;
    test_face_camera();
    assert(face_term_calls == 1 && strstr(face_term_args, "test /dev/video128 --user ") &&
           strstr(face_term_args, "--apply --interactive") && strstr(notice, "tested and saved"));
    face_choice[0] = '\0';
    term_status = 1;
    test_face_camera();
    assert(face_term_calls == 2 && strstr(face_term_args, "auto --user ") && strstr(notice, "failed"));
    term_status = fake_howdy = 0;
    camera_listing = "";
    face_choice[0] = '\0';
    puts("PASS camera candidates from capture enumeration, automatic choice and preview without configuration writes");
    /* With both unlock ways on, sudo names both PAM services. */
    assert(fc_index() == fp_index() + 4 && rclone_index() == fc_index() + 7 &&
           lang_index() == rclone_index() + 3 && host_index() == lang_index() + 1 &&
           ime_index() == host_index() + 1 && tpm_index() == ime_index() + 2 &&
           bri_index() == tpm_index() + 1 && mach_index() == bri_index() + 1 &&
           upd_index() == mach_index() + 1 && boot_index() == upd_index() + 2 && opt_index() == boot_index() + 5 &&
           repo_index() == opt_index() + NOPT_MODULES && tnet_index() == repo_index() + NREPO_SECTIONS + 1 &&
           circ_index() == tnet_index() + 1 && imek_index() == circ_index() + 3 &&
           kb_index() == imek_index() + NIME_KEYS && num_hw_items() == kb_index() + nkeyboards_seen);
    forks_before = forks;
    selected = ime_index();
    adjust(+1);
    assert(forks == forks_before + 1 && !strcmp(pkg_action, "install") && !strcmp(pkg_name, IME_BASE_PACKAGES));
    pkg_job = 0;
    selected = ime_index() + 1;
    adjust(+1);
    assert(forks == forks_before + 2 && !strcmp(pkg_action, "install") && !strcmp(pkg_name, IME_ALL_PACKAGES));
    pkg_job = 0;
    fake_rime = 1;
    fake_rime_zh = 1;
    adjust(+1);
    assert(forks == forks_before + 3 && !strcmp(pkg_action, "remove") && !strcmp(pkg_name, IME_ZH_PACKAGES));
    pkg_job = 0;
    fake_rime_zh = 0;
    selected = ime_index();
    adjust(+1);
    assert(forks == forks_before + 4 && !strcmp(pkg_action, "remove") && !strcmp(pkg_name, IME_ALL_PACKAGES));
    pkg_job = 0;
    fake_rime = 0;
    puts("PASS optional Rime input method rows: English base and Chinese rime-frost installed/removed separately");
    char cam[32], local[1400];
    int has_camera = camera(cam, sizeof cam);
    forks_before = forks;
    selected = fc_index();
    adjust(+1);
    assert(has_camera ? forks == forks_before + 1 && !strcmp(pkg_name, "python-dlib " FACE_PACKAGES) &&
                        !strcmp(pkg_action, "install")
                      : forks == forks_before && strstr(notice, "No camera"));
    pkg_job = 0;
    /* What computes them: without NVIDIA's driver the CPU, nothing to choose. */
    snprintf(local, sizeof local, "%s/pacman-local", root);
    assert(mkdir(local, 0755) == 0);
    selected = fc_index() + 1;
    rc_log[0] = '\0';
    forks_before = forks;
    adjust(+1);
    assert(!face_cuda() && !rc_log[0] && forks == forks_before && strstr(notice, "NVIDIA's driver"));
    /* With it: CUDA unless rc.conf says the CPU; the row goes between them,
     * and once a dlib is installed, the other takes its place. */
    fake_nvidia = 1;
    assert(face_cuda() && !strcmp(dlib_package(face_cuda()), "python-dlib-cuda"));
    adjust(+1);
    assert(!face_cuda() && !strcmp(rc_log, "face_dlib=cpu;") && forks == forks_before && strstr(notice, "the CPU"));
    snprintf(local, sizeof local, "%s/pacman-local/python-dlib-20.0.1-2", root);
    assert(mkdir(local, 0755) == 0 && dlib_installed() == 1);
    adjust(-1);
    assert(face_cuda() && strstr(rc_log, "face_dlib=cuda;") && forks == forks_before + 1 &&
           !strcmp(pkg_action, "swap") && !strcmp(pkg_name, "python-dlib python-dlib-cuda") &&
           strstr(notice, "python-dlib-cuda"));
    pkg_job = 0;
    rmdir(local);
    fake_nvidia = 0;
    face_dlib[0] = '\0';
    selected = fc_index() + 2;
    adjust(+1);
    assert(!face_on && strstr(notice, "not installed"));
    fingerprint_on = face_on = 1;
    assert(sudo_ways() == 0);
    sudo_file = fopen(sudo_path, "r");
    sudo_len = sudo_file ? fread(sudo_text, 1, sizeof sudo_text - 1, sudo_file) : 0;
    sudo_text[sudo_len] = '\0';
    if (sudo_file) fclose(sudo_file);
    assert(strstr(sudo_text, "pam_locachy_any.so locachy-lock-fingerprint locachy-lock-face\n"));
    fingerprint_on = face_on = 0;
    assert(sudo_ways() == 0);
    test_bluetooth();
    test_circular();
    test_ime_keys();
    test_keyboards();
    /* Line editing in prompts: cursor movement, Delete and Ctrl+U. */
    char edited[64] = "abc";
    size_t epos = strlen(edited);
    edit_line_key(K_LEFT, edited, sizeof edited, &epos);
    edit_line_key('X', edited, sizeof edited, &epos);
    edit_line_key(K_BOL, edited, sizeof edited, &epos);
    edit_line_key(K_DEL, edited, sizeof edited, &epos);
    assert(strcmp(edited, "bXc") == 0);
    /* A character beyond ASCII: inserted whole, gone whole with Backspace. */
    edit_line_key(K_EOL, edited, sizeof edited, &epos);
    edit_line_key(K_TEXT, edited, sizeof edited, &epos);
    edit_line_key('!', edited, sizeof edited, &epos);
    assert(strcmp(edited, "bXcé!") == 0 && epos == strlen(edited));
    edit_line_key(K_LEFT, edited, sizeof edited, &epos);
    edit_line_key('\b', edited, sizeof edited, &epos);
    assert(strcmp(edited, "bXc!") == 0 && epos == 3);
    allow_rc = 0;
    puts("PASS faces: installing needs a camera; dlib on the CPU, or CUDA with NVIDIA's driver (rc.conf's face_dlib, "
         "swapped once installed); on only once howdy is installed; sudo's line names both ways");
    puts("PASS fingerprints: fprintd installed and unlock on are two rows; on only once installed (sudo's PAM "
         "file gets pam_locachy_any.so before its first auth line), removing switches off first (the line goes); "
         "the finger to add goes round the ten");
    printf("settings monitors: %d UI frames passed; fixtures and snapshots: %s\n", frames, root);
    return 0;
}
