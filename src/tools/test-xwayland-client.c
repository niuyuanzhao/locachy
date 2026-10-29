/* A plain X11 client for test-xwayland.sh, without a toolkit or a Wayland
 * fallback. On DISPLAY:
 *   test-xwayland-client [--hold]   a window (and, with --hold, keep it)
 *   test-xwayland-client copy PREFIX [DELAY_MS]
 *                                   a window that owns CLIPBOARD and PRIMARY with
 *                                   PREFIX-1 now, PREFIX-2 on SIGUSR1, ...,
 *                                   answering conversions (the text after
 *                                   DELAY_MS) until it is ended
 *   test-xwayland-client paste      a window that prints CLIPBOARD as UTF8_STRING
 *                                   ("pasted TEXT"), or fails after 4 seconds
 * Every line it prints is flushed at once. */
#include <xcb/xcb.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static xcb_connection_t *c;
static xcb_window_t window;
static volatile sig_atomic_t want_copy;

static void on_usr1(int sig)
{
    (void)sig;
    want_copy = 1;
}

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* The next event, or NULL after ms milliseconds or a signal. */
static xcb_generic_event_t *next_event(int ms)
{
    double until = now() + ms / 1000.0;
    for (;;) {
        xcb_generic_event_t *e = xcb_poll_for_event(c);
        if (e || xcb_connection_has_error(c)) return e;
        int left = (int)((until - now()) * 1000);
        if (left <= 0) return NULL;
        struct pollfd p = { .fd = xcb_get_file_descriptor(c), .events = POLLIN };
        if (poll(&p, 1, left) < 0 && errno == EINTR) return NULL;
    }
}

static char text[256];

static void own(xcb_atom_t clipboard, const char *prefix)
{
    static int copies;
    snprintf(text, sizeof text, "%s-%d", prefix, ++copies);
    xcb_set_selection_owner(c, window, clipboard, XCB_CURRENT_TIME);
    xcb_set_selection_owner(c, window, XCB_ATOM_PRIMARY, XCB_CURRENT_TIME);
    xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, clipboard), NULL);
    printf(r && r->owner == window ? "copied %s\n" : "copy refused %s\n", text);
    fflush(stdout);
    free(r);
}

/* Owns PREFIX-1, then PREFIX-2 on SIGUSR1, ...; answers the text after
 * delay_ms (TARGETS at once). */
static int copy(const char *prefix, int delay_ms)
{
    xcb_atom_t clipboard = atom("CLIPBOARD"), targets = atom("TARGETS"), utf8 = atom("UTF8_STRING");
    xcb_atom_t plain = atom("text/plain;charset=utf-8"), text_atom = atom("TEXT");
    signal(SIGUSR1, on_usr1);
    /* After the window is mapped (and focused): xwm forwards a selection only
     * while an X11 window has the focus. */
    for (xcb_generic_event_t *e; (e = next_event(300)); free(e)) { }
    own(clipboard, prefix);
    for (;;) {
        if (want_copy) {
            want_copy = 0;
            own(clipboard, prefix);
        }
        xcb_generic_event_t *e = next_event(100); /* a SIGUSR1 between the check and poll */
        if (!e) {
            if (xcb_connection_has_error(c)) return 1;
            continue;
        }
        uint8_t type = e->response_type & 0x7f;
        if (type == XCB_SELECTION_REQUEST) {
            xcb_selection_request_event_t *r = (xcb_selection_request_event_t *)e;
            xcb_selection_notify_event_t n = { .response_type = XCB_SELECTION_NOTIFY, .time = r->time,
                .requestor = r->requestor, .selection = r->selection, .target = r->target,
                .property = r->property ? r->property : r->target };
            if (r->target == targets) {
                xcb_atom_t list[] = { targets, utf8, plain, text_atom, XCB_ATOM_STRING };
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, n.property, XCB_ATOM_ATOM, 32,
                    sizeof list / sizeof list[0], list);
            } else if (r->target == utf8 || r->target == plain || r->target == text_atom ||
                       r->target == XCB_ATOM_STRING) {
                if (delay_ms > 0) usleep((useconds_t)delay_ms * 1000);
                xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, n.property, r->target, 8,
                    (uint32_t)strlen(text), text);
            } else {
                n.property = XCB_ATOM_NONE;
            }
            xcb_send_event(c, 0, r->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&n);
            xcb_flush(c);
            printf("served %s\n", n.property == XCB_ATOM_NONE ? "nothing" : "a conversion");
            fflush(stdout);
        } else if (type == XCB_SELECTION_CLEAR) {
            printf("lost the selection\n");
            fflush(stdout);
        }
        free(e);
    }
}

