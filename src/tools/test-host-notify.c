/* The actual notification server over private D-Bus buses, without a display. */
#define main locachy_bar_main
#include "../progs/bar.c"
#undef main
#include "i18n-stub.h"
#include <assert.h>

void say(const char *fmt, ...) { (void)fmt; }
double uptime(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
int text_px(double scale) { (void)scale; return 18; }
int open_rule(const char *type, char *name, size_t size)
{ (void)type; (void)name; (void)size; return OPEN_NO_RULE; }
void open_label(const char *name, const char *type, char *out, size_t size)
{ (void)type; snprintf(out, size, "%s", name); }
const struct panel_font *panel_font(int height, int bold) { (void)height; (void)bold; return NULL; }
void panel_font_size(const struct panel_font *font, int *w, int *h) { (void)font; *w = 9; *h = 18; }
void panel_fill(uint32_t *px, int w, int h, int stride, int x, int y, int bw, int bh, uint32_t color)
{ (void)px;(void)w;(void)h;(void)stride;(void)x;(void)y;(void)bw;(void)bh;(void)color; }
void panel_text(uint32_t *px, int w, int h, int stride, int x, int y, const char *text,
                uint32_t fg, uint32_t bg, const struct panel_font *font)
{ (void)px;(void)w;(void)h;(void)stride;(void)x;(void)y;(void)text;(void)fg;(void)bg;(void)font; }
int panel_text_width(const char *text, const struct panel_font *font) { (void)font; return (int)strlen(text) * 9; }
size_t panel_text_cut(const char *text, int width, const struct panel_font *font)
{ (void)font; size_t n = (size_t)(width > 0 ? width / 9 : 0); return strlen(text) < n ? strlen(text) : n; }
void panel_text_fit(char *out, size_t size, const char *text, int width, const struct panel_font *font)
{ (void)width;(void)font;snprintf(out,size,"%.*s",(int)(size ? size-1 : 0),text); }

static int finished;
static GVariant *reply;
static GError *call_error;
static unsigned int closed_id, closed_reason, action_id;
static char action_name[64];
static void replied(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)data;
    reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &call_error);
    finished = 1;
}
static void pump(void)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
    usleep(1000);
}
static GVariant *call(GDBusConnection *client, const char *method, GVariant *params, const GVariantType *type)
{
    finished = 0;
    reply = NULL;
    call_error = NULL;
    g_dbus_connection_call(client, NOTES_NAME, NOTES_PATH, NOTES_NAME, method, params, type,
                           G_DBUS_CALL_FLAGS_NONE, 2000, NULL, replied, NULL);
    for (int i = 0; !finished && i < 4000; i++) pump();
    if (call_error) fprintf(stderr, "%s: %s\n", method, call_error->message);
    assert(finished && reply && !call_error);
    return reply;
}
static void signalled(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                      const gchar *signal, GVariant *params, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)data;
    if (strcmp(signal, "NotificationClosed") == 0) {
        g_variant_get(params, "(uu)", &closed_id, &closed_reason);
    } else if (strcmp(signal, "ActionInvoked") == 0) {
        const char *name;
        g_variant_get(params, "(u&s)", &action_id, &name);
        snprintf(action_name, sizeof action_name, "%s", name);
    }
}
static GDBusConnection *connect_bus(const char *address)
{
    GError *error = NULL;
    GDBusConnection *conn = g_dbus_connection_new_for_address_sync(
        address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    if (error) fprintf(stderr, "%s\n", error->message);
    assert(conn && !error);
    return conn;
}
static unsigned int notify(GDBusConnection *client, unsigned int replaces, int timeout, int action, const char *summary)
{
    const gchar *actions[] = { "default", "Open", NULL }, *none[] = { NULL };
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    GVariant *result = call(client, "Notify", g_variant_new("(susss^asa{sv}i)", "Cloud drives", replaces, "",
        summary, "Cloud: authentication rejected. Open Settings > Cloud drives.", action ? actions : none,
        &hints, timeout), G_VARIANT_TYPE("(u)"));
    unsigned int id;
    g_variant_get(result, "(u)", &id);
    g_variant_unref(result);
    return id;
}
int main(void)
{
    assert(getenv("DBUS_SESSION_BUS_ADDRESS"));
    active = 0; /* queue and standard protocol are real; overlays have no display */
    host_notes_tick();
    for (int i = 0; !host_notes.notes_own && i < 4000; i++) pump();
    assert(host_notes.conn && host_notes.notes_object && host_notes.notes_own && !trays);
    GDBusConnection *client = connect_bus(getenv("DBUS_SESSION_BUS_ADDRESS"));
    g_dbus_connection_signal_subscribe(client, NULL, NOTES_NAME, NULL, NOTES_PATH, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, signalled, NULL, NULL);
    for (int i = 0; i < 50; i++) pump();
    GVariant *result = call(client, "GetCapabilities", NULL, G_VARIANT_TYPE("(as)"));
    gchar **caps = NULL;
    g_variant_get(result, "(^as)", &caps);
    assert(g_strv_length(caps) == 2 && strcmp(caps[0], "body") == 0 && strcmp(caps[1], "actions") == 0);
    g_strfreev(caps);
    g_variant_unref(result);
    unsigned int id = notify(client, 0, 0, 1, "Authentication required");
    assert(id && nnotes == 1 && !notes[0].container[0] && notes[0].has_default);
    assert(strcmp(notes[0].summary, "Authentication required") == 0);
    puts("PASS host notification uses the standard session-bus interface with no container identity");

    assert(notify(client, id, 0, 1, "Still requires authentication") == id && nnotes == 1);
    assert(strcmp(notes[0].summary, "Still requires authentication") == 0);
    puts("PASS standard replaces_id updates the host notification instead of adding a popup");

    notes_ovl.ch = 18;
    notes_click(1, BTN_LEFT);
    for (int i = 0; (!closed_id || !action_id) && i < 2000; i++) pump();
    assert(nnotes == 0 && closed_id == id && closed_reason == 2 && action_id == id && strcmp(action_name, "default") == 0);
    assert(!again.pending && !again.watch[0]);
    puts("PASS host click sends ActionInvoked and dismissed without opening an application tray");

    closed_id = closed_reason = action_id = 0;
    id = notify(client, 0, 0, 0, "Recovered");
    result = call(client, "CloseNotification", g_variant_new("(u)", id), NULL);
    g_variant_unref(result);
    for (int i = 0; !closed_id && i < 2000; i++) pump();
    assert(!nnotes && closed_id == id && closed_reason == 3 && !action_id);
    puts("PASS recovery can close the standard host notification");

    closed_id = closed_reason = 0;
    id = notify(client, 0, 1, 0, "Short notification");
    notes[0].shown_at = uptime() - 1;
    notes_tick();
    assert(nnotes == 1); /* elapsed time is paused while home is visible */
    notes[0].shown_at = uptime() - 1;
    active = 1;
    notes_tick();
    active = 0;
    for (int i = 0; !closed_id && i < 2000; i++) pump();
    assert(nnotes == 0 && closed_id == id && closed_reason == 1);
    puts("PASS host notification expiry retains the shared display pause semantics");

    id = notify(client, 0, 0, 0, "Before reconnect");
    assert(id && nnotes == 1);
    g_dbus_connection_close_sync(host_notes.conn, NULL, NULL);
    for (int i = 0; host_notes.conn && i < 2000; i++) pump();
    assert(!host_notes.conn && !nnotes && !host_notes.notes_own && !host_notes.notes_object);
    host_notes.retry_at = 0;
    host_notes_tick();
    for (int i = 0; !host_notes.notes_own && i < 4000; i++) pump();
    assert(host_notes.conn && host_notes.notes_own);
    for (int i = 0; i < 50; i++) pump();
    id = notify(client, 0, 0, 0, "After reconnect");
    assert(id && nnotes == 1);
    puts("PASS a closed host bus connection removes stale notes and reconnects");

    notes_drop(&host_notes);
    g_object_unref(host_notes.conn);
    host_notes.conn = NULL;
    g_object_unref(client);

    GTestDBus *private_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(private_bus);
    struct tray app = { .name = "demo" };
    trays = &app;
    app.conn = connect_bus(g_test_dbus_get_bus_address(private_bus));
    notes_serve(&app);
    client = connect_bus(g_test_dbus_get_bus_address(private_bus));
    for (int i = 0; i < 50; i++) pump();
    id = notify(client, 0, 0, 1, "Container notification");
    assert(id && nnotes == 1 && strcmp(notes[0].container, "demo") == 0 && app.notified);
    assert(notify(client, id, 0, 1, "Container replacement") == id && nnotes == 1);
    result = call(client, "CloseNotification", g_variant_new("(u)", id), NULL);
    g_variant_unref(result);
    assert(nnotes == 0);
    puts("PASS container-private notification ownership, replacement and close remain unchanged");
    notes_drop(&app);
    g_object_unref(app.conn);
    g_object_unref(client);
    trays = NULL;
    for (int i = 0; i < 50; i++) pump();
    g_test_dbus_down(private_bus);
    g_object_unref(private_bus);
    return 0;
}
