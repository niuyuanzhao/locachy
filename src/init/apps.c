/* Container jobs never own DRM or borrow home's control socket. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base.h"
#include "apps.h"

#define MAX_APPS 48
#ifndef APP_CONF
#define APP_CONF CONTAINERS_CONF
#endif
#ifndef APP_RUN
#define APP_RUN "/run/locachy/apps"
#endif
#ifndef APP_TOOL
#define APP_TOOL "/usr/local/bin/locachy-app"
#endif
#ifndef APP_CGROUP_ROOT
#define APP_CGROUP_ROOT "/sys/fs/cgroup"
#endif
#ifndef APP_DISPLAY
#define APP_DISPLAY "/run/user/0/locachy-0"
#endif
#ifndef APP_DISPLAY_READY
#define APP_DISPLAY_READY "/run/user/0/wayland-0"
#endif
/* Host programs shown as managed windows on the shared display
 * (apps_window_open): fixed executables, no recipe or configuration. */
#ifndef APP_NATIVE_DIR
#define APP_NATIVE_DIR "/usr/local/bin"
#endif
#ifndef APP_MIXER_COMMAND
#define APP_MIXER_COMMAND "/usr/bin/wiremix"
#endif
#ifndef APP_BLUETOOTH_COMMAND
#define APP_BLUETOOTH_COMMAND "/usr/bin/bluetui"
#endif
#ifndef APP_NETWORK_COMMAND
#define APP_NETWORK_COMMAND "/usr/bin/nmtui"
#endif
#ifndef APP_TERM_COMMAND
#define APP_TERM_COMMAND "/usr/local/lib/locachy/user-shell"
#endif
#ifndef APP_CLIPBOARD
#define APP_CLIPBOARD "/usr/local/lib/locachy/clipboard"
#endif
#ifndef APP_WAYLAND_RUNTIME
#define APP_WAYLAND_RUNTIME "/run/user/0"
#endif
#ifndef APP_WAYLAND_SOCKET
#define APP_WAYLAND_SOCKET "wayland-0"
#endif
#ifndef APP_WINDOW_CONTROL
#define APP_WINDOW_CONTROL "/run/user/0/locachy-control.sock"
#endif
#define MAX_NATIVE 16
/* The descriptors a native window inherits: it writes one byte to the first
 * once its first window is configured and drawn; it reads requests ("page
 * NAME") from the second until end of file. */
#define NATIVE_READY_FD 3
#define NATIVE_REQUEST_FD 4

/* What a native window may be given: nothing, a file (view: one it can read;
 * none: its chooser), a folder (files: none, the home) or a settings page. */
enum { TARGET_NONE, TARGET_FILE, TARGET_FOLDER, TARGET_PAGE };

struct native_program {
    const char *program, *title, *binary, *argument; /* argument: fixed (the mixer's command) */
    int single; /* one window: another request raises it (and changes its page) */
    int target;
};

static const struct native_program natives[] = {
    { "view", "View", APP_NATIVE_DIR "/view", NULL, 0, TARGET_FILE },
    { "files", "Files", APP_NATIVE_DIR "/files", NULL, 0, TARGET_FOLDER },
    { "settings", "Settings", APP_NATIVE_DIR "/settings", NULL, 1, TARGET_PAGE },
    { "mixer", "Mixer", APP_NATIVE_DIR "/term", APP_MIXER_COMMAND, 1, TARGET_NONE },
    { "bluetooth", "Bluetooth", APP_NATIVE_DIR "/term", APP_BLUETOOTH_COMMAND, 1, TARGET_NONE },
    { "network", "Network", APP_NATIVE_DIR "/term", APP_NETWORK_COMMAND, 1, TARGET_NONE },
    { "term", "Terminal", APP_NATIVE_DIR "/term", APP_TERM_COMMAND, 0, TARGET_NONE },
};

static const char *const settings_pages[] = { "network", "power", "devices", "date-time", "hardware" };

struct app {
    char name[64];
    pid_t launcher, job;
    int want_start, own_tor;
    char pending[16], error[240];
    char target[1024]; /* to open when it has started (apps_open) */
    double deadline;
    /* A native window (apps_window_open): its program, which is the launcher
     * itself, its readiness and request pipes (-1 when closed). It has no
     * configuration file; it is forgotten once nothing of it runs. */
    const struct native_program *native;
    int ready_fd, request_fd, seen_ready;
};

static struct app apps[MAX_APPS];
static int count;

static int valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= sizeof apps[0].name || !isalnum((unsigned char)name[0])) {
        return 0;
    }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.') {
            return 0;
        }
    }
    return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static struct app *lookup(const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(apps[i].name, name) == 0) {
            return &apps[i];
        }
    }
    return NULL;
}

static int populated(const struct app *app);

static void native_forget(struct app *app)
{
    char cg[160];
    if (app->ready_fd >= 0) close(app->ready_fd);
    if (app->request_fd >= 0) close(app->request_fd);
    app->ready_fd = app->request_fd = -1;
    snprintf(cg, sizeof cg, APP_CGROUP_ROOT "/app_%.63s", app->name);
    rmdir(cg); /* empty now; a cgroup still in use stays */
}

/* The containers there are: APP_CONF/NAME.conf. One whose configuration has
 * gone is forgotten once nothing of it runs. A native window has none: it is
 * forgotten once nothing of it runs, unless it failed (home shows why until
 * it is stopped). */
