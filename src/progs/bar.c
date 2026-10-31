/*
 * progs/bar.c (/usr/local/bin/locachy-bar): the shared compositor's status
 * bar and input method, a host client of labwc started by locachy-seat.
 *
 * - Top status bar on every screen: zwlr_layer_shell_v1 (LAYER_TOP, one and
 *   a half rows of the direct screens' text at the screen's scale high, and
 *   its exclusive zone), drawn by panel.c as at the top of the
 *   framebuffer programs, with the window list in its middle (clicks:
 *   activate or bring back, minimize the focused one; middle click: close).
 *   The pointer pushed into a screen's top left corner (as KDE's) shows the
 *   overview of the windows (locachy-overview), and closes it.
 * - Bottom Rime overlay bar: zwlr_layer_shell_v1 (LAYER_OVERLAY, one or two
 *   rows of 16px at scale 1) on the focused window's screen, shown only while
 *   composing or for a 0.8 s notice (mode switch, volume).
 * - Tray: the StatusNotifierWatcher on each container's private session bus;
 *   a container with a tray item and no window is in the window list, and
 *   opening it asks the item for the program's own window (no icons or menus).
 * - Notifications: org.freedesktop.Notifications on the same buses; popups at
 *   the top right of the focused window's screen, the container marked in the
 *   window list (no icons, action buttons, sound or history).
 * - Keyboard: reads /dev/input/event* for rc.conf's keys (key_ime, the
 *   volume keys; key_home and key_lock while it holds the keyboard, read
 *   again each time the seat comes back). In the CN/EN input
 *   modes it grabs the keyboard (EVIOCGRAB), composes through base/ime.c,
 *   and types committed text and every other key into the focused window
 *   through zwp_virtual_keyboard_v1, so applications need no input method
 *   support; rc.conf's key_ime_width_cn (Ctrl+. and Shift+Space unless
 *   set) switches Chinese mode's punctuation between full width (where it
 *   starts: default.custom.yaml's table only) and half width (every key as
 *   typed). While
 *   locachy-seat has released the seat (home is on the screen) it ignores the
 *   keyboard.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <gio/gio.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "base.h"
#include "internal.h"
#include "ime.h"

#define FG_COLOR   0xFFD0D0D0u
#define OVL_BG     0xFF182848u
#define OVL_FG1    0xFFFFFFFFu
#define OVL_FG2    0xFFB8D8FFu
#define TOAST_BG   0xFF183860u

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *wl_seat_obj;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwp_virtual_keyboard_manager_v1 *vkbd_mgr;

/* The top bar: a layer surface on each screen (wl_output), made when the
 * screen appears and destroyed when it goes. labwc closes one that has no
 * screen to be on; it is asked for again a second later. */
#define MAX_BUTTONS 64 /* drawn on a bar: arrows and windows */
#define MAX_ENTRIES 256 /* in its window list, scrolled through */
struct window;
struct bar {
    uint32_t name;              /* the wl_output global */
    struct wl_output *output;
    struct wl_surface *surf;
    struct zwlr_layer_surface_v1 *lsurf;
    uint32_t w, h;              /* logical pixels; the buffer is these at the screen's scale */
    uint32_t want_h;            /* the height asked for (bar_height) */
    struct wp_fractional_scale_v1 *scale;
    struct wp_viewport *viewport;
    uint32_t scale120;          /* the screen's scale, times 120 */
    int configured, shown;      /* shown: labwc put it on a screen (wl_surface.enter) */
    double since;               /* asked for, or closed, at */
    int retries;
    struct {
        struct window *win;     /* a window, or NULL: the container tray names, */
        char tray[OPEN_NAME];   /* or (both empty) a scroll arrow: step -1 or 1 */
        int step, x0, x1;
    } buttons[MAX_BUTTONS];     /* the window list as drawn (buffer pixels) */
    int first;                  /* the window list scrolled: its first entry shown */
    struct window *focus_seen;  /* the focused window when it was last drawn */
    double wheel;               /* scrolling not yet a whole entry */
    int nbuttons;
    struct bar *next;
};
static struct bar *bars;
static struct wp_fractional_scale_manager_v1 *scale_mgr;
static struct wp_viewporter *viewporter;
static int bars_dirty; /* draw them now: a bar was configured, the windows changed */

static void buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void)data;
    wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener buf_listener = { .release = buffer_release };

static struct wl_buffer *create_shm_buffer(int w, int h, uint32_t **out_px)
{
    int stride = w * 4;
    int size = stride * h;
    int fd = memfd_create("locachy-bar-shm", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return NULL;
    }
    uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(buffer, &buf_listener, NULL);
    *out_px = px;
    return buffer;
}

/* Windows (wlr-foreign-toplevel), for the window list: in the order they
 * opened, each in the list of the screen it is on (a minimized one, of the
 * screen it was on); a click activates one (and brings it back if it was
 * minimized), or minimizes the focused one; a middle click closes it. */
#define WINDOW_OUTPUTS 4
#define MAX_WINDOW_IDS 16
struct window {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char title[128], app_id[64];
    char container[OPEN_NAME];  /* whose .desktop names its app id; "" if none */
    int activated, minimized;
    double focused_at;          /* when it last had the focus */
    struct wl_output *outputs[WINDOW_OUTPUTS], *last;
    int noutputs;
    struct window *next;
};
static struct zwlr_foreign_toplevel_manager_v1 *toplevel_mgr;
static struct window *windows;

static void win_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title)
{
    struct window *w = data;
    (void)h;
    snprintf(w->title, sizeof w->title, "%s", title);
}

static void win_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *id)
{
    struct window *w = data;
    char names[64][OPEN_NAME], ids[MAX_WINDOW_IDS][OPEN_WINDOW_ID];
    (void)h;
    snprintf(w->app_id, sizeof w->app_id, "%s", id);
    w->container[0] = '\0';
    for (int i = open_containers(names, 64) - 1; i >= 0 && !w->container[0]; i--) {
        int n = open_window_ids(names[i], ids, MAX_WINDOW_IDS);
        for (int k = 0; k < n; k++) {
            if (strcasecmp(id, ids[k]) == 0) {
                memcpy(w->container, names[i], sizeof w->container);
                break;
            }
        }
    }
}

static void win_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
    struct window *w = data;
    (void)h;
    for (int i = 0; i < w->noutputs; i++) {
        if (w->outputs[i] == o) {
            return;
        }
    }
    if (w->noutputs < WINDOW_OUTPUTS) {
        w->outputs[w->noutputs++] = o;
    }
    w->last = o;
}

static void win_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
    struct window *w = data;
    (void)h;
    for (int i = 0; i < w->noutputs; i++) {
        if (w->outputs[i] == o) {
            w->outputs[i] = w->outputs[--w->noutputs];
            break;
        }
    }
}

static void win_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *states)
{
    struct window *w = data;
    uint32_t *state;
    (void)h;
    w->activated = w->minimized = 0;
    wl_array_for_each(state, states) {
        if (*state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
            w->activated = 1;
        } else if (*state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) {
            w->minimized = 1;
        }
    }
    if (w->activated) {
        w->focused_at = uptime();
    }
}

static void win_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)data;
    (void)h;
    bars_dirty = 1;
}

static void win_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    struct window *w = data;
    for (struct window **p = &windows; *p; p = &(*p)->next) {
        if (*p == w) {
            *p = w->next;
            break;
        }
    }
    for (struct bar *b = bars; b; b = b->next) {
        b->nbuttons = 0; /* drawn anew below, without it */
        if (b->focus_seen == w) {
            b->focus_seen = NULL;
        }
    }
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    free(w);
    bars_dirty = 1;
}

static void win_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                       struct zwlr_foreign_toplevel_handle_v1 *parent)
{
    (void)data;
    (void)h;
    (void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener win_listener = {
    .title = win_title,
    .app_id = win_app_id,
    .output_enter = win_output_enter,
    .output_leave = win_output_leave,
    .state = win_state,
    .done = win_done,
    .closed = win_closed,
    .parent = win_parent,
};

static void new_window(void *data, struct zwlr_foreign_toplevel_manager_v1 *m,
                       struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)data;
    (void)m;
    struct window *w = calloc(1, sizeof *w), **tail = &windows;
    if (!w) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    w->handle = handle;
    while (*tail) {
        tail = &(*tail)->next;
    }
    *tail = w;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &win_listener, w);
}

static void toplevels_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *m)
{
    (void)data;
    zwlr_foreign_toplevel_manager_v1_destroy(m);
    toplevel_mgr = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevels_listener = {
    .toplevel = new_window,
    .finished = toplevels_finished,
};

/*
 * The tray. Each running container has a session bus of its own (locachy-app
 * starts it; its socket is TRAY_DIR/NAME/bus/bus here), on which the bar is
 * the StatusNotifierWatcher: a program that lives on in a tray (QQ, WeChat,
 * Clash) registers its item here. No icons and no menus: while its container
 * has no window, the window list has a button for it (marked when an item
 * asks for attention, as for a new message), and a click, or opening the
 * container from home, Activates its item: the program shows its own window,
 * so no second copy is started to find it.
 */
#define TRAY_DIR "/run/locachy/apps"
#define WATCHER "org.kde.StatusNotifierWatcher"
#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define MAX_TRAY_IDS 16

struct tray_item {
    char bus[128], path[256], title[128];
    int attention;
    guint watch, signals;
    struct tray_item *next;
};

struct tray {
    char name[OPEN_NAME];
    char ids[MAX_TRAY_IDS][OPEN_WINDOW_ID]; /* its windows' app ids */
    int nids, seen, connecting;
    double retry_at;
    GDBusConnection *conn;
    guint own, object;
    guint notes_own, notes_object; /* the notification server on the bus */
    int notified;                  /* marked: a notification since its window had the focus */
    struct tray_item *items;
    struct tray *next;
};
static struct tray *trays;
static struct tray host_notes; /* root's session bus, never a container's tray */
static double tray_scanned;

static const char watcher_xml[] =
    "<node><interface name='org.kde.StatusNotifierWatcher'>"
    "<method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>"
    "<method name='RegisterStatusNotifierHost'><arg type='s' direction='in'/></method>"
    "<property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "<property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "<property name='ProtocolVersion' type='i' access='read'/>"
    "<signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    "<signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    "<signal name='StatusNotifierHostRegistered'/>"
    "</interface></node>";

static void ask_again(const char *name, const char *why, int replace);
static void watch_window(const char *name);
static void notes_serve(struct tray *t);
static void notes_drop(struct tray *t);
static struct wl_surface *notes_surface(void);
static void notes_click(double y, uint32_t button);

static struct tray *tray_find(const char *name)
{
    for (struct tray *t = trays; t; t = t->next) {
        if (strcmp(t->name, name) == 0) {
            return t;
        }
    }
    return NULL;
}

static struct tray *tray_of(GDBusConnection *conn)
{
    if (!conn) return NULL;
    if (host_notes.conn == conn) return &host_notes;
    for (struct tray *t = trays; t; t = t->next) {
        if (t->conn == conn) {
            return t;
        }
    }
    return NULL;
}

/* Callbacks find an item by its names, not a pointer: it may be gone. */
struct item_key {
    char name[OPEN_NAME], bus[128], path[256];
};

static struct item_key *item_key(const struct tray *t, const struct tray_item *it)
{
    struct item_key *k = calloc(1, sizeof *k);
    if (k) {
        memcpy(k->name, t->name, sizeof k->name);
        memcpy(k->bus, it->bus, sizeof k->bus);
        memcpy(k->path, it->path, sizeof k->path);
    }
    return k;
}

static struct tray_item *item_find(const struct item_key *k, struct tray **tp)
{
    struct tray *t = k ? tray_find(k->name) : NULL;
    for (struct tray_item *it = t ? t->items : NULL; it; it = it->next) {
        if (strcmp(it->bus, k->bus) == 0 && strcmp(it->path, k->path) == 0) {
            if (tp) {
                *tp = t;
            }
            return it;
        }
    }
    return NULL;
}

/* The buttons drawn name what may be gone: drawn anew before a click. */
static void forget_buttons(void)
{
    for (struct bar *b = bars; b; b = b->next) {
        b->nbuttons = 0;
    }
    bars_dirty = 1;
}

static void item_free(struct tray *t, struct tray_item *it)
{
    if (it->watch) {
        g_bus_unwatch_name(it->watch);
    }
    if (it->signals && t->conn) {
        g_dbus_connection_signal_unsubscribe(t->conn, it->signals);
    }
    free(it);
}

static void item_props(GObject *src, GAsyncResult *res, gpointer data)
{
    struct item_key *k = data;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    struct tray_item *it = item_find(k, NULL);
    if (v && it) {
        GVariant *all = g_variant_get_child_value(v, 0);
        const char *s;
        if ((g_variant_lookup(all, "Title", "&s", &s) && s[0]) || g_variant_lookup(all, "Id", "&s", &s)) {
            snprintf(it->title, sizeof it->title, "%s", s);
        }
        if (g_variant_lookup(all, "Status", "&s", &s)) {
            it->attention = strcmp(s, "NeedsAttention") == 0;
        }
        g_variant_unref(all);
        bars_dirty = 1;
    }
    if (v) {
        g_variant_unref(v);
    }
    free(k);
}

static void item_fetch(struct tray *t, struct tray_item *it)
{
    struct item_key *k = item_key(t, it);
    if (k) {
        g_dbus_connection_call(t->conn, it->bus, it->path, "org.freedesktop.DBus.Properties", "GetAll",
                               g_variant_new("(s)", ITEM_IFACE), G_VARIANT_TYPE("(a{sv})"),
                               G_DBUS_CALL_FLAGS_NONE, 2000, NULL, item_props, k);
    }
}

/* NewTitle, NewStatus and the like: its properties read again. */
static void item_signal(GDBusConnection *conn, const gchar *sender, const gchar *path,
                        const gchar *iface, const gchar *signal, GVariant *params, gpointer data)
{
    struct tray *t;
    struct tray_item *it = item_find(data, &t);
    (void)conn;
    (void)sender;
    (void)path;
    (void)iface;
    (void)signal;
    (void)params;
    if (it) {
        item_fetch(t, it);
    }
}

static void watcher_emit(struct tray *t, const char *signal, const struct tray_item *it)
{
    char id[400];
    snprintf(id, sizeof id, "%s%s", it->bus, it->path);
    g_dbus_connection_emit_signal(t->conn, NULL, "/StatusNotifierWatcher", WATCHER, signal,
                                  g_variant_new("(s)", id), NULL);
}

static void item_gone(GDBusConnection *conn, const gchar *bus, gpointer data)
{
    struct tray *t;
    struct tray_item *it = item_find(data, &t);
    (void)conn;
    (void)bus;
    if (!it) {
        return;
    }
    for (struct tray_item **p = &t->items; *p; p = &(*p)->next) {
        if (*p == it) {
            *p = it->next;
            break;
        }
    }
    say("locachy-bar: %s: a tray item went away", t->name);
    watcher_emit(t, "StatusNotifierItemUnregistered", it);
    item_free(t, it);
    forget_buttons();
}

static void watcher_call(GDBusConnection *conn, const gchar *sender, const gchar *path,
                         const gchar *iface, const gchar *method, GVariant *params,
                         GDBusMethodInvocation *inv, gpointer data)
{
    struct tray *t = tray_of(conn);
    (void)path;
    (void)iface;
    (void)data;
    if (t && strcmp(method, "RegisterStatusNotifierItem") == 0) {
        const char *service;
        struct item_key k;
        g_variant_get(params, "(&s)", &service);
        memcpy(k.name, t->name, sizeof k.name);
        /* A bus name (its item at /StatusNotifierItem), or a path on the caller's. */
        snprintf(k.bus, sizeof k.bus, "%s", service[0] == '/' ? sender : service);
        snprintf(k.path, sizeof k.path, "%s", service[0] == '/' ? service : "/StatusNotifierItem");
        struct tray_item *it = item_find(&k, NULL), **tail = &t->items;
        if (!it && (it = calloc(1, sizeof *it)) != NULL) {
            memcpy(it->bus, k.bus, sizeof it->bus);
            memcpy(it->path, k.path, sizeof it->path);
            snprintf(it->title, sizeof it->title, "%s", t->name);
            while (*tail) {
                tail = &(*tail)->next;
            }
            *tail = it;
            it->watch = g_bus_watch_name_on_connection(conn, it->bus, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                       NULL, item_gone, item_key(t, it), free);
            it->signals = g_dbus_connection_signal_subscribe(conn, it->bus, ITEM_IFACE, NULL, it->path,
                                                             NULL, G_DBUS_SIGNAL_FLAGS_NONE, item_signal,
                                                             item_key(t, it), free);
            item_fetch(t, it);
            watcher_emit(t, "StatusNotifierItemRegistered", it);
            say("locachy-bar: %s: tray item %s%s", t->name, it->bus, it->path);
            forget_buttons();
        }
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (strcmp(method, "RegisterStatusNotifierHost") == 0) {
        g_dbus_method_invocation_return_value(inv, NULL);
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "no method %s", method);
    }
}

static GVariant *watcher_get(GDBusConnection *conn, const gchar *sender, const gchar *path,
                             const gchar *iface, const gchar *prop, GError **error, gpointer data)
{
    struct tray *t = tray_of(conn);
    (void)sender;
    (void)path;
    (void)iface;
    (void)data;
    if (strcmp(prop, "RegisteredStatusNotifierItems") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        for (struct tray_item *it = t ? t->items : NULL; it; it = it->next) {
            char id[400];
            snprintf(id, sizeof id, "%s%s", it->bus, it->path);
            g_variant_builder_add(&b, "s", id);
        }
        return g_variant_builder_end(&b);
    }
    if (strcmp(prop, "IsStatusNotifierHostRegistered") == 0) {
        return g_variant_new_boolean(TRUE);
    }
    if (strcmp(prop, "ProtocolVersion") == 0) {
        return g_variant_new_int32(0);
    }
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s", prop);
    return NULL;
}

static const GDBusInterfaceVTable watcher_vtable = {
    .method_call = watcher_call,
    .get_property = watcher_get,
};

static void tray_drop(struct tray *t)
{
    notes_drop(t);
    while (t->items) {
        struct tray_item *it = t->items;
        t->items = it->next;
        item_free(t, it);
    }
    if (t->own) {
        g_bus_unown_name(t->own);
        t->own = 0;
    }
    if (t->object && t->conn) {
        g_dbus_connection_unregister_object(t->conn, t->object);
    }
    t->object = 0;
    if (t->conn) {
        g_object_unref(t->conn);
        t->conn = NULL;
    }
    forget_buttons();
}

static void tray_closed(GDBusConnection *conn, gboolean remote, GError *error, gpointer data)
{
    struct tray *t = tray_of(conn);
    (void)remote;
    (void)error;
    (void)data;
    if (t) {
        say("locachy-bar: the bus of %s closed", t->name);
        tray_drop(t);
        t->retry_at = uptime() + 1.0;
    }
}

static void tray_connected(GObject *src, GAsyncResult *res, gpointer data)
{
    static GDBusNodeInfo *info;
    char *name = data;
    GError *error = NULL;
    GDBusConnection *conn = g_dbus_connection_new_for_address_finish(res, &error);
    struct tray *t = tray_find(name);
    (void)src;
    if (t) {
        t->connecting = 0;
    }
    if (!info) {
        info = g_dbus_node_info_new_for_xml(watcher_xml, NULL);
    }
    if (!conn || !t || !info) {
        if (t) {
            t->retry_at = uptime() + 5.0;
            say("locachy-bar: cannot reach the bus of %s: %s", name, error ? error->message : "no watcher");
        }
        if (conn) {
            g_object_unref(conn);
        }
        if (error) {
            g_error_free(error);
        }
        free(name);
        return;
    }
    t->conn = conn;
    g_dbus_connection_set_exit_on_close(conn, FALSE);
    g_signal_connect(conn, "closed", G_CALLBACK(tray_closed), NULL);
    notes_serve(t); /* its name asked for before the watcher's */
    t->object = g_dbus_connection_register_object(conn, "/StatusNotifierWatcher", info->interfaces[0],
                                                  &watcher_vtable, NULL, NULL, NULL);
    t->own = g_bus_own_name_on_connection(conn, WATCHER, G_BUS_NAME_OWNER_FLAGS_NONE,
                                          NULL, NULL, NULL, NULL);
    say("locachy-bar: the tray watcher of %s", name);
    free(name);
}

static int container_frozen(const char *name)
{
    char path[160], text[128];
    snprintf(path, sizeof path, "/sys/fs/cgroup/app_%.63s/cgroup.events", name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, text, sizeof text - 1) : -1;
    if (fd >= 0) {
        close(fd);
    }
    text[n > 0 ? n : 0] = '\0';
    return strstr(text, "frozen 1") != NULL;
}

/* The buses there are, every quarter of a second: a container waits for the
 * watcher before its program starts, so that the program finds it. A frozen
 * container is left alone; asynchronous throughout, so a frozen bus cannot
 * hold the bar. */
static void tray_scan(void)
{
    if (uptime() - tray_scanned < 0.25) {
        return;
    }
    tray_scanned = uptime();
    for (struct tray *t = trays; t; t = t->next) {
        t->seen = 0;
    }
    DIR *dir = opendir(TRAY_DIR);
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL) {
        char path[320];
        struct stat st;
        if (e->d_name[0] == '.' || strlen(e->d_name) >= OPEN_NAME) {
            continue;
        }
        snprintf(path, sizeof path, TRAY_DIR "/%s/bus/bus", e->d_name);
        if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode)) {
            continue;
        }
        struct tray *t = tray_find(e->d_name);
        if (!t && (t = calloc(1, sizeof *t)) != NULL) {
            snprintf(t->name, sizeof t->name, "%s", e->d_name);
            t->nids = open_window_ids(t->name, t->ids, MAX_TRAY_IDS);
            t->next = trays;
            trays = t;
        }
        if (!t) {
            continue;
        }
        t->seen = 1;
        if (!t->conn && !t->connecting && uptime() >= t->retry_at && !container_frozen(t->name)) {
            char address[400];
            snprintf(address, sizeof address, "unix:path=%s", path);
            t->connecting = 1;
            g_dbus_connection_new_for_address(address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                              G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                              NULL, NULL, tray_connected, strdup(t->name));
        }
    }
    if (dir) {
        closedir(dir);
    }
    for (struct tray **p = &trays; *p;) {
        struct tray *t = *p;
        if (t->seen) {
            p = &t->next;
            continue;
        }
        *p = t->next;
        tray_drop(t);
        free(t);
    }
}

