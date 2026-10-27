/*
 * progs/auth.c: a PAM transaction in a child process of its own (auth.h).
 * Lines from the child: "secret TEXT" (asks, not to be shown: a password),
 * "ask TEXT" (asks), "info TEXT", "error TEXT" (what a module says), "failed
 * TEXT" (PAM's own word when it fails), "passed" (it succeeded); each
 * "secret"/"ask" waits for one line back. It succeeded only if it said
 * "passed" and exited 0: a process that reaps children itself (or ignores
 * SIGCHLD) can take its exit status first, and then "passed" decides. Tests
 * build it with AUTH_TEST and set auth_confdir: a directory of their own
 * services (pam_start_confdir), never the system's.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <security/pam_appl.h>

#include "auth.h"

#ifdef AUTH_TEST
const char *auth_confdir;
#endif

struct child {
    int in, out;
};

static void put_line(int fd, const char *kind, const char *text)
{
    char line[1024];
    int n = snprintf(line, sizeof line, "%s %s", kind, text ? text : "");
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof line - 2) {
        n = (int)sizeof line - 2;
    }
    for (int i = 0; i < n; i++) {
        if (line[i] == '\n' || line[i] == '\r') {
            line[i] = ' ';
        }
    }
    line[n++] = '\n';
    for (int done = 0; done < n;) {
        ssize_t w = write(fd, line + done, (size_t)(n - done));
        if (w <= 0 && errno != EINTR) {
            return;
        }
        done += w > 0 ? (int)w : 0;
    }
}

/* One line from the parent, without its newline; NULL when it has gone. */
static char *get_line(int fd)
{
    char line[1024];
    size_t len = 0;
    for (;;) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return NULL;
        }
        if (c == '\n') {
            break;
        }
        if (len < sizeof line - 1) {
            line[len++] = c;
        }
    }
    line[len] = '\0';
    char *copy = strdup(line);
    explicit_bzero(line, sizeof line);
    return copy;
}

static int converse(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    struct child *c = data;
    struct pam_response *r = calloc((size_t)n, sizeof *r);
    if (r == NULL) {
        return PAM_BUF_ERR;
    }
    for (int i = 0; i < n; i++) {
        int style = msg[i]->msg_style;
        if (style == PAM_PROMPT_ECHO_OFF || style == PAM_PROMPT_ECHO_ON) {
            put_line(c->out, style == PAM_PROMPT_ECHO_OFF ? "secret" : "ask", msg[i]->msg);
            if ((r[i].resp = get_line(c->in)) == NULL) {
                for (int k = 0; k < i; k++) {
                    free(r[k].resp);
                }
                free(r);
                return PAM_CONV_ERR;
            }
        } else {
            put_line(c->out, style == PAM_ERROR_MSG ? "error" : "info", msg[i]->msg);
        }
    }
    *resp = r;
    return PAM_SUCCESS;
}

static void child(const char *service, const char *user, int what, int in, int out)
{
    struct child c = { in, out };
    struct pam_conv conv = { converse, &c };
    pam_handle_t *h = NULL;
#ifdef AUTH_TEST
    int rc = pam_start_confdir(service, user, &conv, auth_confdir, &h);
#else
    int rc = pam_start(service, user, &conv, &h);
#endif
    if (rc == PAM_SUCCESS) {
        rc = what == AUTH_CHANGE ? pam_chauthtok(h, 0) : pam_authenticate(h, 0);
    }
    if (rc == PAM_SUCCESS && what == AUTH_AUTHENTICATE) {
        rc = pam_acct_mgmt(h, 0);
    }
    if (rc != PAM_SUCCESS) {
        put_line(out, "failed", pam_strerror(h, rc));
    } else {
        put_line(out, "passed", "");
    }
    if (h != NULL) {
        pam_end(h, rc);
    }
    _exit(rc == PAM_SUCCESS ? 0 : 1);
}

