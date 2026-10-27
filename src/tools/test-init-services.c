/* Process 1's rc.conf reload (SIGHUP): additions, removals and restarts, no
 * fixed service count, and a failed read or allocation leaves it unchanged. */
#include <stdlib.h>

static int allocations_left = -1;

static void *settings_realloc(void *p, size_t size)
{
    if (allocations_left == 0) {
        return NULL;
    }
    if (allocations_left > 0) {
        allocations_left--;
    }
    return realloc(p, size);
}

#define realloc settings_realloc
#define main init_main
#include "../init/init.c"
#undef main
#undef realloc
#include <assert.h>

static const char *const *conf;
static int read_error;

int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    for (int i = 0; conf[i]; i += 2) {
        fn(conf[i], conf[i + 1], arg);
    }
    return read_error ? -1 : 0;
}

static struct service *find(const char *command)
{
    for (size_t i = 0; i < nservices; i++) {
        if (strcmp(services[i].command, command) == 0) {
            return &services[i];
        }
    }
    return NULL;
}

/* What the main loop does with services that are not running. */
static void start_waiting(void)
{
    for (size_t i = 0; i < nservices; i++) {
        if (services[i].pid == 0 && services[i].restart_at <= now_ms()) {
            service_start(&services[i]);
        }
    }
}

static int alive(pid_t pid)
{
    return waitpid(pid, NULL, WNOHANG) == 0;
}

int main(void)
{
    /* As process 1: these blocked, read from a signalfd; services unblock them. */
    sigemptyset(&handled);
    sigaddset(&handled, SIGCHLD);
    sigaddset(&handled, SIGHUP);
    sigaddset(&handled, SIGTERM);
    sigprocmask(SIG_BLOCK, &handled, NULL);
    static const char *const a[] = {
        "hibernate_after", "30",
        "service_one", "sleep 101",
        "service_two", "sleep 102; true", /* sh stays: the session holds two processes */
        "key_home", "Super+Esc",          /* someone else's: no complaint, no effect */
        NULL,
    };
    conf = a;
    read_settings();
    start_waiting();
    assert(nservices == 2 && hibernate_after == 30);
    pid_t one = find("sleep 101")->pid, two = find("sleep 102; true")->pid;
    assert(one > 0 && two > 0 && alive(one) && alive(two));

    static const char *const b[] = {
        "hibernate_after", "45",
        "service_two", "sleep 102; true",
        "service_three", "sleep 103",
        NULL,
    };
    conf = b;
    read_settings();
    usleep(200000);
    assert(hibernate_after == 45);
    assert(find("sleep 101") == NULL && !alive(one));         /* ended and forgotten */
    assert(find("sleep 102; true")->pid == two && alive(two)); /* untouched */
    assert(find("sleep 103") && find("sleep 103")->pid == 0);  /* waiting to start */
    start_waiting();
    pid_t three = find("sleep 103")->pid;
    assert(three > 0 && alive(three));

    static const char *const c[] = { NULL };
    conf = c;
    read_settings();
    usleep(200000);
    assert(nservices == 0 && !alive(two) && !alive(three));
    /* sleep 102 ran under sh -c in its own session: it ended with it. */
    assert(system("pgrep -x -f 'sleep 102' >/dev/null") != 0);
    /* Cross the old 8-service limit and the proposed 32-service patch. The
     * list grows with the configuration, not with another constant. */
    enum { MANY = 65 };
    char keys[MANY][32], commands[MANY][64];
    const char *many[MANY * 2 + 1];
    for (int i = 0; i < MANY; i++) {
        snprintf(keys[i], sizeof keys[i], "service_test_%d", i);
        snprintf(commands[i], sizeof commands[i], "/bin/true %d", i);
        many[i * 2] = keys[i];
        many[i * 2 + 1] = commands[i];
    }
    many[MANY * 2] = NULL;
    conf = many;
    read_settings();
    assert(nservices == MANY);
    for (int i = 0; i < MANY; i++) {
        assert(find(commands[i]) && find(commands[i])->pid == 0);
    }
    /* Reap and restart a service at the end of the grown list. */
    struct service *last = find(commands[MANY - 1]);
    service_start(last);
    pid_t first_pid = last->pid;
    for (int i = 0; last->pid != 0 && i < 100; i++) {
        usleep(10000);
        reap();
    }
    assert(first_pid > 0 && last->pid == 0 && last->restart_at > now_ms());
    last->restart_at = 0;
    start_waiting();
    for (int i = 0; last->pid != 0 && i < 100; i++) {
        usleep(10000);
        reap();
    }
    assert(last->pid == 0 && last->restart_at > now_ms());
    struct service *old = services;
    conf = a;
    read_error = 1;
    read_settings();
    assert(services == old && nservices == MANY && hibernate_after == 45);
    read_error = 0;
    allocations_left = 1; /* fail after part of the new list was read */
    read_settings();
    assert(services == old && nservices == MANY && hibernate_after == 45);
    allocations_left = -1;
    /* Shrink, preserve state on a reload, and ignore duplicate commands. */
    conf = b;
    read_settings();
    assert(nservices == 2);
    find("sleep 102; true")->restart_at = 12345;
    read_settings();
    assert(nservices == 2 && find("sleep 102; true")->restart_at == 12345);
    static const char *const duplicate[] = {
        "service_one", "sleep 102; true", "service_two", "sleep 102; true", NULL,
    };
    conf = duplicate;
    read_settings();
    assert(nservices == 1 && find("sleep 102; true")->restart_at == 12345);
    conf = c;
    read_settings();
    assert(nservices == 0 && services == NULL);
    puts("init: service reload, growth, restart and failed reload passed.");
    return 0;
}
