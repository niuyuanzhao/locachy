/* No DRM device is opened: all screen ownership and buffer syscalls are mocked.
 * Build with the same UAPI include path as screen.c, without CONFIG_FREETYPE:
 * cc -O2 -Wall -Wextra -Werror -I src/base/include src/tools/test-screen-mirror.c -o /tmp/test-screen-mirror
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int mock_ioctl(int fd, unsigned long request, ...);
static void *mock_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
static int mock_munmap(void *addr, size_t len);
static int mock_close(int fd);
static int mock_usleep(useconds_t usec);
static int mock_open(const char *path, int flags, ...);
#define ioctl mock_ioctl
#define mmap mock_mmap
#define munmap mock_munmap
#define close mock_close
#define usleep mock_usleep
#define open mock_open
#include "../base/screen.c"
#undef ioctl
#undef mmap
#undef munmap
#undef close
#undef usleep
#undef open

static unsigned long fail_request;
static int fail_at, request_calls, fail_map;
static int buffers, frames, mappings, closes, sets, dirties, lease_calls, reject_multi;
static uint32_t lease_counts[2];
static struct drm_clip_rect clips[2];
static int single_output, external_only, disconnected, master_busy;
static int cursor_calls, no_cursor_plane, cursor_last_x, cursor_last_y;
/* A real 2x2 PSF2 file stands in for Terminus; Terminus itself is "missing". */
static char test_font[] = "/tmp/test-screen-font-XXXXXX";
static int font_fd = -1, scale_lines, extended_conf, positioned;
static double internal_scale = 1.0, external_scale = 2.0;
/* When set, Terminus is "there" (the test font) but for its bold at 12, as
 * the real one; the paths asked for are kept. */
static int serve_terminus, nasked;
static char asked[4][192];
static int mock_open(const char *path, int flags, ...)
{
    if (strcmp(path, "/dev/dri/card0") == 0) return 7;
    if (strcmp(path, test_font) == 0) return font_fd = open(path, flags);
    if (serve_terminus && strncmp(path, BASE_FONT_DIR "/", strlen(BASE_FONT_DIR) + 1) == 0) {
        if (nasked < 4) snprintf(asked[nasked++], sizeof asked[0], "%s", path);
        if (strstr(path, "/ter-u12b.psf") == NULL) return font_fd = open(test_font, flags);
    }
    errno = ENOENT;
    return -1;
}
static void write_test_font(void)
{
    uint32_t h[8] = { PSF2_MAGIC, 0, 32, 0, 128, 2, 2, 2 };
    uint8_t glyph_bits[256] = { 0 };
    glyph_bits['A' * 2] = 0x80;     /* 'A': the left pixel in row 0, */
    glyph_bits['A' * 2 + 1] = 0x40; /* the right one in row 1 */
    int fd = mkstemp(test_font);
    assert(fd >= 0 && write(fd, h, sizeof h) == sizeof h);
    assert(write(fd, glyph_bits, sizeof glyph_bits) == sizeof glyph_bits && close(fd) == 0);
    snprintf(font_fallback, sizeof font_fallback, "%s", test_font);
}
static const char *named_internal; /* rc.conf's internal_screen */
int monitor_internal(const char *name)
{
    return named_internal ? strcmp(name, named_internal) == 0 : strncmp(name, "eDP", 3) == 0;
}
int monitor_conf(const char *name, struct monitor_conf *out)
{
    memset(out, 0, sizeof *out);
    if (extended_conf) {
        int internal = strcmp(name, "eDP-1") == 0;
        out->scale = internal ? internal_scale : external_scale;
        snprintf(out->place, sizeof out->place, "%s", internal ? "origin" : "right");
        if (positioned) {
            out->has_position = 1;
            out->x = internal ? -8 : 3;
            out->y = internal ? 0 : 2;
        }
        return 1;
    }
    out->scale = scale_lines && strcmp(name, "eDP-1") == 0 ? 1.5 : 0;
    return out->scale > 0;
}

void say(const char *fmt, ...)
{
    (void)fmt;
}

