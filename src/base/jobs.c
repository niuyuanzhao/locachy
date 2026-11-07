/* The programs' side of the system jobs' queue (jobs.h): requests to
 * locachy-jobs on its control socket. Not linked into process 1. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "base.h"
#include "jobs.h"

/* Send fields (up to NULL) and take the answer: its fields into out (max at
 * most, NUL separated in buf). How many, or -1 with why. */
static int ask(const char *const fields[], char *buf, size_t size, char **out, int max, char *why, size_t wsize)
{
    static char msg[JOBS_MSG_MAX];
    size_t len = 0;
    for (int i = 0; fields[i]; i++) {
        size_t n = strlen(fields[i]) + 1;
        if (len + n > sizeof msg) {
            snprintf(why, wsize, "%s", _("The request is too long."));
            return -1;
        }
        memcpy(msg + len, fields[i], n);
        len += n;
    }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", JOBS_CONTROL);
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct timeval limit = { .tv_sec = 5 };
    if (s < 0 || setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit) < 0 ||
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit) < 0 ||
        connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        snprintf(why, wsize, _("The job service (locachy-jobs) does not run: %s."), strerror(errno));
        if (s >= 0) close(s);
        return -1;
    }
    ssize_t got = send(s, msg, len ? len - 1 : 0, MSG_NOSIGNAL) == (ssize_t)(len ? len - 1 : 0) ?
                  recv(s, buf, size - 1, 0) : -1;
    int err = errno;
    close(s);
    if (got <= 0) {
        snprintf(why, wsize, _("The job service (locachy-jobs) did not answer: %s."),
                 got == 0 ? _("refused") : strerror(err));
        return -1;
    }
    buf[got] = '\0';
    int n = 0;
    for (ssize_t i = 0; i < got && n < max; i += (ssize_t)strlen(buf + i) + 1) {
        out[n++] = buf + i;
    }
    if (n > 0 && strcmp(out[0], "ok") != 0) {
        snprintf(why, wsize, "%s", n > 1 ? out[1] : out[0]);
        return strcmp(out[0], "confirm") == 0 ? -2 : -1;
    }
    return n;
}

/* A ROW (jobs.h) into row: 0, or -1 if it is not one. */
static int parse_row(char *text, struct jobs_row *row)
{
    char *f[8];
    int n = 0;
    for (char *p = text; n < 8; n++) {
        f[n] = p;
        char *tab = strchr(p, '\t');
        if (!tab) {
            n++;
            break;
        }
        *tab = '\0';
        p = tab + 1;
    }
    if (n != 8) return -1;
    memset(row, 0, sizeof *row);
    row->id = atoi(f[0]);
    row->status = atoi(f[3]);
    row->position = atoi(f[4]);
    snprintf(row->kind, sizeof row->kind, "%s", f[1]);
    snprintf(row->state, sizeof row->state, "%s", f[2]);
    snprintf(row->from, sizeof row->from, "%s", f[5]);
    snprintf(row->args, sizeof row->args, "%s", f[6]);
    snprintf(row->last, sizeof row->last, "%s", f[7]);
    return row->id > 0 ? 0 : -1;
}

int jobs_submit(const char *kind, const char *const args[], int *id, int *position, char *why, size_t size)
{
    static char buf[4096];
    const char *fields[JOBS_ARGS_MAX + 3] = { "submit", kind };
    char *out[4];
    int n = 2;
    for (; args && args[n - 2] && n < JOBS_ARGS_MAX + 2; n++) {
        fields[n] = args[n - 2];
    }
    fields[n] = NULL;
    int got = ask(fields, buf, sizeof buf, out, 4, why, size);
    if (got != 3) {
        if (got >= 0) snprintf(why, size, "%s", _("The job service gave no job."));
        return -1;
    }
    if (id) *id = atoi(out[1]);
    if (position) *position = atoi(out[2]);
    return 0;
}

int jobs_list(struct jobs_row *rows, int max, char *why, size_t size)
{
    static char buf[JOBS_MSG_MAX];
    static const char *const fields[] = { "list", NULL };
    char *out[JOBS_ROWS_MAX + 1];
    int got = ask(fields, buf, sizeof buf, out, JOBS_ROWS_MAX + 1, why, size);
    if (got < 1) return -1;
    int n = 0;
    for (int i = 1; i < got && n < max; i++) {
        if (parse_row(out[i], &rows[n]) == 0) n++;
    }
    return n;
}

