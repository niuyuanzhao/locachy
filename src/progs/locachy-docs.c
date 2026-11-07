/*
 * locachy-docs (/usr/local/lib/locachy/locachy-docs): the host's document
 * service, through which a private file of the user's home is dragged into
 * one container and no other (tools/DOCUMENTS.md).
 *
 *   locachy-docs              the service (rc.conf service_documents), as
 *                             root: in a cgroup of its own (DOCS_CGROUP) it
 *                             runs, as the user, the documents bus (a
 *                             dbus-daemon only the user reaches, never a
 *                             container), on which the packaged
 *                             xdg-document-portal and xdg-permission-store
 *                             start by their own service files, and the
 *                             server below; it attaches the containers'
 *                             views and ends all of it when one part ends
 *                             (process 1 starts it again)
 *   locachy-docs --attach NAME  (as root: link/locachy-app before container
 *                             NAME starts) DOCS_VIEWS/NAME made, and NAME's
 *                             view of the document portal attached on it
 *                             when the service runs; the service attaches
 *                             every DOCS_VIEWS/NAME itself when it starts
 *   locachy-docs --user       the server, as the user (the service starts it,
 *                             with the control socket on fd 3)
 *
 * The server answers the control socket (docs-proto.h), which only root
 * reaches; a container's process (cgroup app_NAME) is refused even then.
 * begin: the files of a drag become documents with no permission for any
 * application (AddFull, app id "", not persistent, not reused). drop: from
 * labwc before it lets a drop through, GrantPermissions(read) for container
 * NAME's app id (docs-proto.h) once NAME runs and has its view. end
 * cancelled, a connection that closes before its end, a drop refused: the
 * documents are deleted, their grants revoked first (labwc may have given
 * up waiting for the answer). A finished drop's documents stay until the
 * container's cgroup is empty (cgroup.events), then they go the same way.
 *
 * A view: the document portal's FUSE mount (/run/user/UID/doc, the user's,
 * without allow_other) shows container NAME only by-app/APPID: the documents
 * it may read. Root cannot look up a path in it, so the server opens it
 * (O_PATH, as the user) and root clones that (open_tree) onto DOCS_VIEWS/NAME
 * (move_mount): a directory of a shared mount. link/locachy-app binds that
 * directory itself (not the view on it) into the container as a slave at
 * /run/user/UID/doc, then attaches the view again: every attach and detach
 * on the host (the service started again) reaches the running container.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "docs-proto.h"

#define USER_UID 1000
#ifndef DOCS_CGROUP
#define DOCS_CGROUP "/sys/fs/cgroup/locachy-docs"
#endif
#ifndef CGROUP_ROOT
#define CGROUP_ROOT "/sys/fs/cgroup" /* the containers' app_NAME */
#endif
#ifndef BUS_CONF
#define BUS_CONF "/usr/local/share/locachy/docs-bus.conf"
#endif
#ifndef SELF
#define SELF "/usr/local/lib/locachy/locachy-docs"
#endif
#define DOCS_LOCK DOCS_DIR "/lock"
#define DOCS_USER DOCS_DIR "/user"  /* the user's: the bus, the portal's data */
#define DOCS_BUS DOCS_USER "/bus"
#define DOCS_MAX 1024               /* documents of one drag */
#define LISTEN_FD 3
#define READY_FD 4
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif

#define PORTAL "org.freedesktop.portal.Documents"
#define PORTAL_PATH "/org/freedesktop/portal/documents"

__attribute__((format(printf, 1, 2)))
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("locachy-docs: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* --- Root: the views --- */

/* A directory of root's (mode), made if missing; not a link, not another's. */
static int root_dir(const char *path, mode_t mode)
{
    if (mkdir(path, mode) < 0 && errno != EEXIST) {
        say("cannot make %s: %s", path, strerror(errno));
        return -1;
    }
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_uid != 0 || fchmod(fd, mode) < 0) {
        say("%s is not a folder of root's", path);
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* Whatever is mounted on path, unmounted (lazily: a container may still be
 * reading its view). */
static void detach(const char *path)
{
    for (int i = 0; i < 64 && umount2(path, MNT_DETACH | UMOUNT_NOFOLLOW) == 0; i++) {}
}

/* DOCS_VIEWS: a tmpfs of its own, shared, so that what is mounted on a
 * directory in it reaches the containers' slave binds of that directory.
 * Under DOCS_LOCK (the service and locachy-app make it). */
static int views_ready(void)
{
    struct stat dir, views;
    if (root_dir(DOCS_VIEWS, 0755) < 0) return -1;
    if (stat(DOCS_DIR, &dir) < 0 || stat(DOCS_VIEWS, &views) < 0) return -1;
    if (dir.st_dev == views.st_dev &&
        mount("docs-views", DOCS_VIEWS, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0755,size=1m") < 0) {
        say("cannot mount %s: %s", DOCS_VIEWS, strerror(errno));
        return -1;
    }
    if (mount(NULL, DOCS_VIEWS, NULL, MS_SHARED, NULL) < 0) {
        say("cannot make %s shared: %s", DOCS_VIEWS, strerror(errno));
        return -1;
    }
    return 0;
}

static int lock_docs(void)
{
    if (root_dir("/run/locachy", 0755) < 0 || root_dir(DOCS_DIR, 0755) < 0) return -1;
    int fd = open(DOCS_LOCK, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0 && flock(fd, LOCK_EX) < 0) {
        close(fd);
        fd = -1;
    }
    if (fd < 0) say("cannot lock %s: %s", DOCS_LOCK, strerror(errno));
    return fd;
}

/* DOCS_VIEWS/NAME, made: a directory of root's on the views mount, checked
 * in a clone of that mount alone (none of the views on it), never through
 * the view attached on it, the user's FUSE mount, which root cannot enter. */
static int view_dir(const char *name, char *path, size_t size)
{
    int lock = lock_docs();
    int ok = lock >= 0 && views_ready() == 0 && snprintf(path, size, "%s/%s", DOCS_VIEWS, name) < (int)size &&
             (mkdir(path, 0755) == 0 || errno == EEXIST);
    int bare = ok ? open_tree(AT_FDCWD, DOCS_VIEWS, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC) : -1;
    struct stat st;
    ok = bare >= 0 && fstatat(bare, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode) &&
         st.st_uid == 0 && fchmodat(bare, name, 0755, 0) == 0;
    if (!ok && lock >= 0) say("%s/%s is not a folder of root's: %s", DOCS_VIEWS, name, strerror(errno));
    if (bare >= 0) close(bare);
    if (lock >= 0) close(lock);
    return ok ? 0 : -1;
}

/* The server's view of container NAME: an O_PATH fd of by-app/APPID of the
 * document portal, opened as the user. -1 when the service does not run. */
static int ask_view(const char *name)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX, .sun_path = DOCS_CONTROL };
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct timeval limit = { .tv_sec = 10 };
    if (s < 0 || setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit) < 0 ||
        connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        if (s >= 0) close(s);
        return -1;
    }
    char msg[128], reply[256];
    int n = snprintf(msg, sizeof msg, "view %s", name);
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = reply, .iov_len = sizeof reply - 1 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control };
    int fd = -1;
    ssize_t got = send(s, msg, (size_t)n, MSG_NOSIGNAL) == n ? recvmsg(s, &m, MSG_CMSG_CLOEXEC) : -1;
    struct cmsghdr *c = got > 0 ? CMSG_FIRSTHDR(&m) : NULL;
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len == CMSG_LEN(sizeof(int))) {
        memcpy(&fd, CMSG_DATA(c), sizeof fd);
    }
    if (got > 0) reply[got] = '\0';
    if (got <= 0 || strcmp(reply, "ok") != 0 || fd < 0) {
        say("container %s: no view from the server: %s", name, got > 0 ? reply : "no answer");
        if (fd >= 0) close(fd);
        fd = -1;
    }
    close(s);
    return fd;
}

