/* Seat release must never wait indefinitely for a gesture helper. The
 * worker variants below are real process groups; only the unreapable-child
 * case doubles waitpid/kill to model a kernel wait without stalling a test. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>

static pid_t release_waitpid(pid_t pid, int *status, int options);
static int release_kill(pid_t pid, int signal);
static pid_t release_getpgid(pid_t pid);
#define waitpid release_waitpid
#define kill release_kill
#define getpgid release_getpgid
#define main seat_main
#include "../progs/seat.c"
#undef main
#undef getpgid
#undef kill
#undef waitpid
#include <assert.h>
#include <time.h>

static int stuck, killed, waited, fanout;
void say(const char *fmt, ...) { (void)fmt; }
double uptime(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static pid_t release_waitpid(pid_t pid, int *status, int options)
{
    if (stuck) { assert(options == WNOHANG); waited++; return 0; }
    return waitpid(pid, status, options);
}
static int release_kill(pid_t pid, int signal)
{
    if (stuck) { assert(pid == -123456); killed += signal == SIGKILL; return 0; }
    return kill(pid, signal);
}
static pid_t release_getpgid(pid_t pid)
{
    return stuck ? pid : getpgid(pid);
}

static pid_t worker(int ignore, int children, int *child)
{
    int pair[2];
    assert(pipe(pair) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(pair[0]);
        assert(setsid() > 0);
        if (ignore) signal(SIGTERM, SIG_IGN);
        pid_t nested = children ? fork() : 0;
        if (children && nested == 0) {
            close(pair[1]);
            for (;;) pause();
        }
        assert(write(pair[1], &nested, sizeof nested) == sizeof nested);
        close(pair[1]);
        for (;;) pause();
    }
    close(pair[1]);
    pid_t nested;
    assert(read(pair[0], &nested, sizeof nested) == sizeof nested);
    close(pair[0]);
    *child = (int)nested;
    return pid;
}

int main(void)
{
    double t = uptime();
    assert(stop_group(-1) == 1 && uptime() - t < 0.01);
    puts("PASS no helper: seat release does not wait");
    for (int i = 0; i < 8; i++) {
        int child;
        pid_t pid = worker(0, 0, &child);
        t = uptime();
        int reaped = stop_group(pid);
        assert(uptime() - t < 0.2);
        if (!reaped) {
            for (int j = 0; j < 200 && waitpid(pid, NULL, WNOHANG) == 0; j++) usleep(1000);
        }
        assert(waitpid(pid, NULL, WNOHANG) < 0 && errno == ECHILD);
    }
    puts("PASS graceful helper groups stop and reap across repeated seat releases");
    int child;
    pid_t pid = worker(1, 0, &child);
    t = uptime();
    int reaped = stop_group(pid);
    assert(uptime() - t < 0.2);
    if (!reaped) assert(waitpid(pid, NULL, 0) == pid);
    puts("PASS a helper ignoring SIGTERM is killed within the bounded handover budget");

    /* Adopt the grandchild so the test can verify the whole killed group,
     * including after the leader was already reaped. */
    assert(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    pid = worker(1, 1, &child);
    assert(child > 0);
    t = uptime();
    reaped = stop_group(pid);
    assert(uptime() - t < 0.2);
    if (!reaped) assert(waitpid(pid, NULL, 0) == pid);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    fanout++;
    assert(kill(-pid, 0) < 0 && errno == ESRCH);
    puts("PASS helper descendants cannot survive their leader or retain input after release");

    stuck = 1; killed = waited = 0;
    t = uptime();
    assert(stop_group(123456) == 0 && uptime() - t < 0.2 && killed == 1 && waited > 0);
    gestures_pid = -1; gestures_stopping = 123456;
    gestures(1);
    assert(gestures_pid == -1 && gestures_stopping == 123456);
    stuck = 0;
    puts("PASS an unreapable helper never blocks release or starts another input-reader group");
    puts("ALL PASSED");
    return 0;
}
