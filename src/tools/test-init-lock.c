/* The lock screen in process 1 (init.c's lock, unlock, lock_wait, and gone
 * and message while locked), without DRM, inputs, or PID 1. Home and the
 * tasks are frames on socketpairs as in test-init-screen; the lock program is
 * this test itself, started by start() as process 1 starts it, which says
 * "locked" when given the screen and exits with the code it is sent.
 * start() makes its cgroups in /sys/fs/cgroup, so this runs only on a tmpfs
 * there, in a mount namespace of its own:
 *   unshare --mount --propagation private sh -c \
 *       'mount -t tmpfs tmpfs /sys/fs/cgroup && exec test-init-lock' */
#define main init_main
#include "../init/init.c"
#undef main
#include <assert.h>
#include <linux/magic.h>
#include <sys/vfs.h>

void say(const char *fmt, ...) { (void)fmt; }
void apps_list(char *buf, size_t size) { if (size) buf[0] = 0; }
int apps_display_state(void) { return APPS_DISPLAY_ACTIVE; }
int apps_open(const char *name, const char *target, char *why, size_t size)
{ (void)name; (void)target; (void)why; (void)size; abort(); }
int apps_action(const char *action, const char *name, char *why, size_t size)
{ (void)action; (void)name; (void)why; (void)size; abort(); }
void apps_tick(void) {}
int apps_reap(pid_t pid, int status) { (void)pid; (void)status; return 0; }
int screen_refresh(void) { return 1; }
int screen_lend(uint32_t *lessee) { *lessee = 42; return open("/dev/null", O_RDONLY); }
int screen_take_back(uint32_t lessee) { assert(lessee == 42); return 0; }
int screen_drop_master(uint32_t lessee) { (void)lessee; return 0; }
int keys_lend(int *fds, int max) { (void)fds; (void)max; return 0; }
void keys_take_back(const int *fds, int n) { (void)fds; assert(n == 0); }
int *pointer_lend(size_t *n, char (*paths)[POINTER_PATH], size_t max) { (void)paths; (void)max; *n = 0; return NULL; }
void pointer_take_back(int *fds, size_t n) { assert(!fds && n == 0); }

/* The lock program, as this test runs in start()'s child. */
static int fake_lock(void)
{
    char msg[CONTROL_MAX];
    int fds[CONTROL_MAX_FDS], n;
    for (;;) {
        int len = control_recv(3, msg, sizeof msg, fds, &n);
        for (int i = 0; i < n; i++) close(fds[i]);
        if (len <= 0) return 2;
        if (strcmp(msg, "take") == 0) control_send(3, "locked", 6, NULL, 0);
        else if (strncmp(msg, "exit ", 5) == 0) return atoi(msg + 5);
    }
}

static void events(const char *cgroup, const char *text)
{
    char path[160];
    snprintf(path, sizeof path, "%s/cgroup.events", cgroup);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

/* A frame with its own control socket (peer in *peer) and a cgroup that
 * freezes at once. */
static void frame(struct frame *f, const char *name, int *peer)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    memset(f, 0, sizeof *f);
    f->ctl = pair[0];
    *peer = pair[1];
    f->pid = 1000;
    f->lessee = 42;
    snprintf(f->name, sizeof f->name, "%s", name);
    snprintf(f->cgroup, sizeof f->cgroup, CGROUP "/%s", name);
    assert(mkdir(f->cgroup, 0700) == 0);
    events(f->cgroup, "populated 1\nfrozen 1\n");
}

/* Its program has released the screen (when it is next asked to). */
static void released(int peer)
{
    assert(send(peer, "released", 8, 0) == 8);
}

/* What the frame's program was last told (its peer's next messages, read
 * until one starting with first); fields joined by spaces. */
