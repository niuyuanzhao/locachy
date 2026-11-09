/* Direct-screen hotplug handover, switching between home and tasks (a
 * swipe), and several screens each showing its own program (MON-03),
 * without DRM, physical inputs, or PID 1. Socketpairs carry the control
 * messages, the release acknowledgements answered at once; screen and input
 * operations are doubles recording their ordering and their leases. Frozen
 * frames write their cgroup files in a scratch directory only
 * (test-init-screen DIR). */
#define main init_main
#include "../init/init.c"
#undef main
#include <assert.h>
#include <sys/stat.h>

static int refreshed, lends, returns, releases, available = 1, send_fail, kbd_count, ndev;
static int restore_result; /* screen_take_back: master failure, restored, or stale scanout */
static char order[128], log_[8192], dir[200];
static int peer[256];
static struct screen_output outs[4];
static int nouts;
static uint32_t lessee_of[4], next_lessee = 40;

static void step(char c) { size_t n = strlen(order); if (n + 1 < sizeof order) { order[n] = c; order[n + 1] = 0; } }
void say(const char *fmt, ...) { (void)fmt; }
void apps_list(char *buf, size_t size) { if (size) buf[0] = 0; }

static const char *who(int fd)
{
    struct frame *all[2 + MAX_TASKS * MAX_DEPTH];
    int n = frames(all);
    for (int k = 0; k < n; k++) {
        if (all[k]->ctl == fd) return all[k]->name;
    }
    return "?";
}

static void note(int fd, const char *text)
{
    size_t n = strlen(log_);
    snprintf(log_ + n, sizeof log_ - n, "%s:%s\n", who(fd), text);
}

int screen_refresh(void) { refreshed++; step('R'); return available; }
int screen_places(struct screen_output *out, int max)
{
    if (available <= 0) return 0;
    int n = nouts < max ? nouts : max;
    memcpy(out, outs, sizeof *out * (size_t)n);
    return n;
}
/* A lease of the outputs named (all of them for NULL) not lent already. */
static int lease(const char *const *names, int n, uint32_t *lessee)
{
    uint32_t id = next_lessee++;
    int count = 0;
    for (int i = 0; i < nouts; i++) {
        int wanted = names == NULL;
        for (int k = 0; k < n; k++) wanted |= strcmp(names[k], outs[i].name) == 0;
        if (!wanted || lessee_of[i]) continue;
        lessee_of[i] = id;
        count++;
    }
    if (count == 0 && !(names == NULL && nouts == 0)) return -1;
    lends++;
    step('L');
    *lessee = id;
    return open("/dev/null", O_RDONLY);
}
int screen_lend_outputs(const char *const *names, int n, uint32_t *lessee)
{
    *lessee = 0;
    return available <= 0 ? -1 : lease(names, n, lessee);
}
int screen_lend(uint32_t *lessee)
{
    *lessee = 0;
    return available <= 0 ? -1 : lease(NULL, 0, lessee);
}
int screen_take_back(uint32_t lessee)
{
    returns++;
    step('B');
    for (int i = 0; i < 4; i++) {
        if (lessee_of[i] == lessee) lessee_of[i] = 0;
    }
    return restore_result;
}
int screen_drop_master(uint32_t lessee) { (void)lessee; return 0; }
int keys_lend(int *fds, int max)
{
    int n = 0;
    for (; n < kbd_count && n < max; n++) fds[n] = open("/dev/null", O_RDONLY);
    return n;
}
void keys_take_back(const int *fds, int n)
{
    for (int i = 0; i < n; i++) close(fds[i]);
    step('K');
}
void keys_leds(int on) { (void)on; }
int keys_locks(void) { return 0; }
void keys_set_locks(int locks) { (void)locks; }
/* ndev pointer devices: files of their own in the scratch directory, as
 * distinct inodes as event nodes are. */