static int paste(void)
{
    xcb_atom_t clipboard = atom("CLIPBOARD"), utf8 = atom("UTF8_STRING"), property = atom("LOCACHY_TEST_PASTE");
    for (xcb_generic_event_t *e; (e = next_event(300)); free(e)) { }
    xcb_convert_selection(c, window, clipboard, utf8, property, XCB_CURRENT_TIME);
    xcb_flush(c);
    double until = now() + 4;
    while (now() < until) {
        xcb_generic_event_t *e = next_event((int)((until - now()) * 1000) + 1);
        if (!e) continue;
        if ((e->response_type & 0x7f) == XCB_SELECTION_NOTIFY) {
            xcb_selection_notify_event_t *n = (xcb_selection_notify_event_t *)e;
            int ok = n->property != XCB_ATOM_NONE;
            free(e);
            if (!ok) break;
            xcb_get_property_reply_t *r = xcb_get_property_reply(c,
                xcb_get_property(c, 1, window, property, XCB_GET_PROPERTY_TYPE_ANY, 0, 1 << 20), NULL);
            if (!r) break;
            printf("pasted %.*s\n", xcb_get_property_value_length(r), (char *)xcb_get_property_value(r));
            fflush(stdout);
            free(r);
            return 0;
        }
        free(e);
    }
    printf("paste failed\n");
    fflush(stdout);
    return 1;
}

int main(int argc, char **argv)
{
    int screen_index;
    c = xcb_connect(NULL, &screen_index);
    int error = xcb_connection_has_error(c);
    if (error) {
        fprintf(stderr, "X11 connection failed: %d (DISPLAY=%s)\n", error,
            getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
        xcb_disconnect(c);
        return 1;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int n = 0; n < screen_index; n++) xcb_screen_next(&it);
    xcb_screen_t *s = it.data;
    window = xcb_generate_id(c);
    uint32_t values[] = { 0x35a66f, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                                    XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_FOCUS_CHANGE };
    xcb_void_cookie_t cookie = xcb_create_window_checked(c, XCB_COPY_FROM_PARENT, window, s->root,
        0, 0, 400, 240, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    xcb_generic_error_t *request_error = xcb_request_check(c, cookie);
    if (request_error) {
        fprintf(stderr, "X11 create window failed: %d\n", request_error->error_code);
        free(request_error);
        xcb_disconnect(c);
        return 1;
    }
    const char wmclass[] = "locachy-x11-test\0locachy-x11-test\0";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING,
        8, sizeof wmclass - 1, wmclass);
    const char *title = "LOCachy Xwayland test";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING,
        8, (uint32_t)strlen(title), title);
    /* WM_HINTS: flags InputHint, input True (ICCCM): it takes the focus. */
    uint32_t hints[9] = { 1, 1 };
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS,
        32, 9, hints);
    xcb_map_window(c, window);
    xcb_flush(c);
    printf("connected %s uid=%u window=%u\n", getenv("DISPLAY"), (unsigned)getuid(), window);
    fflush(stdout);
    int status = 0;
    if (argc > 2 && strcmp(argv[1], "copy") == 0) {
        status = copy(argv[2], argc > 3 ? atoi(argv[3]) : 0);
    } else if (argc > 1 && strcmp(argv[1], "paste") == 0) {
        status = paste();
    } else if (argc > 1 && strcmp(argv[1], "--hold") == 0) {
        xcb_generic_event_t *event;
        while ((event = xcb_wait_for_event(c))) free(event);
    }
    xcb_destroy_window(c, window);
    xcb_disconnect(c);
    return status;
}