static int tray_has_window(const struct tray *t)
{
    for (struct window *w = windows; w; w = w->next) {
        for (int i = 0; i < t->nids; i++) {
            if (strcasecmp(w->app_id, t->ids[i]) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* A container in the window list: it has tray items and no window. */
static int tray_listed(const struct tray *t)
{
    return t->items && t->nids > 0 && !tray_has_window(t);
}

static void tray_activated(GObject *src, GAsyncResult *res, gpointer data)
{
    char *name = data;
    GError *error = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (v) {
        g_variant_unref(v);
    } else {
        say("locachy-bar: %s: its tray item would not show a window: %s", name,
            error ? error->message : "no answer");
        ask_again(name, N_("Its tray item would not show a window."), 0);
        if (error) {
            g_error_free(error);
        }
    }
    free(name);
}

/* Container name's first tray item, asked for its window (Activate); when
 * none of its windows comes within 3 s, whether to start the program again is
 * asked. -1: it has no tray item. */
static int tray_activate(const char *name)
{
    struct tray *t = tray_find(name);
    if (!t || !t->conn || !t->items) {
        return -1;
    }
    say("locachy-bar: %s: its tray item, for its window", name);
    g_dbus_connection_call(t->conn, t->items->bus, t->items->path, ITEM_IFACE, "Activate",
                           g_variant_new("(ii)", 0, 0), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL,
                           tray_activated, strdup(name));
    watch_window(name);
    return 0;
}


static void click(struct window *w, uint32_t button)
{
    if (button == BTN_MIDDLE) {
        zwlr_foreign_toplevel_handle_v1_close(w->handle);
    } else if (button == BTN_LEFT && w->activated && !w->minimized) {
        zwlr_foreign_toplevel_handle_v1_set_minimized(w->handle);
    } else if (button == BTN_LEFT) {
        if (w->minimized) {
            zwlr_foreign_toplevel_handle_v1_unset_minimized(w->handle);
        }
        zwlr_foreign_toplevel_handle_v1_activate(w->handle, wl_seat_obj);
    }
    wl_display_flush(display);
}

static void human_bytes(unsigned long long n, char *out, size_t size)
{
    if (n >= 1024ULL * 1024 * 1024) {
        snprintf(out, size, "%.1fG", n / (1024.0 * 1024 * 1024));
    } else {
        snprintf(out, size, "%lluM", n / (1024 * 1024));
    }
}

#define WIN_BG  0xFF2A3C5Cu /* a window's button */
#define WIN_ON  0xFF46669Au /* the focused window's */
#define WIN_FG  0xFFD0D0D0u
#define WIN_DIM 0xFF8C98ACu /* a minimized window's title, and second rows */
#define WIN_ATTN 0xFFB0702Au /* a program in a tray that asks for attention */

/* A window is in the list of each screen it is on; one on none (minimized,
 * or not yet shown), in the list of the screen it was last on, or of all. */
static int window_on(const struct window *w, const struct bar *b)
{
    for (int i = 0; i < w->noutputs; i++) {
        if (w->outputs[i] == b->output) {
            return 1;
        }
    }
    if (w->noutputs > 0) {
        return 0;
    }
    if (w->last) {
        for (struct bar *o = bars; o; o = o->next) {
            if (o->output == w->last) {
                return o == b;
            }
        }
    }
    return 1;
}

/* One button of the list: a window, or a container in a tray (in every
 * screen's list: it has no window to say where). */
struct entry {
    struct window *win;
    struct tray *tray;
    const char *top, *bottom;
    char second[96];
    uint32_t bg;
};

static int list_entries(const struct bar *b, struct entry *e, int max)
{
    int n = 0;
    for (struct window *w = windows; w && n < max; w = w->next) {
        if (!window_on(w, b)) {
            continue;
        }
        struct entry *x = &e[n++];
        memset(x, 0, sizeof *x);
        x->win = w;
        x->top = w->title[0] ? w->title : _("(untitled)");
        const char *owner = w->container[0] ? w->container : w->app_id;
        /* TRANSLATORS: %s is the container name or app id; keep short: bar button. */
        snprintf(x->second, sizeof x->second, w->minimized ? _("%s, minimized") : "%s", owner);
        x->bottom = x->second;
        struct tray *t = w->container[0] ? tray_find(w->container) : NULL;
        x->bg = w->activated && !w->minimized ? WIN_ON : t && t->notified ? WIN_ATTN : WIN_BG;
    }
    for (struct tray *t = trays; t && n < max; t = t->next) {
        int attention = t->notified;
        if (!tray_listed(t)) {
            continue;
        }
        for (struct tray_item *it = t->items; it; it = it->next) {
            attention |= it->attention;
        }
        struct entry *x = &e[n++];
        memset(x, 0, sizeof *x);
        x->tray = t;
        x->top = t->items->title;
        /* TRANSLATORS: %s is the container name; keep short: bar button. */
        snprintf(x->second, sizeof x->second, attention ? _("%s, needs attention") : _("%s, in the tray"), t->name);
        x->bottom = x->second;
        x->bg = attention ? WIN_ATTN : WIN_BG;
    }
    return n;
}

static void add_button(struct bar *b, struct window *w, const char *tray, int step, int x0, int x1)
{
    if (b->nbuttons < MAX_BUTTONS) {
        b->buttons[b->nbuttons].win = w;
        snprintf(b->buttons[b->nbuttons].tray, sizeof b->buttons[0].tray, "%s", tray ? tray : "");
        b->buttons[b->nbuttons].step = step;
        b->buttons[b->nbuttons].x0 = x0;
        b->buttons[b->nbuttons++].x1 = x1;
    }
}

/* The window list in [x0, x1) of the buffer, bh high, as on the taskbar of
 * Windows XP: buttons all as wide, at most 20 characters, their text cut
 * where it does not fit; more of them, all narrower, down to 6; still too
 * many, the list scrolls (the wheel, or < and > at its ends), a window that
 * gets the focus brought in sight. */
#define BUTTON_MAX 20
#define BUTTON_MIN 6
static void draw_windows(struct bar *b, uint32_t *px, int bw_px, int bh, int x0, int x1)
{
    static struct entry e[MAX_ENTRIES];
    const struct panel_font *f = panel_font(bh / 2, 0);
    int cw, ch, pad = bh / 18 + 1;
    panel_font_size(f, &cw, &ch);
    b->nbuttons = 0;
    int n = f ? list_entries(b, e, MAX_ENTRIES) : 0;
    int left = x0 + cw, avail = x1 - x0 - 2 * cw;
    if (n == 0 || avail < BUTTON_MIN * cw) {
        return;
    }
    int width = avail / n;
    width = width > BUTTON_MAX * cw ? BUTTON_MAX * cw : width;
    int scroll = width < BUTTON_MIN * cw || n > MAX_BUTTONS - 2, arrow = 2 * cw, shown = n;
    if (!scroll) {
        b->first = 0;
    } else {
        width = BUTTON_MIN * cw;
        avail -= 2 * arrow;
        left += arrow;
        shown = avail / width;
        shown = shown < 1 ? 1 : shown > MAX_BUTTONS - 2 ? MAX_BUTTONS - 2 : shown;
        int focus = -1;
        for (int i = 0; i < n; i++) {
            if (e[i].win && e[i].win->activated && !e[i].win->minimized) {
                focus = i;
            }
        }
        /* A window that has just got the focus is brought in sight; scrolled
         * away from afterwards, the list stays where it was scrolled to. */
        if (focus >= 0 && e[focus].win != b->focus_seen && focus < b->first) {
            b->first = focus;
        } else if (focus >= 0 && e[focus].win != b->focus_seen && focus >= b->first + shown) {
            b->first = focus - shown + 1;
        }
        b->focus_seen = focus >= 0 ? e[focus].win : NULL;
        b->first = b->first > n - shown ? n - shown : b->first < 0 ? 0 : b->first;
    }
    int y0 = (bh - 2 * ch) / 2, x = left, room = width - 2 * pad - cw;
    for (int i = scroll ? b->first : 0; i < n && i < (scroll ? b->first : 0) + shown; i++) {
        char top[160], bottom[160];
        panel_text_fit(top, sizeof top, e[i].top, room, f);
        panel_text_fit(bottom, sizeof bottom, e[i].bottom, room, f);
        panel_fill(px, bw_px, bh, bw_px, x, pad / 2, width - 2 * pad, bh - pad, e[i].bg);
        panel_text(px, bw_px, bh, bw_px, x + cw / 2, y0, top,
                   e[i].win && e[i].win->minimized ? WIN_DIM : WIN_FG, e[i].bg, f);
        panel_text(px, bw_px, bh, bw_px, x + cw / 2, y0 + ch, bottom, WIN_DIM, e[i].bg, f);
        add_button(b, e[i].win, e[i].tray ? e[i].tray->name : NULL, 0, x, x + width - 2 * pad);
        x += width;
    }
    if (scroll) {
        int ay = (bh - ch) / 2, ax = x0 + cw, bx = left + avail;
        uint32_t lc = b->first > 0 ? WIN_FG : WIN_DIM, rc = b->first + shown < n ? WIN_FG : WIN_DIM;
        panel_fill(px, bw_px, bh, bw_px, ax, pad / 2, arrow - pad, bh - pad, WIN_BG);
        panel_text(px, bw_px, bh, bw_px, ax + (arrow - pad - cw) / 2, ay, "<", lc, WIN_BG, f);
        add_button(b, NULL, NULL, -1, ax, ax + arrow - pad);
        panel_fill(px, bw_px, bh, bw_px, bx + pad, pad / 2, arrow - pad, bh - pad, WIN_BG);
        panel_text(px, bw_px, bh, bw_px, bx + pad + (arrow - pad - cw) / 2, ay, ">", rc, WIN_BG, f);
        add_button(b, NULL, NULL, 1, bx + pad, bx + arrow);
    }
}

/* Drawn at the screen's scale (wp_fractional_scale_v1), shown at its
 * logical size (wp_viewport): as sharp at 1.25 or 2 as at 1. */
static void render_bar(struct bar *b, const char *title, const char *sub)
{
    struct entry e[MAX_BUTTONS];
    if (!b->configured || !b->surf || !shm) return;
    uint32_t s120 = b->viewport ? b->scale120 : 120;
    int w = (int)((b->w * s120 + 60) / 120), h = (int)((b->h * s120 + 60) / 120), x0, x1, cw, ch;
    panel_font_size(panel_font(h / 2, 0), &cw, &ch);
    int n = list_entries(b, e, 3); /* room for up to three, if the left blocks can give it */
    uint32_t *px = NULL;
    struct wl_buffer *buf = create_shm_buffer(w, h, &px);
    if (!buf) return;
    panel_draw(px, w, h, w, title, sub, ime_mode_label(), n ? n * 10 * cw + 2 * cw : 0, &x0, &x1);
    draw_windows(b, px, w, h, x0, x1);
    munmap(px, (size_t)w * (size_t)h * 4);
    wl_surface_attach(b->surf, buf, 0, 0);
    if (b->viewport) {
        wp_viewport_set_destination(b->viewport, (int32_t)b->w, (int32_t)b->h);
    }
    wl_surface_damage_buffer(b->surf, 0, 0, w, h);
    wl_surface_commit(b->surf);
}

/* Every screen's bar; the title block is the container of the focused window. */
static void render_bars(void)
{
    char title[64], sub[64] = "", size[24];
    const char *focus = "";
    unsigned long long ram;
    for (struct window *w = windows; w; w = w->next) {
        if (w->activated && !w->minimized && w->container[0]) {
            focus = w->container;
        }
    }
    struct tray *seen = focus[0] ? tray_find(focus) : NULL;
    if (seen) {
        seen->notified = 0; /* one of its windows has the focus */
    }
    int frozen = panel_containers(focus, &ram);
    /* TRANSLATORS: keep short: bar field. */
    snprintf(title, sizeof title, "%s", focus[0] ? focus : _("no container"));
    human_bytes(ram, size, sizeof size);
    if (focus[0]) {
        /* TRANSLATORS: %s is RAM size (e.g. "320M"), %d is the number of frozen containers; keep short: bar field. */
        snprintf(sub, sizeof sub, P_("RAM %s, %d frozen", "RAM %s, %d frozen", frozen), size, frozen);
    } else if (frozen) {
        /* TRANSLATORS: %d is the number of frozen containers; keep short: bar field. */
        snprintf(sub, sizeof sub, P_("%d frozen", "%d frozen", frozen), frozen);
    }
    for (struct bar *b = bars; b; b = b->next) {
        render_bar(b, title, sub);
    }
}

/* Pointer: clicks on the window list, with an arrow over the bar. */
static struct wl_pointer *pointer;
static struct bar *pointer_bar;
static int pointer_notes; /* over the notifications */
static double pointer_x, pointer_y;
static struct wl_cursor_theme *cursor_theme;
static struct wl_surface *cursor_surface;
static struct zwp_relative_pointer_manager_v1 *relative_mgr;
static struct zwp_relative_pointer_v1 *relative;

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    pointer_bar = NULL;
    for (struct bar *b = bars; b; b = b->next) {
        if (b->surf == s) {
            pointer_bar = b;
        }
    }
    pointer_notes = s && s == notes_surface();
    pointer_x = wl_fixed_to_double(x);
    pointer_y = wl_fixed_to_double(y);
    struct wl_cursor *cursor = cursor_theme ? wl_cursor_theme_get_cursor(cursor_theme, "left_ptr") : NULL;
    if (!cursor && cursor_theme) {
        cursor = wl_cursor_theme_get_cursor(cursor_theme, "default");
    }
    if (cursor && cursor_surface) {
        struct wl_cursor_image *image = cursor->images[0];
        wl_surface_attach(cursor_surface, wl_cursor_image_get_buffer(image), 0, 0);
        wl_surface_damage_buffer(cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
        wl_surface_commit(cursor_surface);
        wl_pointer_set_cursor(p, serial, cursor_surface, (int32_t)image->hotspot_x, (int32_t)image->hotspot_y);
    }
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
    (void)data;
    (void)p;
    (void)serial;
    (void)s;
    pointer_bar = NULL;
    pointer_notes = 0;
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data;
    (void)p;
    (void)time;
    pointer_x = wl_fixed_to_double(x);
    pointer_y = wl_fixed_to_double(y);
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)data;
    (void)p;
    (void)serial;
    (void)time;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED && pointer_notes) {
        notes_click(pointer_y, button);
        return;
    }
    if (state != WL_POINTER_BUTTON_STATE_PRESSED || !pointer_bar) {
        return;
    }
    double x = pointer_x * (pointer_bar->viewport ? pointer_bar->scale120 : 120) / 120.0;
    for (int i = 0; i < pointer_bar->nbuttons; i++) {
        if (x >= pointer_bar->buttons[i].x0 && x < pointer_bar->buttons[i].x1) {
            if (pointer_bar->buttons[i].win) {
                click(pointer_bar->buttons[i].win, button);
            } else if (pointer_bar->buttons[i].tray[0] && button == BTN_LEFT) {
                tray_activate(pointer_bar->buttons[i].tray);
            } else if (pointer_bar->buttons[i].step && button == BTN_LEFT) {
                pointer_bar->first += pointer_bar->buttons[i].step;
                bars_dirty = 1;
            }
            return;
        }
    }
}

/* The wheel over a bar scrolls its window list, an entry a notch. */
static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data;
    (void)p;
    (void)time;
    if (!pointer_bar || axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
        return;
    }
    pointer_bar->wheel += wl_fixed_to_double(value);
    while (pointer_bar->wheel >= 10) {
        pointer_bar->wheel -= 10;
        pointer_bar->first++;
        bars_dirty = 1;
    }
    while (pointer_bar->wheel <= -10) {
        pointer_bar->wheel += 10;
        pointer_bar->first = pointer_bar->first > 0 ? pointer_bar->first - 1 : 0;
        bars_dirty = 1;
    }
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

/* The overview, by itself (its parent gone at once, the bar has no child to
 * wait for); run while it is up, it closes it. */
static void run_overview(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execl("/usr/local/bin/locachy-overview", "locachy-overview", (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

/* The top left corner of a screen, as KWin's screen edges have it
 * (screenedge.cpp: Edge::check, canActivate; kwinrc's defaults): pushed into
 * for ElectricBorderDelay, 75 ms, it triggers, every push of an attempt
 * within the reactivation threshold (ElectricBorderCooldown, 350 ms) of its
 * first; the pointer more than 30 pixels away, the attempt is over; after a
 * trigger, pushes go unheeded until the corner has been left alone for
 * 275 ms (that threshold less the delay), so one long push shows or closes
 * the overview once. KWin pushes the pointer back a pixel so that each push
 * meets the corner again; a client cannot move the pointer, so here a push
 * is relative motion (zwp-relative-pointer, which the compositor sends
 * though the pointer goes no further) up or left while the pointer is in the
 * corner's pixel of a bar (the overview leaves that pixel to the bar). A
 * screen beside or above, the pointer goes there instead: no corner, as in
 * KWin. (x, y): the pointer on the bar, (dx, dy): its motion, ms: when.
 * 1: the corner triggers. */
static long long corner_attempt, corner_triggered; /* ms; 0: none */

static int corner_push(double x, double y, double dx, double dy, long long ms)
{
    if (x >= 1 || y >= 1 || (dx >= 0 && dy >= 0)) {
        if (x + y > 30) {
            corner_attempt = 0;
        }
        return 0;
    }
    if (corner_triggered && ms - corner_triggered < 350 - 75) {
        corner_triggered = ms; /* still cooling down: from now on */
        return 0;
    }
    if (!corner_attempt || ms - corner_attempt > 350) {
        corner_attempt = ms;
        return 0;
    }
    if (ms - corner_attempt < 75) {
        return 0;
    }
    corner_triggered = ms;
    corner_attempt = 0;
    return 1;
}

static void relative_motion(void *data, struct zwp_relative_pointer_v1 *r, uint32_t hi, uint32_t lo,
                            wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t udx, wl_fixed_t udy)
{
    (void)data;
    (void)r;
    (void)udx;
    (void)udy;
    long long ms = (long long)(((uint64_t)hi << 32 | lo) / 1000);
    if (pointer_bar && corner_push(pointer_x, pointer_y, wl_fixed_to_double(dx), wl_fixed_to_double(dy), ms)) {
        say("locachy-bar: the top left corner: the overview");
        run_overview();
    }
}

static const struct zwp_relative_pointer_v1_listener relative_listener = { relative_motion };

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
        if (relative_mgr) {
            relative = zwp_relative_pointer_manager_v1_get_relative_pointer(relative_mgr, pointer);
            zwp_relative_pointer_v1_add_listener(relative, &relative_listener, NULL);
        }
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
        if (relative) {
            zwp_relative_pointer_v1_destroy(relative);
            relative = NULL;
        }
        wl_pointer_destroy(pointer);
        pointer = NULL;
        pointer_bar = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static char toast_msg[64];
static double toast_expire;
static int toast_active;

static void bar_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                          uint32_t serial, uint32_t width, uint32_t height)
{
    struct bar *b = data;
    if (width > 0) b->w = width;
    if (height > 0) b->h = height;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    b->configured = 1;
    bars_dirty = 1;
}

static void destroy_bar_surface(struct bar *b)
{
    if (b->scale) {
        wp_fractional_scale_v1_destroy(b->scale);
        b->scale = NULL;
    }
    if (b->viewport) {
        wp_viewport_destroy(b->viewport);
        b->viewport = NULL;
    }
    if (b->lsurf) {
        zwlr_layer_surface_v1_destroy(b->lsurf);
        b->lsurf = NULL;
    }
    if (b->surf) {
        wl_surface_destroy(b->surf);
        b->surf = NULL;
    }
    if (pointer_bar == b) {
        pointer_bar = NULL;
    }
    b->configured = b->shown = 0;
    b->nbuttons = 0;
}

/* labwc closes a layer surface that has no screen to be on: when it asked
 * before the screens were up, or its screen went away. Asked again later. */
static void bar_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    struct bar *b = data;
    (void)surface;
    say("locachy-bar: the compositor closed the bar of screen %u; asking again", b->name);
    destroy_bar_surface(b);
    b->since = uptime();
}

static const struct zwlr_layer_surface_v1_listener bar_layer_listener = {
    .configure = bar_configure,
    .closed = bar_closed,
};

static void bar_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    struct bar *b = data;
    (void)surface;
    (void)output;
    b->shown = 1;
    b->retries = 0;
}

static void bar_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data;
    (void)surface;
    (void)output;
}

static const struct wl_surface_listener bar_surface_listener = {
    .enter = bar_enter,
    .leave = bar_leave,
};

/* The bar's height at a scale: one and a half rows of the direct screens'
 * text there (text_px, the nearest Terminus), in logical pixels rounded up,
 * so that at the scale it is as high as theirs (or a pixel higher, where no
 * whole number of logical pixels gives theirs) and has the same fonts. */
static uint32_t bar_height(uint32_t s120)
{
    uint32_t px = (uint32_t)text_px(s120 / 120.0) * 3 / 2;
    return (px * 120 + s120 - 1) / s120;
}

static void ask_bar_height(struct bar *b)
{
    b->want_h = bar_height(b->scale120);
    zwlr_layer_surface_v1_set_size(b->lsurf, 0, b->want_h);
    zwlr_layer_surface_v1_set_exclusive_zone(b->lsurf, (int32_t)b->want_h);
}

static void bar_scale(void *data, struct wp_fractional_scale_v1 *scale, uint32_t scale120)
{
    struct bar *b = data;
    (void)scale;
    if (b->scale120 != scale120) {
        b->scale120 = scale120;
        bars_dirty = 1;
        if (b->lsurf && bar_height(scale120) != b->want_h) {
            ask_bar_height(b); /* configured anew at it, then drawn */
            wl_surface_commit(b->surf);
        }
    }
}

static const struct wp_fractional_scale_v1_listener bar_scale_listener = {
    .preferred_scale = bar_scale,
};

static void create_bar_surface(struct bar *b)
{
    b->surf = wl_compositor_create_surface(compositor);
    wl_surface_add_listener(b->surf, &bar_surface_listener, b);
    b->scale120 = 120;
    if (scale_mgr && viewporter) {
        b->scale = wp_fractional_scale_manager_v1_get_fractional_scale(scale_mgr, b->surf);
        wp_fractional_scale_v1_add_listener(b->scale, &bar_scale_listener, b);
        b->viewport = wp_viewporter_get_viewport(viewporter, b->surf);
    }
    b->lsurf = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, b->surf, b->output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "locachy-bar");
    ask_bar_height(b);
    zwlr_layer_surface_v1_set_anchor(b->lsurf,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_add_listener(b->lsurf, &bar_layer_listener, b);
    wl_surface_commit(b->surf);
    b->since = uptime();
}

/* Surfaces for the bars that need one: a new screen's, a closed one a second
 * later, and one that labwc configured but has not put on a screen after 3 s
 * (it lost it while setting up the screens at start), at most 3 times. */
static void check_bars(void)
{
    for (struct bar *b = bars; b; b = b->next) {
        if (!b->surf && uptime() - b->since >= 1.0) {
            create_bar_surface(b);
        } else if (b->surf && b->configured && !b->shown && uptime() - b->since >= 3.0 &&
                   b->retries < 3) {
            say("locachy-bar: the bar of screen %u is on no screen; asking again", b->name);
            b->retries++;
            destroy_bar_surface(b);
            create_bar_surface(b);
        }
    }
}

/*
 * The overlays: the input method's bar at the bottom (with the notices of
 * mode and volume), the questions in the middle (which program opens a file,
 * whether to start one again) and the notifications at the top right, under
 * the bar. Each is a layer surface on the screen
 * of the focused window, made anew there when the focus goes to another
 * screen, and drawn at that screen's scale as the bars are: text in cells of
 * the bold font text_px high at the scale, as the direct screens' text.
 */
struct overlay {
    const char *ns;             /* the layer surface's namespace */
    void (*render)(void);       /* draws it anew: configured, the scale changed */
    uint32_t anchor;            /* the edges it is at; none: in the middle */
    int margin;                 /* from them, logical pixels */
    struct wl_surface *surf;
    struct zwlr_layer_surface_v1 *lsurf;
    struct wp_fractional_scale_v1 *scale;
    struct wp_viewport *viewport;
    struct wl_output *output;   /* the screen asked for; NULL: labwc's choice */
    uint32_t scale120;
    uint32_t want_w, want_h;    /* logical pixels asked for, and configured: */
    uint32_t w, h;
    int configured;
    /* while it is drawn: the buffer, bw x bh, and the font at the scale */
    struct wl_buffer *buf;
    uint32_t *px;
    int bw, bh, cw, ch;
    const struct panel_font *font;
};

/* The screen of the focused window; NULL if none has the focus. */
static struct wl_output *focused_output(void)
{
    for (struct window *w = windows; w; w = w->next) {
        if (w->activated && !w->minimized && w->noutputs > 0) {
            return w->outputs[0];
        }
    }
    return NULL;
}

static uint32_t output_scale(struct wl_output *output)
{
    for (struct bar *b = bars; b; b = b->next) {
        if (b->output == output && b->viewport) {
            return b->scale120;
        }
    }
    return 120;
}

static void overlay_destroy(struct overlay *o)
{
    if (o->scale) {
        wp_fractional_scale_v1_destroy(o->scale);
        o->scale = NULL;
    }
    if (o->viewport) {
        wp_viewport_destroy(o->viewport);
        o->viewport = NULL;
    }
    if (o->lsurf) {
        zwlr_layer_surface_v1_destroy(o->lsurf);
        o->lsurf = NULL;
    }
    if (o->surf) {
        wl_surface_destroy(o->surf);
        o->surf = NULL;
    }
    o->configured = 0;
    o->want_w = o->want_h = 0;
}

static void overlay_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                              uint32_t serial, uint32_t width, uint32_t height)
{
    struct overlay *o = data;
    if (width > 0) o->w = width;
    if (height > 0) o->h = height;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    o->configured = 1;
    o->render();
}

