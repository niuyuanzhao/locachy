/*
 * home manages containers above the host's program_NAME launchers.
 * Container actions are asynchronous requests, never base_open calls. Only
 * base_apps supplies their displayed state and memory usage. Enter opens: a
 * closed container starts, a running one is shown; the compositor remains one
 * shared host session. The first host entry, files, is home's own file manager
 * (files.c), also shown when a container asks for a folder.
 *
 * Requires these base interfaces (implemented alongside this UI):
 *   void base_apps(char *buf, size_t size);
 *   int base_app_action(const char *action, const char *name,
 *                       char *why, size_t size);
 * Lists contain name, state, RAM bytes, swap bytes and error, tab-separated.
 * Actions start/resume/freeze/stop/show return 0 for accepted, -1 for error.
 */
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"
#include "files.h"

void base_apps(char *buf, size_t size);
int base_app_action(const char *action, const char *name, char *why, size_t size);

#define FG     0xD0D0D0
#define BG     0x101820
#define ACCENT 0x80C0FF
#define ERROR  0xFF9090
#define MAX_ENTRIES 16
#define REFRESH_MS 500
#define MIB 1048576ULL
#define HIT_ROW 0        /* + the list position */
#define HIT_KEY 100000   /* + the key a button stands for */

struct entry {
    char name[16];
    char spec[256]; /* the command, split in place */
    char *argv[16];
    char key[256];  /* the command joined with spaces: the task's name */
    int builtin;    /* files: part of home, not a task */
};

struct app {
    char *name, *state, *error; /* owned by app_data */
    unsigned long long ram, swap;
    int marked;               /* operation target, not running state */
};

enum confirmation { CONF_NONE, CONF_STOP, CONF_PROGRAM, CONF_REBOOT, CONF_POWEROFF };

static struct entry entries[MAX_ENTRIES];
static int nentries, selected, scroll;
static struct app *apps;
static char *app_data;
static int napps;
static char tasks[2048];
static char last[512] = N_("Container actions stay here; state refreshes every 500 ms.");
static int last_customized;
static char action_error[512], list_error[160];
static double refresh_at;
static enum confirmation confirming;
static char **stop_names;
static int nstop, stop_program;
#define HOME_OUTPUTS 32
static int home_output;
static int output_first[HOME_OUTPUTS], output_scroll[HOME_OUTPUTS];

/* One program_NAME="PATH ARGS" setting of rc.conf. */
static void program(const char *name, const char *value, void *arg)
{
    (void)arg;
    if (strncmp(name, "program_", 8) != 0 || name[8] == '\0' ||
        strcmp(name + 8, "desktop") == 0 || strcmp(name + 8, "files") == 0 ||
        nentries == MAX_ENTRIES) {
        return; /* The compositor is shared container infrastructure, not a host menu entry. */
    }
    struct entry *e = &entries[nentries];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name + 8);
    snprintf(e->spec, sizeof e->spec, "%s", value);
    int argc = 0;
    size_t len = 0;
    for (char *w = strtok(e->spec, " \t"); w != NULL && argc < 15;
         w = strtok(NULL, " \t")) {
        e->argv[argc++] = w;
        len += (size_t)snprintf(e->key + len, sizeof e->key - len,
                               argc > 1 ? " %s" : "%s", w);
    }
    e->argv[argc] = NULL;
    if (argc > 0) {
        nentries++;
    }
}

static void load(void)
{
    int host = selected - napps;
    int power = host == nentries;
    char name[sizeof entries[0].name] = "";
    if (host >= 0 && host < nentries) {
        snprintf(name, sizeof name, "%s", entries[host].name);
    }
    memset(&entries[0], 0, sizeof entries[0]);
    snprintf(entries[0].name, sizeof entries[0].name, "files");
    entries[0].builtin = 1;
    nentries = 1;
    rc_read(program, NULL);
    if (host < 0) {
        return;
    }
    selected = napps + (power ? nentries : 0);
    for (int i = 0; name[0] && i < nentries; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            selected = napps + i;
            break;
        }
    }
}

