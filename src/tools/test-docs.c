/*
 * test-docs: the private-file drag's pieces without a device (test-docs.sh,
 * test-docs-portal.sh). Built with the same DOCS_DIR (and CGROUP_ROOT) as
 * the locachy-docs it drives.
 *   unit                   docs-proto.h: the key, app ids, names, peers
 *   portal LOG MOUNT       a stand-in org.freedesktop.portal.Documents on the
 *                          session bus: what it is asked goes to LOG
 *   protocol DIR SERVER    SERVER --user (locachy-docs) against the stand-in,
 *                          on the session bus, its home DIR/home: begin, end,
 *                          drop, view, peers, all or nothing, a connection
 *                          that closes, a container that ends
 *   key PATH...            the key of these paths
 *   drag NAME FILE...      as files and labwc do it, on DOCS_CONTROL: begin
 *                          FILE..., print the paths; wait for a line on stdin;
 *                          drop on container NAME; wait for a line; end done
 *                          (end cancelled when that line is "cancelled"),
 *                          which has no answer: "end sent"
 *   attach-fd NAME TARGET  ask the server for container NAME's view and
 *                          clone it onto TARGET (open_tree, move_mount)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../progs/docs-proto.h"

#ifndef CGROUP_ROOT
#define CGROUP_ROOT "/sys/fs/cgroup"
#endif

static int failures;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    failures += !ok;
}

/* --- unit --- */

static int roundtrip(const char *data, size_t n)
{
    char key[4096], back[4096];
    long k = docs_key_encode(data, n, key, sizeof key);
    long m = k > 0 ? docs_key_decode(key, back, sizeof back) : -1;
    return k == (long)strlen(key) + 1 && !strchr(key, ':') && !strchr(key, '=') && m == (long)n &&
           memcmp(data, back, n) == 0;
}

