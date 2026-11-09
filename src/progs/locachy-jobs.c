/*
 * locachy-jobs (/usr/local/lib/locachy/locachy-jobs): the system jobs'
 * queue (base/include/jobs.h, tools/JOBS.md), a service (rc.conf
 * service_jobs: process 1 starts it, and again 5 s after it ends), as root.
 *
 * It runs the jobs programs ask for on its control socket one at a time, in
 * the order they came: package transactions, kernel and repository changes,
 * the boot image, container builds, firmware. Only the kinds of the table
 * below, each a fixed program of the system with its own fixed arguments
 * and the job's, checked here; no shell. It runs in a cgroup of its own
 * (JOBS_CGROUP/service), each job in another (JOBS_CGROUP/job-ID): none of
 * them is a program's, so a program that ends, is killed with its cgroup or
 * is frozen leaves its jobs as they are; process 1 freezes no other cgroup.
 *
 * A job: a runner (a child of the service, in a session and the cgroup of
 * the job) runs its steps with their output in JOBS_DIR/ID.log, writes how
 * it ended in ID.exit and holds a lock on ID.run while it lives. The service
 * keeps every job's state in ID.state, so that when it starts again (it
 * ended, or was ended: SIGTERM leaves a running job alone) it finds the job
 * that runs by its lock, takes its result from ID.exit once its runner has
 * ended, and goes on with the queue. When a job has ended what it left in
 * its cgroup ends (cgroup.kill: gpg-agent, say).
 *
 * Stopping a job that runs (cancel ID confirm): SIGINT, as Ctrl+C gives it,
 * to what its step runs (stop_job: pacman stops between two packages and
 * releases its lock), then, if it still runs JOBS_STOP_GRACE s later,
 * everything in its cgroup is killed. A package transaction stopped either
 * way can leave the system between two states; the service answers
 * "confirm" first, and the programs say so before asking again.
 *
 * SIGTERM ends the service alone: a running job goes on, and the next start
 * watches it. A queued job waits for that start.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "docs-proto.h" /* docs_peer_kind: who a peer is, by its cgroup */
#include "jobs.h"

#ifndef JOBS_PROC
#define JOBS_PROC "/proc"
#endif
#ifndef JOBS_UID
#define JOBS_UID 0 /* the peers' and the service's */
#endif
#ifndef JOBS_BIN
#define JOBS_BIN "/usr/local/bin" /* the programs the jobs run */
#endif
#ifndef JOBS_UNTRUSTED_KEYS
#define JOBS_UNTRUSTED_KEYS "/var/lib/locachy/builder/untrusted-keys" /* locachy-build's */
#endif
#ifndef JOBS_STOP_GRACE
#define JOBS_STOP_GRACE 20
#endif
#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif
#define JOBS_KEEP 32      /* ended jobs kept, with their output */
#define JOBS_QUEUED_MAX 32
#define JOBS_MAX (JOBS_KEEP + JOBS_QUEUED_MAX + 2)
#define MAX_CONNS 16
#define ARGS_TEXT 4608    /* a job's arguments, NUL separated */
#define ROW_ARGS 400      /* of them in a ROW */
#define ROW_LAST 200

