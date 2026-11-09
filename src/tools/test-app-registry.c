/* Exercise host asynchronous lifecycle bookkeeping without booting a PID 1. */
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "apps.h"

static void tick_for(int milliseconds)
{
    for (int i = 0; i < milliseconds / 10; i++) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            assert(apps_reap(pid, status));
        }
        apps_tick();
        usleep(10000);
    }
}

static void contains(const char *needle)
{
    char rows[4096];
    apps_list(rows, sizeof rows);
    printf("%s", rows);
    assert(strstr(rows, needle) != NULL);
}

static void lacks(const char *needle)
{
    char rows[4096];
    apps_list(rows, sizeof rows);
    assert(strstr(rows, needle) == NULL);
}

/* What the test tool was given to open in alpha, one line each. */
static void opened(const char *expected)
{
    char text[512] = "";
    FILE *in = fopen(APP_RUN "/alpha.opened", "r");
    assert(in);
    size_t n = fread(text, 1, sizeof text - 1, in);
    text[n] = '\0';
    fclose(in);
    assert(strcmp(text, expected) == 0);
}

#define ROOT "/run/locachy-registry-test"

/* The whole of a small file of the test's, or "" when there is none. */
static const char *text_of(const char *path)
{
    static char text[2048];
    text[0] = '\0';
    FILE *in = fopen(path, "r");
    if (in) {
        size_t n = fread(text, 1, sizeof text - 1, in);
        text[n] = '\0';
        fclose(in);
    }
    return text;
}

static void put(const char *path, const char *text)
{
    FILE *out = fopen(path, "w");
    assert(out && fputs(text, out) >= 0 && fclose(out) == 0);
}

/* The program of a native window ends as when its window is closed. */
static void close_window(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, ROOT "/cgroup/app_%s/cgroup.procs", name);
    pid_t pid = (pid_t)atoi(text_of(path));
    assert(pid > 1 && kill(pid, SIGTERM) == 0);
}

/* Host programs as managed windows (apps_window_open): fixed programs and
 * checked targets, a cgroup each, the backend chosen explicitly, started when
 * the program has drawn its window, counted in the shared display's state,
 * one settings window told its pages, cleaned up when closed. */
