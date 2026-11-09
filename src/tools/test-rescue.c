/*
 * Linux native UI unit test. Screen/input are doubles; helper, term and
 * keytable are exec'd copies of this binary (not shell scripts). The real
 * UI event loop, prompts, argv, raw stdin, pipes, timeout and cleanup run.
 * Build with base/ui.c, base/textsel.c and json-c, e.g.:
 * cc -O1 -g -D_GNU_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined
 *    -fno-omit-frame-pointer -Ibase/include tools/test-rescue.c
 *    base/ui.c base/textsel.c $(pkg-config --cflags --libs json-c) -o test-rescue
 * test-rescue NEW-DIRECTORY
 * This is not a real storage/link/Wayland or complete ISO acceptance test.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static char directory[512];
static char *path(const char *name);
#define RESCUE_HELPER path("helper")
#define RESCUE_TERM path("term")
#define RESCUE_SYSTEMCTL path("systemctl")
#define RESCUE_RUN_DIR path("run")
#define RESCUE_KEYTABLE path("keytable")
#define RESCUE_KEYMAP path("run/keymap.table")
#define RESCUE_KEYMAP_TMP path("run/keymap.table.tmp")
#define RESCUE_TIMEOUT_MS 600
#define RESCUE_KILL_MS 100
#define main rescue_main
#include "../progs/rescue.c"
#undef main
#include "i18n-stub.h"

static char *path(const char *name)
{
    static char paths[32][1024]; static int i;
    i = (i + 1) % 32;
    int n = snprintf(paths[i], sizeof paths[i], "%s/%s", directory, name);
    assert(n > 0 && (size_t)n < sizeof paths[i]);
    return paths[i];
}
static void write_file(const char *name, const char *text)
{
    FILE *f = fopen(path(name), "w"); assert(f);
    assert(fputs(text, f) >= 0); assert(!fclose(f));
}
static char *slurp(const char *name)
{
    static char data[256 * 1024];
    FILE *f = fopen(path(name), "r");
    if (!f) { data[0] = '\0'; return data; }
    size_t n = fread(data, 1, sizeof data - 1, f);
    assert(!ferror(f)); assert(!fclose(f)); data[n] = '\0';
    return data;
}
static int exists(const char *name) { return access(path(name), F_OK) == 0; }
static void remove_file(const char *name) { unlink(path(name)); }
static const char closed_json[] =
    "{\"ok\":true,\"target\":{\"state\":\"closed\",\"device\":\"\",\"uuid\":\"\",\"mount\":\"\",\"swap\":\"\",\"esp\":\"\",\"esp_reason\":\"\",\"repair_blocked\":false},"
    "\"devices\":[],\"interfaces\":[],\"link\":{\"active\":false,\"interface\":\"\",\"address\":\"\",\"port\":2222,\"fingerprint\":\"\",\"state\":\"off\"},"
    "\"output\":\"No disks or interfaces.\",\"error\":\"\"}\n";
static int fake(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/'); base = base ? base + 1 : argv[0];
    int fd = open(path("calls"), O_WRONLY | O_APPEND | O_CREAT, 0600); assert(fd >= 0);
    dprintf(fd, "%s", base);
    for (int i = 1; i < argc; i++) dprintf(fd, "|%s", argv[i]);
    dprintf(fd, "\n"); close(fd);
    if (!strcmp(base, "systemctl")) {
        assert(argc == 2 && (!strcmp(argv[1], "reboot") || !strcmp(argv[1], "poweroff")));
        return exists("power-fail") ? 1 : 0;
    }
    if (!strcmp(base, "term")) {
        assert(argc == 3 && !strcmp(argv[1], RESCUE_HELPER) && !strcmp(argv[2], "target-terminal"));
        return exists("term-fail") ? 1 : 0;
    }
    if (!strcmp(base, "keytable")) {
        assert(argc == 3 && !strcmp(argv[2], RESCUE_KEYMAP));
        fprintf(stderr, "keytable diagnostic\n");
        if (exists("keytable-fail")) return 1;
        FILE *f = fopen(argv[2], "w"); assert(f);
        fprintf(f, "console %s\nlayout %s\nkey 21 y 7a 5a\n", argv[1], argv[1]);
        assert(!fclose(f)); return 0;
    }
    assert(argc >= 2);
    unsigned char input[SECRET_MAX + 2]; size_t got = 0;
    ssize_t n;
    while ((n = read(STDIN_FILENO, input + got, sizeof input - got)) > 0) {
        got += (size_t)n; assert(got < sizeof input);
    }
    assert(n == 0);
    fd = open(path("stdin"), O_WRONLY | O_APPEND | O_CREAT, 0600); assert(fd >= 0);
    dprintf(fd, "%s|%zu|", argv[1], got);
    for (size_t i = 0; i < got; i++) dprintf(fd, "%02x", input[i]);
    dprintf(fd, "\n"); close(fd);
    explicit_bzero(input, sizeof input);
    if (!strcmp(argv[1], "scan") && exists("hang")) {
        signal(SIGTERM, SIG_IGN);
        write_file("hang-ready", "ready");
        for (;;) pause();
    }
    if (!strcmp(argv[1], "scan") && exists("oversize")) {
        for (int i = 0; i < 140000; i++) fputs("xxxxxxxx", stdout);
        fflush(stdout); return 0;
    }
    if (!strcmp(argv[1], "scan") && exists("descendant")) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) { for (;;) pause(); }
        puts(closed_json); fflush(stdout); return 0;
    }
    if (!strcmp(argv[1], "scan") && exists("malformed")) { puts("{invalid"); return 0; }
    int fail = !strcmp(argv[1], "cleanup") && exists("busy");
    int bad_key = !strcmp(argv[1], "unlock-readonly") && exists("bad-key");
    const char *mode = "closed";
    if (exists("rw")) mode = "readwrite";
    else if (exists("ro")) mode = "readonly";
    if (!strcmp(argv[1], "unlock-readonly") && !bad_key) { assert(argc == 3); write_file("ro", argv[2]); mode = "readonly"; }
    if (!strcmp(argv[1], "enable-repair")) {
        assert(argc == 3 && !strcmp(argv[2], "--confirm"));
        write_file("rw", "rw"); mode = "readwrite";
    }
    if ((!strcmp(argv[1], "close-target") || !strcmp(argv[1], "cleanup")) && !fail) {
        remove_file("ro"); remove_file("rw"); mode = "closed";
    }
    if (!strcmp(argv[1], "start-link")) { assert(argc == 3); write_file("active", argv[2]); }
    if (!strcmp(argv[1], "stop-link") || (!strcmp(argv[1], "cleanup") && !fail)) remove_file("active");
    if (!strcmp(argv[1], "cleanup") && exists("lying-cleanup")) { write_file("ro", "/dev/sdb3"); mode = "readonly"; }
    struct json_object *j = json_tokener_parse(closed_json); assert(j);
    json_object_object_add(j, "ok", json_object_new_boolean(!fail && !bad_key));
    json_object_object_add(j, "output", json_object_new_string(!strcmp(argv[1], "scan") ?
        "Scan result\nline 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\nline 9\nline 10\n" : "Operation output"));
    json_object_object_add(j, "error", json_object_new_string(fail ? "Target is busy; cleanup refused" : bad_key ? "Wrong passphrase" : ""));
    struct json_object *t = NULL; assert(json_object_object_get_ex(j, "target", &t));
    json_object_object_add(t, "state", json_object_new_string(mode));
    if (strcmp(mode, "closed")) {
        json_object_object_add(t, "device", json_object_new_string("/dev/sdb3"));
        json_object_object_add(t, "uuid", json_object_new_string("second-uuid"));
        json_object_object_add(t, "mount", json_object_new_string("/run/rescue-target"));
        json_object_object_add(t, "swap", json_object_new_string("/dev/sdb2"));
        json_object_object_add(t, "esp", json_object_new_string("/dev/sdb1"));
    }
    if (exists("esp-blocked")) {
        json_object_object_add(t, "repair_blocked", json_object_new_boolean(1));
        json_object_object_add(t, "esp_reason", json_object_new_string("ESP identity is ambiguous"));
    }
    if (exists("old-stub")) {
        json_object_object_del(t, "repair_blocked"); json_object_object_del(t, "esp_reason");
    }
    if (!exists("empty")) {
        struct json_object *devices = json_tokener_parse("[{\"path\":\"/dev/sda3\",\"uuid\":\"first-uuid\",\"label\":\"locachy-crypt\",\"model\":\"First SSD\",\"size\":107374182400},{\"path\":\"/dev/sdb3\",\"uuid\":\"second-uuid\",\"label\":\"locachy-crypt\",\"model\":\"Second SSD\",\"size\":214748364800}]");
        struct json_object *interfaces = json_tokener_parse("[{\"name\":\"enp1s0\",\"address\":\"02:00:00:00:00:01\",\"carrier\":true},{\"name\":\"enp2s0\",\"address\":\"02:00:00:00:00:02\",\"carrier\":false}]");
        assert(devices && interfaces);
        json_object_object_add(j, "devices", devices); json_object_object_add(j, "interfaces", interfaces);
    }
    if (exists("active")) {
        struct json_object *l = NULL; assert(json_object_object_get_ex(j, "link", &l));
        json_object_object_add(l, "active", json_object_new_boolean(1));
        json_object_object_add(l, "interface", json_object_new_string("enp2s0"));
        json_object_object_add(l, "address", json_object_new_string("fe80::1234%enp2s0"));
        json_object_object_add(l, "fingerprint", json_object_new_string("SHA256:unit-test-host-key"));
        json_object_object_add(l, "state", json_object_new_string("active"));
    }
    fprintf(stderr, "helper stderr diagnostic\n");
    puts(json_object_to_json_string_ext(j, JSON_C_TO_STRING_PLAIN));
    json_object_put(j);
    return fail || bad_key ? 1 : 0;
}

#define MAX_COLS 160
#define MAX_ROWS 64
static int cols = 140, rows = 44, screen_opens, screen_releases, key_opens, key_releases, close_kept, polls, sensitive_shown;
static char grid[MAX_ROWS][MAX_COLS + 1], history[256 * 1024], copied[2][2048], layout_table[128];
static struct pointer_event mouse;
static int events[8192], nevents, event_at, resizing, when;
#define WAIT_JOB (-1)
#define SNAP (-2)
#define WAIT_HANG (-3)
#define REMOVE_BUSY (-4)
#define RESIZE (-5)
#define MOUSE (-6)
#define REMOVE_LIE (-7)
#define STOP_UI (-8)
static struct pointer_event pointers[128]; static int npointers, pointer_at;

void say(const char *fmt, ...)
{
    (void)fmt;
}
int screen_open(const char *font) { (void)font; screen_opens++; return 1; }
void screen_release(void) { screen_releases++; }
int keys_open(void) { key_opens++; return 1; }
void keys_release(void) { key_releases++; }
void keys_leds(int on) { (void)on; }
void key_close_kept(void) { close_kept++; }
int pointer_open(void) { return 0; }
int *pointer_lend(size_t *n, char paths[][POINTER_PATH], size_t max) { (void)paths; (void)max; *n = 0; return NULL; }
int pointer_take(int fd, const char *name) { (void)fd; (void)name; return 0; }
void pointer_release(void) {}
const struct pointer_event *pointer_last(void) { return &mouse; }
const char *keys_layout_name(const char *name) { (void)name; return layout_table[0] ? layout_table : "us"; }
int keys_layout_load(const char *name)
{
    FILE *f = fopen(name, "r"); if (!f) return -1;
    int ok = fscanf(f, "console %127s", layout_table) == 1;
    fclose(f); return ok ? 0 : -1;
}
const char *keys_text(void) { return "ü"; }
int screen_cols(void) { return cols; }
int screen_rows(void) { return rows; }
int screen_char_cells(uint32_t cp) { (void)cp; return 1; }
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
void screen_fill(int x, int y, int w, int h, uint32_t color)
{
    (void)color;
    assert(w >= 0 && h >= 0);
    for (int r = y < 0 ? 0 : y; r < y + h && r < rows; r++) {
        for (int c = x < 0 ? 0 : x; c < x + w && c < cols; c++) grid[r][c] = ' ';
    }
}
void screen_text(int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    (void)fg; (void)bg;
    if (strstr(text, "disk-secret") || strstr(text, "link-secret-12")) sensitive_shown++;
    for (int c = x; *text && c < cols; c++, text++) if (c >= 0 && y >= 0 && y < rows) grid[y][c] = *text;
}
void screen_flush(void) {}
int clipboard_set(int primary, const void *text, size_t size)
{
    snprintf(copied[primary ? 1 : 0], sizeof copied[0], "%.*s", (int)size, (const char *)text); return CLIPBOARD_OK;
}
const char *clipboard_error(int status) { (void)status; return "clipboard error"; }
static void snapshot(void)
{
    size_t n = strlen(history);
    for (int r = 0; r < rows && n + (size_t)cols + 2 < sizeof history; r++) {
        memcpy(history + n, grid[r], (size_t)cols); n += (size_t)cols; history[n++] = '\n';
    }
    history[n] = '\0';
}
static void k(int key) { assert(nevents < (int)(sizeof events / sizeof events[0])); events[nevents++] = key; }
static void type(const char *text) { for (; *text; text++) k((unsigned char)*text); }
static void click(int col, int row, int clicks)
{
    assert(npointers < 128);
    pointers[npointers++] = (struct pointer_event){ .col = col, .row = row, .button = BTN_LEFT, .pressed = 1, .clicks = clicks };
    k(MOUSE);
}
static void wheel(int col, int row, int amount)
{
    assert(npointers < 128);
    pointers[npointers++] = (struct pointer_event){ .col = col, .row = row, .wheel = amount };
    k(MOUSE);
}
int key_poll_timeout(int fd, int ms)
{
    (void)fd;
    assert(ms >= 0 && ms <= 500);
    assert(++polls < 20000);
    if (event_at == nevents) {
        fprintf(stderr, "Missing test event: modal=%d child=%d exiting=%d notice=%s\n", modal, (int)child.pid, exiting, notice);
        abort();
    }
    int key = events[event_at];
    if (key == WAIT_JOB || key == WAIT_HANG) {
        if ((key == WAIT_JOB && child.pid) || (key == WAIT_HANG && !exists("hang-ready"))) {
            struct pollfd p = { .fd = -1 }; poll(&p, 0, 5); return 0;
        }
        event_at++; return 0;
    }
    event_at++;
    if (key == SNAP) { snapshot(); return 0; }
    if (key == REMOVE_BUSY) { remove_file("busy"); return 0; }
    if (key == REMOVE_LIE) { remove_file("lying-cleanup"); return 0; }
    if (key == STOP_UI) { interrupted = SIGTERM; return 0; }
    if (key == RESIZE) { cols = resizing ? 140 : 52; rows = resizing ? 44 : 30; resizing = !resizing; return K_SHOWN; }
    if (key == MOUSE) { assert(pointer_at < npointers); mouse = pointers[pointer_at++]; return K_POINTER; }
    when++;
    return key;
}
int key_get(void) { return key_poll_timeout(-1, 500); }
static void action(int which)
{
    k(K_BOL);
    for (int i = 0; i < which; i++) k(K_DOWN);
    k('\n');
}
static void reset(void)
{
    assert(!child.pid);
    close_fd(&lock_fd);
    state = (struct snapshot){0};
    known = selected = action_top = focus = status_scroll = output_scroll = modal = pick_sel = pick_top = pick_offset = 0;
    own_keyboard = 0;
    boot_session = session_closed = closed_selected = power_confirmation = signal_exit = 0;
    unsetenv("LOCACHY_RESCUE_BOOT");
    exiting = done = notice_error = log_count = nstatus = handed_screen = interrupted = 0;
    notice[0] = ssh[0] = partial[0] = history[0] = layout_table[0] = '\0';
    wipe_secrets();
    child = (struct child){ .in = -1, .out = -1, .err = -1 };
    cols = 140; rows = 44; nevents = event_at = npointers = pointer_at = polls = resizing = 0;
    sensitive_shown = close_kept = 0;
    textsel_clear();
    memset(grid, 0, sizeof grid); memset(copied, 0, sizeof copied);
    for (int r = 0; r < MAX_ROWS; r++) { memset(grid[r], ' ', MAX_COLS); grid[r][MAX_COLS] = '\0'; }
    const char *files[] = { "calls", "stdin", "ro", "rw", "active", "empty", "busy", "hang", "hang-ready", "malformed",
        "oversize", "term-fail", "keytable-fail", "bad-key", "lying-cleanup", "descendant", "esp-blocked", "old-stub", "power-fail" };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) remove_file(files[i]);
}
static void run(const char *name)
{
    printf("== %s\n", name); fflush(stdout);
    assert(rescue_main() == 0);
    if (event_at < nevents && events[event_at] == WAIT_JOB && event_at + 1 == nevents) event_at++;
    assert(event_at == nevents);
    assert(!child.pid && child.in < 0 && child.out < 0 && child.err < 0);
    assert(!secret[0] && !first_secret[0] && !child.input[0]);
    assert(!strcmp(state.state, "closed") && !state.active && done);
}
static void exit_events(void) { k('q'); k(WAIT_JOB); }
static void lock_tests(void)
{
    assert(session_lock() == 0);
    int first = lock_fd;
    lock_fd = -1;
    assert(session_lock() < 0);
    close(first);
    assert(session_lock() == 0); close_fd(&lock_fd);
    char target[1024]; snprintf(target, sizeof target, "%s", path("run/ui.lock"));
    assert(!unlink(target)); assert(!symlink("../calls", target));
    assert(session_lock() < 0); assert(!unlink(target));
    assert(session_lock() == 0); close_fd(&lock_fd);
    printf("PASS single UI ownership and symlink refusal\n");
}
int main(int argc, char **argv)
{
    const char *dir = getenv("RESCUE_TEST_DIRECTORY");
    if (dir) { snprintf(directory, sizeof directory, "%s", dir); return fake(argc, argv); }
    assert(argc == 2);
    struct rlimit original_core;
    assert(!getrlimit(RLIMIT_CORE, &original_core));
    struct rlimit finite_core = { 1024 * 1024, 1024 * 1024 };
    assert(!setrlimit(RLIMIT_CORE, &finite_core));
    snprintf(directory, sizeof directory, "%s", argv[1]);
    assert(!mkdir(directory, 0700));
    assert(!setenv("RESCUE_TEST_DIRECTORY", directory, 1));
    assert(!symlink("/proc/self/exe", RESCUE_HELPER));
    assert(!symlink("/proc/self/exe", RESCUE_TERM));
    assert(!symlink("/proc/self/exe", RESCUE_SYSTEMCTL));
    assert(!symlink("/proc/self/exe", RESCUE_KEYTABLE));
    assert(!mkdir(RESCUE_RUN_DIR, 0700));
    /* Children follow /proc/self/exe through exec to this binary, selecting
     * fake() with the inherited test directory; the parent keeps running UI. */
    lock_tests();

    reset(); write_file("empty", "");
    k(WAIT_JOB); k(SNAP); action(A_UNLOCK); k(SNAP); exit_events();
    run("standalone start with no disk: no implicit unlock or terminal");
    assert(strstr(history, "No encrypted target was found"));
    assert(!strcmp(slurp("calls"), "helper|status\nhelper|cleanup\n"));

    reset();
    k(WAIT_JOB); action(A_UNLOCK); k(K_DOWN); k('\n'); type("disk-secret"); k(K_TEXT); k('\n');
    k(WAIT_JOB); k(SNAP);
    action(A_REPAIR); k('n'); action(A_REPAIR); k('y'); type("disk-secret"); k('\n'); k(WAIT_JOB); k(SNAP);
    action(A_START_LINK); type("enp2"); k('\n'); type("short"); k('\n'); k(SNAP); k(21);
    type("link-secret-12"); k('\n'); type("different-secret"); k('\n'); k(SNAP);
    type("link-secret-12"); k('\n'); type("link-secret-12"); k('\n'); k(WAIT_JOB); k(SNAP);
    k('a'); k('f'); k(RESIZE); k('\t'); k(K_EOL); k(SNAP); k(RESIZE); k('\t'); k('\t');
    action(A_TERMINAL); k(WAIT_JOB); k(SNAP); action(A_STOP_LINK); k(WAIT_JOB); exit_events();
    run("real UI disk/interface selection, hidden raw secrets, confirmation, link, target terminal");
    const char *calls = slurp("calls");
    assert(strstr(calls, "helper|unlock-readonly|/dev/sdb3\n"));
    assert(strstr(calls, "helper|enable-repair|--confirm\n"));
    assert(strstr(calls, "helper|start-link|enp2s0\n"));
    assert(strstr(calls, "term|") && strstr(calls, "|target-terminal\n"));
    assert(!strstr(calls, "disk-secret") && !strstr(calls, "link-secret") && !strstr(calls, "short"));
    assert(!strstr(calls, "/bin/sh"));
    const char *inputs = slurp("stdin");
    assert(strstr(inputs, "unlock-readonly|13|6469736b2d736563726574c3bc\n"));
    assert(strstr(inputs, "enable-repair|11|6469736b2d736563726574\n"));
    assert(strstr(inputs, "start-link|14|6c696e6b2d7365637265742d3132\n"));
    assert(!sensitive_shown);
    assert(strstr(history, "readwrite") && strstr(history, "Second SSD") && strstr(history, "200.0 GiB"));
    assert(strstr(history, "at least 12 characters") && strstr(history, "did not match"));
    assert(strstr(history, "local-interface") && strstr(history, "Host key"));
    assert(!strcmp(copied[0], "SHA256:unit-test-host-key"));
    assert(screen_opens >= 2 && screen_releases >= 2 && key_opens >= 2 && key_releases >= 2);

    reset(); write_file("bad-key", "");
    k(WAIT_JOB); action(A_UNLOCK); k('\n'); k('\n'); k(SNAP); type("x"); k('\n'); k(WAIT_JOB); k(SNAP); exit_events();
    run("empty disk passphrase refused locally; one-character LUKS passphrase sent without policy restriction");
    assert(strstr(history, "A nonempty secret is required"));
    assert(strstr(history, "Wrong passphrase")); assert(strstr(slurp("stdin"), "unlock-readonly|1|78\n"));

    reset(); write_file("busy", "");
    k(WAIT_JOB); k(K_CLOSE); k(WAIT_JOB); k(SNAP); k(K_CLOSE); k(WAIT_JOB); k(SNAP); k(REMOVE_BUSY); exit_events();
    run("cleanup busy and repeated window close keep Rescue alive; retry exits only after verified cleanup");
    assert(strstr(history, "Cleanup did not finish")); assert(close_kept >= 3);

    reset(); write_file("lying-cleanup", "");
    k(WAIT_JOB); k('q'); k(WAIT_JOB); k(SNAP); k(REMOVE_LIE); exit_events();
    run("cleanup ok alone is insufficient: target must actually be closed");
    assert(strstr(history, "Cleanup did not finish"));

    reset(); write_file("hang", "");
    k(WAIT_JOB); action(A_SCAN); k(WAIT_HANG); k(K_SHOWN); k(RESIZE); k(K_PGUP); k(WAIT_JOB); k(SNAP); exit_events();
    run("helper timeout escalates TERM to KILL while input/resize keep dispatching");
    assert(strstr(history, "timed out") || strstr(history, "state is not verified"));

    reset(); write_file("hang", "");
    k(WAIT_JOB); action(A_SCAN); k(WAIT_HANG); k(K_CLOSE); k(WAIT_JOB);
    run("window close cancels a running helper without blocking before cleanup");

    reset(); write_file("malformed", "");
    k(WAIT_JOB); action(A_SCAN); k(WAIT_JOB); k(SNAP); action(A_UNLOCK); k(SNAP); exit_events();
    run("malformed JSON cannot authorize target actions");
    assert(strstr(history, "State is not verified"));

    reset(); write_file("oversize", "");
    k(WAIT_JOB); action(A_SCAN); k(WAIT_JOB); k(SNAP); exit_events();
    run("bounded output and pipe draining reject oversized helper JSON");
    assert(strstr(history, "size limit"));

    reset(); write_file("descendant", "");
    k(WAIT_JOB); action(A_SCAN); k(WAIT_JOB); k(SNAP); exit_events();
    run("child exit with inherited pipe held open has a bounded drain deadline");
    assert(strstr(history, "descendant kept"));

    reset();
    k(WAIT_JOB); action(A_SCAN); k(WAIT_JOB); k(SNAP); k(K_PGUP); k(SNAP);
    wheel(70, 18, -2); k(SNAP); click(3, 3, 3); k('y'); k(SNAP); exit_events();
    run("separate output scroll, pointer text selection and clipboard");
    assert(strstr(history, "Scan result")); assert(copied[0][0] && copied[1][0]);

    reset();
    k(WAIT_JOB); action(A_KEYMAP); k(K_F1 + 2); k(WAIT_JOB); k(SNAP);
    action(A_UNLOCK); k('\n'); type("check-layout"); k(K_F1 + 1); k(SNAP); k(K_SHOWN); k(K_F1 + 1); k(27);
    action(A_KEYMAP); type("de"); k('\n'); k(WAIT_JOB); k(SNAP); exit_events();
    run("independent US/JIS/custom layout via native keytable; reveal is explicit and not selectable");
    assert(strstr(slurp("calls"), "keytable|jp|") && strstr(slurp("calls"), "keytable|de|"));
    assert(strstr(history, "Keyboard: jp") && strstr(history, "check-layout"));
    assert(!strstr(slurp("calls"), "check-layout"));

    reset(); write_file("esp-blocked", "");
    k(WAIT_JOB); action(A_UNLOCK); k(K_DOWN); k('\n'); type("disk-secret"); k('\n'); k(WAIT_JOB); action(A_REPAIR); k(SNAP); k('q'); k(WAIT_JOB);
    run("read-only target with ambiguous ESP blocks repair and shows reason");
    assert(strstr(history, "ESP identity is ambiguous") && strstr(history, "Repair blocked"));

    reset(); write_file("old-stub", "");
    k(WAIT_JOB); action(A_SCAN); k(WAIT_JOB); k(SNAP); exit_events();
    run("older helper envelope without ESP optional fields remains accepted");

    reset(); write_file("term-fail", ""); write_file("rw", "");
    k(WAIT_JOB); action(A_TERMINAL); k(WAIT_JOB); k(SNAP); exit_events();
    run("target terminal failure returns to Rescue instead of exiting it");
    assert(strstr(history, "Target terminal failed"));
    reset(); setenv("LOCACHY_RESCUE_BOOT", "1", 1);
    k(WAIT_JOB); k('q'); k(WAIT_JOB); k(SNAP);
    k('b'); k(SNAP); k('\n'); k(SNAP); k('p'); k(SNAP); k('n'); k(SNAP);
    k('r'); k(WAIT_JOB); k(SNAP); k('q'); k(WAIT_JOB); k(SNAP); k(STOP_UI);
    run("dedicated boot cleanup shows a usable closed page, power needs Y, and a new session stays independent");
    assert(strstr(history, "Rescue session closed") && strstr(history, "Start a new rescue session"));
    assert(strstr(history, "No terminal or installer will open automatically"));
    assert(strstr(history, "Reboot this computer?") && strstr(history, "Power off this computer?"));
    assert(!strstr(slurp("calls"), "systemctl|") && !strstr(slurp("calls"), "term|"));
    assert(strstr(slurp("calls"), "helper|status\nhelper|cleanup\nhelper|status\nhelper|cleanup\n"));

    reset(); setenv("LOCACHY_RESCUE_BOOT", "1", 1); write_file("power-fail", "");
    k(WAIT_JOB); k('q'); k(WAIT_JOB); k('b'); k('y'); k(WAIT_JOB); k(SNAP); k(STOP_UI);
    run("dedicated boot power failure leaves the closed page usable");
    assert(strstr(slurp("calls"), "systemctl|reboot\n"));
    assert(strstr(history, "closed rescue session remains on screen"));

    reset(); setenv("LOCACHY_RESCUE_BOOT", "1", 1);
    k(WAIT_JOB); k('q'); k(WAIT_JOB); k('p'); k('Y'); k(WAIT_JOB); k(SNAP); k(STOP_UI);
    run("dedicated boot power off is requested only after explicit Y");
    assert(strstr(slurp("calls"), "systemctl|poweroff\n"));
    assert(strstr(history, "Shutdown requested"));
    unsetenv("LOCACHY_RESCUE_BOOT");
    printf("PASS rescue UI (native helper argv/stdin, lifecycle, timeout, clipboard and keyboard layout)\n");
    return 0;
}
