/* Process 1's application operations (init/apps.c) with real processes:
 * apps.c forks and runs test-apps-lifecycle-tool.sh as locachy-app, and this
 * program reaps them as process 1 does (a subreaper, apps_reap). The cgroups
 * are fixture files under FX (a compile-time path): cgroup.events is derived
 * from whether the processes cgroup.procs lists still live, as the kernel
 * would. Steps are ordered by the tool's events on a pipe and its FIFO gates;
 * every wait is bounded. Only the display (fixture files, no window control)
 * and the clock (uptime, to pass a deadline) are doubles.
 * Built and run by test-apps-lifecycle.sh. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if !defined FX || !defined TOOL
#error "FX: the fixture directory; TOOL: test-apps-lifecycle-tool.sh installed (strings)"
#endif
#define APP_CONF FX "/conf"
#define APP_RUN FX "/run"
#define APP_TOOL TOOL
#define APP_CGROUP_ROOT FX "/cgroup"
#define APP_DISPLAY FX "/display"
#define APP_DISPLAY_READY FX "/display-ready"
#define APP_CLIPBOARD FX "/no-clipboard"
#define APP_PROC FX "/proc"
#define APP_NATIVE_DIR FX "/bin"
#define APP_WAYLAND_RUNTIME FX
#define APP_WINDOW_CONTROL "/nonexistent/locachy-control.sock"
#include "../init/apps.c"

#include <stdarg.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <time.h>

static double clock_offset;
static char said[65536];
static char evlog[262144];
static int evfd = -1, sigfd = -1, passed, failed;
static const char *names[16];
static int nnames;

double uptime(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9 + clock_offset;
}

void say(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(said);
    va_start(ap, fmt);
    vsnprintf(said + n, sizeof said - n, fmt, ap);
    va_end(ap);
    n = strlen(said);
    if (n + 1 < sizeof said) { said[n] = '\n'; said[n + 1] = '\0'; }
}

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    ok ? passed++ : failed++;
}

static void put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f || fputs(text, f) < 0 || fclose(f) != 0) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
}

static int alive(pid_t pid)
{
    char path[64], text[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "re");
    if (!f) return 0;
    char *got = fgets(text, sizeof text, f);
    fclose(f);
    char *end = got ? strrchr(text, ')') : NULL;
    return end && end[1] == ' ' && end[2] != 'Z';
}

/* cgroup.events of app_NAME, as the kernel keeps it: populated while one of
 * the processes it lists lives; frozen when populated and cgroup.freeze 1. */
static void sync_cgroup(const char *name)
{
    char path[512], text[64] = "0";
    int populated = 0;
    long pid;
    snprintf(path, sizeof path, APP_CGROUP_ROOT "/app_%s/cgroup.procs", name);
    FILE *f = fopen(path, "re");
    while (f && fscanf(f, "%ld", &pid) == 1) populated |= alive((pid_t)pid);
    if (f) fclose(f);
    snprintf(path, sizeof path, APP_CGROUP_ROOT "/app_%s/cgroup.freeze", name);
    f = fopen(path, "re");
    if (f && !fgets(text, sizeof text, f)) text[0] = '0';
    if (f) fclose(f);
    snprintf(path, sizeof path, APP_CGROUP_ROOT "/app_%s/cgroup.events", name);
    char events[64];
    snprintf(events, sizeof events, "populated %d\nfrozen %d\n", populated, populated && text[0] == '1');
    put_file(path, events);
}

static void sync_all(void)
{
    for (int i = 0; i < nnames; i++) sync_cgroup(names[i]);
}

/* What process 1's reap does: every child that ended to apps_reap. Each is
 * noted in the event log ("reaped - PID STATUS"), in order with the tool's. */
static void reap_all(void)
{
    int status;
    pid_t pid;
    sync_all();
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        size_t n = strlen(evlog);
        snprintf(evlog + n, sizeof evlog - n, "reaped - %d %d\n", (int)pid,
                 WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
        sync_all();
        apps_reap(pid, status);
        sync_all();
    }
}

/* Up to ms for an event line, a child's end or the next 20 ms tick. */
static void pump(int ms)
{
    struct pollfd p[2] = { { .fd = evfd, .events = POLLIN }, { .fd = sigfd, .events = POLLIN } };
    if (poll(p, 2, ms) > 0) {
        if (p[0].revents & POLLIN) {
            size_t n = strlen(evlog);
            ssize_t got = read(evfd, evlog + n, sizeof evlog - n - 1);
            if (got > 0) evlog[n + (size_t)got] = '\0';
        }
        if (p[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sigfd, &si, sizeof si) == (ssize_t)sizeof si) {}
        }
    }
    reap_all();
}

