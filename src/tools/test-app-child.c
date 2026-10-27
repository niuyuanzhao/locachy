/* Exercise reaper cleanup of orphaned, separate-session descendants. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        signal(SIGTERM, SIG_IGN);
        puts("stubborn child ready");
        fflush(stdout);
        for (;;) {
            pause();
        }
    }
    pid_t pid = fork();
    if (pid < 0) {
        return 125;
    }
    if (pid == 0) {
        setsid();
        signal(SIGTERM, SIG_IGN);
        for (;;) {
            pause();
        }
    }
    printf("orphan candidate %d\n", (int)pid);
    fflush(stdout);
    return 7;
}
