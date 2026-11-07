/*
 * Drive packages (the TUI of installs and updates) by its keys, with the
 * screen and keyboard as test doubles and locachy-pkg and locachy-app as
 * scripts that answer from fixed text and write down how they were called;
 * files (choosing a folder) and the user's home as doubles too; the pointer
 * as synthetic events on what is shown. The presets: the system's (DIR/presets)
 * and the app presets' catalog (DIR/catalog); rc.conf and the source's
 * commit as DIR's files; locachy-build's untrusted-keys and LOCachy's
 * recipes (DIR/pkgbuilds) too; the clipboard as a double that keeps what
 * was copied; the system jobs' queue (locachy-jobs) as a double that runs
 * its jobs at once, or queued behind another program's (test-jobs.sh tests
 * the queue).
 *   test-packages DIR   (DIR: a new directory for the doubles' files)
 * Build with base/ui.c, base/rc.c and base/textsel.c.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char dir[256];
#define D(rel) (dir_path(rel))
static char *dir_path(const char *rel);

#define SYSTEM_PRESETS D("presets")
#define APPS_CATALOG D("catalog")
#define SOURCE_COMMIT D("source-commit")
#define SOURCE_PENDING D("source-pending")
#define RC_FILE D("rc.conf")
#define STATE D("state")
#define CONF_DIR D("conf")
#define PKG D("pkg")
#define APP D("app")
#define UNTRUSTED_KEYS D("untrusted-keys")
#define RECIPES D("pkgbuilds")
#define main packages_main
#include "../progs/packages.c"
#include "i18n-stub.h"
#undef main
#include "clipboard.h"

static char *dir_path(const char *rel)
{
    static char paths[8][512];
    static int i;
    i = (i + 1) % 8;
    snprintf(paths[i], sizeof paths[i], "%s/%s", dir, rel);
    return paths[i];
}

/* The system jobs' queue (jobs.h) as a double: a job runs at once, its
 * program the double of locachy-pkg or locachy-app with the arguments the
 * service gives its kind, its output in DIR/jobs/ID.log; what was asked for,
 * "KIND ARG..." a line each, in DIR/submitted. It ends as the service says:
 * keys when it failed and DIR/untrusted-keys lists keys (locachy-build's). */
#define MAX_JOBS 64
static struct jobs_row queue[MAX_JOBS + 1];
static int queue_busy(void)
{
    struct stat st;
    return stat(D("queue-busy"), &st) == 0;
}
static pid_t queue_pid[MAX_JOBS + 1];
static int queued;

int jobs_submit(const char *kind, const char *const args[], int *id, int *position, char *why, size_t size)
{
    static const char *const commands[][3] = {
        { "pkg-install", "pkg", "install" }, { "pkg-remove", "pkg", "remove" }, { "pkg-sync", "pkg", "sync" },
        { "pkg-plan", "pkg", "plan" }, { "pkg-apply", "pkg", "apply" }, { "app-build", "app", "build" },
        { "app-rollback", "app", "rollback" }, { "app-remove", "app", "remove" },
    };
    char program[512], log[512];
    const char *argv[JOB_ARGV + 4];
    int n = 0;
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        if (strcmp(kind, commands[i][0]) == 0) {
            snprintf(program, sizeof program, "%s", D(commands[i][1]));
            argv[n++] = program;
            argv[n++] = commands[i][2];
        }
    }
    assert(n == 2 && queued < MAX_JOBS);
    struct jobs_row *r = &queue[++queued];
    memset(r, 0, sizeof *r);
    r->id = queued;
    r->status = -1;
    snprintf(r->kind, sizeof r->kind, "%s", kind);
    snprintf(r->state, sizeof r->state, "%s", queue_busy() ? "queued" : "running");
    snprintf(r->from, sizeof r->from, "packages");
    size_t len = 0;
    for (int i = 0; args[i]; i++) {
        argv[n++] = args[i];
        len += (size_t)snprintf(r->args + len, sizeof r->args - len, "%s%s", i ? " " : "", args[i]);
    }
    argv[n] = NULL;
    FILE *f = fopen(D("submitted"), "a");
    assert(f);
    fprintf(f, "%s %s\n", kind, r->args);
    fclose(f);
    jobs_log_path(r->id, log, sizeof log);
    pid_t pid = fork();
    if (pid == 0) {
        if (!freopen(log, "w", stdout) || dup2(STDOUT_FILENO, STDERR_FILENO) < 0) _exit(126);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    assert(pid > 0);
    queue_pid[r->id] = pid;
    *id = r->id;
    *position = strcmp(r->state, "queued") == 0;
    snprintf(why, size, "ok");
    return 0;
}

/* Job id, as it is now: queued while DIR/queue-busy is there (another
 * program's job runs; its program has started, its output waits). */
static struct jobs_row *queue_look(int id)
{
    struct jobs_row *r = &queue[id];
    int st;
    struct stat keys;
    if (strcmp(r->state, "queued") == 0) {
        if (queue_busy()) return r;
        snprintf(r->state, sizeof r->state, "running");
    }
    if (r->status < 0 && waitpid(queue_pid[id], &st, WNOHANG) == queue_pid[id]) {
        r->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        snprintf(r->state, sizeof r->state, "%s", r->status == 0 ? "done" :
                 stat(D("untrusted-keys"), &keys) == 0 && keys.st_size > 0 ? "keys" : "failed");
    }
    return r;
}

int jobs_status(int id, struct jobs_row *row, char *why, size_t size)
{
    (void)why;
    (void)size;
    assert(id > 0 && id <= queued);
    *row = *queue_look(id);
    return 0;
}

int jobs_mine(struct jobs_row *rows, int max)
{
    int n = 0;
    for (int id = 1; id <= queued && n < max; id++) {
        rows[n++] = *queue_look(id);
    }
    return n;
}

int jobs_ended(const struct jobs_row *row)
{
    return strcmp(row->state, "running") != 0 && strcmp(row->state, "queued") != 0;
}
void jobs_log_path(int id, char *out, size_t size) { snprintf(out, size, "%s/jobs/%d.log", dir, id); }
const char *jobs_state_label(const char *state) { return state; }

/* The screen: what was last flushed, a text line per row. */
#define COLS 140
#define ROWS 40
static char grid[ROWS][COLS + 1], shown[ROWS][COLS + 1];
static int current_cols = COLS, current_rows = ROWS;