/* fd's tree cloned onto DOCS_VIEWS/NAME, in place of what was there. */
static int attach(const char *name, int fd)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", DOCS_VIEWS, name) >= (int)sizeof path) return -1;
    int tree = open_tree(fd, "", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH);
    if (tree < 0) {
        say("container %s: cannot clone its view: %s", name, strerror(errno));
        return -1;
    }
    /* Private before it is attached: a peer of nothing but what the shared
     * mount gives it there. */
    struct mount_attr attr = { .propagation = MS_PRIVATE };
    if (mount_setattr(tree, "", AT_EMPTY_PATH | AT_RECURSIVE, &attr, sizeof attr) < 0) {
        say("container %s: its view stays a peer of the portal's mount: %s", name, strerror(errno));
    }
    detach(path);
    int r = move_mount(tree, "", AT_FDCWD, path, MOVE_MOUNT_F_EMPTY_PATH);
    if (r < 0) say("container %s: cannot attach its view on %s: %s", name, path, strerror(errno));
    close(tree);
    return r;
}

/* Under DOCS_LOCK: the service and a container's start may attach at once. */
static int attach_name(const char *name)
{
    int fd = ask_view(name);
    if (fd < 0) return -1;
    int lock = lock_docs();
    int r = lock >= 0 ? attach(name, fd) : -1;
    if (lock >= 0) close(lock);
    close(fd);
    return r;
}

/* locachy-docs --attach NAME */
static int attach_command(const char *name)
{
    char path[PATH_MAX];
    if (geteuid() != 0 || !docs_container_name(name)) {
        say("usage: locachy-docs --attach NAME (as root; NAME a container)");
        return 2;
    }
    if (view_dir(name, path, sizeof path) < 0) return 1;
    if (attach_name(name) < 0) {
        say("container %s: no document view now (the document service attaches it when it runs)", name);
        return 1;
    }
    return 0;
}

/* Every view directory there is (containers running or not: a view grants
 * nothing by itself), attached anew. */
static void attach_all(void)
{
    DIR *d = opendir(DOCS_VIEWS);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (docs_container_name(e->d_name) && e->d_type == DT_DIR) attach_name(e->d_name);
    }
    if (d) closedir(d);
}

static void detach_all(void)
{
    DIR *d = opendir(DOCS_VIEWS);
    struct dirent *e;
    char path[PATH_MAX];
    while (d && (e = readdir(d)) != NULL) {
        if (docs_valid_name(e->d_name) && snprintf(path, sizeof path, "%s/%s", DOCS_VIEWS, e->d_name) < (int)sizeof path) {
            detach(path);
        }
    }
    if (d) closedir(d);
}

/* --- Root: the service --- */

static struct passwd user;
static char user_name[64], user_home[PATH_MAX], runtime[64], doc_mount[80];
static pid_t bus_pid, server_pid;

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? write(fd, text, strlen(text)) : -1;
    if (fd >= 0) close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int cgroup_populated(const char *dir)
{
    char path[PATH_MAX], line[128];
    snprintf(path, sizeof path, "%s/cgroup.events", dir);
    FILE *in = fopen(path, "re");
    int populated = 0;
    while (in && fgets(line, sizeof line, in)) {
        if (strncmp(line, "populated ", 10) == 0) populated = atoi(line + 10);
    }
    if (in) fclose(in);
    return populated;
}