static char told[256];
static const char *last_told(int peer, const char *first)
{
    char msg[CONTROL_MAX], *fields[16];
    int fds[CONTROL_MAX_FDS], nfds;
    told[0] = 0;
    for (;;) {
        struct pollfd w = { .fd = peer, .events = POLLIN };
        if (poll(&w, 1, 0) != 1) return told;
        int len = control_recv(peer, msg, sizeof msg, fds, &nfds);
        for (int i = 0; i < nfds; i++) close(fds[i]);
        if (len <= 0) return told;
        int n = control_fields(msg, len, fields, 15);
        if (strcmp(fields[0], first) != 0) continue;
        size_t at = 0;
        for (int i = 0; i < n; i++) at += (size_t)snprintf(told + at, sizeof told - at, i ? " %s" : "%s", fields[i]);
        return told;
    }
}

/* A message from the frame's program, as process 1 receives it; then (if
 * then_released) it releases the screen when asked. */
static void from(struct frame *f, int peer, const char *const *fields, int then_released)
{
    assert(control_send_fields(peer, fields) == 0);
    if (then_released) released(peer);
    message(f);
}

/* The lock program ends with code; process 1 sees its socket close. */
static void lock_ends(int code)
{
    char text[16];
    int len = snprintf(text, sizeof text, "exit %d", code);
    events(locker.cgroup, "populated 0\n");
    assert(control_send(locker.ctl, text, (size_t)len, NULL, 0) == 0);
    while (locker.pid > 0) {
        struct pollfd w = { .fd = locker.ctl, .events = POLLIN };
        assert(poll(&w, 1, 5000) == 1);
        message(&locker);
    }
}

