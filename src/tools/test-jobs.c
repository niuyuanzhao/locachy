/*
 * A client of locachy-jobs for tools/test-jobs.sh, through base/jobs.c (the
 * programs' helper), with the peer it should look like:
 *   test-jobs [--cgroup LINE] [--uid N] COMMAND ARG...
 *     --cgroup LINE  JOBS_PROC/PID/cgroup of this process says LINE first
 *                    (the service reads who it is there); none: "0::/p1"
 *     --uid N        connect as uid N (setuid before connecting)
 *   submit KIND ARG...    "ok ID POSITION" or "error WHY"
 *   list                  a ROW a line (jobs.h)
 *   status ID | log ID
 *   cancel ID [confirm]   "ok WHAT", "confirm WHY" or "error WHY"
 *   pending               the jobs not ended, and the running one's id
 *   raw FIELD...          FIELDs NUL separated, sent as they are (a "" field
 *                         last: a trailing NUL); the answer's fields, a line each
 *   hold KIND ARG...      submit, then wait (killed by the test: a client
 *                         that goes away)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "jobs.h"
#include "i18n-stub.h"

#ifndef JOBS_PROC
#define JOBS_PROC "/proc"
#endif

void say(const char *fmt, ...)
{
    (void)fmt;
}

static int raw(int argc, char **argv)
{
    char msg[JOBS_MSG_MAX], reply[JOBS_MSG_MAX];
    size_t len = 0;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (len + n + 1 > sizeof msg) return 2;
        memcpy(msg + len, argv[i], n);
        len += n;
        if (i + 1 < argc) msg[len++] = '\0';
    }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", JOBS_CONTROL);
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        printf("connect %s\n", strerror(errno));
        return 1;
    }
    ssize_t got = send(s, msg, len, 0) == (ssize_t)len ? recv(s, reply, sizeof reply - 1, 0) : -1;
    if (got <= 0) {
        printf("no answer\n");
        return 1;
    }
    for (ssize_t i = 0; i < got; i += (ssize_t)strlen(reply + i) + 1) {
        reply[got] = '\0';
        printf("%s\n", reply + i);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *line = "0::/p1", *comm = NULL;
    int uid = -1, i = 1;
    for (; i + 1 < argc && strncmp(argv[i], "--", 2) == 0; i += 2) {
        if (strcmp(argv[i], "--cgroup") == 0) line = argv[i + 1];
        else if (strcmp(argv[i], "--uid") == 0) uid = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--comm") == 0) comm = argv[i + 1];
    }
    if (comm) prctl(PR_SET_NAME, comm, 0, 0, 0);
    if (i >= argc) return 2;
    char path[256];
    snprintf(path, sizeof path, "%s/%d", JOBS_PROC, (int)getpid());
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/%d/cgroup", JOBS_PROC, (int)getpid());
    FILE *f = fopen(path, "w");
    if (f) {
        if (line[0]) fprintf(f, "%s\n", line);
        fclose(f);
    }
    if (uid >= 0 && (setgroups(0, NULL) < 0 || setgid((gid_t)uid) < 0 || setuid((uid_t)uid) < 0)) {
        printf("setuid %s\n", strerror(errno));
        return 3;
    }
    const char *cmd = argv[i];
    char why[512];
    if (strcmp(cmd, "submit-input") == 0) {
        if (i + 1 >= argc) return 2;
        char secret[JOBS_INPUT_MAX + 1];
        ssize_t len = read(STDIN_FILENO, secret, sizeof secret);
        if (len <= 0 || len > JOBS_INPUT_MAX) return 2;
        int input[2];
        if (pipe2(input, O_CLOEXEC) < 0) return 2;
        if (write(input[1], secret, (size_t)len) != len) return 2;
        memset(secret, 0, sizeof secret);
        close(input[1]);
        int id = 0, pos = 0;
        int result = jobs_submit_input(argv[i + 1], (const char *const *)argv + i + 2,
                                       input[0], &id, &pos, why, sizeof why);
        close(input[0]);
        if (result < 0) {
            printf("error %s\n", why);
            return 1;
        }
        printf("ok %d %d\n", id, pos);
        return 0;
    }
    if (strcmp(cmd, "submit") == 0 || strcmp(cmd, "hold") == 0) {
        if (i + 1 >= argc) return 2;
        int id = 0, pos = 0;
        if (jobs_submit(argv[i + 1], (const char *const *)argv + i + 2, &id, &pos, why, sizeof why) < 0) {
            printf("error %s\n", why);
            return 1;
        }
        printf("ok %d %d\n", id, pos);
        fflush(stdout);
        if (cmd[0] == 'h') pause();
        return 0;
    }
    if (strcmp(cmd, "list") == 0) {
        static struct jobs_row rows[JOBS_ROWS_MAX];
        int n = jobs_list(rows, JOBS_ROWS_MAX, why, sizeof why);
        if (n < 0) {
            printf("error %s\n", why);
            return 1;
        }
        for (int k = 0; k < n; k++) {
            printf("%d\t%s\t%s\t%d\t%d\t%s\t%s\t%s\n", rows[k].id, rows[k].kind, rows[k].state, rows[k].status,
                   rows[k].position, rows[k].from, rows[k].args, rows[k].last);
        }
        return 0;
    }
    if (strcmp(cmd, "status") == 0 && i + 1 < argc) {
        struct jobs_row r;
        if (jobs_status(atoi(argv[i + 1]), &r, why, sizeof why) < 0) {
            printf("error %s\n", why);
            return 1;
        }
        printf("%d\t%s\t%s\t%d\t%d\t%s\t%s\t%s\n", r.id, r.kind, r.state, r.status, r.position, r.from, r.args,
               r.last);
        return 0;
    }
    if (strcmp(cmd, "log") == 0 && i + 1 < argc) {
        static char text[JOBS_LOG_TAIL + 64];
        int r = jobs_log(atoi(argv[i + 1]), text, sizeof text);
        printf("%s%s", r < 0 ? "error " : "", text);
        return r < 0;
    }
    if (strcmp(cmd, "cancel") == 0 && i + 1 < argc) {
        int r = jobs_cancel(atoi(argv[i + 1]), i + 2 < argc && strcmp(argv[i + 2], "confirm") == 0, why, sizeof why);
        printf("%s %s\n", r == 0 ? "ok" : r == 1 ? "confirm" : "error", why);
        return r < 0;
    }
    if (strcmp(cmd, "mine") == 0) {
        static struct jobs_row rows[JOBS_ROWS_MAX];
        int n = jobs_mine(rows, JOBS_ROWS_MAX);
        if (n < 0) return 1;
        for (int k = 0; k < n; k++) {
            printf("%d:%s:%s:%s\n", rows[k].id, rows[k].kind, rows[k].state, rows[k].from);
        }
        return 0;
    }
    if (strcmp(cmd, "pending") == 0) {
        struct jobs_row r = { 0 };
        int n = jobs_pending(&r);
        printf("%d %d\n", n, n > 0 ? r.id : 0);
        return 0;
    }
    if (strcmp(cmd, "raw") == 0) {
        return raw(argc - i - 1, argv + i + 1);
    }
    return 2;
}
