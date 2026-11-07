/* Read supported timings from DRM rather than sysfs' resolution-only modes.
 * No master, modeset, EDID parsing or inferred modelines. */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "include/base.h"
#include "include/monitor-mode.h"

static const char *const mode_connector_types[] = {
    "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS", "Component",
    "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB"
};

static int connector_modes(int fd, uint32_t id, const char *name, struct monitor_mode *out, int max)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        struct drm_mode_get_connector c = { .connector_id = id };
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) return -1;
        char actual[64];
        snprintf(actual, sizeof actual, "%s-%u", c.connector_type < sizeof mode_connector_types / sizeof *mode_connector_types ?
                 mode_connector_types[c.connector_type] : "Unknown", c.connector_type_id);
        if (strcmp(name, actual)) return -2;
        if (c.connection != 1 || !c.count_modes) return 0;
        uint32_t count = c.count_modes;
        if (count > 4096) return -1;
        struct drm_mode_modeinfo *modes = calloc(count, sizeof *modes);
        if (!modes) return -1;
        c.modes_ptr = (uintptr_t)modes;
        c.count_props = c.count_encoders = 0;
        int got = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
        if (got < 0 || c.connection != 1) { free(modes); return -1; }
        if (c.count_modes > count) { free(modes); continue; }
        struct drm_mode_modeinfo current = { 0 };
        struct drm_mode_get_encoder enc = { .encoder_id = c.encoder_id };
        if (c.encoder_id && ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id) {
            struct drm_mode_crtc crtc = { .crtc_id = enc.crtc_id };
            if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0 && crtc.mode_valid) current = crtc.mode;
        }
        int n = 0;
        for (uint32_t i = 0; i < c.count_modes; i++) {
            const struct drm_mode_modeinfo *m = &modes[i];
            if (!m->hdisplay || !m->vdisplay) continue;
            int refresh = monitor_mode_millihz(m);
            int j = 0;
            while (j < n && (out[j].width != m->hdisplay || out[j].height != m->vdisplay ||
                            out[j].refresh_mhz != refresh)) j++;
            if (j == n) {
                if (n == max) continue;
                out[n++] = (struct monitor_mode){ .width = m->hdisplay, .height = m->vdisplay,
                                                  .refresh_mhz = refresh };
            }
            out[j].preferred |= !!(m->type & DRM_MODE_TYPE_PREFERRED);
            out[j].current |= current.hdisplay && monitor_mode_same(m, &current);
        }
        free(modes);
        return n;
    }
    return -1;
}

int monitor_modes(const char *name, struct monitor_mode *out, int max)
{
    if (!name || !*name || !out || max <= 0) return -1;
    glob_t g = { 0 };
    int result = -1;
    if (glob("/dev/dri/card[0-9]*", 0, NULL, &g) == 0) {
        for (size_t card = 0; card < g.gl_pathc; card++) {
            int fd = open(g.gl_pathv[card], O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            for (int attempt = 0; attempt < 4; attempt++) {
                struct drm_mode_card_res res = { 0 };
                if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || res.count_connectors > 4096) break;
                uint32_t count = res.count_connectors;
                if (!count) break;
                uint32_t *ids = calloc(count, sizeof *ids);
                if (!ids) break;
                res.connector_id_ptr = (uintptr_t)ids;
                res.count_crtcs = res.count_encoders = res.count_fbs = 0;
                int got = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
                if (got == 0 && res.count_connectors <= count) {
                    for (uint32_t i = 0; i < res.count_connectors; i++) {
                        int n = connector_modes(fd, ids[i], name, out, max);
                        if (n >= 0) { result = n; break; }
                    }
                    free(ids);
                    break;
                }
                free(ids);
                if (got < 0) break;
            }
            close(fd);
            if (result >= 0) break;
        }
    }
    globfree(&g);
    return result;
}
