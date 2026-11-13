/* Process 1's rc.conf reload (SIGHUP): additions, removals and restarts, no
 * fixed service count, and a failed read or allocation leaves it unchanged.
 * Last, a service taken out and written back at once while its TERM cleanup
 * runs (test-init-services-svc.sh, ordered by its events on a pipe and a FIFO
 * gate): the new instance starts only once the old one's tree has ended, its
 * own cleanup and that of a child of its session alike.
 *   test-init-services NEW_DIRECTORY */
#include <stdlib.h>

static int allocations_left = -1;

static void *settings_realloc(void *p, size_t size)
{
    if (allocations_left == 0) {
        return NULL;
    }
    if (allocations_left > 0) {
        allocations_left--;
    }
    return realloc(p, size);
}

#define realloc settings_realloc
#define main init_main
#include "../init/init.c"
#undef main
#undef realloc
#include <assert.h>
#include <sys/prctl.h>

void say(const char *fmt, ...) { (void)fmt; }
void keys_reload(void) {} /* rc.conf's key settings: not this test's */
int apps_reap(pid_t pid, int status) { (void)pid; (void)status; return 0; } /* no application here */

static const char *const *conf;
static int read_error;

int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    for (int i = 0; conf[i]; i += 2) {
        fn(conf[i], conf[i + 1], arg);
    }
    return read_error ? -1 : 0;
}

static struct service *find(const char *command)
{
    for (size_t i = 0; i < nservices; i++) {
        if (strcmp(services[i].command, command) == 0) {
            return &services[i];
        }
    }
    return NULL;
}

/* What the main loop does with services that are not running. */
static void start_waiting(void)
{
    for (size_t i = 0; i < nservices; i++) {
        if (service_due(&services[i], now_ms())) {
            service_start(&services[i]);
        }
    }
}

static int alive(pid_t pid)
{
    return waitpid(pid, NULL, WNOHANG) == 0;
}

/* The services' events (test-init-services-svc.sh writes "EVENT TAG PID" to
 * $SVC_EVFD), in the order they came. */
static char events[8192];
static int event_fd = -1;

static const char *event_line(const char *head)
{
    for (const char *line = events; *line;) {
        if (!strncmp(line, head, strlen(head))) return line;
        const char *next = strchr(line, '\n');
        if (!next) break;
        line = next + 1;
    }
    return NULL;
}

static pid_t event_pid(const char *head)
{
    const char *line = event_line(head);
    return line ? (pid_t)atoi(line + strlen(head)) : 0;
}

/* What the services said within 20 ms, into events. */
static void collect(void)
{
    struct pollfd p = { .fd = event_fd, .events = POLLIN };
    if (poll(&p, 1, 20) == 1) {
        size_t n = strlen(events);
        ssize_t got = read(event_fd, events + n, sizeof events - n - 1);
        if (got > 0) events[n + (size_t)got] = '\0';
    }
}

/* Up to ms for an event line that starts with head. */
static pid_t wait_event(const char *head, int ms)
{
    for (int left = ms; !event_line(head) && left > 0; left -= 20) {
        collect();
    }
    return event_pid(head);
}

static long event_at(const char *head)
{
    const char *line = event_line(head);
    return line ? (long)(line - events) : -1;
}

static void release(const char *dir, const char *tag)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/gate-%s", dir, tag);
    int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    unlink(path);
    assert(fd >= 0 && write(fd, "\n", 1) == 1);
    close(fd);
}

/* Reap until service command has ended (pid 0), up to 3 s. */
static int ended_service(const char *command)
{
    for (int i = 0; i < 300 && find(command) && find(command)->pid != 0; i++) {
        usleep(10000);
        reap();
    }
    return !find(command) || find(command)->pid == 0;
}

/* A service stopped (its line taken out) and wanted again at once: the new
 * instance starts only after the old one's cleanup has ended. tag one: the
 * cleanup is the service's own (TERM trapped); tag tree: its main process
 * ends at once, a child of its session still cleans up. */
