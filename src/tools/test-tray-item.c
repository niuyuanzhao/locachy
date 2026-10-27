/*
 * test-tray-item OUT [attention]: a program that lives on in a tray, for
 * test-tray.sh: a StatusNotifierItem on the session bus, registered with the
 * watcher. OUT gets "watcher yes|no" (was the watcher there when it started),
 * "registered" or "not registered", and "activated" for each Activate. With
 * attention, its Status is NeedsAttention (as for a new message).
 */
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char xml[] =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='Category' type='s' access='read'/>"
    "<signal name='NewStatus'><arg type='s'/></signal>"
    "</interface></node>";

static const char *out_path;
static int attention;

static void note(const char *line)
{
    FILE *out = fopen(out_path, "a");
    if (out) {
        fprintf(out, "%s\n", line);
        fclose(out);
    }
}

static void call(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                 const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    (void)conn;
    (void)sender;
    (void)path;
    (void)iface;
    (void)params;
    (void)data;
    if (strcmp(method, "Activate") == 0) {
        note("activated");
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *get(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                     const gchar *prop, GError **error, gpointer data)
{
    (void)conn;
    (void)sender;
    (void)path;
    (void)iface;
    (void)error;
    (void)data;
    if (strcmp(prop, "Id") == 0) return g_variant_new_string("test-tray");
    if (strcmp(prop, "Title") == 0) return g_variant_new_string("Test tray");
    if (strcmp(prop, "Status") == 0) return g_variant_new_string(attention ? "NeedsAttention" : "Active");
    return g_variant_new_string("Communications");
}

int main(int argc, char **argv)
{
    static const GDBusInterfaceVTable vtable = { .method_call = call, .get_property = get };
    GError *error = NULL;
    char name[64];
    gboolean watcher = FALSE;
    if (argc < 2) {
        fprintf(stderr, "usage: test-tray-item OUT [attention]\n");
        return 2;
    }
    out_path = argv[1];
    attention = argc > 2;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!conn) {
        note("no bus");
        return 1;
    }
    GVariant *has = g_dbus_connection_call_sync(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                "org.freedesktop.DBus", "NameHasOwner",
                                                g_variant_new("(s)", "org.kde.StatusNotifierWatcher"),
                                                G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (has) {
        g_variant_get(has, "(b)", &watcher);
        g_variant_unref(has);
    }
    note(watcher ? "watcher yes" : "watcher no");
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, NULL);
    g_dbus_connection_register_object(conn, "/StatusNotifierItem", info->interfaces[0], &vtable,
                                      NULL, NULL, NULL);
    snprintf(name, sizeof name, "org.kde.StatusNotifierItem-%d-1", (int)getpid());
    GVariant *owned = g_dbus_connection_call_sync(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "RequestName",
                                                  g_variant_new("(su)", name, 0u), G_VARIANT_TYPE("(u)"),
                                                  G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (owned) {
        g_variant_unref(owned);
    }
    GVariant *registered = g_dbus_connection_call_sync(conn, "org.kde.StatusNotifierWatcher",
                                                       "/StatusNotifierWatcher",
                                                       "org.kde.StatusNotifierWatcher",
                                                       "RegisterStatusNotifierItem",
                                                       g_variant_new("(s)", name), NULL,
                                                       G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    note(registered ? "registered" : "not registered");
    if (registered) {
        g_variant_unref(registered);
    }
    g_main_loop_run(g_main_loop_new(NULL, FALSE));
    return 0;
}
