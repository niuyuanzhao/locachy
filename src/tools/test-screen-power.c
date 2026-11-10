/*
 * test-screen-power: base_screen_power on screen.c's direct backend, with the
 * DRM calls it makes (SCREEN_POWER_IOCTL) answered by a recording fake and no
 * device: the DPMS property found by name in the connector's property list,
 * off then on set on each connector, every failure returned and a half-done
 * change undone, on again when the screen is released and when the process
 * exits (not when a child forked later exits), and nothing asked without a
 * screen. Built by tools/test-screen-power.sh.
 */
#include <stdint.h>

#define SCREEN_POWER_IOCTL fake_ioctl
static int fake_ioctl(int fd, unsigned long request, void *arg);

#include "../base/screen.c"

#include <stdarg.h>
#include <sys/wait.h>

/* The fake card: two connectors whose property lists hold EDID, link-status
 * and (unless taken away) DPMS, in that order, under these ids. */
#define EDID_ID 12
#define LINK_ID 30
static uint32_t dpms_id = 47;
static struct fake_connector {
    uint32_t id;
    int has_dpms;
    uint64_t dpms;
} conns[2];
static int fake_fd = -1, fail_list, grow;
static uint32_t fail_set;       /* the connector whose SETPROPERTY is refused */
static int calls, other_calls;  /* every fake call; ones that are not property calls */
static struct { uint32_t obj, prop; uint64_t value; } set_log[16];
static int sets;
static int record_fd = -1;      /* the exit case's pipe: "pid value" per SETPROPERTY */
static char last_say[256];

void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_say, sizeof last_say, fmt, ap);
    va_end(ap);
}

static struct fake_connector *conn(uint32_t id)
{
    for (int i = 0; i < 2; i++) {
        if (conns[i].id == id) return &conns[i];
    }
    return NULL;
}