/* The pid of the nth (from 1) "EVENT NAME PID" line, else 0; extra: its
 * fourth field. */
static pid_t event_pid(const char *event, const char *name, int nth, long *extra)
{
    char head[96];
    int len = snprintf(head, sizeof head, "%s %s ", event, name);
    for (const char *line = evlog; *line;) {
        const char *next = strchr(line, '\n');
        if (!next) break;
        if (!strncmp(line, head, (size_t)len) && --nth == 0) {
            long pid = 0, more = 0;
            int fields = sscanf(line + len, "%ld %ld", &pid, &more);
            if (extra) *extra = fields == 2 ? more : 0;
            return (pid_t)pid;
        }
        line = next + 1;
    }
    return 0;
}

static int event_count(const char *event, const char *name)
{
    int n = 0;
    while (event_pid(event, name, n + 1, NULL)) n++;
    return n;
}

/* Where an event line is in the log (its order), -1 if not there. */
static long event_at(const char *text)
{
    const char *at = strstr(evlog, text);
    return at ? (long)(at - evlog) : -1;
}

static pid_t wait_event(const char *event, const char *name, int nth, int ms)
{
    double until = uptime() + ms / 1e3;
    pid_t pid;
    while (!(pid = event_pid(event, name, nth, NULL)) && uptime() < until) pump(20);
    return pid;
}

static int reaped_status(pid_t pid)
{
    char text[48];
    snprintf(text, sizeof text, "reaped - %d ", (int)pid);
    const char *at = strstr(evlog, text);
    return at ? atoi(at + strlen(text)) : -1;
}

static int wait_reaped(pid_t pid, int ms)
{
    double until = uptime() + ms / 1e3;
    while (reaped_status(pid) < 0 && uptime() < until) pump(20);
    return reaped_status(pid);
}

/* NAME's row in apps_list: its state, and its error into error. */
static void state_of(const char *name, char *state, size_t size, char *error, size_t esize)
{
    static char list[16384];
    apps_list(list, sizeof list);
    snprintf(state, size, "absent");
    if (error) error[0] = '\0';
    size_t len = strlen(name);
    for (char *line = list; *line;) {
        char *next = strchr(line, '\n');
        if (next) *next = '\0';
        if (!strncmp(line, name, len) && line[len] == '\t') {
            char *st = line + len + 1, *tab = strchr(st, '\t');
            if (tab) *tab = '\0';
            snprintf(state, size, "%s", st);
            char *err = tab ? strrchr(tab + 1, '\t') : NULL;
            if (error && err) snprintf(error, esize, "%s", err + 1);
        }
        if (!next) break;
        line = next + 1;
    }
}

static int wait_state(const char *name, const char *want, int ms)
{
    char st[32];
    double until = uptime() + ms / 1e3;
    for (;;) {
        reap_all();
        state_of(name, st, sizeof st, NULL, 0);
        if (!strcmp(st, want)) return 1;
        if (uptime() >= until) return 0;
        pump(20);
    }
}

static int is_state(const char *name, const char *want)
{
    char st[32];
    reap_all();
    state_of(name, st, sizeof st, NULL, 0);
    return !strcmp(st, want);
}

static void gate(const char *name, const char *step)
{
    char path[512];
    snprintf(path, sizeof path, FX "/gate-%s-%s", name, step);
    unlink(path);
    if (mkfifo(path, 0600) != 0) { perror(path); exit(2); }
}

/* Lets the one waiting at gate STEP of NAME go on; the gate goes. */
static int release(const char *name, const char *step)
{
    char path[512];
    snprintf(path, sizeof path, FX "/gate-%s-%s", name, step);
    int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    unlink(path);
    if (fd < 0) return 0;
    int ok = write(fd, "\n", 1) == 1;
    close(fd);
    return ok;
}

static void app_exit(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, FX "/app-exit-%s", name);
    int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) { (void)!write(fd, "\n", 1); close(fd); }
}

static void app_setup(const char *name)
{
    char path[512];
    names[nnames++] = name;
    snprintf(path, sizeof path, APP_CONF "/%s.conf", name);
    put_file(path, "package=\"fixture\"\n");
    snprintf(path, sizeof path, APP_CGROUP_ROOT "/app_%s", name);
    mkdir(path, 0755);
    snprintf(path, sizeof path, APP_CGROUP_ROOT "/app_%s/cgroup.procs", name);
    put_file(path, "");
    snprintf(path, sizeof path, APP_CGROUP_ROOT "/app_%s/cgroup.freeze", name);
    put_file(path, "0\n");
    snprintf(path, sizeof path, FX "/app-exit-%s", name);
    mkfifo(path, 0600);
    sync_cgroup(name);
}