__attribute__((format(printf, 1, 2)))
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("locachy-jobs: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* --- The kinds and their arguments --- */

enum check {
    C_NONE, C_PACKAGES, C_SWAP, C_KERNEL, C_ONOFF, C_SECTION, C_TARGETS, C_CONTAINER, C_CONTAINER_DATA,
    C_DEVICE, C_RELEASE, C_FILE, C_SPI_PROBE, C_SPI_BACKUP, C_SPI_VERIFY, C_SPI_WRITE,
    C_TPM, C_TPM_ENROLL, C_SECURE_BOOT, C_BOOT, C_REBUILD,
};

struct kind {
    const char *name, *tool;  /* tool: JOBS_BIN/tool */
    const char *fixed[3];     /* its arguments before the job's */
    int check;
};

static const struct kind kinds[] = {
    { "pkg-install", "locachy-pkg", { "install" }, C_PACKAGES },
    { "pkg-remove", "locachy-pkg", { "remove" }, C_PACKAGES },
    { "pkg-swap", "locachy-pkg", { "remove" }, C_SWAP }, /* OLD NEW: remove OLD, then install NEW */
    { "pkg-sync", "locachy-pkg", { "sync" }, C_NONE },
    { "pkg-plan", "locachy-pkg", { "plan" }, C_NONE },
    { "pkg-apply", "locachy-pkg", { "apply" }, C_TARGETS },
    { "kernel-select", "locachy-pkg", { "kernel", "select" }, C_KERNEL },
    { "kernel-headers", "locachy-pkg", { "kernel", "headers" }, C_ONOFF },
    { "repos-rank", "locachy-repos", { "rank" }, C_NONE },
    { "repos-set", "locachy-repos", { "apply", "--set" }, C_SECTION },
    { "boot", "locachy-boot", { NULL }, C_BOOT },
    { "locachy-rebuild", "locachy-pkg", { "rebuild" }, C_REBUILD },
    { "tpm-pending", "locachy-tpm", { "enroll", "--pending" }, C_NONE },
    { "tpm-enroll", "locachy-tpm", { "enroll", "--passphrase-stdin" }, C_TPM_ENROLL },
    { "tpm-prepare", "locachy-tpm", { "prepare", "--passphrase-stdin" }, C_NONE },
    { "tpm-clear", "locachy-tpm", { "clear" }, C_TPM },
    { "secure-boot", "locachy-tpm", { "secure-boot" }, C_SECURE_BOOT },
    { "app-build", "locachy-app", { "build" }, C_CONTAINER },
    { "app-rollback", "locachy-app", { "rollback" }, C_CONTAINER },
    { "app-remove", "locachy-app", { "remove" }, C_CONTAINER_DATA },
    { "fw-refresh", "locachy-firmware", { "fwupd-action", "refresh" }, C_NONE },
    { "fw-update", "locachy-firmware", { "fwupd-action", "update" }, C_NONE },
    { "fw-verify", "locachy-firmware", { "fwupd-action", "verify" }, C_DEVICE },
    { "fw-unlock", "locachy-firmware", { "fwupd-action", "unlock" }, C_DEVICE },
    { "fw-local-install", "locachy-firmware", { "fwupd-action", "local-install" }, C_FILE },
    { "fw-install-release", "locachy-firmware", { "fwupd-action", "install-release" }, C_RELEASE },
    { "spi-probe", "locachy-firmware", { "spi-probe" }, C_SPI_PROBE },
    { "spi-backup", "locachy-firmware", { "spi-backup" }, C_SPI_BACKUP },
    { "spi-verify", "locachy-firmware", { "spi-verify" }, C_SPI_VERIFY },
    { "spi-write", "locachy-firmware", { "spi-write" }, C_SPI_WRITE },
};
#define NKINDS ((int)(sizeof kinds / sizeof kinds[0]))

static const struct kind *kind_named(const char *name)
{
    for (int i = 0; i < NKINDS; i++) {
        if (strcmp(kinds[i].name, name) == 0) return &kinds[i];
    }
    return NULL;
}

/* s: 1..max-1 bytes, each a letter, a digit or one of extra; the first not
 * '-' (no option for the program) nor '.'. */
static int word(const char *s, const char *extra, size_t max)
{
    size_t n = strlen(s);
    if (n == 0 || n >= max || s[0] == '-' || s[0] == '.') return 0;
    for (const char *p = s; *p; p++) {
        if (!isalnum((unsigned char)*p) && !strchr(extra, *p)) return 0;
    }
    return 1;
}

/* An absolute path of something there (dir: a folder, else a file), as
 * people name them: no control character. */
static int path_of(const char *s, int dir)
{
    struct stat st;
    if (s[0] != '/' || strlen(s) >= 4096) return 0;
    for (const char *p = s; *p; p++) {
        if ((unsigned char)*p < 32 || *p == 127) return 0;
    }
    return stat(s, &st) == 0 && (dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
}

static int container_name(const char *s)
{
    return isalnum((unsigned char)s[0]) && strncmp(s, "window-", 7) != 0 && word(s, "._-", 65);
}

/* flashrom's options after the positional arguments: --chip NAME,
 * --regions LIST (fd,bios,...), --expected-size BYTES, each once and only
 * those allowed (bits: 1 chip, 2 regions, 4 size). */
static int spi_options(char **a, int n, int allowed)
{
    int seen = 0;
    for (int i = 0; i < n; i += 2) {
        if (i + 1 >= n) return 0;
        int bit = strcmp(a[i], "--chip") == 0 ? 1 : strcmp(a[i], "--regions") == 0 ? 2 :
                  strcmp(a[i], "--expected-size") == 0 ? 4 : 0;
        if (!bit || !(allowed & bit) || (seen & bit)) return 0;
        seen |= bit;
        const char *v = a[i + 1];
        if (bit == 1 && !word(v, "_./()+, -", 97)) return 0; /* flashrom's chip names */
        if (bit == 2 && (strlen(v) >= 64 || !v[0] || strspn(v, "abcdefghijklmnopqrstuvwxyz0123456789,") != strlen(v) ||
                         v[0] == ',' || v[strlen(v) - 1] == ',' || strstr(v, ",,"))) return 0;
        if (bit == 4 && (strlen(v) > 12 || !v[0] || strspn(v, "0123456789") != strlen(v))) return 0;
    }
    return 1;
}

/* The job's arguments a (n of them) for kind k: 0, or -1 with why. */
static int check_args(const struct kind *k, char **a, int n, char *why, size_t size)
{
    int ok = 0;
    switch (k->check) {
    case C_NONE:
        ok = n == 0;
        break;
    case C_PACKAGES:
    case C_SWAP:
        ok = k->check == C_SWAP ? n == 2 : n >= 1 && n <= 32;
        for (int i = 0; ok && i < n; i++) ok = word(a[i], "@._+-", 128);
        break;
    case C_KERNEL:
        ok = n == 1 && strncmp(a[0], "linux", 5) == 0 && strlen(a[0]) < 64 &&
             strspn(a[0], "abcdefghijklmnopqrstuvwxyz0123456789+._-") == strlen(a[0]);
        break;
    case C_ONOFF:
        ok = n == 1 && (strcmp(a[0], "on") == 0 || strcmp(a[0], "off") == 0);
        break;
    case C_SECTION:
        if (n == 1) {
            static const char *const sections[] = { "core=on", "core=off", "extra=on", "extra=off",
                                                    "multilib=on", "multilib=off" };
            for (size_t i = 0; i < sizeof sections / sizeof *sections; i++) ok |= strcmp(a[0], sections[i]) == 0;
        }
        break;
    case C_TARGETS:
        ok = n <= JOBS_ARGS_MAX - 1;
        for (int i = 0; ok && i < n; i++) ok = container_name(a[i]);
        break;
    case C_CONTAINER:
        ok = n == 1 && container_name(a[0]);
        break;
    case C_CONTAINER_DATA:
        ok = (n == 1 || (n == 2 && strcmp(a[1], "--data") == 0)) && container_name(a[0]);
        break;
    case C_DEVICE:
        ok = n == 1 && isalnum((unsigned char)a[0][0]) && word(a[0], "._:-", 97);
        break;
    case C_RELEASE:
        ok = n == 2 && isalnum((unsigned char)a[0][0]) && word(a[0], "._:-", 97);
        if (ok) {
            static const char *const words[] = { "install", "upgrade", "downgrade", "reinstall", "update" };
            int known = 0;
            for (size_t i = 0; i < sizeof words / sizeof *words; i++) known |= strcmp(a[1], words[i]) == 0;
            int url = (strncmp(a[1], "https://", 8) == 0 || strncmp(a[1], "http://", 7) == 0) && strlen(a[1]) < 1024;
            for (const char *p = a[1]; url && *p; p++) url = (unsigned char)*p > 32 && *p != 127;
            ok = known || url;
        }
        break;
    case C_FILE:
        ok = n == 1 && path_of(a[0], 0);
        break;
    case C_BOOT:
        ok = n == 0 || (n == 1 && (strcmp(a[0], "--luks") == 0 || strcmp(a[0], "--register-only") == 0));
        break;
    case C_REBUILD:
        ok = n == 0 || (n == 1 && strcmp(a[0], "--force") == 0);
        break;
    case C_TPM_ENROLL:
        /* Only PCR numbers and absolute /dev paths; no worker/offline flags. */
        if (n >= 2 && strcmp(a[0], "--pcrs") == 0) {
            const char *p = a[1];
            if (strcmp(p, "none") != 0 && strcmp(p, "srk") != 0) {
                if (!p[0] || strlen(p) > 80) break;
                while (*p) {
                    char *end;
                    long v = strtol(p, &end, 10);
                    if (!isdigit((unsigned char)*p) || end == p || v > 23 || (*end && *end != '+' && *end != ',')) break;
                    p = *end ? end + 1 : end;
                    if (*end && !*p) { p = end; break; }
                }
                if (*p) break;
            }
            a += 2;
            n -= 2;
        }
        /* fall through */
    case C_TPM:
        ok = n <= 16;
        for (int i = 0; ok && i < n; i++) {
            ok = strncmp(a[i], "/dev/", 5) == 0 && strlen(a[i]) < 256 && !strstr(a[i], "..") &&
                 strspn(a[i], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_-.:") == strlen(a[i]);
        }
        break;
    case C_SECURE_BOOT: {
        int seen = 0;
        ok = n <= 3;
        for (int i = 0; ok && i < n; i++) {
            int bit = strcmp(a[i], "--enroll") == 0 ? 1 : strcmp(a[i], "--microsoft") == 0 ? 2 :
                      strcmp(a[i], "--passphrase-stdin") == 0 ? 4 : 0;
            ok = bit && !(seen & bit);
            seen |= bit;
        }
        ok = ok && (!(seen & 6) || (seen & 1));
        break;
    }
    case C_SPI_PROBE:
        ok = n >= 1 && word(a[0], "_:=,./-", 128) && spi_options(a + 1, n - 1, 1);
        break;
    case C_SPI_BACKUP:
    case C_SPI_VERIFY:
    case C_SPI_WRITE:
        ok = n >= 2 && word(a[0], "_:=,./-", 128) && path_of(a[1], k->check == C_SPI_BACKUP) &&
             spi_options(a + 2, n - 2, k->check == C_SPI_WRITE ? 7 : 3);
        break;
    }
    if (!ok) snprintf(why, size, "the arguments of %s are not valid", k->name);
    return ok ? 0 : -1;
}

/* --- The jobs --- */

struct job {
    int id, status, nargs;
    char kind[24], state[16], from[32];
    char args[ARGS_TEXT];  /* nargs arguments, NUL separated */
    char last[ROW_LAST];   /* its output's last line, once it has ended */
    pid_t runner;          /* 0: none */
    int adopted, pidfd;    /* a runner of the last run (not a child) and its pidfd (-1: none) */
    int input;            /* anonymous pipe, RAM only; -1 without input (never saved) */
    double stop_at;        /* now_s() when a stopping job is killed; 0 */
    long long submitted, started, ended;
};

static struct job jobs[JOBS_MAX];
static int njobs, next_id = 1;

static const char *const *job_args(const struct job *j, const char **out)
{
    const char *p = j->args;
    for (int i = 0; i < j->nargs; i++) {
        out[i] = p;
        p += strlen(p) + 1;
    }
    out[j->nargs] = NULL;
    return out;
}

static int needs_input(const struct job *j)
{
    if (strcmp(j->kind, "tpm-enroll") == 0 || strcmp(j->kind, "tpm-prepare") == 0) return 1;
    const char *a[JOBS_ARGS_MAX + 1];
    job_args(j, a);
    for (int i = 0; strcmp(j->kind, "secure-boot") == 0 && i < j->nargs; i++) {
        if (strcmp(a[i], "--passphrase-stdin") == 0) return 1;
    }
    return 0;
}

static void close_input(struct job *j)
{
    if (j->input >= 0) close(j->input);
    j->input = -1;
}

static void job_path(const struct job *j, const char *what, char *path, size_t size)
{
    snprintf(path, size, "%s/%d.%s", JOBS_DIR, j->id, what);
}

static void cgroup_path(const struct job *j, const char *file, char *path, size_t size)
{
    snprintf(path, size, "%s/job-%d%s%s", JOBS_CGROUP, j->id, file ? "/" : "", file ? file : "");
}

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? write(fd, text, strlen(text)) : -1;
    if (fd >= 0) close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int ended(const struct job *j)
{
    return strcmp(j->state, "done") == 0 || strcmp(j->state, "failed") == 0 || strcmp(j->state, "cancelled") == 0 ||
           strcmp(j->state, "keys") == 0;
}

static int active(const struct job *j)
{
    return strcmp(j->state, "running") == 0 || strcmp(j->state, "stopping") == 0;
}

/* ID.state, whole or not at all. */
static void save(const struct job *j)
{
    char path[512], tmp[520];
    job_path(j, "state", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f) {
        say("cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "kind=%s\nstate=%s\nstatus=%d\nfrom=%s\nrunner=%d\nsubmitted=%lld\nstarted=%lld\nended=%lld\n",
            j->kind, j->state, j->status, j->from, (int)j->runner, j->submitted, j->started, j->ended);
    const char *a[JOBS_ARGS_MAX + 1];
    job_args(j, a);
    for (int i = 0; i < j->nargs; i++) fprintf(f, "arg=%s\n", a[i]);
    if (j->last[0]) fprintf(f, "last=%s\n", j->last);
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        say("cannot write %s: %s", path, strerror(errno));
        unlink(tmp);
    }
}

/* How many jobs are before j (0: it runs); -1 once it has ended. */
static int position(const struct job *j)
{
    if (ended(j)) return -1;
    if (active(j)) return 0;
    int n = 0;
    for (int i = 0; i < njobs; i++) {
        n += !ended(&jobs[i]) && (active(&jobs[i]) || jobs[i].id < j->id);
    }
    return n;
}

/* text into out (size bytes), cut on a character's start if it is longer. */
static void copy_cut(char *out, size_t size, const char *text)
{
    size_t n = strlen(text);
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, text, n);
    out[n] = '\0';
}

/* The end of j's output: from a line's start, at most size - 1 bytes. */
static void log_tail(const struct job *j, char *out, size_t size)
{
    char path[512];
    job_path(j, "log", path, sizeof path);
    out[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat st;
    off_t from = fstat(fd, &st) == 0 && st.st_size > (off_t)size - 1 ? st.st_size - (off_t)size + 1 : 0;
    ssize_t n = pread(fd, out, size - 1, from);
    close(fd);
    if (n <= 0) return;
    out[n] = '\0';
    for (ssize_t i = 0; i < n; i++) {
        if (out[i] == '\0') out[i] = ' ';
    }
    if (from > 0) {
        char *nl = strchr(out, '\n');
        memmove(out, nl ? nl + 1 : out + n, strlen(nl ? nl + 1 : out + n) + 1);
    }
}

/* Its last line (a line rewritten with \r: its last form), for a ROW. */
static void last_line(const struct job *j, char *out, size_t size)
{
    char tail[2048];
    log_tail(j, tail, sizeof tail);
    char *end = tail + strlen(tail);
    while (end > tail && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = '\0';
    char *line = strrchr(tail, '\n');
    line = line ? line + 1 : tail;
    char *cr = strrchr(line, '\r');
    if (cr) line = cr + 1;
    copy_cut(out, size, line);
    for (char *p = out; *p; p++) {
        if ((unsigned char)*p < 32 || *p == 127) *p = ' ';
    }
}

/* A ROW (jobs.h). */
static void row(const struct job *j, char *out, size_t size)
{
    char all[ARGS_TEXT], args[ROW_ARGS], last[ROW_LAST];
    const char *a[JOBS_ARGS_MAX + 1];
    job_args(j, a);
    size_t len = 0;
    all[0] = '\0';
    for (int i = 0; i < j->nargs && len < sizeof all; i++) {
        len += (size_t)snprintf(all + len, sizeof all - len, "%s%s", i ? " " : "", a[i]);
    }
    copy_cut(args, sizeof args, all);
    if (ended(j)) snprintf(last, sizeof last, "%s", j->last);
    else last_line(j, last, sizeof last);
    snprintf(out, size, "%d\t%s\t%s\t%d\t%d\t%s\t%s\t%s", j->id, j->kind, j->state, j->status, position(j), j->from,
             args, last);
}

/* pid's parent, from /proc/PID/stat; 0 if it has gone. */
static pid_t parent_of(long pid)
{
    char path[64], text[512];
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, text, sizeof text - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) return 0;
    text[n] = '\0';
    char *end = strrchr(text, ')'); /* after the name, which may hold anything */
    int ppid = 0;
    return end && sscanf(end + 1, " %*c %d", &ppid) == 1 ? (pid_t)ppid : 0;
}

/* A stop: SIGINT, as Ctrl+C gives it, to what the job's step runs (pacman,
 * fwupdmgr, flashrom...), so that it stops at a point of its own (pacman:
 * between two packages, its lock released) and the step sees it fail and
 * ends by itself; to the step only when it runs nothing. Not to both: a
 * Python tool interrupted while it waits for pacman kills it 0.25 s later
 * (subprocess). Its cgroup lists them all, whatever their session. */
static void stop_job(const struct job *j)
{
    char path[512];
    long pids[256], pid;
    int n = 0, others = 0;
    pid_t step = 0;
    cgroup_path(j, "cgroup.procs", path, sizeof path);
    FILE *in = fopen(path, "re");
    while (in && n < 256 && fscanf(in, "%ld", &pid) == 1) {
        if (pid <= 1 || pid == (long)j->runner || pid == (long)getpid()) continue;
        if (parent_of(pid) == j->runner) step = (pid_t)pid;
        pids[n++] = pid;
    }
    if (in) fclose(in);
    for (int i = 0; i < n; i++) {
        if (pids[i] != (long)step && kill((pid_t)pids[i], SIGINT) == 0) others++;
    }
    if (!others && step > 0) {
        kill(step, SIGINT);
    } else if (!others && j->runner > 0) {
        kill(-j->runner, SIGINT); /* no list: its runner's group (the runner ignores it) */
    }
    say("job %d (%s): interrupted %d process%s%s", j->id, j->kind, others, others == 1 ? "" : "es",
        others ? "" : " (its step itself)");
}

/* What an ended job left in its cgroup ends; the cgroup goes. */
static void cgroup_end(const struct job *j)
{
    char path[512];
    cgroup_path(j, "cgroup.kill", path, sizeof path);
    write_text(path, "1");
    cgroup_path(j, "cgroup.events", path, sizeof path);
    for (int i = 0; i < 100; i++) {
        char line[64];
        int populated = 0;
        FILE *in = fopen(path, "re");
        while (in && fgets(line, sizeof line, in)) {
            if (strncmp(line, "populated ", 10) == 0) populated = atoi(line + 10);
        }
        if (in) fclose(in);
        if (!populated) break;
        usleep(10000);
    }
    cgroup_path(j, NULL, path, sizeof path);
    rmdir(path);
}

/* Forget the oldest ended jobs beyond JOBS_KEEP, their files with them. */
static void prune(void)
{
    int count = 0;
    for (int i = 0; i < njobs; i++) count += ended(&jobs[i]);
    for (int i = 0; i < njobs && count > JOBS_KEEP;) {
        if (!ended(&jobs[i])) {
            i++;
            continue;
        }
        static const char *const files[] = { "state", "log", "exit", "run" };
        for (size_t k = 0; k < sizeof files / sizeof *files; k++) {
            char path[512];
            job_path(&jobs[i], files[k], path, sizeof path);
            unlink(path);
        }
        memmove(&jobs[i], &jobs[i + 1], (size_t)(njobs - i - 1) * sizeof *jobs);
        njobs--;
        count--;
    }
}

/* The status ID.exit has, or -1. */
static int exit_status(const struct job *j)
{
    char path[512];
    job_path(j, "exit", path, sizeof path);
    FILE *in = fopen(path, "re");
    int status = -1;
    if (in && fscanf(in, "%d", &status) != 1) status = -1;
    if (in) fclose(in);
    return status;
}

static void run_next(void);

/* j's runner has ended with status. */
/* A job that builds (locachy-pkg's, locachy-app's: locachy-build) and
 * failed stopped for source signing keys a person has not trusted yet:
 * locachy-build exits 3, and its handover JOBS_UNTRUSTED_KEYS lists them,
 * written while the job ran (locachy-pkg says only that it failed). */
static int stopped_for_keys(const struct job *j, int status)
{
    const struct kind *k = kind_named(j->kind);
    struct stat st;
    if (status == 0 || !k || (strcmp(k->tool, "locachy-pkg") != 0 && strcmp(k->tool, "locachy-app") != 0)) return 0;
    return status == 3 || (stat(JOBS_UNTRUSTED_KEYS, &st) == 0 && st.st_size > 0 && (long long)st.st_mtime >= j->started);
}

static void finish(struct job *j, int status)
{
    close_input(j);
    snprintf(j->state, sizeof j->state, "%s", strcmp(j->state, "stopping") == 0 ? "cancelled" :
             status == 0 ? "done" : stopped_for_keys(j, status) ? "keys" : "failed");
    j->status = status;
    j->runner = 0;
    j->stop_at = 0;
    j->adopted = 0;
    j->ended = (long long)time(NULL);
    if (j->pidfd >= 0) close(j->pidfd);
    j->pidfd = -1;
    cgroup_end(j);
    last_line(j, j->last, sizeof j->last);
    char path[512];
    job_path(j, "run", path, sizeof path);
    unlink(path);
    job_path(j, "exit", path, sizeof path);
    unlink(path);
    save(j);
    say("job %d (%s) %s, status %d", j->id, j->kind, j->state, status);
    prune();
    run_next();
}

/* --- The runner: a child of the service, in the job's session and cgroup --- */

static int step_count(const struct job *j)
{
    return strcmp(j->kind, "pkg-swap") == 0 ? 2 : 1;
}

/* The command line of j's step (static: one at a time). */
static char **step_argv(const struct job *j, int step)
{
    static char tool[256];
    static char *argv[JOBS_ARGS_MAX + 8];
    const struct kind *k = kind_named(j->kind);
    const char *a[JOBS_ARGS_MAX + 1];
    job_args(j, a);
    int n = 0;
    snprintf(tool, sizeof tool, "%s/%s", JOBS_BIN, k->tool);
    argv[n++] = tool;
    if (k->check == C_SWAP) {
        argv[n++] = step == 0 ? "remove" : "install";
        argv[n++] = (char *)a[step];
    } else {
        for (int i = 0; i < 3 && k->fixed[i]; i++) argv[n++] = (char *)k->fixed[i];
        for (int i = 0; i < j->nargs; i++) argv[n++] = (char *)a[i];
    }
    argv[n] = NULL;
    return argv;
}

static void dprint_log(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void dprint_log(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vdprintf(fd, fmt, ap);
    va_end(ap);
}

static void runner(const struct job *j, int lock)
{
    for (int fd = 3; fd < 1024; fd++) {
        if (fd != lock && fd != j->input) close(fd); /* the service's sockets and other jobs' input */
    }
    setsid();
    signal(SIGINT, SIG_IGN); /* a stop's SIGINT is for its steps; it says how they ended */
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    char path[512];
    job_path(j, "log", path, sizeof path);
    int log = open(path, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    cgroup_path(j, "cgroup.procs", path, sizeof path);
    if (write_text(path, "0") < 0) {
        if (log >= 0) dprint_log(log, "locachy-jobs: cannot enter %s: %s; not run\n", path, strerror(errno));
        _exit(125); /* never outside its cgroup */
    }
    int status = 0;
    for (int step = 0; step < step_count(j) && status == 0; step++) {
        char **argv = step_argv(j, step);
        pid_t pid = fork();
        if (pid == 0) {
            int input = j->input >= 0 ? j->input : open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (input < 0 || dup2(input, STDIN_FILENO) < 0) _exit(126);
            if (input != STDIN_FILENO) close(input);
            if (log >= 0) {
                dup2(log, STDOUT_FILENO);
                dup2(log, STDERR_FILENO);
            }
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            /* The steps write system files that others read (mirror lists,
             * pacman.conf, rc.conf, packages): a root shell's umask, not the
             * service's own. */
            umask(022);
            unsetenv("LOCACHY_CONTROL");
            execv(argv[0], argv);
            dprintf(STDERR_FILENO, "locachy-jobs: cannot run %s: %s\n", argv[0], strerror(errno));
            _exit(127);
        }
        int st = 0;
        while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
        status = pid < 0 ? 126 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    char tmp[520]; /* how it ended: in its state, not in its output (whose last line people see) */
    job_path(j, "exit", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (f && fprintf(f, "%d\n", status) > 0 && fclose(f) == 0) {
        rename(tmp, path);
    } else if (f) {
        fclose(f);
    }
    _exit(status & 255);
}

/* The first queued job starts when none runs. */
static int restoring;

static void run_next(void)
{
    struct job *j = NULL;
    if (restoring) return;
    for (int i = 0; i < njobs; i++) {
        if (active(&jobs[i])) return;
        if (!j && strcmp(jobs[i].state, "queued") == 0) j = &jobs[i];
    }
    if (!j) return;
    char path[512];
    cgroup_path(j, NULL, path, sizeof path);
    if (mkdir(path, 0755) < 0 && errno != EEXIST) say("cannot make %s: %s", path, strerror(errno));
    job_path(j, "log", path, sizeof path);
    int log = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (log >= 0) {
        const char *a[JOBS_ARGS_MAX + 1];
        job_args(j, a);
        dprint_log(log, "locachy-jobs: job %d: %s", j->id, j->kind);
        for (int i = 0; i < j->nargs; i++) dprint_log(log, " %s", a[i]);
        dprint_log(log, " (from %s)\n", j->from);
        close(log);
    }
    job_path(j, "run", path, sizeof path);
    int lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        say("cannot lock %s: %s", path, strerror(errno));
        if (lock >= 0) close(lock);
        snprintf(j->state, sizeof j->state, "running");
        finish(j, 126);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) runner(j, lock);
    if (pid < 0) {
        close(lock);
        close_input(j);
        say("cannot start job %d: %s", j->id, strerror(errno));
        snprintf(j->state, sizeof j->state, "running");
        finish(j, 126);
        return;
    }
    close(lock); /* the runner holds it now */
    close_input(j); /* only the runner keeps this job's input */
    j->runner = pid;
    j->started = (long long)time(NULL);
    snprintf(j->state, sizeof j->state, "running");
    save(j);
    say("job %d (%s) runs (runner %d)", j->id, j->kind, (int)pid);
}

/* --- Starting again: the jobs of the last run --- */

static int lock_held(const struct job *j)
{
    char path[512];
    job_path(j, "run", path, sizeof path);
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    int held = flock(fd, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK;
    close(fd);
    return held;
}

static int by_id(const void *a, const void *b)
{
    return ((const struct job *)a)->id - ((const struct job *)b)->id;
}

static void restore(void)
{
    DIR *d = opendir(JOBS_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && njobs < JOBS_MAX) {
        int id = 0, len = 0;
        if (sscanf(e->d_name, "%d.state%n", &id, &len) != 1 || id <= 0 || e->d_name[len] != '\0') continue;
        char path[512], line[ARGS_TEXT + 16];
        snprintf(path, sizeof path, "%s/%s", JOBS_DIR, e->d_name);
        FILE *in = fopen(path, "re");
        if (!in) continue;
        struct job *j = &jobs[njobs];
        memset(j, 0, sizeof *j);
        j->id = id;
        j->pidfd = -1;
        j->input = -1;
        size_t used = 0;
        while (fgets(line, sizeof line, in)) {
            line[strcspn(line, "\n")] = '\0';
            char *v = strchr(line, '=');
            if (!v) continue;
            *v++ = '\0';
            if (strcmp(line, "kind") == 0) snprintf(j->kind, sizeof j->kind, "%s", v);
            else if (strcmp(line, "state") == 0) snprintf(j->state, sizeof j->state, "%s", v);
            else if (strcmp(line, "status") == 0) j->status = atoi(v);
            else if (strcmp(line, "from") == 0) snprintf(j->from, sizeof j->from, "%s", v);
            else if (strcmp(line, "runner") == 0) j->runner = (pid_t)atoi(v);
            else if (strcmp(line, "submitted") == 0) j->submitted = atoll(v);
            else if (strcmp(line, "started") == 0) j->started = atoll(v);
            else if (strcmp(line, "ended") == 0) j->ended = atoll(v);
            else if (strcmp(line, "last") == 0) snprintf(j->last, sizeof j->last, "%s", v);
            else if (strcmp(line, "arg") == 0 && j->nargs < JOBS_ARGS_MAX) {
                size_t n = strlen(v) + 1;
                if (used >= sizeof j->args || n > sizeof j->args - used) continue;
                memcpy(&j->args[used], v, n);
                used += n;
                j->nargs++;
            }
        }
        fclose(in);
        if (id >= next_id) next_id = id + 1;
        const struct kind *k = kind_named(j->kind);
        const char *a[JOBS_ARGS_MAX + 1];
        char why[128];
        if (!k || (!active(j) && !ended(j) && strcmp(j->state, "queued") != 0)) continue;
        njobs++;
        if (strcmp(j->state, "queued") == 0 &&
            (check_args(k, (char **)job_args(j, a), j->nargs, why, sizeof why) < 0 || needs_input(j))) {
            snprintf(j->state, sizeof j->state, "failed"); /* invalid or RAM-only input lost: never run */
            j->status = 125;
            snprintf(j->last, sizeof j->last, "not run after service restart: input unavailable or arguments invalid; submit again");
            j->ended = (long long)time(NULL);
            save(j);
        }
    }
    if (d) closedir(d);
    qsort(jobs, (size_t)njobs, sizeof *jobs, by_id);
    restoring = 1; /* the queue goes on once every job is known */
    for (int i = 0; i < njobs; i++) {
        struct job *j = &jobs[i];
        if (!active(j) || j->adopted) continue;
        if (j->runner > 0 && lock_held(j)) {
            /* Its runner, by the lock it holds; looked at again once the
             * pidfd is open, should it have ended in between. */
            j->adopted = 1;
            j->pidfd = (int)syscall(SYS_pidfd_open, j->runner, 0);
            if (lock_held(j)) {
                say("job %d (%s) still runs (runner %d): watched", j->id, j->kind, (int)j->runner);
                if (strcmp(j->state, "stopping") == 0) j->stop_at = now_s() + JOBS_STOP_GRACE;
                continue;
            }
        }
        say("job %d (%s) ended while the service did not run", j->id, j->kind);
        finish(j, exit_status(j));
        i = -1; /* finish may have moved the jobs */
    }
    restoring = 0;
}

/* --- Requests --- */

struct conn {
    int fd;
    char from[32];
};
static struct conn conns[MAX_CONNS];
static int nconns;

/* A peer: root (JOBS_UID), not a container's process, still the process that
 * connected; its name into from. */
static int peer(int fd, char *from, size_t size)
{
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || cred.pid <= 0) return -1;
    if (cred.uid != JOBS_UID) {
        say("pid %d (uid %u) refused: not root", (int)cred.pid, (unsigned)cred.uid);
        return -1;
    }
    int pidfd = -1;
    len = sizeof pidfd;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &len) < 0) pidfd = -1;
    char path[256], line[512] = "";
    snprintf(path, sizeof path, "%s/%d/cgroup", JOBS_PROC, (int)cred.pid);
    FILE *in = fopen(path, "re");
    if (!in || !fgets(line, sizeof line, in)) line[0] = '\0';
    if (in) fclose(in);
    line[strcspn(line, "\n")] = '\0';
    snprintf(path, sizeof path, "/proc/%d/comm", (int)cred.pid);
    in = fopen(path, "re");
    if (!in || !fgets(from, (int)size, in)) snprintf(from, size, "?");
    if (in) fclose(in);
    from[strcspn(from, "\n")] = '\0';
    for (char *p = from; *p; p++) {
        if ((unsigned char)*p < 32 || *p == 127 || *p == '=') *p = '_';
    }
    int alive = pidfd < 0 || syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0) == 0 || errno == EPERM;
    if (pidfd >= 0) close(pidfd);
    int kind = docs_peer_kind(line);
    if (!alive || kind == DOCS_PEER_CONTAINER || kind == DOCS_PEER_UNKNOWN) {
        say("pid %d (%s) refused: %s", (int)cred.pid, from, alive ? line : "gone");
        return -1;
    }
    return 0;
}

static struct job *job_of(const char *id)
{
    if (!id[0] || strspn(id, "0123456789") != strlen(id) || strlen(id) > 9) return NULL;
    for (int i = 0; i < njobs; i++) {
        if (jobs[i].id == atoi(id)) return &jobs[i];
    }
    return NULL;
}

static char reply[JOBS_MSG_MAX];
static size_t reply_len;

static void put(const char *field)
{
    size_t n = strlen(field) + 1;
    if (reply_len + n > sizeof reply) return;
    memcpy(reply + reply_len, field, n);
    reply_len += n;
}

static void answer(const char *a, const char *b)
{
    reply_len = 0;
    put(a);
    if (b) put(b);
}

/* Input must already be complete: a bounded anonymous read pipe with no writer.
 * Check with non-consuming poll/FIONREAD; never block the service on a peer. */
static int input_ready(int fd)
{
    struct stat st;
    struct statfs fs;
    int bytes = 0, flags = fcntl(fd, F_GETFL);
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return flags >= 0 && (flags & O_ACCMODE) == O_RDONLY && fstat(fd, &st) == 0 &&
           S_ISFIFO(st.st_mode) && fstatfs(fd, &fs) == 0 &&
           fs.f_type == PIPEFS_MAGIC && ioctl(fd, FIONREAD, &bytes) == 0 &&
           bytes > 0 && bytes <= JOBS_INPUT_MAX && poll(&p, 1, 0) == 1 &&
           (p.revents & POLLHUP);
}

/* Takes input only on success; caller closes any descriptor not taken. */
static void submit(char **f, int n, const char *from, int *input)
{
    const struct kind *k = kind_named(f[1]);
    char why[160], text[ARGS_TEXT];
    size_t used = 0;
    if (!k) {
        answer("error", "no such kind of job");
        return;
    }
    if (n - 2 > JOBS_ARGS_MAX - 1 || check_args(k, f + 2, n - 2, why, sizeof why) < 0) {
        answer("error", n - 2 > JOBS_ARGS_MAX - 1 ? "too many arguments" : why);
        return;
    }
    for (int i = 2; i < n; i++) {
        size_t len = strlen(f[i]) + 1;
        if (used + len > sizeof text) {
            answer("error", "the arguments are too long");
            return;
        }
        memcpy(text + used, f[i], len);
        used += len;
    }
    struct job candidate = { .nargs = n - 2, .input = -1 };
    snprintf(candidate.kind, sizeof candidate.kind, "%s", k->name);
    memcpy(candidate.args, text, used);
    if ((needs_input(&candidate) && (*input < 0 || !input_ready(*input))) ||
        (!needs_input(&candidate) && *input >= 0)) {
        answer("error", "this kind needs exactly its complete RAM-only input pipe, or takes no input");
        return;
    }
    struct job *j = NULL;
    int queued = 0;
    for (int i = 0; i < njobs; i++) queued += strcmp(jobs[i].state, "queued") == 0;
    { /* Every accepted request is its own event, including repeated requests. */
        if (queued >= JOBS_QUEUED_MAX) {
            answer("error", "the queue is full");
            return;
        }
        if (njobs == JOBS_MAX) prune();
        if (njobs == JOBS_MAX) {
            answer("error", "too many jobs");
            return;
        }
        j = &jobs[njobs++];
        memset(j, 0, sizeof *j);
        j->id = next_id++;
        j->pidfd = -1;
        j->input = *input;
        *input = -1;
        j->status = -1;
        j->nargs = n - 2;
        j->submitted = (long long)time(NULL);
        memcpy(j->args, text, used);
        snprintf(j->kind, sizeof j->kind, "%s", k->name);
        snprintf(j->state, sizeof j->state, "queued");
        snprintf(j->from, sizeof j->from, "%s", from);
        save(j);
        say("job %d (%s) queued by %s", j->id, j->kind, from);
        int id = j->id;
        run_next(); /* may move jobs: by id from here */
        for (int i = 0; i < njobs; i++) {
            if (jobs[i].id == id) j = &jobs[i];
        }
    }
    char id[16], pos[16];
    snprintf(id, sizeof id, "%d", j->id);
    snprintf(pos, sizeof pos, "%d", position(j));
    answer("ok", id);
    put(pos);
}

static void cancel(struct job *j, int confirm)
{
    char why[160];
    if (ended(j)) {
        answer("ok", "ended");
    } else if (strcmp(j->state, "queued") == 0) {
        close_input(j);
        snprintf(j->state, sizeof j->state, "cancelled");
        j->ended = (long long)time(NULL);
        save(j);
        say("job %d (%s) removed from the queue", j->id, j->kind);
        prune();
        answer("ok", "removed");
    } else if (!confirm) {
        snprintf(why, sizeof why, "job %d runs: stopping it can leave the system half-changed; cancel %d confirm",
                 j->id, j->id);
        answer("confirm", why);
    } else {
        if (strcmp(j->state, "stopping") != 0) {
            snprintf(j->state, sizeof j->state, "stopping");
            j->stop_at = now_s() + JOBS_STOP_GRACE;
            save(j);
            say("job %d (%s) stopping", j->id, j->kind);
            stop_job(j);
        }
        answer("ok", "stopping");
    }
}

static void request(struct conn *c)
{
    static char msg[JOBS_MSG_MAX + 1];
    struct iovec iov = { .iov_base = msg, .iov_len = sizeof msg - 1 };
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int) * 8)]; } control;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                       .msg_control = control.bytes, .msg_controllen = sizeof control.bytes };
    ssize_t len = recvmsg(c->fd, &m, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    int input = -1, nfds = 0;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); len >= 0 && h; h = CMSG_NXTHDR(&m, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS || h->cmsg_len < CMSG_LEN(0)) continue;
        size_t count = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        int *fds = (int *)CMSG_DATA(h);
        for (size_t i = 0; i < count; i++) {
            if (++nfds == 1) input = fds[i];
            else close(fds[i]);
        }
    }
    if (len <= 0) {
        if (input >= 0) close(input);
        if (len == 0 || (errno != EAGAIN && errno != EINTR)) {
            close(c->fd);
            *c = conns[--nconns];
        }
        return;
    }
    msg[len] = '\0';
    char *f[JOBS_ARGS_MAX + 4];
    int n = 0;
    for (ssize_t i = 0; i < len && n < (int)(sizeof f / sizeof *f); i += (ssize_t)strlen(msg + i) + 1) {
        f[n++] = msg + i;
    }
    struct job *j = n >= 2 ? job_of(f[1]) : NULL;
    if ((m.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || nfds > 1 || n == 0 ||
        (len > 0 && msg[len - 1] == '\0') || (input >= 0 && strcmp(f[0], "submit") != 0)) {
        answer("error", "malformed request");
    } else if (strcmp(f[0], "submit") == 0 && n >= 2) {
        submit(f, n, c->from, &input);
    } else if (strcmp(f[0], "list") == 0 && n == 1) {
        static char text[2048];
        answer("ok", NULL);
        for (int i = 0; i < njobs; i++) {
            row(&jobs[i], text, sizeof text);
            put(text);
        }
    } else if ((strcmp(f[0], "status") == 0 || strcmp(f[0], "log") == 0) && n == 2) {
        static char text[JOBS_LOG_TAIL];
        if (!j) {
            answer("error", "no such job");
        } else if (f[0][0] == 's') {
            row(j, text, sizeof text);
            answer("ok", text);
        } else {
            log_tail(j, text, sizeof text);
            answer("ok", text);
        }
    } else if (strcmp(f[0], "cancel") == 0 && (n == 2 || (n == 3 && strcmp(f[2], "confirm") == 0))) {
        if (!j) answer("error", "no such job");
        else cancel(j, n == 3);
    } else {
        answer("error", "unknown request");
    }
    if (input >= 0) close(input);
    if (send(c->fd, reply, reply_len ? reply_len - 1 : 0, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
        close(c->fd);
        *c = conns[--nconns];
    }
}

static int listen_control(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", JOBS_CONTROL);
    unlink(JOBS_CONTROL);
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0 || chmod(JOBS_CONTROL, 0600) < 0 || listen(s, 16) < 0) {
        say("cannot listen on %s: %s", JOBS_CONTROL, strerror(errno));
        if (s >= 0) close(s);
        return -1;
    }
    return s;
}