static void overlay_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    (void)surface;
    overlay_destroy(data); /* the next drawing asks for a new one */
}

static const struct zwlr_layer_surface_v1_listener overlay_listener = {
    .configure = overlay_configure,
    .closed = overlay_closed,
};

static void overlay_scale(void *data, struct wp_fractional_scale_v1 *scale, uint32_t scale120)
{
    struct overlay *o = data;
    (void)scale;
    if (o->scale120 != scale120) {
        o->scale120 = scale120;
        o->render();
    }
}

static const struct wp_fractional_scale_v1_listener overlay_scale_listener = {
    .preferred_scale = overlay_scale,
};

/* Ready to draw o, rows cells high and cols wide (0: as wide as the screen,
 * anchored left and right): 1 with o's buffer to draw into, or 0
 * (its surface, or its new size, is asked for; drawn when configured). */
static int overlay_begin(struct overlay *o, int cols, int rows)
{
    struct wl_output *out = focused_output();
    if (o->surf && out && out != o->output) {
        overlay_destroy(o); /* the focus went to another screen */
    }
    if (!o->surf) {
        o->output = out;
        o->scale120 = output_scale(out);
        o->surf = wl_compositor_create_surface(compositor);
        if (scale_mgr && viewporter) {
            o->scale = wp_fractional_scale_manager_v1_get_fractional_scale(scale_mgr, o->surf);
            wp_fractional_scale_v1_add_listener(o->scale, &overlay_scale_listener, o);
            o->viewport = wp_viewporter_get_viewport(viewporter, o->surf);
        }
        o->lsurf = zwlr_layer_shell_v1_get_layer_surface(layer_shell, o->surf, out,
                                                         ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, o->ns);
        zwlr_layer_surface_v1_set_anchor(o->lsurf, o->anchor);
        if (cols == 0) {
            zwlr_layer_surface_v1_set_exclusive_zone(o->lsurf, -1); /* over the bar too */
        } /* else kept off the bar by labwc */
        zwlr_layer_surface_v1_set_margin(o->lsurf, o->margin, o->margin, o->margin, o->margin);
        zwlr_layer_surface_v1_add_listener(o->lsurf, &overlay_listener, o);
    }
    uint32_t s120 = o->viewport ? o->scale120 : 120;
    int row = text_px(s120 / 120.0);
    o->font = panel_font(row, 1);
    panel_font_size(o->font, &o->cw, &o->ch);
    if (!o->font) {
        o->cw = row / 2;
        o->ch = row;
    }
    uint32_t want_w = cols ? ((uint32_t)(cols * o->cw) * 120 + s120 - 1) / s120 : 0;
    uint32_t want_h = ((uint32_t)(rows * o->ch) * 120 + s120 - 1) / s120;
    if (want_w != o->want_w || want_h != o->want_h) {
        o->want_w = want_w;
        o->want_h = want_h;
        zwlr_layer_surface_v1_set_size(o->lsurf, want_w, want_h);
        wl_surface_commit(o->surf);
        wl_display_flush(display);
        return 0;
    }
    if (!o->configured) {
        return 0;
    }
    o->bw = (int)((o->w * s120 + 60) / 120);
    o->bh = (int)((o->h * s120 + 60) / 120);
    o->buf = create_shm_buffer(o->bw, o->bh, &o->px);
    return o->buf != NULL;
}