static int unit(void)
{
    const char paths[] = "/run/user/1000/doc/a1b2/report.pdf\0/home/u/Documents/x y.txt";
    int all = 1;
    for (size_t n = 0; n <= sizeof paths - 1; n++) all &= roundtrip(paths, n);
    unsigned char bytes[256];
    for (int i = 0; i < 256; i++) bytes[i] = (unsigned char)i;
    all &= roundtrip((char *)bytes, sizeof bytes);
    check(all, "key: every length and every byte encodes and decodes back, no ':' and no padding");
    char key[256], back[256];
    docs_key_encode("/a", 2, key, sizeof key);
    check(strcmp(key, "LOCACHYDOCL2E") == 0 && strspn(key, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
          strlen(key), "key: the prefix and base64url without padding (\"/a\" is LOCACHYDOCL2E)");
    check(docs_key_decode("LOCACHYDOCL2E=", back, sizeof back) < 0 && docs_key_decode("LOCACHYDOCL2+", back, sizeof back) < 0 &&
          docs_key_decode("LOCACHYDOCL2F", back, sizeof back) < 0 && docs_key_decode("LOCACHYDOCL", back, sizeof back) < 0 &&
          docs_key_decode("OTHERL2E", back, sizeof back) < 0 && docs_key_decode("file:///x", back, sizeof back) < 0 &&
          docs_key_decode("LOCACHYDOCL2E", back, 1) < 0,
          "key: padding, a foreign character, stray bits, an impossible length, another prefix, a URI, no room: refused");
    check(docs_key_encode("abc", 3, key, 14) < 0 && docs_key_encode("abc", 3, key, 15) == 15,
          "key: encoding refuses a buffer one byte short");

    char a[256], b[256];
    check(docs_app_id("edge", a, sizeof a) == 0 && strcmp(a, "org.locachy.app.edge") == 0,
          "app id: a plain name");
    check(docs_app_id("my.app_1", a, sizeof a) == 0 && strcmp(a, "org.locachy.app.my_dapp__1") == 0,
          "app id: '.' and '_' escaped");
    check(docs_app_id("7zip", a, sizeof a) == 0 && strcmp(a, "org.locachy.app._7zip") == 0,
          "app id: a leading digit gets '_' (an element cannot begin with one)");
    check(docs_app_id("a_d", a, sizeof a) == 0 && strcmp(a, "org.locachy.app.a__d") == 0 &&
          docs_app_id("a.d", b, sizeof b) == 0 && strcmp(a, b) != 0 && strcmp(b, "org.locachy.app.a_dd") == 0 &&
          docs_app_id("a-b", a, sizeof a) == 0 && strcmp(a, "org.locachy.app.a-b") == 0,
          "app id: no two names share one (a_d, a.d); '-' kept (allowed in the last element)");
    check(docs_app_id("", a, sizeof a) < 0 && docs_app_id(".x", a, sizeof a) < 0 && docs_app_id("a/b", a, sizeof a) < 0 &&
          docs_app_id("a b", a, sizeof a) < 0 && docs_app_id("edge", a, 20) < 0 &&
          docs_app_id("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", a, sizeof a) < 0,
          "app id: no id for what is no container name (empty, '.', '/', space, 65 characters) or no room");
    check(docs_app_id("window-files-1", a, sizeof a) < 0 && !docs_container_name("window-view-2") &&
          docs_window_name("window-files-1") && docs_container_name("windows") && docs_container_name("edge"),
          "a native window's name (window-PROGRAM-N) is no container's: no app id, no view");
    check(docs_peer_kind("0::/app_edge\n") == DOCS_PEER_CONTAINER && docs_peer_kind("0::/app_edge/sub\n") == DOCS_PEER_CONTAINER &&
          docs_peer_kind("0::/app_window-files-1\n") == DOCS_PEER_WINDOW && docs_peer_kind("0::/p3\n") == DOCS_PEER_HOST &&
          docs_peer_kind("0::/\n") == DOCS_PEER_HOST && docs_peer_kind("0::/locachy-docs\n") == DOCS_PEER_HOST &&
          docs_peer_kind("1:name=systemd:/\n") == DOCS_PEER_UNKNOWN && docs_peer_kind("") == DOCS_PEER_UNKNOWN,
          "peers: a container's cgroup, a native window's, the host's, and no cgroup v2 line told apart");
    return failures;
}

/* --- portal: a stand-in document portal --- */

static FILE *portal_log;
static char portal_mount[PATH_MAX];
static unsigned portal_docs;

static const gchar portal_xml[] =
    "<node><interface name='org.freedesktop.portal.Documents'>"
    "<method name='GetMountPoint'><arg type='ay' direction='out'/></method>"
    "<method name='AddFull'><arg type='ah' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='as' direction='in'/><arg type='as' direction='out'/>"
    "<arg type='a{sv}' direction='out'/></method>"
    "<method name='GrantPermissions'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='as' direction='in'/></method>"
    "<method name='RevokePermissions'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='as' direction='in'/></method>"
    "<method name='Delete'><arg type='s' direction='in'/></method>"
    "</interface></node>";

static void portal_call(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                        const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)data;
    if (strcmp(method, "GetMountPoint") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(^ay)", portal_mount));
    } else if (strcmp(method, "AddFull") == 0) {
        GVariant *handles = NULL;
        guint32 flags = 0;
        const gchar *app = NULL;
        const gchar **perms = NULL;
        g_variant_get(params, "(@ahu&s^a&s)", &handles, &flags, &app, &perms);
        GUnixFDList *list = g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(inv));
        gsize n = g_variant_n_children(handles);
        GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
        int refused = 0;
        GString *lines = g_string_new(NULL);
        for (gsize i = 0; i < n; i++) {
            gint32 h;
            g_variant_get_child(handles, i, "h", &h);
            int fd = list ? g_unix_fd_list_get(list, h, NULL) : -1;
            char at[64], target[PATH_MAX] = "?";
            snprintf(at, sizeof at, "/proc/self/fd/%d", fd);
            ssize_t r = readlink(at, target, sizeof target - 1);
            if (r > 0) target[r] = '\0';
            if (fd >= 0) close(fd);
            const char *base = strrchr(target, '/');
            refused |= base && strncmp(base + 1, "refuse", 6) == 0;
            char *id = g_strdup_printf("d%u", ++portal_docs);
            g_string_append_printf(lines, "add %s flags=%u app=%s perms=%u %s\n", id, flags, app,
                                   perms ? g_strv_length((gchar **)perms) : 0, target);
            g_ptr_array_add(ids, id);
        }
        if (refused) {
            portal_docs -= (unsigned)n;
            fprintf(portal_log, "refused %zu\n", (size_t)n);
            g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.portal.Error.NotAllowed", "refused");
        } else {
            fputs(lines->str, portal_log);
            g_ptr_array_add(ids, NULL);
            GVariantBuilder extra;
            g_variant_builder_init(&extra, G_VARIANT_TYPE_VARDICT);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(^as@a{sv})", (gchar **)ids->pdata,
                                                                     g_variant_builder_end(&extra)));
        }
        g_string_free(lines, TRUE);
        g_ptr_array_unref(ids);
        g_variant_unref(handles);
        g_free(perms);
    } else if (strcmp(method, "GrantPermissions") == 0 || strcmp(method, "RevokePermissions") == 0) {
        const gchar *id, *app, **perms = NULL;
        g_variant_get(params, "(&s&s^a&s)", &id, &app, &perms);
        gchar *joined = g_strjoinv(",", (gchar **)perms);
        fprintf(portal_log, "%s %s %s %s\n", method[0] == 'G' ? "grant" : "revoke", id, app, joined);
        g_free(joined);
        g_free(perms);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (strcmp(method, "Delete") == 0) {
        const gchar *id;
        g_variant_get(params, "(&s)", &id);
        fprintf(portal_log, "delete %s\n", id);
        g_dbus_method_invocation_return_value(inv, NULL);
    }
    fflush(portal_log);
}

