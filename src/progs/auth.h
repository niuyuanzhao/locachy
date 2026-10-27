/*
 * progs/auth.{c,h}: a PAM transaction in a child process of its own (lock,
 * settings). The child runs the service's modules for the user (to
 * authenticate, then check the account; or to change the password) and
 * talks through a pipe in lines; the parent shows what the modules say and
 * answers what they ask. Several can run side by side (the password, a
 * fingerprint, a face): the first to succeed wins, and the others are
 * stopped. The caller ignores SIGPIPE: an answer may go to a child that has
 * just ended.
 */
#ifndef PROGS_AUTH_H
#define PROGS_AUTH_H

#include <sys/types.h>

enum { AUTH_AUTHENTICATE, AUTH_CHANGE };
enum { AUTH_IDLE, AUTH_ASKS_HIDDEN, AUTH_ASKS_SHOWN }; /* asking */

struct auth {
    pid_t pid;          /* 0: not running */
    int from, to;       /* its lines in; our answers out */
    char line[1024];    /* a line read in part */
    size_t len;
    int asking;         /* AUTH_ASKS_*: it waits for auth_answer */
    char prompt[256];   /* what it asks (its module's words) */
    char message[256];  /* what it last said (lines that came at once, joined);
                         * if it said nothing since it last asked, the
                         * word PAM failed with */
    int said;           /* it said something since it last asked */
    int passed;         /* it said it succeeded */
    int done, status;   /* it has ended, and how: 0 succeeded (it said so and exited 0) */
};

/* Start the service (an /etc/pam.d file) for user: AUTH_AUTHENTICATE or
 * AUTH_CHANGE (the password, as root: no old one asked). -1 if it cannot. */
int auth_start(struct auth *a, const char *service, const char *user, int what);
/* a->from is readable: read what came. 1 if something changed (it asks,
 * said something, or ended: done and status set, the process gone). */
int auth_read(struct auth *a);
/* Answer what it asks (a password: as typed). -1 if it does not ask. */
int auth_answer(struct auth *a, const char *text);
/* End it now, with its process group. */
void auth_stop(struct auth *a);

#endif
