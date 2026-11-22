/* The Settings backend on an isolated container's private session bus. */
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef LOCACHY_APPEARANCE_DIR
#define LOCACHY_APPEARANCE_DIR "/run/locachy-appearance"
#endif
#ifndef LOCACHY_APPEARANCE_BUS
#define LOCACHY_APPEARANCE_BUS "unix:path=/run/locachy-bus/bus"
#endif

#define NAME "org.freedesktop.impl.portal.desktop.locachy"
#define PATH "/org/freedesktop/portal/desktop"
#define INTERFACE "org.freedesktop.impl.portal.Settings"
#define APPEARANCE "org.freedesktop.appearance"
#define COLOR_SCHEME "color-scheme"
#define ACCENT_COLOR "accent-color"

static const char introspection[] =
    "<node><interface name='" INTERFACE "'>"
    "<method name='Read'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='v' direction='out'/></method>"
    "<method name='ReadAll'><arg type='as' direction='in'/>"
    "<arg type='a{sa{sv}}' direction='out'/></method>"
    "<signal name='SettingChanged'><arg type='s'/><arg type='s'/><arg type='v'/></signal>"
    "<property name='version' type='u' access='read'/>"
    "</interface></node>";

struct appearance {
    guint32 scheme; /* 1: dark, 2: light */
    guint32 accent; /* sRGB, 0xRRGGBB */
};

struct portal {
    GDBusConnection *bus;
    GFile *file;
    struct appearance appearance;
};

static struct appearance read_snapshot(void)
{
    const struct appearance fallback = { 1, 0x80c0ff };
    struct appearance result = fallback;
    char text[128];
    size_t len = 0;
    struct stat st;
    int fd = open(LOCACHY_APPEARANCE_DIR "/settings",
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return fallback;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        while (len < sizeof text) {
            ssize_t n = read(fd, text + len, sizeof text - len);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EINTR) continue;
                len = 0;
                break;
            }
            len += (size_t)n;
        }
    }
    close(fd);
    if (len == sizeof text || memchr(text, '\0', len)) return fallback;
    text[len] = '\0';

    unsigned schemes = 0, accents = 0;
    gboolean scheme_valid = FALSE, accent_valid = FALSE;
    char *rest = text;
    for (char *line; (line = strsep(&rest, "\n"));) {
        if (!*line) continue;
        if (g_str_has_prefix(line, "color_scheme=")) {
            schemes++;
            if (schemes == 1 && (strcmp(line, "color_scheme=\"dark\"") == 0 ||
                                 strcmp(line, "color_scheme=\"light\"") == 0)) {
                result.scheme = strcmp(line, "color_scheme=\"light\"") == 0 ? 2 : 1;
                scheme_valid = TRUE;
            } else {
                scheme_valid = FALSE;
            }
        } else if (g_str_has_prefix(line, "accent_color=")) {
            const char prefix[] = "accent_color=\"#";
            const size_t hex = sizeof prefix - 1;
            accents++;
            if (accents > 1 || !g_str_has_prefix(line, prefix) ||
                strlen(line) != hex + 7 || line[hex + 6] != '"') {
                accent_valid = FALSE;
                continue;
            }
            guint32 rgb = 0;
            accent_valid = TRUE;
            for (size_t i = 0; i < 6; i++) {
                int digit = g_ascii_xdigit_value(line[hex + i]);
                if (digit < 0) {
                    accent_valid = FALSE;
                    break;
                }
                rgb = (rgb << 4) | (guint32)digit;
            }
            if (accent_valid) result.accent = rgb;
        }
    }
    if (!scheme_valid) result.scheme = fallback.scheme;
    if (!accent_valid) result.accent = fallback.accent;
    return result;
}

static GVariant *accent_value(guint32 rgb)
{
    return g_variant_new("(ddd)", (rgb >> 16) / 255.0,
                         ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
}

static void emit_changed(struct portal *portal, const char *key, GVariant *value)
{
    g_dbus_connection_emit_signal(portal->bus, NULL, PATH, INTERFACE, "SettingChanged",
                                  g_variant_new("(ssv)", APPEARANCE, key, value), NULL);
}

static void file_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                         GFileMonitorEvent event, gpointer data)
{
    (void)monitor; (void)event;
    struct portal *portal = data;
    if (!g_file_equal(file, portal->file) &&
        (!other || !g_file_equal(other, portal->file)))
        return;
    struct appearance previous = portal->appearance;
    struct appearance next = read_snapshot();
    portal->appearance = next;
    if (next.scheme != previous.scheme) emit_changed(portal, COLOR_SCHEME, g_variant_new_uint32(next.scheme));
    if (next.accent != previous.accent) emit_changed(portal, ACCENT_COLOR, accent_value(next.accent));
}