static void discover(void)
{
    for (int i = 0; i < count;) {
        char path[256];
        struct stat st;
        if (apps[i].native) {
            if (apps[i].launcher || apps[i].job || apps[i].want_start || apps[i].pending[0] ||
                apps[i].error[0] || populated(&apps[i])) {
                i++;
            } else {
                native_forget(&apps[i]);
                apps[i] = apps[--count];
            }
            continue;
        }
        snprintf(path, sizeof path, "%s/%s.conf", APP_CONF, apps[i].name);
        if (stat(path, &st) == 0 || apps[i].launcher || apps[i].job || apps[i].want_start ||
            apps[i].pending[0] || populated(&apps[i])) {
            i++;
            continue;
        }
        apps[i] = apps[--count];
    }
    DIR *dir = opendir(APP_CONF);
    struct dirent *entry;
    while (dir && (entry = readdir(dir)) != NULL) {
        char name[64], path[256];
        struct stat st;
        size_t len = strlen(entry->d_name);
        if (len <= 5 || len - 5 >= sizeof name || strcmp(entry->d_name + len - 5, ".conf") != 0) {
            continue;
        }
        snprintf(name, sizeof name, "%.*s", (int)(len - 5), entry->d_name);
        if (!valid_name(name) || lookup(name) || count == MAX_APPS) {
            continue;
        }
        snprintf(path, sizeof path, "%s/%s", APP_CONF, entry->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        memset(&apps[count], 0, sizeof apps[count]); /* a slot a forgotten entry left */
        memcpy(apps[count++].name, name, sizeof name);
    }
    if (dir) {
        closedir(dir);
    }
}

static long long number(const struct app *app, const char *file, const char *field)
{
    char path[192], line[128];
    snprintf(path, sizeof path, APP_CGROUP_ROOT "/app_%.63s/%s", app->name, file);
    FILE *in = fopen(path, "re");
    if (!in) {
        return 0;
    }
    long long result = 0;
    while (fgets(line, sizeof line, in)) {
        if (!field) {
            result = atoll(line);
            break;
        }
        size_t n = strlen(field);
        if (strncmp(line, field, n) == 0 && line[n] == ' ') {
            result = atoll(line + n + 1);
            break;
        }
    }
    fclose(in);
    return result;
}

static int populated(const struct app *app)
{
    return number(app, "cgroup.events", "populated") != 0;
}

static int frozen(const struct app *app)
{
    return number(app, "cgroup.events", "frozen") != 0;
}

static int ready(const struct app *app)
{
    char path[192];
    snprintf(path, sizeof path, APP_RUN "/%s.ready", app->name);
    return access(path, F_OK) == 0;
}

/* The shared display said display-ready (seat: its sockets, and X11 once
 * labwc's xinitrc has published it, when Xwayland is installed) for the
 * display socket it has now, by its inode: a new session's socket waits for
 * its own message. Its sockets alone come first: a program started then
 * finds no X11. */
static ino_t announced;

void apps_display_ready(void)
{
    struct stat st;
    announced = stat(APP_DISPLAY_READY, &st) == 0 ? st.st_ino : 0;
}

static int display_ready(void)
{
    struct stat st;
    return announced && access(APP_DISPLAY, F_OK) == 0 && stat(APP_DISPLAY_READY, &st) == 0 &&
           st.st_ino == announced;
}

static void failure(struct app *app, const char *message)
{
    snprintf(app->error, sizeof app->error, "%s", message);
    app->want_start = app->own_tor = 0;
    app->target[0] = '\0';
    app->pending[0] = '\0';
    app->deadline = 0;
    say("apps: %s: %s", app->name, app->error);
}

static pid_t launch(struct app *app, const char *action, int in_app);

void apps_init(void)
{
    mkdir("/run/locachy", 0755);
    mkdir(APP_RUN, 0700);
    discover();
}

static void write_value(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (write(fd, value, strlen(value)) < 0) {
        }
        close(fd);
    }
}

/* Everything left of a native window ends (cgroup.kill: also descendants in
 * sessions of their own). */
static void native_kill(const struct app *app)
{
    char path[192];
    snprintf(path, sizeof path, APP_CGROUP_ROOT "/app_%.63s/cgroup.kill", app->name);
    write_value(path, "1");
}

/* The program of a native window, in a cgroup of its own, as root (as the
 * direct programs are), with only the shared display's own Wayland socket and
 * the backend chosen explicitly: never process 1's control socket, a lease of
 * screens or input devices. */
