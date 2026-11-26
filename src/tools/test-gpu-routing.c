/* Synthetic hybrid topology; no DRM device is created, opened or queried.
 * Linux: gcc -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
 *   -Isrc/base/include -Isrc/base src/tools/test-gpu-routing.c
 *   -Wl,--gc-sections -o /tmp/test-gpu-routing */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int fixture_lstat(const char *path, struct stat *st);
static int fixture_access(const char *path, int mode);
static int fixture_stat(const char *path, struct stat *st);
#define lstat fixture_lstat
#define access fixture_access
#define stat(...) fixture_stat(__VA_ARGS__)
#include "../base/screen.c"
#undef stat
#undef access
#undef lstat

static const char *const sysfs = "sys/class/drm", *const dri = "dev/dri";
static const char *const intel = "sys/devices/pci0000:00/0000:00:02.0";
static const char *const amd = "sys/devices/pci0000:00/0000:00:08.1/0000:05:00.0";
static const char *const nvidia = "sys/devices/pci0000:00/0000:00:01.0/0000:01:00.0";
static int installed, accessible = 1, good_node = 1;
static const char *rc_card, *machine_policy, *user_policy;

static int fixture_lstat(const char *path, struct stat *st)
{
    if (lstat(path, st) < 0) return -1;
    if (strncmp(path, "dev/dri/card", 12) == 0) {
        st->st_mode = S_IFCHR | 0600;
        st->st_rdev = makedev(226, good_node ? atoi(path + 12) : 255);
    }
    return 0;
}

static int fixture_access(const char *path, int mode)
{
    if (!accessible && strncmp(path, "dev/dri/card", 12) == 0) return -1;
    return access(path, mode);
}

static int fixture_stat(const char *path, struct stat *st)
{
    if (strcmp(path, "/usr/local/lib/locachy/level") != 0) return stat(path, st);
    st->st_mode = S_IFREG | 0644;
    return installed ? 0 : -1;
}

int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    if (machine_policy) fn("gpu_graphics_policy", machine_policy, arg);
    if (user_policy) fn("gpu_graphics_policy", user_policy, arg);
    if (rc_card) fn("display_card", rc_card, arg);
    return 0;
}

static void pathf(char *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, PATH_MAX, fmt, ap);
    va_end(ap);
    assert(n > 0 && n < PATH_MAX);
}

static void mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    pathf(tmp, "%s", path);
    for (char *p = tmp; ; p++) {
        if (*p == '/' || !*p) {
            char c = *p;
            *p = '\0';
            assert(mkdir(tmp, 0755) == 0 || errno == EEXIST);
            if (!c) break;
            *p = c;
        }
    }
}