int jobs_status(int id, struct jobs_row *row, char *why, size_t size)
{
    static char buf[4096];
    char number[16];
    snprintf(number, sizeof number, "%d", id);
    const char *const fields[] = { "status", number, NULL };
    char *out[2];
    int got = ask(fields, buf, sizeof buf, out, 2, why, size);
    if (got != 2 || parse_row(out[1], row) < 0) {
        if (got >= 0) snprintf(why, size, "%s", _("The job service's answer is not a job."));
        return -1;
    }
    return 0;
}

int jobs_log(int id, char *buf, size_t size)
{
    static char reply[JOBS_LOG_TAIL + 64];
    char number[16], why[256];
    snprintf(number, sizeof number, "%d", id);
    const char *const fields[] = { "log", number, NULL };
    char *out[2];
    int got = ask(fields, reply, sizeof reply, out, 2, why, sizeof why);
    snprintf(buf, size, "%s", got == 2 ? out[1] : got == 1 ? "" : why);
    return got >= 1 ? 0 : -1;
}

int jobs_cancel(int id, int confirm, char *why, size_t size)
{
    static char buf[1024];
    char number[16];
    snprintf(number, sizeof number, "%d", id);
    const char *const fields[] = { "cancel", number, confirm ? "confirm" : NULL, NULL };
    char *out[2];
    int got = ask(fields, buf, sizeof buf, out, 2, why, size);
    if (got == -2) return 1;
    if (got < 0) return -1;
    snprintf(why, size, "%s", got == 2 ? out[1] : "");
    return 0;
}

int jobs_ended(const struct jobs_row *row)
{
    return strcmp(row->state, "done") == 0 || strcmp(row->state, "failed") == 0 ||
           strcmp(row->state, "cancelled") == 0 || strcmp(row->state, "keys") == 0;
}

void jobs_log_path(int id, char *out, size_t size)
{
    snprintf(out, size, "%s/%d.log", JOBS_DIR, id);
}

int jobs_mine(struct jobs_row *rows, int max)
{
    static struct jobs_row all[JOBS_ROWS_MAX];
    char why[256], me[32] = "";
    FILE *in = fopen("/proc/self/comm", "re");
    if (!in || !fgets(me, sizeof me, in)) me[0] = '\0';
    if (in) fclose(in);
    me[strcspn(me, "\n")] = '\0';
    int n = jobs_list(all, JOBS_ROWS_MAX, why, sizeof why), k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (me[0] && strcmp(all[i].from, me) == 0) rows[k++] = all[i];
    }
    return n < 0 ? -1 : k;
}

/* The job cgroups (job-ID) that have processes: how many, the first id into
 * running. */
static int populated_jobs(struct jobs_row *running)
{
    DIR *d = opendir(JOBS_CGROUP);
    struct dirent *e;
    int n = 0, id;
    while (d && (e = readdir(d)) != NULL) {
        char path[512], line[64];
        int populated = 0;
        if (sscanf(e->d_name, "job-%d", &id) != 1 || id <= 0) continue;
        snprintf(path, sizeof path, "%s/%s/cgroup.events", JOBS_CGROUP, e->d_name);
        FILE *in = fopen(path, "re");
        while (in && fgets(line, sizeof line, in)) {
            if (strncmp(line, "populated ", 10) == 0) populated = atoi(line + 10);
        }
        if (in) fclose(in);
        if (!populated) continue;
        if (running && n == 0) {
            memset(running, 0, sizeof *running);
            running->id = id;
            snprintf(running->state, sizeof running->state, "running");
        }
        n++;
    }
    if (d) closedir(d);
    return n;
}

int jobs_pending(struct jobs_row *running)
{
    static struct jobs_row rows[JOBS_ROWS_MAX];
    char why[256];
    int n = jobs_list(rows, JOBS_ROWS_MAX, why, sizeof why), pending = 0, found = 0;
    if (n < 0) return populated_jobs(running);
    for (int i = 0; i < n; i++) {
        if (jobs_ended(&rows[i])) continue;
        pending++;
        if (running && !found && rows[i].position == 0) {
            *running = rows[i];
            found = 1;
        }
    }
    for (int i = 0; running && !found && i < n; i++) {
        if (!jobs_ended(&rows[i])) {
            *running = rows[i];
            found = 1;
        }
    }
    return pending;
}

const char *jobs_state_label(const char *state)
{
    if (strcmp(state, "queued") == 0) return _("queued");
    if (strcmp(state, "running") == 0) return _("running");
    if (strcmp(state, "stopping") == 0) return _("stopping");
    if (strcmp(state, "done") == 0) return _("done");
    if (strcmp(state, "failed") == 0) return _("failed");
    if (strcmp(state, "cancelled") == 0) return _("cancelled");
    if (strcmp(state, "keys") == 0) return _("waiting for keys");
    return state;
}