static int valid_name(const char *name)
{
    if (!*name) {
        return 0;
    }
    for (; *name; name++) {
        unsigned char c = (unsigned char)*name;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static int valid_state(const char *state)
{
    static const char *const states[] = {
        "closed", "starting", "opening", "running", "freezing", "frozen", "stopping", "failed"
    };
    for (size_t i = 0; i < sizeof states / sizeof states[0]; i++) {
        if (strcmp(state, states[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *state_label(const char *state)
{
    if (strcmp(state, "closed") == 0) return _("closed");
    if (strcmp(state, "starting") == 0) return _("starting");
    if (strcmp(state, "opening") == 0) return _("opening");
    if (strcmp(state, "running") == 0) return _("running");
    if (strcmp(state, "freezing") == 0) return _("freezing");
    if (strcmp(state, "frozen") == 0) return _("frozen");
    if (strcmp(state, "stopping") == 0) return _("stopping");
    if (strcmp(state, "failed") == 0) return _("failed");
    return state;
}

static int bytes(const char *s, unsigned long long *out)
{
    unsigned long long n = 0;
    if (!*s) {
        return 0;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > (ULLONG_MAX - (*s - '0')) / 10) {
            return 0;
        }
        n = n * 10 + (unsigned)(*s - '0');
    }
    *out = n;
    return 1;
}

/* Replace one snapshot, retaining marks and focus by full, validated name. */
static void load_apps(void)
{
    size_t size = 8192;
    char *data = NULL;
    for (;;) {
        char *p = realloc(data, size);
        if (p == NULL) {
            free(data);
            snprintf(list_error, sizeof list_error, _("Cannot refresh containers: out of memory."));
            return; /* Keep the last good snapshot, including its marks. */
        }
        data = p;
        data[0] = data[size - 1] = '\0';
        base_apps(data, size);
        data[size - 1] = '\0';
        if (strlen(data) < size - 1) {
            break;
        }
        if (size > (size_t)-1 / 2) {
            free(data);
            snprintf(list_error, sizeof list_error, _("Cannot refresh containers: list too large."));
            return;
        }
        size *= 2;
    }

    size_t capacity = 1;
    for (char *p = data; *p; p++) {
        capacity += *p == '\n';
    }
    if (capacity > (size_t)(INT_MAX - MAX_ENTRIES - 2) ||
        capacity > (size_t)-1 / sizeof(struct app)) {
        free(data);
        snprintf(list_error, sizeof list_error, _("Cannot refresh containers: too many rows."));
        return;
    }
    struct app *next = calloc(capacity, sizeof *next);
    if (next == NULL) {
        free(data);
        snprintf(list_error, sizeof list_error, _("Cannot refresh containers: out of memory."));
        return;
    }

    int count = 0, invalid = 0, focus = -1;
    int host = selected - napps;
    const char *focus_name = host < 0 ? apps[selected].name : NULL;
    for (char *line = data; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end++ = '\0';
        }
        char *field[5] = { line };
        int fields = 1;
        while (fields < 5) {
            char *tab = strchr(field[fields - 1], '\t');
            if (!tab) {
                break;
            }
            *tab = '\0';
            field[fields++] = tab + 1;
        }
        struct app a = { 0 };
        int valid = fields == 5 && valid_name(field[0]) && valid_state(field[1]) &&
                    bytes(field[2], &a.ram) && bytes(field[3], &a.swap);
        for (int i = 0; valid && i < count; i++) {
            if (strcmp(next[i].name, field[0]) == 0) {
                valid = 0;
            }
        }
        if (valid) {
            a.name = field[0];
            a.state = field[1];
            a.error = field[4];
            for (char *p = a.error; *p; p++) {
                if ((unsigned char)*p < 32 || (unsigned char)*p == 127) {
                    *p = ' ';
                }
            }
            for (int i = 0; i < napps; i++) {
                if (strcmp(apps[i].name, a.name) == 0) {
                    a.marked = apps[i].marked;
                    break;
                }
            }
            if (focus_name && strcmp(focus_name, a.name) == 0) {
                focus = count;
            }
            next[count++] = a;
        } else {
            invalid++;
        }
        line = end;
    }
    if (host >= 0) {
        selected = count + host;
    } else if (focus >= 0) {
        selected = focus;
    } else {
        selected = count; /* A vanished target must not select a different container. */
    }
    free(apps);
    free(app_data);
    apps = next;
    app_data = data;
    napps = count;
    list_error[0] = '\0';
    if (invalid) {
        snprintf(list_error, sizeof list_error,
                 P_("Ignored %d invalid container record.",
                    "Ignored %d invalid container records.", invalid), invalid);
    }
}

static void refresh(void)
{
    load_apps();
    base_tasks(tasks, sizeof tasks);
    refresh_at = uptime() + REFRESH_MS / 1000.0;
}

/* What runs in the task called key, from base_tasks, or "not running". */
static const char *task_of(const char *key, char *out, size_t size)
{
    size_t k = strlen(key);
    const char *line = tasks;

    while (line != NULL && *line != '\0') {
        const char *next = strchr(line, '\n');
        size_t linelen = next ? (size_t)(next - line) : strlen(line);
        if (linelen > k && strncmp(line, key, k) == 0 && line[k] == ':') {
            const char *rest = line + k + 1;
            while (rest < line + linelen && (*rest == ' ' || *rest == '\t')) {
                rest++;
            }
            int rlen = (int)(line + linelen - rest);
            snprintf(out, size, "%.*s", rlen < (int)size ? rlen : (int)size - 1, rest);
            return out;
        }
        line = next ? next + 1 : NULL;
    }
    return _("not running");
}

static int marked_count(void)
{
    int count = 0;
    for (int i = 0; i < napps; i++) {
        count += apps[i].marked != 0;
    }
    return count;
}

/* All drawing is bounded by physical character cells. */
static void text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    if (col >= 0 && col < screen_cols() && row >= 0 && row < screen_rows()) {
        screen_text(col, row, s, fg, bg);
    }
}

static void wrapped(int row, int lines, const char *s, uint32_t color)
{
    int width = screen_cols() - 2;
    if (width <= 0) {
        return;
    }
    char part[512];
    for (int i = 0; i < lines && *s; i++) {
        const unsigned char *start = (const unsigned char *)s, *p = start, *cut = p, *prev = p;
        int cells = 0;
        while (*p) {
            const unsigned char *c0 = p;
            unsigned char b = *p++;
            int more = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
            uint32_t cp = b & (0x3Fu >> more);
            for (; more > 0 && (*p & 0xC0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3F);
            int w = screen_char_cells(cp);
            if (cells + w > width || (size_t)(p - start) >= sizeof part) {
                p = c0;
                break;
            }
            prev = cut;
            cells += w;
            cut = p;
        }
        if (cut == start && *p) {
            cut = p;
        }
        s = (const char *)cut;
        if (i == lines - 1 && *s) {
            size_t n = (size_t)((cells >= width && prev > start ? prev : cut) - start);
            memcpy(part, start, n);
            if (n + 1 < sizeof part) {
                part[n++] = '~';
            }
            part[n] = '\0';
        } else {
            size_t n = (size_t)(cut - start);
            memcpy(part, start, n);
            part[n] = '\0';
        }
        text(1, row + i, part, color, BG);
    }
}

static int footer_rows(void)
{
    return 4;
}

static int visible_rows(void)
{
    int n = screen_rows() - footer_rows() - 3;
    return n > 0 ? n : 1;
}

static int grid_columns(void)
{
    int n = (screen_cols() - 2) / 26;
    return n < 1 ? 1 : n > 5 ? 5 : n;
}

static int grid_width(void)
{
    int n = (screen_cols() - 2) / grid_columns();
    return n > 0 ? n : 1;
}

static int grid_visible(void)
{
    int n = visible_rows() / 3;
    return n > 0 ? n : 1;
}

static void grid_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    char out[512];
    size_t n = 0;
    int cells = 0, width = grid_width() - 2;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n + 5 < sizeof out) {
        const unsigned char *start = p;
        unsigned char b = *p++;
        int more = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : 3;
        uint32_t cp = b & (0x7Fu >> more);
        for (; more && (*p & 0xC0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3F);
        int w = screen_char_cells(cp);
        if (cells + w > width) break;
        memcpy(out + n, start, (size_t)(p - start));
        n += (size_t)(p - start);
        cells += w;
    }
    out[n] = '\0';
    text(col, row, out, fg, bg);
}

static void move_selection(int key)
{
    int columns = grid_columns(), last_item = napps + nentries + 1;
    int step = key == K_PGUP || key == K_PGDN ? grid_visible() * columns :
               key == K_UP || key == K_DOWN ? columns : 1;
    if (key == K_BOL) selected = 0;
    else if (key == K_EOL) selected = last_item;
    else selected += key == K_UP || key == K_LEFT || key == K_PGUP ? -step : step;
    if (selected < 0) selected = 0;
    if (selected > last_item) selected = last_item;
}

static const struct app *error_app(void)
{
    if (selected < napps && (apps[selected].error[0] ||
                             strcmp(apps[selected].state, "failed") == 0)) {
        return &apps[selected];
    }
    for (int i = 0; i < napps; i++) {
        if (apps[i].error[0] || strcmp(apps[i].state, "failed") == 0) {
            return &apps[i];
        }
    }
    return NULL;
}

/* ready also tells the log that home waits for keys (drive.py's prompt). */
static void draw_output(int ready)
{
    char line[1024], state[200], failure[1024] = "";
    int cols = screen_cols(), rows = screen_rows();
    int footer = rows - footer_rows(), visible = visible_rows();
    if (cols > 0 && rows > 0) {
        screen_fill(0, 0, cols, rows, BG);
    }
    ui_hits_clear();
    screen_bar(_("home"), NULL);
    snprintf(line, sizeof line, _("%d containers   %d marked (operation targets only)"),
             napps, marked_count());
    text(1, 1, line, ACCENT, BG);

    if (confirming != CONF_NONE) {
        if (confirming == CONF_STOP) {
            /* TRANSLATORS: Y and N are the confirmation keys; keep them. */
            snprintf(line, sizeof line,
                     P_("Y/N: Stop %d container completely?",
                        "Y/N: Stop %d containers completely?", nstop), nstop);
        } else if (confirming == CONF_PROGRAM) {
            /* TRANSLATORS: Y and N are the confirmation keys; keep them. %s is a host program name. */
            snprintf(line, sizeof line, _("Y/N: Close host program %s?"), entries[stop_program].name);
        } else if (confirming == CONF_REBOOT) {
            /* TRANSLATORS: Y and N are the confirmation keys; keep them. */
            snprintf(line, sizeof line, _("Y/N: Reboot the host?"));
        } else {
            /* TRANSLATORS: Y and N are the confirmation keys; keep them. */
            snprintf(line, sizeof line, _("Y/N: Power off the host?"));
        }
        text(1, 2, line, ERROR, BG);
        if (confirming == CONF_STOP) {
            for (int i = 0; i < nstop && i < visible; i++) {
                if (i == visible - 1 && nstop > visible) {
                    snprintf(line, sizeof line,
                             P_("... %d more container",
                                "... %d more containers", nstop - i), nstop - i);
                    text(1, 3 + i, line, FG, BG);
                } else {
                    text(1, 3 + i, stop_names[i], FG, BG);
                }
            }
        }
    } else {
        int columns = grid_columns(), width = grid_width(), shown = grid_visible();
        int focus = selected / columns, total = (napps + nentries + 2 + columns - 1) / columns;
        if (scroll > focus) scroll = focus;
        if (focus >= scroll + shown) scroll = focus - shown + 1;
        int max_scroll = total > shown ? total - shown : 0;
        if (scroll > max_scroll) scroll = max_scroll;
        int first = scroll * columns;
        for (int pos = first; pos < napps + nentries + 2; pos++) {
            int row = 3 + ((pos - first) / columns) * 3, col = 1 + ((pos - first) % columns) * width;
            if (row + 1 >= footer) break;
            uint32_t fg = pos == selected ? BG : FG, bg = pos == selected ? ACCENT : BG;
            screen_fill(col, row, width - 1, 2, bg);
            if (pos < napps) {
                if (pos != selected && strcmp(apps[pos].state, "failed") == 0) fg = ERROR;
                snprintf(line, sizeof line, "[%c] %s", apps[pos].marked ? 'x' : ' ', apps[pos].name);
                grid_text(col + 1, row, line, fg, bg);
                snprintf(line, sizeof line, "%s  %llu/%llu MiB", state_label(apps[pos].state),
                         apps[pos].ram / MIB, apps[pos].swap / MIB);
            } else if (pos < napps + nentries) {
                int i = pos - napps;
                grid_text(col + 1, row, entries[i].name, fg, bg);
                snprintf(line, sizeof line, "%s", entries[i].builtin ? _("home and the shared folders") :
                         task_of(entries[i].key, state, sizeof state));
            } else {
                grid_text(col + 1, row, pos == napps + nentries ? _("reboot") : _("power off"), fg, bg);
                line[0] = '\0';
            }
            grid_text(col + 1, row + 1, line, fg, bg);
            ui_hit(col, row, width - 1, 2, HIT_ROW + pos);
        }
        text(1, 2, _("Containers: state, RAM / swap MiB"), FG, BG);
    }

    const struct app *a = error_app();
    if (a) {
        snprintf(failure, sizeof failure, "%s: %s [%.80s]", state_label(a->state),
                 a->error[0] ? a->error : _("No failure reason reported."), a->name);
    }
    wrapped(footer, 1, last_customized ? last : _(last), ACCENT);
    wrapped(footer + 1, 1, action_error[0] ? action_error : list_error[0] ? list_error : failure, ERROR);
    if (confirming != CONF_NONE) {
        /* TRANSLATORS: Y and N are the confirmation keys; keep them. */
        int c = ui_button(1, rows - 2, _("Y: confirm"), HIT_KEY + 'y');
        ui_button(c, rows - 2, _("N: cancel"), HIT_KEY + 'n');
        text(1, rows - 1, _("Any other key cancels without acting."), FG, BG);
    } else {
        /* TRANSLATORS: Enter, Space, R, F, K are key names; keep them. */
        int c = ui_button(1, rows - 2, _("Enter: open"), HIT_KEY + '\n');
        c = ui_button(c, rows - 2, _("Space: mark"), HIT_KEY + ' ');
        c = ui_button(c, rows - 2, _("R: start/resume"), HIT_KEY + 'r');
        c = ui_button(c, rows - 2, _("F: freeze"), HIT_KEY + 'f');
        ui_button(c, rows - 2, _("K: stop/close"), HIT_KEY + 'k');
        /* TRANSLATORS: Up/Down/PgUp/PgDn and Super+Esc are key names; keep them. */
        text(1, rows - 1, _("Arrows/PgUp/PgDn or the wheel: move   double click: open   "
                            "right click: actions   Super+Esc: home"), FG, BG);
    }
    screen_flush();
    if (ready) {
        say("home> ");
    }
}

static void draw(int ready)
{
    int count = screen_is_extended && screen_is_extended() && screen_output_count ? screen_output_count() : 1;
    if (count <= 1 || !screen_output_select) {
        home_output = 0;
        draw_output(ready);
        return;
    }
    if (count > HOME_OUTPUTS) count = HOME_OUTPUTS;
    if (home_output < 0 || home_output >= count) home_output = 0;
    output_scroll[home_output] = scroll;
    /* Every output is a launcher, not a slice of one shared list. Drawing
     * another output must not change where an activation came from. */
    for (int i = 0; i < count; i++) {
        screen_output_select(i);
        scroll = output_scroll[i];
        draw_output(0);
        output_scroll[i] = scroll;
        output_first[i] = scroll * grid_columns();
    }
    screen_output_select(home_output);
    scroll = output_scroll[home_output];
    if (ready) say("home> ");
}

static void view_files(void)
{
    int was = screen_is_extended && screen_is_extended();
    if (was) screen_extended(0);
    files_view();
    if (was) screen_extended(1);
}

/* Accepted is not completed: no local state changes, including for show. */
static int request(const char *action, const char *name)
{
    char why[256] = "";
    int rc = base_app_action(action, name, why, sizeof why);
    why[sizeof why - 1] = '\0';
    for (char *p = why; *p; p++) {
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127) {
            *p = ' ';
        }
    }
    say("home: %s %s: %s%s%s", action, name, rc == 0 ? "accepted" : "error",
        why[0] ? ": " : "", why);
    if (rc != 0 && !action_error[0]) {
        snprintf(action_error, sizeof action_error, "%s %s: %s", action, name,
                 why[0] ? why : _("Request failed without a reason."));
    }
    return rc == 0;
}

static void act(const char *action)
{
    refresh();
    int marked = marked_count(), accepted = 0, errors = 0;
    if (!marked && selected >= napps) {
        last_customized = 1;
        snprintf(last, sizeof last, _("Select a container or mark containers first."));
        return;
    }
    action_error[0] = '\0';
    for (int i = 0; i < napps; i++) {
        if (marked ? apps[i].marked : i == selected) {
            const char *verb = strcmp(action, "start") == 0 &&
                               strcmp(apps[i].state, "frozen") == 0 ? "resume" : action;
            if (request(verb, apps[i].name)) {
                accepted++;
            } else {
                errors++;
            }
        }
    }
    last_customized = 1;
    if (strcmp(action, "start") == 0) {
        snprintf(last, sizeof last,
                 _("Start/resume: %d accepted, %d error(s); state is refreshed from base."),
                 accepted, errors);
    } else {
        snprintf(last, sizeof last,
                 _("Freeze: %d accepted, %d error(s); state is refreshed from base."),
                 accepted, errors);
    }
    refresh();
}

static void clear_confirmation(void)
{
    for (int i = 0; i < nstop; i++) {
        free(stop_names[i]);
    }
    free(stop_names);
    stop_names = NULL;
    nstop = 0;
    confirming = CONF_NONE;
}

/* Capture names, not row numbers: periodic refresh may reorder or remove rows. */
static void confirm_stop(void)
{
    refresh();
    int marked = marked_count();
    if (!marked && selected >= napps) {
        int host = selected - napps;
        if (host < nentries && entries[host].builtin) {
            last_customized = 1;
            snprintf(last, sizeof last, _("%s is part of home: nothing to close."), entries[host].name);
        } else if (host < nentries) {
            stop_program = host;
            confirming = CONF_PROGRAM;
        } else {
            last_customized = 1;
            /* TRANSLATORS: K is the key name; keep it. */
            snprintf(last, sizeof last, _("K does not power off the host."));
        }
        return;
    }
    int count = marked ? marked : 1;
    stop_names = calloc((size_t)count, sizeof *stop_names);
    if (stop_names) {
        for (int i = 0; i < napps; i++) {
            if (marked ? apps[i].marked : i == selected) {
                size_t size = strlen(apps[i].name) + 1;
                stop_names[nstop] = malloc(size);
                if (!stop_names[nstop]) {
                    break;
                }
                memcpy(stop_names[nstop++], apps[i].name, size);
            }
        }
    }
    if (nstop != count) {
        clear_confirmation();
        snprintf(action_error, sizeof action_error, _("Cannot prepare stop confirmation: out of memory."));
        return;
    }
    confirming = CONF_STOP;
}

/* Consume every confirmation key, so Enter cannot fall through to power off. */
static void confirm_key(int key)
{
    if (key != 'y' && key != 'Y') {
        last_customized = 1;
        snprintf(last, sizeof last, "%s",
                 confirming == CONF_REBOOT ? _("Reboot cancelled.") :
                 confirming == CONF_POWEROFF ? _("Power off cancelled.") : _("Stop cancelled."));
        clear_confirmation();
        return;
    }
    action_error[0] = '\0';
    if (confirming == CONF_STOP) {
        int accepted = 0;
        for (int i = 0; i < nstop; i++) {
            accepted += request("stop", stop_names[i]);
        }
        last_customized = 1;
        snprintf(last, sizeof last,
                 _("Stop: %d accepted, %d error(s); state is refreshed from base."),
                 accepted, nstop - accepted);
    } else if (confirming == CONF_PROGRAM) {
        base_kill(entries[stop_program].argv);
        last_customized = 1;
        snprintf(last, sizeof last, _("Close requested for host program %s."), entries[stop_program].name);
    } else if (confirming == CONF_REBOOT) {
        clear_confirmation();
        say("home: reboot");
        base_reboot();
        return;
    } else if (confirming == CONF_POWEROFF) {
        clear_confirmation();
        say("home: power off");
        base_poweroff();
        return;
    }
    clear_confirmation();
    refresh();
}

/* The actions for the selected row, as their keys. */
static int row_menu(int col, int row)
{
    static const int container_keys[] = { '\n', 'r', 'f', 'k', ' ' }, program_keys[] = { '\n', 'k' };
    const char *container[] = {
        _("Open"), _("Start / resume"), _("Freeze"), _("Stop..."), _("Mark / unmark"), NULL
    };
    const char *program[] = { _("Open"), _("Close..."), NULL };
    const char *builtin[] = { _("Open"), NULL };
    const char *reb[] = { _("Reboot..."), NULL };
    const char *power[] = { _("Power off..."), NULL };
    int choice;
    if (selected < napps) {
        choice = ui_menu(col + 1, row, container);
        return choice < 0 ? -1 : container_keys[choice];
    }
    if (selected == napps + nentries) {
        return ui_menu(col + 1, row, reb) == 0 ? '\n' : -1;
    }
    if (selected == napps + nentries + 1) {
        return ui_menu(col + 1, row, power) == 0 ? '\n' : -1;
    }
    if (entries[selected - napps].builtin) {
        return ui_menu(col + 1, row, builtin) == 0 ? '\n' : -1;
    }
    choice = ui_menu(col + 1, row, program);
    return choice < 0 ? -1 : program_keys[choice];
}

/* What a pointer event does: the key it stands for, -1 to redraw only, 0
 * for nothing (a motion: the screen moved the cursor). */
static int pointer_key(void)
{
    const struct pointer_event *e = pointer_last();
    if (screen_is_extended && screen_is_extended() && screen_output_select) {
        int previous = home_output;
        if (e->output < 0 || e->output >= HOME_OUTPUTS || screen_output_select(e->output) < 0) return 0;
        output_scroll[previous] = scroll;
        home_output = e->output;
        scroll = output_scroll[home_output];
        if (!e->button && !e->wheel && !e->hwheel && !e->gesture) return previous == home_output ? 0 : -1;
    }
    if (e->wheel && confirming == CONF_NONE) {
        selected -= e->wheel * grid_columns();
        selected = selected < 0 ? 0 : selected > napps + nentries + 1 ? napps + nentries + 1 : selected;
        return -1;
    }
    if (!e->pressed || (e->button != BTN_LEFT && e->button != BTN_RIGHT)) {
        return 0;
    }
    int id = ui_hit_at(e->col, e->row);
    if (id >= HIT_KEY) {
        return e->button == BTN_LEFT ? id - HIT_KEY : 0;
    }
    if (id < HIT_ROW || id - HIT_ROW >= napps + nentries + 2 || confirming != CONF_NONE) {
        return 0;
    }
    int pos = id - HIT_ROW;
    selected = pos;
    if (e->button == BTN_RIGHT) {
        draw(0);
        return row_menu(e->col, e->row);
    }
    int first = screen_is_extended && screen_is_extended() && home_output < HOME_OUTPUTS ? output_first[home_output] : 0;
    if (pos < napps && e->col - (1 + (pos - first) % grid_columns() * grid_width()) <= 3) {
        return ' '; /* the box marks it */
    }
    return e->clicks == 2 ? '\n' : -1;
}

static void open_entry(int host)
{
    last_customized = 1;
    snprintf(last, sizeof last, _("opening %s..."), entries[host].name);
    draw(0);
    struct screen_output on;
    int several = screen_is_extended && screen_is_extended() && screen_output_count &&
                  screen_output_count() > 1 && screen_output_get(home_output, &on) == 0;
    base_open_on(several ? on.name : NULL, entries[host].argv, last, sizeof last);
    say("home: %s", last);
    if (several && strncmp(last, "shown ", 6) == 0) {
        snprintf(last, sizeof last, _("%s is open on %s."), entries[host].name, on.name);
    }
}

int main(void)
{
    load();
    screen_open(BASE_FONT);
    if (screen_extended) screen_extended(1);
    keys_open();
    refresh();
    selected = 0;
    say("home: %d containers, %d host programs", napps, nentries);
    draw(0);

    for (;;) {
        double remaining = (refresh_at - uptime()) * 1000.0;
        int timeout = remaining <= 0 ? 0 : remaining < REFRESH_MS ? (int)remaining + 1 : REFRESH_MS;
        int key = key_poll_timeout(-1, timeout);
        if (key == K_SHOWN) {
            if (confirming != CONF_NONE) {
                confirm_key(0); /* Never preserve a destructive prompt across a switch. */
            }
            if (files_request()) {
                view_files();
            }
            load();
            refresh();
            draw(1);
            continue;
        }
        int redraw = uptime() >= refresh_at;
        if (redraw) {
            refresh();
            if (confirming == CONF_NONE && files_request()) {
                view_files();
                refresh();
                draw(0);
                continue;
            }
        }
        if (key == K_POINTER) {
            key = pointer_key();
            if (key == -1) {
                draw(0);
                continue;
            }
        }
        if (key == 0) {
            if (redraw) {
                draw(0);
            }
            continue;
        }
        if (confirming != CONF_NONE) {
            confirm_key(key);
        } else if (key == K_UP || key == K_DOWN || key == K_LEFT || key == K_RIGHT ||
                   key == K_PGUP || key == K_PGDN || key == K_BOL || key == K_EOL) {
            move_selection(key);
        } else if (key == ' ' && selected < napps) {
            apps[selected].marked = !apps[selected].marked;
        } else if (key == 'r' || key == 'R') {
            act("start");
        } else if (key == 'f' || key == 'F') {
            act("freeze");
        } else if (key == 'k' || key == 'K') {
            confirm_stop();
        } else if ((key == 'x' || key == K_DEL) && !marked_count() &&
                   selected >= napps && selected < napps + nentries) {
            confirm_stop(); /* Preserve the old host-program close aliases. */
        } else if (key == '\n' && selected < napps) {
            action_error[0] = '\0';
            int accepted = request("open", apps[selected].name);
            last_customized = 1;
            snprintf(last, sizeof last,
                     accepted ? _("open %s: accepted") : _("open %s: error"), apps[selected].name);
            if (accepted) {
                continue;
            }
            refresh();
        } else if (key == '\n' && selected == napps + nentries) {
            confirming = CONF_REBOOT;
        } else if (key == '\n' && selected == napps + nentries + 1) {
            confirming = CONF_POWEROFF;
        } else if (key == '\n' && entries[selected - napps].builtin) {
            view_files();
            refresh();
        } else if (key == '\n') {
            open_entry(selected - napps);
        }
        draw(0);
    }
}
