/*
 * container-documents (/usr/local/lib/locachy/container-documents): the
 * minimal org.freedesktop.portal.Documents service on a container's private
 * session bus.
 *
 * Containers share user directories directly by bind mount (no FUSE device
 * and no setuid fusermount3 inside the container). Upstream xdg-desktop-portal
 * 1.22+ treats org.freedesktop.portal.Documents as a hard startup dependency;
 * without it, every portal query from Chromium/Edge/GTK activates
 * xdg-desktop-portal, which fails to start xdg-document-portal and exits,
 * causing a continuous D-Bus activation storm.
 *
 * Owning org.freedesktop.portal.Documents and answering GetMountPoint lets
 * xdg-desktop-portal stay resident and serve ScreenCast/Screenshot while
 * cleanly declining file-transfer document operations.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <gio/gio.h>
#include <stdlib.h>
#include <unistd.h>

static const gchar introspect_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.portal.Documents'>"
    "    <property name='version' type='u' access='read'/>"
    "    <method name='GetMountPoint'>"
    "      <arg type='ay' name='path' direction='out'/>"
    "    </method>"
    "    <method name='Add'>"
    "      <arg type='h' name='o_path_fd' direction='in'/>"
    "      <arg type='b' name='reuse_existing' direction='in'/>"
    "      <arg type='b' name='persistent' direction='in'/>"
    "      <arg type='s' name='doc_id' direction='out'/>"
    "    </method>"
    "    <method name='AddNamed'>"
    "      <arg type='h' name='o_path_parent_fd' direction='in'/>"
    "      <arg type='ay' name='filename' direction='in'/>"
    "      <arg type='b' name='reuse_existing' direction='in'/>"
    "      <arg type='b' name='persistent' direction='in'/>"
    "      <arg type='s' name='doc_id' direction='out'/>"
    "    </method>"
    "    <method name='AddFull'>"
    "      <arg type='ah' name='o_path_fds' direction='in'/>"
    "      <arg type='u' name='flags' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='as' name='permissions' direction='in'/>"
    "      <arg type='as' name='doc_ids' direction='out'/>"
    "      <arg type='a{sv}' name='extra_out' direction='out'/>"
    "    </method>"
    "    <method name='AddNamedFull'>"
    "      <arg type='h' name='o_path_fd' direction='in'/>"
    "      <arg type='ay' name='filename' direction='in'/>"
    "      <arg type='u' name='flags' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='as' name='permissions' direction='in'/>"
    "      <arg type='s' name='doc_id' direction='out'/>"
    "      <arg type='a{sv}' name='extra_out' direction='out'/>"
    "    </method>"
    "    <method name='GrantPermissions'>"
    "      <arg type='s' name='doc_id' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='as' name='permissions' direction='in'/>"
    "    </method>"
    "    <method name='RevokePermissions'>"
    "      <arg type='s' name='doc_id' direction='in'/>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='as' name='permissions' direction='in'/>"
    "    </method>"
    "    <method name='Delete'>"
    "      <arg type='s' name='doc_id' direction='in'/>"
    "    </method>"
    "    <method name='Lookup'>"
    "      <arg type='ay' name='filename' direction='in'/>"
    "      <arg type='s' name='doc_id' direction='out'/>"
    "    </method>"
    "    <method name='Info'>"
    "      <arg type='s' name='doc_id' direction='in'/>"
    "      <arg type='ay' name='path' direction='out'/>"
    "      <arg type='a{sas}' name='apps' direction='out'/>"
    "    </method>"
    "    <method name='List'>"
    "      <arg type='s' name='app_id' direction='in'/>"
    "      <arg type='a{say}' name='docs' direction='out'/>"
    "    </method>"
    "    <method name='GetHostPaths'>"
    "      <arg type='as' name='doc_ids' direction='in'/>"
    "      <arg type='a{say}' name='paths' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static gchar doc_mount[256] = "/tmp";
static GMainLoop *loop;

static void handle_call(GDBusConnection *conn, const gchar *sender, const gchar *path,
                        const gchar *iface, const gchar *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)params; (void)data;
    if (g_strcmp0(method, "GetMountPoint") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(^ay)", doc_mount));
    } else if (g_strcmp0(method, "Lookup") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", ""));
    } else if (g_strcmp0(method, "List") == 0 || g_strcmp0(method, "GetHostPaths") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{say}"));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(a{say})", &b));
    } else {
        g_dbus_method_invocation_return_dbus_error(inv,
            "org.freedesktop.portal.Error.NotAllowed",
            "Document portal transfers are disabled in LOCachy containers");
    }
}

static GVariant *handle_prop(GDBusConnection *conn, const gchar *sender, const gchar *path,
                             const gchar *iface, const gchar *prop, GError **err, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)err; (void)data;
    if (g_strcmp0(prop, "version") == 0) {
        return g_variant_new_uint32(5);
    }
    return NULL;
}

static void on_bus_acquired(GDBusConnection *conn, const gchar *name, gpointer data)
{
    (void)name; (void)data;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(introspect_xml, NULL);
    static const GDBusInterfaceVTable vtable = { handle_call, handle_prop, NULL, { 0 } };
    if (!info || !g_dbus_connection_register_object(conn, "/org/freedesktop/portal/documents",
                                                    info->interfaces[0], &vtable, NULL, NULL, NULL)) {
        g_main_loop_quit(loop);
    }
    if (info) g_dbus_node_info_unref(info);
}

static void on_name_lost(GDBusConnection *conn, const gchar *name, gpointer data)
{
    (void)conn; (void)name; (void)data;
    g_main_loop_quit(loop);
}

int main(void)
{
    if (geteuid() == 0) {
        return 1;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0] == '/') {
        g_snprintf(doc_mount, sizeof doc_mount, "%s/doc", rt);
        g_mkdir_with_parents(doc_mount, 0700);
    }
    loop = g_main_loop_new(NULL, FALSE);
    guint id = g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.portal.Documents",
                              G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, NULL, on_name_lost, NULL, NULL);
    g_main_loop_run(loop);
    g_bus_unown_name(id);
    g_main_loop_unref(loop);
    return 0;
}