static gboolean matches_appearance(GVariant *params)
{
    g_autoptr(GVariant) filters = g_variant_get_child_value(params, 0);
    if (g_variant_n_children(filters) == 0) return TRUE;
    GVariantIter iter;
    const char *filter;
    g_variant_iter_init(&iter, filters);
    while (g_variant_iter_next(&iter, "&s", &filter)) {
        size_t len = strlen(filter);
        if (!len || strcmp(filter, APPEARANCE) == 0 ||
            (filter[len - 1] == '*' && strncmp(filter, APPEARANCE, len - 1) == 0))
            return TRUE;
    }
    return FALSE;
}

static void method_call(GDBusConnection *conn, const char *sender, const char *path,
                        const char *iface, const char *method, GVariant *params,
                        GDBusMethodInvocation *invocation, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface;
    const struct portal *portal = data;
    struct appearance a = portal->appearance;
    if (strcmp(method, "ReadAll") == 0) {
        GVariantBuilder namespaces;
        g_variant_builder_init(&namespaces, G_VARIANT_TYPE("a{sa{sv}}"));
        if (matches_appearance(params)) {
            GVariantBuilder values;
            g_variant_builder_init(&values, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&values, "{sv}", COLOR_SCHEME, g_variant_new_uint32(a.scheme));
            g_variant_builder_add(&values, "{sv}", ACCENT_COLOR, accent_value(a.accent));
            g_variant_builder_add(&namespaces, "{sa{sv}}", APPEARANCE, &values);
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sa{sv}})", &namespaces));
    } else {
        const char *ns, *key;
        GVariant *value = NULL;
        g_variant_get(params, "(&s&s)", &ns, &key);
        if (strcmp(ns, APPEARANCE) == 0) {
            if (strcmp(key, COLOR_SCHEME) == 0) value = g_variant_new_uint32(a.scheme);
            else if (strcmp(key, ACCENT_COLOR) == 0) value = accent_value(a.accent);
        }
        if (value) g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
        else g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.portal.Error.NotFound",
                                                        "Requested setting not found");
    }
}

static GVariant *get_property(GDBusConnection *conn, const char *sender, const char *path,
                              const char *iface, const char *property, GError **error, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)property; (void)error; (void)data;
    return g_variant_new_uint32(1);
}

static void name_lost(GDBusConnection *conn, const char *name, gpointer data)
{
    (void)conn; (void)name;
    g_main_loop_quit(data);
}

int main(void)
{
    if (geteuid() == 0) return 1;
    g_autoptr(GError) error = NULL;
    /* Connect only to the bus in the container's mount namespace. */
    g_autoptr(GDBusConnection) bus = g_dbus_connection_new_for_address_sync(
        LOCACHY_APPEARANCE_BUS, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
    if (!bus) {
        g_printerr("appearance-portal: %s\n", error->message);
        return 1;
    }

    g_autoptr(GFile) dir = g_file_new_for_path(LOCACHY_APPEARANCE_DIR);
    g_autoptr(GFile) file = g_file_get_child(dir, "settings");
    /* Watch the directory before reading, so an atomic replacement during
     * startup cannot leave the service with stale settings. */
    g_autoptr(GFileMonitor) monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, NULL, &error);
    if (!monitor) {
        g_printerr("appearance-portal: %s\n", error->message);
        return 1;
    }
    struct portal portal = { bus, file, read_snapshot() };
    g_signal_connect(monitor, "changed", G_CALLBACK(file_changed), &portal);
    g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(introspection, &error);
    static const GDBusInterfaceVTable vtable = { method_call, get_property, NULL, { 0 } };
    guint registration = info ? g_dbus_connection_register_object(bus, PATH, info->interfaces[0],
                                                                   &vtable, &portal, NULL, &error) : 0;
    if (!registration) {
        g_printerr("appearance-portal: %s\n", error->message);
        return 1;
    }
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    guint owner = g_bus_own_name_on_connection(bus, NAME, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                               NULL, name_lost, loop, NULL);
    g_main_loop_run(loop);
    g_bus_unown_name(owner);
    g_dbus_connection_unregister_object(bus, registration);
    g_file_monitor_cancel(monitor);
    return 0;
}
