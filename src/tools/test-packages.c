/*
 * Drive packages (the TUI of installs and updates) by its keys, with the
 * screen and keyboard as test doubles and locachy-pkg and locachy-app as
 * scripts that answer from fixed text and write down how they were called;
 * files (choosing a folder) and the user's home as doubles too; the pointer
 * as synthetic events on what is shown.
 *   test-packages DIR   (DIR: a new directory for the doubles' files)
 * Build with base/ui.c and base/rc.c.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[256];
#define D(rel) (dir_path(rel))
static char *dir_path(const char *rel);

#define JOB_LOG D("job.log")
#define JOB_PID D("job.pid")
#define PRESETS D("presets")
#define STATE D("state")
#define CONF_DIR D("conf")
#define PKG D("pkg")
#define APP D("app")
#define main packages_main
#include "../progs/packages.c"
#include "i18n-stub.h"
#undef main

static char *dir_path(const char *rel)
{
    static char paths[8][512];
    static int i;
    i = (i + 1) % 8;
    snprintf(paths[i], sizeof paths[i], "%s/%s", dir, rel);
    return paths[i];
}

/* The screen: what was last flushed, a text line per row. */
#define COLS 140
#define ROWS 40
static char grid[ROWS][COLS + 1], shown[ROWS][COLS + 1];

void say(const char *fmt, ...) { (void)fmt; }
int screen_open(const char *font) { (void)font; return 1; }
int keys_open(void) { return 1; }
int screen_cols(void) { return COLS; }
int screen_rows(void) { return ROWS; }
int screen_char_cells(uint32_t cp) { (void)cp; return 1; }
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
void screen_fill(int col, int row, int cols, int rows, uint32_t rgb)
{
    (void)rgb;
    for (int r = row; r < row + rows && r < ROWS; r++) {
        for (int c = col; c < col + cols && c < COLS; c++) {
            grid[r][c] = ' ';
        }
    }
}
void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    (void)fg;
    (void)bg;
    for (int c = col; *s && c < COLS && row < ROWS; c++, s++) {
        grid[row][c] = *s;
    }
}
void screen_flush(void)
{
    memcpy(shown, grid, sizeof shown);
}

static int on_screen(const char *text)
{
    for (int r = 0; r < ROWS; r++) {
        if (strstr(shown[r], text)) {
            return 1;
        }
    }
    return 0;
}

/* The keys: a script; WAIT lets time pass (a job ends); SNAP keeps what is
 * on the screen then (snapped looks there); CLICK(i) is clicks[i], on the
 * first place its text is shown (none: a motion); at its end, q. */
#define END 0
#define WAIT (-1)
#define SNAP (-2)
#define CLICK(i) (-100 - (i))
static const int *script;
static int step;
static char snaps[8][ROWS][COLS + 1];
static int nsnaps;

struct click {
    const char *text;
    int button, clicks, wheel;
};
static const struct click *clicks;
static struct pointer_event mouse;
const struct pointer_event *pointer_last(void) { return &mouse; }

static void point(const struct click *c)
{
    int col = 0, row = 0;
    for (row = 0; c->text && row < ROWS; row++) {
        const char *at = strstr(shown[row], c->text);
        if (at) {
            col = (int)(at - shown[row]);
            break;
        }
    }
    assert(row < ROWS);
    mouse = (struct pointer_event){ .col = col, .row = row, .button = c->button, .pressed = c->button != 0,
                                    .clicks = c->clicks, .wheel = c->wheel };
}

int key_poll_timeout(int fd, int ms)
{
    (void)fd;
    (void)ms;
    int key = script[step];
    if (key == END) {
        return 'q';
    }
    step++;
    if (key <= CLICK(0)) {
        point(&clicks[CLICK(0) - key]);
        return K_POINTER;
    }
    if (key == WAIT) {
        usleep(300000);
        return 0;
    }
    if (key == SNAP) {
        assert(nsnaps < 8);
        memcpy(snaps[nsnaps++], shown, sizeof shown);
        return 0;
    }
    return key;
}

int key_get(void)
{
    return key_poll_timeout(-1, -1);
}

static int snapped(const char *text)
{
    for (int s = 0; s < nsnaps; s++) {
        for (int r = 0; r < ROWS; r++) {
            if (strstr(snaps[s][r], text)) {
                return 1;
            }
        }
    }
    return 0;
}