/* /dev/fuse as the portal needs it: libfuse opens it as the user (no
 * fusermount3 fallback when that open fails), so the node must be as udevd
 * makes it (0666, the packaged rule), not as the kernel made it (0600). At
 * boot the service starts before udevd has described the devices; as
 * udev-client asks of a program that needs a device, it asks udevd for this
 * one itself and waits for it (seat does the same for its devices). */
static int fuse_ready(void)
{
    struct stat st = { 0 };
    if (access("/dev/fuse", F_OK) != 0 && system("/sbin/modprobe -q fuse") != 0) {
        say("no /dev/fuse: the document portal cannot mount");
        return -1;
    }
    if (stat("/dev/fuse", &st) == 0 && (st.st_mode & 0006) == 0006) return 0;
    for (int i = 0; i < 100 && access("/run/udev/control", F_OK) != 0; i++) usleep(100000);
    if (access("/run/udev/control", F_OK) != 0 ||
        system("timeout 30 /usr/bin/udevadm trigger --action=add --subsystem-match=misc "
               "--sysname-match=fuse --settle") != 0) {
        say("udevd did not describe /dev/fuse");
    }
    if (stat("/dev/fuse", &st) < 0 || (st.st_mode & 0006) != 0006) {
        say("/dev/fuse is not open to the user (mode %o): the document portal cannot mount",
            (unsigned)(st.st_mode & 07777));
        return -1;
    }
    return 0;
}

/* DOCS_CGROUP, emptied of what an earlier run left, entered. */
static int cgroup_enter(void)
{
    if (mkdir(DOCS_CGROUP, 0755) < 0 && errno != EEXIST) {
        say("cannot make %s: %s", DOCS_CGROUP, strerror(errno));
        return -1;
    }
    if (cgroup_populated(DOCS_CGROUP)) {
        say("ending what an earlier run left in %s", DOCS_CGROUP);
        write_text(DOCS_CGROUP "/cgroup.kill", "1");
        for (int i = 0; i < 100 && cgroup_populated(DOCS_CGROUP); i++) usleep(20000);
    }
    if (write_text(DOCS_CGROUP "/cgroup.procs", "0") < 0) {
        say("cannot enter %s: %s", DOCS_CGROUP, strerror(errno));
        return -1;
    }
    return 0;
}

/* The user's runtime directory, where the portal mounts doc: nothing left
 * mounted there from before. */
static int runtime_ready(void)
{
    if (root_dir("/run/user", 0755) < 0) return -1;
    if (mkdir(runtime, 0700) == 0 && chown(runtime, user.pw_uid, user.pw_gid) < 0) return -1;
    int fd = open(runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    int ok = fd >= 0 && fstat(fd, &st) == 0 && st.st_uid == user.pw_uid;
    if (fd >= 0) close(fd);
    if (!ok) {
        say("%s is not %s's runtime directory", runtime, user_name);
        return -1;
    }
    detach(doc_mount);
    /* The user's directory for the bus and the portal's data: only root
     * can rename it (DOCS_DIR is root's). */
    if (mkdir(DOCS_USER, 0700) < 0 && errno != EEXIST) return -1;
    fd = open(DOCS_USER, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ok = fd >= 0 && fchown(fd, user.pw_uid, user.pw_gid) == 0 && fchmod(fd, 0700) == 0;
    if (fd >= 0) close(fd);
    if (!ok) say("cannot make %s: %s", DOCS_USER, strerror(errno));
    return ok ? 0 : -1;
}

static int listen_control(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX, .sun_path = DOCS_CONTROL };
    unlink(DOCS_CONTROL);
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0 || chmod(DOCS_CONTROL, 0600) < 0 || listen(s, 16) < 0) {
        say("cannot listen on %s: %s", DOCS_CONTROL, strerror(errno));
        if (s >= 0) close(s);
        return -1;
    }
    return s;
}

/* argv as the user, in this process group and cgroup; fd3 and fd4 (if not
 * -1) as 3 and 4. nnp: no new privileges (not for the bus: the portal it
 * starts mounts its FUSE file system through the setuid fusermount3). */
static pid_t spawn(char *const argv[], int fd3, int fd4, int nnp)
{
    pid_t parent = getpid(), pid = fork();
    if (pid != 0) return pid;
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    int a = fd3 >= 0 ? fcntl(fd3, F_DUPFD_CLOEXEC, 10) : -1, b = fd4 >= 0 ? fcntl(fd4, F_DUPFD_CLOEXEC, 10) : -1;
    if ((fd3 >= 0 && (a < 0 || dup2(a, LISTEN_FD) < 0)) || (fd4 >= 0 && (b < 0 || dup2(b, READY_FD) < 0))) _exit(125);
    int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null >= 0) dup2(null, STDIN_FILENO);
    if (setgroups(0, NULL) < 0 || setgid(user.pw_gid) < 0 || setuid(user.pw_uid) < 0 || getuid() != user.pw_uid ||
        geteuid() != user.pw_uid || getegid() != user.pw_gid) _exit(125);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) _exit(125);
    if (nnp && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) _exit(125);
    char data[PATH_MAX];
    snprintf(data, sizeof data, "%s/data", DOCS_USER);
    mkdir(data, 0700);
    clearenv();
    setenv("HOME", user_home, 1);
    setenv("USER", user_name, 1);
    setenv("LOGNAME", user_name, 1);
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/bin", 1);
    setenv("XDG_RUNTIME_DIR", runtime, 1);
    /* Kept in the runtime: the documents are transient; nothing remains in
     * the home's flatpak database. */
    setenv("XDG_DATA_HOME", data, 1);
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=" DOCS_BUS, 1);
    if (chdir("/") < 0) _exit(125);
    execv(argv[0], argv);
    say("cannot run %s: %s", argv[0], strerror(errno));
    _exit(127);
}

/* A line from fd within seconds (the bus's address once it listens, the
 * server's ready byte); 0 when it came. */