static void native_windows(void)
{
    char why[256];
    assert(apps_display_state() == APPS_DISPLAY_STOPPED);
    assert(apps_window_open("edit", "", why, sizeof why) == -1);
    assert(apps_window_open("../settings", "", why, sizeof why) == -1);
    assert(apps_window_open("settings", "bogus", why, sizeof why) == -1);
    assert(apps_window_open("settings", "network\n", why, sizeof why) == -1);
    assert(apps_window_open("mixer", "/usr/bin/sh", why, sizeof why) == -1);
    assert(apps_window_open("files", ROOT "/doc.txt", why, sizeof why) == -1); /* a file is no folder */
    assert(apps_window_open("files", "runtime", why, sizeof why) == -1);
    assert(apps_window_open("view", "doc.txt", why, sizeof why) == -1);
    assert(apps_window_open("view", ROOT "/missing.txt", why, sizeof why) == -1);
    assert(apps_window_open("view", ROOT, why, sizeof why) == -1); /* a folder is no file */
    lacks("window-");
    puts("PASS window requests: only the fixed programs, settings pages, view files, files folders");

    setenv("LOCACHY_CONTROL", "3", 1); /* process 1's: never a window's */
    assert(apps_window_open("settings", "network", why, sizeof why) == 0);
    tick_for(300);
    contains("window-settings-1\trunning\t");
    const char *env = text_of(ROOT "/runtime/window-settings-1.env");
    assert(strstr(env, "program=settings\n") && strstr(env, "args=--page=network\n"));
    assert(strstr(env, "backend=wayland\n") && strstr(env, "display=" ROOT "/wayland-0\n"));
    assert(strstr(env, "title=Settings\n") && strstr(env, "fds=3 4\n") && strstr(env, "control=none\n"));
    assert(apps_display_state() == APPS_DISPLAY_ACTIVE); /* no container runs */
    puts("PASS a settings window: own cgroup, wayland backend, page argument, keeps the display");

    assert(apps_window_open("settings", "power", why, sizeof why) == 0);
    assert(apps_window_open("settings", "", why, sizeof why) == 0); /* raised only */
    tick_for(200);
    lacks("window-settings-2");
    assert(strcmp(text_of(ROOT "/runtime/window-settings-1.requests"), "page power\n") == 0);
    puts("PASS a second settings request goes to the open window, not a second copy");

    assert(apps_window_open("view", ROOT "/doc.txt", why, sizeof why) == 0);
    assert(apps_window_open("view", ROOT "/doc.txt", why, sizeof why) == 0);
    tick_for(300);
    contains("window-view-1\trunning\t");
    contains("window-view-2\trunning\t");
    assert(strstr(text_of(ROOT "/runtime/window-view-2.env"), "args=" ROOT "/doc.txt\n"));
    assert(apps_window_open("files", "", why, sizeof why) == 0);
    assert(apps_window_open("files", ROOT "/runtime", why, sizeof why) == 0);
    tick_for(300);
    contains("window-files-1\trunning\t");
    contains("window-files-2\trunning\t");
    assert(strstr(text_of(ROOT "/runtime/window-files-1.env"), "args=\n"));
    assert(strstr(text_of(ROOT "/runtime/window-files-2.env"), "args=" ROOT "/runtime\n"));
    puts("PASS view and files open a window each, files at a folder when given one");

    /* Closing view's window ends its program: what it left is ended
     * (cgroup.kill) and it is forgotten. */
    close_window("window-view-1");
    tick_for(300);
    lacks("window-view-1\t");
    assert(strcmp(text_of(ROOT "/cgroup/app_window-view-1/cgroup.kill"), "1") == 0);
    puts("PASS a closed window: its cgroup ended, the window forgotten");

    assert(apps_action("freeze", "window-settings-1", why, sizeof why) == 0);
    tick_for(300);
    contains("window-settings-1\tfrozen\t");
    assert(apps_action("resume", "window-settings-1", why, sizeof why) == 0);
    assert(strcmp(text_of(ROOT "/cgroup/app_window-settings-1/cgroup.freeze"), "0") == 0);
    put(ROOT "/cgroup/app_window-settings-1/cgroup.events", "populated 1\nfrozen 0\n"); /* the kernel thaws */
    contains("window-settings-1\trunning\t");
    puts("PASS freeze through the clipboard service, resume through the freezer");

    assert(apps_action("stop", "window-settings-1", why, sizeof why) == 0);
    assert(apps_action("stop", "window-view-2", why, sizeof why) == 0);
    assert(apps_action("stop", "window-files-1", why, sizeof why) == 0);
    assert(apps_action("stop", "window-files-2", why, sizeof why) == 0);
    tick_for(800);
    lacks("window-");
    assert(apps_display_state() == APPS_DISPLAY_STOPPED);
    assert(!apps_busy());
    puts("PASS stopped windows are forgotten; the shared display may stop");

    /* A program that ends before showing a window fails, with its status;
     * one that closes its readiness without a window fails too. */
    assert(apps_window_open("view", ROOT "/fail-doc", why, sizeof why) == 0);
    tick_for(300);
    contains("window-view-1\tfailed\t");
    contains("\tview exited with status 9; inspect " APP_RUN "/window-view-1.log\n");
    assert(apps_window_open("view", ROOT "/silent-doc", why, sizeof why) == 0);
    tick_for(300);
    contains("window-view-2\tfailed\t");
    contains("\tno window was shown; inspect " APP_RUN "/window-view-2.log\n");
    tick_for(1300); /* it ends by itself */
    contains("window-view-2\tfailed\t");
    assert(apps_action("stop", "window-view-1", why, sizeof why) == 0);
    assert(apps_action("stop", "window-view-2", why, sizeof why) == 0);
    lacks("window-");
    puts("PASS a window that never shows fails visibly; stopping it forgets it");

    /* A new display session's socket: the window waits for its display-ready
     * (the first window after boot), then starts; the mixer is fixed. */
    assert(unlink(APP_DISPLAY_READY) == 0);
    int fd = open(APP_DISPLAY_READY, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && close(fd) == 0);
    assert(apps_window_open("mixer", "", why, sizeof why) == 0);
    tick_for(200);
    contains("window-mixer-1\tstarting\t");
    assert(apps_display_state() == APPS_DISPLAY_ACTIVE); /* the display is started for it */
    apps_display_ready();
    tick_for(300);
    contains("window-mixer-1\trunning\t");
    env = text_of(ROOT "/runtime/window-mixer-1.env");
    assert(strstr(env, "program=term\n") && strstr(env, "args=/usr/bin/wiremix\n") && strstr(env, "title=Mixer\n"));
    assert(apps_window_open("mixer", "", why, sizeof why) == 0);
    lacks("window-mixer-2");
    assert(apps_window_open("bluetooth", "", why, sizeof why) == 0);
    assert(apps_window_open("bluetooth", "/usr/bin/sh", why, sizeof why) == -1);
    assert(apps_window_open("network", "", why, sizeof why) == 0);
    assert(apps_window_open("network", "eth0", why, sizeof why) == -1);
    assert(apps_window_open("term", "", why, sizeof why) == 0);
    assert(apps_window_open("term", "", why, sizeof why) == 0);
    assert(apps_window_open("term", "/bin/sh", why, sizeof why) == -1);
    tick_for(300);
    assert(apps_window_open("bluetooth", "", why, sizeof why) == 0);
    assert(apps_window_open("network", "", why, sizeof why) == 0);
    lacks("window-bluetooth-2");
    lacks("window-network-2");
    contains("window-bluetooth-1\trunning\t");
    contains("window-network-1\trunning\t");
    contains("window-term-1\trunning\t");
    contains("window-term-2\trunning\t");
    assert(strstr(text_of(ROOT "/runtime/window-bluetooth-1.env"), "args=/usr/bin/bluetui\n") &&
           strstr(text_of(ROOT "/runtime/window-bluetooth-1.env"), "title=Bluetooth\n"));
    assert(strstr(text_of(ROOT "/runtime/window-network-1.env"), "args=/usr/bin/nmtui\n") &&
           strstr(text_of(ROOT "/runtime/window-network-1.env"), "title=Network\n"));
    assert(strstr(text_of(ROOT "/runtime/window-term-1.env"), "args=/usr/local/lib/locachy/user-shell\n") &&
           strstr(text_of(ROOT "/runtime/window-term-1.env"), "title=Terminal\n"));
    assert(apps_action("stop", "window-mixer-1", why, sizeof why) == 0);
    assert(apps_action("stop", "window-bluetooth-1", why, sizeof why) == 0);
    assert(apps_action("stop", "window-network-1", why, sizeof why) == 0);
    assert(apps_action("stop", "window-term-1", why, sizeof why) == 0);
    assert(apps_action("stop", "window-term-2", why, sizeof why) == 0);
    tick_for(800);
    lacks("window-");
    puts("PASS the first window waits for display-ready; single mixer/bluetooth/network, multiple user-shell terms");

    /* A container may not be opened as a window, nor a window as one. */
    assert(apps_open("window-mixer-1", "/x", why, sizeof why) == -1);
    unsetenv("LOCACHY_CONTROL");
}