static int act(const char *action, const char *name, char *why, size_t size)
{
    reap_all();
    int r = apps_action(action, name, why, size);
    sync_all();
    return r;
}

static int refused(const char *action, const char *name, const char *message)
{
    char why[256] = "";
    int r = act(action, name, why, sizeof why);
    if (r == -1 && !strcmp(why, message)) return 1;
    printf("  %s %s: %d \"%s\"\n", action, name, r, why);
    return 0;
}

static int answered(const char *action, const char *name, const char *message)
{
    char why[256] = "";
    int r = act(action, name, why, sizeof why);
    if (r == 0 && !strcmp(why, message)) return 1;
    printf("  %s %s: %d \"%s\"\n", action, name, r, why);
    return 0;
}

static const char busy[] = "container operation still in progress";

/* An application started and running (its launcher in app_NAME, ready). */
static pid_t running_app(const char *name)
{
    char why[256];
    if (act("start", name, why, sizeof why) != 0) return 0;
    pid_t launcher = wait_event("start", name, 1, 5000);
    return launcher && wait_event("ready", name, 1, 5000) && wait_state(name, "running", 5000) ? launcher : 0;
}

static int open_refused(const char *name)
{
    char why[256] = "";
    reap_all();
    int r = apps_open(name, "/target", why, sizeof why);
    sync_all();
    if (r == -1 && !strcmp(why, busy)) return 1;
    printf("  open %s /target: %d \"%s\"\n", name, r, why);
    return 0;
}

static int closed_clean(const char *name)
{
    char st[32], error[256];
    int ok = wait_state(name, "closed", 5000);
    state_of(name, st, sizeof st, error, sizeof error);
    if (error[0]) printf("  %s: error \"%s\"\n", name, error);
    return ok && !error[0];
}

/* (a) Stopped while it starts, its launcher not yet in app_NAME (waiting at
 * gate enter, as for the app's lock behind a build); (b) what the other
 * actions get meanwhile. */
static void stop_while_starting(void)
{
    const char *n = "a1";
    char why[256], text[64];
    app_setup(n);
    gate(n, "enter");
    int ok = act("start", n, why, sizeof why) == 0 && !strcmp(why, "start accepted");
    pid_t launcher = wait_event("start", n, 1, 5000);
    ok = ok && launcher && wait_event("gate-enter", n, 1, 5000) && is_state(n, "starting");
    long helper = 0;
    event_pid("helper", n, 1, &helper);
    check(ok && helper > 0, "a start: its launcher waits outside app_NAME, with a helper in its process group (starting)");
    check(answered("open", n, "open accepted") && refused("show", n, "container is closed") &&
          refused("resume", n, busy) && refused("start", n, busy) && open_refused(n) &&
          event_count("start", n) == 1,
          "while it starts: open is the start under way, show says closed, resume, start and open with a target "
          "are refused; one launcher");
    check(answered("stop", n, "stop accepted") && is_state(n, "stopping"), "a stop while it starts: accepted, stopping");
    check(refused("open", n, busy) && refused("show", n, busy) && refused("start", n, busy) &&
          answered("stop", n, "stop already in progress"),
          "while the start is cancelled: open, show and start are refused; another stop is the one under way");
    int status = wait_reaped(launcher, 5000);
    for (int i = 0; i < 100 && alive((pid_t)helper); i++) pump(20);
    check(status == 128 + SIGKILL && !alive((pid_t)helper),
          "the launcher outside app_NAME is killed (SIGKILL) with its process group, and reaped");
    pid_t stop = wait_event("stop", n, 1, 5000);
    snprintf(text, sizeof text, "reaped - %d ", (int)launcher);
    check(stop && event_at(text) >= 0 && event_at(text) < event_at("stop a1 "),
          "the stop helper starts only once the killed launcher has been reaped");
    check(closed_clean(n) && event_count("app", n) == 0 && event_count("start", n) == 1,
          "it ends closed without an error: the old launcher's status changed nothing, nothing entered app_NAME, "
          "nothing started again");
}

/* (b) A running app stopped; (d) a start right after it does not overlap the
 * old launcher's cleanup. */
