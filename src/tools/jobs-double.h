/* The system jobs' queue (base/include/jobs.h) for a test that includes a
 * program: a job is a number and nothing runs; the queue itself is
 * test-jobs.sh's. jobs_double_hook, if set, is asked first and refuses the
 * job with -1; jobs_double_ended says whether the jobs have ended (done, or
 * failed with jobs_double_status); jobs_double_last is the last job asked
 * for, "KIND ARG..."; jobs_double_mine are the jobs a new run of the program
 * finds (jobs_mine). */
#ifndef LOCACHY_JOBS_DOUBLE_H
#define LOCACHY_JOBS_DOUBLE_H

#include <stdio.h>
#include <string.h>

#include "jobs.h"

static int jobs_double_count, jobs_double_ended = 1, jobs_double_status, jobs_double_nmine;
static int (*jobs_double_hook)(void);
static char jobs_double_last[512];
static struct jobs_row jobs_double_mine[4];

int jobs_submit(const char *kind, const char *const args[], int *id, int *position, char *why, size_t size)
{
    if (jobs_double_hook && jobs_double_hook() < 0) {
        snprintf(why, size, "refused");
        return -1;
    }
    size_t len = (size_t)snprintf(jobs_double_last, sizeof jobs_double_last, "%s", kind);
    for (int i = 0; args && args[i] && len < sizeof jobs_double_last; i++) {
        len += (size_t)snprintf(jobs_double_last + len, sizeof jobs_double_last - len, " %s", args[i]);
    }
    *id = 7000 + ++jobs_double_count;
    *position = 0;
    return 0;
}

int jobs_status(int id, struct jobs_row *row, char *why, size_t size)
{
    (void)why;
    (void)size;
    memset(row, 0, sizeof *row);
    row->id = id;
    row->status = jobs_double_ended ? jobs_double_status : -1;
    snprintf(row->state, sizeof row->state, "%s",
             !jobs_double_ended ? "running" : jobs_double_status ? "failed" : "done");
    snprintf(row->last, sizeof row->last, "working");
    return 0;
}

int jobs_ended(const struct jobs_row *row)
{
    return strcmp(row->state, "running") != 0 && strcmp(row->state, "queued") != 0 &&
           strcmp(row->state, "stopping") != 0;
}

void jobs_log_path(int id, char *out, size_t size)
{
    snprintf(out, size, "/run/locachy/jobs/%d.log", id);
}

int jobs_mine(struct jobs_row *rows, int max)
{
    int n = jobs_double_nmine < max ? jobs_double_nmine : max;
    memcpy(rows, jobs_double_mine, (size_t)n * sizeof *rows);
    return n;
}

#endif