static int await(int fd, double seconds, char *line, size_t size)
{
    size_t n = 0;
    double until = now_s() + seconds;
    while (n + 1 < size) {
        int left = (int)((until - now_s()) * 1000);
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (left <= 0 || poll(&p, 1, left) <= 0) break;
        ssize_t r = read(fd, line + n, size - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        if (memchr(line, '\n', n)) break;
    }
    line[n] = '\0';
    return n > 0 ? 0 : -1;
}

/* Everything in DOCS_CGROUP but this process ended: asked, then killed. */
static void end_children(void)
{
    for (int round = 0; round < 2; round++) {
        double until = now_s() + 3;
        int left = 1, sig = round == 0 ? SIGTERM : SIGKILL;
        while (left && now_s() < until) {
            left = 0;
            FILE *in = fopen(DOCS_CGROUP "/cgroup.procs", "re");
            int pid;
            while (in && fscanf(in, "%d", &pid) == 1) {
                if (pid > 0 && pid != getpid()) {
                    left = 1;
                    kill(pid, sig);
                }
            }
            if (in) fclose(in);
            /* Its own children too, should the cgroup not list them. */
            if (bus_pid > 0 && kill(bus_pid, sig) == 0) left = 1;
            if (server_pid > 0 && kill(server_pid, sig) == 0) left = 1;
            pid_t gone;
            while ((gone = waitpid(-1, NULL, WNOHANG)) > 0) {
                if (gone == bus_pid) bus_pid = 0;
                if (gone == server_pid) server_pid = 0;
            }
            if (left) usleep(50000);
            if (round == 1) break;
        }
    }
}

static int service(void)
{
    umask(077);
    if (geteuid() != 0) {
        say("the service runs as root");
        return 2;
    }
    struct passwd *pw = getpwuid(USER_UID);
    if (!pw || !pw->pw_dir || pw->pw_dir[0] != '/') {
        say("there is no user %d", USER_UID);
        return 1;
    }
    user = *pw;
    snprintf(user_name, sizeof user_name, "%s", pw->pw_name);
    snprintf(user_home, sizeof user_home, "%s", pw->pw_dir);
    snprintf(runtime, sizeof runtime, "/run/user/%u", (unsigned)pw->pw_uid);
    snprintf(doc_mount, sizeof doc_mount, "%s/doc", runtime);
    user.pw_name = user_name;
    user.pw_dir = user_home;
    if (cgroup_enter() < 0) return 1;
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    if (fuse_ready() < 0) return 1;
    int lock = lock_docs();
    int ok = lock >= 0 && views_ready() == 0;
    if (ok) detach_all(); /* views of an earlier run's portal */
    if (lock >= 0) close(lock);
    if (!ok || runtime_ready() < 0) return 1;
    sigset_t handled;
    sigemptyset(&handled);
    sigaddset(&handled, SIGCHLD);
    sigaddset(&handled, SIGTERM);
    sigaddset(&handled, SIGINT);
    sigaddset(&handled, SIGHUP);
    sigprocmask(SIG_BLOCK, &handled, NULL);
    int signals = signalfd(-1, &handled, SFD_CLOEXEC);
    int control = listen_control(), address[2] = { -1, -1 }, ready[2] = { -1, -1 };
    if (signals < 0 || control < 0 || pipe2(address, O_CLOEXEC) < 0 || pipe2(ready, O_CLOEXEC) < 0) return 1;

    char print[32], line[256];
    snprintf(print, sizeof print, "--print-address=%d", READY_FD);
    char *bus_argv[] = { "/usr/bin/dbus-daemon", "--config-file=" BUS_CONF, "--nofork", "--nopidfile",
                         "--address=unix:path=" DOCS_BUS, print, NULL };
    bus_pid = spawn(bus_argv, -1, address[1], 0);
    close(address[1]);
    if (bus_pid < 0 || await(address[0], 10, line, sizeof line) < 0) {
        say("the documents bus did not start");
        end_children();
        return 1;
    }
    close(address[0]);
    char *server_argv[] = { SELF, "--user", NULL };
    server_pid = spawn(server_argv, control, ready[1], 1);
    close(control);
    close(ready[1]);
    /* The server is ready once the portal has mounted its file system. */
    if (server_pid < 0 || await(ready[0], 30, line, sizeof line) < 0) {
        say("the server did not start");
        end_children();
        unlink(DOCS_CONTROL);
        return 1;
    }
    close(ready[0]);
    attach_all();
    say("serving %s (bus pid %d, server pid %d)", DOCS_CONTROL, (int)bus_pid, (int)server_pid);

    int status = 0;
    for (;;) {
        struct signalfd_siginfo si;
        if (read(signals, &si, sizeof si) != sizeof si) {
            if (errno == EINTR) continue;
            status = 1;
            break;
        }
        if (si.ssi_signo != SIGCHLD) {
            say("ending (signal %u)", si.ssi_signo);
            break;
        }
        pid_t pid;
        int st, gone = 0;
        while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
            if (pid == bus_pid || pid == server_pid) {
                say("%s ended (status %d): ending, to start again", pid == bus_pid ? "the bus" : "the server",
                    WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
                gone = 1;
            }
        }
        if (gone) {
            status = 1;
            break;
        }
    }
    end_children();
    int l = lock_docs();
    detach_all();
    if (l >= 0) close(l);
    detach(doc_mount);
    unlink(DOCS_CONTROL);
    return status;
}

/* --- The server, as the user --- */

enum { DROP_NONE, DROP_CONTAINER, DROP_HOST };

struct conn {
    int fd;
    pid_t pid;
    int kind;              /* docs_peer_kind */
    char comm[16];
    struct conn *next;
};

struct session {
    unsigned id;
    struct conn *conn;     /* of its begin; NULL once it ended (done) */
    pid_t pid;             /* that connection's peer: the drag's source */
    GPtrArray *docs;       /* document ids */
    int drop;
    char name[65];         /* DROP_CONTAINER: the container */
    struct session *next;
};

struct watch {
    int wd;
    char name[65];
    struct watch *next;
};

static GDBusConnection *bus;
static GMainLoop *loop;
static char mountpoint[PATH_MAX], home[PATH_MAX];
static struct conn *conns;
static struct session *sessions;
static struct watch *watches;
static unsigned next_session;
static int notify = -1;

static GVariant *portal_call(const char *method, GVariant *args, GUnixFDList *fds, int timeout_ms, char *why, size_t size)
{
    GError *error = NULL;
    GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync(bus, PORTAL, PORTAL_PATH, PORTAL, method, args, NULL,
                                                               G_DBUS_CALL_FLAGS_NONE, timeout_ms, fds, NULL, NULL,
                                                               &error);
    if (!r) {
        snprintf(why, size, "%s: %s", method, error ? error->message : "failed");
        g_clear_error(&error);
    }
    return r;
}

/* The session's documents gone: a container's grant revoked first (also
 * one the compositor gave up waiting for), then each document deleted. */
static void delete_docs(struct session *s)
{
    char why[256], app[256];
    int granted = s->drop == DROP_CONTAINER && docs_app_id(s->name, app, sizeof app) == 0;
    for (guint i = 0; i < s->docs->len; i++) {
        const char *id = g_ptr_array_index(s->docs, i);
        GVariant *r = granted ? portal_call("RevokePermissions", g_variant_new("(ss^as)", id, app,
                                            (char *[]){ "read", NULL }), NULL, 5000, why, sizeof why) : NULL;
        if (r) g_variant_unref(r);
        else if (granted) say("%s", why);
        r = portal_call("Delete", g_variant_new("(s)", id), NULL, 5000, why, sizeof why);
        if (r) g_variant_unref(r);
        else say("%s", why);
    }
    g_ptr_array_set_size(s->docs, 0);
}

static void session_free(struct session *s)
{
    for (struct session **p = &sessions; *p; p = &(*p)->next) {
        if (*p == s) {
            *p = s->next;
            break;
        }
    }
    g_ptr_array_unref(s->docs);
    g_free(s);
}

static void session_drop(struct session *s)
{
    delete_docs(s);
    session_free(s);
}

static struct session *session_of(struct conn *c, const char *id)
{
    for (struct session *s = sessions; s; s = s->next) {
        char text[16];
        snprintf(text, sizeof text, "%u", s->id);
        if (s->conn == c && strcmp(text, id) == 0) return s;
    }
    return NULL;
}

static void reply(struct conn *c, const char *text, int fd)
{
    char control[CMSG_SPACE(sizeof(int))] = { 0 };
    struct iovec iov = { .iov_base = (void *)text, .iov_len = strlen(text) };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (fd >= 0) {
        m.msg_control = control;
        m.msg_controllen = sizeof control;
        struct cmsghdr *h = CMSG_FIRSTHDR(&m);
        h->cmsg_level = SOL_SOCKET;
        h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h), &fd, sizeof fd);
    }
    if (sendmsg(c->fd, &m, MSG_NOSIGNAL) < 0) say("pid %d: reply not sent: %s", (int)c->pid, strerror(errno));
}