int *pointer_lend(size_t *count, char paths[][POINTER_PATH], size_t max)
{
    int *fds = calloc((size_t)ndev + 1, sizeof *fds);
    *count = 0;
    for (int i = 0; i < ndev && (size_t)i < max; i++) {
        char path[300];
        snprintf(path, sizeof path, "%s/event%d", dir, i);
        fds[i] = open(path, O_RDONLY | O_CREAT, 0600);
        snprintf(paths[i], POINTER_PATH, "/dev/input/event%d", i);
        (*count)++;
    }
    return fds;
}
void pointer_take_back(int *fds, size_t n)
{
    for (size_t i = 0; i < n; i++) close(fds[i]);
    free(fds);
    step('P');
}
int control_send_fields(int fd, const char *const *fields)
{
    char text[512] = "";
    for (int i = 0; fields[i]; i++) {
        size_t n = strlen(text);
        snprintf(text + n, sizeof text - n, "%s%s", i ? " " : "", fields[i]);
    }
    note(fd, text);
    return 0;
}
int control_send(int fd, const char *msg, size_t size, const int *fds, int n)
{
    (void)size;
    if (strcmp(msg, "release") == 0) {
        releases++;
        step('r');
        assert(send(peer[fd], "released", 8, 0) == 8);
        return 0;
    }
    if (strcmp(msg, "take") == 0) {
        assert(n >= 1 && fds[0] >= 0);
        step('T');
        if (send_fail) return -1;
    }
    char text[64];
    snprintf(text, sizeof text, "%s %d", msg, n);
    note(fd, text);
    return 0;
}
int control_recv(int fd, char *msg, size_t size, int *fds, int *nfds)
{ (void)fds; *nfds = 0; ssize_t n = recv(fd, msg, size - 1, 0); if (n >= 0) msg[n] = 0; return (int)n; }
/* As base/control.c splits a message into its NUL-separated fields. */
int control_fields(char *msg, int len, char **fields, int max)
{
    int n = 0;
    for (int i = 0; i < len && n < max; i += (int)strlen(msg + i) + 1) fields[n++] = msg + i;
    return n;
}
/* Containers: none here (message's app requests are not sent). */
int apps_open_mode(const char *name, const char *target, int own_tor, char *why, size_t size)
{ (void)name; (void)target; (void)own_tor; if (size) why[0] = 0; return -1; }
int apps_open(const char *name, const char *target, char *why, size_t size)
{ (void)name; (void)target; snprintf(why, size, "none"); return -1; }
int apps_action(const char *action, const char *name, char *why, size_t size)
{ (void)action; (void)name; snprintf(why, size, "none"); return -1; }
int apps_window_open(const char *program, const char *target, char *why, size_t size)
{ (void)program; (void)target; snprintf(why, size, "none"); return -1; }
void apps_display_ready(void) {}
void apps_tick(void) {}

/* A frame with its own control socket and a scratch cgroup whose freezing
 * is done at once. */
static void frame(struct frame *f, const char *name)
{
    static int made;
    int pair[2];
    char path[256];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0 && pair[0] < 256);
    memset(f, 0, sizeof *f);
    f->ctl = pair[0];
    peer[pair[0]] = pair[1];
    f->pid = 1000;
    snprintf(f->name, sizeof f->name, "%s", name);
    assert(snprintf(f->cgroup, sizeof f->cgroup, "%s/c%d", dir, made++) < (int)sizeof f->cgroup);
    assert(mkdir(f->cgroup, 0700) == 0);
    snprintf(path, sizeof path, "%s/cgroup.kill", f->cgroup);
    FILE *kill_file = fopen(path, "w");
    assert(kill_file && fputc('0', kill_file) == '0' && fclose(kill_file) == 0);
    snprintf(path, sizeof path, "%s/cgroup.events", f->cgroup);
    FILE *events = fopen(path, "w");
    assert(events && fputs("populated 1\nfrozen 1\n", events) >= 0 && fclose(events) == 0);
}

/* A running task in slot i, frozen as one waiting. */
static struct frame *task(int i, const char *key)
{
    frame(&tasks[i].f[0], key);
    tasks[i].depth = 1;
    tasks[i].f[0].frozen = 1;
    snprintf(tasks[i].key, sizeof tasks[i].key, "%s", key);
    return &tasks[i].f[0];
}

static void output(const char *name, int x, int y, int w, int h, int internal)
{
    struct screen_output *o = &outs[nouts++];
    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "%s", name);
    o->x = x; o->y = y; o->width = w; o->height = h; o->internal = internal;
}

