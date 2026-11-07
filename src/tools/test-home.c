/* Drive the actual home handlers with host-control and screen test doubles,
 * and the pointer with synthetic events.
 * Build with base/ui.c and base/textsel.c. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
static char fixture_dir[256] = "/tmp";
#define HOME_APP_RUN fixture_dir
#define main home_program_main
#include "../progs/home.c"
#include "i18n-stub.h"
#include "clipboard.h"
#undef main
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>

static int start_calls, freeze_calls, stop_calls, show_calls, opens, reboots, poweroffs, files_views;
static int running, beta_failed;
static char copied[2][4096];
static const char *klog_fixture = "";

int clipboard_set(int primary, const void *text, size_t size)
{
    snprintf(copied[primary ? 1 : 0], sizeof copied[0], "%.*s", (int)size, (const char *)text);
    return CLIPBOARD_OK;
}
const char *clipboard_error(int status) { return status == CLIPBOARD_OK ? "ok" : "failed"; }

int home_klog_read(char *buf, int size)
{
    return snprintf(buf, (size_t)size, "%s", klog_fixture);
}

void base_apps(char *out, size_t size)
{
    snprintf(out, size, "alpha\t%s\t1048576\t0\t\nbeta\t%s\t0\t1048576\t%s\n",
             running ? "running" : "closed", beta_failed ? "failed" : "frozen",
             beta_failed ? "application exited with status 1; inspect /run/locachy/apps/beta.log" : "");
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
static char opened_output[32];
void base_open(char *const argv[], char *out, size_t size)
{ (void)argv; opens++; snprintf(out, size, "returned"); }
void base_open_on(const char *output, char *const argv[], char *out, size_t size)
{
    assert(argv && argv[0]);
    snprintf(opened_output, sizeof opened_output, "%s", output ? output : "");
    opens++;
    snprintf(out, size, "shown %s", argv[0]);
}
void base_kill(char *const argv[]) { (void)argv; }
void base_reboot(void) { reboots++; }

/* The system jobs' queue (jobs.h) as a double: the rows it lists, what was
 * asked to be cancelled; queue_started: a queued job starts before Y. */
static struct jobs_row queue_rows[4];
static int queue_n, cancels, cancel_id, cancel_confirmed, queue_started;
int jobs_list(struct jobs_row *rows, int max, char *why, size_t size)
{
    (void)why; (void)size;
    int n = queue_n < max ? queue_n : max;
    memcpy(rows, queue_rows, (size_t)n * sizeof *rows);
    return n;
}
int jobs_log(int id, char *buf, size_t size)
{
    snprintf(buf, size, "line one of %d\nlast line of %d\n", id, id);
    return 0;
}
int jobs_ended(const struct jobs_row *row)
{
    return strcmp(row->state, "running") != 0 && strcmp(row->state, "queued") != 0;
}
int jobs_cancel(int id, int confirm, char *why, size_t size)
{
    cancels++;
    cancel_id = id;
    cancel_confirmed = confirm;
    if (!confirm && queue_started) {
        snprintf(why, size, "job %d runs: stopping it can leave the system half-changed", id);
        return 1;
    }
    snprintf(why, size, "%s", confirm ? "stopping" : "removed");
    return 0;
}
int jobs_pending(struct jobs_row *running)
{
    int n = 0;
    for (int i = queue_n - 1; i >= 0; i--) {
        if (jobs_ended(&queue_rows[i])) continue;
        n++;
        *running = queue_rows[i];
    }
    return n;
}
const char *jobs_state_label(const char *state) { return state; }
void base_poweroff(void) { poweroffs++; }
void files_view(void) { files_views++; }
int files_request(void) { return 0; }
void base_status(const char *tag, char *out, size_t size)
{ (void)tag; snprintf(out, size, "Network: test"); }
int screen_open(const char *font) { (void)font; return 1; }
int keys_open(void) { return 1; }
static int test_cols = 80, test_rows = 25;
static int extended_fixture, output_fixture, output_count_fixture = 3;
static const char *const output_names[] = { "eDP-1", "HDMI-A-1", "DP-1" };
int screen_extended(int on) { int old=extended_fixture;extended_fixture=on;return old!=on; }
int screen_is_extended(void) { return extended_fixture; }
int screen_output_current(void) { return output_fixture; }
int screen_output_count(void) { return extended_fixture ? output_count_fixture : 1; }
int screen_output_get(int index, struct screen_output *out)
{
    if (index < 0 || index >= screen_output_count()) return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", output_names[index]);
    return 0;
}
int screen_output_select(int index)
{
    if (index < 0 || index >= screen_output_count()) return -1;
    int old=output_fixture;output_fixture=index;return old;
}
static char canvas[100][256];
int screen_cols(void) { return test_cols; }
int screen_char_cells(uint32_t cp) { return cp >= 0x2E80 && cp <= 0xA4CF ? 2 : 1; }
int screen_rows(void) { return test_rows; }
void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    (void)fg; (void)bg;
    if (row < 0 || row >= test_rows) return;
    for (int i = 0; s[i] && col + i < test_cols; i++) if (col + i >= 0) canvas[row][col + i] = s[i];
}
void screen_fill(int col, int row, int width, int height, uint32_t color)
{
    (void)color;
    for (int y = row; y < row + height && y < test_rows; y++)
        for (int x = col; x < col + width && x < test_cols; x++)
            if (x >= 0 && y >= 0) canvas[y][x] = ' ';
}
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