int control_fd(void) { return -1; }
void control_wait_take(void) { abort(); }
void base_language_reload(void) {}
int panel_containers(const char *name, unsigned long long *ram)
{
    (void)name;
    *ram = 0;
    return 0;
}
void panel_draw(uint32_t *px, int w, int h, int stride, const char *title, const char *sub,
                const char *ime, int reserve, int *mid_x0, int *mid_x1)
{
    (void)px; (void)w; (void)h; (void)stride; (void)title; (void)sub;
    (void)ime; (void)reserve; (void)mid_x0; (void)mid_x1;
    abort();
}

static int mock_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 7 || fd == 8);
    if (request == fail_request && ++request_calls == fail_at) {
        errno = EIO;
        return -1;
    }
    if (request == DRM_IOCTL_SET_MASTER && master_busy) { errno = EBUSY; return -1; }
    if (request == DRM_IOCTL_SET_MASTER || request == DRM_IOCTL_DROP_MASTER) return 0;
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == DRM_IOCTL_MODE_GETRESOURCES) {
        struct drm_mode_card_res *r = arg;
        if (r->count_connectors >= 3) {
            uint32_t ids[] = { 101, 102, 103 };
            memcpy((void *)(uintptr_t)r->connector_id_ptr, ids, sizeof ids);
        }
        if (r->count_crtcs >= 2) {
            uint32_t ids[] = { 201, 202 };
            memcpy((void *)(uintptr_t)r->crtc_id_ptr, ids, sizeof ids);
        }
        r->count_connectors = 3;
        r->count_crtcs = 2;
    } else if (request == DRM_IOCTL_MODE_GETCONNECTOR) {
        struct drm_mode_get_connector *c = arg;
        uint32_t id = c->connector_id;
        if (disconnected && id == 103) {
            c->connection = 2;
            c->count_modes = 0;
            return 0;
        }
        c->connection = 1;
        c->connector_type = id == 102 ? DRM_MODE_CONNECTOR_eDP : DRM_MODE_CONNECTOR_HDMIA;
        c->connector_type_id = id - 101; /* eDP-1, HDMI-A-0 / HDMI-A-2 */
        c->encoder_id = id + 200;
        if (c->count_modes >= 2) {
            struct drm_mode_modeinfo modes[] = {
                { .hdisplay = 10, .vdisplay = 8 },
                { .hdisplay = id == 102 ? 8 : 12, .vdisplay = id == 102 ? 6 : 10,
                  .type = DRM_MODE_TYPE_PREFERRED }
            };
            memcpy((void *)(uintptr_t)c->modes_ptr, modes, sizeof modes);
        }
        if (c->count_encoders >= 2) {
            uint32_t encs[] = { id + 200, id + 300 };
            memcpy((void *)(uintptr_t)c->encoders_ptr, encs, sizeof encs);
        }
        c->count_modes = c->count_encoders = 2;
    } else if (request == DRM_IOCTL_MODE_GETENCODER) {
        struct drm_mode_get_encoder *e = arg;
        /* The first connector needs its alternative encoder for both to fit. */
        e->possible_crtcs = e->encoder_id == 301 || e->encoder_id == 302 ? 1 :
                            e->encoder_id == 401 ? 2 : 0;
        if (single_output && e->encoder_id != 302) e->possible_crtcs = 0;
        if (external_only && e->encoder_id == 302) e->possible_crtcs = 0;
        e->crtc_id = e->encoder_id == 301 ? 201 : 0;
    } else if (request == DRM_IOCTL_MODE_CREATE_DUMB) {
        struct drm_mode_create_dumb *d = arg;
        d->pitch = d->width == 64 ? 256 : d->width * 4 + 8; /* a cursor buffer: no padding */
        d->size = (uint64_t)d->pitch * d->height;
        d->handle = (uint32_t)++buffers;
    } else if (request == DRM_IOCTL_MODE_ADDFB) {
        struct drm_mode_fb_cmd *f = arg;
        f->fb_id = (uint32_t)(++frames + 1000);
    } else if (request == DRM_IOCTL_MODE_MAP_DUMB) {
        struct drm_mode_map_dumb *m = arg;
        m->offset = m->handle * 4096u;
    } else if (request == DRM_IOCTL_MODE_RMFB) {
        assert(frames > 0);
        frames--;
    } else if (request == DRM_IOCTL_MODE_DESTROY_DUMB) {
        assert(buffers > 0);
        buffers--;
    } else if (request == DRM_IOCTL_MODE_SETCRTC) {
        struct drm_mode_crtc *s = arg;
        assert(s->count_connectors == 1 && s->mode_valid && s->fb_id);
        sets++;
    } else if (request == DRM_IOCTL_MODE_DIRTYFB) {
        struct drm_mode_fb_dirty_cmd *d = arg;
        assert(d->num_clips == 1);
        clips[dirties++ % 2] = *(struct drm_clip_rect *)(uintptr_t)d->clips_ptr;
    } else if (request == DRM_IOCTL_MODE_CREATE_LEASE) {
        struct drm_mode_create_lease *l = arg;
        assert(lease_calls < 2);
        lease_counts[lease_calls++] = l->object_count;
        uint32_t *ids = (uint32_t *)(uintptr_t)l->object_ids;
        for (uint32_t i = 0; i < l->object_count / 2; i++) {
            assert(ids[2 * i] == outputs[i].conn_id);
            assert(ids[2 * i + 1] == outputs[i].crtc_id);
        }
        if (reject_multi && l->object_count > 2) {
            errno = EINVAL;
            return -1;
        }
        l->fd = 9;
        l->lessee_id = 42;
    } else if (request == DRM_IOCTL_MODE_CURSOR2) {
        struct drm_mode_cursor2 *c = arg;
        assert(c->crtc_id == 201 || c->crtc_id == 202);
        cursor_calls++;
        if (no_cursor_plane) { errno = ENXIO; return -1; }
        if (c->flags & DRM_MODE_CURSOR_MOVE) { cursor_last_x = c->x; cursor_last_y = c->y; }
    } else {
        assert(request == DRM_IOCTL_MODE_REVOKE_LEASE);
    }
    return 0;
}