/* Nothing running but home, with the screens and devices given. */
static void reset(int screens, int keyboards, int devices)
{
    struct frame *all[2 + MAX_TASKS * MAX_DEPTH];
    int n = frames(all);
    for (int k = 0; k < n; k++) {
        if (all[k]->pid) close(all[k]->ctl);
    }
    memset(tasks, 0, sizeof tasks);
    memset(&home, 0, sizeof home);
    memset(&locker, 0, sizeof locker);
    memset(places, 0, sizeof places);
    memset(lessee_of, 0, sizeof lessee_of);
    nplaces = focus_place = hover_place = 0;
    display_front = NULL;
    locked = 0;
    home_waits[0] = 0;
    stay_on_display = 0;
    nouts = 0;
    if (screens > 0) output("eDP-1", 0, 0, 1366, 768, 1);
    if (screens > 1) output("HDMI-A-1", 1366, 0, 1920, 1080, 0);
    if (screens > 2) output("DP-1", 3286, 0, 2560, 1440, 0);
    kbd_count = keyboards;
    ndev = devices;
    available = 1;
    send_fail = restore_result = 0;
    frame(&home, "home");
    arrange();
    order[0] = log_[0] = 0;
}

static void hotplug(void)
{
    reset(1, 0, 0);
    assert(home.screens == 1);
    display_pending = 1; display_refresh_at = 0;
    assert(refresh_display() == 1 && home.screens == 1 && !home.frozen);
    assert(strcmp(order, "rKPBRLT") == 0);
    assert(!display_pending && display_refresh_at < 0);
    assert(strstr(log_, "home:screens eDP-1 0 0 1366 768\n"));
    puts("PASS direct hotplug returns inputs/lease, rescans, relends the same unfrozen program");

    frame(&tasks[3].f[0], "display");
    tasks[3].depth = 1;
    tasks[3].f[0].shared_display = 1;
    tasks[3].f[0].screens = 1;
    display_front = &tasks[3];
    int before = refreshed;
    display_pending = 1; display_refresh_at = 0;
    assert(refresh_display() == 0 && refreshed == before);
    assert(display_pending && display_refresh_at < 0);
    tasks[3].f[0].screens = 0;
    display_front = NULL;
    tasks[3].depth = 0;
    puts("PASS compositor-owned DRM is left alone; topology remains pending");

    available = 0;
    display_refresh_at = 0;
    assert(refresh_display() == 1 && home.screens == 0 && !home.frozen && display_pending);
    available = 1;
    display_changed(); display_refresh_at = 0;
    assert(refresh_display() == 1 && home.screens == 1 && !display_pending);
    puts("PASS disconnect keeps the program, and reconnect retries even with the same topology IDs");

    take_back(&home);
    available = -1;
    display_changed(); display_refresh_at = 0;
    assert(refresh_display() == 1 && display_pending && display_refresh_at > now_ms());
    available = 1; display_refresh_at = 0;
    assert(refresh_display() == 1 && home.screens == 1 && !display_pending);
    puts("PASS temporary master ownership failure schedules a bounded retry");

    take_back(&home);
    send_fail = 1; kbd_count = 1;
    before = returns;
    assert(lend(&home, 1) == -1 && home.screens == 0 && home.lessee == 0 && home.nkbd == 0);
    assert(returns == before + 1 && lessee_of[0] == 0);
    send_fail = 0;
    puts("PASS failed take packet revokes the lease and input copies");
}

/* A compositor's returned master is not a successful restore of init's
 * stale scanout: hotplug happened while labwc owned the real DRM file. */
