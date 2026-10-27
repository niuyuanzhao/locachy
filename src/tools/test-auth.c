/* progs/auth.c, the PAM transactions of lock and settings, against services
 * of its own (pam_start_confdir) made of pam_exec scripts: a password
 * ("secret" passes), a fingerprint stand-in (says something, then succeeds or
 * fails after a while), a password change. Nothing of the system's PAM
 * configuration or its users' passwords is used.
 * test-auth SCRATCH_DIR (a new directory; as root, for pam_exec).
 * Build with -DAUTH_TEST, progs/auth.c and -lpam. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../progs/auth.h"

extern const char *auth_confdir;
static char dir[PATH_MAX], confdir[PATH_MAX + 8];

static const char *at(const char *where, const char *name)
{
    static char path[2 * PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", where, name);
    return path;
}

static void put(const char *path, const char *text, int mode)
{
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
    assert(chmod(path, (mode_t)mode) == 0);
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* Read from those running until one of them changes (or ms pass); 1 if one did. */
static int step(struct auth **a, int n, int ms)
{
    struct pollfd p[4];
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (a[i]->pid) {
            p[k++] = (struct pollfd){ .fd = a[i]->from, .events = POLLIN };
        }
    }
    if (k == 0 || poll(p, (nfds_t)k, ms) <= 0) {
        return 0;
    }
    int changed = 0;
    for (int i = 0; i < n; i++) {
        if (a[i]->pid && auth_read(a[i])) {
            changed = 1;
        }
    }
    return changed;
}

static void until_asked(struct auth *a)
{
    struct auth *one[] = { a };
    double t0 = now_ms();
    while (!a->asking && !a->done && now_ms() - t0 < 5000) {
        step(one, 1, 100);
    }
    if (a->asking != AUTH_ASKS_HIDDEN) {
        fprintf(stderr, "not asked: done %d, status %d, said \"%s\"\n", a->done, a->status, a->message);
    }
    assert(a->asking == AUTH_ASKS_HIDDEN);
}