static void *mock_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)addr; (void)prot; (void)flags; (void)off;
    assert(fd == 7 || fd == 8);
    if (fail_map && ++request_calls == fail_at) return MAP_FAILED;
    void *p = malloc(len);
    assert(p != NULL);
    memset(p, 0xAB, len);
    mappings++;
    return p;
}
static int mock_munmap(void *addr, size_t len)
{
    (void)len;
    assert(mappings > 0);
    mappings--;
    free(addr);
    return 0;
}
static int mock_close(int fd)
{
    if (fd == font_fd) {
        font_fd = -1;
        return close(fd);
    }
    assert(fd == 7 || fd == 8);
    closes++;
    return 0;
}
static int mock_usleep(useconds_t usec) { (void)usec; return 0; }

static void test_copy(void)
{
    uint32_t src[64], dst[120];
    for (uint32_t i = 0; i < 64; i++) src[i] = i + 1;
    for (uint32_t sw = 1; sw <= 8; sw++) {
        for (uint32_t sh = 1; sh <= 8; sh++) {
            for (uint32_t dw = 1; dw <= 9; dw++) {
                for (uint32_t dh = 1; dh <= 9; dh++) {
                    for (unsigned i = 0; i < 120; i++) dst[i] = UINT32_MAX;
                    struct drm_clip_rect damage = { 0, 0, 1, 1 };
                    struct drm_clip_rect c = copy_picture((uint8_t *)(dst + 1), (dw + 2) * 4,
                        dw, dh, src, sw, sh, damage);
                    struct drm_clip_rect fit = fit_rect(sw, sh, dw, dh);
                    assert(fit.x1 < fit.x2 && fit.x2 <= dw && fit.y1 < fit.y2 && fit.y2 <= dh);
                    if (sw == dw && sh == dh) {
                        assert(memcmp(&c, &damage, sizeof c) == 0);
                    } else {
                        assert(c.x1 == 0 && c.y1 == 0 && c.x2 == dw && c.y2 == dh);
                    }
                    for (uint32_t y = 0; y < dh; y++) {
                        for (uint32_t x = 0; x < dw + 2; x++) {
                            uint32_t expected = UINT32_MAX;
                            if (x < dw && sw == dw && sh == dh) {
                                if (x == 0 && y == 0) expected = src[0];
                            } else if (x < dw) {
                                expected = 0;
                                if (x >= fit.x1 && x < fit.x2 && y >= fit.y1 && y < fit.y2) {
                                    uint32_t sx = (x - fit.x1) * sw / (fit.x2 - fit.x1);
                                    uint32_t sy = (y - fit.y1) * sh / (fit.y2 - fit.y1);
                                    expected = src[sy * sw + sx];
                                }
                            }
                            assert(dst[1 + y * (dw + 2) + x] == expected);
                        }
                    }
                    assert(dst[0] == UINT32_MAX && dst[1 + dh * (dw + 2)] == UINT32_MAX);
                }
            }
        }
    }
    struct drm_clip_rect f = fit_rect(1366, 768, 1920, 1080);
    assert(f.x1 == 0 && f.x2 == 1920 && f.y1 == 0 && f.y2 == 1079);
    f = fit_rect(1920, 1080, 1024, 768);
    assert(f.x1 == 0 && f.x2 == 1024 && f.y1 == 96 && f.y2 == 672);
    /* A non-origin dirty rectangle must not overwrite other same-size pixels. */
    memset(dst, 0, sizeof dst);
    struct drm_clip_rect d = { 1, 1, 3, 3 };
    copy_picture((uint8_t *)dst, 24, 4, 4, src, 4, 4, d);
    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 6; x++) {
            assert(dst[y * 6 + x] == (x >= 1 && x < 3 && y >= 1 && y < 3 ? src[y * 4 + x] : 0));
        }
    }
}