int main(int argc, char **argv)
{
    if (strcmp(argv[0], "/proc/self/exe") == 0) return fake_lock();
    assert(argc == 1);
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct statfs fs;
    assert(statfs(CGROUP, &fs) == 0 && fs.f_type == TMPFS_MAGIC); /* never the real cgroups */
    int hp, tp, dp;

    lock_argv[0] = "/nonexistent/lock";
    frame(&home, "home0", &hp);
    fg = &home;
    lock();
    assert(!locked && fg == &home && !home.frozen && locker.pid == 0);
    close(hp);
    puts("PASS without the lock program there is no locking");

    lock_argv[0] = "/proc/self/exe";
    frame(&home, "home1", &hp);
    fg = &home;
    front = NULL;
    released(hp);
    lock_wait();
    assert(locked && lock_shown && fg == &locker && locker.pid > 0 && home.frozen);
    puts("PASS from home: home released and frozen, the lock on the screen when lock_wait returns (the lid)");
    from(&home, hp, (const char *[]){ "open", "/usr/bin/x", NULL }, 0);
    assert(strcmp(last_told(hp, "ended"), "ended /usr/bin/x -1") == 0 && tasks[0].depth == 0 && fg == &locker);
    from(&home, hp, (const char *[]){ "app", "start", "qq", NULL }, 0);
    assert(strcmp(last_told(hp, "app-result"), "app-result error qq the screen is locked") == 0 && fg == &locker);
    puts("PASS while locked, open and app (sent before home was frozen) are refused");
    lock_ends(0);
    assert(!locked && fg == &home && !home.frozen && front == NULL);
    assert(strcmp(last_told(hp, "take"), "take") == 0);
    puts("PASS the lock exiting 0 unlocks: home thawed and given the screen");

    /* The lock killed (SIGTERM here; a crash or the OOM killer alike) and
     * reaped by process 1 (SIGCHLD) before it sees the socket close, as its
     * main loop may: still locked, the lock started again. reap() kept no
     * status for the lock, so gone() found none and read 0: unlocked. */
    released(hp);
    lock_wait();
    assert(locked && fg == &locker && locker.pid > 0);
    pid_t killed = locker.pid;
    events(locker.cgroup, "populated 0\n");
    assert(kill(killed, SIGTERM) == 0);
    for (double t0 = now_ms(); kill(killed, 0) == 0; usleep(1000)) { /* until no longer a zombie */
        assert(now_ms() - t0 < 5000);
        reap();
    }
    while (locker.pid > 0) {
        struct pollfd w = { .fd = locker.ctl, .events = POLLIN };
        assert(poll(&w, 1, 5000) == 1);
        message(&locker);
    }
    assert(locked && fg == NULL && lock_start_at > now_ms());
    lock_start(); /* the main loop, at lock_start_at */
    assert(locked && locker.pid > 0 && locker.pid != killed && fg == &locker);
    lock_ends(0);
    assert(!locked && fg == &home && !home.frozen);
    puts("PASS the lock killed and reaped before its socket closes: still locked, started again");

    frame(&tasks[0].f[0], "term", &tp);
    tasks[0].depth = 1;
    snprintf(tasks[0].key, sizeof tasks[0].key, "term");
    home.frozen = 1;
    front = &tasks[0];
    fg = &tasks[0].f[0];
    from(fg, tp, (const char *[]){ "lock", NULL }, 1);
    assert(locked && fg == &locker && tasks[0].f[0].frozen);
    from(&tasks[0].f[0], tp, (const char *[]){ "call", "/usr/bin/y", NULL }, 0);
    assert(strcmp(last_told(tp, "returned"), "returned /usr/bin/y -1") == 0 && tasks[0].depth == 1);
    pid_t first = locker.pid;
    lock_ends(1);
    assert(locked && locker.pid == 0 && fg == NULL && lock_start_at > now_ms());
    lock_start(); /* the main loop, at lock_start_at */
    assert(locked && locker.pid > 0 && locker.pid != first && fg == &locker);
    lock_ends(0);
    assert(!locked && front == &tasks[0] && fg == &tasks[0].f[0] && !tasks[0].f[0].frozen && home.frozen);
    puts("PASS a task asks (key_lock); the lock ending otherwise starts it again; unlocked, back to the task");

    events(tasks[0].f[0].cgroup, "populated 0\n");
    released(tp);
    lock();
    tasks[0].f[0].reaped = 1;
    tasks[0].f[0].status = 0;
    gone(&tasks[0].f[0]); /* it ended while locked */
    assert(tasks[0].depth == 0 && front == NULL && fg == &locker && home.frozen);
    assert(strcmp(last_told(hp, "ended"), "ended term 0") == 0);
    lock_ends(0);
    assert(!locked && fg == &home && !home.frozen);
    close(tp);
    puts("PASS a task ending while locked leaves the lock on the screen; unlocked, home");

    frame(&tasks[2].f[0], "display", &dp);
    tasks[2].depth = 1;
    tasks[2].f[0].shared_display = 1;
    snprintf(tasks[2].key, sizeof tasks[2].key, "display");
    released(hp);
    switch_to(&tasks[2].f[0]);
    front = &tasks[2];
    from(fg, dp, (const char *[]){ "lock", NULL }, 1); /* seat, on key_lock in the shared display */
    assert(locked && fg == &locker && !tasks[2].f[0].frozen && home.frozen);
    from(&tasks[2].f[0], dp, (const char *[]){ "switch", "next", NULL }, 0);
    assert(locked && fg == &locker && front == &tasks[2]);
    lock_ends(0);
    assert(!locked && fg == &tasks[2].f[0] && front == &tasks[2] && !tasks[2].f[0].frozen);
    puts("PASS the shared display asks (seat): released, not frozen; back to it after");
    assert(now_ms() - active_at < 1000); /* unlocking was input */
    lock_after = 2;
    assert(idle_lock_at() < 0); /* the shared display has the screen: labwc knows */
    fg = &home;
    active_at = now_ms() - 2500;
    assert(idle_lock_at() >= 0 && idle_lock_at() <= now_ms());
    lock_after = 0;
    assert(idle_lock_at() < 0);
    lock_after = 2;
    locked = 1;
    assert(idle_lock_at() < 0);
    locked = 0;
    puts("PASS idleness: lock_after s after the last input with a direct program on the screen; "
         "not on the shared display, nor while locked, nor with 0; unlocking counts as input");
    close(hp);
    close(dp);
    puts("ALL PASSED");
    return 0;
}
