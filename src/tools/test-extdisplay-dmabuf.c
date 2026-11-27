/*
 * Host-side unit tests for tear-free extended-display presentation:
 *   1. Double-buffered SHM page-flip path in screen.c (screen_present_shm_flip):
 *      - Lazily creates a second dumb FB and never writes into the currently
 *        scanned-out front buffer while flipping.
 *      - Uses DRM_IOCTL_MODE_PAGE_FLIP with DRM_MODE_PAGE_FLIP_EVENT (never ASYNC),
 *        enforces single in-flight flip (EBUSY), swaps front/back on
 *        DRM_EVENT_FLIP_COMPLETE, and cleans up both buffers on screen_release().
 *      - Falls back cleanly to SETCRTC if PAGE_FLIP returns ENOTSUP.
 *   2. DMA-BUF slot cache and page-flip synchronization in screen.c:
 *      - Rejects overlapping flips with EBUSY until screen_flip_complete().
 *      - Caches FBs per (generation, slot) and returns ENOENT when invalidated.
 *      - Retires non-cached FBs only on FLIP_COMPLETE.
 *   3. FFmpeg libswscale VUYX -> RGB32 render_frame() in extdisplay-decode.c:
 *      - Exact 1:1 per-pixel conversion with no horizontal chroma shift.
 *
 * Build on Linux:
 *   gcc -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -DCONFIG_EXTDISPLAY_FFMPEG \
 *       -DEXTDISPLAY_DECODE_TEST -ffunction-sections -fdata-sections \
 *       -Isrc/base/include -Isrc/base -Isrc/progs \
 *       $(pkg-config --cflags libavcodec libavutil libswscale json-c) \
 *       src/tools/test-extdisplay-dmabuf.c src/progs/extdisplay-decode.c \
 *       src/progs/extdisplay-protocol.c src/base/key-action.c src/base/help.c \
 *       $(pkg-config --libs libavcodec libavutil libswscale json-c) -lm \
 *       -Wl,--gc-sections -o /tmp/test-extdisplay-dmabuf
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
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
static ssize_t mock_read(int fd, void *buf, size_t count);
static int mock_fcntl(int fd, int cmd, ...);

#define ioctl mock_ioctl
#define mmap mock_mmap
#define munmap mock_munmap
#define close mock_close
#define usleep mock_usleep
#define open mock_open
#define read mock_read
#define fcntl mock_fcntl
#include "../base/screen.c"
#undef ioctl
#undef mmap
#undef munmap
#undef close
#undef usleep
#undef open
#undef read
#undef fcntl

static char test_font[] = "/tmp/test-extdisplay-dmabuf-font-XXXXXX";
static int font_fd = -1;
static int buffers, fbs, mappings, closes;
static uint32_t next_fb_id = 1000, next_handle = 1;
static uint32_t active_crtc_fb, last_flip_fb, last_flip_flags;
static uint64_t last_flip_user_data;
static int flip_calls, setcrtc_calls, addfb2_calls, rmfb_calls;
static int flip_fail_errno;
static int event_ready;
static struct drm_event_vblank pending_event;

static int mock_open(const char *path, int flags, ...)
{
    if (strcmp(path, test_font) == 0) return font_fd = open(path, flags);
    errno = ENOENT;
    return -1;
}

static int mock_fcntl(int fd, int cmd, ...)
{
    if (fd == 8) {
        if (cmd == F_GETFL) return O_RDWR | O_NONBLOCK;
        return 0;
    }
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    return fcntl(fd, cmd, arg);
}

static ssize_t mock_read(int fd, void *buf, size_t count)
{
    if (fd == 8) {
        if (!event_ready) {
            errno = EAGAIN;
            return -1;
        }
        assert(count >= sizeof pending_event);
        memcpy(buf, &pending_event, sizeof pending_event);
        event_ready = 0;
        return (ssize_t)sizeof pending_event;
    }
    return read(fd, buf, count);
}

static int mock_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 8);
    if (request == DRM_IOCTL_SET_MASTER || request == DRM_IOCTL_DROP_MASTER) return 0;
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == DRM_IOCTL_MODE_GETRESOURCES) {
        struct drm_mode_card_res *r = arg;
        if (r->count_connectors >= 1) *(uint32_t *)(uintptr_t)r->connector_id_ptr = 101;
        if (r->count_crtcs >= 1) *(uint32_t *)(uintptr_t)r->crtc_id_ptr = 201;
        r->count_connectors = 1;
        r->count_crtcs = 1;
    } else if (request == DRM_IOCTL_MODE_GETCONNECTOR) {
        struct drm_mode_get_connector *c = arg;
        c->connection = 1;
        c->connector_type = DRM_MODE_CONNECTOR_eDP;
        c->connector_type_id = 1;
        c->encoder_id = 301;
        if (c->count_modes >= 1) {
            struct drm_mode_modeinfo m = { .hdisplay = 16, .vdisplay = 8, .vrefresh = 60,
                                           .type = DRM_MODE_TYPE_PREFERRED };
            memcpy((void *)(uintptr_t)c->modes_ptr, &m, sizeof m);
        }
        if (c->count_encoders >= 1) *(uint32_t *)(uintptr_t)c->encoders_ptr = 301;
        c->count_modes = 1;
        c->count_encoders = 1;
    } else if (request == DRM_IOCTL_MODE_GETENCODER) {
        struct drm_mode_get_encoder *e = arg;
        e->possible_crtcs = 1;
        e->crtc_id = 201;
    } else if (request == DRM_IOCTL_MODE_GETCRTC) {
        struct drm_mode_crtc *c = arg;
        c->mode_valid = 0;
    } else if (request == DRM_IOCTL_MODE_CREATE_DUMB) {
        struct drm_mode_create_dumb *d = arg;
        d->pitch = (d->width * 4u + 63u) & ~63u;
        d->size = (uint64_t)d->pitch * d->height;
        d->handle = next_handle++;
        buffers++;
    } else if (request == DRM_IOCTL_MODE_ADDFB) {
        struct drm_mode_fb_cmd *f = arg;
        f->fb_id = next_fb_id++;
        fbs++;
    } else if (request == DRM_IOCTL_MODE_ADDFB2) {
        struct drm_mode_fb_cmd2 *f = arg;
        assert(f->width == 16 && f->height == 8);
        f->fb_id = next_fb_id++;
        fbs++;
        addfb2_calls++;
    } else if (request == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
        struct drm_prime_handle *p = arg;
        assert((int)p->fd >= 0);
        p->handle = next_handle++;
    } else if (request == DRM_IOCTL_GEM_CLOSE) {
        /* PRIME handle closed after ADDFB2 */
    } else if (request == DRM_IOCTL_MODE_MAP_DUMB) {
        struct drm_mode_map_dumb *m = arg;
        m->offset = m->handle * 4096u;
    } else if (request == DRM_IOCTL_MODE_RMFB) {
        assert(fbs > 0);
        fbs--;
        rmfb_calls++;
    } else if (request == DRM_IOCTL_MODE_DESTROY_DUMB) {
        assert(buffers > 0);
        buffers--;
    } else if (request == DRM_IOCTL_MODE_SETCRTC) {
        struct drm_mode_crtc *s = arg;
        assert(s->fb_id != 0);
        active_crtc_fb = s->fb_id;
        setcrtc_calls++;
    } else if (request == DRM_IOCTL_MODE_PAGE_FLIP) {
        struct drm_mode_crtc_page_flip *fl = arg;
        flip_calls++;
        /* Must be vblank-synchronized with EVENT, never ASYNC. */
        assert((fl->flags & DRM_MODE_PAGE_FLIP_EVENT) != 0);
        assert((fl->flags & DRM_MODE_PAGE_FLIP_ASYNC) == 0);
        if (flip_fail_errno) {
            errno = flip_fail_errno;
            return -1;
        }
        last_flip_fb = fl->fb_id;
        last_flip_flags = fl->flags;
        last_flip_user_data = fl->user_data;
    } else if (request == DRM_IOCTL_MODE_DIRTYFB) {
        /* no-op */
    } else {
        assert(0 && "unexpected DRM ioctl");
    }
    return 0;
}