static void portal_acquired(GDBusConnection *conn, const gchar *name, gpointer data)
{
    (void)name; (void)data;
    static const GDBusInterfaceVTable vtable = { portal_call, NULL, NULL, { 0 } };
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(portal_xml, NULL);
    g_dbus_connection_register_object(conn, "/org/freedesktop/portal/documents", info->interfaces[0], &vtable,
                                      NULL, NULL, NULL);
    g_dbus_node_info_unref(info);
}

static void portal_owned(GDBusConnection *conn, const gchar *name, gpointer data)
{
    (void)conn; (void)name; (void)data;
    printf("ready\n");
    fflush(stdout);
}

static int portal(const char *log, const char *mount)
{
    portal_log = fopen(log, "a");
    snprintf(portal_mount, sizeof portal_mount, "%s", mount);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.portal.Documents", G_BUS_NAME_OWNER_FLAGS_NONE,
                   portal_acquired, portal_owned, NULL, NULL, NULL);
    g_main_loop_run(loop);
    return 0;
}

/* --- the control socket, as files, labwc and locachy-app use it --- */

static int dial(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX, .sun_path = DOCS_CONTROL };
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct timeval limit = { .tv_sec = 5 };
    if (s < 0 || setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit) < 0 ||
        connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        if (s >= 0) close(s);
        return -1;
    }
    return s;
}

/* text with fds; the answer into reply ("" when the connection closed), an
 * fd that came with it into *got. */
static void ask(int s, const char *text, const int *fds, int nfds, char *reply, size_t size, int *got)
{
    char control[CMSG_SPACE(sizeof(int) * (DOCS_FDS_MAX + 4))] = { 0 };
    struct iovec iov = { .iov_base = (void *)text, .iov_len = strlen(text) };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (nfds > 0) {
        m.msg_control = control;
        m.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *h = CMSG_FIRSTHDR(&m);
        h->cmsg_level = SOL_SOCKET;
        h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(h), fds, sizeof(int) * (size_t)nfds);
    }
    reply[0] = '\0';
    if (got) *got = -1;
    if (sendmsg(s, &m, MSG_NOSIGNAL) < 0) return;
    char back[CMSG_SPACE(sizeof(int))];
    struct iovec in = { .iov_base = reply, .iov_len = size - 1 };
    struct msghdr r = { .msg_iov = &in, .msg_iovlen = 1, .msg_control = back, .msg_controllen = sizeof back };
    ssize_t n = recvmsg(s, &r, MSG_CMSG_CLOEXEC);
    reply[n > 0 ? n : 0] = '\0';
    struct cmsghdr *h = n > 0 ? CMSG_FIRSTHDR(&r) : NULL;
    if (h && h->cmsg_type == SCM_RIGHTS && got) memcpy(got, CMSG_DATA(h), sizeof(int));
}

/* A request that has no answer (end): sent, nothing read. */
static void tell(int s, const char *text)
{
    if (send(s, text, strlen(text), MSG_NOSIGNAL) < 0) perror("send");
}