static int restart_waits(const char *dir, const char *script, const char *tag)
{
    char command[256], gate[1024], head[64];
    snprintf(command, sizeof command, "exec sh %s %s %s", script, dir, tag);
    snprintf(gate, sizeof gate, "%s/gate-%s", dir, tag);
    assert(mkfifo(gate, 0600) == 0);
    const char *with[] = { "service_test", command, NULL }, *without[] = { NULL };
    conf = with;
    read_settings();
    start_waiting();
    snprintf(head, sizeof head, "start %s ", tag);
    pid_t old = wait_event(head, 3000);
    assert(old > 0 && old == find(command)->pid);
    snprintf(head, sizeof head, "child-ready %s ", tag);
    assert(strcmp(tag, "tree") || wait_event(head, 3000)); /* its child's TERM trap is set */
    conf = without;
    read_settings(); /* SIGTERM to its session */
    snprintf(head, sizeof head, "%s %s ", strcmp(tag, "tree") ? "term" : "child-term", tag);
    assert(wait_event(head, 3000));
    conf = with;
    for (int i = 0; i < 20; i++) { /* the main loop goes round while the cleanup runs */
        usleep(10000);
        reap();
        read_settings();
        start_waiting();
    }
    snprintf(head, sizeof head, "start %s ", tag);
    pid_t during = 0;
    for (int i = 0; i < 5; i++) {
        collect(); /* what came meanwhile */
    }
    for (const char *line = events; (line = strstr(line, head)) != NULL; line++) {
        if ((pid_t)atoi(line + strlen(head)) != old) during = (pid_t)atoi(line + strlen(head));
    }
    release(dir, tag);
    snprintf(head, sizeof head, "%s %s ", strcmp(tag, "tree") ? "end" : "child-end", tag);
    long cleaned = wait_event(head, 3000) ? event_at(head) : -1;
    pid_t now = 0;
    for (int i = 0; i < 300 && !now; i++) { /* the main loop: reap, then start what is due */
        usleep(10000);
        reap();
        struct service *s = find(command);
        if (s && s->pid == 0 && s->restart_at > now_ms()) {
            s->restart_at = 0; /* its 5 s are not this test's */
        }
        start_waiting();
        if (s && s->pid != 0 && s->pid != old) {
            now = s->pid;
        }
    }
    char started[64];
    snprintf(started, sizeof started, "start %s %d", tag, (int)now);
    wait_event(started, 3000);
    int ok = !during && cleaned >= 0 && now > 0 && now != old && event_at(started) > cleaned;
    if (!ok) printf("  %s: a new instance during the cleanup: %d; cleanup ended: %s; then: %d\n%s", tag,
                    (int)during, cleaned >= 0 ? "yes" : "no", (int)now, events);
    conf = without;
    read_settings();
    ended_service(command);
    reap();
    return ok;
}