static void field(const char *dir, const char *name, const char *value)
{
    char path[PATH_MAX];
    pathf(path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    assert(f && fputs(value, f) >= 0 && fclose(f) == 0);
}

static void link_at(const char *dir, const char *name, const char *target)
{
    char path[PATH_MAX];
    pathf(path, "%s/%s", dir, name);
    unlink(path);
    assert(symlink(target, path) == 0);
}

static void make_card(const char *name, const char *pci, const char *driver, const char *vendor)
{
    char card_dir[PATH_MAX], dev[32];
    mkdir_p(pci);
    link_at(pci, "subsystem", "../../../bus/pci");
    link_at(pci, "driver", driver);
    field(pci, "vendor", vendor);
    field(pci, "class", "0x030000\n");
    pathf(card_dir, "%s/%s", sysfs, name);
    mkdir_p(card_dir);
    char device[PATH_MAX];
    pathf(device, "../../../../%s", pci);
    link_at(card_dir, "device", device);
    snprintf(dev, sizeof dev, "226:%d\n", atoi(name + 4));
    field(card_dir, "dev", dev);
    field(dri, name, "");
}

static void check(int expected, const char *panel)
{
    struct screen_hybrid_route route = { "stale", "stale" };
    assert(screen_hybrid_route_from(sysfs, dri, &route) == expected);
    assert(strcmp(route.panel_card, expected ? panel : "") == 0);
    assert(strcmp(route.nvidia_card, expected ? "dev/dri/card0" : "") == 0);
}

/* Test the installed and layered gates without probing the host's sysfs. */
static void check_policy(void)
{
    struct screen_hybrid_route route;
    assert(unsetenv("LOCACHY_DRM_CARD") == 0);
    assert(screen_hybrid_route(&route) == 0 && !route.panel_card[0]);
    int auto_policy = 1;
    on_rc_gpu_graphics_policy("gpu_graphics_policy", "off", &auto_policy);
    assert(auto_policy == 0);
    on_rc_gpu_graphics_policy("gpu_graphics_policy", "auto", &auto_policy);
    assert(auto_policy == 1);
    on_rc_gpu_graphics_policy("gpu_graphics_policy", "other", &auto_policy);
    assert(auto_policy != 1);
    installed = 1;
    machine_policy = "auto";
    user_policy = "off";
    assert(screen_hybrid_route(&route) == 0 && !route.panel_card[0]);
    user_policy = "other";
    assert(screen_hybrid_route(&route) == 0 && !route.panel_card[0]);
    user_policy = "auto";
    rc_card = "i915";
    assert(screen_hybrid_route(&route) == 0 && !route.panel_card[0]);
    rc_card = "auto";
    assert(setenv("LOCACHY_DRM_CARD", "nouveau", 1) == 0);
    assert(screen_hybrid_route(&route) == 0 && !route.panel_card[0]);
}

int main(void)
{
    char root[] = "/tmp/locachy-gpu-routing-XXXXXX";
    assert(mkdtemp(root) && chdir(root) == 0);
    mkdir_p(sysfs);
    mkdir_p(dri);
    make_card("card0", nvidia, "nvidia", "0x10de\n");
    make_card("card17", intel, "i915", "0x8086\n");
    const char *panel = "sys/class/drm/card17-eDP-1";
    mkdir_p(panel);
    field(panel, "status", "connected\n");
    field(nvidia, "removable", "fixed\n");
    check(1, "dev/dri/card17"); /* NVIDIA first by card number and enumeration order */
    field(panel, "status", "disconnected\n");
    check(0, NULL);
    field(panel, "status", "connected\n");
    field(nvidia, "removable", "removable\n");
    check(0, NULL);
    field(nvidia, "removable", "unknown\n");
    check(0, NULL);
    const char *fwnode = "sys/devices/pci0000:00/0000:00:01.0/0000:01:00.0/firmware_node";
    mkdir_p(fwnode);
    field(fwnode, "adr", "0x00000000\n");
    check(1, "dev/dri/card17");
    const char *bridge = "sys/devices/pci0000:00/0000:00:01.0";
    field(bridge, "removable", "removable\n");
    check(0, NULL);
    field(bridge, "removable", "fixed\n");
    accessible = 0;
    check(0, NULL);
    accessible = 1;
    good_node = 0;
    check(0, NULL);
    good_node = 1;
    assert(unlink("dev/dri/card0") == 0);
    check(0, NULL);
    field(dri, "card0", "");
    mkdir_p("sys/class/drm/card28");
    check(0, NULL);
    assert(rmdir("sys/class/drm/card28") == 0);
    make_card("card17", amd, "amdgpu", "0x1002\n");
    field(amd, "device", "0x73bf\n"); /* Navi 21 is not an APU. */
    check(0, NULL);
    field(amd, "device", "0x1638\n");
    check(1, "dev/dri/card17");
    field(amd, "device", "0x164e\n");
    const char *gc = "sys/devices/pci0000:00/0000:00:08.1/0000:05:00.0/ip_discovery/die/0/GC/0";
    mkdir_p(gc);
    field(gc, "major", "10\n"); field(gc, "minor", "3\n"); field(gc, "revision", "6\n");
    check(1, "dev/dri/card17");
    check_policy();
    assert(chdir("/tmp") == 0);
    char cmd[80];
    snprintf(cmd, sizeof cmd, "rm -rf -- %s", root);
    assert(system(cmd) == 0);
    puts("PASS GPU topology, node and policy gates");
    return 0;
}
