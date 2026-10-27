/* pam_locachy_any.so (built with AUTH_TEST) in a sudo-like PAM service of
 * its own, run by a client on a pseudo-terminal of its own: services in a
 * scratch confdir made of pam_exec scripts ("finger" says something, waits,
 * then passes or fails; the stack's password check passes "secret"). Checks
 * that a typed password goes to the stack at once, a wrong one fails there,
 * the finger wins without anything left reading the terminal, Enter alone
 * leaves the password to the stack, and Ctrl+C puts the echo back before the
 * client's own handling. Nothing of the system's PAM configuration.
 * test-pam-any MODULE.so SCRATCH_DIR (a new directory; as root). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <limits.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <security/pam_appl.h>

static char dir[PATH_MAX];

static void put(const char *name, const char *text, int mode)
{
    char path[2 * PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
    assert(chmod(path, (mode_t)mode) == 0);
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* The client's conversation: the stack's own password prompt gets "secret". */
static int converse(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    (void)data;
    struct pam_response *r = calloc((size_t)n, sizeof *r);
    for (int i = 0; i < n; i++) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            r[i].resp = strdup("secret");
        }
    }
    *resp = r;
    return PAM_SUCCESS;
}

static void interrupted(int sig)
{
    (void)sig;
    struct termios t;
    _exit(tcgetattr(0, &t) == 0 && (t.c_lflag & ECHO) ? 40 : 41);
}

/* The client: pam_authenticate on the terminal; exit 0 passed, 1 refused,
 * +2 if the echo was left off, +4 if the line after was not its own. */
static int client(int read_after)
{
    struct pam_conv conv = { converse, NULL };
    pam_handle_t *h;
    char confdir[PATH_MAX + 8], line[64];
    snprintf(confdir, sizeof confdir, "%s/pam.d", dir);
    signal(SIGINT, interrupted);
    assert(pam_start_confdir("sudo-like", "root", &conv, confdir, &h) == PAM_SUCCESS);
    int rc = pam_authenticate(h, 0);
    pam_end(h, rc);
    struct termios t;
    int code = rc == PAM_SUCCESS ? 0 : 1;
    if (tcgetattr(0, &t) < 0 || !(t.c_lflag & ECHO)) {
        code += 2;
    }
    if (read_after && (!fgets(line, sizeof line, stdin) || strcmp(line, "after\n") != 0)) {
        code += 4;
    }
    return code;
}

struct run {
    pid_t pid;
    int master;
    char out[8192];
    size_t len;
};

static void start(struct run *r, int read_after)
{
    memset(r, 0, sizeof *r);
    r->pid = forkpty(&r->master, NULL, NULL, NULL);
    assert(r->pid >= 0);
    if (r->pid == 0) {
        _exit(client(read_after));
    }
}

/* Read what the client writes until text appears (or s seconds pass). */
static int wait_for(struct run *r, const char *text, double s)
{
    double t0 = now();
    while (now() - t0 < s) {
        r->out[r->len] = '\0';
        if (strstr(r->out, text)) return 1;
        struct pollfd p = { .fd = r->master, .events = POLLIN };
        if (poll(&p, 1, 100) > 0) {
            ssize_t n = read(r->master, r->out + r->len, sizeof r->out - 1 - r->len);
            if (n <= 0) break;
            r->len += (size_t)n;
        }
    }
    r->out[r->len] = '\0';
    return strstr(r->out, text) != NULL;
}

static int finish(struct run *r)
{
    int status;
    double t0 = now();
    while (waitpid(r->pid, &status, WNOHANG) == 0) {
        assert(now() - t0 < 10);
        wait_for(r, "\001", 0.1); /* keep draining the terminal */
    }
    close(r->master);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    setvbuf(stdout, NULL, _IOLBF, 0);
    assert(mkdir(argv[2], 0700) == 0 && realpath(argv[2], dir));
    char pamd[PATH_MAX + 8], text[4 * PATH_MAX], module[PATH_MAX];
    assert(realpath(argv[1], module));
    snprintf(pamd, sizeof pamd, "%s/pam.d", dir);
    assert(mkdir(pamd, 0700) == 0);
    snprintf(text, sizeof text, "#!/bin/bash\necho 'Place your finger'\nsleep \"$(cat %s/finger.wait)\"\n"
             "[ \"$(cat %s/finger.ok)\" = 1 ]\n", dir, dir);
    put("finger.sh", text, 0755);
    put("password.sh", "#!/bin/bash\nIFS= read -r -d '' pw\n[ \"$pw\" = secret ]\n", 0755);
    snprintf(text, sizeof text, "auth required pam_exec.so stdout quiet %s/finger.sh\naccount required pam_permit.so\n",
             dir);
    put("pam.d/finger", text, 0644);
    snprintf(text, sizeof text, "auth sufficient %s confdir=%s finger\n"
             "auth required pam_exec.so expose_authtok quiet %s/password.sh\naccount required pam_permit.so\n",
             module, pamd, dir);
    put("pam.d/sudo-like", text, 0644);
    struct run r;

    put("finger.wait", "5\n", 0644);
    put("finger.ok", "0\n", 0644);
    start(&r, 0);
    assert(wait_for(&r, "Password for root (or a fingerprint): ", 5));
    double t0 = now();
    assert(write(r.master, "secret\n", 7) == 7);
    assert(finish(&r) == 0 && now() - t0 < 2);
    assert(!strstr(r.out, "secret")); /* not echoed */
    puts("PASS a typed password goes to the stack at once (not echoed), while the finger still waits");

    start(&r, 0);
    assert(wait_for(&r, "Password for root", 5));
    assert(write(r.master, "wrong\n", 6) == 6);
    assert(finish(&r) == 1);
    puts("PASS a wrong one is refused by the stack");

    put("finger.wait", "1\n", 0644);
    put("finger.ok", "1\n", 0644);
    start(&r, 1);
    assert(wait_for(&r, "Place your finger", 5));
    usleep(1500000); /* the finger has passed */
    assert(write(r.master, "after\n", 6) == 6);
    assert(finish(&r) == 0);
    puts("PASS the finger wins; the echo is back and the next line is the program's, not left to a prompt");

    put("finger.wait", "5\n", 0644);
    put("finger.ok", "0\n", 0644);
    start(&r, 0);
    assert(wait_for(&r, "Password for root", 5));
    assert(write(r.master, "\n", 1) == 1);
    assert(finish(&r) == 0);
    puts("PASS Enter alone leaves the password to the stack (its own prompt)");

    start(&r, 0);
    assert(wait_for(&r, "Password for root", 5));
    assert(write(r.master, "\003", 1) == 1);
    assert(finish(&r) == 40);
    puts("PASS Ctrl+C: the echo is back when the program's own handler runs");
    puts("ALL PASSED");
    return 0;
}