static void *mock_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)addr; (void)prot; (void)flags; (void)off;
    assert(fd == 8);
    void *p = calloc(1, len);
    assert(p != NULL);
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
    assert(fd == 8);
    closes++;
    return 0;
}

static int mock_usleep(useconds_t usec) { (void)usec; return 0; }

void say(const char *fmt, ...) { (void)fmt; }
int control_fd(void) { return -1; }
void control_wait_take(void) { abort(); }
void base_language_reload(void) {}
int monitor_profiles_select_sysfs(void) { return 0; }
int monitor_internal(const char *name) { return strncmp(name, "eDP", 3) == 0; }
int monitor_conf(const char *name, struct monitor_conf *out)
{
    (void)name;
    memset(out, 0, sizeof *out);
    return 0;
}
int panel_containers(const char *name, unsigned long long *ram)
{
    (void)name;
    if (ram) *ram = 0;
    return 0;
}
void panel_draw(uint32_t *px, int w, int h, int stride, const char *title, const char *sub,
                const char *ime, int reserve, int *mid_x0, int *mid_x1)
{
    (void)px; (void)w; (void)h; (void)stride; (void)title; (void)sub;
    (void)ime; (void)reserve; (void)mid_x0; (void)mid_x1;
    abort();
}

static void write_test_font(void)
{
    uint32_t h[8] = { PSF2_MAGIC, 0, 32, 0, 128, 2, 2, 2 };
    uint8_t bits[256] = { 0 };
    int fd = mkstemp(test_font);
    assert(fd >= 0 && write(fd, h, sizeof h) == sizeof h);
    assert(write(fd, bits, sizeof bits) == sizeof bits && close(fd) == 0);
    snprintf(font_fallback, sizeof font_fallback, "%s", test_font);
}