static int maximum(const struct output *list, int index, uint32_t used)
{
    if (index == 3) return 0;
    int best = maximum(list, index + 1, used);
    for (uint32_t k = 0; k < 3; k++) {
        uint32_t bit = 1u << k;
        if ((list[index].possible_crtcs & bit) && !(used & bit)) {
            int n = 1 + maximum(list, index + 1, used | bit);
            if (n > best) best = n;
        }
    }
    return best;
}

static void test_matching(void)
{
    uint32_t crtcs[] = { 201, 202, 203 };
    for (uint32_t masks = 0; masks < 512; masks++) {
        struct output list[3] = { 0 };
        uint32_t owners[32];
        for (unsigned k = 0; k < 32; k++) owners[k] = UINT32_MAX;
        for (unsigned i = 0; i < 3; i++) {
            list[i].possible_crtcs = (masks >> (i * 3)) & 7;
            list[i].preferred_crtc = 201;
        }
        int expected = maximum(list, 0, 0), count = 0;
        for (unsigned i = 0; i < 3; i++) {
            uint32_t seen = 0;
            count += match_output(list, i, crtcs, 3, owners, &seen);
        }
        assert(count == expected);
        uint32_t used = 0;
        for (unsigned i = 0; i < 3; i++) {
            if (!list[i].crtc_id) continue;
            uint32_t bit = 1u << (list[i].crtc_id - 201);
            assert((list[i].possible_crtcs & bit) && !(used & bit));
            used |= bit;
        }
    }
    struct output high = { .possible_crtcs = 1u << 31 };
    uint32_t all[32], owners[32], seen = 0;
    for (unsigned i = 0; i < 32; i++) { all[i] = i + 1; owners[i] = UINT32_MAX; }
    assert(match_output(&high, 0, all, 32, owners, &seen) && high.crtc_id == 32);
}

