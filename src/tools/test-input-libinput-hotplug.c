/* libinput path-device lifecycle doubles, not virtual or physical devices.
 * Exercise rejected-before-udev retry, duplicate leases and incremental
 * remove/reconnect through the real pointer.c libinput adapter. Link libinput
 * for the event accessors (unused here); no libinput device is opened. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define CONFIG_LIBINPUT
#include "../base/pointer.c"
#include <assert.h>

static const struct libinput_interface *callbacks;
static int ready, adds, removes, closes, input_fd = -1, refs;
static struct libinput *context = (struct libinput *)(uintptr_t)1;
static struct libinput_device *device = (struct libinput_device *)(uintptr_t)2;

struct libinput *libinput_path_create_context(const struct libinput_interface *i, void *data)
{ (void)data; callbacks = i; return context; }
int libinput_get_fd(struct libinput *c) { assert(c == context); return -1; }
int libinput_dispatch(struct libinput *c) { assert(c == context); return 0; }
struct libinput_event *libinput_get_event(struct libinput *c) { assert(c == context); return NULL; }
struct libinput_device *libinput_path_add_device(struct libinput *c, const char *path)
{
    assert(c == context && input_fd < 0);
    adds++;
    input_fd = callbacks->open_restricted(path, O_RDONLY, NULL);
    assert(input_fd >= 0);
    if (!ready) {
        callbacks->close_restricted(input_fd, NULL); closes++;
        input_fd = -1;
        return NULL;
    }
    refs = 1; /* context's reference */
    return device;
}
struct libinput_device *libinput_device_ref(struct libinput_device *d)
{ assert(d == device); refs++; return d; }
struct libinput_device *libinput_device_unref(struct libinput_device *d)
{ assert(d == device && refs > 0); refs--; return refs ? d : NULL; }
void libinput_path_remove_device(struct libinput_device *d)
{
    assert(d == device && input_fd >= 0);
    removes++;
    callbacks->close_restricted(input_fd, NULL); closes++;
    input_fd = -1;
    refs--; /* context's reference */
}
struct libinput *libinput_unref(struct libinput *c)
{
    assert(c == context);
    if (input_fd >= 0) {
        callbacks->close_restricted(input_fd, NULL); closes++;
        input_fd = -1;
        refs--;
    }
    assert(!refs);
    return NULL;
}
void screen_pixels(int *w, int *h) { *w = 1000; *h = 800; }
void screen_cell_at(int x, int y, int *col, int *row) { *col = x / 10; *row = y / 20; }
void screen_cursor_move(int x, int y) { (void)x; (void)y; }
void screen_cursor_visible(int on) { (void)on; }
double screen_scale(void) { return 1; }
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{ (void)fn; (void)arg; return 0; }
int control_fd(void) { return -1; }
int control_send_fields(int fd, const char *const *fields) { (void)fd; (void)fields; abort(); }

int main(void)
{
    const char *path = "/dev/input/event5";
    int pair[2];
    assert(pipe2(pair, O_NONBLOCK | O_CLOEXEC) == 0);
    int original = dup(pair[0]);
    assert(pointer_take(original, path) < 0 && ndevices == 1 && adds == 1 && closes == 1);
    assert(devices[0].fd == original && fcntl(original, F_GETFD) >= 0);
    ready = 1; /* the processed udev event follows the raw kernel event */
    pointer_retry();
    assert(ndevices == 1 && devices[0].input == device && adds == 2 && refs == 2);
    assert(fcntl(original, F_GETFD) >= 0 && input_fd != original);
    pointer_retry();
    assert(adds == 2); /* already accepted devices are never reopened */
    assert(pointer_take(dup(pair[0]), path) == 0 && ndevices == 1 && adds == 2);
    pointer_remove(path);
    assert(!ndevices && removes == 1 && closes == 2 && !refs);
    assert(fcntl(original, F_GETFD) < 0 && errno == EBADF);
    pointer_remove(path); /* late duplicate removal is harmless */
    for (int i = 0; i < 12; i++) {
        assert(pointer_take(dup(pair[0]), path) == 0 && ndevices == 1);
        pointer_remove(path);
        assert(!ndevices && !refs && input_fd < 0);
    }
    assert(pointer_take(dup(pair[0]), path) == 0);
    int replacement[2];
    assert(pipe2(replacement, O_NONBLOCK | O_CLOEXEC) == 0);
    int before = removes;
    assert(pointer_take(dup(replacement[0]), path) == 0);
    assert(ndevices == 1 && removes == before + 1 && input_same(devices[0].fd, replacement[0]));
    pointer_release();
    assert(!ndevices && !li && !refs && input_fd < 0 && epoll_fd < 0 && timer_fd < 0);
    close(replacement[0]); close(replacement[1]);
    close(pair[0]); close(pair[1]);
    puts("PASS rejected pre-udev leases retry without stale descriptors or duplicate accepted devices");
    puts("PASS incremental libinput removal/reconnect and final release balance device refs and fd copies");
    puts("ALL PASSED");
    return 0;
}