static void stop_then_start(void)
{
    const char *n = "a2";
    char text[64], why[256];
    app_setup(n);
    gate(n, "cleanup");
    pid_t first = running_app(n);
    check(first != 0, "an app runs: its launcher in app_NAME, ready");
    gate(n, "stop");
    check(answered("stop", n, "stop accepted") && wait_event("gate-stop", n, 1, 5000) && is_state(n, "stopping"),
          "a stop of a running app: its helper runs, stopping");
    check(refused("open", n, busy) && refused("show", n, busy) && refused("resume", n, busy) &&
          refused("start", n, busy) && open_refused(n) && answered("stop", n, "stop already in progress"),
          "while it stops: open, show, resume, start and open with a target are refused; another stop is the one "
          "under way");
    release(n, "stop");
    pid_t cleaning = wait_event("gate-cleanup", n, 1, 5000);
    wait_event("stop-end", n, 1, 5000);
    check(cleaning == first && is_state(n, "stopping") && refused("start", n, busy) && refused("open", n, busy),
          "the application has ended, its launcher still cleans up: stopping; start and open are refused");
    release(n, "cleanup");
    check(closed_clean(n) && reaped_status(first) >= 0, "once the launcher has ended: closed, no error");
    int ok = act("start", n, why, sizeof why) == 0;
    pid_t second = wait_event("start", n, 2, 5000);
    char started[64];
    snprintf(text, sizeof text, "cleanup-end %s %d ", n, (int)first);
    snprintf(started, sizeof started, "start %s %d ", n, (int)second);
    check(ok && second && second != first && event_at(text) >= 0 && event_at(text) < event_at(started),
          "a start right after: a new launcher, only after the old one's cleanup ended");
    if (wait_event("ready", n, 2, 5000)) app_exit(n);
    wait_state(n, "closed", 5000);
}

/* (d) The application ended by itself; its launcher still cleans up. */
static void start_during_cleanup(void)
{
    const char *n = "a3";
    char why[256] = "";
    app_setup(n);
    gate(n, "cleanup");
    pid_t first = running_app(n);
    app_exit(n);
    int ok = first && wait_event("gate-cleanup", n, 1, 5000);
    int r = act("start", n, why, sizeof why);
    if (r != -1 || strcmp(why, busy)) printf("  start %s: %d \"%s\"\n", n, r, why);
    check(ok && r == -1 && !strcmp(why, busy) && event_count("start", n) == 1,
          "the application ended by itself, its launcher still cleans up: a start is refused, no second launcher "
          "overlaps that cleanup");
    release(n, "cleanup");
    wait_reaped(first, 5000);
    ok = closed_clean(n) && answered("start", n, "start accepted") && wait_event("ready", n, 2, 5000);
    check(ok, "once that launcher has ended: closed, and a start runs a new one");
    app_exit(n);
    wait_state(n, "closed", 5000);
}

/* (c) A running app whose freeze (or open) helper is under way, stopped: the
 * helper ends, the stop runs, the stale helper's end and the launcher's end
 * after the stop overwrite nothing. */
static void stop_while_helper(const char *n, const char *how)
{
    char why[256] = "", what[128];
    int freezing = !strcmp(how, "freeze");
    app_setup(n);
    pid_t launcher = running_app(n);
    gate(n, how);
    int ok = launcher && (freezing ? act("freeze", n, why, sizeof why) : apps_open(n, "/target", why, sizeof why)) == 0;
    pid_t job = wait_event(freezing ? "freeze" : "open-running", n, 1, 5000);
    ok = ok && job && wait_event(freezing ? "gate-freeze" : "gate-open", n, 1, 5000) &&
         is_state(n, freezing ? "freezing" : "opening");
    snprintf(what, sizeof what, "a running app: its %s helper is under way (%s)", how, freezing ? "freezing" : "opening");
    check(ok, what);
    snprintf(what, sizeof what, "while it is %s: open and show are refused", freezing ? "freezing" : "opening");
    check(refused("open", n, busy) && refused("show", n, busy), what);
    check(answered("stop", n, "stop accepted") && is_state(n, "stopping"), freezing ?
          "a stop while it freezes: accepted, stopping" : "a stop while it opens: accepted, stopping");
    snprintf(what, sizeof what, "the %s helper is killed (SIGKILL) and reaped", how);
    check(wait_reaped(job, 5000) == 128 + SIGKILL, what);
    check(wait_event("stop", n, 1, 5000) != 0,
          "then the stop helper runs, though the app's launcher still runs in app_NAME");
    snprintf(what, sizeof what, "it ends closed without an error: neither the killed %s helper nor the launcher's end "
             "after the stop is a failure", how);
    check(closed_clean(n) && (!freezing || event_count("freeze-end", n) == 0), what);
}