__attribute__((format(printf, 2, 3)))
static void replyf(struct conn *c, const char *fmt, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    for (char *p = text; *p; p++) {
        if (*p == '\n') *p = ' ';
    }
    reply(c, text, -1);
}

/* begin / begin-more SESSION: names (lines after the first) and fds. */
static void begin(struct conn *c, char *text, const int *fds, int nfds)
{
    char *names[DOCS_FDS_MAX + 1], *save = NULL;
    int n = 0;
    char *head = strtok_r(text, "\n", &save);
    for (char *l = strtok_r(NULL, "\n", &save); l && n <= DOCS_FDS_MAX; l = strtok_r(NULL, "\n", &save)) {
        names[n++] = l;
    }
    struct session *s = NULL;
    if (strcmp(head, "begin") == 0) {
        s = g_new0(struct session, 1);
        s->id = ++next_session;
        s->conn = c;
        s->pid = c->pid;
        s->docs = g_ptr_array_new_with_free_func(g_free);
        s->next = sessions;
        sessions = s;
    } else if (head && strncmp(head, "begin-more ", 11) == 0) {
        s = session_of(c, head + 11);
    }
    if (!s || s->drop != DROP_NONE) {
        replyf(c, "error no such drag");
        return;
    }
    char why[512] = "";
    if (n != nfds || n == 0 || n > DOCS_FDS_MAX) {
        snprintf(why, sizeof why, "%d names for %d files", n, nfds);
    } else if (s->docs->len + (guint)n > DOCS_MAX) {
        snprintf(why, sizeof why, "more than %d files", DOCS_MAX);
    }
    /* What each fd is, as the portal will see it: its path's last name
     * must be the name files saw, in the home, a file or a folder. */
    int dirs[DOCS_FDS_MAX] = { 0 }, ndirs = 0;
    for (int i = 0; i < n && !why[0]; i++) {
        char at[64], path[PATH_MAX];
        struct stat st;
        int flags = fcntl(fds[i], F_GETFL);
        snprintf(at, sizeof at, "/proc/self/fd/%d", fds[i]);
        ssize_t r = readlink(at, path, sizeof path - 1);
        if (r > 0) path[r] = '\0';
        const char *base = r > 0 ? strrchr(path, '/') : NULL;
        size_t hlen = strlen(home);
        if (flags < 0 || !(flags & O_PATH) || (flags & O_NOFOLLOW) || fstat(fds[i], &st) < 0) {
            snprintf(why, sizeof why, "%s: not an O_PATH descriptor that follows links", names[i]);
        } else if (!base || strcmp(base + 1, names[i]) != 0) {
            snprintf(why, sizeof why, "%s: renamed or moved", names[i]);
        } else if (strncmp(path, home, hlen) != 0 || path[hlen] != '/') {
            snprintf(why, sizeof why, "%s: not in the home", names[i]);
        } else if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
            snprintf(why, sizeof why, "%s: neither a file nor a folder", names[i]);
        } else {
            dirs[i] = S_ISDIR(st.st_mode);
            ndirs += dirs[i];
        }
    }
    /* Files, then folders (the DIRECTORY flag is per call): AddFull, no
     * application's permission, a document of its own each time. */
    char *ids[DOCS_FDS_MAX] = { 0 };
    for (int pass = 0; pass < 2 && !why[0]; pass++) {
        if ((pass == 1 && ndirs == 0) || (pass == 0 && ndirs == n)) continue;
        GUnixFDList *list = g_unix_fd_list_new();
        GVariantBuilder handles;
        g_variant_builder_init(&handles, G_VARIANT_TYPE("ah"));
        int order[DOCS_FDS_MAX], k = 0;
        for (int i = 0; i < n && !why[0]; i++) {
            if (dirs[i] != pass) continue;
            int h = g_unix_fd_list_append(list, fds[i], NULL);
            if (h < 0) snprintf(why, sizeof why, "cannot pass %s", names[i]);
            g_variant_builder_add(&handles, "h", h);
            order[k++] = i;
        }
        GVariant *r = NULL;
        if (why[0]) {
            g_variant_builder_clear(&handles);
        } else {
            r = portal_call("AddFull", g_variant_new("(ahus^as)", &handles, pass ? 8u : 0u, "", (char *[]){ NULL }),
                            list, 10000, why, sizeof why);
        }
        g_object_unref(list);
        if (!r) break;
        gchar **got = NULL;
        g_variant_get(r, "(^as@a{sv})", &got, NULL);
        g_variant_unref(r);
        guint count = got ? g_strv_length(got) : 0;
        for (guint i = 0; i < count; i++) {
            if (got[i][0]) g_ptr_array_add(s->docs, g_strdup(got[i])); /* deleted with the session */
            if (i < (guint)k) ids[order[i]] = got[i][0] ? g_strdup(got[i]) : NULL;
        }
        if (count != (guint)k) snprintf(why, sizeof why, "the portal made %u documents of %d", count, k);
        g_strfreev(got);
    }
    GString *out = g_string_new(NULL);
    g_string_printf(out, "ok %u", s->id);
    for (int i = 0; i < n && !why[0]; i++) {
        if (!ids[i] || strchr(ids[i], '/')) {
            snprintf(why, sizeof why, "%s: no document", names[i]);
            break;
        }
        g_string_append_printf(out, "\n%s/%s/%s", mountpoint, ids[i], names[i]);
    }
    if (out->len >= DOCS_MSG_MAX) snprintf(why, sizeof why, "the paths do not fit a reply");
    if (why[0]) {
        /* All or nothing: the drag's documents so far go too. */
        say("pid %d: drag refused: %s", (int)c->pid, why);
        session_drop(s);
        replyf(c, "error %s", why);
    } else {
        reply(c, out->str, -1);
    }
    g_string_free(out, TRUE);
    for (int i = 0; i < DOCS_FDS_MAX; i++) g_free(ids[i]);
}

