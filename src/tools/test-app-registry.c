/* Exercise host asynchronous lifecycle bookkeeping without booting a PID 1. */
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
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
    contains("\tapplication exited with status 23; inspect broken.log\n");
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
    puts("Host registry state transitions passed.");
    return 0;
}