static pid_t launch_native(struct app *app)
{
    const struct native_program *p = app->native;
    char log_path[192], cg[160];
    int ready[2], request[2];
    snprintf(log_path, sizeof log_path, APP_RUN "/%s.log", app->name);
    snprintf(cg, sizeof cg, APP_CGROUP_ROOT "/app_%.63s", app->name);
    if (mkdir(cg, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    if (pipe2(ready, O_CLOEXEC) < 0) {
        return -1;
    }
    if (pipe2(request, O_CLOEXEC) < 0) {
        close(ready[0]);
        close(ready[1]);
        return -1;
    }
    int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        close(fd);
    }
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        setsid();
        int in = open("/dev/null", O_RDONLY | O_CLOEXEC);
        int out = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (in >= 0) {
            dup2(in, STDIN_FILENO);
        }
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            dup2(out, STDERR_FILENO);
        }
        char path[192];
        snprintf(path, sizeof path, "%s/cgroup.procs", cg);
        int procs = open(path, O_WRONLY | O_CLOEXEC);
        if (procs < 0 || write(procs, "0", 1) != 1) {
            dprintf(2, "apps: cannot enter %s: %s\n", cg, strerror(errno));
            _exit(125);
        }
        close(procs);
        /* Out of the way of the fixed numbers first (dup2 onto another
         * number clears close-on-exec only on the copy). */
        int r = fcntl(ready[1], F_DUPFD_CLOEXEC, 10), q = fcntl(request[0], F_DUPFD_CLOEXEC, 10);
        if (r < 0 || q < 0 || dup2(r, NATIVE_READY_FD) < 0 || dup2(q, NATIVE_REQUEST_FD) < 0) {
            dprintf(2, "apps: cannot pass the window descriptors: %s\n", strerror(errno));
            _exit(125);
        }
        char ready_number[8], request_number[8];
        snprintf(ready_number, sizeof ready_number, "%d", NATIVE_READY_FD);
        snprintf(request_number, sizeof request_number, "%d", NATIVE_REQUEST_FD);
        unsetenv("LOCACHY_CONTROL");
        unsetenv("LOCACHY_WAYLAND_DISPLAY");
        unsetenv("WAYLAND_SOCKET");
        unsetenv("DISPLAY");
        setenv("LOCACHY_BACKEND", "wayland", 1);
        setenv("XDG_RUNTIME_DIR", APP_WAYLAND_RUNTIME, 1);
        setenv("WAYLAND_DISPLAY", APP_WAYLAND_SOCKET, 1);
        setenv("LOCACHY_WINDOW_TITLE", p->title, 1);
        setenv("LOCACHY_WINDOW_APP_ID", app->name, 1);
        setenv("LOCACHY_WINDOW_READY_FD", ready_number, 1);
        setenv("LOCACHY_REQUEST_FD", request_number, 1);
        char page[64];
        char *argv[3] = { (char *)p->binary, NULL, NULL };
        if (p->argument) {
            argv[1] = (char *)p->argument;
        } else if (p->target == TARGET_PAGE && app->target[0]) {
            snprintf(page, sizeof page, "--page=%.40s", app->target); /* one of settings_pages */
            argv[1] = page;
        } else if ((p->target == TARGET_FILE || p->target == TARGET_FOLDER) && app->target[0]) {
            argv[1] = app->target;
        }
        execv(p->binary, argv);
        dprintf(2, "apps: exec %s: %s\n", p->binary, strerror(errno));
        _exit(127);
    }
    close(ready[1]);
    close(request[0]);
    if (pid < 0) {
        close(ready[0]);
        close(request[1]);
        return -1;
    }
    fcntl(ready[0], F_SETFL, O_NONBLOCK);
    fcntl(request[1], F_SETFL, O_NONBLOCK);
    if (app->ready_fd >= 0) close(app->ready_fd);
    if (app->request_fd >= 0) close(app->request_fd);
    app->ready_fd = ready[0];
    app->request_fd = request[1];
    app->seen_ready = 0;
    return pid;
}

/* A native window started when its program says it has drawn its window. A
 * program that closes the descriptor without that while it still runs has no
 * window to show: it ends. */
static void native_tick(struct app *app)
{
    if (app->ready_fd >= 0) {
        char byte;
        ssize_t n = read(app->ready_fd, &byte, 1);
        if (n >= 0) { /* the byte, or its end without one */
            close(app->ready_fd);
            app->ready_fd = -1;
            if (n > 0) {
                app->seen_ready = 1;
            } else if (app->launcher && strcmp(app->pending, "starting") == 0) {
                char message[sizeof app->error];
                snprintf(message, sizeof message, "no window was shown; inspect " APP_RUN "/%.63s.log",
                         app->name);
                failure(app, message);
                native_kill(app);
            }
        }
    }
    if (strcmp(app->pending, "starting") == 0 && app->seen_ready && app->launcher) {
        app->pending[0] = '\0';
        app->deadline = 0;
    }
}