static void queue_flip_event(uint64_t user_data)
{
    memset(&pending_event, 0, sizeof pending_event);
    pending_event.base.type = DRM_EVENT_FLIP_COMPLETE;
    pending_event.base.length = sizeof pending_event;
    pending_event.user_data = user_data;
    active_crtc_fb = (uint32_t)user_data;
    event_ready = 1;
}

static void test_shm_double_buffer(void)
{
    write_test_font();
    assert(screen_take(8) && noutputs == 1);
    /* Ordinary take creates only 1 dumb buffer (no overhead for TUIs). */
    assert(buffers == 1 && fbs == 1 && mappings == 1);
    assert(outputs[0].back_map == NULL && outputs[0].back_fb_id == 0);
    uint32_t fb0 = outputs[0].fb_id;
    uint8_t *map0 = outputs[0].map;
    memset(map0, 0x11, (size_t)outputs[0].map_size);

    uint32_t frame1[16 * 8], frame2[16 * 8];
    for (int i = 0; i < 16 * 8; i++) {
        frame1[i] = 0x00AA0000u | (uint32_t)i;
        frame2[i] = 0x0000BB00u | (uint32_t)i;
    }

    /* Frame 1 writes into the lazily created back buffer, leaving front map0 untouched! */
    flip_calls = 0;
    assert(screen_present_shm_flip(frame1, 16, 8) == 1);
    assert(buffers == 2 && fbs == 2 && mappings == 2);
    assert(flip_calls == 1 && last_flip_fb == outputs[0].back_fb_id);
    assert(screen_flip_fd() == 8);
    assert(*(uint32_t *)map0 == 0x11111111u); /* live front buffer untouched */
    assert(*(uint32_t *)outputs[0].back_map == frame1[0]);

    /* A second present while flip is in flight is refused with EBUSY (never overwrites). */
    errno = 0;
    assert(screen_present_shm_flip(frame2, 16, 8) == -1 && errno == EBUSY);
    assert(*(uint32_t *)outputs[0].back_map == frame1[0]);

    /* Complete flip 1: front/back swap so map0 becomes the new back buffer. */
    uint32_t fb1 = outputs[0].back_fb_id;
    uint8_t *map1 = outputs[0].back_map;
    queue_flip_event(last_flip_user_data);
    assert(screen_flip_complete() == 1);
    assert(screen_flip_fd() == -1);
    assert(outputs[0].fb_id == fb1 && outputs[0].map == map1);
    assert(outputs[0].back_fb_id == fb0 && outputs[0].back_map == map0);

    /* Frame 2 writes into map0 (now off-screen back buffer), leaving map1 untouched! */
    assert(screen_present_shm_flip(frame2, 16, 8) == 1);
    assert(flip_calls == 2 && last_flip_fb == fb0);
    assert(*(uint32_t *)map1 == frame1[0]); /* live front buffer untouched */
    assert(*(uint32_t *)map0 == frame2[0]); /* back buffer has frame 2 */

    /* Complete flip 2: front returns to fb0/map0. */
    queue_flip_event(last_flip_user_data);
    assert(screen_flip_complete() == 1);
    assert(outputs[0].fb_id == fb0 && outputs[0].map == map0);

    /* A TUI redraw (screen_flush) on a double-buffered output draws the whole
     * shadow into the back buffer (fb1/map1) and flips to it: the front (map0,
     * scanned out) is never written. */
    for (int i = 0; i < 16 * 8; i++) shadow[i] = 0x00CC0000u | (uint32_t)i;
    touch(0, 0, 16, 8);
    flip_calls = 0;
    uint32_t front_before = *(uint32_t *)map0;
    screen_flush();
    assert(flip_calls == 1 && last_flip_fb == fb1);
    assert(*(uint32_t *)map0 == front_before);
    assert(*(uint32_t *)map1 == shadow[0]);
    assert(outputs[0].shm_flip_pending == 1);

    /* A second redraw while that flip has no vblank: the target is settled
     * synchronously (SETCRTC to fb1, which becomes the front) and the new frame
     * goes into the other buffer, so bookkeeping always matches the CRTC. */
    touch(0, 0, 16, 8);
    int prev_setcrtc = setcrtc_calls;
    screen_flush();
    assert(setcrtc_calls == prev_setcrtc + 1 && active_crtc_fb == fb1);
    assert(outputs[0].fb_id == fb1 && outputs[0].map == map1);
    assert(flip_calls == 2 && last_flip_fb == fb0);
    queue_flip_event(last_flip_user_data);
    assert(screen_flip_complete() == 1 && outputs[0].fb_id == fb0);

    /* If PAGE_FLIP returns ENOTSUP, falls back to SETCRTC and still swaps front/back. */
    flip_fail_errno = ENOTSUP;
    int prev_sets = setcrtc_calls;
    assert(screen_present_shm_flip(frame1, 16, 8) == 0);
    assert(setcrtc_calls == prev_sets + 1 && active_crtc_fb == fb1);
    assert(outputs[0].fb_id == fb1 && outputs[0].shm_flip_broken == 1);
    flip_fail_errno = 0;

    screen_release();
    assert(buffers == 0 && fbs == 0 && mappings == 0);
    free(shadow);
    shadow = NULL;
    drop_presented(&presented, &presented_w, &presented_h, &presented_valid);
    unlink(test_font);
    puts("PASS: SHM double-buffered PAGE_FLIP never writes into live scanout buffer");
}

