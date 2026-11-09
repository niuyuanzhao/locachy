/*
 * System jobs (tools/JOBS.md): the queue of long root operations (package
 * transactions, kernel and repository changes, the boot image, firmware)
 * that locachy-jobs, a service (rc.conf service_jobs), runs one at a time in
 * cgroups of its own, apart from the programs that ask for them: closing or
 * quitting a program, or its freezing, never touches a job.
 *
 * The control socket JOBS_CONTROL: SOCK_SEQPACKET, root's alone (0600, in a
 * directory of mode 0700). A peer is root and not a container's process
 * (cgroup app_NAME; a native window's app_window-PROGRAM-N is a host program
 * as root). One request a message, one reply a message; fields separated by
 * NUL:
 *   submit KIND ARG...   ok ID POSITION   queued (POSITION 0: it runs now; N:
 *                                         N jobs before it); a job the same as
 *                                         one still queued: that one
 *                        error WHY        KIND or an argument refused
 *   list                 ok ROW...        every job kept, oldest first
 *   status ID            ok ROW
 *   log ID               ok TEXT          the end of its output (JOBS_LOG_TAIL
 *                                         bytes at most, from a line's start)
 *   cancel ID            ok removed       it was queued: it never runs
 *                        ok ended         it had ended already
 *                        confirm WHY      it runs: stopping it needs confirm
 *   cancel ID confirm    ok stopping      SIGINT to its processes, as Ctrl+C
 *                                         in a terminal (pacman stops between
 *                                         packages and releases its lock),
 *                                         then SIGKILL after JOBS_STOP_GRACE s
 * Any request can also be answered "error WHY" (unknown ID, malformed).
 * A ROW: ID, KIND, STATE, STATUS, POSITION, FROM, ARGS, LAST, tab separated:
 *   STATE     queued, running, stopping, done, failed, cancelled, or keys:
 *             a build (locachy-pkg's, locachy-app's) stopped for source
 *             signing keys a person has not trusted yet (locachy-build exits
 *             3, its untrusted-keys lists them); the program that asked
 *             asks again once they are trusted
 *   STATUS    the exit status once it has ended (128 + N: signal N); -1 before
 *   POSITION  0 running, N queued with N jobs before it, -1 ended
 *   FROM      the program that asked (its name, /proc/PID/comm)
 *   ARGS      its arguments, separated by spaces (for people)
 *   LAST      the last line of its output
 * The files of job ID (root's): JOBS_DIR/ID.state, ID.log (its output).
 */
#ifndef LOCACHY_JOBS_H
#define LOCACHY_JOBS_H

#include <stddef.h>

#ifndef JOBS_DIR
#define JOBS_DIR "/run/locachy/jobs"
#endif
#ifndef JOBS_CGROUP
#define JOBS_CGROUP "/sys/fs/cgroup/locachy-jobs" /* service: the service's; job-ID: a job's */
#endif
#define JOBS_CONTROL JOBS_DIR "/control"
#define JOBS_MSG_MAX 65536
#define JOBS_LOG_TAIL 8192
#define JOBS_ARGS_MAX 64     /* arguments of one job */
#define JOBS_ROWS_MAX 96     /* what list answers at most: the queue and the jobs kept */

struct jobs_row {
    int id, status, position;
    char kind[24], state[16], from[32];
    char args[512], last[256];
};

/* Queue KIND ARG... (args up to NULL). 0 with its id and position, or -1
 * with why (the service's words, or that it does not run). */
int jobs_submit(const char *kind, const char *const args[], int *id, int *position, char *why, size_t size);
/* The jobs, oldest first: how many (max at most), or -1 with why. */
int jobs_list(struct jobs_row *rows, int max, char *why, size_t size);
int jobs_status(int id, struct jobs_row *row, char *why, size_t size);
/* The end of job id's output, into buf: 0, or -1 with why in buf. */
int jobs_log(int id, char *buf, size_t size);
/* Cancel job id: 0 when done (why: removed, ended, stopping), 1 when it runs
 * and confirm was 0 (why: the service's warning), -1 on error. */
int jobs_cancel(int id, int confirm, char *why, size_t size);
/* It has ended: done, failed, cancelled or keys. */
int jobs_ended(const struct jobs_row *row);
/* The output of job id (JOBS_DIR/ID.log, root's): its path. */
void jobs_log_path(int id, char *out, size_t size);
/* The jobs this program follows (its own by name, -window stripped, and any
 * running or queued job in the same domain so packages, settings and firmware
 * see each other's active work), oldest first: how many, or -1 with no service. */
int jobs_mine(struct jobs_row *rows, int max);
/* How many jobs have not ended (running or queued), and the one that runs
 * into running (if not NULL). When the service does not answer: the jobs
 * whose cgroups still have processes (a job outlives its service; running
 * then has only its id). */
int jobs_pending(struct jobs_row *running);
const char *jobs_state_label(const char *state); /* translated */

#endif