void say(const char *fmt, ...) { (void)fmt; }
int screen_open(const char *font) { (void)font; return 1; }
int keys_open(void) { return 1; }
int screen_cols(void) { return current_cols; }
int screen_rows(void) { return current_rows; }
int screen_char_cells(uint32_t cp) { (void)cp; return 1; }
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
void screen_fill(int col, int row, int cols, int rows, uint32_t rgb)
{
    (void)rgb;
    assert(col >= 0 && row >= 0);
    for (int r = row; r < row + rows && r < current_rows; r++) {
        for (int c = col; c < col + cols && c < current_cols; c++) {
            grid[r][c] = ' ';
        }
    }
}
void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    (void)fg;
    (void)bg;
    assert(col >= 0 && row >= 0);
    for (int c = col; *s && c < current_cols && row < current_rows; c++, s++) {
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

/* The clipboard: what was copied last, to the clipboard [0] and to the
 * primary selection [1]. */
static char copied[2][4096];
int clipboard_set(int primary, const void *text, size_t size)
{
    snprintf(copied[primary ? 1 : 0], sizeof copied[0], "%.*s", (int)size, (const char *)text);
    return CLIPBOARD_OK;
}
const char *clipboard_error(int status) { return status == CLIPBOARD_OK ? "ok" : "failed"; }

/* The keys: a script; WAIT lets time pass (a job ends); SNAP keeps what is
 * on the screen then (snapped looks there); CLICK(i) is clicks[i], on the
 * first place its text is shown (none: a motion); DRAG(i) is drags[i], so
 * and offset cells into it, the left button pressed, held (a motion) or
 * released; at its end, q. */
#define END 0
#define WAIT (-1)
#define SNAP (-2)
#define RESIZE (-3)
#define NARROW (-4)
#define FREE_QUEUE (-5) /* the job before it has ended: DIR/queue-busy goes */
#define CLICK(i) (-100 - (i))
#define DRAG(i) (-200 - (i))
static const int *script;
static int step;
static char snaps[8][ROWS][COLS + 1];
static int nsnaps;

struct click {
    const char *text;
    int button, clicks, wheel;
};
enum { PRESS, HOLD, RELEASE };
struct drag {
    const char *text;
    int offset, how;
};
static const struct drag *drags;
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
    if (row == ROWS) {
        fprintf(stderr, "click target not shown: %s\n", c->text ? c->text : "(motion)");
        for (int r = 0; r < current_rows; r++) fprintf(stderr, "%s\n", shown[r]);
    }
    assert(row < ROWS);
    mouse = (struct pointer_event){ .col = col, .row = row, .button = c->button, .pressed = c->button != 0,
                                    .clicks = c->clicks, .wheel = c->wheel };
}