static void overlay_end(struct overlay *o)
{
    munmap(o->px, (size_t)o->bw * (size_t)o->bh * 4);
    wl_surface_attach(o->surf, o->buf, 0, 0);
    if (o->viewport) {
        wp_viewport_set_destination(o->viewport, (int32_t)o->w, (int32_t)o->h);
    }
    wl_surface_damage_buffer(o->surf, 0, 0, o->bw, o->bh);
    wl_surface_commit(o->surf);
    wl_display_flush(display);
}

static void ovl_clear(struct overlay *o, uint32_t argb)
{
    panel_fill(o->px, o->bw, o->bh, o->bw, 0, 0, o->bw, o->bh, argb);
}

/* cols 0: to the right edge. */
static void ovl_fill(struct overlay *o, int col, int row, int cols, int rows, uint32_t argb)
{
    panel_fill(o->px, o->bw, o->bh, o->bw, col * o->cw, row * o->ch,
               cols ? cols * o->cw : o->bw, rows * o->ch, argb);
}

static void ovl_text(struct overlay *o, int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    panel_text(o->px, o->bw, o->bh, o->bw, col * o->cw, row * o->ch, s, fg, bg, o->font);
}

/* The input method's bar: shown only while composing, or for a notice. */
static void render_bot_bar(void);
static struct overlay ime_bar = {
    .ns = "locachy-ime", .render = render_bot_bar,
    .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
              ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
};

static void render_bot_bar(void)
{
    char row1[512], row2[512];
    int active = ime_get_overlay(row1, row2, sizeof row1);
    struct overlay *o = &ime_bar;
    if (toast_active && uptime() >= toast_expire) {
        toast_active = 0;
    }

    if (!active && !toast_active) {
        if (o->surf) {
            overlay_destroy(o);
            wl_display_flush(display);
        }
        return;
    }
    if (!overlay_begin(o, 0, active && row2[0] != '\0' ? 2 : 1)) {
        return;
    }
    if (active) {
        ovl_clear(o, OVL_BG);
        ovl_text(o, 1, 0, row1, OVL_FG1, OVL_BG);
        if (row2[0] != '\0') {
            ovl_text(o, 1, 1, row2, OVL_FG2, OVL_BG);
        }
    } else {
        ovl_clear(o, 0x00000000u);
        int cols = o->bw / o->cw;
        int tw = panel_text_width(toast_msg, o->font) / o->cw;
        int tcol = cols > tw ? (cols - tw) / 2 : 0;
        ovl_fill(o, tcol, 0, tw, 1, TOAST_BG);
        ovl_text(o, tcol, 0, toast_msg, OVL_FG1, TOAST_BG);
    }
    overlay_end(o);
}

/* The keyboards (evdev), each with its layout. */
#define MAX_KBDS 8
static int kbds[MAX_KBDS];
static int nkbds;
static int kbd_layout[MAX_KBDS];
static volatile sig_atomic_t sig_regrab, sig_release, sig_reload, active = 1;
static void set_grab(int on);
static void load_width_keys(void);

/* Virtual keyboard + XKB keymap handling. A virtual keyboard for each
 * keyboard layout in use, the system's first: a keyboard with a keymap of its
 * own (keyboards.conf, keys.c) types through its layout's, as sway gives each
 * keyboard its keymap; the seat sends the focused client the keymap and
 * modifiers of the keyboard a key comes from. Each layout's xkb state has the
 * keys of its keyboards and the modifier keys held on all of them, as that
 * layout's key of the same role (Shift on one keyboard, 2 on another); the
 * locks are one (share_locks). */
#define MAX_LAYOUTS 8
#define ROLE_NUM 64 /* Num Lock, besides keys.c's KEYS_* roles */
#define NROLES 7
struct layout {
    char names[4][128]; /* layout, model, variant, options: keys.c's table's */
    struct xkb_keymap *map;
    struct xkb_state *st;
    int keymap_fd, keymap_sent;
    size_t keymap_len;
    struct zwp_virtual_keyboard_v1 *vkbd; /* kept when the layout is made again */
    xkb_mod_mask_t sent[3];               /* the modifiers last sent: depressed, latched, locked */
    xkb_layout_index_t sent_group;
    int sent_valid;
    uint8_t fed[KEY_MAX + 1];  /* presses in st, by keycode */
    uint8_t down[KEY_MAX + 1]; /* presses passed on to vkbd, by keycode */
    unsigned char role[256];   /* each key's modifier role */
    int role_key[NROLES];      /* the layout's first key of each role */
};
static struct xkb_context *xkb_ctx;
static struct layout layouts[MAX_LAYOUTS];
static int nlayouts, active_layout, cur_layout; /* the seat's; the last key's keyboard's */
/* The dead keys while the keyboard is the input method's: libxkbcommon's
 * compose for the locale, as the programs would have them. */
static struct xkb_compose_state *compose_st;
static struct zwp_virtual_keyboard_v1 *text_vkbd;
static int input_epoch;
static void reset_keyboard_state(void);
static void clear_virtual_keys(void);

