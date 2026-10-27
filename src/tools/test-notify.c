/*
 * test-notify OUT TIMEOUT_MS SUMMARY: a program that sends a notification,
 * for test-bar.sh: Notify on the session bus with a default action and the
 * time TIMEOUT_MS (0: until clicked, -1: the server's). OUT gets "caps
 * CAPABILITIES", "id N", and then "action N KEY" and "closed N REASON" as the
 * server says them.
 */
#include <gio/gio.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *out_path;

static void note(const char *fmt, ...)
{
    va_list ap;
    FILE *out = fopen(out_path, "a");
    if (out) {
        va_start(ap, fmt);
        vfprintf(out, fmt, ap);
        va_end(ap);
        fputc('\n', out);
        fclose(out);
    }
}

static void signal_cb(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                      const gchar *signal, GVariant *params, gpointer data)
{
    guint32 id, reason;
    const char *key;
    (void)conn;
    (void)sender;
    (void)path;
    (void)iface;
    (void)data;
    if (strcmp(signal, "ActionInvoked") == 0) {
        g_variant_get(params, "(u&s)", &id, &key);
        note("action %u %s", id, key);
    } else if (strcmp(signal, "NotificationClosed") == 0) {
        g_variant_get(params, "(uu)", &id, &reason);
        note("closed %u %u", id, reason);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: test-notify OUT TIMEOUT_MS SUMMARY\n");
        return 2;
    }
    out_path = argv[1];
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!conn) {
        note("no bus");
        return 1;
    }
    g_dbus_connection_signal_subscribe(conn, NULL, "org.freedesktop.Notifications", NULL,
                                       "/org/freedesktop/Notifications", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                       signal_cb, NULL, NULL);
    GVariant *caps = g_dbus_connection_call_sync(conn, "org.freedesktop.Notifications",
                                                 "/org/freedesktop/Notifications",
                                                 "org.freedesktop.Notifications", "GetCapabilities", NULL,
                                                 G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (caps) {
        gchar **list = NULL;
        g_variant_get(caps, "(^as)", &list);
        gchar *joined = g_strjoinv(",", list);
        note("caps %s", joined);
        g_free(joined);
        g_strfreev(list);
        g_variant_unref(caps);
    }
    const gchar *actions[] = { "default", "Open", NULL };
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    GVariant *id = g_dbus_connection_call_sync(
        conn, "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
        "Notify", g_variant_new("(susss^asa{sv}i)", "Test app", 0u, "", argv[3],
                                "A <b>body</b> long enough to need its second line on a popup &amp; more",
                                actions, &hints, atoi(argv[2])),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (!id) {
        note("no server");
        return 1;
    }
    guint32 n;
    g_variant_get(id, "(u)", &n);
    note("id %u", n);
    g_variant_unref(id);
    g_main_loop_run(g_main_loop_new(NULL, FALSE));
    return 0;
}
