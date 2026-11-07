/* Real advertised-mode enumeration with DRM queries doubled: never modesets. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
static int fake_ioctl(int fd, unsigned long req, ...);
static int fake_open(const char *path, int flags, ...);
static int fake_close(int fd);
static int fake_glob(const char *pattern, int flags, int (*err)(const char *, int), glob_t *out);
static void fake_globfree(glob_t *g);
#define ioctl fake_ioctl
#define open fake_open
#define close fake_close
#define glob fake_glob
#define globfree fake_globfree
#include "../base/monitor-modes.c"
#undef ioctl
#undef open
#undef close
#undef glob
#undef globfree

static struct drm_mode_modeinfo timing(int refresh)
{
    return (struct drm_mode_modeinfo){ .hdisplay = 1920, .vdisplay = 1080,
        .htotal = 2200, .vtotal = 1125, .clock = (uint32_t)(refresh == 59940 ? 148352 : refresh == 30000 ? 74250 : 148500),
        .vrefresh = (uint32_t)(refresh / 1000) };
}
static struct drm_mode_modeinfo modes[4];
static int queries;
static int fake_open(const char *p, int flags, ...)
{
    assert(!strcmp(p, "/dev/dri/card0") && flags == (O_RDONLY | O_CLOEXEC));
    return 7;
}
static int fake_close(int fd) { assert(fd == 7); return 0; }
static int fake_glob(const char *p, int flags, int (*err)(const char *, int), glob_t *g)
{
    (void)flags; (void)err;
    assert(!strcmp(p, "/dev/dri/card[0-9]*"));
    static char *paths[] = { "/dev/dri/card0", NULL };
    g->gl_pathc = 1; g->gl_pathv = paths;
    return 0;
}
static void fake_globfree(glob_t *g) { (void)g; }
static int fake_ioctl(int fd, unsigned long req, ...)
{
    assert(fd == 7);
    va_list ap; va_start(ap, req); void *p = va_arg(ap, void *); va_end(ap);
    queries++;
    if (req == DRM_IOCTL_MODE_GETRESOURCES) {
        struct drm_mode_card_res *r = p;
        if (r->connector_id_ptr && r->count_connectors >= 1) *(uint32_t *)(uintptr_t)r->connector_id_ptr = 11;
        r->count_connectors = 1;
    } else if (req == DRM_IOCTL_MODE_GETCONNECTOR) {
        struct drm_mode_get_connector *c = p;
        c->connection = 1; c->connector_type = DRM_MODE_CONNECTOR_HDMIA; c->connector_type_id = 1; c->encoder_id = 22;
        if (c->modes_ptr && c->count_modes >= 4) memcpy((void *)(uintptr_t)c->modes_ptr, modes, sizeof modes);
        c->count_modes = 4;
    } else if (req == DRM_IOCTL_MODE_GETENCODER) {
        ((struct drm_mode_get_encoder *)p)->crtc_id = 33;
    } else if (req == DRM_IOCTL_MODE_GETCRTC) {
        struct drm_mode_crtc *c = p; c->mode_valid = 1; c->mode = modes[2];
    } else { assert(!"enumeration must not change DRM state"); }
    return 0;
}
int main(void)
{
    modes[0] = timing(60000); modes[0].type = DRM_MODE_TYPE_PREFERRED;
    modes[1] = timing(59940);
    modes[2] = timing(30000);
    modes[3] = modes[0]; /* duplicate timing is one displayed choice */
    assert(monitor_mode_millihz(&modes[0]) == 60000);
    assert(monitor_mode_millihz(&modes[1]) == 59940);
    assert(monitor_mode_millihz(&modes[2]) == 30000);
    struct drm_mode_modeinfo special = modes[0];
    special.flags = DRM_MODE_FLAG_INTERLACE; assert(monitor_mode_millihz(&special) == 120000);
    special.flags = DRM_MODE_FLAG_DBLSCAN; assert(monitor_mode_millihz(&special) == 30000);
    special.flags = 0; special.vscan = 2; assert(monitor_mode_millihz(&special) == 30000);
    assert(monitor_mode_choose(modes, 4, &modes[2], 1920, 1080, 0) == 2);
    assert(monitor_mode_choose(modes, 4, &modes[2], 1920, 1080, 60000) == 0);
    assert(monitor_mode_choose(modes, 4, &modes[0], 1920, 1080, 59940) == 1);
    assert(monitor_mode_choose(modes, 4, &modes[0], 1920, 1080, 50000) == -1);
    puts("PASS refresh identity: 59.940 and 60 Hz distinct; interlace/doublescan/vscan match wlroots units");
    puts("PASS exact requested refresh; legacy resolution keeps active 30 Hz instead of choosing preferred 60 Hz");
    struct monitor_mode found[8];
    assert(monitor_modes("HDMI-A-1", found, 8) == 3);
    assert(found[0].preferred && found[0].refresh_mhz == 60000 && !found[0].current);
    assert(found[1].refresh_mhz == 59940 && !found[1].current);
    assert(found[2].refresh_mhz == 30000 && found[2].current);
    assert(monitor_modes("DP-9", found, 8) == -1);
    assert(queries > 0);
    puts("PASS DRM advertised modes enumerate distinct rates with current/preferred flags, no master or mode change");
    return 0;
}