void apps_tick(void)
{
    for (int i = 0; i < count; i++) {
        struct app *app = &apps[i];
        if (app->want_start && display_ready()) {
            app->want_start = 0;
            pid_t pid = app->native ? launch_native(app) : launch(app, app->target[0] ? "open" : "start", 0);
            if (!app->native) {
                app->target[0] = '\0';
                app->own_tor = 0;
            }
            if (pid < 0) {
                failure(app, app->native ? "cannot start the window's program" : "cannot start container operation");
                continue;
            }
            app->launcher = pid;
        }
        if (app->native) {
            native_tick(app);
            if (strcmp(app->pending, "starting") == 0 && app->launcher == 0 && !app->want_start &&
                !app->seen_ready) {
                char message[sizeof app->error];
                snprintf(message, sizeof message, "the program ended before showing a window; inspect "
                         APP_RUN "/%.63s.log", app->name);
                failure(app, message);
                native_kill(app);
            }
            if (strcmp(app->pending, "stopping") == 0 && app->job == 0 && app->launcher == 0 && !populated(app)) {
                app->pending[0] = '\0';
                app->deadline = 0;
            }
            if (app->deadline > 0 && uptime() >= app->deadline) {
                char message[sizeof app->error];
                snprintf(message, sizeof message, "%s timed out; inspect " APP_RUN "/%.63s.log",
                         app->pending, app->name);
                int starting = strcmp(app->pending, "starting") == 0;
                failure(app, message);
                if (starting) {
                    native_kill(app); /* no window: nothing of it stays */
                }
            }
            continue;
        }
        if (strcmp(app->pending, "starting") == 0 && ready(app) && populated(app)) {
            app->pending[0] = '\0';
            app->deadline = 0;
        }
        if (!app->want_start && strcmp(app->pending, "starting") == 0 &&
            app->launcher == 0 && !populated(app)) {
            failure(app, "application exited before becoming ready");
        }
        if (strcmp(app->pending, "stopping") == 0 && app->job == 0 &&
            app->launcher == 0 && !populated(app)) {
            app->pending[0] = '\0';
            app->deadline = 0;
        }
        if (app->deadline > 0 && uptime() >= app->deadline) {
            char message[sizeof app->error];
            snprintf(message, sizeof message, "%s timed out; inspect " APP_RUN "/%s.log",
                     app->pending, app->name);
            failure(app, message);
        }
    }
}

int apps_busy(void)
{
    for (int i = 0; i < count; i++) {
        if (apps[i].want_start || apps[i].pending[0] || apps[i].job) {
            return 1;
        }
    }
    return 0;
}

int apps_display_state(void)
{
    discover();
    int any_frozen = 0;
    for (int i = 0; i < count; i++) {
        struct app *app = &apps[i];
        if (app->want_start || app->job > 0 || app->pending[0]) {
            return APPS_DISPLAY_ACTIVE;
        }
        if (populated(app)) {
            if (!frozen(app)) {
                return APPS_DISPLAY_ACTIVE;
            }
            any_frozen = 1;
        }
    }
    return any_frozen ? APPS_DISPLAY_FROZEN : APPS_DISPLAY_STOPPED;
}

static const char *state(const struct app *app)
{
    if (app->pending[0]) {
        return app->pending;
    }
    if (app->error[0]) {
        return "failed";
    }
    return populated(app) ? (frozen(app) ? "frozen" : "running") : "closed";
}

void apps_list(char *buf, size_t size)
{
    if (!size) {
        return;
    }
    discover();
    apps_tick();
    buf[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        struct app *app = &apps[i];
        char row[480];
        int n = snprintf(row, sizeof row, "%s\t%s\t%lld\t%lld\t%s\n",
                         app->name, state(app), number(app, "memory.current", NULL),
                         number(app, "memory.swap.current", NULL), app->error);
        if (n < 0 || (size_t)n >= sizeof row || (size_t)n >= size - used) {
            break;
        }
        memcpy(buf + used, row, (size_t)n + 1);
        used += (size_t)n;
    }
}

static pid_t launch(struct app *app, const char *action, int in_app)
{
    char log_path[192], cg[160];
    snprintf(log_path, sizeof log_path, APP_RUN "/%s.log", app->name);
    snprintf(cg, sizeof cg, APP_CGROUP_ROOT "/app_%.63s", app->name);
    if (in_app && mkdir(cg, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    setsid();
    unsetenv("LOCACHY_CONTROL");
    int in = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int out = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (in >= 0) {
        dup2(in, STDIN_FILENO);
    }
    if (out >= 0) {
        dup2(out, STDOUT_FILENO);
        dup2(out, STDERR_FILENO);
    }
    if (in_app) {
        char path[192];
        snprintf(path, sizeof path, "%s/cgroup.procs", cg);
        int fd = open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0 || write(fd, "0", 1) != 1) {
            dprintf(2, "apps: cannot enter %s: %s\n", cg, strerror(errno));
            _exit(125);
        }
        close(fd);
    }
    setenv("LOCACHY_WAYLAND_DISPLAY", "locachy-0", 1);
    if (strcmp(action, "open") == 0) {
        if (app->own_tor) {
            execl(APP_TOOL, "locachy-app", action, "--own-tor", app->name, app->target, (char *)NULL);
        } else {
            execl(APP_TOOL, "locachy-app", action, app->name, app->target, (char *)NULL);
        }
    } else if (strcmp(action, "start") == 0 && app->own_tor) {
        execl(APP_TOOL, "locachy-app", action, "--own-tor", app->name, (char *)NULL);
    } else {
        execl(APP_TOOL, "locachy-app", action, app->name, (char *)NULL);
    }
    dprintf(2, "apps: exec %s: %s\n", APP_TOOL, strerror(errno));
    _exit(127);
}

static int show_window(const struct app *app, char *why, size_t size)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = APP_WINDOW_CONTROL };
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address) < 0) {
        if (fd >= 0) {
            close(fd);
        }
        snprintf(why, size, "the shared display's window control (labwc-control) is not running");
        return -1;
    }
    char message[128];
    /* A native window's app id is its name; it has no tray to ask instead. */
    int n = snprintf(message, sizeof message, "%s %s", app->native ? "raise" : "show", app->name);
    if (send(fd, message, (size_t)n, MSG_NOSIGNAL) != n) {
        close(fd);
        snprintf(why, size, "cannot send window request");
        return -1;
    }
    struct pollfd wait = { .fd = fd, .events = POLLIN };
    int r = poll(&wait, 1, 150);
    ssize_t got = r > 0 ? recv(fd, message, sizeof message - 1, MSG_DONTWAIT) : -1;
    close(fd);
    if (got <= 0) {
        snprintf(why, size, "window control timed out");
        return -1;
    }
    message[got] = '\0';
    snprintf(why, size, "%s", message);
    return strncmp(message, "ok ", 3) == 0 ? 0 : -1;
}