/* The connection still answers, and nothing was queued before this answer:
 * the request before it had none. */
static int answers_next(int s)
{
    char reply[256];
    ask(s, "hello", NULL, 0, reply, sizeof reply, NULL);
    return strcmp(reply, "error unknown request") == 0;
}

/* begin (or begin-more SESSION) of these files: their O_PATH fds, names. */
static void begin(int s, const char *session, char *const *files, int n, char *reply, size_t size)
{
    char text[DOCS_MSG_MAX];
    int fds[DOCS_FDS_MAX + 4];
    size_t len = session ? (size_t)snprintf(text, sizeof text, "begin-more %s", session) :
                           (size_t)snprintf(text, sizeof text, "begin");
    for (int i = 0; i < n; i++) {
        fds[i] = open(files[i], O_PATH | O_CLOEXEC);
        const char *base = strrchr(files[i], '/');
        len += (size_t)snprintf(text + len, sizeof text - len, "\n%s", base ? base + 1 : files[i]);
    }
    ask(s, text, fds, n, reply, size, NULL);
    for (int i = 0; i < n; i++) close(fds[i]);
}

/* As labwc: comm "labwc" (while the server looks), its own connection. */
static void drop(const char *what, char *reply, size_t size)
{
    char comm[16] = "";
    prctl(PR_GET_NAME, comm);
    prctl(PR_SET_NAME, "labwc");
    int s = dial();
    char text[256];
    snprintf(text, sizeof text, "drop %d %s", (int)getpid(), what);
    ask(s, text, NULL, 0, reply, size, NULL);
    prctl(PR_SET_NAME, comm);
    close(s);
}

/* --- protocol --- */

static char dir[256], log_path[PATH_MAX];

static int log_has(const char *text, int wait_ms)
{
    for (int i = 0;; i++) {
        gchar *all = NULL;
        int has = g_file_get_contents(log_path, &all, NULL, NULL) && strstr(all, text);
        g_free(all);
        if (has || i * 20 >= wait_ms) return has;
        usleep(20000);
    }
}