static void drag(const struct drag *d)
{
    struct click at = { d->text, BTN_LEFT, 1, 0 };
    point(&at);
    mouse.col += d->offset;
    mouse.button = d->how == HOLD ? 0 : BTN_LEFT;
    mouse.pressed = d->how == PRESS;
    mouse.clicks = d->how == PRESS;
    mouse.buttons = d->how != RELEASE;
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
    if (key <= DRAG(0)) {
        drag(&drags[DRAG(0) - key]);
        return K_POINTER;
    }
    if (key <= CLICK(0)) {
        point(&clicks[CLICK(0) - key]);
        return K_POINTER;
    }
    if (key == RESIZE || key == NARROW) {
        current_cols = key == NARROW ? 71 : 110;
        current_rows = key == NARROW ? 27 : 28;
        for (int r = 0; r < ROWS; r++) {
            memset(grid[r], ' ', COLS);
            grid[r][COLS] = '\0';
        }
        memcpy(shown, grid, sizeof shown);
        return K_SHOWN;
    }
    if (key == WAIT) {
        usleep(300000);
        return 0;
    }
    if (key == FREE_QUEUE) {
        unlink(D("queue-busy"));
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

static int snap_has(int s, const char *text)
{
    assert(s < nsnaps);
    for (int r = 0; r < ROWS; r++) {
        if (strstr(snaps[s][r], text)) return 1;
    }
    return 0;
}

static int snapped(const char *text)
{
    for (int s = 0; s < nsnaps; s++) {
        if (snap_has(s, text)) return 1;
    }
    return 0;
}

/* The row of snap s that shows text, or -1. */
static int snap_row(int s, const char *text)
{
    assert(s < nsnaps);
    for (int r = 0; r < ROWS; r++) {
        if (strstr(snaps[s][r], text)) return r;
    }
    return -1;
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
    current_cols = COLS;
    current_rows = ROWS;
    page = UPDATES;
    memset(sel, 0, sizeof sel);
    show_log = details_from = 0;
    details_name[0] = '\0';
    /* As a new run of the program: what it knows of the last job, from its files. */
    nwanted = 0;
    job = 0;
    job_kind[0] = job_log[0] = '\0';
    job_argv[0] = NULL;
    job_result = -1;
    textsel_clear();
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

/* A package its build installed: an entry of its upper's pacman database,
 * as pacman writes it. */
static void installed_in(const char *container, const char *name, const char *version, long long bytes)
{
    char rel[256], text[512];
    snprintf(rel, sizeof rel, "state/%s/upper/var/lib/pacman/local/%s-%s", container, name, version);
    assert(mkdir(D(rel), 0755) == 0);
    snprintf(text, sizeof text, "%%NAME%%\n%s\n\n%%VERSION%%\n%s\n\n%%BASE%%\n%s\n\n%%DESC%%\nA package\n\n"
             "%%SIZE%%\n%lld\n\n%%REASON%%\n1\n\n", name, version, name, bytes);
    strncat(rel, "/desc", sizeof rel - strlen(rel) - 1);
    put(rel, text);
}

/* A package's line as the lists show it: name, version, size, in columns. */
static const char *columns(const char *name, const char *version, const char *size)
{
    static char line[160];
    snprintf(line, sizeof line, "%-32s %-28s %10s", name, version, size);
    return line;
}

/* What the doubles were called with, in all. */
static const char *slurp_calls(void)
{
    static char buf[8192];
    FILE *f = fopen(D("calls"), "r");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[n] = '\0';
    if (f) fclose(f);
    return buf;
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
    mkdir(D("catalog"), 0755);
    mkdir(D("state"), 0755);
    mkdir(D("home"), 0755);
    mkdir(D("home/Projects"), 0755);
    mkdir(D("home/Downloads"), 0755);
    mkdir(D("jobs"), 0755);
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
    /* The doubles: they answer, and write down their arguments in calls.
     * With keys-wanted, apply and build stop as locachy-build does for
     * source signing keys not trusted yet (its lines, its untrusted-keys). */
    const char *lyre = "40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C", *obfs = "07948FFA64160A425BCD27EAC732B1D1C28F4E2F";
    char script_text[4096];
    snprintf(script_text, sizeof script_text,
             "#!/bin/sh\necho \"pkg $*\" >>%s/calls\n"
             "[ \"$1\" != install ] || echo \"pkg install: $# arguments\" >>%s/calls\n"
             "case \"$*\" in \"plan --cached --machine\") cat %s/plan.txt ;;\n"
             "\"status --machine\") cat %s/status.txt ;; info\\ *) cat %s/info.txt ;;\n"
             "search\\ *) echo \"catalog $LOCACHY_APPS_CATALOG\" >>%s/calls; cat %s/search.txt ;;\n"
             "plan) seq 1 60; sleep 0.2 ;;\n"
             "commands\\ *) cat %s/commands.txt ;;\n"
             "apply\\ *) if [ -e %s/keys-wanted ]; then\n"
             "  echo '===> host'; echo '===> host: done'; echo '===> edge'\n"
             "  echo '===> /usr/local/bin/locachy-build lyrebird-proxy obfs4proxy'\n"
             "  echo 'locachy-build: source signing keys not trusted yet; none is added by itself. Nothing was built:'\n"
             "  echo 'Source key to trust: lyrebird-proxy %s'\n"
             "  echo 'Source key to trust: obfs4proxy %s'\n"
             "  echo 'locachy-build: check each with its upstream project, then (as root):'\n"
             "  echo 'Trust them: locachy-build --trust-key %s %s'\n"
             "  printf 'lyrebird-proxy %s\\nobfs4proxy %s\\n' >%s/untrusted-keys\n"
             "  echo '===> edge: not done'; echo '===> not done: edge'; exit 1\n"
             "  fi; echo working; sleep 0.2 ;;\n"
             "*) echo working; sleep 0.2 ;; esac\n",
             dir, dir, dir, dir, dir, dir, dir, dir, dir, lyre, obfs, obfs, lyre, lyre, obfs, dir);
    put("pkg", script_text);
    snprintf(script_text, sizeof script_text,
             "#!/bin/sh\necho \"app $*\" >>%s/calls\n"
             "if [ \"$1\" = build ] && [ -e %s/keys-wanted ]; then\n"
             "  echo 'Source key to trust: obfs4proxy %s'; echo 'Trust them: locachy-build --trust-key %s'\n"
             "  echo 'obfs4proxy %s' >%s/untrusted-keys; exit 3\nfi\necho done\n",
             dir, dir, obfs, obfs, obfs, dir);
    put("app", script_text);
    chmod(D("pkg"), 0755);
    chmod(D("app"), 0755);
    const char *edge = "package=\"microsoft-edge-stable-bin\"\nextras=\"\"\ncommand=\"/usr/bin/microsoft-edge-stable\"\n"
                       "arguments=\"\"\nnetwork=\"1\"\ndirectories=\"\"\n";
    const char *firefox = "# Mozilla Firefox\npackage=\"firefox\"\nextras=\"libnotify\"\n"
                          "command=\"/usr/bin/firefox\"\narguments=\"\"\nnetwork=\"1\"\ndirectories=\"\"\n";
    put("catalog/edge.conf", edge); /* app presets; the system's (host programs) come later */
    put("catalog/firefox.conf", firefox);
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
    /* Where updates come from, over the list: rc.conf says nothing (the
     * defaults), no commit recorded, no catalog; the app presets a row of
     * their own, nothing in the plan for them. */
    assert(on_screen("LOCachy source: its commit is not recorded; updates: https://github.com/niuyuanzhao/locachy.git (main)"));
    assert(on_screen("App presets: none installed; updates: https://github.com/niuyuanzhao/locachy-apps.git (main)"));
    assert(on_screen("[ ] apps") && on_screen("up to date"));

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
        (const int[]){ K_DOWN, K_DOWN, K_DOWN, ' ', 'a', 'y', WAIT, WAIT, WAIT, END });
    assert(has("calls", "pkg apply host locachy\n"));
    assert(on_screen("done"));

    /* An apply stops for source signing keys: not a failure, a person's
     * decision. The keys are listed (package, fingerprint as gpg groups it,
     * the recipe whose validpgpkeys name it: LOCachy's own, else the AUR's);
     * y copies the command that trusts them; the pane's text is selected
     * with the pointer (the primary selection), y copies it; Y all of it. */
    char command[256], expect[1024], cell[1024];
    snprintf(command, sizeof command, "locachy-build --trust-key %s %s", obfs, lyre);
    mkdir(D("pkgbuilds"), 0755);
    mkdir(D("pkgbuilds/obfs4proxy"), 0755);
    put("keys-wanted", "");
    unlink(D("calls"));
    struct click line_click[] = { { "Source key to trust: lyrebird-proxy", BTN_LEFT, 3, 0 } }; /* triple */
    clicks = line_click;
    run("apply stopped for source signing keys: listed, the command copied, a line selected",
        (const int[]){ 'a', 'y', WAIT, WAIT, WAIT, SNAP, 'y', SNAP, CLICK(0), END });
    snprintf(expect, sizeof expect, "Source key to trust: lyrebird-proxy %s", lyre);
    assert(strcmp(copied[1], expect) == 0); /* the triple click: its line, as the primary selection */
    assert(strcmp(copied[0], command) == 0); /* y, nothing selected: the command, as locachy-build printed it */
    assert(snap_has(0, "Waiting for you: source signing keys to trust. None is trusted by itself."));
    snprintf(expect, sizeof expect, "%-24s %s", "lyrebird-proxy", "40BB CBED 223F 5EB2 A03E  F657 D7D7 A110 ABC7 9A6C");
    assert(snap_has(0, expect));
    snprintf(expect, sizeof expect, "%-24s %s", "obfs4proxy", "0794 8FFA 6416 0A42 5BCD  27EA C732 B1D1 C28F 4E2F");
    assert(snap_has(0, expect));
    assert(snap_has(0, "validpgpkeys of the AUR's recipe: "
                       "https://aur.archlinux.org/cgit/aur.git/tree/PKGBUILD?h=lyrebird-proxy"));
    assert(snap_has(0, "validpgpkeys of LOCachy's recipe: /"));
    key_source("obfs4proxy", expect, sizeof expect);
    snprintf(cell, sizeof cell, "validpgpkeys of LOCachy's recipe: %s/pkgbuilds/obfs4proxy", dir);
    assert(strcmp(expect, cell) == 0);
    assert(snap_has(0, "Check each with its upstream project; then run this as root (admin, from home):"));
    assert(snap_has(0, command) && snap_has(0, "once they are trusted, g continues where it stopped."));
    assert(snap_has(0, "Output: Apply: host locachy edge (stopped)") && snap_has(0, "Y: copy all"));
    assert(snap_has(0, "Apply: host locachy edge: stopped for source signing keys to trust (below)"));
    assert(snap_has(0, "[g: continue]") && snap_has(0, "[y: copy]"));
    snprintf(expect, sizeof expect, "Copied: %s", command);
    assert(snap_has(1, expect));
    assert(has("calls", "pkg apply host locachy edge\n") && nwanted == 2);

    /* Another run of the program finds it stopped (its files). A drag over
     * two lines of the output: the primary selection; y copies it; a row
     * clicked, the selection goes; Y: all of the output. */
    struct drag selecting[] = {
        { "Source key to trust: lyrebird-proxy", 0, PRESS },
        { "Source key to trust: obfs4proxy", 10, HOLD }, /* over the next line */
        { "Source key to trust: obfs4proxy", 10, RELEASE },
    };
    struct click row_click[] = { { "[x] host", BTN_LEFT, 1, 0 } };
    drags = selecting;
    clicks = row_click;
    run("stopped, found again: a drag selects, y copies it; Y copies all the output",
        (const int[]){ DRAG(0), DRAG(1), DRAG(2), 'y', SNAP, CLICK(0), 'Y', SNAP, END });
    snprintf(expect, sizeof expect, "Source key to trust: lyrebird-proxy %s\n  Source key", lyre);
    assert(strcmp(copied[1], expect) == 0); /* the drag: the primary selection; y: the same (below) */
    assert(snap_has(0, "Copied 2 lines: Source key to trust: lyrebird-proxy"));
    /* Y: the keys as the pane shows them, their origins whole, then the output. */
    assert(strncmp(copied[0], "Waiting for you: source signing keys to trust. None is trusted by itself.\n", 74) == 0);
    assert(strstr(copied[0], "validpgpkeys of the AUR's recipe: "
                             "https://aur.archlinux.org/cgit/aur.git/tree/PKGBUILD?h=lyrebird-proxy\n"));
    snprintf(cell, sizeof cell, "validpgpkeys of LOCachy's recipe: %s/pkgbuilds/obfs4proxy\n", dir);
    assert(strstr(copied[0], cell));
    snprintf(cell, sizeof cell, "\n  %s\ny copies the command; once they are trusted, g continues where it stopped.\n"
             "===> host\n===> host: done\n===> edge\n", command);
    assert(strstr(copied[0], cell));
    snprintf(expect, sizeof expect, "\nTrust them: %s\n===> edge: not done\n===> not done: edge", command);
    assert(strstr(copied[0], expect) && strcmp(copied[0] + strlen(copied[0]) - strlen(expect), expect) == 0);
    assert(snap_has(1, "Copied 19 lines: Waiting for you: source signing keys to trust.") && nwanted == 2);

    /* Another run of the program finds it stopped (its files); narrow, the
     * command wraps, each line but the last ending in " \": one command to a
     * shell, selected and pasted together. */
    run("stopped, found again by a new run; narrow: the command wraps", (const int[]){ NARROW, SNAP, END });
    int at = snap_row(0, "locachy-build --trust-key \\");
    snprintf(expect, sizeof expect, "%s \\", obfs);
    assert(at > 0 && strstr(snaps[0][at + 1], expect) && strstr(snaps[0][at + 2], lyre));
    assert(!strstr(snaps[0][at + 2], "\\") && snap_has(0, "Waiting for you: source signing keys to trust."));
    /* The origin wider than the pane: on the rows below, under itself, whole. */
    int from = snap_row(0, "validpgpkeys of the AUR's recipe: ");
    char joined[512] = "";
    for (int k = 0; from > 0 && k < 3; k++) {
        size_t len = strlen(joined);
        snprintf(joined + len, sizeof joined - len, "%s", snaps[0][from + k] + 29);
        for (len = strlen(joined); len > 0 && joined[len - 1] == ' '; len--) joined[len - 1] = '\0';
    }
    assert(strcmp(joined, "validpgpkeys of the AUR's recipe: "
                          "https://aur.archlinux.org/cgit/aur.git/tree/PKGBUILD?h=lyrebird-proxy") == 0);
    assert(at > from + 2);

    /* A person trusts one in a terminal (--trust-key takes its line out of
     * untrusted-keys): it shows as trusted, the command has the other only. */
    snprintf(expect, sizeof expect, "obfs4proxy %s\n", obfs);
    put("untrusted-keys", expect);
    run("one trusted in a terminal: marked so, the command for the rest", (const int[]){ SNAP, 'y', END });
    snprintf(expect, sizeof expect, "%-24s %s   trusted now", "lyrebird-proxy",
             "40BB CBED 223F 5EB2 A03E  F657 D7D7 A110 ABC7 9A6C");
    assert(snap_has(0, expect));
    snprintf(expect, sizeof expect, "locachy-build --trust-key %s", obfs);
    assert(strcmp(copied[0], expect) == 0);

    /* All trusted: g goes on from where it stopped, with the plan it
     * stopped with: the target done (host) is not applied again; the job's
     * output goes on after what it printed. */
    put("untrusted-keys", "");
    unlink(D("keys-wanted"));
    unlink(D("calls"));
    run("all trusted: g continues the apply, the targets not done",
        (const int[]){ SNAP, 'g', SNAP, WAIT, WAIT, WAIT, SNAP, END });
    assert(snap_has(0, "All of them are trusted now: g continues where it stopped."));
    assert(snap_has(1, "Apply: host locachy edge: continuing; it goes on if you leave."));
    assert(has("calls", "pkg apply locachy edge\n") && !has("calls", "pkg apply host"));
    /* The continuation is a new job, queued: its own output after the one
     * that stopped; what is asked for, the targets not done. */
    char stopped_log[64], continued_log[64];
    snprintf(stopped_log, sizeof stopped_log, "jobs/%d.log", queued - 1);
    snprintf(continued_log, sizeof continued_log, "jobs/%d.log", queued);
    assert(has(stopped_log, "Trust them: ") && has(continued_log, "working\n") && !has(continued_log, "Trust them"));
    assert(has("submitted", "pkg-apply host locachy edge\npkg-apply locachy edge\n"));
    assert(queue[queued - 1].state[0] == 'k'); /* the one that stopped: the queue says keys, not failed */
    assert(snap_has(2, "Output: Apply: host locachy edge (done)") && !snap_has(2, "[g: continue]") && nwanted == 0);

    /* A container's build stops so too; g builds it again (nothing was
     * built before its keys were checked). */
    put("keys-wanted", "");
    unlink(D("calls"));
    run("a container's build stopped for a key: g builds it again once trusted",
        (const int[]){ '\t', 'b', WAIT, WAIT, SNAP, END });
    assert(snapped("build edge: stopped for source signing keys to trust") && snapped("[g: continue]"));
    put("untrusted-keys", "");
    unlink(D("keys-wanted"));
    run("a container's build, continued", (const int[]){ '\t', 'g', WAIT, WAIT, SNAP, END });
    const char *built = strstr(slurp_calls(), "app build edge\n");
    assert(built && strstr(built + 1, "app build edge\n") && snapped("build edge (done)"));
    unlink(D("untrusted-keys"));

    run("check: its output in the pane, how it ended, which lines; PgUp scrolls it back",
        (const int[]){ 'c', WAIT, WAIT, WAIT, SNAP, K_PGUP, END });
    assert(has("calls", "pkg plan\n"));
    assert(snapped("Output: check for updates (done)") && snapped("lines 44-60 of 60"));
    assert(on_screen("lines 29-45 of 60") && !on_screen("lines 44-60"));
    assert(sel[UPDATES] == 0); /* the list did not move */

    /* Asked for while another program's job runs: queued, and the pane's
     * title says so (not "running") until the queue starts it. */
    put("queue-busy", "");
    run("check while the queue is busy: queued, then running, then done",
        (const int[]){ 'c', SNAP, FREE_QUEUE, WAIT, WAIT, WAIT, SNAP, END });
    assert(snap_has(0, "Output: check for updates (queued)") && !snap_has(0, "(running)"));
    assert(snap_has(0, "check for updates: queued (1 before it); it goes on if you leave."));
    assert(snap_has(1, "Output: check for updates (done)"));

    unlink(D("calls"));
    run("containers: edge, its form (looked up), back: the list",
        (const int[]){ '\t', '\n', SNAP, END });
    assert(snapped("Container edge") && snapped("microsoft-edge-stable-bin") && snapped("AUR 155.0.4283.62-1"));
    assert(snapped("yes (locachy-net)") && snapped("fixed once made"));
    assert(has("calls", "pkg info microsoft-edge-stable-bin\n"));
    assert(on_screen("edge") && on_screen("network") && on_screen("built 2026-10-20 10:02:06 (previous kept)"));

    /* What edge's build installed over the host's: its upper's pacman
     * database, with its version file and the whiteout of a host package
     * the build replaced (overlayfs makes a character device; a plain file
     * here: neither has a desc, so neither is a package). plain is not
     * built. */
    const char *db[] = { "state/edge/upper", "state/edge/upper/var", "state/edge/upper/var/lib",
                         "state/edge/upper/var/lib/pacman", "state/edge/upper/var/lib/pacman/local" };
    for (size_t i = 0; i < sizeof db / sizeof db[0]; i++) {
        mkdir(D(db[i]), 0755);
    }
    put("state/edge/upper/var/lib/pacman/local/ALPM_DB_VERSION", "9\n");
    put("state/edge/upper/var/lib/pacman/local/glibc-2.40+r16-1", "");
    installed_in("edge", "microsoft-edge-stable-bin", "155.0.4283.62-1", 629145600);
    installed_in("edge", "zlib-ng", "2.2.2-1", 204800);
    installed_in("edge", "gtk3", "1:3.24.43-4", 50331648);
    installed_in("edge", "libxss", "1.2.4-1", 37500);
    installed_in("edge", "nss", "3.105-1", 7340032);
    installed_in("edge", "libcups", "2:2.4.11-1", 1048576);
    installed_in("edge", "xdg-utils", "1.2.1-1", 300000);
    installed_in("edge", "at-spi2-core", "2.54.0-2", 4194304);
    put("conf/plain.conf", "package=\"sl\"\nextras=\"\"\ncommand=\"/usr/bin/sl\"\n");
    run("containers: what each build installed, in all; edge's packages by name, their versions and sizes",
        (const int[]){ '\t', END });
    assert(on_screen("8 packages, 660.5 MiB  built 2026-10-20 10:02:06 (previous kept)"));
    assert(on_screen("Its build installed 8 packages, 660.5 MiB; the rest is the host's"));
    assert(on_screen(columns("Package", "Version", "Size")));
    assert(on_screen(columns("at-spi2-core", "2.54.0-2", "4.0 MiB")) && on_screen(columns("libxss", "1.2.4-1", "36.6 KiB")));
    assert(on_screen(columns("microsoft-edge-stable-bin", "155.0.4283.62-1", "600.0 MiB")));
    assert(on_screen(columns("gtk3", "1:3.24.43-4", "48.0 MiB")) && on_screen(columns("libcups", "2:2.4.11-1", "1.0 MiB")));
    assert(on_screen("Details: edge   l: output") && !on_screen("Details: edge ("));
    assert(on_screen("lines 1-17 of 20   PgUp/PgDn: scroll") && !on_screen("zlib-ng") && !on_screen("glibc"));
    struct click wheel[] = { { "Its build installed", 0, 0, 1 } };
    clicks = wheel;
    run("containers: PgDn scrolls the details to the last package, the wheel back; plain: not built, all fits, "
        "PgUp moves the selection",
        (const int[]){ '\t', K_PGDN, SNAP, CLICK(0), SNAP, K_DOWN, SNAP, K_PGUP, END });
    assert(snapped("lines 4-20 of 20") && snapped(columns("zlib-ng", "2.2.2-1", "200.0 KiB")));
    assert(snapped(columns("nss", "3.105-1", "7.0 MiB")) && snapped(columns("xdg-utils", "1.2.1-1", "293.0 KiB")));
    assert(snapped("lines 1-17 of 20"));
    assert(snapped("Details: plain") && snapped("Not built yet: no build has installed anything"));
    snprintf(script_text, sizeof script_text, "%-12s %-8s %8s %7s %22s built never", "plain", "closed", "", "", "");
    assert(snapped(script_text) && sel[CONTAINERS] == 0 && on_screen("Details: edge"));
    unlink(D("conf/plain.conf"));

    run("new: from the edge preset, saved as it is: there is an edge already",
        (const int[]){ '\t', 'n', '\n', '\n', K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN,
                       K_DOWN, K_DOWN, '\n', SNAP, END });
    assert(snapped("There is a container edge already") && has("conf/edge.conf", "network=\"1\""));

    unlink(D("calls"));
    run("new: from the edge preset, named edge2, sl as an extra, saved and built",
        (const int[]){ '\t', 'n', '\n', '\n', K_DOWN, '\n', '2', '\n', K_DOWN, K_DOWN, '\n', 's', 'l', '\n',
                       SNAP, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_RIGHT, '\n', WAIT, WAIT, END });
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
                       K_DOWN, '\n', K_DOWN, '\n', K_DOWN, '\n', K_DOWN, '\n', SNAP, K_DOWN, K_DOWN, '\n', END });
    assert(has("calls", "pkg commands edge2\n"));
    assert(snapped("which every container has already") && snapped("in its build"));
    assert(has("conf/edge2.conf", "command=\"/usr/bin/microsoft-edge-stable\""));
    assert(has("conf/edge2.conf", "arguments=\"--new-window -x\"") && has("conf/edge2.conf", "network=\"0\""));
    assert(has("conf/edge2.conf", "camera=\"1\""));
    snprintf(script_text, sizeof script_text, "directories=\"%s\"", projects);
    assert(has("conf/edge2.conf", script_text));

    run("edge2: its directory removed", (const int[]){ '\t', K_DOWN, '\n', K_DOWN, K_DOWN, K_DOWN, K_DOWN, K_DOWN,
                                                       K_DOWN, K_DOWN, K_DEL, K_DOWN, K_DOWN, '\n', END });
    assert(has("conf/edge2.conf", "directories=\"\""));
    /* Built, with nothing in its upper's pacman database. */
    assert(on_screen("Details: edge2") && on_screen("Its build installed nothing: all its packages are the host's"));
    assert(on_screen("0 packages, 0 B"));

    unlink(D("calls"));
    run("edge2 removed (its home kept)", (const int[]){ '\t', K_DOWN, 'd', 'y', 'n', WAIT, WAIT, END });
    assert(has("calls", "app remove edge2\n"));

    unlink(D("calls"));
    run("host: its manifest, add foo, remove labwc",
        (const int[]){ '\t', '\t', 'i', 'f', 'o', 'o', '\n', WAIT, WAIT, K_DOWN, 'x', 'y', WAIT, WAIT, END });
    assert(has("calls", "pkg install foo\n") && has("calls", "pkg remove labwc\n"));
    assert(on_screen("labwc"));

    /* Several names at once: each its own argument, as locachy-pkg install takes them. */
    unlink(D("calls"));
    run("host: add two packages at once",
        (const int[]){ '\t', '\t', 'i', 'l', 'i', 'b', 'r', 'a', 'w', ' ', ' ', 'c', 'h', 'm', 'l', 'i', 'b', ' ', '\n',
                       WAIT, WAIT, END });
    assert(has("calls", "pkg install libraw chmlib\n") && has("calls", "pkg install: 3 arguments\n"));
    unlink(D("calls"));
    run("host: a name pacman does not take is refused",
        (const int[]){ '\t', '\t', 'i', 'a', ';', 'b', '\n', WAIT, END });
    assert(!has("calls", "pkg install") && on_screen("Package names: letters, digits and @ . _ + -, at most 32."));

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
    assert(snapped("[ ] host") && !targets[0].marked && sel[UPDATES] == 4);
    unlink(D("calls"));
    run("host: the prompt edits where the cursor is (Left, Home, Delete)",
        (const int[]){ '\t', '\t', 'i', 'a', 'c', K_LEFT, 'b', K_BOL, K_DEL, '\n', WAIT, WAIT, END });
    assert(has("calls", "pkg install bc\n"));

    unlink(D("calls"));
    run("containers: / searches recipes/repos/AUR, Enter picks firefox, Enter saves and builds it",
        (const int[]){ '\t', '/', 'f', 'i', 'r', 'e', '\n', '\n', '\n', WAIT, WAIT, END });
    assert(has("calls", "pkg search fire\n") && has("calls", "app build firefox\n"));
    snprintf(script_text, sizeof script_text, "catalog %s\n", D("catalog")); /* search reads the app presets' catalog */
    assert(has("calls", script_text));
    assert(has("conf/firefox.conf", "# Mozilla Firefox\n") && has("conf/firefox.conf", "package=\"firefox\""));
    assert(has("conf/firefox.conf", "extras=\"libnotify\""));

    run("resize: the main page lays out the new rows and hit map",
        (const int[]){ RESIZE, SNAP, END });
    assert(strstr(shown[26], "[c: check]") && ui_hit_at(2, 26) == HIT_KEY + 'c');
    assert(on_screen("Installed: 1234 packages"));

    unlink(D("calls"));
    run("resize: a package prompt keeps its text and redraws the page",
        (const int[]){ '\t', '\t', 'i', 'f', RESIZE, 'o', 'o', SNAP, '\n', WAIT, WAIT, END });
    assert(has("calls", "pkg install foo\n") && snapped("Declared package"));

    unlink(D("calls"));
    clicks = applying;
    run("resize: confirmation waits for an answer and stays clickable",
        (const int[]){ 'a', RESIZE, SNAP, CLICK(4), WAIT, WAIT, END });
    assert(has("calls", "pkg apply host locachy edge\n") && snapped("Y: yes"));

    clicks = editing;
    run("resize: a container field redraws and keeps its draft",
        (const int[]){ CLICK(0), CLICK(1), CLICK(2), K_EOL, 'x', RESIZE, 'y', SNAP, '\n', CLICK(3), END });
    assert(has("conf/edge.conf", "arguments=\"-zxy\"") && snapped("Container edge"));

    char refusal[400], shared[512];
    snprintf(shared, sizeof shared, "%s/Downloads", D("home"));
    assert(directory_refused(shared, refusal, sizeof refusal));
    assert(strstr(refusal, "Downloads is in Downloads, which every container has already."));
    assert(!strstr(refusal, dir)); /* long home paths must not push the reason off screen */

    /* The app presets' part of the plan, a row of its own: its notes first,
     * then what it would do; its details: the commit, each configuration
     * that follows its new preset; a container rebuilt for it. Over the
     * list, a line each: the source installed and where it comes from, the
     * catalog's origin and where it comes from; the lower pane where it is
     * on the other pages. */
    put("source-commit", "89abcdef0123456789abcdef0123456789abcdef\n");
    put("catalog/CATALOG", "format=\"1\"\norigin=\"bootstrap\"\nrepo=\"\"\nbranch=\"\"\ncommit=\"\"\n"
                           "installed=\"2026-10-30 10:00\"\nsystem_commit=\"89abcdef0123456789abcdef0123456789abcdef\"\n");
    put("rc.conf", "update_branch=\"next\"\napps_repo=\"https://example.org/me/apps.git\"\napps_branch=\"stable\"\n");
    put("plan.txt", "checked\t2026-10-30 11:00\n"
                    "apps\trepo\tlocachy-apps\tbootstrap\t0123456 (Add zathura)\n"
                    "apps\tcatalog\tapp presets from branch stable\tbootstrap\t0123456789abcdef0123456789abcdef01234567\n"
                    "apps\tmerge\tedge\tpackage extras\t\n"
                    "apps\tnote\tbootstrap catalog from the system source, not from the apps repository\t\t\n"
                    "edge\tstate\tclosed\t\t\n"
                    "edge\trebuild\tits preset names other packages\tmicrosoft-edge-stable-bin\t"
                    "microsoft-edge-stable-bin libva\n");
    unlink(D("calls"));
    run("app presets: their row and details, the two sources' lines, applied with the rest",
        (const int[]){ SNAP, K_DOWN, K_DOWN, SNAP, '\t', '\t', SNAP, '\t', 'a', 'y', WAIT, WAIT, WAIT, END });
    assert(snapped("LOCachy source: 89abcde installed; updates: https://github.com/niuyuanzhao/locachy.git (next)"));
    assert(snapped("App presets: from the system, not yet from the apps repository; "
                   "updates: https://example.org/me/apps.git (stable)"));
    assert(snapped("[x] apps") && snapped("bootstrap catalog from the system source, not from the apps repository; "
                                          "1 update"));
    int dim_row;
    char apps_row[512];
    row_text(2, apps_row, sizeof apps_row, &dim_row); /* the rest of it, past the screen's edge */
    assert(!strcmp(targets[2].name, "apps") && strstr(apps_row, "; 1 update; 1 configuration follows its new preset") &&
           !dim_row);
    assert(snapped("[x] edge") && snapped("rebuild: its preset names other packages"));
    assert(strstr(snaps[0][3], "LOCachy source: ") && strstr(snaps[0][4], "App presets: ") &&
           strstr(snaps[0][5], "[ ] host")); /* the list under them */
    assert(snapped("Details: apps   l: output") && snapped("locachy-apps") && snapped("bootstrap -> 0123456 (Add zathura)"));
    assert(snapped("app presets from branch stable: bootstrap -> 0123456") && !snapped("01234567"));
    assert(snapped("edge follows its new preset: package extras"));
    /* The pane: under half of what is left, on Updates as on Host. */
    assert(strstr(snaps[0][20], "Details: host") && strstr(snaps[1][20], "Details: apps") &&
           strstr(snaps[2][20], " declared, ") && strstr(snaps[2][3], "Declared package"));
    assert(has("calls", "pkg apply apps edge\n"));

    /* Updates off for both: no row for the app presets; the catalog from
     * their repository, its commit. */
    put("plan.txt", plan_text);
    put("rc.conf", "update_repo=\"off\"\napps_repo=\"off\"\n");
    put("catalog/CATALOG", "format=\"1\"\norigin=\"repository\"\nrepo=\"https://example.org/me/apps.git\"\n"
                           "branch=\"main\"\ncommit=\"fedcba9876543210fedcba9876543210fedcba98\"\n"
                           "installed=\"2026-10-30 10:00\"\n");
    run("updates off: both lines say so, no app presets' row", (const int[]){ END });
    assert(on_screen("LOCachy source: 89abcde installed; updates: off"));
    assert(on_screen("App presets: fedcba9 from the apps repository; updates: off") && !on_screen("fedcba98"));
    assert(!on_screen("] apps") && on_screen("[x] host") && on_screen("[x] edge"));

    /* A long repository: what is installed stays first, the buttons below. */
    snprintf(script_text, sizeof script_text, "apps_repo=\"https://example.org/%0200d.git\"\n", 0);
    put("rc.conf", script_text);
    put("catalog/CATALOG", "format=\"1\"\norigin=\"medium\"\nrepo=\"https://example.org/me/apps.git\"\nbranch=\"main\"\n"
                           "commit=\"fedcba9876543210fedcba9876543210fedcba98\"\ninstalled=\"2026-10-30 10:00\"\n");
    run("a long app presets repository: installed first, nothing pushed off", (const int[]){ END });
    assert(on_screen("App presets: fedcba9 from the installation medium; updates: https://example.org/0000"));
    assert(on_screen("[ ] apps") && on_screen("[c: check]") && on_screen("[q: back]") && !on_screen("(null)"));
    /* A source update that stopped (its builds waiting for keys): the one
     * applied stays the installed one, the other is said to be unfinished. */
    put("rc.conf", "update_repo=\"off\"\napps_repo=\"off\"\n");
    put("source-pending", "0123456789abcdef0123456789abcdef01234567\n");
    run("a source in place whose update has not finished", (const int[]){ END });
    assert(on_screen("LOCachy source: 89abcde installed, 0123456 not finished; updates: off"));
    unlink(D("source-commit"));
    run("a pending source with no commit recorded before it", (const int[]){ END });
    assert(on_screen("LOCachy source: its commit is not recorded, 0123456 not finished; updates: off"));
    unlink(D("source-pending"));
    unlink(D("rc.conf"));
    unlink(D("catalog/CATALOG"));
    unlink(D("source-commit"));

    /* System and downloadable host-access recipes share the store flow.
     * Selecting one is not yet permission to save it or install packages. */
    put("presets/foot.conf", "# foot: a host program\npackage=\"foot\"\ncommand=\"/usr/bin/foot\"\nhost=\"1\"\n");
    put("catalog/foot.conf", "package=\"foot-app\"\ncommand=\"/usr/bin/foot\"\n");
    snprintf(script_text, sizeof script_text,
             "# ChatGPT recipe\npackage=\"chatgpt-desktop\"\ncommand=\"/usr/bin/chatgpt\"\nhost=\"1\"\n"
             "network=\"1\"\ncamera=\"1\"\ndirectories=\"%s\"\n"
             "desktop=\"chatgpt.desktop\"\nx11=\"0\"\n", projects);
    put("catalog/chatgpt.conf", script_text);
    struct click access[] = {
        { "foot       ", BTN_LEFT, 2, 0 },
        { "chatgpt       ", BTN_LEFT, 2, 0 },
        { "[ Save ]", BTN_LEFT, 1, 0 },
        { "[ Save and install ]", BTN_LEFT, 1, 0 },
        { "Access", BTN_LEFT, 2, 0 },
        { "Preset", BTN_LEFT, 2, 0 },
        { "(blank)", BTN_LEFT, 2, 0 },
        { "Arguments", BTN_LEFT, 2, 0 },
        { "sandbox   ", BTN_LEFT, 2, 0 },
        { "chatgpt   ", BTN_LEFT, 2, 0 },
    };
    clicks = access;
    unlink(D("calls"));
    run("new: all recipes offered, system name wins; refusing real system access saves and installs nothing",
        (const int[]){ '\t', 'n', '\n', SNAP, CLICK(0), SNAP, CLICK(3), SNAP, 'n', SNAP, 27, END });
    snprintf(script_text, sizeof script_text, "%-16s %s", "foot", "foot");
    assert(strstr(snaps[0][3], script_text) && strstr(snaps[0][4], "chatgpt") && strstr(snaps[0][5], "edge"));
    assert(strstr(snaps[0][6], "firefox") && strstr(snaps[0][7], "(blank)") && !snapped("foot-app"));
    assert(snapped("Access") && snapped("real system and full home") && snapped("Save and install"));
    assert(!snap_has(1, "Camera") && !snap_has(1, "Directories"));
    assert(snapped("Packages will be installed on the real system.") && snapped("system files as you."));
    assert(snapped("sudo still requires authentication.") && snapped("whole process tree."));
    assert(snapped("nothing saved or installed") && !has("conf/foot.conf", "host="));
    assert(!has("calls", "app build foot"));

    run("new: system host recipe explicitly granted and saved",
        (const int[]){ '\t', 'n', '\n', CLICK(0), CLICK(2), 'y', END });
    assert(has("conf/foot.conf", "# foot: a host program\n") && has("conf/foot.conf", "host=\"1\""));
    assert(has("conf/foot.conf", "package=\"foot\"") && !has("conf/foot.conf", "foot-app"));

    run("new: switching a host recipe to blank drops its access request",
        (const int[]){ '\t', 'n', '\n', CLICK(1), CLICK(5), CLICK(6), SNAP, 27, END });
    assert(snap_has(0, "isolated filesystem") && !snap_has(0, "real system and full home"));
    assert(snapped("Camera") && snapped("Directories"));

    put("search.txt", "chatgpt\trecipe\tchatgpt-desktop\t\t/usr/bin/chatgpt\t\t1\tChatGPT recipe\n");
    unlink(D("calls"));
    run("store search: downloaded full recipe, permissions shown; cancel saves and installs nothing",
        (const int[]){ '\t', '/', 'c', '\n', '\n', SNAP, '\n', SNAP, 27, 27, END });
    assert(snapped("real system and full home") && snapped("Allow chatgpt to access your real system?"));
    assert(!has("conf/chatgpt.conf", "host=") && !has("calls", "app build chatgpt"));
    run("narrow store form: install remains visible; disclosure wraps; Enter refuses by default",
        (const int[]){ NARROW, '\t', '/', 'c', '\n', '\n', SNAP, '\n', SNAP, '\n', SNAP, 27, END });
    assert(snap_has(0, "Save and install") && snap_has(0, "real system and full home"));
    assert(snap_has(1, "system files as you.") && snap_has(1, "authentication."));
    assert(snap_has(1, "whole process") && snap_has(1, "tree.") && snap_has(1, "N: no"));
    assert(snap_has(2, "access not granted") && !has("conf/chatgpt.conf", "host="));
    run("store search: grant real system access, save the full recipe and install",
        (const int[]){ '\t', '/', 'c', '\n', '\n', '\n', 'y', WAIT, WAIT, END });
    assert(has("calls", "app build chatgpt\n") && has("conf/chatgpt.conf", "host=\"1\""));
    assert(has("conf/chatgpt.conf", "camera=\"1\"") && has("conf/chatgpt.conf", projects));
    assert(has("conf/chatgpt.conf", "desktop=\"chatgpt.desktop\"") && has("conf/chatgpt.conf", "x11=\"0\""));

    snprintf(script_text, sizeof script_text,
             "#!/bin/sh\necho \"app $*\" >>%s/calls\n"
             "[ \"$1\" != build ] || { echo 'install failed: offline' >&2; exit 23; }\necho done\n", dir);
    put("app", script_text);
    unlink(D("calls"));
    run("failed host install: show failure, keep its configuration for retry",
        (const int[]){ '\t', 'b', WAIT, WAIT, SNAP, END });
    assert(snapped("install chatgpt on the host (failed)") && snapped("install failed: offline"));
    assert(has("conf/chatgpt.conf", "host=\"1\"") && !has("state/chatgpt/built", "date="));
    snprintf(script_text, sizeof script_text, "#!/bin/sh\necho \"app $*\" >>%s/calls\necho done\n", dir);
    put("app", script_text);
    run("retry host install from the saved recipe",
        (const int[]){ '\t', 'b', WAIT, WAIT, SNAP, END });
    assert(has("calls", "app build chatgpt\n") && snapped("install chatgpt on the host (done)"));

    run("installed host-access app stays listed with truthful details",
        (const int[]){ '\t', SNAP, END });
    assert(snapped("chatgpt") && snapped("packages on the host") && snapped("real system and full home"));
    assert(snapped("devices") && snapped("by your permissions") && !snapped("Its build installed nothing"));
    run("existing real system access: an ordinary edit saves without another grant",
        (const int[]){ '\t', CLICK(9), CLICK(7), '-', 'x', '\n', CLICK(2), END });
    assert(has("conf/chatgpt.conf", "arguments=\"-x\"") && has("conf/chatgpt.conf", "host=\"1\""));
    unlink(D("calls"));
    run("host-access app has no overlay rollback; removing it keeps host packages and user files",
        (const int[]){ '\t', 'r', SNAP, 'd', SNAP, 'y', WAIT, WAIT, END });
    assert(snapped("there is no private build to roll back") && !has("calls", "app rollback"));
    assert(snapped("Host packages and your files will be kept") && has("calls", "app remove chatgpt\n"));
    assert(!has("calls", "--data"));

    put("conf/sandbox.conf", "package=\"sl\"\ncommand=\"/usr/bin/sl\"\nhost=\"0\"\n");
    unlink(D("calls"));
    run("existing isolated app: switching Access does not grant it until saving is confirmed",
        (const int[]){ '\t', CLICK(8), CLICK(4), SNAP, CLICK(3), 'n', SNAP, 27, END });
    assert(snapped("real system and full home") && snapped("nothing saved or installed"));
    assert(has("conf/sandbox.conf", "host=\"0\"") && !has("calls", "app build sandbox"));
    run("existing isolated app: explicitly grant the Access change and install",
        (const int[]){ '\t', CLICK(8), CLICK(4), CLICK(3), 'y', WAIT, WAIT, END });
    assert(has("conf/sandbox.conf", "host=\"1\"") && has("calls", "app build sandbox\n"));
    run("existing host access: return to isolated mode without a grant prompt",
        (const int[]){ '\t', CLICK(8), CLICK(4), SNAP, CLICK(2), END });
    assert(has("conf/sandbox.conf", "host=\"0\"") && snapped("isolated filesystem"));
    assert(snapped("Camera") && snapped("Directories"));
    unlink(D("conf/sandbox.conf"));
    unlink(D("conf/chatgpt.conf"));
    unlink(D("conf/foot.conf"));
    unlink(D("presets/foot.conf"));
    unlink(D("catalog/foot.conf"));
    unlink(D("catalog/chatgpt.conf"));

    puts("ALL PASSED");
    return 0;
}