/* A job of a native window, outside its cgroup and outside process 1: its
 * freeze through the clipboard service (as locachy-app freezes a container:
 * the compositor keeps the cgroup gated until it thaws), or its stop: thawed,
 * the clipboard saved, asked to end, then everything of it ended. */
static pid_t native_job(struct app *app, const char *action)
{
    char log_path[192], cg[160], path[200];
    snprintf(log_path, sizeof log_path, APP_RUN "/%s.log", app->name);
    snprintf(cg, sizeof cg, APP_CGROUP_ROOT "/app_%.63s", app->name);
    pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    setsid();
    int in = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int out = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (in >= 0) dup2(in, STDIN_FILENO);
    if (out >= 0) {
        dup2(out, STDOUT_FILENO);
        dup2(out, STDERR_FILENO);
    }
    setenv("XDG_RUNTIME_DIR", APP_WAYLAND_RUNTIME, 1); /* the clipboard's guard socket */
    if (strcmp(action, "freeze") == 0) {
        if (access(APP_CLIPBOARD, X_OK) != 0) {
            dprintf(2, "apps: %s: the clipboard service is not installed; not frozen\n", app->name);
            _exit(1);
        }
        execl(APP_CLIPBOARD, "clipboard", "freeze", cg, (char *)NULL);
        _exit(127);
    }
    snprintf(path, sizeof path, "%s/cgroup.freeze", cg);
    write_value(path, "0");
    if (access(APP_CLIPBOARD, X_OK) == 0) {
        pid_t sync = fork();
        if (sync == 0) {
            execl(APP_CLIPBOARD, "clipboard", "sync", (char *)NULL);
            _exit(127);
        }
        int status = 0;
        if (sync < 0 || waitpid(sync, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            dprintf(2, "apps: %s: the clipboard was not saved before stopping\n", app->name);
        }
    }
    /* Its own processes only: those the kernel lists in its cgroup. */
    snprintf(path, sizeof path, "%s/cgroup.procs", cg);
    FILE *procs = fopen(path, "re");
    long member;
    while (procs && fscanf(procs, "%ld", &member) == 1) {
        if (member > 1) kill((pid_t)member, SIGTERM);
    }
    if (procs) fclose(procs);
    for (int i = 0; i < 300 && populated(app); i++) {
        usleep(10000);
    }
    snprintf(path, sizeof path, "%s/cgroup.kill", cg);
    write_value(path, "1");
    for (int i = 0; i < 100 && populated(app); i++) {
        usleep(10000);
    }
    _exit(populated(app) ? 1 : 0);
}

static int native_action(struct app *app, const char *action, char *why, size_t size)
{
    char path[192];
    int exists = populated(app) || app->launcher;
    if (strcmp(action, "open") == 0 || strcmp(action, "show") == 0 || strcmp(action, "resume") == 0) {
        if (exists) {
            if (frozen(app)) {
                snprintf(path, sizeof path, APP_CGROUP_ROOT "/app_%.63s/cgroup.freeze", app->name);
                write_value(path, "0");
                for (int i = 0; i < 200 && frozen(app); i++) {
                    usleep(1000);
                }
            }
            if (strcmp(action, "resume") == 0) {
                snprintf(why, size, "resume accepted");
                return 0;
            }
            if (show_window(app, why, size) != 0) {
                char reason[160];
                snprintf(reason, sizeof reason, "%s", strncmp(why, "error ", 6) == 0 ? why + 6 : why);
                snprintf(why, size, "running; its window was not raised: %s", reason);
            }
            return 0;
        }
        if (strcmp(action, "open") != 0) {
            snprintf(why, size, "window is closed");
            return -1;
        }
        action = "start";
    }
    if (strcmp(action, "start") == 0) {
        if (app->want_start || app->job || app->pending[0] || exists) {
            snprintf(why, size, exists ? "already running" : "window operation still in progress");
            return exists ? 0 : -1;
        }
        app->error[0] = '\0';
        snprintf(app->pending, sizeof app->pending, "starting");
        if (!display_ready()) {
            app->want_start = 1;
            app->deadline = uptime() + 25;
            snprintf(why, size, "start accepted");
            return 0;
        }
        pid_t pid = launch_native(app);
        if (pid < 0) {
            app->pending[0] = '\0';
            snprintf(why, size, "cannot start the window's program: %s", strerror(errno));
            return -1;
        }
        app->launcher = pid;
        app->deadline = uptime() + 20;
        snprintf(why, size, "start accepted");
        return 0;
    }
    if (strcmp(action, "freeze") != 0 && strcmp(action, "stop") != 0) {
        snprintf(why, size, "unknown window action");
        return -1;
    }
    int stopping = strcmp(action, "stop") == 0;
    if (stopping && (app->want_start || strcmp(app->pending, "starting") == 0)) {
        app->want_start = 0; /* a start that has not shown its window yet */
        app->pending[0] = '\0';
        app->deadline = 0;
    } else if (app->job || app->pending[0]) {
        snprintf(why, size, strcmp(app->pending, "stopping") == 0 && stopping ?
                 "stop already in progress" : "window operation still in progress");
        return strcmp(app->pending, "stopping") == 0 && stopping ? 0 : -1;
    }
    if (!exists) {
        app->error[0] = '\0'; /* stopping a failed one: home forgets it */
        snprintf(why, size, "already closed");
        return 0;
    }
    if (!stopping && frozen(app)) {
        snprintf(why, size, "already frozen");
        return 0;
    }
    pid_t pid = native_job(app, action);
    if (pid < 0) {
        snprintf(why, size, "cannot start window operation: %s", strerror(errno));
        return -1;
    }
    app->job = pid;
    app->error[0] = '\0';
    snprintf(app->pending, sizeof app->pending, "%s", stopping ? "stopping" : "freezing");
    app->deadline = uptime() + 12;
    snprintf(why, size, "%s accepted", action);
    return 0;
}

static int native_valid_target(const struct native_program *p, const char *target, char *why, size_t size)
{
    size_t len = strlen(target);
    struct stat st;
    for (const char *c = target; *c; c++) {
        if ((unsigned char)*c < 32 || *c == 127) {
            snprintf(why, size, "invalid target");
            return 0;
        }
    }
    if (len >= sizeof apps[0].target) {
        snprintf(why, size, "target too long");
        return 0;
    }
    if (!len) {
        return 1;
    }
    switch (p->target) {
    case TARGET_PAGE:
        for (size_t i = 0; i < sizeof settings_pages / sizeof *settings_pages; i++) {
            if (strcmp(target, settings_pages[i]) == 0) return 1;
        }
        snprintf(why, size, "unknown settings page");
        return 0;
    case TARGET_FILE:
    case TARGET_FOLDER:
        if (target[0] != '/' || stat(target, &st) != 0 ||
            (p->target == TARGET_FOLDER ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) {
            snprintf(why, size, "%s cannot open %.200s", p->program, target);
            return 0;
        }
        return 1;
    default:
        snprintf(why, size, "%s takes no target", p->program);
        return 0;
    }
}

/* A host program as a window of its own on the shared display (the top
 * bar's status areas, a file opened on the shared display). Only the fixed
 * programs of natives, with a target checked here; a single-window program
 * already open is raised (settings: told its new page). */
int apps_window_open(const char *program, const char *target, char *why, size_t size)
{
    const struct native_program *p = NULL;
    for (size_t i = 0; i < sizeof natives / sizeof *natives; i++) {
        if (strcmp(program, natives[i].program) == 0) p = &natives[i];
    }
    if (!p) {
        snprintf(why, size, "no such window program");
        return -1;
    }
    if (!native_valid_target(p, target, why, size)) {
        return -1;
    }
    discover();
    int windows = 0;
    struct app *app = NULL;
    for (int i = 0; i < count; i++) {
        if (apps[i].native == p && (populated(&apps[i]) || apps[i].launcher || apps[i].want_start ||
                                    strcmp(apps[i].pending, "starting") == 0)) {
            app = &apps[i];
        }
        windows += apps[i].native != NULL;
    }
    if (app && p->single) {
        if (target[0] && p->target == TARGET_PAGE) {
            if (app->launcher == 0 || app->request_fd < 0) {
                snprintf(why, size, "%s is still starting", p->program);
                return -1;
            }
            char line[64];
            int n = snprintf(line, sizeof line, "page %s\n", target);
            if (write(app->request_fd, line, (size_t)n) != n) {
                snprintf(why, size, "%s does not take requests now: %s", p->program, strerror(errno));
                return -1;
            }
        }
        return apps_action("open", app->name, why, size);
    }
    if (windows >= MAX_NATIVE || count == MAX_APPS) {
        snprintf(why, size, "too many windows are open");
        return -1;
    }
    /* A name no container and no window has: its log, cgroup and app id. */
    char name[64], path[256];
    struct stat st;
    for (int n = 1;; n++) {
        if (n > 999) {
            snprintf(why, size, "no free window name");
            return -1;
        }
        snprintf(name, sizeof name, "window-%s-%d", p->program, n);
        snprintf(path, sizeof path, "%s/%s.conf", APP_CONF, name);
        if (lookup(name) || stat(path, &st) == 0) continue;
        struct app probe = { 0 };
        snprintf(probe.name, sizeof probe.name, "%s", name);
        if (!populated(&probe)) break;
    }
    app = &apps[count++];
    memset(app, 0, sizeof *app);
    snprintf(app->name, sizeof app->name, "%s", name);
    snprintf(app->target, sizeof app->target, "%s", target);
    app->native = p;
    app->ready_fd = app->request_fd = -1;
    /* Not through apps_action: its look would forget an entry not started. */
    if (native_action(app, "start", why, size) != 0) {
        return -1; /* forgotten at the next look: nothing of it runs */
    }
    snprintf(why, size, "%s window %s accepted", p->program, app->name);
    return 0;
}

int apps_action(const char *action, const char *name, char *why, size_t size)
{
    int own_tor = 0;
    if (strcmp(action, "open-own-tor") == 0) {
        action = "open";
        own_tor = 1;
    } else if (strcmp(action, "start-own-tor") == 0) {
        action = "start";
        own_tor = 1;
    }
    discover();
    struct app *app = valid_name(name) ? lookup(name) : NULL;
    if (!app) {
        snprintf(why, size, "unknown container");
        return -1;
    }
    if (app->native) {
        if (own_tor) {
            snprintf(why, size, "unknown window action");
            return -1;
        }
        return native_action(app, action, why, size);
    }
    if (strcmp(action, "open") == 0) {
        if (populated(app)) {
            if (frozen(app)) {
                char fpath[192];
                snprintf(fpath, sizeof fpath, APP_CGROUP_ROOT "/app_%.63s/cgroup.freeze", app->name);
                int ffd = open(fpath, O_WRONLY | O_CLOEXEC);
                if (ffd >= 0) {
                    (void)write(ffd, "0\n", 2);
                    close(ffd);
                }
                for (int i = 0; i < 200 && frozen(app); i++) {
                    usleep(1000);
                }
            }
            /* The display comes to the front either way; say what happened. */
            if (show_window(app, why, size) != 0) {
                char reason[160];
                snprintf(reason, sizeof reason, "%s", strncmp(why, "error ", 6) == 0 ? why + 6 : why);
                snprintf(why, size, "running; its window was not raised: %s", reason);
            }
            return 0;
        }
        if (strcmp(app->pending, "starting") == 0) {
            snprintf(why, size, "open accepted");
            return 0;
        }
        action = "start";
    }
    if (strcmp(action, "show") == 0) {
        if (!populated(app)) {
            snprintf(why, size, "container is closed");
            return -1;
        }
        if (frozen(app)) {
            char fpath[192];
            snprintf(fpath, sizeof fpath, APP_CGROUP_ROOT "/app_%.63s/cgroup.freeze", app->name);
            int ffd = open(fpath, O_WRONLY | O_CLOEXEC);
            if (ffd >= 0) {
                (void)write(ffd, "0\n", 2);
                close(ffd);
            }
            for (int i = 0; i < 200 && frozen(app); i++) {
                usleep(1000);
            }
        }
        return show_window(app, why, size);
    }
    if (strcmp(action, "start") != 0 && strcmp(action, "resume") != 0 &&
        strcmp(action, "freeze") != 0 && strcmp(action, "stop") != 0) {
        snprintf(why, size, "unknown container action");
        return -1;
    }
    if (strcmp(action, "stop") == 0 && (app->want_start || app->job || app->pending[0])) {
        if (strcmp(app->pending, "stopping") == 0) {
            snprintf(why, size, "stop already in progress");
            return 0;
        }
        if (app->job > 0) {
            kill(app->job, SIGTERM);
            app->job = 0;
        }
        app->want_start = app->own_tor = 0;
        app->target[0] = '\0';
        app->pending[0] = '\0';
        app->deadline = 0;
    } else if (app->want_start || app->job || app->pending[0]) {
        snprintf(why, size, "container operation still in progress");
        return -1;
    }
    int exists = populated(app);
    if (strcmp(action, "start") == 0 && exists) {
        action = "resume";
        own_tor = 0;
    }
    if (strcmp(action, "resume") == 0 && !exists) {
        snprintf(why, size, "container is not running");
        return -1;
    }
    if ((strcmp(action, "stop") == 0 || strcmp(action, "freeze") == 0) && !exists) {
        if (strcmp(action, "stop") == 0) {
            app->error[0] = '\0';
        }
        snprintf(why, size, "already closed");
        return 0;
    }
    char marker[192];
    if (strcmp(action, "start") == 0) {
        app->own_tor = own_tor;
        snprintf(marker, sizeof marker, APP_RUN "/%s.ready", app->name);
        unlink(marker);
        snprintf(marker, sizeof marker, APP_RUN "/%s.log", app->name);
        int fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            close(fd);
        }
        if (!display_ready()) {
            app->want_start = 1;
            app->error[0] = '\0';
            snprintf(app->pending, sizeof app->pending, "starting");
            app->deadline = uptime() + 25;
            snprintf(why, size, "start accepted");
            return 0;
        }
    }
    int starting = strcmp(action, "start") == 0;
    /* The script holds its start lock before entering the application cgroup. */
    pid_t pid = launch(app, action, 0);
    app->own_tor = 0;
    if (pid < 0) {
        snprintf(why, size, "cannot start container operation: %s", strerror(errno));
        return -1;
    }
    if (starting) {
        app->launcher = pid;
    } else {
        app->job = pid;
    }
    app->error[0] = '\0';
    snprintf(app->pending, sizeof app->pending, "%s", starting ? "starting" :
             strcmp(action, "stop") == 0 ? "stopping" :
             strcmp(action, "freeze") == 0 ? "freezing" : "starting");
    /* Clipboard sync/check (2.5 s each), freezer transition and cancellation
     * need their own bounded time before the asynchronous helper is judged. */
    app->deadline = uptime() + (starting ? 20 :
        (strcmp(action, "freeze") == 0 || strcmp(action, "stop") == 0) ? 12 : 8);
    snprintf(why, size, "%s accepted", action);
    return 0;
}

/* Open target (a path or URL) in container name (locachy-app open): in it
 * if it runs, else by starting it with target once the display is up. */
int apps_open_mode(const char *name, const char *target, int own_tor, char *why, size_t size)
{
    discover();
    struct app *app = valid_name(name) ? lookup(name) : NULL;
    if (!app || app->native) { /* a window opens files through window PROGRAM TARGET */
        snprintf(why, size, "unknown container");
        return -1;
    }
    if (!target[0] || strlen(target) >= sizeof app->target || strchr(target, '\n')) {
        snprintf(why, size, "cannot open that");
        return -1;
    }
    if (app->want_start || app->job || app->pending[0]) {
        snprintf(why, size, "container operation still in progress");
        return -1;
    }
    snprintf(app->target, sizeof app->target, "%s", target);
    app->error[0] = '\0';
    if (populated(app)) {
        app->own_tor = 0;
        pid_t pid = launch(app, "open", 0); /* quick: it hands target on and ends */
        app->target[0] = '\0';
        if (pid < 0) {
            snprintf(why, size, "cannot start container operation: %s", strerror(errno));
            return -1;
        }
        app->job = pid;
        snprintf(app->pending, sizeof app->pending, "opening");
        app->deadline = uptime() + 8;
        if (show_window(app, why, size) != 0) {
            snprintf(why, size, "opening in %s", app->name);
        }
        return 0;
    }
    app->own_tor = own_tor;
    char marker[192];
    snprintf(marker, sizeof marker, APP_RUN "/%s.ready", app->name);
    unlink(marker);
    snprintf(app->pending, sizeof app->pending, "starting");
    if (!display_ready()) {
        app->want_start = 1; /* apps_tick launches it, with target */
        app->deadline = uptime() + 25;
        snprintf(why, size, "start accepted");
        return 0;
    }
    pid_t pid = launch(app, "open", 0);
    app->target[0] = '\0';
    app->own_tor = 0;
    if (pid < 0) {
        app->pending[0] = '\0';
        snprintf(why, size, "cannot start container operation: %s", strerror(errno));
        return -1;
    }
    app->launcher = pid;
    app->deadline = uptime() + 20;
    snprintf(why, size, "start accepted");
    return 0;
}

int apps_open(const char *name, const char *target, char *why, size_t size)
{
    return apps_open_mode(name, target, 0, why, size);
}

/* Why locachy-app ended: the last line of its log, when it is one of its own
 * ("locachy-app: REASON", as die writes it before exiting), into out; a line
 * the program wrote after it says nothing about the start. Only the end of
 * the log is read. Returns 1 if there was such a line. */
static int launcher_reason(const struct app *app, char *out, size_t size)
{
    static const char prefix[] = "locachy-app: ";
    char path[192], text[2048];
    snprintf(path, sizeof path, APP_RUN "/%s.log", app->name);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return 0;
    }
    struct stat st;
    off_t from = fstat(fd, &st) == 0 && st.st_size > (off_t)sizeof text - 1 ?
                 st.st_size - (off_t)(sizeof text - 1) : 0;
    ssize_t n = pread(fd, text, sizeof text - 1, from);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    text[n] = '\0';
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r')) {
        text[--n] = '\0';
    }
    char *line = strrchr(text, '\n');
    line = line ? line + 1 : (from == 0 ? text : NULL); /* a cut first line is no line */
    if (!line || strncmp(line, prefix, sizeof prefix - 1) != 0 || !line[sizeof prefix - 1]) {
        return 0;
    }
    snprintf(out, size, "%s", line + sizeof prefix - 1);
    for (char *p = out; *p; p++) { /* apps_list rows are tab separated lines */
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127) {
            *p = ' ';
        }
    }
    return 1;
}

