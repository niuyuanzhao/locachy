/* Drive the actual home handlers with host-control and screen test doubles,
 * and the pointer with synthetic events.
 * Build with base/ui.c (the click places and the menu). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define main home_program_main
#include "../progs/home.c"
#include "i18n-stub.h"
#undef main
#include <assert.h>
#include <stdarg.h>

static int start_calls, freeze_calls, stop_calls, show_calls, opens, reboots, poweroffs, files_views;
static int running;

void base_apps(char *out, size_t size)
{
    snprintf(out, size, "alpha\t%s\t1048576\t0\t\nbeta\tfrozen\t0\t1048576\t\n",
             running ? "running" : "closed");
}

int base_app_action(const char *action, const char *name, char *why, size_t size)
{
    assert(strcmp(name, "alpha") == 0 || strcmp(name, "beta") == 0);
    if (strcmp(action, "start") == 0 || strcmp(action, "resume") == 0) start_calls++;
    else if (strcmp(action, "freeze") == 0) freeze_calls++;
    else if (strcmp(action, "stop") == 0) stop_calls++;
    else if (strcmp(action, "show") == 0 || strcmp(action, "open") == 0) show_calls++;
    else assert(0);
    snprintf(why, size, "accepted");
    return 0;
}

int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    fn("program_term", "/usr/local/bin/term", arg);
    fn("program_network", "/usr/local/bin/term /usr/bin/nmtui", arg);
    return 0;
}
void base_tasks(char *out, size_t size) { if (size) out[0] = '\0'; }
void base_open(char *const argv[], char *out, size_t size)
{ (void)argv; opens++; snprintf(out, size, "returned"); }
void base_kill(char *const argv[]) { (void)argv; }
void base_reboot(void) { reboots++; }
void base_poweroff(void) { poweroffs++; }
void files_view(void) { files_views++; }
int files_request(void) { return 0; }
void base_status(const char *tag, char *out, size_t size)
{ (void)tag; snprintf(out, size, "Network: test"); }
int screen_open(const char *font) { (void)font; return 1; }
int keys_open(void) { return 1; }
int screen_cols(void) { return 80; }
int screen_char_cells(uint32_t cp) { return cp >= 0x2E80 && cp <= 0xA4CF ? 2 : 1; }
int screen_rows(void) { return 25; }
void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{ (void)col; (void)row; (void)s; (void)fg; (void)bg; }
void screen_fill(int col, int row, int width, int height, uint32_t color)
{ (void)col; (void)row; (void)width; (void)height; (void)color; }
void screen_flush(void) {}
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
/* The keys a menu gets: a script; K_POINTER takes the next of events. */
static const int *script;
static const struct pointer_event *events;
static struct pointer_event mouse;
const struct pointer_event *pointer_last(void) { return &mouse; }
int key_poll_timeout(int fd, int timeout)
{
    (void)fd; (void)timeout;
    if (!script) return 0;
    assert(*script); /* a menu waits for no more than is scripted */
    int key = *script++;
    if (key == K_POINTER) mouse = *events++;
    return key;
}
double uptime(void) { return 1.0; }
void say(const char *format, ...) { (void)format; }

/* The first cell of what was registered as id when last drawn. */
static void hit_cell(int id, int *col, int *row)
{
    for (*row = 0; *row < 25; (*row)++)
        for (*col = 0; *col < 80; (*col)++)
            if (ui_hit_at(*col, *row) == id) return;
    assert(0);
}

/* A press on id, offset columns into it: what pointer_key makes of it. */
static int press(int id, int button, int clicks, int offset)
{
    int col, row;
    hit_cell(id, &col, &row);
    mouse = (struct pointer_event){ .col = col + offset, .row = row, .button = button, .pressed = 1,
                                    .clicks = clicks };
    return pointer_key();
}