static int fake_ioctl(int fd, unsigned long request, void *arg)
{
    calls++;
    if (fd != fake_fd || fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (request == DRM_IOCTL_MODE_OBJ_GETPROPERTIES) {
        struct drm_mode_obj_get_properties *l = arg;
        struct fake_connector *c = conn(l->obj_id);
        if (fail_list) { errno = EIO; return -1; }
        if (!c || l->obj_type != DRM_MODE_OBJECT_CONNECTOR) { errno = ENOENT; return -1; }
        uint32_t ids[3] = { EDID_ID, LINK_ID, dpms_id };
        uint64_t values[3] = { 0, 0, c->dpms };
        uint32_t n = c->has_dpms ? 3 : 2;
        if (grow && l->props_ptr == 0) { n--; grow = 0; } /* one more appears before the fill */
        for (uint32_t i = 0; i < n && i < l->count_props; i++) {
            ((uint32_t *)(uintptr_t)l->props_ptr)[i] = ids[i];
            ((uint64_t *)(uintptr_t)l->prop_values_ptr)[i] = values[i];
        }
        l->count_props = n;
        return 0;
    }
    if (request == DRM_IOCTL_MODE_GETPROPERTY) {
        struct drm_mode_get_property *p = arg;
        const char *name = p->prop_id == dpms_id ? "DPMS" : p->prop_id == EDID_ID ? "EDID" :
                           p->prop_id == LINK_ID ? "link-status" : NULL;
        if (!name) { errno = ENOENT; return -1; }
        snprintf(p->name, sizeof p->name, "%s", name);
        p->flags = p->prop_id == EDID_ID ? DRM_MODE_PROP_BLOB : DRM_MODE_PROP_ENUM;
        return 0;
    }
    if (request == DRM_IOCTL_MODE_OBJ_SETPROPERTY) {
        struct drm_mode_obj_set_property *s = arg;
        if (sets < 16) set_log[sets] = (__typeof__(set_log[0])){ s->obj_id, s->prop_id, s->value };
        sets++;
        if (record_fd >= 0) dprintf(record_fd, "%d %llu\n", (int)getpid(), (unsigned long long)s->value);
        struct fake_connector *c = conn(s->obj_id);
        if (!c || s->obj_type != DRM_MODE_OBJECT_CONNECTOR || !c->has_dpms || s->prop_id != dpms_id ||
            s->value > DRM_MODE_DPMS_OFF) { errno = EINVAL; return -1; }
        if (s->obj_id == fail_set) { errno = EACCES; return -1; }
        c->dpms = s->value;
        return 0;
    }
    other_calls++;
    errno = ENOTTY;
    return -1;
}

static int failures, case_failures;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL line %d: %s\n", __LINE__, #expr); \
                                       failures++; case_failures++; } } while (0)

static void pass(const char *what)
{
    if (case_failures == 0) printf("PASS %s\n", what);
    else printf("FAIL %s\n", what);
    case_failures = 0;
    fflush(stdout);
}

/* A shown screen of count outputs on a fresh fake card, every connector on. */
static void backend(uint32_t count)
{
    conns[0] = (struct fake_connector){ 61, 1, DRM_MODE_DPMS_ON };
    conns[1] = (struct fake_connector){ 72, 1, DRM_MODE_DPMS_ON };
    fail_list = grow = 0;
    fail_set = 0;
    dpms_id = 47;
    calls = other_calls = sets = 0;
    memset(set_log, 0, sizeof set_log);
    last_say[0] = 0;
    fake_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    card = fake_fd;
    noutputs = count;
    outputs = count ? calloc(count, sizeof *outputs) : NULL;
    for (uint32_t i = 0; i < count; i++) {
        struct output *o = &outputs[i];
        snprintf(o->name, sizeof o->name, "Virtual-%u", i + 1);
        o->conn_id = conns[i].id;
        o->crtc_id = 90 + i;
        o->mode.hdisplay = 1024;
        o->mode.vdisplay = 768;
    }
}

static void no_backend(void)
{
    backend(0);
    close(card);
    card = -1;
    CHECK(base_screen_power(0) == -1 && base_screen_power(1) == -1);
    CHECK(calls == 0);
    backend(0); /* a card, no output */
    CHECK(base_screen_power(0) == -1 && calls == 0);
    screen_release();
    backend(1);
    window_mode = 1;
    CHECK(base_screen_power(0) == -1 && calls == 0);
    window_mode = 0;
    lent = 1; /* process 1 with its outputs lent: not showing them */
    CHECK(base_screen_power(0) == -1 && calls == 0);
    lent = 0;
    CHECK(conns[0].dpms == DRM_MODE_DPMS_ON && !dark);
    screen_release();
    CHECK(calls == 0 && sets == 0);
    pass("no active backend: -1, no DRM call, nothing changed");
}

static void property_missing(void)
{
    backend(1);
    conns[0].has_dpms = 0;
    CHECK(base_screen_power(0) == -1);
    CHECK(sets == 0 && other_calls == 0 && !dark);
    CHECK(strstr(last_say, "has no DPMS property") != NULL);
    screen_release();
    CHECK(sets == 0);
    pass("connector without a DPMS property: -1, nothing set");
}

static void ioctl_failure(void)
{
    backend(1);
    fail_set = 61;
    CHECK(base_screen_power(0) == -1);
    CHECK(sets == 1 && set_log[0].value == DRM_MODE_DPMS_OFF && conns[0].dpms == DRM_MODE_DPMS_ON);
    CHECK(!dark && strstr(last_say, "DPMS off refused") != NULL);
    CHECK(base_screen_power(1) == -1);
    screen_release();
    CHECK(sets == 2); /* nothing was turned off, nothing to give back */

    backend(1);
    fail_list = 1;
    CHECK(base_screen_power(0) == -1 && sets == 0 && !dark);
    CHECK(strstr(last_say, "cannot list its properties") != NULL);
    screen_release();
    pass("refused SETPROPERTY or unreadable property list: -1, never faked");
}

static void success(void)
{
    backend(1);
    struct output before = outputs[0];
    CHECK(base_screen_power(0) == 0);
    CHECK(sets == 1 && set_log[0].obj == 61 && set_log[0].prop == 47 &&
          set_log[0].value == DRM_MODE_DPMS_OFF && conns[0].dpms == DRM_MODE_DPMS_OFF && dark);
    CHECK(base_screen_power(1) == 0);
    CHECK(sets == 2 && set_log[1].obj == 61 && set_log[1].prop == 47 &&
          set_log[1].value == DRM_MODE_DPMS_ON && conns[0].dpms == DRM_MODE_DPMS_ON && !dark);
    CHECK(other_calls == 0); /* property calls only: no mode set, framebuffer or cursor */
    CHECK(before.conn_id == outputs[0].conn_id && before.crtc_id == outputs[0].crtc_id &&
          before.fb_id == outputs[0].fb_id && monitor_mode_same(&before.mode, &outputs[0].mode));
    CHECK(card == fake_fd && noutputs == 1);
    /* The id comes from the list, wherever the kernel numbers it. */
    dpms_id = 5;
    grow = 1;
    CHECK(base_screen_power(0) == 0 && sets == 3 && set_log[2].prop == 5 && conns[0].dpms == DRM_MODE_DPMS_OFF);
    CHECK(base_screen_power(1) == 0 && sets == 4 && set_log[3].prop == 5);
    screen_release();
    CHECK(sets == 4); /* on already: release sets nothing */
    pass("off then on: DPMS 3 then 0 on the connector, property id from the list");
}

static void two_outputs(void)
{
    backend(2);
    CHECK(base_screen_power(0) == 0 && sets == 2);
    CHECK(set_log[0].obj == 61 && set_log[1].obj == 72 && set_log[1].value == DRM_MODE_DPMS_OFF);
    CHECK(base_screen_power(1) == 0 && sets == 4 && conns[1].dpms == DRM_MODE_DPMS_ON);
    fail_set = 72;
    CHECK(base_screen_power(0) == -1);
    CHECK(sets == 7 && set_log[4].obj == 61 && set_log[4].value == DRM_MODE_DPMS_OFF);
    CHECK(set_log[5].obj == 72 && set_log[6].obj == 61 && set_log[6].value == DRM_MODE_DPMS_ON);
    CHECK(conns[0].dpms == DRM_MODE_DPMS_ON && conns[1].dpms == DRM_MODE_DPMS_ON && !dark);
    screen_release();
    CHECK(sets == 7);
    pass("every connector shown; one refused: -1 and the others turned on again");
}

static void release_restores(void)
{
    backend(1);
    int fd = fake_fd;
    CHECK(base_screen_power(0) == 0 && conns[0].dpms == DRM_MODE_DPMS_OFF);
    screen_release();
    CHECK(sets == 2 && set_log[1].obj == 61 && set_log[1].prop == 47 && set_log[1].value == DRM_MODE_DPMS_ON);
    CHECK(conns[0].dpms == DRM_MODE_DPMS_ON && !dark);
    CHECK(card == -1 && outputs == NULL && noutputs == 0);
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF); /* the lease closed after */
    pass("release: DPMS on before the lease is closed");
}