static int log_count(const char *prefix)
{
    gchar *all = NULL;
    int n = 0;
    if (g_file_get_contents(log_path, &all, NULL, NULL)) {
        for (char *p = all; (p = strstr(p, prefix)); p++) n += p == all || p[-1] == '\n';
    }
    g_free(all);
    return n;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

/* A stand-in cgroup.events changed as the kernel changes it: in one write,
 * never empty in between. */
static void set_events(const char *path, int populated, int frozen)
{
    char text[64];
    int n = snprintf(text, sizeof text, "populated %d\nfrozen %d\n", populated, frozen);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    assert(fd >= 0 && pwrite(fd, text, (size_t)n, 0) == n);
    close(fd);
}

static char *at(const char *rel)
{
    static char paths[8][PATH_MAX];
    static int i;
    i = (i + 1) % 8;
    int n = snprintf(paths[i], PATH_MAX, "%s/%s", dir, rel);
    assert(n > 0 && n < PATH_MAX);
    return paths[i];
}

static pid_t start(char *const argv[], int fd3, int fd4, int out)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (fd3 >= 0) dup2(fd3, 3);
        if (fd4 >= 0) dup2(fd4, 4);
        if (out >= 0) dup2(out, 1);
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static int protocol(const char *top, const char *server)
{
    assert(strlen(top) < sizeof dir);
    snprintf(dir, sizeof dir, "%s", top);
    snprintf(log_path, sizeof log_path, "%s/portal.log", dir);
    /* The stand-ins: the portal's mount and a container's view of it, the
     * cgroups of two containers, a home with files. */
    const char *mk[] = { "doc", "doc/by-app", "doc/by-app/org.locachy.app.edge", "doc/by-app/org.locachy.app.my_dapp__1",
                         "doc/by-app/org.locachy.app.off", "docs", "docs/views", "cgroup", "cgroup/app_edge",
                         "cgroup/app_my.app_1", "cgroup/app_off", "home", "home/folder", "elsewhere", NULL };
    for (int i = 0; mk[i]; i++) assert(mkdir(at(mk[i]), 0755) == 0);
    assert(symlink(at("doc/by-app/org.locachy.app.edge"), at("docs/views/edge")) == 0);
    assert(symlink(at("doc/by-app/org.locachy.app.my_dapp__1"), at("docs/views/my.app_1")) == 0);
    assert(symlink(at("doc/by-app/org.locachy.app.off"), at("docs/views/off")) == 0);
    put(at("cgroup/app_edge/cgroup.events"), "populated 1\nfrozen 0\n");
    put(at("cgroup/app_my.app_1/cgroup.events"), "populated 1\nfrozen 0\n");
    put(at("cgroup/app_off/cgroup.events"), "populated 0\nfrozen 0\n");
    char *files[40];
    for (int i = 0; i < 40; i++) {
        files[i] = g_strdup_printf("%s/home/f%02d.txt", dir, i);
        put(files[i], "private\n");
    }
    put(at("home/refuse-me.txt"), "x\n");
    put(at("elsewhere/out.txt"), "x\n");
    assert(mkfifo(at("home/fifo"), 0600) == 0);

    int p[2];
    assert(pipe(p) == 0);
    char *pargv[] = { "/proc/self/exe", "portal", log_path, at("doc"), NULL };
    char self[PATH_MAX];
    ssize_t r = readlink("/proc/self/exe", self, sizeof self - 1);
    assert(r > 0);
    self[r] = '\0';
    pargv[0] = self;
    pid_t portal_pid = start(pargv, -1, -1, p[1]);
    close(p[1]);
    char line[64] = "";
    assert(read(p[0], line, sizeof line - 1) > 0 && strncmp(line, "ready", 5) == 0);

    struct sockaddr_un a = { .sun_family = AF_UNIX, .sun_path = DOCS_CONTROL };
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0), ready[2];
    assert(listener >= 0 && bind(listener, (struct sockaddr *)&a, sizeof a) == 0 && listen(listener, 8) == 0);
    /* Anyone may connect here (the service's is root's alone): the server's
     * own check of its peers is what is tested. */
    assert(chmod(DOCS_CONTROL, 0777) == 0);
    assert(pipe(ready) == 0);
    setenv("HOME", at("home"), 1);
    char *sargv[] = { (char *)server, "--user", NULL };
    pid_t server_pid = start(sargv, listener, ready[1], -1);
    close(listener);
    close(ready[1]);
    line[0] = '\0';
    check(read(ready[0], line, sizeof line - 1) > 0 && strncmp(line, "ready", 5) == 0,
          "the server is ready once the portal answered GetMountPoint");

    char reply[DOCS_MSG_MAX], expect[DOCS_MSG_MAX];
    int s, fd;

    /* Peers: root only, and only the compositor tells drops. */
    pid_t child = fork();
    if (child == 0) {
        if (setgroups(0, NULL) < 0 || setgid(1000) < 0 || setuid(1000) < 0) _exit(3);
        int c = dial();
        if (c < 0) _exit(4);
        ask(c, "view edge", NULL, 0, reply, sizeof reply, NULL);
        _exit(reply[0] ? 1 : 0);
    }
    int st;
    waitpid(child, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "peers: a process not root is shut out (connection closed unanswered)");

    s = dial();
    begin(s, NULL, (char *[]){ files[0], at("home/folder") }, 2, reply, sizeof reply);
    snprintf(expect, sizeof expect, "ok 1\n%s/d1/f00.txt\n%s/d2/folder", at("doc"), at("doc"));
    check(strcmp(reply, expect) == 0, "begin: a file and a folder: ok SESSION and their documents' paths, in order");
    snprintf(expect, sizeof expect, "add d1 flags=0 app= perms=0 %s\nadd d2 flags=8 app= perms=0 %s/home/folder\n",
             files[0], dir);
    check(log_has(expect, 0), "begin: AddFull without an app or permission, not reused or persistent; folders with DIRECTORY");
    ask(s, "drop 1 container edge", NULL, 0, reply, sizeof reply, NULL);
    check(strncmp(reply, "refuse only the compositor", 26) == 0 && log_count("grant ") == 0,
          "drop: refused from anything but the compositor (comm labwc)");
    drop("xwayland", reply, sizeof reply);
    check(strncmp(reply, "refuse ", 7) == 0 && log_has("delete d1\ndelete d2\n", 0),
          "drop on Xwayland: refused, and the drag's documents are deleted at once");
    tell(s, "end 1 done");
    check(answers_next(s), "end of a drag already refused: no answer (no such drag), the connection still served");
    close(s);

    s = dial();
    begin(s, NULL, (char *[]){ files[1] }, 1, reply, sizeof reply);
    tell(s, "end 2 cancelled");
    check(answers_next(s) && log_has("delete d3\n", 2000), "end cancelled: no answer, the documents are deleted");
    begin(s, NULL, (char *[]){ files[2] }, 1, reply, sizeof reply);
    close(s);
    check(log_has("delete d4\n", 2000), "a connection that closes before its end: its drag's documents are deleted");

    s = dial();
    begin(s, NULL, (char *[]){ files[3] }, 1, reply, sizeof reply);
    drop("host", reply, sizeof reply);
    check(strcmp(reply, "ok") == 0 && log_count("grant ") == 0, "drop on a host client: ok, nothing granted");
    tell(s, "end 4 done");
    close(s);
    usleep(200000);
    check(strcmp(reply, "ok") == 0 && !log_has("delete d5\n", 0),
          "a finished drop on the host (end done, then the connection closed) keeps its documents");

    /* All or nothing over several messages. */
    s = dial();
    begin(s, NULL, files + 4, DOCS_FDS_MAX, reply, sizeof reply);
    int lines = 0;
    for (char *q = reply; (q = strchr(q, '\n')); q++) lines++;
    check(strncmp(reply, "ok 5\n", 5) == 0 && lines == DOCS_FDS_MAX, "begin: 16 files in one message");
    begin(s, "5", (char *[]){ files[20], at("home/refuse-me.txt") }, 2, reply, sizeof reply);
    int deleted = 1;
    for (int i = 6; i <= 21; i++) {
        char d[32];
        snprintf(d, sizeof d, "delete d%d\n", i);
        deleted &= log_has(d, 0);
    }
    check(strncmp(reply, "error ", 6) == 0 && deleted && log_has("refused 2\n", 0),
          "begin-more the portal refuses: error, and every document of the drag so far is deleted");
    begin(s, "5", (char *[]){ files[21] }, 1, reply, sizeof reply);
    check(strcmp(reply, "error no such drag") == 0, "the refused drag is gone: begin-more on it is refused");
    begin(s, NULL, (char *[]){ files[22] }, 1, reply, sizeof reply);
    char *more[DOCS_FDS_MAX + 1];
    for (int i = 0; i <= DOCS_FDS_MAX; i++) more[i] = files[23];
    char text[64];
    snprintf(text, sizeof text, "%.*s", (int)strcspn(reply + 3, "\n"), reply + 3);
    begin(s, text, more, DOCS_FDS_MAX + 1, reply, sizeof reply);
    check(strncmp(reply, "error", 5) == 0, "more than 16 descriptors in a message: refused");
    close(s);

    /* What a descriptor must be. */
    s = dial();
    struct { const char *file, *name, *what; int flags; } bad[] = {
        { files[24], "other.txt", "a name that is not the file's (renamed): refused", O_PATH },
        { at("elsewhere/out.txt"), "out.txt", "a file outside the home: refused", O_PATH },
        { files[25], "f25.txt", "an O_PATH descriptor with O_NOFOLLOW: refused", O_PATH | O_NOFOLLOW },
        { files[26], "f26.txt", "a descriptor not O_PATH: refused", O_RDONLY },
        { at("home/fifo"), "fifo", "neither a file nor a folder: refused", O_PATH },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int f = open(bad[i].file, bad[i].flags | O_CLOEXEC);
        snprintf(text, sizeof text, "begin\n%s", bad[i].name);
        ask(s, text, &f, 1, reply, sizeof reply, NULL);
        close(f);
        check(strncmp(reply, "error ", 6) == 0, bad[i].what);
    }
    ask(s, "begin\nx", NULL, 0, reply, sizeof reply, NULL);
    check(strncmp(reply, "error", 5) == 0, "a name without its descriptor: refused");
    close(s);

    /* A drop on a container: its view and its cgroup decide. */
    s = dial();
    begin(s, NULL, (char *[]){ files[36] }, 1, reply, sizeof reply);
    int deletes = log_count("delete ");
    drop("container window-files-1", reply, sizeof reply);
    check(strstr(reply, "refuse window-files-1 is a native window") == reply && log_count("grant ") == 0 &&
          log_count("delete ") == deletes + 1,
          "a native window named as a container: refused (labwc asks host for one), the documents deleted");
    begin(s, NULL, (char *[]){ files[27], files[28] }, 2, reply, sizeof reply);
    snprintf(text, sizeof text, "%.*s", (int)strcspn(reply + 3, "\n"), reply + 3);
    int before = log_count("delete ");
    drop("container nowhere", reply, sizeof reply);
    check(strncmp(reply, "refuse ", 7) == 0 && log_count("grant ") == 0 && log_count("delete ") == before + 2,
          "drop on a container without a document view: refused, nothing granted, the documents deleted");
    begin(s, NULL, (char *[]){ files[29] }, 1, reply, sizeof reply);
    drop("container off", reply, sizeof reply);
    check(strstr(reply, "refuse off is not running") == reply && log_count("grant ") == 0,
          "drop on a container with its view that is not running: refused");
    begin(s, NULL, (char *[]){ files[30], files[31] }, 2, reply, sizeof reply);
    snprintf(text, sizeof text, "%.*s", (int)strcspn(reply + 3, "\n"), reply + 3);
    drop("container edge", reply, sizeof reply);
    int grants = log_count("grant ");
    check(strcmp(reply, "ok") == 0 && grants == 2 && log_has(" org.locachy.app.edge read\n", 0),
          "drop on a running container with its view: GrantPermissions read for its app id, each document, then ok");
    drop("container edge", reply, sizeof reply);
    check(strncmp(reply, "refuse ", 7) == 0 && log_count("grant ") == 2, "one drop per drag: a second is refused");
    char end_text[128];
    snprintf(end_text, sizeof end_text, "end %s done", text);
    tell(s, end_text);
    check(answers_next(s), "end done after the drop: no answer");
    close(s);
    usleep(200000);
    before = log_count("delete ");
    check(before == log_count("delete "), "the granted documents stay while the container runs");
    set_events(at("cgroup/app_edge/cgroup.events"), 1, 1);
    usleep(200000);
    check(log_count("delete ") == before, "a container frozen keeps its documents");
    set_events(at("cgroup/app_edge/cgroup.events"), 0, 0);
    int revoked = 0;
    for (int i = 0; i < 100 && !revoked; i++, usleep(20000)) revoked = log_count("delete ") == before + 2;
    check(revoked, "the container's cgroup empty: its documents deleted");

    s = dial();
    begin(s, NULL, (char *[]){ files[32] }, 1, reply, sizeof reply);
    drop("container my.app_1", reply, sizeof reply);
    check(strcmp(reply, "ok") == 0 && log_has(" org.locachy.app.my_dapp__1 read\n", 0),
          "a name with '.' and '_': its escaped app id is granted");
    close(s);
    usleep(200000);

    /* Granted, then the drag ends otherwise (labwc gave up waiting, the
     * pointer left the target, the files connection went): revoked and
     * deleted, every document. */
    int revokes = log_count("revoke ");
    before = log_count("delete ");
    s = dial();
    begin(s, NULL, (char *[]){ files[33], files[34] }, 2, reply, sizeof reply);
    snprintf(text, sizeof text, "%.*s", (int)strcspn(reply + 3, "\n"), reply + 3);
    drop("container my.app_1", reply, sizeof reply);
    check(strcmp(reply, "ok") == 0, "a second drag granted to the container");
    snprintf(end_text, sizeof end_text, "end %s cancelled", text);
    tell(s, end_text);
    check(answers_next(s) && log_count("revoke ") == revokes + 2 && log_count("delete ") == before + 2 &&
          log_has(" org.locachy.app.my_dapp__1 read\ndelete ", 0),
          "granted, then end cancelled: each document's grant revoked, then the document deleted");
    begin(s, NULL, (char *[]){ files[35] }, 1, reply, sizeof reply);
    drop("container my.app_1", reply, sizeof reply);
    close(s);
    int gone = 0;
    for (int i = 0; i < 100 && !gone; i++, usleep(20000)) {
        gone = log_count("revoke ") == revokes + 3 && log_count("delete ") == before + 3;
    }
    check(strcmp(reply, "ok") == 0 && gone, "granted, then the files connection closes before its end: revoked and deleted");

    /* view NAME: an O_PATH descriptor of the container's by-app view. */
    s = dial();
    ask(s, "view edge", NULL, 0, reply, sizeof reply, &fd);
    struct stat x, y;
    check(strcmp(reply, "ok") == 0 && fd >= 0 && fstat(fd, &x) == 0 && stat(at("doc/by-app/org.locachy.app.edge"), &y) == 0 &&
          x.st_ino == y.st_ino && (fcntl(fd, F_GETFL) & O_PATH),
          "view NAME: ok with an O_PATH descriptor of the portal's by-app/APPID");
    if (fd >= 0) close(fd);
    ask(s, "view ../x", NULL, 0, reply, sizeof reply, &fd);
    check(strncmp(reply, "error", 5) == 0 && fd < 0, "view of what is no container name: refused");
    ask(s, "hello", NULL, 0, reply, sizeof reply, NULL);
    check(strcmp(reply, "error unknown request") == 0, "an unknown request: error");
    close(s);

    kill(server_pid, SIGTERM);
    kill(portal_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    waitpid(portal_pid, NULL, 0);
    for (int i = 0; i < 40; i++) g_free(files[i]);
    if (!failures) puts("ALL PASSED");
    return failures != 0;
}

/* --- for the real portal (test-docs-portal.sh) --- */

static int key(int argc, char **argv)
{
    GString *joined = g_string_new(NULL);
    for (int i = 0; i < argc; i++) g_string_append_len(joined, argv[i], (gssize)strlen(argv[i]) + (i + 1 < argc));
    char out[8192];
    if (docs_key_encode(joined->str, joined->len, out, sizeof out) < 0) return 1;
    puts(out);
    return 0;
}

static int drag(const char *name, int argc, char **argv)
{
    char reply[DOCS_MSG_MAX], line[64], text[64];
    int s = dial();
    if (s < 0) {
        perror("dial");
        return 1;
    }
    begin(s, NULL, argv, argc, reply, sizeof reply);
    printf("%s\n", reply);
    fflush(stdout);
    if (strncmp(reply, "ok ", 3) != 0 || !fgets(line, sizeof line, stdin)) return 1;
    snprintf(text, sizeof text, "container %s", name);
    char answer[512];
    drop(text, answer, sizeof answer);
    printf("drop %s\n", answer);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) return 1;
    snprintf(text, sizeof text, "end %.*s %s", (int)strcspn(reply + 3, "\n"), reply + 3,
             strncmp(line, "cancelled", 9) == 0 ? "cancelled" : "done");
    /* As files: the end, unanswered, then the connection closed. That it
     * was taken: the connection still answers the next request. */
    tell(s, text);
    printf("end %s\n", answers_next(s) ? "sent" : "lost");
    fflush(stdout);
    close(s);
    return 0;
}