static void test_lifecycle(void)
{
    write_test_font();
    assert(prepare(7) == NULL && glyph_w == 2 && glyph_h == 2 && scale == 1.0);
    assert(bar_h == 3 && top == 1 && cols == 4); /* one and a half rows of the 2x2 font */
    assert(noutputs == 2 && outputs[0].conn_id == 102 && outputs[0].crtc_id == 201);
    assert(outputs[1].conn_id == 101 && outputs[1].crtc_id == 202);
    assert(width == 8 && height == 6);
    uint32_t *saved = shadow;
    shadow[0] = 0x123456;
    assert(show(7) == NULL && sets == 2 && frames == 2 && mappings == 2);
    assert(*(uint32_t *)outputs[0].map == 0x123456);
    dirties = 0;
    touch(1, 1, 2, 2);
    screen_flush();
    assert(dirties == 2 && clips[0].x1 == 1 && clips[0].x2 == 2);
    assert(clips[1].x1 == 0 && clips[1].x2 == 12 && clips[1].y2 == 10);
    uint32_t lessee;
    assert(screen_lend(&lessee) == 9 && lessee == 42 && lease_counts[0] == 4);
    dirties = 0;
    touch(0, 0, 1, 1);
    screen_flush();
    assert(dirties == 0 && dirty.x1 != dirty.x2);
    int previous_sets = sets;
    assert(screen_take_back(lessee) == 0);
    assert(sets == previous_sets + 2 && dirties == 2 && !lent);
    lease_calls = 0;
    reject_multi = 1;
    assert(screen_lend(&lessee) == 9 && lessee == 42);
    assert(lease_calls == 2 && lease_counts[0] == 4 && lease_counts[1] == 2);
    screen_take_back(lessee);
    screen_release();
    assert(!outputs && !noutputs && !buffers && !frames && !mappings && closes == 1);
    assert(shadow == saved && shadow[0] == 0x123456);
    assert(screen_take(8));
    assert(shadow == saved && width == 8 && height == 6);
    screen_release();
    /* Topology errors cleanly fail; an individual output error preserves
     * the other scanout, whether the first or second allocation failed. */
    fail_request = DRM_IOCTL_MODE_GETRESOURCES;
    for (int nth = 1; nth <= 2; nth++) {
        fail_at = nth; request_calls = 0;
        assert(!screen_take(8));
        assert(!outputs && !noutputs && card == -1 && !buffers && !frames && !mappings);
    }
    unsigned long requests[] = { DRM_IOCTL_MODE_CREATE_DUMB, DRM_IOCTL_MODE_ADDFB,
                                DRM_IOCTL_MODE_MAP_DUMB, DRM_IOCTL_MODE_SETCRTC };
    for (unsigned i = 0; i < sizeof requests / sizeof requests[0]; i++) {
        for (int nth = 1; nth <= 2; nth++) {
            fail_request = requests[i]; fail_at = nth; request_calls = 0;
            assert(screen_take(8));
            assert(noutputs == 1 && buffers == 1 && frames == 1 && mappings == 1);
            assert(shadow == saved && shadow[0] == 0x123456);
            screen_release();
            assert(!outputs && !noutputs && card == -1 && !buffers && !frames && !mappings);
        }
    }
    fail_request = 0;
    fail_map = 1;
    for (int nth = 1; nth <= 2; nth++) {
        fail_at = nth; request_calls = 0;
        assert(screen_take(8) && noutputs == 1);
        screen_release();
        assert(!outputs && !noutputs && card == -1 && !buffers && !frames && !mappings);
    }
    fail_map = 0;
    assert(screen_take(8));
    screen_release();
    /* A different output resizes the canvas; an unchanged one keeps it. */
    single_output = 1;
    assert(screen_take(8) && noutputs == 1 && outputs[0].conn_id == 102);
    lease_calls = 0;
    assert(screen_lend(&lessee) == 9 && lease_calls == 1 && lease_counts[0] == 2);
    screen_take_back(lessee);
    screen_release();
    single_output = 0;
    external_only = disconnected = 1;
    assert(screen_take(8) && noutputs == 1 && outputs[0].conn_id == 101);
    assert(outputs[0].mode.hdisplay == 12 && width == 12 && height == 10);
    assert(cols == 6 && rows == 4 && bar_h == 3 && shadow[0] == 0);
    saved = shadow;
    shadow[0] = 0x654321;
    screen_release();
    assert(screen_take(8) && shadow == saved && shadow[0] == 0x654321);
    screen_release();
    external_only = disconnected = 0;
    assert(screen_refresh() == 1 && card == 7 && noutputs == 2);
    assert(width == 8 && height == 6 && cols == 4 && rows == 2 && shadow[0] == 0);
    saved = shadow;
    puts("PASS canvas follows the leased output size and preserves unchanged geometry");
    screen_release();
    external_only = disconnected = 1;
    snprintf(font_fallback, sizeof font_fallback, "%s", "/nonexistent/font.psf");
    assert(!screen_take(8) && width == 8 && height == 6 && shadow == saved && card == -1);
    snprintf(font_fallback, sizeof font_fallback, "%s", test_font);
    assert(screen_take(8) && width == 12 && height == 10);
    screen_release();
    external_only = disconnected = 0;
    assert(screen_take(8) && width == 8 && height == 6);
    saved = shadow;
    puts("PASS failed resize keeps the old canvas; a later take can retry safely");
    lease_calls = 0; reject_multi = 0;
    assert(screen_lend(&lessee) == 9 && active_lessee == 42);
    master_busy = 1;
    assert(screen_take_back(lessee) == -1 && active_lessee == 0 && lent == 0);
    master_busy = 0;
    assert(screen_refresh() == 1);
    screen_release();
    /* rc.conf's internal_screen: that output comes first. */
    named_internal = "HDMI-A-0";
    assert(screen_take(8) && strcmp(outputs[0].name, "HDMI-A-0") == 0);
    assert(outputs[0].internal && !outputs[1].internal);
    screen_release();
    named_internal = NULL;
    /* A scale for the first output: the font stays when no larger one can be
     * read, and the bar stays one and a half of its rows; the picture is
     * cleared. */
    scale_lines = 1;
    assert(screen_take(8) && strcmp(outputs[0].name, "eDP-1") == 0);
    assert(strcmp(outputs[1].name, "HDMI-A-0") == 0);
    assert(scale == 1.5 && bar_h == 3 && shadow[0] == 0);
    assert(screen_rescale() == 0);
    /* The cursor: one buffer on each output's cursor plane, moved alone; a
     * scaled output gets it where the picture is fitted. Hidden, it is taken
     * off; it is not put on before it is wanted. */
    assert(cursor_calls == 0);
    screen_cursor_visible(1);
    assert(cursor_calls == 2 && cursor_handle && cursor_map && !outputs[0].soft_cursor);
    screen_cursor_move(3, 2);
    assert(cursor_calls == 4);
    assert(cursor_last_x == 4 - hot_x && cursor_last_y == 3 - hot_y); /* HDMI 12x10: 8x6 fitted as 12x9 */
    screen_cursor_visible(0);
    assert(cursor_calls == 6);
    /* No cursor plane: blended into the scanout, the picture put back. */
    no_cursor_plane = 1;
    screen_cursor_move(0, 0); /* hidden: only remembered; its white fill is in view */
    assert(cursor_calls == 6);
    screen_cursor_visible(1);
    assert(outputs[0].soft_cursor && outputs[0].cursor_at.x1 != outputs[0].cursor_at.x2);
    int white = 0;
    for (uint32_t y = 0; y < 6; y++)
        for (uint32_t x = 0; x < 8; x++)
            white += *(uint32_t *)(outputs[0].map + y * outputs[0].pitch + x * 4) == 0xFFFFFF;
    assert(white > 0);
    screen_cursor_visible(0);
    white = 0;
    for (uint32_t y = 0; y < 6; y++)
        for (uint32_t x = 0; x < 8; x++)
            white += *(uint32_t *)(outputs[0].map + y * outputs[0].pitch + x * 4) == 0xFFFFFF;
    assert(white == 0 && outputs[0].cursor_at.x1 == outputs[0].cursor_at.x2);
    no_cursor_plane = 0;
    /* Text above 32 pixels (18 x 2.25 = 40.5): Terminus 20, each pixel 2x2;
     * at 12 pixels (18 x 0.65, below any scale settings offers) the normal
     * one, as there is no bold. */
    /* The text row at settings' scales, the nearest Terminus to 18 x the
     * scale (the shared display's bar and overlays follow it too). */
    static const struct { double scale; int px; } rows[] = {
        { 0.75, 14 }, { 1.0, 18 }, { 1.25, 24 }, { 1.5, 28 }, { 1.75, 32 },
        { 2.0, 36 }, { 2.25, 40 }, { 2.5, 44 }, { 2.75, 48 }, { 3.0, 56 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
        assert(text_px(rows[i].scale) == rows[i].px);
    serve_terminus = 1;
    nasked = 0;
    assert(scaled_font(2.25) == NULL && glyph_mag == 2 && glyph_w == 4 && glyph_h == 4);
    assert(nasked == 1 && strstr(asked[0], "/ter-u20b.psf"));
    uint32_t top_was = top;
    top = 0;
    glyph(0, 0, 'A', 0xFFFFFF, 0);
    top = top_was;
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++)
            assert(shadow[y * width + x] == ((x < 2) == (y < 2) ? 0xFFFFFFu : 0u));
    nasked = 0;
    assert(scaled_font(0.65) == NULL && glyph_mag == 1 && glyph_w == 2 && glyph_h == 2);
    assert(nasked == 2 && strstr(asked[0], "/ter-u12b.psf") && strstr(asked[1], "/ter-u12n.psf"));
    serve_terminus = 0;
    scale_lines = 0;
    saved = shadow;
    assert(screen_extended(1) == 1 && screen_is_extended() && screen_output_count() == 2);
    assert(screen_output_current() == 0);
    extended_conf = 1;
    assert(screen_rescale() == 1);
    struct screen_output info[2];
    assert(screen_output_get(0, &info[0]) == 0 && screen_output_get(1, &info[1]) == 0);
    assert(!strcmp(info[0].name, "eDP-1") && info[0].pixel_width == 8 && info[0].pixel_height == 6);
    assert(info[0].x == 0 && info[0].y == 0 && info[0].width == 8 && info[0].height == 6 && info[0].scale == 1);
    assert(info[1].x == 8 && info[1].y == 0 && info[1].width == 6 && info[1].height == 5 && info[1].scale == 2);
    assert(screen_output_select(0) == 0);
    screen_fill_px(0, 0, 8, 6, 0x123456);
    assert(screen_output_select(1) == 0 && screen_output_current() == 1 && screen_cols() == info[1].cols);
    screen_fill_px(0, 0, 12, 10, 0x654321);
    screen_flush();
    assert(*(uint32_t *)outputs[0].map == 0x123456 && *(uint32_t *)outputs[1].map == 0x654321);
    assert(outputs[0].surface != outputs[1].surface && outputs[0].surface->pixels != outputs[1].surface->pixels);
    assert(screen_output_select(0) == 1);
    dirties = 0;
    screen_fill_px(2, 2, 1, 1, 0xAABBCC);
    screen_flush();
    assert(dirties == 1 && clips[0].x1 == 2 && clips[0].x2 == 3 && clips[0].y1 == 2 && clips[0].y2 == 3);
    assert(*(uint32_t *)(outputs[0].map + outputs[0].pitch * 2 + 8) == 0xAABBCC);
    assert(*(uint32_t *)outputs[1].map == 0x654321);
    assert(screen_output_select(1) == 0);
    int local_x, local_y;
    assert(screen_output_at(9, 2, &local_x, &local_y) == 1 && local_x == 2 && local_y == 4);
    assert(screen_output_at(13, 5, NULL, NULL) == -1);
    int projected_x = 13, projected_y = 5;
    screen_output_project(&projected_x, &projected_y);
    assert(projected_x == 13 && projected_y == 4);
    positioned = 1;
    assert(screen_rescale() == 1 && screen_output_get(0, &info[0]) == 0 && screen_output_get(1, &info[1]) == 0);
    assert(info[0].x == -8 && info[1].x == 3 && info[1].y == 2);
    assert(screen_output_at(0, 3, NULL, NULL) == -1);
    projected_x = 0; projected_y = 3;
    screen_output_project(&projected_x, &projected_y);
    assert(projected_x == -1 && projected_y == 3);
    positioned = 0;
    assert(screen_rescale() == 1);
    puts("PASS extended outputs have distinct native canvases, per-output scale and gap-safe signed coordinates");
    uint32_t *first_pixels = outputs[0].surface->pixels, *second_pixels = outputs[1].surface->pixels;
    screen_cursor_move(1, 1);
    screen_cursor_visible(1);
    assert(outputs[0].soft_cursor && outputs[0].cursor_at.x1 != outputs[0].cursor_at.x2);
    assert(outputs[1].cursor_at.x1 == outputs[1].cursor_at.x2);
    screen_cursor_move(9, 1);
    assert(outputs[0].cursor_at.x1 == outputs[0].cursor_at.x2);
    assert(outputs[1].cursor_at.x1 != outputs[1].cursor_at.x2);
    assert(*(uint32_t *)outputs[0].map == 0x123456);
    screen_cursor_visible(0);
    assert(outputs[1].cursor_at.x1 == outputs[1].cursor_at.x2);
    screen_release();
    assert(screen_take(8) && noutputs == 2 && outputs[0].surface->pixels == first_pixels &&
           outputs[1].surface->pixels == second_pixels);
    assert(*(uint32_t *)outputs[0].map == 0x123456 && *(uint32_t *)outputs[1].map == 0x654321);
    screen_release();
    single_output = 1;
    assert(screen_take(8) && screen_output_count() == 1 && outputs[0].surface->pixels == first_pixels);
    screen_release();
    single_output = 0;
    assert(screen_take(8) && screen_output_count() == 2 && outputs[1].surface->pixels == second_pixels);
    screen_release();
    /* The second output's canvas cannot be made (a new scale, and no font
     * for it): that output is dropped, never shown without a canvas. */
    external_scale = 3.0;
    snprintf(font_fallback, sizeof font_fallback, "%s", "/nonexistent/font.psf");
    assert(screen_take(8) && screen_output_count() == 1 && !strcmp(outputs[0].name, "eDP-1"));
    assert(outputs[0].surface && outputs[0].surface->pixels == first_pixels);
    assert(screen_output_get(1, &info[1]) == -1 && screen_output_select(1) == -1);
    assert(screen_output_select(0) == 0 && screen_output_current() == 0);
    screen_fill_px(0, 0, 1, 1, 0x112233);
    screen_flush();
    assert(*(uint32_t *)outputs[0].map == 0x112233);
    assert(screen_output_at(9, 1, NULL, NULL) == -1);
    projected_x = 9; projected_y = 1;
    screen_output_project(&projected_x, &projected_y);
    assert(projected_x == 7 && projected_y == 1);
    screen_release();
    /* No output's canvas can be made: the take fails and leaves no output. */
    internal_scale = 1.5;
    assert(!screen_take(8) && screen_output_count() == 0 && !outputs && card == -1);
    assert(screen_output_select(0) == -1 && screen_output_current() == -1);
    screen_fill_px(0, 0, 1, 1, 0x445566);
    screen_flush();
    internal_scale = 1.0;
    external_scale = 2.0;
    snprintf(font_fallback, sizeof font_fallback, "%s", test_font);
    assert(screen_take(8) && screen_output_count() == 2 && outputs[0].surface->pixels == first_pixels &&
           outputs[1].surface->pixels == second_pixels);
    screen_release();
    puts("PASS extended outputs whose canvas cannot be made are dropped, never selected without one");
    assert(screen_extended(0) == 1 && !screen_is_extended() && shadow == saved);
    assert(screen_output_current() == -1);
    extended_conf = 0;
    while (surfaces) {
        struct output_surface *next = surfaces->next;
        free(surfaces->pixels); free(surfaces->font); free(surfaces);
        surfaces = next;
    }
    puts("PASS extended cursor belongs to one output and canvases survive lease release and hotplug");
    free(shadow);
    shadow = NULL;
    unlink(test_font);
}

int main(void)
{
    test_copy();
    test_matching();
    test_lifecycle();
    puts("screen mirror: copy/fit, maximum CRTC matching, lease fallback and cleanup passed");
    return 0;
}