int main(void)
{
    load();
    refresh();
    assert(napps == 2 && nentries == 3 && entries[0].builtin && strcmp(entries[0].name, "files") == 0);
    selected = 0;
    apps[0].marked = 1;
    apps[1].marked = 1;
    running = 1;
    refresh();
    assert(marked_count() == 2);
    act("start");
    assert(start_calls == 2 && opens == 0);
    act("freeze");
    assert(freeze_calls == 2 && opens == 0);
    confirm_stop();
    assert(confirming == CONF_STOP && nstop == 2 && stop_calls == 0);
    confirm_key('n');
    assert(confirming == CONF_NONE && stop_calls == 0);
    confirm_stop();
    confirm_key('\n');
    assert(stop_calls == 0 && poweroffs == 0);
    confirm_stop();
    confirm_key('y');
    assert(stop_calls == 2 && opens == 0 && poweroffs == 0);
    apps[0].marked = apps[1].marked = 0;
    selected = 1;
    act("freeze");
    assert(freeze_calls == 3);
    assert(request("show", "alpha"));
    assert(show_calls == 1 && opens == 0);
    selected = napps; /* files: part of home, nothing to close */
    confirm_stop();
    assert(confirming == CONF_NONE && strstr(last, "part of home"));
    /* Reboot and power off rows: both require Y confirmation, Enter/N cancel. */
    selected = napps + nentries;
    confirming = CONF_REBOOT;
    confirm_key('\n');
    assert(confirming == CONF_NONE && reboots == 0 && strstr(last, "Reboot cancelled"));
    confirming = CONF_REBOOT;
    confirm_key('y');
    assert(confirming == CONF_NONE && reboots == 1 && poweroffs == 0);
    selected = napps + nentries + 1;
    confirming = CONF_POWEROFF;
    confirm_key('n');
    assert(confirming == CONF_NONE && poweroffs == 0 && strstr(last, "Power off cancelled"));
    confirming = CONF_POWEROFF;
    confirm_key('Y');
    assert(confirming == CONF_NONE && reboots == 1 && poweroffs == 1);
    draw(0);
    assert(press(HIT_ROW + napps + nentries + 1, BTN_LEFT, 2, 6) == '\n' && selected == napps + nentries);
    assert(press(HIT_ROW + napps + nentries + 2, BTN_LEFT, 2, 6) == '\n' && selected == napps + nentries + 1);
    selected = 0;
    draw(0);
    puts("Host UI: selection, batch actions, stop confirmation and in-place controls passed.");

    /* The pointer: a click selects a row, its box marks, a double click
     * opens; the header is nothing. */
    assert(press(HIT_ROW + 1, BTN_LEFT, 1, 10) == -1 && selected == 1);
    assert(press(HIT_ROW + 1, BTN_LEFT, 2, 10) == '\n');
    assert(press(HIT_ROW + 0, BTN_LEFT, 1, 2) == ' ' && selected == 0);
    assert(press(HIT_ROW + napps, BTN_LEFT, 2, 10) == 0 && selected == 0);
    assert(press(HIT_ROW + napps + 1, BTN_LEFT, 2, 10) == '\n' && selected == napps);
    /* The right button: the row's actions, by keys or by the pointer (the
     * opening release chooses nothing; a press outside closes it). */
    draw(0);
    script = (const int[]){ K_DOWN, K_DOWN, '\n', 0 };
    assert(press(HIT_ROW + 0, BTN_RIGHT, 1, 10) == 'f' && selected == 0);
    draw(0);
    int col, row;
    hit_cell(HIT_ROW + 1, &col, &row);
    struct pointer_event chosen[] = {
        { .col = 10, .row = row, .button = BTN_RIGHT },
        { .col = 13, .row = row + 3 },
        { .col = 13, .row = row + 3, .button = BTN_LEFT, .pressed = 1 },
        { .col = 13, .row = row + 3, .button = BTN_LEFT },
    };
    events = chosen;
    script = (const int[]){ K_POINTER, K_POINTER, K_POINTER, K_POINTER, 0 };
    assert(press(HIT_ROW + 1, BTN_RIGHT, 1, 10) == 'k' && selected == 1);
    draw(0);
    struct pointer_event outside[] = { { .col = 70, .row = 20, .button = BTN_LEFT, .pressed = 1 } };
    events = outside;
    script = (const int[]){ K_POINTER, 0 };
    assert(press(HIT_ROW + 1, BTN_RIGHT, 1, 10) == -1);
    script = NULL;
    /* The wheel moves the selection, kept on the list. */
    selected = 0;
    mouse = (struct pointer_event){ .wheel = -1 };
    assert(pointer_key() == -1 && selected == 1);
    mouse.wheel = 50;
    assert(pointer_key() == -1 && selected == 0);
    /* The buttons are their keys (the left button only); while a question
     * is asked, only its buttons. A motion is nothing. */
    draw(0);
    assert(press(HIT_KEY + 'r', BTN_LEFT, 1, 0) == 'r');
    assert(press(HIT_KEY + 'r', BTN_RIGHT, 1, 0) == 0);
    confirm_stop();
    assert(confirming == CONF_STOP);
    draw(0);
    mouse = (struct pointer_event){ .col = 10, .row = 4, .button = BTN_LEFT, .pressed = 1, .clicks = 1 };
    assert(pointer_key() == 0 && selected == 0);
    assert(press(HIT_KEY + 'n', BTN_LEFT, 1, 0) == 'n');
    confirm_key('n');
    mouse = (struct pointer_event){ .col = 5, .row = 5 };
    assert(pointer_key() == 0 && stop_calls == 2);
    puts("Host UI pointer: rows, boxes, double clicks, row menus, the wheel and buttons passed.");
    free(apps);
    free(app_data);
    return 0;
}