/* Caps Lock locked, as the keyboard's xkb state has it. */
static int caps_locked(void)
{
    struct xkb_state *st = layouts[cur_layout].st;
    return st && xkb_state_mod_name_is_active(st, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0;
}

static uint32_t now_ms32(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Layout l, its keymap from the X names (missing: us, pc105), as labwc would
 * make it from them; 0, or -1 when it cannot be made. */
static int layout_init(struct layout *l, const char *const names[4])
{
    struct xkb_rule_names rmlvo = {
        .rules = "evdev",
        .layout = names[0] && names[0][0] ? names[0] : "us",
        .model = names[1] && names[1][0] ? names[1] : "pc105",
        .variant = names[2],
        .options = names[3],
    };
    struct xkb_keymap *map = xkb_ctx ? xkb_keymap_new_from_names(xkb_ctx, &rmlvo, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    struct xkb_state *st = map ? xkb_state_new(map) : NULL;
    char *str = map ? xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    size_t size = str ? strlen(str) + 1 : 0;
    int fd = str ? memfd_create("locachy-xkb", MFD_CLOEXEC) : -1;
    if (!st || fd < 0 || write(fd, str, size) != (ssize_t)size) {
        if (fd >= 0) close(fd);
        free(str);
        xkb_state_unref(st);
        xkb_keymap_unref(map);
        return -1;
    }
    free(str);
    for (int i = 0; i < 4; i++) snprintf(l->names[i], sizeof l->names[i], "%s", names[i] ? names[i] : "");
    l->map = map;
    l->st = st;
    l->keymap_fd = fd;
    l->keymap_len = size;
    l->keymap_sent = l->sent_valid = 0;
    /* The roles as keytable gives them keys.c, and Num Lock. */
    for (int code = 1; code < 256; code++) {
        struct xkb_state *press = xkb_state_new(map);
        if (!press) break;
        xkb_state_update_key(press, (xkb_keycode_t)code + 8, XKB_KEY_DOWN);
        static const struct { const char *name; enum xkb_state_component kind; int role; } mods[] = {
            { XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_DEPRESSED, KEYS_SHIFT },
            { XKB_MOD_NAME_CTRL, XKB_STATE_MODS_DEPRESSED, KEYS_CTRL },
            { XKB_MOD_NAME_ALT, XKB_STATE_MODS_DEPRESSED, KEYS_ALT },
            { XKB_MOD_NAME_LOGO, XKB_STATE_MODS_DEPRESSED, KEYS_META },
            { "Mod5", XKB_STATE_MODS_DEPRESSED, KEYS_ALTGR },
            { XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED, KEYS_CAPS },
            { XKB_MOD_NAME_NUM, XKB_STATE_MODS_LOCKED, ROLE_NUM },
        };
        int role = 0;
        for (size_t m = 0; m < sizeof mods / sizeof mods[0]; m++) {
            if (xkb_state_mod_name_is_active(press, mods[m].name, mods[m].kind) > 0) role |= mods[m].role;
        }
        xkb_state_unref(press);
        l->role[code] = (unsigned char)role;
        for (int r = 0; r < NROLES; r++) {
            if ((role & (1 << r)) && !l->role_key[r]) l->role_key[r] = code;
        }
    }
    return 0;
}

/* Layout l gone; its virtual keyboard stays for the next one in its place. */
static void layout_free(struct layout *l)
{
    struct zwp_virtual_keyboard_v1 *kept = l->vkbd;
    xkb_state_unref(l->st);
    xkb_keymap_unref(l->map);
    if (l->keymap_fd >= 0) close(l->keymap_fd);
    memset(l, 0, sizeof *l);
    l->keymap_fd = -1;
    l->vkbd = kept;
}

static int same_names(const struct layout *l, const char *const names[4])
{
    for (int i = 0; i < 4; i++) {
        if (strcmp(l->names[i], names[i] ? names[i] : "") != 0) return 0;
    }
    return 1;
}

/* The layout of keyboard fd (keys.c's table for it): one there is with the
 * same names, or a new one; the system's when it cannot be made. */
static int layout_of(int fd)
{
    const char *names[4] = { keys_device_layout(fd, "layout"), keys_device_layout(fd, "model"),
                             keys_device_layout(fd, "variant"), keys_device_layout(fd, "options") };
    for (int l = 0; l < nlayouts; l++) {
        if (same_names(&layouts[l], names)) return l;
    }
    if (nlayouts == MAX_LAYOUTS || layout_init(&layouts[nlayouts], names) < 0) {
        say("locachy-bar: cannot load keyboard layout %s; that keyboard types as the system's",
            names[0] ? names[0] : "us");
        return 0;
    }
    say("locachy-bar: a keyboard of its own layout, %s", names[0] ? names[0] : "us");
    return nlayouts++;
}

/* The layouts made again: the system's (keys.c's table: rc.conf's keymap,
 * as locachy-seat gives labwc), then each keyboard's. */
static void init_xkb(void)
{
    if (!xkb_ctx) xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkb_ctx) return;
    const char *names[4] = { keys_layout_name("layout"), keys_layout_name("model"), keys_layout_name("variant"),
                             keys_layout_name("options") };
    struct layout fresh;
    memset(&fresh, 0, sizeof fresh);
    if (layout_init(&fresh, names) < 0) {
        say("locachy-bar: cannot load the selected keyboard layout");
        return;
    }
    for (int l = 0; l < MAX_LAYOUTS; l++) {
        if (l < nlayouts || layouts[l].vkbd) layout_free(&layouts[l]);
    }
    fresh.vkbd = layouts[0].vkbd;
    layouts[0] = fresh;
    nlayouts = 1;
    active_layout = cur_layout = 0;
    for (int i = 0; i < nkbds; i++) kbd_layout[i] = layout_of(kbds[i]);
    const char *locale = getenv("LC_ALL");
    if (!locale || !*locale) locale = getenv("LC_CTYPE");
    if (!locale || !*locale) locale = getenv("LANG");
    struct xkb_compose_table *compose = xkb_compose_table_new_from_locale(xkb_ctx, locale && *locale ? locale : "C",
                                                                          XKB_COMPOSE_COMPILE_NO_FLAGS);
    xkb_compose_state_unref(compose_st);
    compose_st = NULL;
    if (compose) {
        compose_st = xkb_compose_state_new(compose, XKB_COMPOSE_STATE_NO_FLAGS);
        xkb_compose_table_unref(compose);
    }
}

/* Layout l's virtual keyboard, made and given its keymap when first used. */
static struct zwp_virtual_keyboard_v1 *layout_vkbd(struct layout *l)
{
    if (!l->vkbd && vkbd_mgr && wl_seat_obj) {
        l->vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkbd_mgr, wl_seat_obj);
    }
    if (l->vkbd && !l->keymap_sent && l->keymap_fd >= 0) {
        zwp_virtual_keyboard_v1_keymap(l->vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, l->keymap_fd,
                                       (uint32_t)l->keymap_len);
        l->keymap_sent = 1;
        l->sent_valid = 0;
    }
    return l->vkbd;
}

/* The seat's keyboard becomes layout l's, with the modifiers as they are now:
 * labwc gives the seat the virtual keyboard a key or modifiers come from. */
static struct zwp_virtual_keyboard_v1 *use_layout(int l)
{
    struct layout *y = &layouts[l];
    struct zwp_virtual_keyboard_v1 *k = y->st ? layout_vkbd(y) : NULL;
    if (!k) return NULL;
    xkb_mod_mask_t dep = xkb_state_serialize_mods(y->st, XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t lat = xkb_state_serialize_mods(y->st, XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t lck = xkb_state_serialize_mods(y->st, XKB_STATE_MODS_LOCKED);
    xkb_layout_index_t grp = xkb_state_serialize_layout(y->st, XKB_STATE_LAYOUT_EFFECTIVE);
    if (l != active_layout || !y->sent_valid || y->sent[0] != dep || y->sent[1] != lat || y->sent[2] != lck ||
        y->sent_group != grp) {
        zwp_virtual_keyboard_v1_modifiers(k, dep, lat, lck, grp);
        y->sent[0] = dep;
        y->sent[1] = lat;
        y->sent[2] = lck;
        y->sent_group = grp;
        y->sent_valid = 1;
    }
    active_layout = l;
    return k;
}

/* The seat's layout's keymap again (after text typed with keymaps of its
 * own), and its modifiers. */
static void restore_passthrough_keymap(void)
{
    struct layout *y = &layouts[active_layout];
    if (!y->st) return;
    y->keymap_sent = 0;
    if (layout_vkbd(y)) use_layout(active_layout);
}

/* A key of a keyboard of layout l, down or up: into l's state; a modifier
 * held also as its role's key into the other layouts' states (the locks: see
 * share_locks). Each state counts the presses of a key (the same keycode on
 * two keyboards). */
static void feed(struct layout *y, int code, int down)
{
    if (!y->st || code < 1 || code > KEY_MAX) return;
    if (down ? y->fed[code]++ == 0 : (y->fed[code] && --y->fed[code] == 0)) {
        xkb_state_update_key(y->st, (xkb_keycode_t)code + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
    }
}

static void feed_key(int l, int code, int down)
{
    feed(&layouts[l], code, down);
    int role = code > 0 && code < 256 ? layouts[l].role[code] & (KEYS_SHIFT | KEYS_CTRL | KEYS_ALT | KEYS_META | KEYS_ALTGR) : 0;
    for (int m = 0; role && m < nlayouts; m++) {
        for (int r = 0; m != l && r < NROLES; r++) {
            if ((role & (1 << r)) && layouts[m].role_key[r]) feed(&layouts[m], layouts[m].role_key[r], down);
        }
    }
}

/* Caps Lock and Num Lock as layout l has them now, in the other layouts'
 * states too: one lock, however a layout reaches it (jp's Shift+Eisu). */
static void share_locks(int l)
{
    static const char *const locks[] = { XKB_MOD_NAME_CAPS, XKB_MOD_NAME_NUM };
    for (int m = 0; m < nlayouts; m++) {
        struct xkb_state *st = layouts[m].st;
        if (m == l || !st || !layouts[l].st) continue;
        xkb_mod_mask_t locked = xkb_state_serialize_mods(st, XKB_STATE_MODS_LOCKED), was = locked;
        for (size_t i = 0; i < sizeof locks / sizeof locks[0]; i++) {
            xkb_mod_index_t k = xkb_keymap_mod_get_index(layouts[m].map, locks[i]);
            if (k == XKB_MOD_INVALID) continue;
            if (xkb_state_mod_name_is_active(layouts[l].st, locks[i], XKB_STATE_MODS_LOCKED) > 0) locked |= 1u << k;
            else locked &= ~(1u << k);
        }
        if (locked != was) {
            xkb_state_update_mask(st, xkb_state_serialize_mods(st, XKB_STATE_MODS_DEPRESSED),
                                  xkb_state_serialize_mods(st, XKB_STATE_MODS_LATCHED), locked,
                                  xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_DEPRESSED),
                                  xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_LATCHED),
                                  xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_LOCKED));
        }
    }
}

/* The keys text is typed on: letters and digits only. Applications go by the
 * key as well as by the character the keymap gives it, and Esc, Enter, Tab,
 * Backspace... have meanings of their own (Chromium drops a character typed
 * on Esc). */
static const uint8_t text_keys[] = {
    KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
    KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
};
#define NTEXT_KEYS ((int)sizeof text_keys)

/* Type n characters with a keymap that has them on text_keys. */
static void vkbd_type_chunk(const uint32_t *cps, int n)
{
    char map[8192];
    size_t len = 0;
    len += (size_t)snprintf(map + len, sizeof map - len,
        "xkb_keymap {\n"
        "  xkb_keycodes \"(unnamed)\" {\n"
        "    minimum = 8;\n"
        "    maximum = 255;\n");
    for (int i = 0; i < n; i++) {
        len += (size_t)snprintf(map + len, sizeof map - len, "    <K%d> = %d;\n", i, text_keys[i] + 8);
    }
    len += (size_t)snprintf(map + len, sizeof map - len,
        "  };\n"
        "  xkb_types \"(unnamed)\" { include \"complete\" };\n"
        "  xkb_compatibility \"(unnamed)\" { include \"complete\" };\n"
        "  xkb_symbols \"(unnamed)\" {\n");
    for (int i = 0; i < n; i++) {
        len += (size_t)snprintf(map + len, sizeof map - len, "    key <K%d> { [ U%04X ] };\n", i, cps[i]);
    }
    len += (size_t)snprintf(map + len, sizeof map - len, "  };\n};\n");

    int fd = memfd_create("locachy-xkb-utf8", MFD_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, map, len + 1) < 0) {
        close(fd);
        return;
    }
    zwp_virtual_keyboard_v1_keymap(text_vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)(len + 1));
    zwp_virtual_keyboard_v1_modifiers(text_vkbd, 0, 0, 0, 0);
    uint32_t t = now_ms32();
    for (int i = 0; i < n; i++) {
        zwp_virtual_keyboard_v1_key(text_vkbd, t, text_keys[i], WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(text_vkbd, t, text_keys[i], WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    wl_display_roundtrip(display);
    close(fd);
}

static uint32_t decode_utf8(const uint8_t **p, const uint8_t *end)
{
    uint8_t b = *(*p)++;
    if (b < 0x80) return b;
    int n = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
    if (n == 0 || *p + n > end) return '?';
    uint32_t cp = b & (0x3Fu >> n);
    for (int i = 0; i < n; i++) {
        if (((*p)[i] & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | ((*p)[i] & 0x3F);
    }
    *p += n;
    return cp;
}

static void vkbd_type_utf8(const char *s)
{
    if (!text_vkbd || !s || !s[0]) return;
    int epoch = input_epoch;
    uint32_t cps[128];
    int ncps = 0;
    const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s);
    while (p < end && ncps < 120) {
        cps[ncps++] = decode_utf8(&p, end);
    }
    for (int i = 0; i < ncps && epoch == input_epoch; i += NTEXT_KEYS) {
        vkbd_type_chunk(cps + i, ncps - i < NTEXT_KEYS ? ncps - i : NTEXT_KEYS);
    }
    if (epoch == input_epoch) restore_passthrough_keymap();
    wl_display_flush(display);
}

/* Evdev keyboard monitoring & EVIOCGRAB */
static uint8_t kbd_down[MAX_KBDS][KEY_MAX + 1];
static uint8_t kbd_forwarded[MAX_KBDS][KEY_MAX + 1]; /* the layout it went through, + 1; 0: not passed on */
static uint8_t kbd_caps[MAX_KBDS][KEY_MAX + 1]; /* this press is Caps (JIS: Shift+Eisu), through its release */
static int grabbed, want_grab;
static int keys_down;
static int shift, ctrl, alt, meta, altgr; /* altgr: right Alt where the layout makes it AltGr */

static void update_xkb_modifiers(void)
{
    struct xkb_state *st = layouts[cur_layout].st;
    shift = st && xkb_state_mod_name_is_active(st, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
    ctrl = st && xkb_state_mod_name_is_active(st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
    alt = st && xkb_state_mod_name_is_active(st, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
    meta = st && xkb_state_mod_name_is_active(st, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0;
    altgr = st && xkb_state_mod_name_is_active(st, "Mod5", XKB_STATE_MODS_EFFECTIVE) > 0;
}

/* The keyboard is the bar's in the input method's modes; in direct input too
 * while a keyboard types in a layout not the system's, else the compositor
 * would give it the system's. */
static int grab_wanted(void)
{
    for (int i = 0; i < nkbds; i++) {
        if (kbd_layout[i] != 0) return 1;
    }
    return ime_mode() != IME_MODE_RAW;
}

static void reset_keyboard_state(void)
{
    input_epoch++;
    clear_virtual_keys();
    for (int l = 0; l < nlayouts; l++) {
        struct layout *y = &layouts[l];
        for (int code = 1; code <= KEY_MAX; code++) {
            if (y->fed[code]) xkb_state_update_key(y->st, (xkb_keycode_t)code + 8, XKB_KEY_UP);
        }
        memset(y->fed, 0, sizeof y->fed);
        xkb_state_update_mask(y->st, 0, 0, xkb_state_serialize_mods(y->st, XKB_STATE_MODS_LOCKED), 0, 0, 0);
    }
    memset(kbd_down, 0, sizeof kbd_down);
    memset(kbd_caps, 0, sizeof kbd_caps);
    keys_down = 0;
    if (compose_st) xkb_compose_state_reset(compose_st);
    update_xkb_modifiers();
}

static void reconcile_keyboards(const int *next, int nnext)
{
    int old[MAX_KBDS], old_layout[MAX_KBDS], nold = nkbds;
    struct stat identity[MAX_KBDS];
    uint8_t held[MAX_KBDS][KEY_MAX + 1], forwarded[MAX_KBDS][KEY_MAX + 1], capkeys[MAX_KBDS][KEY_MAX + 1];
    memset(identity, 0, sizeof identity);
    for (int i = 0; i < nold; i++) fstat(kbds[i], &identity[i]);
    memcpy(old, kbds, sizeof old);
    memcpy(old_layout, kbd_layout, sizeof old_layout);
    memcpy(held, kbd_down, sizeof held);
    memcpy(forwarded, kbd_forwarded, sizeof forwarded);
    memcpy(capkeys, kbd_caps, sizeof capkeys);
    if (next) {
        nkbds = nnext > MAX_KBDS ? MAX_KBDS : nnext;
        memcpy(kbds, next, (size_t)nkbds * sizeof *kbds);
    } else {
        keys_refresh(); /* Includes separate brightness, audio and vendor devices. */
        nkbds = keys_fds(kbds, MAX_KBDS);
    }
    memset(kbd_down, 0, sizeof kbd_down);
    memset(kbd_forwarded, 0, sizeof kbd_forwarded);
    memset(kbd_caps, 0, sizeof kbd_caps);
    int changed = nold != nkbds, removed[MAX_KBDS] = { 0 }, kept[MAX_KBDS] = { 0 };
    for (int i = 0; i < nold; i++) {
        int found = 0;
        for (int j = 0; j < nkbds; j++) {
            struct stat current;
            if (old[i] == kbds[j] && fstat(kbds[j], &current) == 0 &&
                identity[i].st_dev == current.st_dev && identity[i].st_ino == current.st_ino &&
                identity[i].st_rdev == current.st_rdev) {
                memcpy(kbd_down[j], held[i], sizeof held[i]);
                memcpy(kbd_forwarded[j], forwarded[i], sizeof forwarded[i]);
                memcpy(kbd_caps[j], capkeys[i], sizeof capkeys[i]);
                kbd_layout[j] = old_layout[i];
                kept[j] = found = 1;
                break;
            }
        }
        if (!found) removed[i] = changed = 1;
    }
    for (int j = 0; j < nkbds; j++) {
        if (!kept[j]) kbd_layout[j] = layout_of(kbds[j]);
    }
    for (int i = 0; i < nold; i++) {
        if (!removed[i]) continue;
        for (int code = 1; code <= KEY_MAX; code++) {
            if (held[i][code]) feed_key(old_layout[i], code, 0);
            int fl = forwarded[i][code] - 1;
            if (fl >= 0 && layouts[fl].down[code] && --layouts[fl].down[code] == 0 && layouts[fl].vkbd)
                zwp_virtual_keyboard_v1_key(use_layout(fl), now_ms32(), (uint32_t)code, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
    }
    update_xkb_modifiers();
    if (changed && layouts[active_layout].vkbd) {
        restore_passthrough_keymap();
        wl_display_flush(display);
    }
    if (changed) say("locachy-bar: watching %d keyboard and function-key devices", nkbds);
    keys_down = 0;
    for (int i = 0; i < nkbds; i++) {
        for (int code = 1; code <= KEY_MAX; code++) keys_down += kbd_down[i][code];
        ioctl(kbds[i], EVIOCGRAB, grabbed ? (void *)1 : NULL);
    }
    want_grab = grab_wanted();
    if (keys_down == 0 && active && grabbed != want_grab) {
        set_grab(want_grab);
    }
}

static void open_keyboards(void)
{
    reconcile_keyboards(NULL, 0);
}

/* Each layout's keys passed on, released; its modifiers none; the seat's
 * layout last, so that the seat keeps it. */
static void clear_virtual_keys(void)
{
    memset(kbd_forwarded, 0, sizeof kbd_forwarded);
    uint32_t t = now_ms32();
    for (int n = 0; n < nlayouts; n++) {
        int l = (active_layout + 1 + n) % nlayouts;
        struct layout *y = &layouts[l];
        if (!y->vkbd) {
            memset(y->down, 0, sizeof y->down);
            continue;
        }
        for (int c = 0; c <= KEY_MAX; c++) {
            if (y->down[c]) {
                y->down[c] = 0;
                zwp_virtual_keyboard_v1_key(y->vkbd, t, (uint32_t)c, WL_KEYBOARD_KEY_STATE_RELEASED);
            }
        }
        zwp_virtual_keyboard_v1_modifiers(y->vkbd, 0, 0, 0, 0);
        y->sent_valid = 0;
        wl_display_flush(display);
    }
}

static void set_grab(int on)
{
    /* A release arriving during acquisition must run after the whole set,
     * not ungrab its first device and leave a later one grabbed again. */
    sigset_t release, previous;
    sigemptyset(&release);
    sigaddset(&release, SIGUSR2);
    sigprocmask(SIG_BLOCK, &release, &previous);
    on = on && active; /* a configuration reload never acquires a released seat */
    if (!on) clear_virtual_keys();
    if (grabbed != on) {
        for (int i = 0; i < nkbds; i++) {
            ioctl(kbds[i], EVIOCGRAB, on ? (void *)1 : NULL);
        }
        grabbed = on;
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);
}


/* locachy-seat: the seat has been released (home is on the screen). */
static void on_sigusr2(int sig)
{
    (void)sig;
    for (int i = 0; i < nkbds; i++) {
        ioctl(kbds[i], EVIOCGRAB, NULL);
    }
    grabbed = 0;
    sig_release = 1;
    active = 0;
}

/* locachy-seat: the seat is back. */
static void on_sigusr1(int sig)
{
    (void)sig;
    sig_regrab = 1;
    active = 1;
}

/* Settings may change the layouts while a direct program owns the seat.
 * Reloading configuration does not grant the bar ownership of its input. */
static void on_sighup(int sig)
{
    (void)sig;
    sig_reload = 1;
}

static void register_keyboard_signals(void)
{
    signal(SIGUSR2, on_sigusr2);
    signal(SIGUSR1, on_sigusr1);
    signal(SIGHUP, on_sighup); /* settings changes configuration, not seat ownership */
}

static void keyboard_signals(void)
{
    if (sig_release) {
        sig_release = 0;
        reset_keyboard_state();
    }
    if (sig_regrab || sig_reload) {
        sig_regrab = sig_reload = 0;
        reset_keyboard_state();
        base_language_reload();
        keys_reload();
        load_width_keys();
        init_xkb();
        if (active) restore_passthrough_keymap();
        bars_dirty = 1;
        want_grab = grab_wanted();
        set_grab(want_grab);
    }
}

static int translate_code(int fd, int code)
{
    switch (code) {
    case KEY_UP:       return K_UP;
    case KEY_DOWN:     return K_DOWN;
    case KEY_LEFT:     return K_LEFT;
    case KEY_RIGHT:    return K_RIGHT;
    case KEY_PAGEUP:   return K_PGUP;
    case KEY_PAGEDOWN: return K_PGDN;
    case KEY_HOME:     return K_BOL;
    case KEY_END:      return K_EOL;
    case KEY_DELETE:   return K_DEL;
    case KEY_KPENTER:  return '\n';
    }
    if (ctrl && !alt && !shift && code >= KEY_1 && code <= KEY_0) {
        return K_CTRL_0 + (code == KEY_0 ? 0 : code - KEY_1 + 1);
    }
    int c = keys_device_char(fd, code, shift); /* the keyboard's key table's, as in the direct programs */
    if (ctrl && (((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= '[' && c <= '_'))) {
        return c & 0x1F;
    }
    if (caps_locked() && ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')) {
        c ^= 0x20; /* xkb's Lock: letters only */
    }
    return c;
}

/* Chinese mode's width, besides key_ime_width (which labwc consumes in every
 * application): rc.conf's key_ime_width_cn, keys that switch it only in
 * Chinese mode while the keyboard is the bar's and otherwise type as usual,
 * so labwc does not bind them. Written as the other key_* settings: keys
 * separated by blanks or commas, each its modifiers and a key joined by +,
 * the key a character or a keysym name; matched by the key's symbol (its
 * first level) in the layout of the keyboard it is on. Read at start and
 * when settings changes rc.conf. */
#define MAX_WIDTH_KEYS 8
static struct width_key {
    xkb_keysym_t sym;
    int ctrl, alt, shift, meta;
} width_keys[MAX_WIDTH_KEYS];
static int nwidth_keys;

static void on_width_rc(const char *name, const char *value, void *arg)
{
    char (*spec)[128] = arg;
    if (strcmp(name, "key_ime_width_cn") == 0) snprintf(*spec, sizeof *spec, "%s", value);
}

static void load_width_keys(void)
{
    char spec[128] = "Ctrl+. Shift+Space", *save = NULL;
    rc_read(on_width_rc, &spec);
    for (char *p = spec; *p; p++) {
        if (*p == ',') *p = ' ';
    }
    nwidth_keys = 0;
    for (char *tok = strtok_r(spec, " \t", &save); tok && nwidth_keys < MAX_WIDTH_KEYS;
         tok = strtok_r(NULL, " \t", &save)) {
        struct width_key k = { .sym = XKB_KEY_NoSymbol };
        char *part_save = NULL;
        for (char *part = strtok_r(tok, "+", &part_save); part; part = strtok_r(NULL, "+", &part_save)) {
            if (strcasecmp(part, "Ctrl") == 0 || strcasecmp(part, "Control") == 0) {
                k.ctrl = 1;
            } else if (strcasecmp(part, "Alt") == 0) {
                k.alt = 1;
            } else if (strcasecmp(part, "Shift") == 0) {
                k.shift = 1;
            } else if (strcasecmp(part, "Super") == 0 || strcasecmp(part, "Meta") == 0 ||
                       strcasecmp(part, "Win") == 0) {
                k.meta = 1;
            } else {
                k.sym = part[1] ? xkb_keysym_from_name(part, XKB_KEYSYM_CASE_INSENSITIVE) :
                                  xkb_utf32_to_keysym((uint32_t)tolower((unsigned char)part[0]));
            }
        }
        if (k.sym != XKB_KEY_NoSymbol) width_keys[nwidth_keys++] = k;
    }
}

/* Whether key code of a keyboard of layout l, with the modifiers held now,
 * is one of key_ime_width_cn's. */
static int width_key(int l, int code)
{
    struct layout *y = &layouts[l];
    xkb_keycode_t key = (xkb_keycode_t)code + 8;
    const xkb_keysym_t *syms = NULL;
    int n = y->st ? xkb_keymap_key_get_syms_by_level(y->map, key, xkb_state_key_get_layout(y->st, key), 0, &syms) : 0;
    for (int i = 0; n == 1 && i < nwidth_keys; i++) {
        const struct width_key *k = &width_keys[i];
        if (k->sym == syms[0] && k->ctrl == ctrl && k->alt == alt && k->shift == shift && k->meta == meta) {
            return 1;
        }
    }
    return 0;
}

static void notice(const char *msg)
{
    snprintf(toast_msg, sizeof toast_msg, "%s", msg);
    toast_expire = uptime() + 0.8;
    toast_active = 1;
    render_bars(); /* the input method mode */
    render_bot_bar();
    wl_display_flush(display);
}

/*
 * Opening: xdg-open in a container asks here (OPEN_SOCKET, SOCK_SEQPACKET,
 * "open TARGET" or "with CONTAINER TARGET"; the answer "ok CONTAINER" or
 * "error WHY"). The target opens in the container of the user's rule for its
 * type (open.c), the only one that can open it, or the one the user picks in
 * the chooser (no rule and several candidates, or the rule "ask"). Folders
 * go to home's file manager. One request is served at a time.
 */
#define OPEN_DIR "/run/locachy/open"
#define OPEN_SOCKET OPEN_DIR "/socket"
#define CHOICES 24

static int open_fd = -1;
static struct {
    int client;               /* the waiting xdg-open; -1: no chooser */
    char target[4096], type[128], from[OPEN_NAME];
    char names[CHOICES][OPEN_NAME];
    int n, sel, all;
} choose = { .client = -1 };
static void render_chooser(void);
static struct overlay question = { .ns = "locachy-open", .render = render_chooser };

static void open_listen(void)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = OPEN_SOCKET };
    mkdir("/run/locachy", 0755);
    mkdir(OPEN_DIR, 0755);
    unlink(OPEN_SOCKET);
    open_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (open_fd < 0 || bind(open_fd, (struct sockaddr *)&address, sizeof address) < 0 ||
        chmod(OPEN_SOCKET, 0666) < 0 || listen(open_fd, 8) < 0) {
        say("locachy-bar: cannot listen on %s: %s", OPEN_SOCKET, strerror(errno));
        if (open_fd >= 0) {
            close(open_fd);
        }
        open_fd = -1;
    }
}

/* The container a client runs in: its cgroup, app_NAME. */
static void client_container(int client, char *name, size_t size)
{
    struct ucred cred;
    socklen_t len = sizeof cred;
    char path[64], line[256];
    snprintf(name, size, "?");
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
        return;
    }
    snprintf(path, sizeof path, "/proc/%d/cgroup", (int)cred.pid);
    FILE *in = fopen(path, "re");
    if (in && fgets(line, sizeof line, in) && strncmp(line, "0::/app_", 8) == 0) {
        char *s = line + 8;
        s[strcspn(s, "/\n")] = '\0';
        snprintf(name, size, "%.63s", s);
    }
    if (in) {
        fclose(in);
    }
}

static void answer(int client, const char *fmt, const char *arg)
{
    char reply[512];
    int n = snprintf(reply, sizeof reply, fmt, arg);
    send(client, reply, (size_t)(n < (int)sizeof reply ? n : (int)sizeof reply - 1), MSG_NOSIGNAL);
    close(client);
}

/* locachy-app ACTION NAME [TARGET], in the background, its output in the
 * container's log. -1 if it cannot be run. */
static int run_app(const char *action, const char *name, const char *target)
{
    char log[256];
    snprintf(log, sizeof log, "/run/locachy/apps/%s.log", name);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int out = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            dup2(out, STDERR_FILENO);
        }
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
        }
        setenv("LOCACHY_WAYLAND_DISPLAY", "locachy-0", 1);
        execl("/usr/local/bin/locachy-app", "locachy-app", action, name, target, (char *)NULL);
        _exit(127);
    }
    return pid < 0 ? -1 : 0;
}

/* Whether name is one of what open type (open_candidates). */
static int opens_type(const char *name, const char *type)
{
    char names[CHOICES][OPEN_NAME];
    for (int i = open_candidates(type, names, CHOICES) - 1; i >= 0; i--) {
        if (strcmp(names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* locachy-app open NAME TARGET, in the background; then the window to the
 * front (raise: not the tray, not the question, since the program may not
 * have shown the file yet). */
static void deliver(int client, const char *name)
{
    char names[CHOICES][OPEN_NAME], spec[256], *argv[16];
    int known = 0;
    /* A framebuffer program: home opens it, on its own screen, as root. Only
     * one whose .desktop file opens the type: a request (any container can
     * send one) never has its file run by the terminal or the like. A
     * container's path is the host's only in the shared folders. */
    if (open_program(name, choose.target, spec, sizeof spec, argv, 16)) {
        char why[512];
        if (!opens_type(name, choose.type)) {
            say("locachy-bar: refused %s (%s) from %s with %s: it does not open that", choose.target,
                choose.type, choose.from, name);
            answer(client, "error %s does not open files of this type", name);
        } else if (choose.target[0] == '/' && !open_shared(choose.target, why, sizeof why)) {
            answer(client, "error %s", why);
        } else if (open_in_home(name, choose.target) < 0) {
            answer(client, "error cannot ask home to open it%s", "");
        } else {
            say("locachy-bar: open %s (%s) from %s with %s in home", choose.target, choose.type, choose.from, name);
            kill(getppid(), SIGUSR1); /* locachy-seat asks for home */
            answer(client, "ok home%s", "");
        }
        return;
    }
    for (int i = open_containers(names, CHOICES) - 1; i >= 0 && !known; i--) {
        known = strcmp(names[i], name) == 0;
    }
    if (!known) {
        answer(client, "error there is no container %s", name);
        return;
    }
    /* A container sees the shared folders only; a host program, the home. */
    char why[512];
    if (choose.target[0] == '/' && !open_host(name) && !open_shared(choose.target, why, sizeof why)) {
        answer(client, "error %s", why);
        return;
    }
    say("locachy-bar: open %s (%s) from %s in %s", choose.target, choose.type, choose.from, name);
    if (run_app("open", name, choose.target) < 0) {
        answer(client, "error cannot run locachy-app%s", "");
        return;
    }
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = "/run/user/0/locachy-control.sock" };
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    char raise[96];
    int n = snprintf(raise, sizeof raise, "raise %s", name);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof address) == 0) {
        send(fd, raise, (size_t)n, MSG_NOSIGNAL);
    }
    if (fd >= 0) {
        close(fd);
    }
    answer(client, "ok %s", name);
}

/*
 * Starting a program again: asked on the screen when a container runs with no
 * window and nothing of it in the tray shows one (or its tray item does not
 * within 3 s). Only the user knows whether the program shows its window to a
 * second start (one copy at a time, as QQ or WeChat) or starts a second copy;
 * Enter runs locachy-app again, Esc leaves it.
 */
static struct {
    char name[OPEN_NAME], why[96];
    int pending, shown;
    double until;               /* shown by then, or dropped */
    char watch[OPEN_NAME];      /* a tray item was asked for a window of it */
    double watch_until;
} again;

/* One question at a time: the user opening a container (replace) puts its
 * question in place of another; a tray item that brought no window does
 * not take the place of one on the screen. */
static void ask_again(const char *name, const char *why, int replace)
{
    if ((again.pending || again.shown) && strcmp(again.name, name) == 0) {
        return;
    }
    if (again.shown && !replace) {
        return;
    }
    snprintf(again.name, sizeof again.name, "%s", name);
    snprintf(again.why, sizeof again.why, "%s", why);
    again.pending = 1;
    again.until = uptime() + 10.0;
    if (strcmp(again.watch, name) == 0) {
        again.watch[0] = '\0';
    }
    if (again.shown) {
        render_chooser();
    }
}

static void watch_window(const char *name)
{
    snprintf(again.watch, sizeof again.watch, "%s", name);
    again.watch_until = uptime() + 3.0;
}

static void close_again(void)
{
    again.pending = again.shown = 0;
    overlay_destroy(&question);
    set_grab(want_grab);
    wl_display_flush(display);
}

static void again_key(int code)
{
    char name[OPEN_NAME];
    if (code == KEY_ESC) {
        say("locachy-bar: %s left without a window", again.name);
        close_again();
        return;
    }
    if (code != KEY_ENTER && code != KEY_KPENTER) {
        return;
    }
    memcpy(name, again.name, sizeof name);
    close_again();
    say("locachy-bar: %s: its program again", name);
    run_app("again", name, NULL);
}

/* Each turn: a tray item that did not bring a window, a question to show
 * (once the shared display is on the screen), one to drop. */
static void again_tick(void)
{
    if (again.watch[0] && uptime() >= again.watch_until) {
        struct tray *t = tray_find(again.watch);
        if (t && t->nids > 0 && !tray_has_window(t)) {
            ask_again(again.watch, N_("Its tray item did not show a window."), 0);
        }
        again.watch[0] = '\0';
    }
    if (again.shown && !active) {
        close_again(); /* home came to the front meanwhile */
    }
    if (again.pending && !again.shown) {
        if (uptime() > again.until) {
            again.pending = 0;
        } else if (active && choose.client < 0) {
            again.shown = 1;
            set_grab(1); /* the answer is the bar's, not the application's */
            render_chooser();
        }
    }
}

/*
 * The bar's own socket (BAR_SOCKET, root only, SOCK_SEQPACKET): labwc-control
 * asks "show NAME" for a container with no window; "ok WHAT" (its tray item
 * was asked for one, or the user is asked about starting it again) or
 * "error WHY".
 */
#define BAR_SOCKET "/run/user/0/locachy-bar.sock"
static int bar_fd = -1;

static void bar_listen(void)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = BAR_SOCKET };
    unlink(BAR_SOCKET);
    bar_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (bar_fd < 0 || bind(bar_fd, (struct sockaddr *)&address, sizeof address) < 0 ||
        chmod(BAR_SOCKET, 0600) < 0 || listen(bar_fd, 4) < 0) {
        say("locachy-bar: cannot listen on %s: %s", BAR_SOCKET, strerror(errno));
        if (bar_fd >= 0) {
            close(bar_fd);
        }
        bar_fd = -1;
    }
}

static void bar_request(void)
{
    char request[96], names[CHOICES][OPEN_NAME], ids[1][OPEN_WINDOW_ID];
    int client = accept4(bar_fd, NULL, NULL, SOCK_CLOEXEC), known = 0;
    if (client < 0) {
        return;
    }
    struct pollfd wait = { .fd = client, .events = POLLIN };
    ssize_t n = poll(&wait, 1, 100) > 0 ? recv(client, request, sizeof request - 1, 0) : -1;
    if (n <= 5) {
        close(client);
        return;
    }
    request[n] = '\0';
    const char *name = request + 5;
    for (int i = open_containers(names, CHOICES) - 1; i >= 0 && !known; i--) {
        known = strcmp(names[i], name) == 0;
    }
    if (strncmp(request, "show ", 5) != 0 || !known) {
        answer(client, "error no container %s", name);
    } else if (tray_activate(name) == 0) {
        answer(client, "ok from the tray of %s", name);
    } else if (open_window_ids(name, ids, 1) == 0) {
        answer(client, "error %s has no .desktop file to match its windows", name);
    } else {
        ask_again(name, N_("Nothing of it is in the tray to show one."), 1);
        answer(client, "ok asking whether to start %s again", name);
    }
}


/* One line of the chooser: number, container and its program, the default. */
static void chooser_line(const struct overlay *o, int i, char *out, size_t size)
{
    char rule[OPEN_NAME] = "", label[160], fit[160];
    int is_default = open_rule(choose.type, rule, sizeof rule) == OPEN_RULE_CONTAINER &&
                     strcmp(rule, choose.names[i]) == 0;
    open_label(choose.names[i], choose.type, label, sizeof label);
    panel_text_fit(fit, sizeof fit, label, 52 * o->cw, o->font);
    int pad = 52 - (o->cw > 0 ? panel_text_width(fit, o->font) / o->cw : 0);
    snprintf(out, size, " %2d  %s%*s %s ", i + 1, fit, pad > 0 ? pad : 0, "",
             is_default ? _("default") : "");
}

#define CH_COLS 72

static void render_chooser(void)
{
    int rows = again.shown ? 7 : choose.n + (choose.all ? 0 : 1) + 5;
    struct overlay *o = &question;
    if (!overlay_begin(o, CH_COLS, rows)) {
        return;
    }
    char line[256], cut[160];
    const char *base = strrchr(choose.target, '/');
    ovl_clear(o, OVL_BG);
    if (again.shown) {
        /* TRANSLATORS: %s is the container name. */
        snprintf(line, sizeof line, _("%s runs without a window."), again.name);
        ovl_text(o, 1, 0, line, OVL_FG1, OVL_BG);
        ovl_text(o, 1, 1, _(again.why), OVL_FG2, OVL_BG);
        ovl_text(o, 1, 3, _("Start its program again? One that runs only once shows its"), OVL_FG1, OVL_BG);
        ovl_text(o, 1, 4, _("window; any other starts a second copy."), OVL_FG1, OVL_BG);
        /* TRANSLATORS: Enter and Esc are keys; keep them. */
        ovl_text(o, 1, 6, _("Enter: start it again   Esc: leave it"), OVL_FG2, OVL_BG);
        overlay_end(o);
        return;
    }
    panel_text_fit(cut, sizeof cut, base && base[1] ? base + 1 : choose.target, 40 * o->cw, o->font);
    /* TRANSLATORS: %s is a file name or URL. */
    snprintf(line, sizeof line, _("Open %s with:"), cut);
    ovl_text(o, 1, 0, line, OVL_FG1, OVL_BG);
    snprintf(line, sizeof line, "(%s)", choose.type);
    ovl_text(o, 1, 1, line, OVL_FG2, OVL_BG);
    int r = 2;
    for (int i = 0; i < choose.n; i++, r++) {
        chooser_line(o, i, line, sizeof line);
        uint32_t bg = i == choose.sel ? TOAST_BG : OVL_BG;
        ovl_fill(o, 0, r, 0, 1, bg);
        ovl_text(o, 0, r, line, OVL_FG1, bg);
    }
    if (!choose.all) {
        uint32_t bg = choose.sel == choose.n ? TOAST_BG : OVL_BG;
        snprintf(line, sizeof line, " %2d  %s", choose.n + 1, _("Other container..."));
        ovl_fill(o, 0, r, 0, 1, bg);
        ovl_text(o, 0, r++, line, OVL_FG2, bg);
    }
    /* TRANSLATORS: Enter, A and Esc are keys; keep them. */
    ovl_text(o, 1, r + 1, _("Enter: open   A: always use it   Esc: cancel"), OVL_FG2, OVL_BG);
    overlay_end(o);
}

/*
 * Notifications: org.freedesktop.Notifications on each container's bus (the
 * tray's connection). No icons, no action buttons, no sound, no history: a
 * popup at the top right of the focused window's screen, newest on top, at
 * most NOTES, each on the screen for the time its program asked (NOTE_TIME
 * if it left that to the server; until clicked if it said so or it is
 * critical), counted only while the shared display is on the screen. A click
 * runs its default action and brings the container's window; another button
 * dismisses it. A container with a notification has its buttons in the
 * window list marked until one of its windows has the focus.
 */
#define NOTES_NAME "org.freedesktop.Notifications"
#define NOTES_PATH "/org/freedesktop/Notifications"
#define NOTES 3
#define NOTE_ROWS 4
#define NOTE_COLS 40
#define NOTE_TIME 6000 /* ms */
#define NOTE_TEXT  0xFFD0D0D0u
#define NOTE_LINE  0xFF3A4C68u

struct note {
    uint32_t id;
    char container[OPEN_NAME], app[64], summary[160], body[512];
    int has_default;
    int time;        /* ms on the screen; 0: until clicked */
    double shown_at; /* 0: not on the screen yet */
};
static struct note notes[NOTES]; /* newest first */
static int nnotes;
static uint32_t next_note = 1;

static void render_notes(void);
static struct overlay notes_ovl = {
    .ns = "locachy-notes", .render = render_notes, .margin = 8,
    .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
};

static const char notes_xml[] =
    "<node><interface name='org.freedesktop.Notifications'>"
    "<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='as' direction='in'/><arg type='a{sv}' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='u' direction='out'/></method>"
    "<method name='CloseNotification'><arg type='u' direction='in'/></method>"
    "<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
    "<method name='GetServerInformation'><arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "<arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
    "<signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>"
    "<signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>"
    "</interface></node>";

static void note_emit(const char *container, const char *signal, GVariant *params)
{
    struct tray *t = container[0] ? tray_find(container) : &host_notes;
    if (t && t->conn) {
        g_dbus_connection_emit_signal(t->conn, NULL, NOTES_PATH, NOTES_NAME, signal, params, NULL);
    } else {
        g_variant_unref(g_variant_ref_sink(params));
    }
}

/* reason: 1 expired, 2 dismissed by the user, 3 closed by its program. */
static void note_remove(int i, uint32_t reason)
{
    struct note n = notes[i];
    memmove(&notes[i], &notes[i + 1], (size_t)(nnotes - i - 1) * sizeof notes[0]);
    nnotes--;
    note_emit(n.container, "NotificationClosed", g_variant_new("(uu)", n.id, reason));
    render_notes();
}

/* The body's markup (<b>, <a href=...>, &amp; and the like) as plain text. */
static void plain_text(char *out, size_t size, const char *s)
{
    static const struct { const char *name; char c; } entities[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' }, { "&#39;", '\'' },
    };
    size_t n = 0;
    while (*s && n + 1 < size) {
        const char *end = *s == '<' ? strchr(s, '>') : NULL;
        if (end) {
            s = end + 1;
            continue;
        }
        size_t k = 0;
        while (*s == '&' && k < sizeof entities / sizeof entities[0] &&
               strncmp(s, entities[k].name, strlen(entities[k].name)) != 0) {
            k++;
        }
        if (*s == '&' && k < sizeof entities / sizeof entities[0]) {
            out[n++] = entities[k].c;
            s += strlen(entities[k].name);
            continue;
        }
        out[n++] = *s == '\t' || *s == '\r' ? ' ' : *s;
        s++;
    }
    out[n] = '\0';
}

static void notes_call(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                       const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    struct tray *t = tray_of(conn);
    (void)sender;
    (void)path;
    (void)iface;
    (void)data;
    if (t && strcmp(method, "Notify") == 0) {
        const char *app, *icon, *summary, *body, *action;
        uint32_t replaces;
        int32_t timeout;
        guchar urgency = 1;
        GVariantIter *actions;
        GVariant *hints;
        struct note n = { 0 };
        g_variant_get(params, "(&su&s&s&sas@a{sv}i)", &app, &replaces, &icon, &summary, &body, &actions,
                      &hints, &timeout);
        for (int k = 0; g_variant_iter_next(actions, "&s", &action); k++) {
            n.has_default |= k % 2 == 0 && strcmp(action, "default") == 0; /* key, label, ... */
        }
        g_variant_iter_free(actions);
        g_variant_lookup(hints, "urgency", "y", &urgency);
        g_variant_unref(hints);
        snprintf(n.container, sizeof n.container, "%s", t->name);
        snprintf(n.app, sizeof n.app, "%s", app);
        snprintf(n.summary, sizeof n.summary, "%s", summary);
        plain_text(n.body, sizeof n.body, body);
        n.time = urgency == 2 || timeout == 0 ? 0 : timeout < 0 ? NOTE_TIME : timeout;
        int at = -1;
        for (int i = 0; i < nnotes; i++) {
            if (replaces && notes[i].id == replaces && strcmp(notes[i].container, t->name) == 0) {
                at = i;
            }
        }
        if (at >= 0) {
            n.id = replaces;
            notes[at] = n;
        } else {
            n.id = next_note++;
            next_note += next_note == 0;
            if (nnotes == NOTES) {
                note_remove(NOTES - 1, 1); /* the oldest makes room */
            }
            memmove(&notes[1], &notes[0], (size_t)nnotes * sizeof notes[0]);
            notes[0] = n;
            nnotes++;
        }
        t->notified = 1;
        bars_dirty = 1;
        say("locachy-bar: %s: notification %u from %s", t->name, n.id, app);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", n.id));
        render_notes();
    } else if (t && strcmp(method, "CloseNotification") == 0) {
        uint32_t id;
        g_variant_get(params, "(u)", &id);
        for (int i = 0; i < nnotes; i++) {
            if (notes[i].id == id && strcmp(notes[i].container, t->name) == 0) {
                note_remove(i, 3);
                break;
            }
        }
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (strcmp(method, "GetCapabilities") == 0) {
        static const gchar *const caps[] = { "body", "actions", NULL };
        g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", caps));
    } else if (strcmp(method, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ssss)", "locachy-bar", "LOCachy", "1", "1.2"));
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "no method %s", method);
    }
}

/* On each bus, including the host's. A container gets this before its tray
 * watcher: locachy-app starts the program once the watcher is there. */
static void notes_serve(struct tray *t)
{
    static GDBusNodeInfo *info;
    static const GDBusInterfaceVTable vtable = { .method_call = notes_call };
    if (!info && (info = g_dbus_node_info_new_for_xml(notes_xml, NULL)) == NULL) {
        return;
    }
    t->notes_object = g_dbus_connection_register_object(t->conn, NOTES_PATH, info->interfaces[0], &vtable,
                                                        NULL, NULL, NULL);
    t->notes_own = g_bus_own_name_on_connection(t->conn, NOTES_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                                NULL, NULL, NULL, NULL);
}

/* Its bus is going: its notifications go too (nobody to tell). */
static void notes_drop(struct tray *t)
{
    if (t->notes_own) {
        g_bus_unown_name(t->notes_own);
        t->notes_own = 0;
    }
    if (t->notes_object && t->conn) {
        g_dbus_connection_unregister_object(t->conn, t->notes_object);
    }
    t->notes_object = 0;
    t->notified = 0;
    for (int i = nnotes - 1; i >= 0; i--) {
        if (strcmp(notes[i].container, t->name) == 0) {
            memmove(&notes[i], &notes[i + 1], (size_t)(nnotes - i - 1) * sizeof notes[0]);
            nnotes--;
        }
    }
    render_notes();
}

static void host_notes_closed(GDBusConnection *conn, gboolean remote, GError *error, gpointer data)
{
    (void)remote;
    (void)error;
    (void)data;
    if (host_notes.conn != conn) return;
    notes_drop(&host_notes);
    g_object_unref(host_notes.conn);
    host_notes.conn = NULL;
    host_notes.retry_at = uptime() + 1.0;
}

static void host_notes_connected(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    (void)data;
    host_notes.connecting = 0;
    host_notes.conn = g_bus_get_finish(result, NULL);
    if (!host_notes.conn) {
        host_notes.retry_at = uptime() + 5.0;
        return;
    }
    g_dbus_connection_set_exit_on_close(host_notes.conn, FALSE);
    g_signal_connect(host_notes.conn, "closed", G_CALLBACK(host_notes_closed), NULL);
    notes_serve(&host_notes);
}

/* Services use the standard host session bus. Never bind it into an app. */
static void host_notes_tick(void)
{
    if (!host_notes.conn && !host_notes.connecting && uptime() >= host_notes.retry_at) {
        host_notes.connecting = 1;
        g_bus_get(G_BUS_TYPE_SESSION, NULL, host_notes_connected, NULL);
    }
}

static void render_notes(void)
{
    struct overlay *o = &notes_ovl;
    if (nnotes == 0 || !active) {
        if (o->surf) {
            overlay_destroy(o);
            wl_display_flush(display);
        }
        return;
    }
    if (!overlay_begin(o, NOTE_COLS, nnotes * NOTE_ROWS)) {
        return;
    }
    ovl_clear(o, OVL_BG);
    int room = (NOTE_COLS - 2) * o->cw;
    for (int i = 0; i < nnotes; i++) {
        struct note *n = &notes[i];
        char line[600], cut[600];
        int r = i * NOTE_ROWS;
        if (n->shown_at == 0) {
            n->shown_at = uptime();
        }
        if (i > 0) {
            panel_fill(o->px, o->bw, o->bh, o->bw, 0, r * o->ch, o->bw, 1, NOTE_LINE);
        }
        snprintf(line, sizeof line, "%s%s%s", n->container, n->container[0] ? " \xc2\xb7 " : "", n->app);
        panel_text_fit(cut, sizeof cut, line, room, o->font);
        ovl_text(o, 1, r, cut, OVL_FG2, OVL_BG);
        panel_text_fit(cut, sizeof cut, n->summary, room, o->font);
        ovl_text(o, 1, r + 1, cut, OVL_FG1, OVL_BG);
        /* The body in two lines: what fits of its first, then the rest, cut. */
        snprintf(line, sizeof line, "%.*s", (int)strcspn(n->body, "\n"), n->body);
        size_t first = panel_text_cut(line, room, o->font);
        line[first] = '\0';
        ovl_text(o, 1, r + 2, line, NOTE_TEXT, OVL_BG);
        const char *rest = n->body + first;
        rest += strspn(rest, " \n");
        snprintf(line, sizeof line, "%s", rest);
        for (char *c = strchr(line, '\n'); c; c = strchr(c, '\n')) {
            *c = ' ';
        }
        panel_text_fit(cut, sizeof cut, line, room, o->font);
        ovl_text(o, 1, r + 3, cut, NOTE_TEXT, OVL_BG);
    }
    overlay_end(o);
}

/* Each turn: the time on the screen runs out; while home is in front, it
 * does not run, and the popup is not drawn. */
static void notes_tick(void)
{
    static double last;
    double now = uptime();
    for (int i = nnotes - 1; i >= 0; i--) {
        struct note *n = &notes[i];
        if (!active && n->shown_at > 0) {
            n->shown_at += now - last;
        } else if (n->time > 0 && n->shown_at > 0 && now - n->shown_at >= n->time / 1000.0) {
            note_remove(i, 1);
        }
    }
    last = now;
    if ((!active && notes_ovl.surf) || (active && nnotes > 0 && !notes_ovl.surf)) {
        render_notes();
    }
}

/* Container name's window to the front: the one that had the focus last;
 * with none, its tray item is asked for one. */
static void show_container(const char *name)
{
    struct window *best = NULL;
    for (struct window *w = windows; w; w = w->next) {
        if (strcmp(w->container, name) == 0 && (!best || w->focused_at > best->focused_at)) {
            best = w;
        }
    }
    if (!best) {
        tray_activate(name);
        return;
    }
    if (best->minimized) {
        zwlr_foreign_toplevel_handle_v1_unset_minimized(best->handle);
    }
    zwlr_foreign_toplevel_handle_v1_activate(best->handle, wl_seat_obj);
    wl_display_flush(display);
}

static struct wl_surface *notes_surface(void)
{
    return notes_ovl.surf;
}

/* A click at y on the popup (surface coordinates). */
static void notes_click(double y, uint32_t button)
{
    struct overlay *o = &notes_ovl;
    uint32_t s120 = o->viewport ? o->scale120 : 120;
    int i = o->ch > 0 ? (int)(y * s120 / 120) / (NOTE_ROWS * o->ch) : -1;
    if (i < 0 || i >= nnotes) {
        return;
    }
    struct note n = notes[i];
    if (button == BTN_LEFT) {
        say("locachy-bar: %s: notification %u clicked", n.container, n.id);
        if (n.has_default) {
            note_emit(n.container, "ActionInvoked", g_variant_new("(us)", n.id, "default"));
        }
        if (n.container[0]) show_container(n.container);
    }
    note_remove(i, 2);
}

/* An overlay on the screen goes with the focus to another screen. */
static void overlays_follow(void)
{
    struct wl_output *out = focused_output();
    struct overlay *all[] = { &ime_bar, &question, &notes_ovl };
    for (int i = 0; i < 3; i++) {
        if (all[i]->surf && out && out != all[i]->output) {
            all[i]->render();
        }
    }
}

static void close_chooser(void)
{
    choose.client = -1;
    overlay_destroy(&question);
    set_grab(want_grab);
    wl_display_flush(display);
}

static void open_chooser(int client)
{
    char rule[OPEN_NAME] = "";
    if (!active) {
        answer(client, "error the shared display is not on the screen to choose the program%s", "");
        return;
    }
    choose.client = client;
    choose.all = 0;
    choose.n = open_candidates(choose.type, choose.names, CHOICES);
    if (choose.n == 0) {
        choose.all = 1;
        choose.n = open_containers(choose.names, CHOICES);
    }
    choose.sel = 0;
    if (open_rule(choose.type, rule, sizeof rule) == OPEN_RULE_CONTAINER) {
        for (int i = 0; i < choose.n; i++) {
            if (strcmp(choose.names[i], rule) == 0) {
                choose.sel = i;
            }
        }
    }
    set_grab(1); /* the choice is the bar's, not the application's */
    render_chooser();
}

/* A key while the chooser is up. */
static void chooser_key(int code)
{
    int last = choose.n + (choose.all ? 0 : 1) - 1;
    if (again.shown) {
        again_key(code);
        return;
    }
    if (code == KEY_ESC) {
        answer(choose.client, "error cancelled%s", "");
        close_chooser();
        return;
    }
    if (code == KEY_UP && choose.sel > 0) {
        choose.sel--;
    } else if (code == KEY_DOWN && choose.sel < last) {
        choose.sel++;
    } else if (code >= KEY_1 && code <= KEY_9 && code - KEY_1 <= last) {
        choose.sel = code - KEY_1;
    } else if (code == KEY_ENTER || code == KEY_KPENTER || code == KEY_A) {
        if (choose.sel == choose.n) { /* Other container... */
            choose.all = 1;
            choose.n = open_containers(choose.names, CHOICES);
            choose.sel = 0;
            overlay_destroy(&question);
            render_chooser();
            return;
        }
        if (code == KEY_A) {
            open_set_rule(choose.type, choose.names[choose.sel]);
        }
        int client = choose.client;
        char name[OPEN_NAME];
        snprintf(name, sizeof name, "%s", choose.names[choose.sel]);
        close_chooser();
        deliver(client, name);
        return;
    } else {
        return;
    }
    render_chooser();
}

static void open_request(void)
{
    int client = accept4(open_fd, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) {
        return;
    }
    char request[4300], with[OPEN_NAME] = "", rule[OPEN_NAME];
    struct pollfd wait = { .fd = client, .events = POLLIN };
    ssize_t n = poll(&wait, 1, 500) > 0 ? recv(client, request, sizeof request - 1, 0) : -1;
    if (n <= 0) {
        close(client);
        return;
    }
    request[n] = '\0';
    if (choose.client >= 0 || again.shown) {
        answer(client, "error a question is on the screen%s", "");
        return;
    }
    const char *target;
    if (strncmp(request, "open ", 5) == 0) {
        target = request + 5;
    } else if (strncmp(request, "with ", 5) == 0 && strchr(request + 5, ' ')) {
        snprintf(with, sizeof with, "%.*s", (int)strcspn(request + 5, " "), request + 5);
        target = strchr(request + 5, ' ') + 1;
    } else {
        answer(client, "error unknown request%s", "");
        return;
    }
    client_container(client, choose.from, sizeof choose.from);
    if (open_normalize(target, choose.target, sizeof choose.target) < 0) {
        answer(client, "error %s is neither a path nor a URL", target);
        return;
    }
    if (open_type(choose.target, choose.type, sizeof choose.type) < 0) {
        answer(client, "error cannot tell what %s is", choose.target);
        return;
    }
    if (with[0]) {
        deliver(client, with);
    } else if (strcmp(choose.type, "inode/directory") == 0) {
        /* Folders open in home's file manager: home reads the request when it is shown. */
        FILE *out = fopen(FILES_REQUEST, "we");
        if (out) {
            fprintf(out, "%s\n", choose.target);
            fclose(out);
        }
        say("locachy-bar: open folder %s from %s in home's file manager", choose.target, choose.from);
        kill(getppid(), SIGUSR1); /* locachy-seat asks for home */
        answer(client, "ok home%s", "");
    } else {
        int kind = open_rule(choose.type, rule, sizeof rule);
        char names[2][OPEN_NAME];
        if (kind == OPEN_RULE_CONTAINER) {
            deliver(client, rule);
        } else if (kind == OPEN_NO_RULE && open_candidates(choose.type, names, 2) == 1) {
            deliver(client, names[0]);
        } else {
            open_chooser(client);
        }
    }
}

/* X11 modifier bits as librime's standard front ends use them. Num Lock
 * stays in XKB's resolved keysym (KP_1 versus KP_End), not in Rime's mask. */
static unsigned int rime_modifiers(struct xkb_state *st)
{
    static const struct { const char *name; unsigned int mask; } mods[] = {
        { XKB_MOD_NAME_SHIFT, 1u << 0 }, { XKB_MOD_NAME_CAPS, 1u << 1 },
        { XKB_MOD_NAME_CTRL, 1u << 2 }, { XKB_MOD_NAME_ALT, 1u << 3 },
        { XKB_MOD_NAME_LOGO, 1u << 6 },
    };
    unsigned int mask = 0;
    for (size_t i = 0; i < sizeof mods / sizeof mods[0]; i++) {
        if (xkb_state_mod_name_is_active(st, mods[i].name, XKB_STATE_MODS_EFFECTIVE) > 0) mask |= mods[i].mask;
    }
    return mask;
}

/* Every Rime event can commit, including a modifier or its release. Deliver
 * it now, before forwarding the event, and abandon stale work after handover. */
static int drain_ime_commit(int epoch)
{
    const char *commit = ime_get_commit();
    if (commit && commit[0]) vkbd_type_utf8(commit);
    return epoch == input_epoch;
}

static void handle_kbd_event(int fd)
{
    struct input_event ev;
    while (read(fd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type != EV_KEY || !active || ev.code > KEY_MAX || !nlayouts || ev.value < 0 || ev.value > 2) continue;
        int device = -1;
        for (int i = 0; i < nkbds; i++) if (kbds[i] == fd) device = i;
        if (device < 0) continue;
        int dl = kbd_layout[device];
        cur_layout = dl;
        int caps_before = caps_locked(), was_down = kbd_down[device][ev.code];
        xkb_keysym_t sym_before = xkb_state_key_get_one_sym(layouts[dl].st, (xkb_keycode_t)ev.code + 8);
        unsigned int mask_before = rime_modifiers(layouts[dl].st);
        if (ev.value != 2) {
            int down = ev.value != 0;
            keys_down += down - was_down;
            kbd_down[device][ev.code] = (uint8_t)down;
            if (down != was_down) {
                feed_key(dl, ev.code, down);
                share_locks(dl);
            }
        }
        /* A shifted lock such as JIS Shift+Eisu has no unshifted Caps role.
         * Follow the actual XKB lock transition, and its matching release. */
        int caps_after = caps_locked();
        if (ev.value == 1 && (caps_after != caps_before || sym_before == XKB_KEY_Caps_Lock))
            kbd_caps[device][ev.code] = 1;
        int caps_key = kbd_caps[device][ev.code];
        if (ev.value == 0) kbd_caps[device][ev.code] = 0;
        /* Num Lock passes on as a key, as before; its lock is in every layout's state. */
        int role = ev.code < 256 ? layouts[dl].role[ev.code] : 0, is_mod = (role & ~ROLE_NUM) != 0 || caps_key;
        update_xkb_modifiers();
        if ((caps_key || (role & KEYS_CAPS)) && ev.value != 2) {
            /* Rime as its front ends tell it: the key with the lock before
             * it, then the lock after; it ends what is being composed. */
            ime_set_caps(caps_after);
            if (grabbed) {
                render_bot_bar();
            }
        }

        /* The modifiers, to the seat's layout; another layout gets them
         * with its next key. */
        if (grabbed && is_mod && layouts[active_layout].vkbd) {
            use_layout(active_layout);
            wl_display_flush(display);
        }

        if (ev.value == 0) {
            if (grabbed) {
                int epoch = input_epoch;
                ime_key_event(caps_key ? XKB_KEY_Caps_Lock : sym_before, mask_before | (1u << 30));
                if (!drain_ime_commit(epoch)) continue;
            }
            int fl = kbd_forwarded[device][ev.code] - 1;
            if (fl >= 0) {
                kbd_forwarded[device][ev.code] = 0;
                if (layouts[fl].down[ev.code] && --layouts[fl].down[ev.code] == 0 && layouts[fl].vkbd) {
                    zwp_virtual_keyboard_v1_key(use_layout(fl), now_ms32(), ev.code, WL_KEYBOARD_KEY_STATE_RELEASED);
                    wl_display_flush(display);
                }
            }
            if (keys_down == 0 && grabbed != want_grab) {
                set_grab(want_grab);
            }
            continue;
        }

        if (choose.client >= 0 || again.shown) {
            if (!is_mod) {
                chooser_key(ev.code);
            }
            continue;
        }

        int act = keys_device_action(fd, ev.code, ctrl, alt, shift, meta);
        if (act == K_ACT_HOME || act == K_ACT_LOCK) {
            /* Process 1 sees the key itself unless it is grabbed here. */
            if (ev.value == 1 && grabbed) {
                set_grab(0);
                /* locachy-seat asks for home, or to lock */
                kill(getppid(), act == K_ACT_HOME ? SIGUSR1 : SIGUSR2);
            }
            continue;
        }
        if (act == K_CTRL_SPACE || act == K_ACT_IME_CN || act == K_ACT_IME_RAW || act == K_ACT_IME_WIDTH) {
            const char *msg;
            if (ev.value == 1 && ime_action(act, &msg)) {
                want_grab = grab_wanted();
                if (keys_down == 0) {
                    set_grab(want_grab);
                }
                if (msg) {
                    notice(msg);
                }
            }
            continue;
        }
        if (act == K_VOL_UP || act == K_VOL_DOWN || act == K_VOL_MUTE || act == K_MIC_MUTE) {
            char msg[64];
            if ((ev.value == 1 || act == K_VOL_UP || act == K_VOL_DOWN) &&
                base_audio_key(act, msg, sizeof msg) == 0) {
                notice(msg);
            }
            continue;
        }
        if (act == K_BRI_UP || act == K_BRI_DOWN) {
            char msg[64];
            if ((ev.value == 1 || ev.value == 2) &&
                base_brightness_key(act, msg, sizeof msg) == 0) {
                notice(msg);
            }
            continue;
        }
        if (!grabbed) continue;
        if (is_mod || (role & ROLE_NUM)) {
            int epoch = input_epoch;
            ime_key_event(caps_key ? XKB_KEY_Caps_Lock : sym_before, mask_before);
            if (!drain_ime_commit(epoch)) continue;
            render_bot_bar();
            wl_display_flush(display);
            continue;
        }
        /* Chinese mode's width: key_ime_width_cn (Ctrl+. and Shift+Space, as in
         * Chinese input methods). */
        if (ime_mode() == IME_MODE_CN && width_key(dl, ev.code)) {
            const char *msg = ev.value == 1 ? ime_toggle_width() : NULL;
            if (msg) {
                notice(msg);
            }
            continue;
        }

        /* A dead key and what follows it: composed here, as fcitx5 does
         * before Rime, and typed; while composing, and when the sequence
         * goes nowhere, the keys are dropped (libxkbcommon's cancel). In
         * direct input the keys pass on, and the application composes. */
        if (compose_st && ev.value == 1 && !ctrl && !alt && !meta && ime_mode() != IME_MODE_RAW &&
            xkb_compose_state_feed(compose_st, xkb_state_key_get_one_sym(layouts[dl].st,
                                                                         (xkb_keycode_t)ev.code + 8)) ==
                XKB_COMPOSE_FEED_ACCEPTED) {
            enum xkb_compose_status status = xkb_compose_state_get_status(compose_st);
            if (status == XKB_COMPOSE_COMPOSED) {
                char utf8[64];
                xkb_compose_state_get_utf8(compose_st, utf8, sizeof utf8);
                if (utf8[0]) vkbd_type_utf8(utf8);
                wl_display_flush(display);
            }
            if (status != XKB_COMPOSE_NOTHING) {
                if (status != XKB_COMPOSE_COMPOSING) xkb_compose_state_reset(compose_st);
                continue;
            }
        }

        int epoch = input_epoch;
        int k = translate_code(fd, ev.code);
        int candidate = !alt && !meta && !altgr &&
                        ((!ctrl && k >= '0' && k <= '9') || (k >= K_CTRL_0 && k <= K_CTRL_0 + 9));
        int taken = candidate ? ime_process_key(k) : ime_key_event(sym_before, mask_before);
        if (!drain_ime_commit(epoch)) continue;
        if (taken) {
            render_bot_bar();
            wl_display_flush(display);
        } else if (ev.code <= KEY_MAX && layout_vkbd(&layouts[dl])) {
            /* Through the keyboard's layout's virtual keyboard; released
             * and repeated through the one it was pressed on. */
            uint32_t t = now_ms32();
            if (ev.value == 1 && !kbd_forwarded[device][ev.code]) {
                kbd_forwarded[device][ev.code] = (uint8_t)(dl + 1);
                if (layouts[dl].down[ev.code]++ == 0)
                    zwp_virtual_keyboard_v1_key(use_layout(dl), t, ev.code, WL_KEYBOARD_KEY_STATE_PRESSED);
            } else if (ev.value == 2 && kbd_forwarded[device][ev.code]) {
                struct zwp_virtual_keyboard_v1 *k = use_layout(kbd_forwarded[device][ev.code] - 1);
                zwp_virtual_keyboard_v1_key(k, t, ev.code, WL_KEYBOARD_KEY_STATE_RELEASED);
                zwp_virtual_keyboard_v1_key(k, t, ev.code, WL_KEYBOARD_KEY_STATE_PRESSED);
            }
            render_bot_bar();
            wl_display_flush(display);
        }
    }
}

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver)
{
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        wl_seat_obj = wl_registry_bind(reg, name, &wl_seat_interface, 1);
        wl_seat_add_listener(wl_seat_obj, &seat_listener, NULL);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        vkbd_mgr = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0 && !toplevel_mgr) {
        toplevel_mgr = wl_registry_bind(reg, name, &zwlr_foreign_toplevel_manager_v1_interface,
                                        ver < 3 ? ver : 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_mgr, &toplevels_listener, NULL);
    } else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        scale_mgr = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    } else if (strcmp(iface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        relative_mgr = wl_registry_bind(reg, name, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        struct bar *b = calloc(1, sizeof *b);
        if (b) {
            b->name = name;
            b->output = wl_registry_bind(reg, name, &wl_output_interface, 1);
            b->w = 2560;
            b->h = PANEL_H;
            b->since = -10;
            b->next = bars;
            bars = b;
            say("locachy-bar: screen %u appeared", name);
        }
    }
}

static void reg_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    for (struct bar **p = &bars; *p; p = &(*p)->next) {
        struct bar *b = *p;
        if (b->name == name) {
            say("locachy-bar: screen %u went away", name);
            reset_keyboard_state();
            *p = b->next;
            struct overlay *all[] = { &ime_bar, &question, &notes_ovl };
            for (int i = 0; i < 3; i++) {
                if (all[i]->output == b->output) {
                    overlay_destroy(all[i]);
                    all[i]->output = NULL;
                }
            }
            for (struct window *w = windows; w; w = w->next) {
                for (int i = w->noutputs - 1; i >= 0; i--) {
                    if (w->outputs[i] != b->output) continue;
                    memmove(w->outputs + i, w->outputs + i + 1, (size_t)(w->noutputs - i - 1) * sizeof *w->outputs);
                    w->noutputs--;
                }
                if (w->last == b->output) w->last = NULL;
            }
            destroy_bar_surface(b);
            wl_output_destroy(b->output);
            free(b);
            bars_dirty = 1;
            return;
        }
    }
}

