/* Advertised DRM mode identity and selection. Refresh units match wlroots'
 * calculate_refresh_rate: millihertz, including interlace/doublescan/vscan. */
#ifndef LOCACHY_MONITOR_MODE_H
#define LOCACHY_MONITOR_MODE_H
#include <drm/drm_mode.h>
#include <stdint.h>

static inline int monitor_mode_millihz(const struct drm_mode_modeinfo *mode)
{
    if (!mode->htotal || !mode->vtotal) return (int)mode->vrefresh * 1000;
    int64_t refresh = ((int64_t)mode->clock * 1000000 / mode->htotal + mode->vtotal / 2) / mode->vtotal;
    if (mode->flags & DRM_MODE_FLAG_INTERLACE) refresh *= 2;
    if (mode->flags & DRM_MODE_FLAG_DBLSCAN) refresh /= 2;
    if (mode->vscan > 1) refresh /= mode->vscan;
    return refresh > 0 && refresh <= INT32_MAX ? (int)refresh : 0;
}

static inline int monitor_mode_same(const struct drm_mode_modeinfo *a,
                                    const struct drm_mode_modeinfo *b)
{
    return a->hdisplay == b->hdisplay && a->vdisplay == b->vdisplay &&
           monitor_mode_millihz(a) == monitor_mode_millihz(b) &&
           !!(a->flags & DRM_MODE_FLAG_INTERLACE) == !!(b->flags & DRM_MODE_FLAG_INTERLACE);
}

/* Exact refresh requests never select a different rate at the same size.
 * A legacy size-only request preserves the active rate before preferred.
 * Without a requested size, preserve an active advertised mode first. */
static inline int monitor_mode_choose(const struct drm_mode_modeinfo *modes, unsigned count,
                                      const struct drm_mode_modeinfo *current,
                                      int width, int height, int refresh_mhz)
{
    int best = -1, rank = -1;
    for (unsigned i = 0; i < count; i++) {
        const struct drm_mode_modeinfo *m = &modes[i];
        if (!m->hdisplay || !m->vdisplay) continue;
        if (width > 0 && (m->hdisplay != width || m->vdisplay != height)) continue;
        if (refresh_mhz > 0 && monitor_mode_millihz(m) != refresh_mhz) continue;
        int score = current && current->hdisplay && monitor_mode_same(m, current) ? 2 :
                    (m->type & DRM_MODE_TYPE_PREFERRED) ? 1 : 0;
        if (score > rank) {
            rank = score;
            best = (int)i;
        }
    }
    return best;
}
#endif