/* Some row of the screen shows text. */
static int canvas_has(const char *text)
{
    for (int r = 0; r < test_rows; r++) {
        if (strstr(canvas[r], text)) return 1;
    }
    return 0;
}

/* The first cell of what was registered as id when last drawn. */
static void hit_cell(int id, int *col, int *row)
{
    for (*row = 0; *row < test_rows; (*row)++)
        for (*col = 0; *col < test_cols; (*col)++)
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

int main(int argc, char **argv)
{
    if (argc > 1) {
        snprintf(fixture_dir, sizeof fixture_dir, "%s", argv[1]);
        mkdir(fixture_dir, 0700);
    }
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
    assert(press(HIT_ROW + napps + nentries, BTN_LEFT, 2, 6) == '\n' && selected == napps + nentries);
    assert(press(HIT_ROW + napps + nentries + 1, BTN_LEFT, 2, 6) == '\n' && selected == napps + nentries + 1);
    selected = 0;
    draw(0);
    puts("Host UI: selection, batch actions, stop confirmation and in-place controls passed.");

    /* The pointer: a click selects a row, its box marks, a double click
     * opens; the header is nothing. */
    assert(press(HIT_ROW + 1, BTN_LEFT, 1, 10) == -1 && selected == 1);
    assert(press(HIT_ROW + 1, BTN_LEFT, 2, 10) == '\n');
    assert(press(HIT_ROW + 0, BTN_LEFT, 1, 2) == ' ' && selected == 0);
    assert(press(HIT_ROW + napps, BTN_LEFT, 2, 10) == '\n' && selected == napps);
    /* The right button: the row's actions, by keys or by the pointer (the
     * opening release chooses nothing; a press outside closes it). */
    draw(0);
    script = (const int[]){ K_DOWN, K_DOWN, '\n', 0 };
    assert(press(HIT_ROW + 0, BTN_RIGHT, 1, 10) == 'f' && selected == 0);
    draw(0);
    int col, row;
    hit_cell(HIT_ROW + 1, &col, &row);
    struct pointer_event chosen[] = {
        { .col = col + 10, .row = row, .button = BTN_RIGHT },
        { .col = col + 13, .row = row + 3 },
        { .col = col + 13, .row = row + 3, .button = BTN_LEFT, .pressed = 1 },
        { .col = col + 13, .row = row + 3, .button = BTN_LEFT },
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
    assert(pointer_key() == -1 && selected == grid_columns());
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
    test_cols = 160; test_rows = 35; selected = scroll = 0;
    draw(0);
    assert(grid_columns() == 5);
    int x0, y0, x1, y1;
    hit_cell(HIT_ROW, &x0, &y0); hit_cell(HIT_ROW + 1, &x1, &y1);
    assert(y0 == y1 && x1 > x0);
    hit_cell(HIT_ROW + napps, &x1, &y1);
    assert(y0 == y1 && x1 > x0);
    assert(strstr(canvas[y0 + 1], "running") && strstr(canvas[y0 + 1], "MiB"));
    move_selection(K_RIGHT); assert(selected == 1);
    move_selection(K_DOWN); assert(selected == 6);
    move_selection(K_UP); assert(selected == 1);
    move_selection(K_LEFT); assert(selected == 0);
    assert(press(HIT_ROW + 1, BTN_LEFT, 1, 2) == ' ' && selected == 1);
    test_cols = 110; draw(0); assert(grid_columns() == 4);
    test_cols = 24; test_rows = 12; selected = scroll = 0;
    draw(0); assert(grid_columns() == 1);
    move_selection(K_EOL); draw(0);
    hit_cell(HIT_ROW + napps + nentries + 1, &x0, &y0);
    assert(x0 >= 0 && x0 < test_cols && y0 < test_rows - footer_rows());
    puts("PASS responsive home grid: five/four/one columns, state and memory retained, two-dimensional navigation");
    test_cols = 54; test_rows = 11; selected = scroll = 0;
    screen_extended(1);
    draw(0);
    assert(output_first[0] == 0 && output_first[1] == 0 && output_first[2] == 0);
    for (int i = 0; i < 3; i++) {
        screen_output_select(i);
        hit_cell(HIT_ROW, &x0, &y0);
        assert(ui_hit_at(x0, y0) == HIT_ROW);
        mouse = (struct pointer_event){ .output=i, .col=x0+5, .row=y0, .button=BTN_LEFT, .pressed=1, .clicks=2 };
        assert(pointer_key() == '\n' && selected == 0 && home_output == i);
        draw(0);
        assert(home_output == i && screen_output_current() == i);
    }
    selected = 3;
    draw(0);
    for (int i = 0; i < 3; i++) assert(output_first[i] == 2);
    screen_output_select(1);
    hit_cell(HIT_ROW + 2, &x1, &y1);
    mouse = (struct pointer_event){ .output=1, .col=x1+5, .row=y1, .button=BTN_LEFT, .pressed=1, .clicks=2 };
    assert(pointer_key() == '\n' && selected == 2 && home_output == 1);
    draw(0);
    /* A menu opened on one output: a press at its cells on another closes it
     * and chooses nothing; on its own output the same press chooses. */
    hit_cell(HIT_ROW + 3, &x1, &y1);
    struct pointer_event elsewhere[] = {
        { .output = 1, .col = x1 + 5, .row = y1, .button = BTN_RIGHT },
        { .output = 0, .col = x1 + 7, .row = y1 + 1 },
        { .output = 0, .col = x1 + 7, .row = y1 + 1, .button = BTN_LEFT, .pressed = 1 },
        { .output = 0, .col = x1 + 7, .row = y1 + 1, .button = BTN_LEFT },
    };
    events = elsewhere;
    script = (const int[]){ K_POINTER, K_POINTER, K_POINTER, K_POINTER, 0 };
    mouse = (struct pointer_event){ .output = 1, .col = x1 + 5, .row = y1, .button = BTN_RIGHT, .pressed = 1 };
    assert(pointer_key() == -1 && selected == 3 && *script == K_POINTER); /* the release never read */
    struct pointer_event here[] = {
        { .output = 1, .col = x1 + 5, .row = y1, .button = BTN_RIGHT },
        { .output = 1, .col = x1 + 7, .row = y1 + 1 },
        { .output = 1, .col = x1 + 7, .row = y1 + 1, .button = BTN_LEFT, .pressed = 1 },
        { .output = 1, .col = x1 + 7, .row = y1 + 1, .button = BTN_LEFT },
    };
    events = here;
    script = (const int[]){ K_POINTER, K_POINTER, K_POINTER, K_POINTER, 0 };
    mouse = (struct pointer_event){ .output = 1, .col = x1 + 5, .row = y1, .button = BTN_RIGHT, .pressed = 1 };
    assert(pointer_key() == 'k' && selected == 3 && !*script);
    script = NULL;
    puts("PASS extended menu: a press on another output closes it and chooses nothing");
    test_cols = 160; test_rows = 35; selected = napps + 1; scroll = 0;
    memset(output_scroll, 0, sizeof output_scroll);
    draw(0);
    /* The first output can hold the entire list: the other two must still
     * offer every program and keep their activation's connector. */
    for (int i = 0; i < 3; i++) {
        screen_output_select(i);
        hit_cell(HIT_ROW + napps + 1, &x0, &y0);
        mouse = (struct pointer_event){ .output=i, .col=x0+5, .row=y0, .button=BTN_LEFT, .pressed=1, .clicks=2 };
        assert(pointer_key() == '\n' && selected == napps + 1);
        draw(0);
        open_entry(1);
        assert(strcmp(opened_output, output_names[i]) == 0 && home_output == i);
        /* Motion and the common Open button do not revert to output 0. */
        mouse = (struct pointer_event){ .output=i, .col=5, .row=2 };
        pointer_key();
        draw(0);
        hit_cell(HIT_KEY + '\n', &x0, &y0);
        mouse = (struct pointer_event){ .output=i, .col=x0, .row=y0, .button=BTN_LEFT, .pressed=1, .clicks=1 };
        assert(pointer_key() == '\n');
        open_entry(1);
        assert(strcmp(opened_output, output_names[i]) == 0);
    }
    screen_extended(0);
    puts("PASS three-output home: every screen offers every program; double click, redraw and Open keep its connector");

    /* The system jobs: J lists them with their state and the selected one's
     * output; reboot and power off are refused while one runs or waits; K
     * asks first: Y stops a running one (confirmed to the queue), removes a
     * queued one; N leaves it; one that has ended is not asked about. */
    queue_rows[0] = (struct jobs_row){ .id = 4, .kind = "pkg-install", .state = "running", .status = -1,
                                       .from = "settings", .args = "tailscale" };
    queue_rows[1] = (struct jobs_row){ .id = 5, .kind = "repos-rank", .state = "queued", .status = -1,
                                       .position = 1, .from = "settings" };
    queue_rows[2] = (struct jobs_row){ .id = 3, .kind = "boot", .state = "failed", .status = 1, .position = -1,
                                       .from = "settings" };
    queue_n = 3;
    refresh();
    selected = 0;
    confirming = CONF_NONE;
    draw(0);
    assert(canvas_has("system jobs: 1 running, 1 waiting (J)"));
    int reboots_before = reboots, poweroffs_before = poweroffs;
    confirming = CONF_REBOOT;
    confirm_key('y');
    assert(reboots == reboots_before && strstr(last, "Reboot refused: 2 system jobs run or wait (job 4, pkg-install)"));
    confirming = CONF_POWEROFF;
    confirm_key('y');
    assert(poweroffs == poweroffs_before && strstr(last, "Power off refused"));
    jobs_shown = 1;
    job_selected = 0;
    draw(0);
    assert(canvas_has("pkg-install") && canvas_has("tailscale  [settings]") && canvas_has("(1 before it)") &&
           canvas_has("(status 1)") && canvas_has("Output of job 4") && canvas_has("last line of 4"));
    confirm_job();
    assert(confirming == CONF_JOB && job_asked == 4 && job_asked_running);
    draw(0);
    assert(canvas_has("Y/N: Stop system job 4 while it runs?") && canvas_has("half-changed"));
    confirm_key('n');
    assert(confirming == CONF_NONE && cancels == 0 && strstr(last, "left as it is"));
    confirm_job();
    confirm_key('y');
    assert(cancels == 1 && cancel_id == 4 && cancel_confirmed == 1 && strstr(last, "System job 4: stopping."));
    job_selected = 1;
    confirm_job();
    assert(confirming == CONF_JOB && job_asked == 5 && !job_asked_running);
    confirm_key('y');
    assert(cancels == 2 && cancel_id == 5 && !cancel_confirmed && strstr(last, "removed from the queue"));
    queue_started = 1;
    confirm_job();
    confirm_key('y');
    assert(cancels == 3 && !cancel_confirmed && strstr(last, "started meanwhile: K again to stop it"));
    job_selected = 2;
    confirm_job();
    assert(confirming == CONF_NONE && cancels == 3 && strstr(last, "has ended"));
    draw(0);
    assert(press(HIT_JOB + 0, BTN_LEFT, 1, 2) == -1 && job_selected == 0);
    queue_n = 0;
    refresh();
    jobs_shown = 0;
    confirming = CONF_REBOOT;
    confirm_key('y');
    assert(reboots == reboots_before + 1);
    puts("PASS system jobs: listed with state and output; reboot and power off refused while one runs or waits; "
         "stopped or removed only after Y");

    /* Logs (L): on a host row with a failed container, L opens that failed
     * container's log; Tab toggles the filtered LOCachy system log; wheel/keys
     * scroll; Y copies the whole log and y on the grid copies the failure. */
    test_cols = 140; test_rows = 25;
    beta_failed = 1;
    refresh();
    selected = napps; /* host row ("files"), while beta has failed */
    char beta_log[320];
    snprintf(beta_log, sizeof beta_log, "%s/beta.log", fixture_dir);
    FILE *lf = fopen(beta_log, "we");
    assert(lf);
    for (int i = 1; i <= 25; i++) fprintf(lf, "beta log line %02d: Wayland display disconnected\n", i);
    fclose(lf);
    klog_fixture = "<6>[   12.340000] usb 1-1: noise from kernel driver\n"
                   "<6>[   45.100000] seat: released in 52 ms (gestures 50 ms)\n"
                   "<6>[   45.110000] init: ending locachy-seat to take the screens back\n";
    draw(0);
    assert(canvas_has("inspect /run/locachy/apps/beta.log") && canvas_has("L: logs"));
    copied[0][0] = '\0';
    copy_log(0);
    assert(strstr(copied[0], "inspect /run/locachy/apps/beta.log"));
    open_logs();
    assert(log_shown && !log_system && strcmp(log_name, "beta") == 0);
    draw(0);
    assert(canvas_has("beta.log") && canvas_has("beta log line 25") && !canvas_has("beta log line 01"));
    assert(canvas_has("Tab: system log") && canvas_has("Esc: back"));
    mouse = (struct pointer_event){ .wheel = 2 };
    assert(pointer_key() == -1 && log_from == 6);
    draw(0);
    assert(canvas_has("beta log line 19") && !canvas_has("beta log line 25"));
    copied[0][0] = '\0';
    copy_log(1);
    assert(strstr(copied[0], "beta log line 01") && strstr(copied[0], "beta log line 25"));
    /* Tab switches to the system log (LOCachy lines kept, unrelated driver noise skipped). */
    log_system = 1; log_from = 0;
    draw(0);
    assert(canvas_has("Log: system log") && canvas_has("Tab: container log") &&
           canvas_has("init: ending locachy-seat") && !canvas_has("usb 1-1: noise"));
    copied[0][0] = '\0';
    copy_log(1);
    assert(strstr(copied[0], "seat: released in 52 ms") && !strstr(copied[0], "usb 1-1: noise"));
    open_logs();
    assert(!log_shown);
    unlink(beta_log);
    puts("PASS home logs (L): failed container log and filtered system log, scroll and copy");
    free(apps);
    free(app_data);
    return 0;
}