/* files: the folders it answers, one each time it is asked ("": none chosen). */
static const char *picks[4];
static int npicks;

int files_pick(const char *from, const char *keep, const char *type, const char *what, char *out, size_t size)
{
    (void)from;
    (void)keep;
    (void)what;
    assert(strcmp(type, "inode/directory") == 0 && npicks < 4 && picks[npicks]);
    snprintf(out, size, "%s", picks[npicks++]);
    return out[0] != '\0';
}

void open_user_dirs(char *home, size_t hsize, char *dirs, size_t dsize)
{
    snprintf(home, hsize, "%s", D("home"));
    snprintf(dirs, dsize, "Documents Downloads Music Pictures Videos Drives");
}

static void run(const char *what, const int *keys)
{
    printf("== %s\n", what);
    script = keys;
    step = nsnaps = npicks = 0;
    page = UPDATES;
    memset(sel, 0, sizeof sel);
    show_log = 0;
    for (int i = 0; i < ROWS; i++) {
        memset(grid[i], ' ', COLS);
        grid[i][COLS] = '\0';
    }
    assert(packages_main() == 0);
    assert(script[step] == END);
}

static void put(const char *rel, const char *text)
{
    FILE *f = fopen(D(rel), "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static int has(const char *rel, const char *text)
{
    char buf[4096] = "";
    FILE *f = fopen(D(rel), "r");
    if (!f) {
        return 0;
    }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strstr(buf, text) != NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2 && mkdir(argv[1], 0755) == 0);
    char *real = realpath(argv[1], NULL);
    snprintf(dir, sizeof dir, "%s", real);
    free(real);
    mkdir(D("conf"), 0755);
    mkdir(D("presets"), 0755);
    mkdir(D("state"), 0755);
    mkdir(D("home"), 0755);
    mkdir(D("home/Projects"), 0755);
    mkdir(D("home/Downloads"), 0755);
    put("plan.txt", "checked\t2026-10-20 09:57\n"
                    "host\trepo\tmesa\t1:26.2.4-1.1\t1:26.2.5-1.1\n"
                    "host\taur\trime-wanxiang-gram-zh-hans\t2:1-1\t2:2-1\n"
                    "locachy\tnote\tsource not checked: could not fetch https://example.org/l.git (main): fatal: no\t\t\n"
                    "locachy\trebuild\tits programs, against the updated host (make.sh --boot)\t\t\n"
                    "edge\tstate\tclosed\t\t\n"
                    "edge\trepo\tlibxi\t1.8.3-1.1\t1.8.4-1.2\n"
                    "edge\trebuild\tthe host is updated\t\t\n"
                    "hello\tstate\trunning\t\t\n"
                    "hello\trebuild\tthe host is updated\t\t\n");
    put("status.txt", "leaf\tbase\t3-2\t1024\nleaf\tlabwc\t0.9.1-1\t2097152\nleaf\tsl\t\t\nmissing\tsl\n"
                      "drift\tcowsay\nclosure\t366\ninstalled\t1234\nforeign\t7\nbytes\t1288490188\n");
    put("info.txt", "microsoft-edge-stable-bin\taur\t\t155.0.4283.62-1\tThe web browser from Microsoft\t\n"
                    "sl\trepo\textra-x86-64-v2\t5.05-6.1\tSteam Locomotive runs across your terminal\t\n");
    put("commands.txt", "/usr/bin/microsoft-edge-stable\t--new-window\tMicrosoft Edge\n/usr/bin/sl\t\t\n");
    put("search.txt", "firefox\trecipe\tfirefox\tlibnotify\t/usr/bin/firefox\t\t1\tMozilla Firefox\n");
    /* The doubles: they answer, and write down their arguments in calls. */
    char script_text[4096];
    snprintf(script_text, sizeof script_text,
             "#!/bin/sh\necho \"pkg $*\" >>%s/calls\n"
             "case \"$*\" in \"plan --cached --machine\") cat %s/plan.txt ;;\n"
             "\"status --machine\") cat %s/status.txt ;; info\\ *) cat %s/info.txt ;;\n"
             "search\\ *) cat %s/search.txt ;; plan) seq 1 60; sleep 0.2 ;;\n"
             "commands\\ *) cat %s/commands.txt ;;\n"
             "apply\\ *) if [ -e %s/keys-wanted ]; then\n"
             "  echo 'Source key to trust: lyrebird-proxy 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C'\n"
             "  echo 'Trust them: locachy-build --trust-key 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C'; exit 3\n"
             "  fi; echo working; sleep 0.2 ;;\n"
             "*) echo working; sleep 0.2 ;; esac\n",
             dir, dir, dir, dir, dir, dir, dir);
    put("pkg", script_text);
    snprintf(script_text, sizeof script_text, "#!/bin/sh\necho \"app $*\" >>%s/calls\necho done\n", dir);
    put("app", script_text);
    chmod(D("pkg"), 0755);
    chmod(D("app"), 0755);
    const char *edge = "package=\"microsoft-edge-stable-bin\"\nextras=\"\"\ncommand=\"/usr/bin/microsoft-edge-stable\"\n"
                       "arguments=\"\"\nnetwork=\"1\"\ndirectories=\"\"\n";
    const char *firefox = "# Mozilla Firefox\npackage=\"firefox\"\nextras=\"libnotify\"\n"
                          "command=\"/usr/bin/firefox\"\narguments=\"\"\nnetwork=\"1\"\ndirectories=\"\"\n";
    put("presets/edge.conf", edge);
    put("presets/firefox.conf", firefox);
    put("conf/edge.conf", edge);
    mkdir(D("state/edge"), 0755);
    mkdir(D("state/edge/upper.prev"), 0755);
    put("state/edge/built", "date=\"2026-10-20 10:02:06\"\n");

    run("the plan: a row each, marked but the running one", (const int[]){ END });
    assert(on_screen("[x] host") && on_screen("2 updates, some from the AUR"));
    assert(on_screen("[x] locachy") && on_screen("rebuild: its programs"));
    assert(on_screen("source not checked: could not fetch https://example.org/l.git (main): fatal: no; rebuild"));
    assert(on_screen("[x] edge") && on_screen("1 update; rebuild: the host is updated"));
    assert(on_screen("[ ] hello") && on_screen("running"));
    assert(on_screen("checked 2026-10-20 09:57"));
    assert(on_screen("mesa") && on_screen("1:26.2.4-1.1 -> 1:26.2.5-1.1")); /* host's details */

    /* A plan whose only host work is a declared leaf that is not installed. */
    char plan_text[1024];
    FILE *saved_plan = fopen(D("plan.txt"), "r");
    assert(saved_plan);
    size_t plan_size = fread(plan_text, 1, sizeof plan_text - 1, saved_plan);
    plan_text[plan_size] = '\0';
    fclose(saved_plan);
    put("plan.txt", "checked\t2026-10-20 09:57\nhost\tmissing\tsof-firmware\t\tdeclared, not installed\n");
    run("a declared package not installed: host marked, its details", (const int[]){ END });
    assert(on_screen("[x] host") && on_screen("1 declared to install"));
    assert(on_screen("install sof-firmware") && on_screen("declared, not installed"));
    unlink(D("calls"));
    run("a declared package not installed: applied with the host",
        (const int[]){ 'a', 'y', WAIT, WAIT, WAIT, END });
    assert(has("calls", "pkg apply host\n"));
    put("plan.txt", plan_text);

    unlink(D("calls"));
    run("unmark edge, apply, yes: host and locachy only",
        (const int[]){ K_DOWN, K_DOWN, ' ', 'a', 'y', WAIT, WAIT, WAIT, END });
    assert(has("calls", "pkg apply host locachy\n"));
    assert(on_screen("done"));

    put("keys-wanted", "");
    run("apply stopped for a source signing key: which, and what a person does",
        (const int[]){ 'a', 'y', WAIT, WAIT, WAIT, END });
    assert(on_screen("Source key to trust: lyrebird-proxy 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C"));
    assert(on_screen("Trust the keys below as root, after checking them: "
                     "locachy-build --trust-key 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C; then apply again."));
    unlink(D("keys-wanted"));

    run("check: its output in the pane, how it ended, which lines; PgUp scrolls it back",
        (const int[]){ 'c', WAIT, WAIT, WAIT, SNAP, K_PGUP, END });
    assert(has("calls", "pkg plan\n"));
    assert(snapped("Output: check for updates (done)") && snapped("lines 44-60 of 60"));
    assert(on_screen("lines 29-45 of 60") && !on_screen("lines 44-60"));
    assert(sel[UPDATES] == 0); /* the list did not move */

    unlink(D("calls"));
    run("containers: edge, its form (looked up), back: the list",
        (const int[]){ '\t', '\n', SNAP, END });
    assert(snapped("Container edge") && snapped("microsoft-edge-stable-bin") && snapped("AUR 155.0.4283.62-1"));
    assert(snapped("yes (locachy-net)") && snapped("fixed once made"));
    assert(has("calls", "pkg info microsoft-edge-stable-bin\n"));
    assert(on_screen("edge") && on_screen("network") && on_screen("built 2026-10-20 10:02:06 (previous kept)"));

    run("new: from the edge preset, saved as it is: there is an edge already",
        (const int[]){ '\t', 'n', '\n', '\n', K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN,
                       K_DOWN, '\n', SNAP, END });
    assert(snapped("There is a container edge already") && has("conf/edge.conf", "network=\"1\""));

    unlink(D("calls"));
    run("new: from the edge preset, named edge2, sl as an extra, saved and built",
        (const int[]){ '\t', 'n', '\n', '\n', K_DOWN, '\n', '2', '\n', K_DOWN, K_DOWN, '\n', 's', 'l', '\n',
                       SNAP, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_RIGHT, '\n', WAIT, WAIT, END });
    assert(snapped("A new container: edge2") && snapped("extra-x86-64-v2 5.05-6.1"));
    assert(has("calls", "pkg info microsoft-edge-stable-bin sl\n") && has("calls", "app build edge2\n"));
    assert(has("conf/edge2.conf", "package=\"microsoft-edge-stable-bin\"") && has("conf/edge2.conf", "extras=\"sl\""));
    assert(has("conf/edge2.conf", "command=\"/usr/bin/microsoft-edge-stable\"") && has("conf/edge2.conf", "network=\"1\""));

    /* edge2 built: its command chosen from what its packages run. */
    mkdir(D("state/edge2"), 0755);
    put("state/edge2/built", "date=\"2026-10-20 15:00:00\"\n");
    mkdir(D("state/edge2/upper"), 0755);
    mkdir(D("state/edge2/upper/usr"), 0755);
    mkdir(D("state/edge2/upper/usr/bin"), 0755);
    put("state/edge2/upper/usr/bin/microsoft-edge-stable", "");
    unlink(D("calls"));
    char projects[512], downloads[512];
    snprintf(projects, sizeof projects, "%s", D("home/Projects"));
    snprintf(downloads, sizeof downloads, "%s", D("home/Downloads"));
    picks[0] = projects;
    picks[1] = downloads;
    run("edge2: its command chosen (with its arguments), more arguments, no network, the cameras, a directory; "
        "Downloads refused",
        (const int[]){ '\t', K_DOWN, '\n', K_DOWN, K_DOWN, K_DOWN, '\n', '\n', K_DOWN, '\n', ' ', '-', 'x', '\n',
                       K_DOWN, '\n', K_DOWN, '\n', K_DOWN, '\n', K_DOWN, '\n', SNAP, K_DOWN, '\n', END });
    assert(has("calls", "pkg commands edge2\n"));
    assert(snapped("which every container has already") && snapped("in its build"));
    assert(has("conf/edge2.conf", "command=\"/usr/bin/microsoft-edge-stable\""));
    assert(has("conf/edge2.conf", "arguments=\"--new-window -x\"") && has("conf/edge2.conf", "network=\"0\""));
    assert(has("conf/edge2.conf", "camera=\"1\""));
    snprintf(script_text, sizeof script_text, "directories=\"%s\"", projects);
    assert(has("conf/edge2.conf", script_text));

    run("edge2: its directory removed", (const int[]){ '\t', K_DOWN, '\n', K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN,
                                                       K_DOWN, K_DOWN, K_DEL, K_DOWN, '\n', END });
    assert(has("conf/edge2.conf", "directories=\"\""));

    unlink(D("calls"));
    run("edge2 removed (its home kept)", (const int[]){ '\t', K_DOWN, 'd', 'y', 'n', WAIT, WAIT, END });
    assert(has("calls", "app remove edge2\n"));

    unlink(D("calls"));
    run("host: its manifest, add foo, remove labwc",
        (const int[]){ '\t', '\t', 'i', 'f', 'o', 'o', '\n', WAIT, WAIT, K_DOWN, 'x', 'y', WAIT, WAIT, END });
    assert(has("calls", "pkg install foo\n") && has("calls", "pkg remove labwc\n"));
    assert(on_screen("labwc"));

    run("host: what differs from the manifest; versions, sizes, and what is installed in all",
        (const int[]){ '\t', '\t', END });
    assert(on_screen("drift    cowsay") && on_screen("3 declared, 366 installed"));
    assert(on_screen("Installed: 1234 packages, 1.2 GiB") && on_screen("7 foreign (AUR, LOCachy)"));
    snprintf(script_text, sizeof script_text, " Containers (%d) ", nconts);
    assert(on_screen("drift 1, missing 1") && on_screen(" Host (3) ") && nconts > 0 && on_screen(script_text));
    assert(on_screen("Declared package") && on_screen("0.9.1-1") && on_screen("2.0 MiB") && on_screen("not installed"));

    unlink(D("calls"));
    struct click applying[] = {
        { "[x] edge", BTN_LEFT, 1, 0 },
        { "[Space: mark]", BTN_LEFT, 1, 0 },
        { "[a: apply the marked]", BTN_LEFT, 1, 0 },
        { NULL, 0, 0, 0 },
        { "[Y: yes]", BTN_LEFT, 1, 0 },
    };
    clicks = applying;
    run("pointer: a row, the mark and apply buttons, yes by its button (a motion is no answer)",
        (const int[]){ CLICK(0), CLICK(1), CLICK(2), CLICK(3), CLICK(4), WAIT, WAIT, WAIT, END });
    assert(has("calls", "pkg apply host locachy\n"));

    struct click editing[] = {
        { " Containers ", BTN_LEFT, 1, 0 },
        { "edge   ", BTN_LEFT, 2, 0 },
        { "Arguments", BTN_LEFT, 2, 0 },
        { "[ Save ]", BTN_LEFT, 1, 0 },
    };
    clicks = editing;
    run("pointer: a tab, a double click opens the form, another edits a line, Save by its button",
        (const int[]){ CLICK(0), CLICK(1), CLICK(2), '-', 'z', '\n', CLICK(3), END });
    assert(has("conf/edge.conf", "arguments=\"-z\"") && has("conf/edge.conf", "network=\"1\""));

    struct click moving[] = {
        { "[x] host", 0, 0, -2 },
        { "[Tab: page]", BTN_LEFT, 1, 0 },
    };
    clicks = moving;
    run("pointer: the wheel moves the selection; the Tab button", (const int[]){ CLICK(0), CLICK(1), END });
    assert(sel[UPDATES] == 2 && page == CONTAINERS);

    /* Enter on Updates marks/unmarks, Home/End move the selection, and line
     * editing supports Left/Right/Home/Delete. */
    run("updates: Enter unmarks the selected one; End goes to the last, Home to the first",
        (const int[]){ K_BOL, '\n', SNAP, K_EOL, END });
    assert(snapped("[ ] host") && !targets[0].marked && sel[UPDATES] == 3);
    unlink(D("calls"));
    run("host: the prompt edits where the cursor is (Left, Home, Delete)",
        (const int[]){ '\t', '\t', 'i', 'a', 'c', K_LEFT, 'b', K_BOL, K_DEL, '\n', WAIT, WAIT, END });
    assert(has("calls", "pkg install bc\n"));

    unlink(D("calls"));
    run("containers: / searches recipes/repos/AUR, Enter picks firefox, Enter saves and builds it",
        (const int[]){ '\t', '/', 'f', 'i', 'r', 'e', '\n', '\n', '\n', WAIT, WAIT, END });
    assert(has("calls", "pkg search fire\n") && has("calls", "app build firefox\n"));
    assert(has("conf/firefox.conf", "package=\"firefox\"") && has("conf/firefox.conf", "extras=\"libnotify\""));

    puts("ALL PASSED");
    return 0;
}