int apps_reap(pid_t pid, int status)
{
    for (int i = 0; i < count; i++) {
        struct app *app = &apps[i];
        int launcher = pid == app->launcher;
        if (!launcher && pid != app->job) {
            continue;
        }
        if (launcher) {
            app->launcher = 0;
        } else {
            app->job = 0;
        }
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (app->native && launcher) {
            /* Its window closed, it ended, or it was stopped: what it left
             * ends with it, and it takes no more requests. */
            native_kill(app);
            if (app->request_fd >= 0) {
                close(app->request_fd);
                app->request_fd = -1;
            }
            if (strcmp(app->pending, "stopping") != 0) {
                if (code != 0 && !(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM)) {
                    char error[sizeof app->error];
                    snprintf(error, sizeof error, "%s exited with status %d; inspect " APP_RUN "/%.63s.log",
                             app->native->program, code, app->name);
                    failure(app, error);
                } else {
                    app->pending[0] = '\0'; /* closed, also one that showed nothing */
                    app->deadline = 0;
                }
            }
            apps_tick();
            return 1;
        }
        if (launcher && strcmp(app->pending, "stopping") == 0) {
            apps_tick();
            return 1;
        }
        if (code != 0) {
            char error[sizeof app->error];
            if (!launcher_reason(app, error, sizeof error)) {
                snprintf(error, sizeof error, "%s exited with status %d; inspect " APP_RUN "/%.63s.log",
                         launcher ? "application" : "container operation", code, app->name);
            }
            failure(app, error);
        } else if (!launcher) {
            if (strcmp(app->pending, "stopping") != 0) {
                app->pending[0] = '\0';
                app->deadline = 0;
            }
        } else if (strcmp(app->pending, "starting") != 0) {
            app->pending[0] = '\0';
            app->deadline = 0;
        }
        apps_tick();
        return 1;
    }
    return 0;
}