/* end SESSION done|cancelled: the drag's last message, unanswered (files
 * closes the connection right after it). */
static void end(struct conn *c, const char *text)
{
    char id[16], how[16];
    struct session *s = sscanf(text, "end %15s %15s", id, how) == 2 ? session_of(c, id) : NULL;
    if (!s || (strcmp(how, "done") != 0 && strcmp(how, "cancelled") != 0)) {
        say("pid %d: %.40s: no such drag", (int)c->pid, text);
        return;
    }
    if (strcmp(how, "done") == 0 && s->drop == DROP_CONTAINER) {
        s->conn = NULL; /* until the container stops */
    } else if (strcmp(how, "done") == 0 && s->drop == DROP_HOST) {
        session_free(s); /* the host's: transient documents, kept */
    } else {
        session_drop(s);
    }
}

static int populated(const char *name)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/app_%s", CGROUP_ROOT, name);
    return cgroup_populated(dir);
}

/* Container NAME's view is attached: DOCS_VIEWS/NAME is the portal's
 * by-app/APPID itself. */
static int has_view(const char *name, const char *app)
{
    char view[PATH_MAX], own[PATH_MAX];
    struct stat a, b;
    return snprintf(view, sizeof view, "%s/%s", DOCS_VIEWS, name) < (int)sizeof view &&
           snprintf(own, sizeof own, "%s/by-app/%s", mountpoint, app) < (int)sizeof own &&
           stat(view, &a) == 0 && stat(own, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

/* Container NAME's documents deleted once its cgroup is empty. */
static int watch_container(const char *name)
{
    for (struct watch *w = watches; w; w = w->next) {
        if (strcmp(w->name, name) == 0) return 0;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/app_%s/cgroup.events", CGROUP_ROOT, name);
    int wd = inotify_add_watch(notify, path, IN_MODIFY | IN_DELETE_SELF);
    if (wd < 0) return -1;
    struct watch *w = g_new0(struct watch, 1);
    w->wd = wd;
    snprintf(w->name, sizeof w->name, "%s", name);
    w->next = watches;
    watches = w;
    return 0;
}

static void revoke_container(const char *name)
{
    for (struct session *s = sessions, *next; s; s = next) {
        next = s->next;
        if (s->drop == DROP_CONTAINER && strcmp(s->name, name) == 0) {
            say("container %s ended: its %u documents deleted", name, s->docs->len);
            session_drop(s);
        }
    }
}

/* drop SOURCE_PID container NAME | host | xwayland, from labwc */
static void drop(struct conn *c, const char *text)
{
    int pid = 0, used = 0;
    char where[16] = "", name[80] = "";
    if (strcmp(c->comm, "labwc") != 0) {
        replyf(c, "refuse only the compositor tells drops");
        return;
    }
    if (sscanf(text, "drop %d %15s %n", &pid, where, &used) < 2 || pid <= 0) {
        replyf(c, "refuse malformed drop");
        return;
    }
    if (used) snprintf(name, sizeof name, "%s", text + used);
    struct session *s = NULL;
    for (struct session *t = sessions; t && !s; t = t->next) { /* newest first */
        if (t->pid == pid && t->conn && t->drop == DROP_NONE && t->docs->len > 0) s = t;
    }
    if (!s) {
        replyf(c, "refuse no drag of pid %d", pid);
        return;
    }
    if (strcmp(where, "host") == 0 && !name[0]) {
        s->drop = DROP_HOST;
        reply(c, "ok", -1);
        return;
    }
    char app[256], why[512] = "";
    if (strcmp(where, "container") == 0 && docs_valid_name(name) && docs_window_name(name)) {
        /* labwc asks "host" for one (locachy-drop.c): it is no container. */
        snprintf(why, sizeof why, "%s is a native window, not a container", name);
    } else if (strcmp(where, "container") != 0 || docs_app_id(name, app, sizeof app) < 0) {
        snprintf(why, sizeof why, "%s cannot be told apart", strcmp(where, "xwayland") == 0 ? "X11 programs" : "the target");
    } else if (!has_view(name, app)) {
        snprintf(why, sizeof why, "%s has no document view", name);
    } else if (watch_container(name) < 0 || !populated(name)) {
        snprintf(why, sizeof why, "%s is not running", name);
    } else {
        /* Recorded before the first grant: whatever ends the drag from
         * here (end cancelled, the files connection closing, a grant
         * failing, the container ending) revokes what was granted. labwc
         * may give up waiting before this answers. */
        s->drop = DROP_CONTAINER;
        snprintf(s->name, sizeof s->name, "%s", name);
    }
    for (guint i = 0; i < s->docs->len && !why[0]; i++) {
        GVariant *r = portal_call("GrantPermissions",
                                  g_variant_new("(ss^as)", (char *)g_ptr_array_index(s->docs, i), app,
                                                (char *[]){ "read", NULL }), NULL, 2000, why, sizeof why);
        if (r) g_variant_unref(r);
    }
    if (why[0]) {
        /* Refused: labwc cancels the drag; nothing of it stays readable. */
        say("pid %d: drop refused: %s", pid, why);
        session_drop(s);
        replyf(c, "refuse %s", why);
        return;
    }
    say("pid %d: %u documents readable by %s", pid, s->docs->len, name);
    reply(c, "ok", -1);
}

/* view NAME: from root's link/locachy-app or the service, not a window. */
static void view(struct conn *c, const char *name)
{
    char app[256], path[PATH_MAX];
    if (c->kind != DOCS_PEER_HOST || docs_app_id(name, app, sizeof app) < 0 ||
        snprintf(path, sizeof path, "%s/by-app/%s", mountpoint, app) >= (int)sizeof path) {
        replyf(c, "error no view for that");
        return;
    }
    int fd = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        replyf(c, "error %s: %s", path, strerror(errno));
        return;
    }
    reply(c, "ok", fd);
    close(fd);
}

static void conn_close(struct conn *c)
{
    for (struct session *s = sessions, *next; s; s = next) {
        next = s->next;
        if (s->conn == c) session_drop(s); /* never ended: cancelled */
    }
    for (struct conn **p = &conns; *p; p = &(*p)->next) {
        if (*p == c) {
            *p = c->next;
            break;
        }
    }
    close(c->fd);
    g_free(c);
}

static gboolean on_conn(gint fd, GIOCondition cond, gpointer data)
{
    struct conn *c = data;
    char text[DOCS_MSG_MAX + 1];
    char control[CMSG_SPACE(sizeof(int) * (DOCS_FDS_MAX + 1))];
    struct iovec iov = { .iov_base = text, .iov_len = DOCS_MSG_MAX };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control };
    ssize_t n = recvmsg(fd, &m, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR) && !(cond & (G_IO_HUP | G_IO_ERR))) return G_SOURCE_CONTINUE;
    int fds[DOCS_FDS_MAX + 1], nfds = 0;
    for (struct cmsghdr *h = n > 0 ? CMSG_FIRSTHDR(&m) : NULL; h; h = CMSG_NXTHDR(&m, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
        int count = (int)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < count && nfds <= DOCS_FDS_MAX; i++) memcpy(&fds[nfds++], CMSG_DATA(h) + i * sizeof(int), sizeof(int));
    }
    if (n <= 0) {
        for (int i = 0; i < nfds; i++) close(fds[i]);
        conn_close(c);
        return G_SOURCE_REMOVE;
    }
    text[n] = '\0';
    if ((m.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || nfds > DOCS_FDS_MAX || strlen(text) != (size_t)n) {
        replyf(c, "error malformed request");
    } else if (strncmp(text, "begin", 5) == 0) {
        begin(c, text, fds, nfds);
    } else if (nfds > 0) {
        replyf(c, "error unexpected descriptors");
    } else if (strncmp(text, "end ", 4) == 0) {
        end(c, text);
    } else if (strncmp(text, "drop ", 5) == 0) {
        drop(c, text);
    } else if (strncmp(text, "view ", 5) == 0) {
        view(c, text + 5);
    } else {
        replyf(c, "error unknown request");
    }
    for (int i = 0; i < nfds; i++) close(fds[i]);
    return G_SOURCE_CONTINUE;
}

/* A peer of the control socket: root (the socket is root's alone), and no
 * container's process even then. */
static int peer(int fd, struct conn *c)
{
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || cred.uid != 0 || cred.pid <= 0) return -1;
    int pidfd = -1;
    len = sizeof pidfd;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &len) < 0) pidfd = -1;
    char path[64], line[512] = "";
    snprintf(path, sizeof path, "/proc/%d/cgroup", (int)cred.pid);
    FILE *in = fopen(path, "re");
    if (!in || !fgets(line, sizeof line, in)) line[0] = '\0';
    if (in) fclose(in);
    snprintf(path, sizeof path, "/proc/%d/comm", (int)cred.pid);
    in = fopen(path, "re");
    if (!in || !fgets(c->comm, sizeof c->comm, in)) c->comm[0] = '\0';
    if (in) fclose(in);
    c->comm[strcspn(c->comm, "\n")] = '\0';
    /* Still the process that connected (its pid not taken by another). */
    int alive = pidfd < 0 || syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0) == 0 || errno == EPERM;
    if (pidfd >= 0) close(pidfd);
    c->pid = cred.pid;
    c->kind = docs_peer_kind(line);
    if (!alive || c->kind == DOCS_PEER_CONTAINER || c->kind == DOCS_PEER_UNKNOWN) {
        say("pid %d (%s) refused: %s", (int)cred.pid, c->comm, alive ? line : "gone");
        return -1;
    }
    return 0;
}