static int attach_fd(const char *name, const char *target)
{
    char text[128], reply[256];
    int s = dial(), fd = -1;
    snprintf(text, sizeof text, "view %s", name);
    if (s >= 0) ask(s, text, NULL, 0, reply, sizeof reply, &fd);
    if (fd < 0) {
        fprintf(stderr, "no view: %s\n", s < 0 ? "no server" : reply);
        return 1;
    }
    int tree = open_tree(fd, "", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH);
    if (tree < 0 || move_mount(tree, "", AT_FDCWD, target, MOVE_MOUNT_F_EMPTY_PATH) < 0) {
        perror("open_tree/move_mount");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && strcmp(argv[1], "unit") == 0) {
        unit();
        if (!failures) puts("ALL PASSED");
        return failures != 0;
    }
    if (argc == 4 && strcmp(argv[1], "portal") == 0) return portal(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "protocol") == 0) return protocol(argv[2], argv[3]);
    if (argc >= 3 && strcmp(argv[1], "key") == 0) return key(argc - 2, argv + 2);
    if (argc >= 4 && strcmp(argv[1], "drag") == 0) return drag(argv[2], argc - 3, argv + 3);
    if (argc == 4 && strcmp(argv[1], "attach-fd") == 0) return attach_fd(argv[2], argv[3]);
    fprintf(stderr, "usage: test-docs unit|portal LOG MOUNT|protocol DIR SERVER|key PATH...|drag NAME FILE...|attach-fd NAME TARGET\n");
    return 2;
}