static void shared_hotplug(void)
{
    char path[256];
    reset(2, 1, 0);
    struct frame *display = task(2, "display");
    display->shared_display = 1;
    display_front = &tasks[2];
    arrange();
    assert(display->screens == 3 && home.screens == 0 && !display->frozen);
    int before = refreshed;
    display_changed();
    display_refresh_at = 0;
    assert(refresh_display() == 0 && refreshed == before);
    /* The external output disappears. Master comes back, but restoring the
     * old two-output scanout reports stale geometry rather than EBUSY. */
    nouts = 1;
    restore_result = 1;
    display_pending = 0;
    display_refresh_at = -1;
    take_back(display);
    assert(display_pending && display_refresh_at >= 0);
    go_home(-1);
    assert(display->pid == 1000 && tasks[2].depth == 1 && !display->frozen);
    snprintf(path, sizeof path, "%s/cgroup.kill", display->cgroup);
    FILE *f = fopen(path, "r");
    assert(f && fgetc(f) == '0');
    fclose(f);
    assert(home.screens == 1 && nplaces == 1 && home.nkbd == 1);
    assert(display_front == NULL && display->screens == 0);
    restore_result = 0;
    puts("PASS stale hotplug scanout after returned master rescans and keeps the compositor and its clients alive");

    reset(2, 1, 0);
    display = task(2, "display");
    display->shared_display = 1;
    display_front = &tasks[2];
    arrange();
    frame(&locker, "lock");
    locked = 1;
    nouts = 1;
    restore_result = 1;
    arrange();
    assert(locked && locker.screens == 1 && locker.nkbd == 1 && nplaces == 1);
    assert(display->screens == 0 && display->nkbd == 0 && display->pid == 1000);
    snprintf(path, sizeof path, "%s/cgroup.kill", display->cgroup);
    f = fopen(path, "r");
    assert(f && fgetc(f) == '0');
    fclose(f);
    restore_result = 0;
    puts("PASS a lock after hotplug gets the rescanned screen and keyboard without terminating the released compositor");

    reset(1, 1, 0);
    display = task(2, "display");
    display->shared_display = 1;
    display_front = &tasks[2];
    arrange();
    nouts = 0;
    available = 0;
    restore_result = 1;
    go_home(-1);
    assert(display->pid == 1000 && tasks[2].depth == 1 && display->screens == 0);
    assert(home.pid == 1000 && home.screens == 0 && display_pending);
    snprintf(path, sizeof path, "%s/cgroup.kill", display->cgroup);
    f = fopen(path, "r");
    assert(f && fgetc(f) == '0');
    fclose(f);
    available = 1;
    restore_result = 0;
    output("HDMI-A-1", 0, 0, 1920, 1080, 0);
    display_changed();
    display_refresh_at = 0;
    assert(refresh_display() == 1 && home.screens == 1 && home.nkbd == 1);
    assert(display->pid == 1000 && tasks[2].depth == 1);
    puts("PASS losing every output preserves the shared display until reconnection restores home input and scanout");

    /* Ownership failure is different: background DRM must never remain
     * accessible when home or the lock is being given the screen. */
    reset(1, 0, 0);
    display = task(2, "display");
    display->shared_display = 1;
    display_front = &tasks[2];
    arrange();
    restore_result = -1;
    go_home(-1);
    snprintf(path, sizeof path, "%s/cgroup.kill", display->cgroup);
    assert(access(path, F_OK) == 0);
    f = fopen(path, "r");
    assert(f && fgetc(f) == '1');
    fclose(f);
    restore_result = 0;
    puts("PASS failure to reclaim DRM ownership still ends the compositor before another program receives the screen");
}

/* Swipes on one screen: home, a task in slot 0, a shared display in slot 2. */
static void switching(void)
{
    reset(1, 0, 0);
    struct frame *term = task(0, "term"), *display = task(2, "display");
    display->shared_display = 1;
    do_switch(0, 1);
    assert(places[0].task == &tasks[0] && term->screens == 1 && home.frozen && !term->frozen);
    do_switch(0, 1);
    assert(display_front == &tasks[2] && display->screens == 1 && term->frozen && stay_on_display);
    assert(!strstr(log_, "display:screens"));
    stay_on_display = 0;
    do_switch(0, 1);
    assert(display_front == NULL && places[0].task == NULL && home.screens == 1 && !home.frozen);
    assert(display->screens == 0 && !display->frozen); /* the shared display is only released */
    do_switch(0, -1);
    assert(display_front == &tasks[2] && display->screens == 1);
    do_switch(0, 0);
    assert(display_front == &tasks[2]);
    go_home(0);
    assert(display_front == NULL && home.screens == 1);
    tasks[0].depth = tasks[2].depth = 0;
    order[0] = 0;
    do_switch(0, 1); /* nothing running: home stays */
    assert(places[0].task == NULL && home.screens == 1 && !order[0]);
    puts("PASS swipes go round home and the running tasks; the one left is frozen");
}