static gboolean on_listen(gint fd, GIOCondition cond, gpointer data)
{
    (void)cond; (void)data;
    int s = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (s < 0) return G_SOURCE_CONTINUE;
    struct conn *c = g_new0(struct conn, 1);
    c->fd = s;
    if (peer(s, c) < 0) {
        close(s);
        g_free(c);
        return G_SOURCE_CONTINUE;
    }
    c->next = conns;
    conns = c;
    g_unix_fd_add(s, G_IO_IN | G_IO_HUP | G_IO_ERR, on_conn, c);
    return G_SOURCE_CONTINUE;
}

static gboolean on_notify(gint fd, GIOCondition cond, gpointer data)
{
    (void)cond; (void)data;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n = read(fd, buf, sizeof buf);
    for (char *p = buf; n > 0 && p < buf + n;) {
        struct inotify_event *e = (struct inotify_event *)p;
        p += sizeof *e + e->len;
        for (struct watch **w = &watches; *w; w = &(*w)->next) {
            if ((*w)->wd != e->wd) continue;
            if ((e->mask & IN_IGNORED) || !populated((*w)->name)) {
                struct watch *gone = *w;
                revoke_container(gone->name);
                if (!(e->mask & IN_IGNORED)) inotify_rm_watch(notify, gone->wd);
                *w = gone->next;
                g_free(gone);
            }
            break;
        }
    }
    return G_SOURCE_CONTINUE;
}