int main(void)
{
    char why[256];
    apps_init();
    contains("alpha\tclosed\t");
    assert(apps_action("start", "../invalid", why, sizeof why) == -1);
    assert(apps_action("start", "alpha", why, sizeof why) == 0);
    assert(apps_action("start", "alpha", why, sizeof why) == -1);
    /* The display's sockets are there, but it has not said display-ready
     * (X11 not yet published): the program waits for it. */
    tick_for(200);
    contains("alpha\tstarting\t");
    apps_display_ready();
    tick_for(200);
    contains("alpha\trunning\t");
    assert(apps_action("freeze", "alpha", why, sizeof why) == 0);
    tick_for(200);
    contains("alpha\tfrozen\t");
    assert(apps_action("resume", "alpha", why, sizeof why) == 0);
    tick_for(200);
    contains("alpha\trunning\t");
    assert(apps_action("stop", "alpha", why, sizeof why) == 0);
    tick_for(300);
    contains("alpha\tclosed\t");
    assert(!apps_busy());
    assert(apps_action("start", "broken", why, sizeof why) == 0);
    tick_for(300);
    contains("broken\tfailed\t");
    /* The last line is the program's own: the status, and where to look. */
    contains("\tapplication exited with status 23; inspect " APP_RUN "/broken.log\n");
    assert(apps_action("stop", "broken", why, sizeof why) == 0);
    contains("broken\tclosed\t");
    /* locachy-app refused the start (die): its reason is the failure, for
     * home to show. */
    assert(apps_action("start", "refused", why, sizeof why) == 0);
    tick_for(300);
    contains("refused\tfailed\t");
    contains("\trefused: it asks for X11 (x11=\"1\") and there is no shared X11 display: not started\n");
    puts("PASS a refused start: the reason locachy-app gave is the failure home shows");
    assert(apps_action("stop", "refused", why, sizeof why) == 0);
    contains("refused\tclosed\t");
    /* A container whose configuration has gone is forgotten (APP_CONF/gamma.conf). */
    contains("gamma\tclosed\t");
    assert(unlink(APP_CONF "/gamma.conf") == 0);
    lacks("gamma\t");
    contains("alpha\tclosed\t");
    /* Open: a closed container starts with the target, a running one is
     * handed it; nothing to open or an unknown container is refused. */
    assert(apps_open("alpha", "", why, sizeof why) == -1);
    assert(apps_open("nosuch", "/x", why, sizeof why) == -1);
    assert(apps_open("alpha", "/home/u/Documents/a.txt", why, sizeof why) == 0);
    tick_for(200);
    contains("alpha\trunning\t");
    opened("/home/u/Documents/a.txt\n");
    assert(apps_open("alpha", "https://example.org/", why, sizeof why) == 0);
    tick_for(200);
    contains("alpha\trunning\t");
    opened("/home/u/Documents/a.txt\nhttps://example.org/\n");
    assert(apps_action("stop", "alpha", why, sizeof why) == 0);
    tick_for(300);
    contains("alpha\tclosed\t");
    /* A later session's display (its socket made again) waits for its own
     * display-ready. */
    assert(unlink(APP_DISPLAY_READY) == 0);
    int fd = open(APP_DISPLAY_READY, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && close(fd) == 0);
    assert(apps_action("start", "alpha", why, sizeof why) == 0);
    tick_for(200);
    contains("alpha\tstarting\t");
    apps_display_ready();
    tick_for(200);
    contains("alpha\trunning\t");
    assert(apps_action("stop", "alpha", why, sizeof why) == 0);
    tick_for(300);
    contains("alpha\tclosed\t");
    /* Confirmation is one launch, including a deferred first display start;
     * targets and own-Tor consent never survive a cancelled operation. */
    assert(unlink(APP_DISPLAY_READY) == 0);
    fd = open(APP_DISPLAY_READY, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && close(fd) == 0);
    assert(apps_action("open-own-tor", "alpha", why, sizeof why) == 0);
    tick_for(100);
    contains("alpha\tstarting\t");
    apps_display_ready();
    tick_for(200);
    assert(strcmp(text_of(ROOT "/runtime/tool.argv"), "start\n--own-tor\nalpha\n") == 0);
    assert(apps_open_mode("alpha", "https://example.org/reopen", 1, why, sizeof why) == 0);
    tick_for(200);
    assert(strcmp(text_of(ROOT "/runtime/tool.argv"), "open\nalpha\nhttps://example.org/reopen\n") == 0);
    assert(apps_action("stop", "alpha", why, sizeof why) == 0);
    tick_for(300);
    assert(apps_action("start-own-tor", "alpha", why, sizeof why) == 0);
    tick_for(200);
    assert(strcmp(text_of(ROOT "/runtime/tool.argv"), "start\n--own-tor\nalpha\n") == 0);
    assert(apps_action("stop", "alpha", why, sizeof why) == 0);
    tick_for(300);
    assert(unlink(APP_DISPLAY_READY) == 0);
    fd = open(APP_DISPLAY_READY, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && close(fd) == 0);
    assert(apps_open_mode("alpha", "https://example.org/own", 1, why, sizeof why) == 0);
    apps_display_ready();
    tick_for(200);
    assert(strcmp(text_of(ROOT "/runtime/tool.argv"), "open\n--own-tor\nalpha\nhttps://example.org/own\n") == 0);
    assert(apps_action("stop", "alpha", why, sizeof why) == 0);
    tick_for(300);
    assert(unlink(APP_DISPLAY_READY) == 0);
    fd = open(APP_DISPLAY_READY, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && close(fd) == 0);
    assert(apps_open_mode("alpha", "https://example.org/cancelled", 1, why, sizeof why) == 0);
    assert(apps_action("stop", "alpha", why, sizeof why) == 0);
    assert(apps_action("start", "alpha", why, sizeof why) == 0);
    apps_display_ready();
    tick_for(200);
    assert(strcmp(text_of(ROOT "/runtime/tool.argv"), "start\nalpha\n") == 0);
    assert(apps_action("stop", "alpha", why, sizeof why) == 0);
    tick_for(300);
    puts("PASS own-Tor immediate/deferred starts, target opens, reopens and cancellation isolation");
    native_windows();
    puts("Host registry state transitions passed.");
    return 0;
}
