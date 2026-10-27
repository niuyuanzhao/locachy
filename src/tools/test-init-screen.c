/* Direct-screen hotplug handover, and switching between home and tasks (a
 * swipe), without DRM, physical inputs, or PID 1. Socketpairs provide the
 * release acknowledgements; screen/input operations are doubles recording
 * their ordering. Frozen frames write their cgroup files in a scratch
 * directory only (test-init-screen DIR). */
#define main init_main
#include "../init/init.c"
#undef main
#include <assert.h>
#include <sys/stat.h>

static int refreshed, lends, returns, releases, revoked, available = 1, send_fail;
static char order[128];
static void step(char c) { size_t n = strlen(order); order[n] = c; order[n + 1] = 0; }
void say(const char *fmt, ...) { (void)fmt; }
void apps_list(char *buf, size_t size) { if (size) buf[0] = 0; }
int screen_refresh(void) { refreshed++; step('R'); return available; }
int screen_lend(uint32_t *lessee) { lends++; step('L'); *lessee = 42; return open("/dev/null", O_RDONLY); }
int screen_take_back(uint32_t lessee) { assert(lessee == 42); returns++; step('B'); return 0; }
int keys_lend(int *fds, int max) { (void)fds; (void)max; return 0; }
void keys_take_back(const int *fds, int n) { (void)fds; assert(n == 0); revoked++; step('K'); }
void pointer_take_back(int *fds, size_t n) { assert(!fds && n == 0); step('P'); }
static char told[256];
int control_send_fields(int fd, const char *const *fields)
{
    (void)fd;
    snprintf(told, sizeof told, "%s %s", fields[0], fields[1] ? fields[1] : "");
    return 0;
}
int control_send(int fd, const char *msg, size_t size, const int *fds, int n)
{
    (void)fd; (void)size;
    if (strcmp(msg, "release") == 0) { releases++; step('r'); return 0; }
    assert(strcmp(msg, "take") == 0 && n == 1 && fds[0] >= 0);
    step('T');
    return send_fail ? -1 : 0;
}
int control_recv(int fd, char *msg, size_t size, int *fds, int *nfds)
{ (void)fds; *nfds = 0; ssize_t n = recv(fd, msg, size - 1, 0); if (n >= 0) msg[n] = 0; return (int)n; }

/* A frame with its own control socket (peer in *peer) and a scratch cgroup
 * whose freezing is done at once. */
static void frame(struct frame *f, const char *name, const char *dir, int *peer)
{
    int pair[2];
    char path[256];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    memset(f, 0, sizeof *f);
    f->ctl = pair[0];
    *peer = pair[1];
    f->pid = 1000;
    snprintf(f->name, sizeof f->name, "%s", name);
    snprintf(f->cgroup, sizeof f->cgroup, "%s/%s", dir, name);
    assert(mkdir(f->cgroup, 0700) == 0);
    snprintf(path, sizeof path, "%s/cgroup.events", f->cgroup);
    FILE *events = fopen(path, "w");
    assert(events && fputs("populated 1\nfrozen 1\n", events) >= 0 && fclose(events) == 0);
}

/* The swipe test: home, a task in slot 0, a shared display in slot 2. */
static void switching(const char *dir)
{
    int hp, tp, dp;
    frame(&home, "home-s", dir, &hp);
    frame(&tasks[0].f[0], "term", dir, &tp);
    frame(&tasks[2].f[0], "display", dir, &dp);
    tasks[0].depth = tasks[2].depth = 1;
    snprintf(tasks[0].key, sizeof tasks[0].key, "term");
    snprintf(tasks[2].key, sizeof tasks[2].key, "display");
    tasks[2].f[0].shared_display = 1;
    tasks[0].f[0].frozen = tasks[2].f[0].frozen = 1;
    home.lessee = 42; /* home has the screen */
    send_fail = 0;
    front = NULL;
    fg = &home;
    assert(send(hp, "released", 8, 0) == 8);
    do_switch(1);
    assert(front == &tasks[0] && fg == &tasks[0].f[0] && home.frozen && !tasks[0].f[0].frozen);
    assert(send(tp, "released", 8, 0) == 8);
    do_switch(1);
    assert(front == &tasks[2] && fg == &tasks[2].f[0] && tasks[0].f[0].frozen && stay_on_display);
    stay_on_display = 0;
    tasks[2].f[0].shared_display = 0; /* back from it as from a direct task */
    assert(send(dp, "released", 8, 0) == 8);
    told[0] = 0;
    do_switch(1);
    assert(front == NULL && fg == &home && !home.frozen && strcmp(told, "back display") == 0);
    assert(send(hp, "released", 8, 0) == 8);
    do_switch(-1);
    assert(front == &tasks[2] && fg == &tasks[2].f[0]);
    do_switch(0);
    assert(front == &tasks[2]);
    tasks[0].depth = tasks[2].depth = 0;
    assert(send(dp, "released", 8, 0) == 8);
    go_home();
    told[0] = 0;
    do_switch(1); /* nothing running: home stays */
    assert(front == NULL && fg == &home && !told[0]);
    close(hp); close(tp); close(dp);
    puts("PASS swipes go round home and the running tasks; the one left is frozen, home told when back");
}

int main(int argc, char **argv)
{
    int pair[2];
    assert(argc == 2 && mkdir(argv[1], 0700) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    home.pid = 123;
    home.ctl = pair[0];
    home.lessee = 42;
    snprintf(home.name, sizeof home.name, "home");
    fg = &home;
    display_pending = 1; display_refresh_at = 0;
    assert(send(pair[1], "released", 8, 0) == 8);
    assert(refresh_display() == 1 && fg == &home && !home.frozen);
    assert(strcmp(order, "rKPBRLT") == 0 && refreshed == 1 && lends == 1);
    assert(!display_pending && display_refresh_at < 0);
    puts("PASS direct hotplug returns inputs/lease, rescans, relends the same unfrozen task");
    struct frame compositor = { .shared_display = 1, .pid = 234 };
    fg = &compositor;
    display_pending = 1; display_refresh_at = 0;
    assert(refresh_display() == 0 && fg == &compositor && refreshed == 1);
    assert(display_pending && display_refresh_at < 0);
    puts("PASS compositor-owned DRM is left alone; topology remains pending");
    fg = &home; home.lessee = 42; available = 0;
    display_refresh_at = 0;
    assert(send(pair[1], "released", 8, 0) == 8);
    assert(refresh_display() == 1 && fg == NULL && !home.frozen && display_pending);
    available = 1;
    display_changed(); display_refresh_at = 0;
    assert(refresh_display() == 1 && fg == &home && !display_pending);
    puts("PASS disconnect keeps the task, and reconnect retries even with the same topology IDs");
    fg = NULL; available = -1;
    display_changed(); display_refresh_at = 0;
    assert(refresh_display() == 1 && display_pending && display_refresh_at > now_ms());
    available = 1; display_refresh_at = 0;
    assert(refresh_display() == 1 && fg == &home && !display_pending);
    puts("PASS temporary master ownership failure schedules a bounded retry");
    fg = NULL; home.lessee = 0; send_fail = 1;
    int before = returns;
    assert(lend(&home) == -1 && fg == NULL && home.lessee == 0 && home.nkbd == 0);
    assert(returns == before + 1);
    puts("PASS failed take packet revokes the lease and input copies");
    assert(releases == 2 && revoked >= 2);
    close(pair[0]); close(pair[1]);
    switching(argv[1]);
    puts("ALL PASSED");
    return 0;
}