int auth_start(struct auth *a, const char *service, const char *user, int what)
{
    int up[2], down[2];
    memset(a, 0, sizeof *a);
    a->from = a->to = -1;
    if (pipe2(up, O_CLOEXEC) < 0) {
        return -1;
    }
    if (pipe2(down, O_CLOEXEC) < 0) {
        close(up[0]);
        close(up[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0); /* auth_stop ends whatever its modules started too */
        signal(SIGPIPE, SIG_DFL);
        close(up[0]);
        close(down[1]);
        child(service, user, what, down[0], up[1]);
    }
    close(up[1]);
    close(down[0]);
    if (pid < 0) {
        close(up[0]);
        close(down[1]);
        return -1;
    }
    fcntl(up[0], F_SETFL, O_NONBLOCK);
    a->pid = pid;
    a->from = up[0];
    a->to = down[1];
    return 0;
}

static void ended(struct auth *a)
{
    int status = 0;
    pid_t r;
    while ((r = waitpid(a->pid, &status, 0)) < 0 && errno == EINTR) {
    }
    close(a->from);
    close(a->to);
    a->from = a->to = -1;
    a->pid = 0;
    a->asking = AUTH_IDLE;
    a->done = 1;
    if (r != a->pid) {
        a->status = a->passed ? 0 : 255; /* reaped by someone else: what it said */
    } else {
        a->status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (a->status == 0 && !a->passed) {
            a->status = 255;
        }
    }
}

/* at_once: an earlier line came in the same read (pam_faillock's two lines
 * on a locked account: one message). */
static void got_line(struct auth *a, char *line, int at_once)
{
    char *text = strchr(line, ' ');
    text = text ? text + 1 : line + strlen(line);
    if (strncmp(line, "secret ", 7) == 0 || strncmp(line, "ask ", 4) == 0) {
        a->asking = line[0] == 's' ? AUTH_ASKS_HIDDEN : AUTH_ASKS_SHOWN;
        snprintf(a->prompt, sizeof a->prompt, "%s", text);
        a->said = 0;
    } else if (strncmp(line, "passed ", 7) == 0) {
        a->passed = 1;
    } else if (strncmp(line, "failed ", 7) == 0) {
        if (!a->said) {
            snprintf(a->message, sizeof a->message, "%s", text);
        }
    } else if (*text) {
        size_t n = a->said && at_once ? strlen(a->message) : 0;
        snprintf(a->message + n, sizeof a->message - n, "%s%s", n ? " " : "", text);
        a->said = 1;
    }
}

int auth_read(struct auth *a)
{
    int changed = 0, lines = 0;
    if (a->pid == 0) {
        return 0;
    }
    for (;;) {
        ssize_t r = read(a->from, a->line + a->len, sizeof a->line - 1 - a->len);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0) {
            break; /* nothing more now */
        }
        if (r == 0) {
            ended(a);
            return 1;
        }
        a->len += (size_t)r;
        char *nl;
        while ((nl = memchr(a->line, '\n', a->len)) != NULL) {
            *nl = '\0';
            got_line(a, a->line, lines++ > 0);
            size_t used = (size_t)(nl - a->line) + 1;
            memmove(a->line, nl + 1, a->len - used);
            a->len -= used;
            changed = 1;
        }
        if (a->len == sizeof a->line - 1) {
            a->len = 0; /* a line longer than any module writes: dropped */
        }
    }
    return changed;
}

int auth_answer(struct auth *a, const char *text)
{
    if (a->pid == 0 || a->asking == AUTH_IDLE) {
        return -1;
    }
    a->asking = AUTH_IDLE;
    size_t n = strlen(text);
    char *line = malloc(n + 1);
    if (line == NULL) {
        return -1;
    }
    memcpy(line, text, n);
    line[n] = '\n';
    int ok = write(a->to, line, n + 1) == (ssize_t)(n + 1);
    explicit_bzero(line, n + 1);
    free(line);
    return ok ? 0 : -1;
}

void auth_stop(struct auth *a)
{
    if (a->pid == 0) {
        return;
    }
    /* At once: a module waiting on a device or a bus would not end on a
     * request, and its service notices the client gone. A command a module
     * runs in a session of its own (pam_exec) is not in the group: the
     * lock's cgroup ends it when the lock exits (process 1). */
    kill(-a->pid, SIGKILL);
    kill(a->pid, SIGKILL);
    ended(a);
    a->done = 0;
}
