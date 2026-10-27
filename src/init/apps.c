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

#define MAX_APPS 32
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

struct app {
    char name[64];
    pid_t launcher, job;
    int want_start;
    char pending[16], error[160];
    char target[1024]; /* to open when it has started (apps_open) */
    double deadline;
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

/* The containers there are: APP_CONF/NAME.conf. One whose configuration has
 * gone is forgotten once nothing of it runs. */
static void discover(void)
{
    for (int i = 0; i < count;) {
        char path[256];
        struct stat st;
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
    FILE *in = fopen(path, "r");
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

static int display_ready(void)
{
    return access(APP_DISPLAY, F_OK) == 0 && access(APP_DISPLAY_READY, F_OK) == 0;
}

static void failure(struct app *app, const char *message)
{
    snprintf(app->error, sizeof app->error, "%s", message);
    app->want_start = 0;
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

void apps_tick(void)
{
    for (int i = 0; i < count; i++) {
        struct app *app = &apps[i];
        if (app->want_start && display_ready()) {
            app->want_start = 0;
            pid_t pid = launch(app, app->target[0] ? "open" : "start", 0);
            app->target[0] = '\0';
            if (pid < 0) {
                failure(app, "cannot start container operation");
                continue;
            }
            app->launcher = pid;
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
            char message[160];
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
        char row[384];
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
        execl(APP_TOOL, "locachy-app", action, app->name, app->target, (char *)NULL);
    }
    execl(APP_TOOL, "locachy-app", action, app->name, (char *)NULL);
    dprintf(2, "apps: exec %s: %s\n", APP_TOOL, strerror(errno));
    _exit(127);
}

static int show_window(const struct app *app, char *why, size_t size)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX,
        .sun_path = "/run/user/0/locachy-control.sock" };
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address) < 0) {
        if (fd >= 0) {
            close(fd);
        }
        snprintf(why, size, "the shared display's window control (labwc-control) is not running");
        return -1;
    }
    char message[128];
    int n = snprintf(message, sizeof message, "show %s", app->name);
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

int apps_action(const char *action, const char *name, char *why, size_t size)
{
    discover();
    struct app *app = valid_name(name) ? lookup(name) : NULL;
    if (!app) {
        snprintf(why, size, "unknown container");
        return -1;
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
        app->want_start = 0;
        app->pending[0] = '\0';
        app->deadline = 0;
    } else if (app->want_start || app->job || app->pending[0]) {
        snprintf(why, size, "container operation still in progress");
        return -1;
    }
    int exists = populated(app);
    if (strcmp(action, "start") == 0 && exists) {
        action = "resume";
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
int apps_open(const char *name, const char *target, char *why, size_t size)
{
    discover();
    struct app *app = valid_name(name) ? lookup(name) : NULL;
    if (!app) {
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
        if (launcher && strcmp(app->pending, "stopping") == 0) {
            apps_tick();
            return 1;
        }
        if (code != 0) {
            char error[160];
            snprintf(error, sizeof error, "%s exited with status %d; inspect %.63s.log",
                     launcher ? "application" : "container operation", code, app->name);
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