/* (a, c) Stopped while it starts, its launcher already in app_NAME. */
static void stop_while_entered(void)
{
    const char *n = "a6";
    char why[256];
    app_setup(n);
    gate(n, "ready");
    int ok = act("start", n, why, sizeof why) == 0;
    pid_t launcher = wait_event("start", n, 1, 5000);
    ok = ok && launcher && wait_event("gate-ready", n, 1, 5000) && is_state(n, "starting");
    ok = ok && answered("stop", n, "stop accepted") && is_state(n, "stopping");
    check(ok && wait_event("stop", n, 1, 5000),
          "a stop while it starts, its launcher already in app_NAME: stopped as a running app (the stop helper at once)");
    wait_reaped(launcher, 5000);
    check(closed_clean(n), "it ends closed: the launcher's end in the middle of its start is no failure");
}

/* (a) A start that timed out while its launcher still waits outside
 * app_NAME: that launcher is still an operation of the app. */
static void timed_out_start(void)
{
    const char *n = "a7";
    char why[256] = "";
    app_setup(n);
    gate(n, "enter");
    int ok = act("start", n, why, sizeof why) == 0;
    pid_t launcher = wait_event("start", n, 1, 5000);
    ok = ok && launcher && wait_event("gate-enter", n, 1, 5000);
    clock_offset += 21; /* past its 20 s */
    check(ok && wait_state(n, "failed", 2000), "a start whose launcher still waits outside app_NAME times out: failed");
    int r = act("start", n, why, sizeof why);
    if (r != -1 || strcmp(why, busy)) printf("  start %s: %d \"%s\"\n", n, r, why);
    check(r == -1 && !strcmp(why, busy) && event_count("start", n) == 1,
          "a start then is refused while that launcher lives: no second launcher");
    r = act("stop", n, why, sizeof why);
    printf("  stop %s: %d \"%s\"\n", n, r, why);
    check(r == 0 && wait_reaped(launcher, 5000) == 128 + SIGKILL,
          "a stop then kills that launcher and confirms it ended");
    release(n, "enter"); /* what it would do if it still lived */
    check(closed_clean(n) && !wait_event("app", n, 1, 300),
          "it ends closed: nothing enters app_NAME after the stop");
}

static void run_cases(void)
{
    stop_while_starting();
    stop_then_start();
    start_during_cleanup();
    stop_while_helper("a4", "freeze");
    stop_while_helper("a5", "open");
    stop_while_entered();
    timed_out_start(); /* last: it moves the clock on */
}

int main(void)
{
    if (mkdir(FX, 0700) != 0) { perror(FX); return 2; }
    mkdir(APP_CONF, 0755);
    mkdir(APP_RUN, 0700);
    mkdir(APP_CGROUP_ROOT, 0755);
    put_file(APP_DISPLAY, "");
    put_file(APP_DISPLAY_READY, "");
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) { perror("subreaper"); return 2; }
    int ev[2];
    if (pipe2(ev, O_CLOEXEC) != 0) return 2;
    evfd = ev[0];
    fcntl(evfd, F_SETFL, O_NONBLOCK);
    fcntl(ev[1], F_SETFD, 0); /* every helper apps.c starts inherits it */
    char number[16];
    snprintf(number, sizeof number, "%d", ev[1]);
    setenv("TEST_APPS_EVFD", number, 1);
    setenv("TEST_APPS", FX, 1);
    setenv("TEST_APPS_RUN", APP_RUN, 1);
    sigset_t chld;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld, NULL);
    sigfd = signalfd(-1, &chld, SFD_NONBLOCK | SFD_CLOEXEC);
    apps_display_ready();
    apps_init();
    run_cases();
    /* Nothing of the fixtures is left: what still runs ends. */
    for (const char *line = evlog; *line;) {
        long a = 0, b = 0;
        char word[32], name[64];
        if (sscanf(line, "%31s %63s %ld %ld", word, name, &a, &b) >= 3 && strcmp(word, "reaped")) {
            if (a > 1) kill((pid_t)a, SIGKILL);
            if (b > 1 && (!strcmp(word, "app") || !strcmp(word, "helper"))) kill((pid_t)b, SIGKILL);
        }
        const char *next = strchr(line, '\n');
        if (!next) break;
        line = next + 1;
    }
    for (int i = 0; i < 50 && waitpid(-1, NULL, WNOHANG) >= 0; i++) pump(20);
    if (failed) {
        printf("events:\n%slog:\n%s", evlog, said);
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