static const struct wl_registry_listener reg_listener = {
    .global = reg_global,
    .global_remove = reg_remove,
};

int main(void)
{
    for (int tries = 0; tries < 50; tries++) {
        display = wl_display_connect(NULL);
        if (display) break;
        usleep(50000);
    }
    if (!display) return 1;

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &reg_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !layer_shell) return 1;

    cursor_theme = wl_cursor_theme_load(NULL, 24, shm);
    cursor_surface = wl_compositor_create_surface(compositor);
    init_xkb();
    if (!nlayouts) return 1;
    load_width_keys();
    ime_init(NULL, NULL);
    open_keyboards();
    register_keyboard_signals();

    if (vkbd_mgr && wl_seat_obj) {
        text_vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkbd_mgr, wl_seat_obj);
        restore_passthrough_keymap(); /* the system's layout's virtual keyboard */
    }

    check_bars();
    wl_display_roundtrip(display);
    open_listen();
    bar_listen();

    int wl_fd = wl_display_get_fd(display);
    double last_top = 0, last_keyboards = 0;
    /* GLib's sources (the tray's D-Bus connections) are polled here too. */
    GMainContext *gctx = g_main_context_default();
    g_main_context_acquire(gctx);

    for (;;) {
        keyboard_signals();
        if (active && uptime() - last_keyboards >= 1.0) {
            open_keyboards();
            last_keyboards = uptime();
        }
        wl_display_flush(display);
        GPollFD gfds[32];
        gint gmax, gtimeout;
        g_main_context_prepare(gctx, &gmax);
        int ngfds = g_main_context_query(gctx, gmax, &gtimeout, gfds, 32);
        ngfds = ngfds > 32 ? 32 : ngfds;
        struct pollfd p[3 + MAX_KBDS + 32];
        int nbase = 3 + nkbds;
        p[0] = (struct pollfd){ .fd = wl_fd, .events = POLLIN };
        p[1] = (struct pollfd){ .fd = open_fd, .events = POLLIN }; /* -1: ignored */
        p[2] = (struct pollfd){ .fd = bar_fd, .events = POLLIN };
        for (int i = 0; i < nkbds; i++) {
            p[3 + i] = (struct pollfd){ .fd = kbds[i], .events = POLLIN };
        }
        for (int i = 0; i < ngfds; i++) {
            p[nbase + i] = (struct pollfd){ .fd = gfds[i].fd, .events = (short)gfds[i].events };
        }
        int timeout = toast_active ? 50 : 250; /* 250: the tray looks for new buses */
        if (gtimeout >= 0 && gtimeout < timeout) {
            timeout = gtimeout;
        }
        int polled = poll(p, (nfds_t)(nbase + ngfds), timeout);
        for (int i = 0; i < ngfds; i++) {
            gfds[i].revents = polled > 0 ? (gushort)p[nbase + i].revents : 0;
        }
        if (g_main_context_check(gctx, gmax, gfds, ngfds)) {
            g_main_context_dispatch(gctx);
        }
        if (polled < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (wl_display_dispatch(display) < 0) break;
        }
        if (p[1].revents & POLLIN) {
            open_request();
        }
        if (p[2].revents & POLLIN) {
            bar_request();
        }
        for (int i = 0; i < nkbds; i++) {
            if (p[3 + i].revents & POLLIN) {
                handle_kbd_event(kbds[i]);
            }
        }
        if (choose.client >= 0 && !active) { /* home came to the front meanwhile */
            answer(choose.client, "error the shared display left the screen%s", "");
            close_chooser();
        }
        if (toast_active && uptime() >= toast_expire) {
            render_bot_bar();
        }
        tray_scan();
        host_notes_tick();
        again_tick();
        notes_tick();
        check_bars();
        if (bars_dirty || uptime() - last_top >= 1.0) {
            bars_dirty = 0;
            last_top = uptime();
            render_bars();
            overlays_follow();
        }
    }

    set_grab(0);
    if (text_vkbd) zwp_virtual_keyboard_v1_destroy(text_vkbd);
    keys_release();
    ime_done();
    if (open_fd >= 0) {
        unlink(OPEN_SOCKET);
    }
    if (bar_fd >= 0) {
        unlink(BAR_SOCKET);
    }
    g_main_context_release(gctx);
    return 0;
}
