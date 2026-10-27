/* PID-namespace init for one LOCachy application. No host-side daemon. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const int watched[] = {
    SIGCHLD, SIGALRM, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1, SIGUSR2,
    SIGWINCH, SIGCONT, SIGTSTP, SIGTTIN, SIGTTOU
};

static void signal_handler(int sig)
{
    (void)sig;
}

static int exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 125;
}

/* Only called as namespace PID 1: includes adopted or setsid descendants,
 * excludes ourselves, and cannot address processes outside this namespace. */
static void forward(int sig)
{
    if (kill(-1, sig) < 0 && errno != ESRCH)
        perror("locachy-app-init: forward signal");
}

int main(int argc, char **argv)
{
    sigset_t signals;
    struct sigaction action = {0};
    pid_t child;
    int result = 125, main_exited = 0, cleaning = 0, ready_fd = -1;

    const char *ready = getenv("LOCACHY_APP_READY_FD");
    if (ready) {
        char *end;
        errno = 0;
        long fd = strtol(ready, &end, 10);
        if (errno || !*ready || *end || fd < 3 || fd > INT_MAX) {
            fprintf(stderr, "locachy-app-init: invalid readiness descriptor\n");
            return 125;
        }
        ready_fd = (int)fd;
        unsetenv("LOCACHY_APP_READY_FD");
    }

    if (argc < 2) {
        fprintf(stderr, "usage: locachy-app-init COMMAND [ARG...]\n");
        return 2;
    }
    if (getpid() != 1) {
        fprintf(stderr, "locachy-app-init: must run as PID 1\n");
        return 125;
    }

    sigemptyset(&signals);
    sigemptyset(&action.sa_mask);
    action.sa_handler = signal_handler;
    for (size_t i = 0; i < sizeof(watched) / sizeof(watched[0]); ++i) {
        sigaddset(&signals, watched[i]);
        /* PID-namespace init must explicitly accept external signals, even
         * though sigwait consumes them while they are blocked. */
        if (sigaction(watched[i], &action, NULL) < 0) {
            perror("locachy-app-init: sigaction");
            return 125;
        }
    }
    if (sigprocmask(SIG_BLOCK, &signals, NULL) < 0) {
        perror("locachy-app-init: sigprocmask");
        return 125;
    }

    int exec_status[2];
    if (pipe2(exec_status, O_CLOEXEC) < 0) {
        perror("locachy-app-init: exec status pipe");
        return 125;
    }
    child = fork();
    if (child < 0) {
        perror("locachy-app-init: fork");
        return 125;
    }
    if (child == 0) {
        close(exec_status[0]);
        if (ready_fd >= 0)
            close(ready_fd);
        sigset_t empty;
        struct sigaction reset = {0};
        reset.sa_handler = SIG_DFL;
        sigemptyset(&reset.sa_mask);
        for (int sig = 1; sig < NSIG; ++sig)
            if (sig != SIGKILL && sig != SIGSTOP)
                (void)sigaction(sig, &reset, NULL);
        sigemptyset(&empty);
        if (sigprocmask(SIG_SETMASK, &empty, NULL) < 0) {
            perror("locachy-app-init: child sigprocmask");
            _exit(125);
        }
        const char *uid_str = getenv("LOCACHY_APP_UID");
        if (uid_str && *uid_str) {
            const char *gid_str = getenv("LOCACHY_APP_GID");
            uid_t uid = (uid_t)strtoul(uid_str, NULL, 10);
            gid_t gid = gid_str && *gid_str ? (gid_t)strtoul(gid_str, NULL, 10) : (gid_t)uid;
            unsetenv("LOCACHY_APP_UID");
            unsetenv("LOCACHY_APP_GID");
            if (setgroups(0, NULL) < 0 || setgid(gid) < 0 || setuid(uid) < 0) {
                int error = errno;
                (void)write(exec_status[1], &error, sizeof error);
                perror("locachy-app-init: drop privileges");
                _exit(126);
            }
        }
        /* A container's programs (locachy-app): no setuid or file capability
         * gives them anything (a host program's terminal keeps sudo). */
        const char *nnp = getenv("LOCACHY_APP_NO_NEW_PRIVS");
        if (nnp && strcmp(nnp, "1") == 0 && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
            int error = errno;
            (void)write(exec_status[1], &error, sizeof error);
            perror("locachy-app-init: no_new_privs");
            _exit(126);
        }
        unsetenv("LOCACHY_APP_NO_NEW_PRIVS");
        execvp(argv[1], &argv[1]);
        int error = errno;
        (void)write(exec_status[1], &error, sizeof error);
        fprintf(stderr, "locachy-app-init: cannot execute %s: ", argv[1]);
        errno = error;
        perror(NULL);
        _exit(error == ENOENT ? 127 : 126);
    }

    close(exec_status[1]);
    int exec_error = 0;
    ssize_t exec_result;
    do {
        exec_result = read(exec_status[0], &exec_error, sizeof exec_error);
    } while (exec_result < 0 && errno == EINTR);
    close(exec_status[0]);
    if (exec_result != 0) {
        int status;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        if (ready_fd >= 0) {
            close(ready_fd);
        }
        return exec_result > 0 && exec_error == ENOENT ? 127 : 126;
    }

    /* Release the launch lock only after the application has passed exec. */
    if (ready_fd >= 0) {
        ssize_t written;
        do {
            written = write(ready_fd, "ready\n", 6);
        } while (written < 0 && errno == EINTR);
        close(ready_fd);
        if (written != 6) {
            perror("locachy-app-init: signal readiness");
            forward(SIGKILL);
            return 125;
        }
    }

    for (;;) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (pid == child) {
                result = exit_code(status);
                main_exited = 1;
                cleaning = 1;
                forward(SIGTERM);
                /* Do not let a surviving helper keep the container alive
                 * after its main application has exited. */
                alarm(1);
            }
        }
        if (pid < 0) {
            if (errno == ECHILD)
                break;
            if (errno == EINTR)
                continue;
            perror("locachy-app-init: waitpid");
            forward(SIGKILL);
            return 125;
        }

        int sig, error = sigwait(&signals, &sig);
        if (error) {
            errno = error;
            perror("locachy-app-init: sigwait");
            forward(SIGKILL);
            return 125;
        }
        if (sig == SIGCHLD)
            continue;
        if (sig == SIGALRM) {
            if (cleaning)
                forward(SIGKILL);
            continue;
        }
        /* TERM reaches the main app and all descendants, including orphans
         * and processes in other sessions. Host cgroup.kill is the bounded
         * fallback if the main app itself refuses to exit. */
        forward(sig);
    }
    alarm(0);
    return main_exited ? result : 125;
}