int main(int argc, char **argv)
{
    /* As process 1: these blocked, read from a signalfd; services unblock them. */
    sigemptyset(&handled);
    sigaddset(&handled, SIGCHLD);
    sigaddset(&handled, SIGHUP);
    sigaddset(&handled, SIGTERM);
    sigprocmask(SIG_BLOCK, &handled, NULL);
    static const char *const a[] = {
        "hibernate_after", "30",
        "service_one", "sleep 101",
        "service_two", "sleep 102; true", /* sh stays: the session holds two processes */
        "key_home", "Super+Esc",          /* someone else's: no complaint, no effect */
        NULL,
    };
    conf = a;
    read_settings();
    start_waiting();
    assert(nservices == 2 && hibernate_after == 30);
    pid_t one = find("sleep 101")->pid, two = find("sleep 102; true")->pid;
    assert(one > 0 && two > 0 && alive(one) && alive(two));

    static const char *const b[] = {
        "hibernate_after", "45",
        "service_two", "sleep 102; true",
        "service_three", "sleep 103",
        NULL,
    };
    conf = b;
    read_settings();
    usleep(200000);
    reap(); /* the one taken out has ended: forgotten here, never started again */
    assert(hibernate_after == 45);
    assert(find("sleep 101") == NULL && !alive(one));         /* ended and forgotten */
    assert(find("sleep 102; true")->pid == two && alive(two)); /* untouched */
    assert(find("sleep 103") && find("sleep 103")->pid == 0);  /* waiting to start */
    start_waiting();
    pid_t three = find("sleep 103")->pid;
    assert(three > 0 && alive(three));

    static const char *const c[] = { NULL };
    conf = c;
    read_settings();
    usleep(200000);
    reap(); /* both taken out have ended */
    assert(nservices == 0 && !alive(two) && !alive(three));
    /* sleep 102 ran under sh -c in its own session: it ended with it. */
    assert(system("pgrep -x -f 'sleep 102' >/dev/null") != 0);
    /* Cross the old 8-service limit and the proposed 32-service patch. The
     * list grows with the configuration, not with another constant. */
    enum { MANY = 65 };
    char keys[MANY][32], commands[MANY][64];
    const char *many[MANY * 2 + 1];
    for (int i = 0; i < MANY; i++) {
        snprintf(keys[i], sizeof keys[i], "service_test_%d", i);
        snprintf(commands[i], sizeof commands[i], "/bin/true %d", i);
        many[i * 2] = keys[i];
        many[i * 2 + 1] = commands[i];
    }
    many[MANY * 2] = NULL;
    conf = many;
    read_settings();
    assert(nservices == MANY);
    for (int i = 0; i < MANY; i++) {
        assert(find(commands[i]) && find(commands[i])->pid == 0);
    }
    /* Reap and restart a service at the end of the grown list. */
    struct service *last = find(commands[MANY - 1]);
    service_start(last);
    pid_t first_pid = last->pid;
    for (int i = 0; last->pid != 0 && i < 100; i++) {
        usleep(10000);
        reap();
    }
    assert(first_pid > 0 && last->pid == 0 && last->restart_at > now_ms());
    last->restart_at = 0;
    start_waiting();
    for (int i = 0; last->pid != 0 && i < 100; i++) {
        usleep(10000);
        reap();
    }
    assert(last->pid == 0 && last->restart_at > now_ms());
    struct service *old = services;
    conf = a;
    read_error = 1;
    read_settings();
    assert(services == old && nservices == MANY && hibernate_after == 45);
    read_error = 0;
    allocations_left = 1; /* fail after part of the new list was read */
    read_settings();
    assert(services == old && nservices == MANY && hibernate_after == 45);
    allocations_left = -1;
    /* Shrink, preserve state on a reload, and ignore duplicate commands. */
    conf = b;
    read_settings();
    assert(nservices == 2);
    find("sleep 102; true")->restart_at = 12345;
    read_settings();
    assert(nservices == 2 && find("sleep 102; true")->restart_at == 12345);
    static const char *const duplicate[] = {
        "service_one", "sleep 102; true", "service_two", "sleep 102; true", NULL,
    };
    conf = duplicate;
    read_settings();
    assert(nservices == 1 && find("sleep 102; true")->restart_at == 12345);
    /* Taken out while it runs, then wanted again before it has ended: the same
     * service (its pid), started again only once it has ended. */
    static const char *const d[] = { "service_keep", "sleep 104", NULL };
    static const char *const e[] = { NULL };
    conf = d;
    read_settings();
    start_waiting();
    pid_t keep = find("sleep 104")->pid;
    assert(keep > 0 && alive(keep));
    conf = e;
    read_settings();
    assert(find("sleep 104") && find("sleep 104")->retiring && find("sleep 104")->pid == keep);
    conf = d;
    read_settings();
    assert(find("sleep 104") && !find("sleep 104")->retiring && find("sleep 104")->pid == keep);
    kill(-keep, SIGTERM);
    kill(keep, SIGTERM);
    for (int i = 0; find("sleep 104")->pid != 0 && i < 100; i++) {
        usleep(10000);
        reap();
    }
    assert(find("sleep 104")->pid == 0 && find("sleep 104")->restart_at > now_ms());
    conf = c;
    read_settings();
    assert(nservices == 0 && services == NULL);
    /* A child that ends after its parent (as the pacman guard does) is reparented
     * to the nearest subreaper; here, this process: reap() must end it. */
    assert(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    int pfd[2];
    assert(pipe(pfd) == 0);
    pid_t parent = fork();
    if (parent == 0) {
        close(pfd[0]);
        pid_t grandchild = fork();
        if (grandchild == 0) {
            close(pfd[1]);
            usleep(100000);
            _exit(0);
        }
        assert(write(pfd[1], &grandchild, sizeof grandchild) == (ssize_t)sizeof grandchild);
        close(pfd[1]);
        _exit(0);
    }
    close(pfd[1]);
    pid_t orphan = 0;
    assert(read(pfd[0], &orphan, sizeof orphan) == (ssize_t)sizeof orphan);
    close(pfd[0]);
    for (int i = 0; i < 300 && kill(orphan, 0) == 0; i++) {
        usleep(10000);
        reap();
    }
    assert(kill(orphan, 0) != 0 && errno == ESRCH); /* ended and reaped: no zombie left */
    puts("PASS a reparented child that ends is reaped by process 1 (no zombie)");

    /* Stopped and wanted again at once, with real cleanups: the new instance
     * never overlaps the old one's. */
    assert(argc == 2 && mkdir(argv[1], 0700) == 0);
    char script[1024];
    snprintf(script, sizeof script, "%s", __FILE__);
    char *slash = strrchr(script, '/');
    assert(slash);
    snprintf(slash + 1, sizeof script - (size_t)(slash + 1 - script), "test-init-services-svc.sh");
    int ev[2];
    assert(pipe2(ev, O_CLOEXEC) == 0 && fcntl(ev[1], F_SETFD, 0) == 0);
    event_fd = ev[0];
    char number[16];
    snprintf(number, sizeof number, "%d", ev[1]);
    setenv("SVC_EVFD", number, 1);
    int failed = 0, ok = restart_waits(argv[1], script, "one");
    printf("%s a service taken out while its TERM cleanup runs, written back at once: no second instance until "
           "that cleanup has ended, then a new one\n", ok ? "PASS" : "FAIL");
    failed |= !ok;
    ok = restart_waits(argv[1], script, "tree");
    printf("%s a service whose main process ends at once while a child of its session still cleans up, written "
           "back at once: the new instance waits for that cleanup too\n", ok ? "PASS" : "FAIL");
    failed |= !ok;
    if (failed) return 1;
    puts("init: service reload, growth, restart and failed reload passed.");
    return 0;
}