static void until_done(struct auth *a)
{
    struct auth *one[] = { a };
    double t0 = now_ms();
    while (!a->done && now_ms() - t0 < 10000) {
        step(one, 1, 100);
    }
    assert(a->done);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    assert(mkdir(argv[1], 0700) == 0 && realpath(argv[1], dir)); /* pam.d names the scripts by it */
    snprintf(confdir, sizeof confdir, "%s/pam.d", dir);
    assert(mkdir(confdir, 0700) == 0);
    auth_confdir = confdir;
    char text[2 * PATH_MAX + 200];

    /* pam_exec gives the password on standard input, ending in a NUL. */
    snprintf(text, sizeof text, "#!/bin/bash\nIFS= read -r -d '' pw\n[ \"$pw\" = secret ]\n");
    put(at(dir, "password.sh"), text, 0755);
    /* Says something, waits $(cat finger.wait) s, then passes if finger.ok says 1. */
    snprintf(text, sizeof text, "#!/bin/sh\necho 'Place your finger on the reader'\nsleep \"$(cat %s/finger.wait)\"\n"
             "[ \"$(cat %s/finger.ok)\" = 1 ]\n", dir, dir);
    put(at(dir, "finger.sh"), text, 0755);
    snprintf(text, sizeof text, "#!/bin/bash\nIFS= read -r -d '' pw\nprintf '%%s' \"$pw\" > %s/changed.to\n", dir);
    put(at(dir, "change.sh"), text, 0755);
    snprintf(text, sizeof text, "auth required pam_exec.so expose_authtok quiet %s/password.sh\n"
             "account required pam_permit.so\n", dir);
    put(at(confdir, "password"), text, 0644);
    snprintf(text, sizeof text, "auth required pam_exec.so stdout quiet %s/finger.sh\naccount required pam_permit.so\n", dir);
    put(at(confdir, "finger"), text, 0644);
    snprintf(text, sizeof text, "password required pam_exec.so expose_authtok quiet %s/change.sh\n", dir);
    put(at(confdir, "change"), text, 0644);

    struct auth pw, finger;
    assert(auth_start(&pw, "password", "root", AUTH_AUTHENTICATE) == 0);
    until_asked(&pw);
    assert(auth_answer(&pw, "wrong") == 0);
    until_done(&pw);
    assert(pw.status != 0 && pw.pid == 0);
    assert(auth_start(&pw, "password", "root", AUTH_AUTHENTICATE) == 0);
    until_asked(&pw);
    assert(auth_answer(&pw, "secret") == 0);
    until_done(&pw);
    assert(pw.status == 0);
    puts("PASS a password: asked hidden, a wrong one refused, the right one accepted");

    /* The fingerprint passes while the password is still asked: it wins,
     * and the password's transaction is ended. */
    put(at(dir, "finger.wait"), "1\n", 0644);
    put(at(dir, "finger.ok"), "1\n", 0644);
    assert(auth_start(&pw, "password", "root", AUTH_AUTHENTICATE) == 0);
    assert(auth_start(&finger, "finger", "root", AUTH_AUTHENTICATE) == 0);
    struct auth *both[] = { &pw, &finger };
    double t0 = now_ms();
    while (!finger.done && now_ms() - t0 < 5000) {
        step(both, 2, 100);
    }
    assert(finger.done && finger.status == 0 && strstr(finger.message, "Place your finger"));
    assert(pw.asking == AUTH_ASKS_HIDDEN && !pw.done);
    auth_stop(&pw);
    assert(pw.pid == 0 && !pw.done);
    puts("PASS a fingerprint unlocks while the password waits; what its module says is shown");

    /* The password typed while the fingerprint waits passes at once. */
    put(at(dir, "finger.wait"), "5\n", 0644);
    put(at(dir, "finger.ok"), "0\n", 0644);
    assert(auth_start(&pw, "password", "root", AUTH_AUTHENTICATE) == 0);
    assert(auth_start(&finger, "finger", "root", AUTH_AUTHENTICATE) == 0);
    until_asked(&pw);
    t0 = now_ms();
    assert(auth_answer(&pw, "secret") == 0);
    until_done(&pw);
    assert(pw.status == 0 && now_ms() - t0 < 2000 && !finger.done);
    auth_stop(&finger);
    assert(finger.pid == 0);
    puts("PASS the password passes at once while the fingerprint still waits");

    /* A change: the new password, as often as it is asked (pam_unix asks it
     * twice; pam_exec once), then given to the module. */
    struct auth change;
    struct auth *one[] = { &change };
    int asked = 0;
    assert(auth_start(&change, "change", "root", AUTH_CHANGE) == 0);
    t0 = now_ms();
    while (!change.done && now_ms() - t0 < 5000) {
        if (step(one, 1, 100) && change.asking == AUTH_ASKS_HIDDEN) {
            assert(auth_answer(&change, "n3w pass") == 0);
            asked++;
        }
    }
    FILE *f = fopen(at(dir, "changed.to"), "r");
    assert(change.done && change.status == 0 && asked > 0 && f && fgets(text, sizeof text, f) &&
           strcmp(text, "n3w pass") == 0);
    fclose(f);
    puts("PASS a password change: the new one asked, then given to the module");

    /* Children reaped elsewhere (SIGCHLD ignored, as a program that loads a
     * PAM module may have it): their exit status is gone, and only what the
     * child said decides. Before, a lost status counted as success. */
    signal(SIGCHLD, SIG_IGN);
    assert(auth_start(&pw, "password", "root", AUTH_AUTHENTICATE) == 0);
    until_asked(&pw);
    assert(auth_answer(&pw, "wrong") == 0);
    until_done(&pw);
    assert(pw.status != 0);
    assert(auth_start(&pw, "password", "root", AUTH_AUTHENTICATE) == 0);
    until_asked(&pw);
    assert(auth_answer(&pw, "secret") == 0);
    until_done(&pw);
    assert(pw.status == 0);
    signal(SIGCHLD, SIG_DFL);
    puts("PASS with the children reaped elsewhere, a wrong password still fails and the right one passes");
    puts("ALL PASSED");
    return 0;
}
