/*
 * pam_locachy_any.so (/usr/lib/security): the lock screen's other ways in
 * (a fingerprint, a face) for a program that asks on a terminal, as sudo
 * does, beside its password: like pam-any, the PAM services named in its
 * arguments run side by side, each in a child process of its own
 * (progs/auth.c), while the password is read from the terminal; whichever
 * comes first wins:
 *   - one of the services succeeds: PAM_SUCCESS (the others are stopped);
 *   - a password is typed: it becomes PAM_AUTHTOK and the module returns
 *     PAM_IGNORE, so that the stack's own modules check it (pam_unix with
 *     try_first_pass, and pam_faillock counting a wrong one);
 *   - Enter alone: PAM_IGNORE, and the stack asks for the password itself.
 * The password is read here, not through the program's conversation, so
 * that nothing is left waiting on the terminal when another way wins (what
 * pam-any's and pam-fprint-grosshack's threads do), and the terminal's
 * echo is put back whatever happens. Without a terminal, or with no service
 * named, it does nothing (PAM_IGNORE). settings writes its line into
 * /etc/pam.d/sudo with the services switched on.
 *   auth  sufficient  pam_locachy_any.so locachy-lock-fingerprint [...]
 * Tests build it with AUTH_TEST: an argument confdir=DIR names the
 * directory of the services (pam_start_confdir).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <security/pam_modules.h>
#include <security/pam_ext.h>

#include "auth.h"

#ifdef AUTH_TEST
extern const char *auth_confdir;
#endif

#define MAX_WAYS 4

static volatile sig_atomic_t stopped;

static void on_stop(int sig)
{
    stopped = sig;
}

static void say_line(int tty, const char *text)
{
    dprintf(tty, "\r\n%s\r\n", text);
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)flags;
    const char *user = NULL;
    const char *services[MAX_WAYS];
    int n = 0;
    for (int i = 0; i < argc; i++) {
#ifdef AUTH_TEST
        if (strncmp(argv[i], "confdir=", 8) == 0) {
            auth_confdir = argv[i] + 8;
            continue;
        }
#endif
        if (n < MAX_WAYS) {
            services[n++] = argv[i];
        }
    }
    if (n == 0 || pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || user == NULL) {
        return PAM_IGNORE;
    }
    int tty = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    struct termios was;
    if (tty < 0) {
        return PAM_IGNORE;
    }
    if (tcgetattr(tty, &was) < 0) {
        close(tty);
        return PAM_IGNORE;
    }

    struct auth ways[MAX_WAYS];
    for (int i = 0; i < n; i++) {
        auth_start(&ways[i], services[i], user, AUTH_AUTHENTICATE); /* pid 0 if it could not */
    }
    struct sigaction stop = { .sa_handler = on_stop }, old_int, old_quit, old_tstp, old_pipe;
    struct sigaction ignore = { .sa_handler = SIG_IGN };
    stopped = 0;
    sigaction(SIGINT, &stop, &old_int);
    sigaction(SIGQUIT, &stop, &old_quit);
    sigaction(SIGTSTP, &stop, &old_tstp);
    sigaction(SIGPIPE, &ignore, &old_pipe); /* auth.c: an answer to a child that has ended */
    struct termios quiet = was;
    quiet.c_lflag &= (tcflag_t)~ECHO;
    tcsetattr(tty, TCSAFLUSH, &quiet);
    dprintf(tty, "Password for %s (or %s): ", user, n > 1 ? "another way" :
            strstr(services[0], "face") ? "your face" : "a fingerprint");

    int result = PAM_IGNORE;
    char typed[512];
    size_t len = 0;
    for (;;) {
        struct pollfd p[1 + MAX_WAYS];
        int of[1 + MAX_WAYS], k = 0;
        p[k] = (struct pollfd){ .fd = tty, .events = POLLIN };
        of[k++] = -1;
        for (int i = 0; i < n; i++) {
            if (ways[i].pid) {
                p[k] = (struct pollfd){ .fd = ways[i].from, .events = POLLIN };
                of[k++] = i;
            }
        }
        if (poll(p, (nfds_t)k, -1) < 0 && errno != EINTR) {
            break;
        }
        if (stopped) {
            result = PAM_AUTH_ERR;
            break;
        }
        int won = 0;
        for (int j = 1; j < k; j++) {
            struct auth *a = &ways[of[j]];
            char said[sizeof a->message];
            snprintf(said, sizeof said, "%s", a->message);
            if (!p[j].revents || !auth_read(a)) {
                continue;
            }
            if (a->done && a->status == 0) {
                won = 1;
                break;
            }
            if (a->message[0] && strcmp(a->message, said) != 0) {
                say_line(tty, a->message); /* what its module says, as it says it */
                dprintf(tty, "Password: ");
            }
            if (a->asking) {
                auth_answer(a, ""); /* not a way that asks: a password is read above */
            }
        }
        if (won) {
            result = PAM_SUCCESS;
            break;
        }
        if (p[0].revents & (POLLHUP | POLLERR)) {
            break;
        }
        if (p[0].revents & POLLIN) {
            /* Canonical mode: a whole line, or what is left of it. */
            ssize_t r = read(tty, typed + len, sizeof typed - 1 - len);
            if (r <= 0) {
                break;
            }
            len += (size_t)r;
            if (len == sizeof typed - 1 || typed[len - 1] == '\n') {
                while (len > 0 && (typed[len - 1] == '\n' || typed[len - 1] == '\r')) {
                    len--;
                }
                typed[len] = '\0';
                if (len > 0) {
                    pam_set_item(pamh, PAM_AUTHTOK, typed);
                }
                break;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        auth_stop(&ways[i]);
    }
    explicit_bzero(typed, sizeof typed);
    tcsetattr(tty, TCSAFLUSH, &was);
    dprintf(tty, "\r\n");
    close(tty);
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGQUIT, &old_quit, NULL);
    sigaction(SIGTSTP, &old_tstp, NULL);
    sigaction(SIGPIPE, &old_pipe, NULL);
    if (stopped) {
        raise(stopped); /* the program's own handling, now the terminal is as it was */
    }
    return result;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_IGNORE;
}