static void test_dmabuf_flip_sync(void)
{
    snprintf(test_font, sizeof test_font, "%s", "/tmp/test-extdisplay-dmabuf-font-XXXXXX");
    write_test_font();
    assert(screen_take(8) && noutputs == 1);
    addfb2_calls = 0;
    flip_calls = 0;

    struct screen_dmabuf_planes p0 = {
        .fourcc = DRM_FORMAT_XRGB8888, .num_planes = 1, .slot = 0, .generation = 1,
        .pitches = { 64 }, .offsets = { 0 }, .modifiers = { DRM_FORMAT_MOD_LINEAR },
        .fds = { 42, -1, -1, -1 },
    };
    struct screen_dmabuf_planes p1 = p0;
    p1.slot = 1;

    /* First frame imports slot 0 and schedules PAGE_FLIP with EVENT. */
    assert(screen_present_dmabuf_planes(&p0, 16, 8) == 1);
    assert(addfb2_calls == 1 && flip_calls == 1 && screen_flip_fd() == 8);
    uint32_t slot0_fb = last_flip_fb;

    /* Overlapping present while flip 0 is in flight returns EBUSY. */
    errno = 0;
    assert(screen_present_dmabuf_planes(&p1, 16, 8) == -1 && errno == EBUSY);

    /* Stale/mismatched user_data event does not falsely complete slot 0's flip. */
    queue_flip_event(999999u);
    assert(screen_flip_complete() == 0 && screen_flip_fd() == 8);

    /* Matching FLIP_COMPLETE finishes flip 0 and moves slot 0 to active scanout. */
    queue_flip_event((uint64_t)slot0_fb);
    assert(screen_flip_complete() == 1 && screen_flip_fd() == -1);
    assert(outputs[0].dmabuf_fb_id == slot0_fb && outputs[0].dmabuf_active_slot == 0);

    /* Slot 1 imports and flips; cached slot 0 re-presents with num_planes==0 and no ADDFB2. */
    assert(screen_present_dmabuf_planes(&p1, 16, 8) == 1);
    assert(addfb2_calls == 2);
    queue_flip_event(last_flip_user_data);
    assert(screen_flip_complete() == 1);

    struct screen_dmabuf_planes cached0 = { .slot = 0, .generation = 1, .num_planes = 0 };
    assert(screen_present_dmabuf_planes(&cached0, 16, 8) == 1);
    assert(addfb2_calls == 2 && last_flip_fb == slot0_fb);
    queue_flip_event((uint64_t)slot0_fb);
    assert(screen_flip_complete() == 1);

    /* Transitioning from dmabuf to SHM flip cleanly clears cached dmabuf FBs. */
    uint32_t frame[16 * 8];
    memset(frame, 0, sizeof frame);
    int prev_rmfb = rmfb_calls;
    assert(screen_present_shm_flip(frame, 16, 8) == 0);
    assert(rmfb_calls == prev_rmfb + 2);
    assert(outputs[0].dmabuf_fb_id == 0 && outputs[0].dmabuf_cache_fb[0] == 0);

    /* Cached lookup after cache clear returns ENOENT so frontend triggers REFRESH. */
    errno = 0;
    assert(screen_present_dmabuf_planes(&cached0, 16, 8) == -1 && errno == ENOENT);

    /* screen_flip_reset clears a stuck pending flip without deadlocking. */
    assert(screen_present_dmabuf_planes(&p0, 16, 8) == 1);
    assert(screen_flip_fd() == 8);
    screen_flip_reset();
    assert(screen_flip_fd() == -1);

    screen_release();
    assert(buffers == 0 && fbs == 0 && mappings == 0);
    free(shadow);
    shadow = NULL;
    drop_presented(&presented, &presented_w, &presented_h, &presented_valid);
    unlink(test_font);
    puts("PASS: DMA-BUF PAGE_FLIP sync, slot cache, user_data match, and dmabuf->SHM transition");
}