/* JOBS_DIR: root's, 0700, not a link; its parent made if missing. */
static int jobs_dir(void)
{
    char parent[sizeof JOBS_DIR];
    snprintf(parent, sizeof parent, "%s", JOBS_DIR);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        if (mkdir(parent, 0755) == 0) chmod(parent, 0755); /* not the service's umask */
    }
    if (mkdir(JOBS_DIR, 0700) < 0 && errno != EEXIST) return -1;
    int fd = open(JOBS_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    int ok = fd >= 0 && fstat(fd, &st) == 0 && st.st_uid == geteuid() && fchmod(fd, 0700) == 0;
    if (fd >= 0) close(fd);
    return ok ? 0 : -1;
}

/* JOBS_CGROUP/service, entered. */
static int cgroup_enter(void)
{
    char path[512];
    if (mkdir(JOBS_CGROUP, 0755) < 0 && errno != EEXIST) return -1;
    snprintf(path, sizeof path, "%s/service", JOBS_CGROUP);
    if (mkdir(path, 0755) < 0 && errno != EEXIST) return -1;
    snprintf(path, sizeof path, "%s/service/cgroup.procs", JOBS_CGROUP);
    return write_text(path, "0");
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "usage: locachy-jobs (the service: rc.conf service_jobs)\n");
        return 2;
    }
    umask(077);
    if (geteuid() != JOBS_UID) {
        say("the service runs as root");
        return 2;
    }
    if (jobs_dir() < 0) {
        say("%s is not a folder of root's", JOBS_DIR);
        return 1;
    }
    if (cgroup_enter() < 0) {
        say("cannot enter %s/service: %s", JOBS_CGROUP, strerror(errno));
        return 1;
    }
    sigset_t handled;
    sigemptyset(&handled);
    sigaddset(&handled, SIGCHLD);
    sigaddset(&handled, SIGTERM);
    sigaddset(&handled, SIGINT);
    sigaddset(&handled, SIGHUP);
    sigprocmask(SIG_BLOCK, &handled, NULL);
    signal(SIGPIPE, SIG_IGN);
    int signals = signalfd(-1, &handled, SFD_CLOEXEC | SFD_NONBLOCK);
    restore();
    int listener = listen_control();
    if (signals < 0 || listener < 0) return 1;
    run_next();
    say("ready: %d jobs kept", njobs);

    for (;;) {
        struct pollfd p[2 + MAX_CONNS + JOBS_MAX];
        int watched[JOBS_MAX]; /* ids: finish moves the jobs */
        int n = 0, nwatched = 0;
        p[n++] = (struct pollfd){ .fd = signals, .events = POLLIN };
        p[n++] = (struct pollfd){ .fd = listener, .events = POLLIN };
        for (int i = 0; i < nconns; i++) p[n++] = (struct pollfd){ .fd = conns[i].fd, .events = POLLIN };
        int timeout = -1;
        for (int i = 0; i < njobs; i++) {
            struct job *j = &jobs[i];
            if (j->adopted && j->pidfd >= 0) {
                watched[nwatched++] = j->id;
                p[n++] = (struct pollfd){ .fd = j->pidfd, .events = POLLIN };
            } else if (j->adopted && (timeout < 0 || timeout > 1000)) {
                timeout = 1000; /* no pidfd: its lock looked at once a second */
            }
            if (j->stop_at > 0) {
                int left = (int)((j->stop_at - now_s()) * 1000) + 1;
                if (timeout < 0 || left < timeout) timeout = left < 0 ? 0 : left;
            }
        }
        if (poll(p, (nfds_t)n, timeout) < 0 && errno != EINTR) {
            say("poll: %s", strerror(errno));
            return 1;
        }
        if (p[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            int stop = 0;
            while (read(signals, &si, sizeof si) == sizeof si) {
                stop |= si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT;
            }
            int status;
            pid_t pid;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                for (int i = 0; i < njobs; i++) {
                    if (jobs[i].runner == pid && !jobs[i].adopted) {
                        finish(&jobs[i], WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
                        break;
                    }
                }
            }
            if (stop) {
                /* A running job goes on: its runner is in a session and a
                 * cgroup of its own, and the next start watches it; the
                 * queue waits for that start. */
                int pending = 0;
                for (int i = 0; i < njobs; i++) pending += !ended(&jobs[i]);
                say("ending; %d job%s not ended", pending, pending == 1 ? "" : "s");
                close(listener);
                unlink(JOBS_CONTROL);
                return 0;
            }
        }
        for (int i = 0; i < nwatched; i++) {
            if (!(p[2 + nconns + i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            for (int k = 0; k < njobs; k++) {
                struct job *j = &jobs[k];
                if (j->id != watched[i] || !j->adopted || j->pidfd < 0) continue;
                close(j->pidfd); /* it has ended; the lock says when its pid was another's */
                j->pidfd = -1;
            }
        }
        for (int i = 0; i < njobs; i++) {
            struct job *j = &jobs[i];
            if (j->adopted && j->pidfd < 0 && !lock_held(j)) {
                finish(j, exit_status(j));
                i = -1; /* finish moves the jobs */
                continue;
            }
            if (j->stop_at > 0 && now_s() >= j->stop_at) {
                char path[512];
                say("job %d (%s) still runs %d s after its stop: killed", j->id, j->kind, JOBS_STOP_GRACE);
                j->stop_at = 0;
                cgroup_path(j, "cgroup.kill", path, sizeof path);
                write_text(path, "1");
                if (j->runner > 0) kill(-j->runner, SIGKILL);
            }
        }
        for (int i = nconns - 1; i >= 0; i--) {
            if (p[2 + i].revents & (POLLIN | POLLHUP | POLLERR)) request(&conns[i]);
        }
        if (p[1].revents & POLLIN) {
            int s = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            char from[32];
            if (s >= 0 && (nconns == MAX_CONNS || peer(s, from, sizeof from) < 0)) {
                close(s);
            } else if (s >= 0) {
                conns[nconns].fd = s;
                snprintf(conns[nconns].from, sizeof conns[nconns].from, "%s", from);
                nconns++;
            }
        }
    }
}