/* Two screens: the built-in panel at the origin, an external one right of
 * it; home on both, then a task opened from home on the external one. */
static void several(void)
{
    reset(2, 1, 2);
    assert(nplaces == 2 && home.screens == 3 && lessee_of[0] == lessee_of[1] && lessee_of[0]);
    assert(home.nkbd == 1 && focus_place == 0 && hover_place == 0);
    struct frame *term = task(0, "term");
    char *argv[] = { "term", NULL };
    assert(do_open(place_named("HDMI-A-1"), argv) == 0);
    home_wait(argv);
    assert(places[1].task == &tasks[0] && places[0].task == NULL);
    assert(home.screens == 1 && term->screens == 2 && !home.frozen && !term->frozen);
    assert(lessee_of[0] && lessee_of[1] && lessee_of[0] != lessee_of[1]);
    assert(strstr(log_, "home:shown term\n") && !home_waits[0]);
    assert(strstr(log_, "term:screens eDP-1 0 0 1366 768 HDMI-A-1 1366 0 1920 1080\n"));
    assert(focus_place == 1 && term->nkbd == 1 && home.nkbd == 0);
    puts("PASS a task opened on one screen runs there beside home, which keeps the other and is told shown");

    home.pointer_wanted = term->pointer_wanted = 1; /* both asked: pointer? */
    log_[0] = 0;
    pointer_place();
    assert(home.npointer == 2 && term->npointer == 0 && hover_place == 0);
    pointer_crossed(&home, 1500, 100);
    assert(hover_place == 1 && term->npointer == 2 && home.npointer == 0);
    assert(strstr(log_, "home:pointer-left 0\n") && strstr(log_, "term:pointer-enter 1500 100\n"));
    log_[0] = 0;
    pointer_crossed(&term[0], 5000, 100); /* off every screen: it stays */
    assert(hover_place == 1 && term->npointer == 2 && strstr(log_, "term:pointer-enter 5000 100\n"));
    pointer_crossed(term, 100, 100);
    assert(hover_place == 0 && home.npointer == 2 && term->npointer == 0);
    puts("PASS the pointer's devices go to the program whose screen it moves onto, which is told where");

    log_[0] = 0;
    pressed(term, -1); /* no pointer devices of its own now: still its screen's press */
    assert(focus_place == 1 && term->nkbd == 1);
    pressed(&home, -1);
    assert(focus_place == 0 && home.nkbd == 1 && term->nkbd == 0 && strstr(log_, "term:keys 0\n"));
    pressed(&home, -1);
    assert(focus_place == 0 && home.nkbd == 1);
    puts("PASS the keyboard follows the screen last pressed on");

    go_home(1);
    assert(places[1].task == NULL && home.screens == 3 && term->frozen && term->screens == 0);
    assert(lessee_of[0] == lessee_of[1] && !strstr(log_, "home:back"));
    puts("PASS that screen going home freezes its task; home has both screens again");

    assert(do_open(1, argv) == 0);
    home_wait(argv);
    frame(&locker, "lock");
    locked = 1;
    arrange();
    assert(locker.screens == 3 && home.frozen && term->frozen && home.screens == 0 && term->screens == 0);
    assert(focused() == &locker && locker.nkbd == 1);
    reclaim(&locker); /* it exited 0 */
    close(locker.ctl);
    locker.ctl = -1;
    locker.pid = 0;
    unlock();
    assert(home.screens == 1 && term->screens == 2 && !home.frozen && !term->frozen);
    puts("PASS the lock has every screen; unlocking gives each screen back what it showed");

    nouts = 1; /* the external screen goes, its task on it */
    display_changed(); display_refresh_at = 0;
    assert(refresh_display() == 1 && nplaces == 1 && places[0].task == &tasks[0]);
    assert(term->screens == 1 && home.frozen && home.screens == 0 && focus_place == 0);
    nouts = 2;
    display_changed(); display_refresh_at = 0;
    assert(refresh_display() == 1 && nplaces == 2 && places[0].task == &tasks[0] && places[1].task == NULL);
    assert(term->screens == 1 && home.screens == 2 && !home.frozen);
    puts("PASS a task on a screen that goes moves to one that showed home; home takes the screen that comes");
}