#ifdef EXTDISPLAY_DECODE_TEST
/* From progs/extdisplay-decode.c (built with -DEXTDISPLAY_DECODE_TEST). */
int extdisplay_render_vuyx_for_test(const uint8_t *base, int linesize, int full, int bt709,
                                    uint32_t *dst, int w, int h);

/* One 2560-pixel row: a white and a saturated red pixel at x=1280 and x=2550,
 * black elsewhere. Packed VUYX bytes are [V, U, Y, X] per pixel. FFmpeg's
 * libswscale with SWS_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND must
 * place every output pixel 1:1 with no chroma bleed or horizontal shift. */
static void test_vuyx_exact_placement(void)
{
    enum { W = 2560, H = 2, STRIDE = W * 4 + 64 };
    static uint8_t src[H * STRIDE];
    static uint32_t out[W * H];
    memset(src, 0, sizeof src);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t *p = src + (size_t)y * STRIDE + (size_t)x * 4;
            p[0] = 128; /* V */
            p[1] = 128; /* U */
            p[2] = 16;  /* Y: black, limited range */
            p[3] = 0;   /* X */
        }
    }
    /* white at 1280 (Y=235), saturated red at 2550 (Y=82, U=90, V=240) on both rows. */
    for (int y = 0; y < H; y++) {
        uint8_t *white = src + (size_t)y * STRIDE + (size_t)1280 * 4;
        white[2] = 235;
        uint8_t *red = src + (size_t)y * STRIDE + (size_t)2550 * 4;
        red[0] = 240; red[1] = 90; red[2] = 82;
    }
    assert(extdisplay_render_vuyx_for_test(src, STRIDE, 0, 0, out, W, H) == 0);
    for (int y = 0; y < H; y++) {
        const uint32_t *row = out + (size_t)y * W;
        assert(row[1280] == 0xFFFFFFu);
        assert(row[1279] == 0 && row[1281] == 0);
        assert(row[2550] == 0xFF0100u);
        assert(row[2549] == 0 && row[2551] == 0);
        for (int x = 0; x < W; x++) {
            if (x == 1280 || x == 2550) continue;
            assert(row[x] == 0);
        }
    }
    puts("PASS: libswscale VUYX render_frame places white and red 1:1 with no chroma bleed or shift");
}
#endif

int main(void)
{
    test_shm_double_buffer();
    test_dmabuf_flip_sync();
#ifdef EXTDISPLAY_DECODE_TEST
    test_vuyx_exact_placement();
#endif
    puts("ALL PASSED");
    return 0;
}