/* The exit hook in a child process; a grandchild forked while the screen is
 * off exits first and must not turn it on. */
static void exit_restores(void)
{
    int p[2];
    CHECK(pipe(p) == 0);
    fflush(NULL);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(p[0]);
        record_fd = p[1];
        backend(1);
        if (base_screen_power(0) != 0) _exit(3);
        pid_t grandchild = fork();
        if (grandchild == 0) exit(0);
        if (grandchild < 0 || waitpid(grandchild, NULL, 0) != grandchild) _exit(4);
        exit(0);
    }
    close(p[1]);
    char text[256] = "";
    size_t len = 0;
    ssize_t n;
    while (len < sizeof text - 1 && (n = read(p[0], text + len, sizeof text - 1 - len)) > 0) len += (size_t)n;
    close(p[0]);
    int status = -1;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char want[64];
    snprintf(want, sizeof want, "%d %d\n%d %d\n", (int)child, DRM_MODE_DPMS_OFF, (int)child, DRM_MODE_DPMS_ON);
    CHECK(strcmp(text, want) == 0);
    if (strcmp(text, want) != 0) printf("got: %s", text);
    pass("exit: DPMS on from the exit hook, not from a child forked later");
}

int main(void)
{
    no_backend();
    property_missing();
    ioctl_failure();
    success();
    two_outputs();
    release_restores();
    exit_restores();
    if (failures) {
        printf("FAILED %d checks\n", failures);
        return 1;
    }
    puts("ALL PASSED");
    return 0;
}