/* One screen: home opens a task over it and waits, told back or ended. */
static void waiting(void)
{
    reset(1, 0, 0);
    struct frame *term = task(0, "term");
    char *argv[] = { "term", NULL };
    assert(do_open(-1, argv) == 0);
    home_wait(argv);
    assert(places[0].task == &tasks[0] && home.frozen && home.screens == 0 && strcmp(home_waits, "term") == 0);
    assert(!strstr(log_, "home:shown"));
    go_home(0);
    assert(strstr(log_, "home:back term\n") && home.screens == 1 && term->frozen && !home_waits[0]);
    assert(do_open(-1, argv) == 0);
    home_wait(argv);
    log_[0] = 0;
    gone(term); /* not a child of this test: its status is unknown, 255 */
    assert(tasks[0].depth == 0 && places[0].task == NULL && home.screens == 1 && !home.frozen);
    assert(strstr(log_, "home:ended term 255\n") && !home_waits[0]);
    puts("PASS home, left without a screen, waits for the task it opened: back, ended");
}

static void three(void)
{
    reset(3, 1, 2);
    assert(nplaces == 3 && home.screens == 7);
    const char *names[] = { "eDP-1", "HDMI-A-1", "DP-1" };
    const char *programs[] = { "packages", "settings", "term" };
    struct frame *opened[3];
    for (int i = 0; i < 3; i++) {
        opened[i] = task(i, programs[i]);
        char *argv[] = { (char *)programs[i], NULL };
        assert(do_open(place_named(names[i]), argv) == 0);
        home_wait(argv);
        assert(opened[i]->screens == (1u << i) && !opened[i]->frozen);
        assert(places[i].task == &tasks[i] && focus_place == i && opened[i]->nkbd == 1);
        for (int k = 0; k < i; k++) {
            assert(opened[k]->screens == (1u << k) && !opened[k]->frozen && opened[k]->nkbd == 0);
            assert(lessee_of[k] && lessee_of[k] != lessee_of[i]);
        }
    }
    assert(home.screens == 0 && home.frozen);
    for (int i = 0; i < 3; i++) {
        go_home(i);
        assert(home.screens == (1u << (i + 1)) - 1 && opened[i]->frozen);
    }
    assert(home.screens == 7 && !home.frozen);
    puts("PASS three connectors independently launch tasks with leases 1/2/4 and return each screen to home");

    /* A direct program is one task: opening it on another output moves its
     * existing top, whether it was frozen or still shown somewhere else. */
    char *packages[] = { "packages", NULL };
    assert(do_open(place_named("HDMI-A-1"), packages) == 0);
    assert(places[1].task == &tasks[0] && opened[0]->screens == 2 && !opened[0]->frozen);
    assert(tasks[0].depth == 1 && strcmp(tasks[0].key, "packages") == 0);
    assert(do_open(place_named("DP-1"), packages) == 0);
    assert(places[1].task == NULL && places[2].task == &tasks[0] && opened[0]->screens == 4);
    assert(home.screens == 3 && focus_place == 2 && opened[0]->nkbd == 1);
    go_home(2);
    assert(opened[0]->frozen && home.screens == 7);
    assert(do_open(place_named("eDP-1"), packages) == 0);
    assert(places[0].task == &tasks[0] && opened[0]->screens == 1 && !opened[0]->frozen);
    assert(home.screens == 6 && focus_place == 0 && tasks[0].depth == 1);
    go_home(0);
    puts("PASS one existing packages task moves across all three outputs, frozen or already shown, without a new instance");
}

/* f tells process 1 the fields (NULL ended), which it reads at once
 * (message): home's open (base_open, as its file manager opens a document
 * in view), a press (pointer.c), a program's request for home. */
static void tell(struct frame *f, const char *const *fields)
{
    char msg[512];
    size_t len = 0;
    for (; *fields; fields++) {
        size_t n = strlen(*fields) + 1;
        assert(len + n <= sizeof msg);
        memcpy(msg + len, *fields, n);
        len += n;
    }
    assert(len > 0 && send(peer[f->ctl], msg, len - 1, 0) == (ssize_t)(len - 1));
    message(f);
}

