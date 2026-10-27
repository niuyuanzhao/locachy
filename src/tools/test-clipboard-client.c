/* A deterministic data-control source/receiver for the isolated clipboard
 * tests. No real seat input or user clipboard is accessed.
 * test-clipboard-client source [--primary] [--slow|--hang] LABEL
 * test-clipboard-client read MIME [--primary]
 * test-clipboard-client clear [--primary]
 * All payloads are generated here: text, HTML and binary (with NUL bytes).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-data-control-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_seat *seat;
static struct ext_data_control_manager_v1 *manager;
static struct ext_data_control_device_v1 *device;
static struct ext_data_control_offer_v1 *selection[2];
static int primary, slow, hang, cancelled;
static const char *label;
static int wait_pipe = -1;

static void send_data(void *data, struct ext_data_control_source_v1 *s, const char *mime, int fd)
{
    (void)data; (void)s;
    if (hang) { wait_pipe = fd; return; }
    if (slow) usleep(400000);
    char out[1024];
    size_t n;
    if (strcmp(mime, "image/png") == 0) {
        static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 1, 2, 255 };
        memcpy(out, png, sizeof png); n = sizeof png;
    } else if (strcmp(mime, "text/html") == 0) {
        n = (size_t)snprintf(out, sizeof out, "<p>%s 中文 🌍</p>", label);
    } else {
        n = (size_t)snprintf(out, sizeof out, "%s 中文 🌍\nnext line\n", label);
    }
    fprintf(stderr, "sent %s\n", mime);
    size_t off = 0;
    while (off < n) {
        ssize_t wrote = write(fd, out + off, n - off);
        if (wrote > 0) off += (size_t)wrote;
        else if (wrote < 0 && errno == EINTR) continue;
        else break;
    }
    close(fd);
}
static void cancelled_source(void *data, struct ext_data_control_source_v1 *s)
{ (void)data; (void)s; cancelled = 1; fprintf(stderr, "cancelled\n"); }
static const struct ext_data_control_source_v1_listener source_listener = { .send = send_data, .cancelled = cancelled_source };
static void mime(void *data, struct ext_data_control_offer_v1 *o, const char *name)
{ (void)data; (void)o; (void)name; }
static const struct ext_data_control_offer_v1_listener offer_listener = { .offer = mime };
static void new_offer(void *data, struct ext_data_control_device_v1 *d, struct ext_data_control_offer_v1 *o)
{ (void)data; (void)d; ext_data_control_offer_v1_add_listener(o, &offer_listener, NULL); }
static void select_offer(int which, struct ext_data_control_offer_v1 *o)
{
    if (selection[which]) ext_data_control_offer_v1_destroy(selection[which]);
    selection[which] = o;
}
static void selected(void *data, struct ext_data_control_device_v1 *d, struct ext_data_control_offer_v1 *o)
{ (void)data; (void)d; select_offer(0, o); }
static void selected_primary(void *data, struct ext_data_control_device_v1 *d, struct ext_data_control_offer_v1 *o)
{ (void)data; (void)d; select_offer(1, o); }
static void finished(void *data, struct ext_data_control_device_v1 *d)
{ (void)data; (void)d; exit(1); }
static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = new_offer, .selection = selected, .primary_selection = selected_primary, .finished = finished,
};
static void global(void *data, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version)
{
    (void)data; (void)version;
    if (strcmp(interface, wl_seat_interface.name) == 0 && !seat) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) manager = wl_registry_bind(r, name, &ext_data_control_manager_v1_interface, 1);
}
static void removed(void *data, struct wl_registry *r, uint32_t name) { (void)data; (void)r; (void)name; }
static const struct wl_registry_listener registry_listener = { global, removed };

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--primary") == 0) primary = 1;
        else if (strcmp(argv[i], "--slow") == 0) slow = 1;
        else if (strcmp(argv[i], "--hang") == 0) hang = 1;
        else label = argv[i];
    }
    display = wl_display_connect(NULL);
    if (!display) return 1;
    struct wl_registry *r = wl_display_get_registry(display);
    wl_registry_add_listener(r, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !seat || !manager) return 1;
    device = ext_data_control_manager_v1_get_data_device(manager, seat);
    ext_data_control_device_v1_add_listener(device, &device_listener, NULL);
    if (wl_display_roundtrip(display) < 0) return 1;
    if (strcmp(argv[1], "source") == 0) {
        if (!label) return 2;
        struct ext_data_control_source_v1 *s = ext_data_control_manager_v1_create_data_source(manager);
        ext_data_control_source_v1_add_listener(s, &source_listener, NULL);
        ext_data_control_source_v1_offer(s, "text/plain;charset=utf-8");
        ext_data_control_source_v1_offer(s, "text/html");
        ext_data_control_source_v1_offer(s, "image/png");
        if (primary) ext_data_control_device_v1_set_primary_selection(device, s);
        else ext_data_control_device_v1_set_selection(device, s);
        wl_display_flush(display);
        fprintf(stderr, "ready\n");
        while (wl_display_dispatch(display) >= 0) {
            if (cancelled && !primary && !slow) break;
        }
        if (wait_pipe >= 0) close(wait_pipe);
    } else if (strcmp(argv[1], "read") == 0 && label) {
        if (!selection[primary]) return 3;
        int p[2];
        if (pipe(p) < 0) return 1;
        ext_data_control_offer_v1_receive(selection[primary], label, p[1]);
        close(p[1]);
        wl_display_flush(display);
        char buf[65536];
        ssize_t n;
        while ((n = read(p[0], buf, sizeof buf)) > 0) {
            if (write(STDOUT_FILENO, buf, (size_t)n) != n) return 1;
        }
        close(p[0]);
    } else if (strcmp(argv[1], "clear") == 0) {
        if (primary) ext_data_control_device_v1_set_primary_selection(device, NULL);
        else ext_data_control_device_v1_set_selection(device, NULL);
        if (wl_display_roundtrip(display) < 0) return 1;
    } else return 2;
    wl_display_disconnect(display);
    return 0;
}
