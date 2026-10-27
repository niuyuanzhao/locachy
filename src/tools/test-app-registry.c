/* Exercise host asynchronous lifecycle bookkeeping without booting a PID 1. */
#include <assert.h>
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
    assert(apps_action("stop", "broken", why, sizeof why) == 0);
    contains("broken\tclosed\t");
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
    puts("Host registry state transitions passed.");
    return 0;
}