/* Three screens, home on all of them. What home opens naming no screen
 * ("open": what its file manager opens in view, also for a program that
 * asked home to open something) goes on one screen, as a desktop opens a
 * window on one monitor: the one pressed on or asked from, not every screen
 * home shows. */
static void one_screen(void)
{
    reset(3, 1, 2);
    struct frame *a = task(0, "/usr/local/bin/view /home/u/a.pdf");
    struct frame *b = task(1, "/usr/local/bin/view /home/u/b.pdf");
    tell(&home,(const char *const[]){ "pointer-press", "DP-1", NULL }); /* on home's third screen */
    assert(focus_place == 2 && home.nkbd == 1);
    tell(&home, (const char *const[]){ "open", "/usr/local/bin/view", "/home/u/a.pdf", NULL });
    assert(places[2].task == &tasks[0] && places[0].task == NULL && places[1].task == NULL);
    assert(a->screens == 4 && !a->frozen && home.screens == 3 && !home.frozen);
    assert(strstr(log_, "home:shown /usr/local/bin/view /home/u/a.pdf\n") && !home_waits[0]);
    assert(focus_place == 2 && a->nkbd == 1 && home.nkbd == 0);
    puts("PASS home's open naming no screen: view on the one of home's three screens pressed on; home keeps two");

    /* view follows a link to another document (open_in_home): its screen
     * goes home, and home opens the document there. */
    tell(a, (const char *const[]){ "home", NULL });
    assert(places[2].task == NULL && a->frozen && home.screens == 7 && focus_place == 2);
    tell(&home, (const char *const[]){ "open", "/usr/local/bin/view", "/home/u/b.pdf", NULL });
    assert(places[2].task == &tasks[1] && b->screens == 4 && home.screens == 3 && a->frozen);
    puts("PASS what a program asks home to open goes on that program's screen, not on every screen");

    /* The pointer goes from view onto home's HDMI-A-1 and presses there. */
    home.pointer_wanted = b->pointer_wanted = 1; /* both asked since their take: pointer? */
    pointer_crossed(b, 1500, 100);
    assert(hover_place == 1);
    tell(&home, (const char *const[]){ "pointer-press", "HDMI-A-1", NULL });
    assert(focus_place == 1 && home.nkbd == 1 && b->nkbd == 0);
    tell(&home, (const char *const[]){ "open", "/usr/local/bin/view", "/home/u/a.pdf", NULL });
    assert(places[1].task == &tasks[0] && places[2].task == &tasks[1] && a->screens == 2 && !a->frozen);
    assert(home.screens == 1 && !home.frozen && focus_place == 1 && a->nkbd == 1);
    puts("PASS presses on home's screens move where its next open goes; each view keeps its own screen");

    /* The focused screen shows a program (home asked after the shared
     * display, which every screen showed): the built-in panel if home
     * shows it, else the first screen home shows. */
    reset(3, 1, 2);
    task(0, "term");
    task(1, "settings");
    task(2, "packages");
    assert(do_open(place_named("DP-1"), (char *[]){ "term", NULL }) == 0 && focus_place == 2);
    tell(&home, (const char *const[]){ "open", "settings", NULL });
    assert(places[0].task == &tasks[1] && places[1].task == NULL && places[2].task == &tasks[0]);
    assert(home.screens == 2 && focus_place == 0);
    tell(&home, (const char *const[]){ "open", "packages", NULL });
    assert(places[1].task == &tasks[2] && places[0].task == &tasks[1] && places[2].task == &tasks[0]);
    assert(home.screens == 0 && home.frozen && strcmp(home_waits, "packages") == 0);
    puts("PASS a focused screen home does not show: the built-in panel, else the first home shows; never all");
}

int main(int argc, char **argv)
{
    assert(argc == 2 && mkdir(argv[1], 0700) == 0);
    snprintf(dir, sizeof dir, "%s", argv[1]); /* short and relative, as the cgroup paths are */
    hotplug();
    shared_hotplug();
    switching();
    several();
    three();
    one_screen();
    waiting();
    puts("ALL PASSED");
    return 0;
}