static void portal_vanished(GDBusConnection *connection, const gchar *name, gpointer data)
{
    (void)connection; (void)data;
    say("%s is gone: ending", name);
    g_main_loop_quit(loop);
}

static gboolean on_term(gpointer data)
{
    (void)data;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static int serve(void)
{
    const char *address = getenv("DBUS_SESSION_BUS_ADDRESS"), *h = getenv("HOME");
    if (!address || !h || !realpath(h, home) || fcntl(LISTEN_FD, F_GETFD) < 0) {
        say("--user: started by the service only");
        return 2;
    }
    GError *error = NULL;
    bus = g_dbus_connection_new_for_address_sync(address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                  G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
    if (!bus) {
        say("no documents bus: %s", error->message);
        return 1;
    }
    g_dbus_connection_set_exit_on_close(bus, TRUE);
    char why[512];
    /* Started by its service file, it answers once its FUSE file system is
     * mounted. */
    GVariant *r = portal_call("GetMountPoint", NULL, NULL, 30000, why, sizeof why);
    if (!r) {
        say("no document portal: %s", why);
        return 1;
    }
    const char *mp = NULL;
    g_variant_get(r, "(^&ay)", &mp);
    snprintf(mountpoint, sizeof mountpoint, "%s", mp ? mp : "");
    g_variant_unref(r);
    if (mountpoint[0] != '/') {
        say("the document portal has no mount point");
        return 1;
    }
    loop = g_main_loop_new(NULL, FALSE);
    g_bus_watch_name_on_connection(bus, PORTAL, G_BUS_NAME_WATCHER_FLAGS_NONE, NULL, portal_vanished, NULL, NULL);
    notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (notify < 0) return 1;
    g_unix_fd_add(notify, G_IO_IN, on_notify, NULL);
    g_unix_fd_add(LISTEN_FD, G_IO_IN, on_listen, NULL);
    g_unix_signal_add(SIGTERM, on_term, NULL);
    g_unix_signal_add(SIGINT, on_term, NULL);
    if (write(READY_FD, "ready\n", 6) != 6) return 1;
    close(READY_FD);
    say("documents at %s", mountpoint);
    g_main_loop_run(loop);
    return 1; /* the service ends with it, and starts again */
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && strcmp(argv[1], "--user") == 0) return serve();
    if (argc == 3 && strcmp(argv[1], "--attach") == 0) return attach_command(argv[2]);
    if (argc == 1) return service();
    say("usage: locachy-docs [--attach NAME]");
    return 2;
}
