/* term in a window, closed (K_CLOSE): as a terminal closing, its child's
 * process group is hung up (SIGHUP) and term ends with status 0. The child
 * is a real shell on a real pseudo-terminal; the screen and the keys are the
 * test's own. test-term-close NEW_DIR (run by test-interaction-regressions). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define main term_main
#include "../progs/term.c"
#include "i18n-stub.h"
#undef main
#include <assert.h>

static char ready[512], hung[512];

void say(const char *fmt, ...) { (void)fmt; }
int screen_open(const char *font) { (void)font; return 0; }
int keys_open(void) { return 1; }
static int keymap_loaded;
int keys_layout_load(const char *path) { assert(path[0] == '/'); keymap_loaded++; return strcmp(path, "/bad-keymap") ? 0 : -1; }
void screen_release(void) {}
int screen_cols(void) { return 80; }
int screen_rows(void) { return 25; }
void screen_fill(int c, int r, int w, int h, uint32_t rgb) { (void)c; (void)r; (void)w; (void)h; (void)rgb; }
void screen_text(int c, int r, const char *s, uint32_t fg, uint32_t bg) { (void)c; (void)r; (void)s; (void)fg; (void)bg; }
void screen_flush(void) {}
void screen_cursor(int shape) { (void)shape; }
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
int keys_modifiers(void) { return 0; }
const struct pointer_event *pointer_last(void) { abort(); }
int clipboard_set(int primary, const void *text, size_t size) { (void)primary; (void)text; (void)size; abort(); }
int clipboard_get(int primary, int *fd, size_t *size) { (void)primary; (void)fd; (void)size; abort(); }
const char *clipboard_error(int status) { (void)status; abort(); }
double uptime(void) { return 0; }

/* A process that has ended: none, or a zombie its new parent has not
 * reaped yet. */
static int gone(pid_t pid)
{
    char path[64], text[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = '\0';
    const char *end = strrchr(text, ')');
    return !n || (end && end[1] == ' ' && (end[2] == 'Z' || end[2] == 'X'));
}

/* Nothing typed until the child's shell has set its trap (at most 5 s), then
 * the window's close. */
int key_poll_many(struct pollfd *extra, size_t count, int timeout_ms)
{
    (void)timeout_ms;
    for (size_t i = 0; i < count; i++) extra[i].revents = 0;
    for (int i = 0; i < 500 && access(ready, F_OK) != 0; i++) usleep(10000);
    assert(access(ready, F_OK) == 0);
    return K_CLOSE;
}

int main(int argc, char **argv)
{
    assert(argc == 2 && mkdir(argv[1], 0755) == 0);
    /* As process 1 starts term: SIGHUP not ignored (nohup's would be
     * inherited, and a shell cannot trap a signal ignored at its start). */
    signal(SIGHUP, SIG_DFL);
    snprintf(ready, sizeof ready, "%s/ready", argv[1]);
    snprintf(hung, sizeof hung, "%s/hung", argv[1]);
    char script[1400];
    /* The shell and a child of its own in its process group: both are hung up. */
    snprintf(script, sizeof script,
             "test -z \"${LOCACHY_KEYMAP_TABLE+x}\" || exit 19; "
             "trap 'echo \"$$ $child\" >\"%s\"; exit 0' HUP; sleep 30 & child=$!; : >\"%s\"; wait",
             hung, ready);
    char *args[] = { "term", "/bin/sh", "-c", script, NULL };
    setenv("LOCACHY_KEYMAP_TABLE", "/run/rescue/keymap.table", 1);
    int status = term_main(4, args);
    unsetenv("LOCACHY_KEYMAP_TABLE");
    assert(keymap_loaded == 1);
    assert(status == 0);
    /* term waits half a second for its child; on a busy machine the trap may
     * run after that. */
    int shell = 0, sleeper = 0;
    for (int i = 0; i < 500; i++) {
        FILE *f = fopen(hung, "r");
        int got = f && fscanf(f, "%d %d", &shell, &sleeper) == 2;
        if (f) fclose(f);
        if (got) break;
        usleep(10000);
    }
    assert(shell > 1 && sleeper > 1);
    /* The shell ended, and the sleep was hung up with it (its group's). */
    for (int i = 0; i < 500 && !gone(shell); i++) {
        waitpid(shell, NULL, WNOHANG);
        usleep(10000);
    }
    for (int i = 0; i < 500 && !gone(sleeper); i++) usleep(10000);
    assert(gone(shell) && gone(sleeper));
    puts("PASS term closed as a window: SIGHUP to its child's process group, term ends with status 0");
    setenv("LOCACHY_KEYMAP_TABLE", "/bad-keymap", 1);
    assert(term_main(1, args) == 1);
    unsetenv("LOCACHY_KEYMAP_TABLE");
    puts("PASS term rejects an unreadable explicit keyboard table");
    return 0;
}
