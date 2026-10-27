/*
 * test-screenshot PREFIX: each output's picture (wlr-screencopy) as a binary
 * PPM, PREFIX-NAME.ppm (NAME: the output's, HEADLESS-1 ...; its number if the
 * compositor does not say): for the tests to look at what the host clients
 * draw on a headless labwc.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"

static struct wl_shm *shm;
static struct zwlr_screencopy_manager_v1 *copier;
static uint32_t copier_version;
static struct wl_output *outputs[8];
static char names[8][32];
static int noutputs;

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                            int32_t ph, int32_t subpixel, const char *make, const char *model,
                            int32_t transform)
{
    (void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r)
{
    (void)data; (void)o; (void)flags; (void)w; (void)h; (void)r;
}

static void output_done(void *data, struct wl_output *o)
{
    (void)data;
    (void)o;
}

static void output_scale(void *data, struct wl_output *o, int32_t factor)
{
    (void)data;
    (void)o;
    (void)factor;
}

static void output_name(void *data, struct wl_output *o, const char *name)
{
    (void)o;
    snprintf(names[(intptr_t)data], sizeof names[0], "%s", name);
}

static void output_description(void *data, struct wl_output *o, const char *description)
{
    (void)data;
    (void)o;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

struct shot {
    uint32_t format, width, height, stride, flags;
    int buffer, done, ready, failed;
};

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
                         uint32_t width, uint32_t height, uint32_t stride)
{
    struct shot *s = data;
    (void)f;
    s->format = format;
    s->width = width;
    s->height = height;
    s->stride = stride;
    s->buffer = 1;
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{
    (void)f;
    ((struct shot *)data)->flags = flags;
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c)
{
    (void)f;
    (void)a;
    (void)b;
    (void)c;
    ((struct shot *)data)->ready = 1;
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    (void)f;
    ((struct shot *)data)->failed = 1;
}

static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h)
{
    (void)data;
    (void)f;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

static void frame_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
                         uint32_t w, uint32_t h)
{
    (void)data;
    (void)f;
    (void)format;
    (void)w;
    (void)h;
}

static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    (void)f;
    ((struct shot *)data)->done = 1;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .flags = frame_flags,
    .ready = frame_ready,
    .failed = frame_failed,
    .damage = frame_damage,
    .linux_dmabuf = frame_dmabuf,
    .buffer_done = frame_buffer_done,
};

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *iface,
                   uint32_t version)
{
    (void)data;
    if (strcmp(iface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        copier_version = version < 3 ? version : 3;
        copier = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, copier_version);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && noutputs < 8) {
        snprintf(names[noutputs], sizeof names[0], "%d", noutputs);
        outputs[noutputs] = wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
        wl_output_add_listener(outputs[noutputs], &output_listener, (void *)(intptr_t)noutputs);
        noutputs++;
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static int capture(struct wl_display *display, struct wl_output *output, const char *path)
{
    struct shot s = { 0 };
    struct zwlr_screencopy_frame_v1 *frame = zwlr_screencopy_manager_v1_capture_output(copier, 0, output);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, &s);
    while (!s.failed && !(s.buffer && (copier_version < 3 || s.done))) {
        if (wl_display_dispatch(display) < 0) return -1;
    }
    if (s.failed || (s.format != WL_SHM_FORMAT_XRGB8888 && s.format != WL_SHM_FORMAT_ARGB8888 &&
                     s.format != WL_SHM_FORMAT_XBGR8888 && s.format != WL_SHM_FORMAT_ABGR8888)) {
        fprintf(stderr, "test-screenshot: no capture in a known format (%#x)\n", s.format);
        return -1;
    }
    size_t size = (size_t)s.stride * s.height;
    int fd = memfd_create("test-screenshot", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) return -1;
    uint8_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) return -1;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)s.width, (int32_t)s.height,
                                                         (int32_t)s.stride, s.format);
    zwlr_screencopy_frame_v1_copy(frame, buffer);
    while (!s.ready && !s.failed) {
        if (wl_display_dispatch(display) < 0) return -1;
    }
    FILE *out = s.ready ? fopen(path, "w") : NULL;
    if (out) {
        int bgr = s.format == WL_SHM_FORMAT_XRGB8888 || s.format == WL_SHM_FORMAT_ARGB8888;
        fprintf(out, "P6\n%u %u\n255\n", s.width, s.height);
        for (uint32_t y = 0; y < s.height; y++) {
            uint32_t row = s.flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT ? s.height - 1 - y : y;
            const uint8_t *p = px + (size_t)row * s.stride;
            for (uint32_t x = 0; x < s.width; x++, p += 4) {
                uint8_t rgb[3] = { bgr ? p[2] : p[0], p[1], bgr ? p[0] : p[2] };
                fwrite(rgb, 1, 3, out);
            }
        }
        fclose(out);
    }
    zwlr_screencopy_frame_v1_destroy(frame);
    wl_buffer_destroy(buffer);
    wl_shm_pool_destroy(pool);
    munmap(px, size);
    close(fd);
    return out ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: test-screenshot PREFIX\n");
        return 2;
    }
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); /* the outputs' names */
    if (!shm || !copier || noutputs == 0) {
        fprintf(stderr, "test-screenshot: no wl_shm, screencopy or output\n");
        return 1;
    }
    int failed = 0;
    for (int i = 0; i < noutputs; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s-%s.ppm", argv[1], names[i]);
        failed |= capture(display, outputs[i], path) < 0;
    }
    return failed;
}
