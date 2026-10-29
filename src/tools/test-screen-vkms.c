/* Real KMS/lease/hotplug test. Run only as PID 1 in an isolated VM.
 * The runner supplies the matching vkms.ko, a font, and no physical GPU. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>

#include "../base/screen.c"

void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
    va_end(ap);
}
int monitor_conf(const char *name, struct monitor_conf *out)
{
    (void)name;
    memset(out, 0, sizeof *out);
    return 0; /* no MONITORS_CONF in the VM: scale 1, the runner's /font.psf */
}
int monitor_internal(const char *name) { (void)name; return 0; }
int control_fd(void) { return -1; }
void control_wait_take(void) { abort(); }

void base_language_reload(void) {}

static void die(const char *what)
{
    perror(what);
    fflush(NULL);
    reboot(RB_POWER_OFF);
    _exit(1);
}
#define CHECK(expr) do { if (!(expr)) { say("FAIL line %d: %s", __LINE__, #expr); die(#expr); } } while (0)

static void directory(const char *path)
{
    CHECK(mkdir(path, 0755) == 0 || errno == EEXIST);
}
static void put(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(write(fd, value, strlen(value)) == (ssize_t)strlen(value));
    CHECK(close(fd) == 0);
}
static void configure(void)
{
    directory("/config/vkms/mirror");
    for (int i = 0; i < 2; i++) {
        char p[256], target[256];
        snprintf(p, sizeof p, "/config/vkms/mirror/crtcs/c%d", i);
        directory(p);
        snprintf(p, sizeof p, "/config/vkms/mirror/planes/p%d", i);
        directory(p);
        snprintf(p, sizeof p, "/config/vkms/mirror/planes/p%d/type", i);
        put(p, "1");
        snprintf(target, sizeof target, "/config/vkms/mirror/crtcs/c%d", i);
        snprintf(p, sizeof p, "/config/vkms/mirror/planes/p%d/possible_crtcs/c%d", i, i);
        CHECK(symlink(target, p) == 0);
        snprintf(p, sizeof p, "/config/vkms/mirror/planes/k%d", i); /* a cursor plane each */
        directory(p);
        snprintf(p, sizeof p, "/config/vkms/mirror/planes/k%d/type", i);
        put(p, "2");
        snprintf(p, sizeof p, "/config/vkms/mirror/planes/k%d/possible_crtcs/c%d", i, i);
        CHECK(symlink(target, p) == 0);
        snprintf(p, sizeof p, "/config/vkms/mirror/encoders/e%d", i);
        directory(p);
        snprintf(p, sizeof p, "/config/vkms/mirror/encoders/e%d/possible_crtcs/c%d", i, i);
        CHECK(symlink(target, p) == 0);
        snprintf(p, sizeof p, "/config/vkms/mirror/connectors/o%d", i);
        directory(p);
        snprintf(target, sizeof target, "/config/vkms/mirror/encoders/e%d", i);
        snprintf(p, sizeof p, "/config/vkms/mirror/connectors/o%d/possible_encoders/e%d", i, i);
        CHECK(symlink(target, p) == 0);
        snprintf(p, sizeof p, "/config/vkms/mirror/connectors/o%d/status", i);
        put(p, "2");
    }
    put("/config/vkms/mirror/enabled", "1");
}
static void pattern(uint32_t seed)
{
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            shadow[(uint64_t)y * width + x] =
                ((x * 7u + seed) & 255u) << 16 |
                ((y * 11u + seed) & 255u) << 8 | ((x ^ y ^ seed) & 255u);
    mark_all();
    screen_flush();
}
static void verify_outputs(unsigned expected)
{
    CHECK(noutputs == expected);
    for (uint32_t i = 0; i < noutputs; i++) {
        struct output *o = outputs + i;
        struct drm_mode_crtc c = { .crtc_id = o->crtc_id };
        CHECK(ioctl(card, DRM_IOCTL_MODE_GETCRTC, &c) == 0);
        CHECK(c.mode_valid && c.fb_id == o->fb_id);
        CHECK(c.mode.hdisplay == o->mode.hdisplay && c.mode.vdisplay == o->mode.vdisplay);
        uint32_t dw = o->mode.hdisplay, dh = o->mode.vdisplay;
        uint32_t fw = dw, fh = dh;
        if ((uint64_t)dw * height < (uint64_t)dh * width) fh = (uint64_t)dw * height / width;
        else fw = (uint64_t)dh * width / height;
        uint32_t x0 = (dw - fw) / 2, y0 = (dh - fh) / 2;
        for (uint32_t y = 0; y < dh; y++) {
            const uint32_t *line = (const uint32_t *)(o->map + (uint64_t)y * o->pitch);
            for (uint32_t x = 0; x < dw; x++) {
                uint32_t want = 0;
                if (x >= x0 && x < x0 + fw && y >= y0 && y < y0 + fh) {
                    uint32_t sx = (uint64_t)(x - x0) * width / fw;
                    uint32_t sy = (uint64_t)(y - y0) * height / fh;
                    want = shadow[(uint64_t)sy * width + sx];
                }
                CHECK(line[x] == want);
            }
        }
        say("verified connector=%u crtc=%u fb=%u mode=%ux%u fit=%ux%u+%u+%u",
            o->conn_id, o->crtc_id, o->fb_id, dw, dh, fw, fh, x0, y0);
    }
    if (noutputs > 1) CHECK(outputs[0].crtc_id != outputs[1].crtc_id);
}
static long long now_ms(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void crc_capture(char result[2][128])
{
    int data[2];
    for (int i = 0; i < 2; i++) {
        char p[160];
        snprintf(p, sizeof p, "/sys/kernel/debug/dri/0/crtc-%d/crc/control", i);
        put(p, "auto");
        snprintf(p, sizeof p, "/sys/kernel/debug/dri/0/crtc-%d/crc/data", i);
        data[i] = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        CHECK(data[i] >= 0);
    }
    for (int i = 0; i < 2; i++) {
        long long end = now_ms() + 4000;
        char line[256] = "";
        int valid = 0;
        while (now_ms() < end && valid < 4) {
            ssize_t n = read(data[i], line, sizeof line - 1);
            if (n > 0) {
                line[n] = 0;
                char *space = strchr(line, ' ');
                if (space && !strchr(space, 'X')) {
                    snprintf(result[i], 128, "%.127s", space + 1);
                    char *nl = strchr(result[i], '\n');
                    if (nl) *nl = 0;
                    valid++;
                }
            } else CHECK(n < 0 && (errno == EAGAIN || errno == EINTR));
            struct pollfd p = { .fd = data[i], .events = POLLIN };
            CHECK(poll(&p, 1, 100) >= 0);
        }
        CHECK(valid >= 4);
        CHECK(close(data[i]) == 0); /* Closing data disables CRC composition. */
        say("VKMS composed CRC crtc-%d: %s", i, result[i]);
    }
}
/* The cursor on the cursor planes: seen in the composed picture, moved
 * there, gone when hidden; the scanout buffers are never touched. */
static void cursor_planes(void)
{
    char before[2][128], shown[2][128], moved[2][128], hidden[2][128];
    crc_capture(before);
    screen_cursor_move(40, 30);
    screen_cursor_visible(1);
    CHECK(cursor_put_on && !outputs[0].soft_cursor && !outputs[1].soft_cursor);
    verify_outputs(2);
    crc_capture(shown);
    CHECK(strcmp(shown[0], before[0]) != 0 && strcmp(shown[0], shown[1]) == 0);
    screen_cursor_move(200, 100);
    crc_capture(moved);
    CHECK(strcmp(moved[0], shown[0]) != 0 && strcmp(moved[0], moved[1]) == 0);
    screen_cursor_visible(0);
    verify_outputs(2);
    crc_capture(hidden);
    CHECK(strcmp(hidden[0], before[0]) == 0 && strcmp(hidden[1], before[1]) == 0);
    say("PASS real cursor planes: shown, moved and hidden in the composed CRCs, scanout untouched");
}
static void alternate_mode(void)
{
    struct drm_mode_get_connector c = { .connector_id = outputs[1].conn_id };
    CHECK(ioctl(card, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0);
    struct drm_mode_modeinfo *modes = calloc(c.count_modes, sizeof *modes);
    CHECK(modes != NULL);
    c.modes_ptr = (uintptr_t)modes;
    c.count_props = c.count_encoders = 0;
    CHECK(ioctl(card, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0);
    unsigned k;
    for (k = 0; k < c.count_modes; k++)
        if (modes[k].hdisplay == 1280 && modes[k].vdisplay == 720) break;
    CHECK(k < c.count_modes);
    destroy_output(card, &outputs[1]);
    outputs[1].mode = modes[k];
    free(modes);
    CHECK(create_output(card, &outputs[1]) == NULL);
    pattern(23);
    CHECK(set_crtc(card, &outputs[1]) == 0);
    verify_outputs(2);
    char crc[2][128];
    crc_capture(crc);
    CHECK(strcmp(crc[0], crc[1]) != 0);
    say("PASS real unequal-mode scanout: native buffers, aspect fit and borders");
}
/* Whether a uevent with SUBSYSTEM=drm, ACTION=change and LEASE=1 comes in
 * time: the kernel's notice of a lease ended, which init must not take for
 * a change of displays (base/modload.c). */
static int lease_event(int events)
{
    char buf[4096];
    long long end = now_ms() + 2000;
    while (now_ms() < end) {
        ssize_t n = recv(events, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
            int drm = 0, change = 0, lease = 0;
            for (size_t at = 0; at < (size_t)n;) {
                char *zero = memchr(buf + at, 0, (size_t)n - at);
                if (!zero) break;
                drm |= strcmp(buf + at, "SUBSYSTEM=drm") == 0;
                change |= strcmp(buf + at, "ACTION=change") == 0;
                lease |= strcmp(buf + at, "LEASE=1") == 0;
                at = (size_t)(zero - buf) + 1;
            }
            if (drm && change && lease) return 1;
        } else {
            struct pollfd pollfd = { .fd = events, .events = POLLIN };
            CHECK(poll(&pollfd, 1, 100) >= 0);
        }
    }
    return 0;
}
/* A replay as udevadm trigger makes it: written to the card's uevent file,
 * it comes back with SYNTH_UUID, which base/modload.c leaves out. */
static void replay_event(int events)
{
    char buf[4096];
    while (recv(events, buf, sizeof buf, MSG_DONTWAIT) > 0) {}
    put("/sys/class/drm/card0/uevent", "change");
    long long end = now_ms() + 2000;
    while (now_ms() < end) {
        ssize_t n = recv(events, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
            int drm = 0, synthetic = 0;
            for (size_t at = 0; at < (size_t)n;) {
                char *zero = memchr(buf + at, 0, (size_t)n - at);
                if (!zero) break;
                drm |= strcmp(buf + at, "SUBSYSTEM=drm") == 0;
                synthetic |= strncmp(buf + at, "SYNTH_UUID=", 11) == 0;
                at = (size_t)(zero - buf) + 1;
            }
            if (drm) {
                CHECK(synthetic);
                say("a replayed drm uevent carries SYNTH_UUID");
                return;
            }
        } else {
            struct pollfd pollfd = { .fd = events, .events = POLLIN };
            CHECK(poll(&pollfd, 1, 100) >= 0);
        }
    }
    CHECK(!"a replayed drm uevent came");
}
static void lease_child(int lease, int owner, int ready, int done)
{
    CHECK(close(owner) == 0);
    card = -1;
    clear_outputs(-1);
    CHECK(screen_take(lease));
    pattern(91);
    verify_outputs(2);
    screen_cursor_visible(1); /* the lease holds the cursor planes too */
    CHECK(!outputs[0].soft_cursor && !outputs[1].soft_cursor);
    verify_outputs(2);
    screen_cursor_visible(0);
    CHECK(write(ready, "R", 1) == 1);
    char c;
    CHECK(read(done, &c, 1) == 1);
    struct drm_mode_card_res res = { 0 };
    CHECK(ioctl(card, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0);
    CHECK(res.count_crtcs == 0 && res.count_connectors == 0);
    screen_release();
    _exit(0);
}
static void leases(int events)
{
    char drain[4096];
    while (recv(events, drain, sizeof drain, MSG_DONTWAIT) > 0) {}
    uint32_t id;
    int lease = screen_lend(&id);
    CHECK(lease >= 0 && id != 0);
    struct drm_mode_get_lease l = { 0 };
    CHECK(ioctl(lease, DRM_IOCTL_MODE_GET_LEASE, &l) == 0);
    CHECK(l.count_objects >= 8); /* two connectors, CRTCs, primary and cursor planes */
    say("lease contains %u objects (including implicit planes)", l.count_objects);
    int a[2], b[2];
    CHECK(pipe(a) == 0 && pipe(b) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(a[0]); close(b[1]);
        lease_child(lease, card, a[1], b[0]);
    }
    close(a[1]); close(b[0]); close(lease);
    struct pollfd p = { .fd = a[0], .events = POLLIN };
    CHECK(poll(&p, 1, 4000) == 1);
    char c;
    CHECK(read(a[0], &c, 1) == 1 && c == 'R');
    CHECK(screen_take_back(id) == 0);
    verify_outputs(2);
    CHECK(write(b[1], "D", 1) == 1);
    close(a[0]); close(b[1]);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(lease_event(events));
    say("the lease's end came as a drm change with LEASE=1");
    CHECK(screen_drop_master(0) == 0);
    CHECK(screen_take_back(0) == 0);
    verify_outputs(2);
    say("PASS two-output lease: child modeset and cursor, revoke removes visibility, master reacquired");
}
/* Two programs side by side (MON-03): a lease of each output at once; one
 * taken back leaves the other lent, and nothing rescans meanwhile. */
static void side_by_side(void)
{
    struct screen_output at[4];
    CHECK(screen_places(at, 4) == 2 && at[1].x == at[0].x + at[0].width && at[1].y == at[0].y);
    const char *a[] = { outputs[0].name }, *b[] = { outputs[1].name };
    uint32_t ia, ib, ic;
    int la = screen_lend_outputs(a, 1, &ia), lb = screen_lend_outputs(b, 1, &ib);
    CHECK(la >= 0 && lb >= 0 && ia && ib && ia != ib);
    struct drm_mode_get_lease l = { 0 };
    CHECK(ioctl(la, DRM_IOCTL_MODE_GET_LEASE, &l) == 0 && l.count_objects >= 3 && l.count_objects < 8);
    say("one output's lease contains %u objects", l.count_objects);
    CHECK(screen_lend_outputs(a, 1, &ic) < 0); /* lent already */
    CHECK(screen_refresh() < 0 && errno == EBUSY);
    CHECK(screen_take_back(ia) == 0 && lent && outputs[1].lessee == ib && !outputs[0].lessee);
    CHECK(screen_take_back(ib) == 0 && !lent);
    close(la);
    close(lb);
    verify_outputs(2);
    say("PASS side-by-side leases: one output each at once, taken back one at a time");
}
static int events_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    CHECK(fd >= 0);
    struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_groups = 1 };
    CHECK(bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    return fd;
}
static void status(int events, int connector, const char *state, unsigned expected)
{
    char p[160], buf[4096];
    while (recv(events, buf, sizeof buf, MSG_DONTWAIT) > 0) {}
    snprintf(p, sizeof p, "/config/vkms/mirror/connectors/o%d/status", connector);
    put(p, state);
    long long end = now_ms() + 2000;
    int found = 0;
    while (now_ms() < end && !found) {
        ssize_t n = recv(events, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
            int drm = 0, hotplug = 0;
            for (size_t at = 0; at < (size_t)n;) {
                char *zero = memchr(buf + at, 0, (size_t)n - at);
                if (!zero) break;
                drm |= strcmp(buf + at, "SUBSYSTEM=drm") == 0;
                hotplug |= strcmp(buf + at, "HOTPLUG=1") == 0;
                at = (size_t)(zero - buf) + 1;
            }
            found = drm && hotplug;
        }
        if (!found) {
            struct pollfd pollfd = { .fd = events, .events = POLLIN };
            CHECK(poll(&pollfd, 1, 100) >= 0);
        }
    }
    CHECK(found);
    int result = screen_refresh();
    CHECK(result == (expected != 0));
    verify_outputs(expected);
    say("PASS real hotplug o%d status=%s outputs=%u", connector, state, expected);
}
int main(void)
{
    if (getpid() != 1) {
        fprintf(stderr, "This test must run as PID 1 inside its disposable VM.\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    directory("/proc"); directory("/sys"); directory("/dev"); directory("/config");
    CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0);
    CHECK(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0);
    CHECK(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0);
    CHECK(mount("configfs", "/config", "configfs", 0, NULL) == 0);
    directory("/sys/kernel/debug");
    CHECK(mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL) == 0);
    int module = open("/vkms.ko", O_RDONLY | O_CLOEXEC);
    CHECK(module >= 0);
    CHECK(syscall(SYS_finit_module, module, "create_default_dev=0", 0) == 0);
    close(module);
    configure();
    CHECK(!screen_open("/font.psf"));
    CHECK(card < 0 && shadow == NULL);
    CHECK(screen_refresh() == 0);
    put("/config/vkms/mirror/connectors/o0/status", "1");
    put("/config/vkms/mirror/connectors/o1/status", "1");
    CHECK(screen_refresh() == 1);
    say("PASS headless boot: no card/shadow, reconnect probes and enables both outputs");
    char name[32] = { 0 };
    struct drm_version v = { .name = name, .name_len = sizeof name - 1 };
    CHECK(ioctl(card, DRM_IOCTL_VERSION, &v) == 0 && strcmp(name, "vkms") == 0);
    say("real DRM driver=%s logical=%ux%u", name, width, height);
    pattern(11);
    verify_outputs(2);
    char crc[2][128];
    crc_capture(crc);
    CHECK(strcmp(crc[0], crc[1]) == 0);
    say("PASS real mirrored scanout: two CRTCs have identical composed CRCs");
    cursor_planes();
    alternate_mode();
    int events = events_open();
    leases(events);
    side_by_side();
    replay_event(events);
    uint32_t *original = shadow;
    uint32_t w = width, h = height;
    status(events, 1, "2", 1);
    status(events, 0, "2", 0);
    CHECK(card >= 0 && shadow == original && width == w && height == h);
    status(events, 1, "1", 1);
    status(events, 0, "1", 2);
    CHECK(shadow == original && width == w && height == h);
    screen_release();
    CHECK(screen_refresh() == 1);
    verify_outputs(2);
    CHECK(shadow == original && width == w && height == h);
    close(events);
    CHECK(screen_extended(1) == 1 && screen_output_count() == 2);
    CHECK(screen_output_select(0) >= 0);
    screen_fill_px(0, 0, (int)width, (int)height, 0xCC2200);
    CHECK(screen_output_select(1) >= 0);
    screen_fill_px(0, 0, (int)width, (int)height, 0x0066CC);
    screen_flush();
    for (uint32_t i = 0; i < noutputs; i++) {
        uint32_t want = i == 0 ? 0xCC2200 : 0x0066CC;
        CHECK(*(uint32_t *)outputs[i].map == want);
        CHECK(*(uint32_t *)(outputs[i].map + (outputs[i].mode.vdisplay - 1) * outputs[i].pitch) == want);
    }
    crc_capture(crc);
    CHECK(strcmp(crc[0], crc[1]) != 0);
    struct screen_output out;
    CHECK(screen_output_get(1, &out) == 0);
    screen_cursor_move(out.x + out.width / 2, out.y + out.height / 2);
    screen_cursor_visible(1);
    CHECK(outputs[0].cursor_at.x1 == outputs[0].cursor_at.x2 &&
          outputs[1].cursor_at.x1 != outputs[1].cursor_at.x2);
    screen_cursor_visible(0);
    screen_release();
    CHECK(screen_refresh() == 1 && screen_output_count() == 2);
    CHECK(*(uint32_t *)outputs[0].map == 0xCC2200 && *(uint32_t *)outputs[1].map == 0x0066CC);
    CHECK(screen_extended(0) == 1);
    screen_release();
    say("PASS screen-vkms: real KMS, CRCs, cursor planes, lease, master, uevents, disconnect/reconnect, extended outputs");
    sync();
    CHECK(reboot(RB_POWER_OFF) == 0);
    return 0;
}
