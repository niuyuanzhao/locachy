/*
 * packages: installs and updates of the host and the containers, shown before
 * they are done. The work is locachy-pkg's, locachy-app's and
 * locachy-build's; this shows what they would do and starts them. Three
 * pages, Tab between them:
 *   Updates     the last plan (locachy-pkg plan): a row for the host, for
 *               LOCachy and for each container, with what an update would
 *               do there; c checks again (on the network), Space marks, a
 *               applies the marked ones, Enter shows a row's packages
 *   Containers  their configurations (/etc/locachy/containers) and builds:
 *               n new, Enter edits one: its form, a line for each thing it
 *               is made of (its package and extras, as the repositories or
 *               the AUR have them; its command, from what its packages
 *               bring once built; its arguments, network and directories);
 *               b build, r roll back, d remove, l output. Each row: what
 *               its build installed over the host's (its upper's pacman
 *               database), how many packages, how much; its details list
 *               them, each one's version and size (PgUp/PgDn scroll them
 *               when they do not fit)
 *   Host        its manifest (/etc/locachy/packages), each package's version
 *               and size as installed: i adds a package, x removes the
 *               selected one, s brings the system to it
 * Above the lists, what the host has installed in all (how many packages,
 * how much, declared, with what they need, foreign; drift and missing).
 * What takes time runs as a job, one at a time, out of this program's task
 * (in the root cgroup: it goes on while home is on the screen, or after this
 * program has ended, and is never frozen); its output is shown in the lower
 * pane (a background of its own, under a title bar: what runs, how it ended,
 * which lines; PgUp/PgDn scroll it) and kept in JOB_LOG.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base.h"
#include "files.h"

#define FG     0xD0D0D0
#define BG     0x101820
#define ACCENT 0x80C0FF
#define DIM    0x808890
#define ERROR  0xFF9090
#define WARN   0xE0C060
#define PANE   0x05080C /* the output pane: a terminal's black, apart from the lists */

#ifndef JOB_LOG /* the places: another for each in tools/test-packages.c */
#define JOB_LOG "/run/locachy/packages.log"
#define JOB_PID "/run/locachy/packages.pid"
#define PRESETS "/usr/local/share/locachy/containers"
#define STATE   "/var/lib/locachy/containers"
#define CONF_DIR CONTAINERS_CONF
#define PKG "/usr/local/bin/locachy-pkg"
#define APP "/usr/local/bin/locachy-app"
#endif
#define MANIFEST "/etc/locachy/packages"

#define MAX_ROWS 1024
#define MAX_TARGETS 64
#define MAX_LEAVES 512
#define MAX_CPKGS 2048 /* the packages of all the containers' builds */
#define VALUE_MAX 480 /* a value of a configuration: its line stays under conf_read's 512 */
#define HIT_ROW    0       /* + a list's or the form's line */
#define HIT_TAB    100000  /* + the page */
#define HIT_LOWER  200000  /* the output/details pane */
#define HIT_BUTTON 300000  /* + a button of the form */
#define HIT_KEY    400000  /* + the key a button stands for */

enum { UPDATES, CONTAINERS, HOST, PAGES };
static const char *const page_names[PAGES] = { N_("Updates"), N_("Containers"), N_("Host") };
static int page, sel[PAGES], log_from, log_total; /* the output: lines back from its end, how many */
static int show_log; /* the job's output in the lower half, not the selected row's details */
static char notice[512];
static int notice_error;

/* The plan: target, kind (repo, aur, rebuild, state), what, old, new. */
struct row {
    char target[64], kind[16], what[200], old[80], new_[80];
};
static struct row rows[MAX_ROWS];
static int nrows;
static char checked[40];

struct target {
    char name[64], state[16], why[200], note[200]; /* note: what the check could not do */
    int repo, aur, rebuild, missing, marked; /* missing: declared leaves to install */
};
static struct target targets[MAX_TARGETS];
static int ntargets;

struct container {
    char name[64], package[128], extras[VALUE_MAX + 1], command[256], arguments[256], network[8], camera[8];
    char directories[VALUE_MAX + 1], state[16], built[40];
    int prev, host;
    int pkg_first, pkg_count, pkg_listed; /* its packages: cpkgs[pkg_first], pkg_listed of pkg_count */
    long long pkg_bytes;
};
static struct container conts[MAX_TARGETS];
static int nconts;

/* What the containers' builds installed over the host's (each one's upper's
 * pacman database), by name: their versions and sizes as installed. */
struct cpkg {
    char name[64], version[64];
    long long bytes;
};
static struct cpkg cpkgs[MAX_CPKGS];
static int ncpkgs;
static int details_from, details_total, details_rows; /* a container's details: first line shown, of how many, how many fit */
static char details_name[64];                         /* whose */

/* The host's manifest: its leaves (their versions and sizes as installed),
 * and what differs from it; what is installed in all. */
static char leaves[MAX_LEAVES][64], leaf_versions[MAX_LEAVES][64], others[MAX_LEAVES][80];
static long long leaf_bytes[MAX_LEAVES];
static int nleaves, nothers, nmissing, ndrift;
static char closure[16];
static long long installed = -1, foreign, installed_bytes;

/* The job: ours (a child), or one a previous run left going; how the last
 * one ended (-1 none yet, 0 done, 1 failed, 2 ended: not ours to know). */
static pid_t job;
static int job_ours, job_result = -1;
static char job_label[160];

static void say_notice(int error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say_notice(int error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(notice, sizeof notice, fmt, ap);
    va_end(ap);
    notice_error = error;
}

/* Fields of a line, split at tabs (empty ones kept). */
static int fields(char *line, char **f, int max)
{
    int n = 0;
    line[strcspn(line, "\n")] = '\0';
    while (n < max) {
        f[n++] = line;
        char *tab = strchr(line, '\t');
        if (!tab) {
            break;
        }
        *tab = '\0';
        line = tab + 1;
    }
    return n;
}

static struct target *target(const char *name)
{
    for (int i = 0; i < ntargets; i++) {
        if (strcmp(targets[i].name, name) == 0) {
            return &targets[i];
        }
    }
    if (ntargets == MAX_TARGETS) {
        return NULL;
    }
    struct target *t = &targets[ntargets++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    return t;
}

static void load_plan(void)
{
    char line[600], *f[5];
    nrows = ntargets = 0;
    checked[0] = '\0';
    target("host");
    target("locachy");
    char cmd[600];
    snprintf(cmd, sizeof cmd, "%s plan --cached --machine 2>/dev/null", PKG);
    FILE *in = popen(cmd, "r");
    while (in && fgets(line, sizeof line, in)) {
        int n = fields(line, f, 5);
        if (n >= 2 && strcmp(f[0], "checked") == 0) {
            snprintf(checked, sizeof checked, "%s", f[1]);
            continue;
        }
        if (n < 3 || nrows == MAX_ROWS) {
            continue;
        }
        struct row *r = &rows[nrows++];
        snprintf(r->target, sizeof r->target, "%s", f[0]);
        snprintf(r->kind, sizeof r->kind, "%s", f[1]);
        snprintf(r->what, sizeof r->what, "%s", f[2]);
        snprintf(r->old, sizeof r->old, "%s", n > 3 ? f[3] : "");
        snprintf(r->new_, sizeof r->new_, "%s", n > 4 ? f[4] : "");
        struct target *t = target(r->target);
        if (!t) {
            continue;
        }
        if (strcmp(r->kind, "repo") == 0) {
            t->repo++;
        } else if (strcmp(r->kind, "aur") == 0) {
            t->aur++;
        } else if (strcmp(r->kind, "missing") == 0) {
            t->missing++;
        } else if (strcmp(r->kind, "rebuild") == 0 || strcmp(r->kind, "local") == 0) {
            if (!t->rebuild++) {
                snprintf(t->why, sizeof t->why, strcmp(r->kind, "local") == 0 ?
                         _("local package %.180s") : "%.180s", r->what);
            }
        } else if (strcmp(r->kind, "state") == 0) {
            snprintf(t->state, sizeof t->state, "%s", r->what);
        } else if (strcmp(r->kind, "note") == 0) {
            snprintf(t->note, sizeof t->note, "%s", r->what);
        }
    }
    if (in) {
        pclose(in);
    }
    for (int i = 0; i < ntargets; i++) {
        struct target *t = &targets[i];
        t->marked = (t->repo || t->aur || t->rebuild || t->missing) &&
                    (!t->state[0] || strcmp(t->state, "closed") == 0);
    }
}

static void container_value(const char *name, const char *value, void *arg)
{
    struct container *c = arg;
    if (strcmp(name, "package") == 0) {
        snprintf(c->package, sizeof c->package, "%s", value);
    } else if (strcmp(name, "extras") == 0) {
        snprintf(c->extras, sizeof c->extras, "%s", value);
    } else if (strcmp(name, "command") == 0) {
        snprintf(c->command, sizeof c->command, "%s", value);
    } else if (strcmp(name, "arguments") == 0) {
        snprintf(c->arguments, sizeof c->arguments, "%s", value);
    } else if (strcmp(name, "network") == 0) {
        snprintf(c->network, sizeof c->network, "%s", value);
    } else if (strcmp(name, "camera") == 0) {
        snprintf(c->camera, sizeof c->camera, "%s", value);
    } else if (strcmp(name, "directories") == 0) {
        snprintf(c->directories, sizeof c->directories, "%s", value);
    } else if (strcmp(name, "host") == 0) {
        c->host = strcmp(value, "1") == 0;
    }
}

static void built_value(const char *name, const char *value, void *arg)
{
    struct container *c = arg;
    if (strcmp(name, "date") == 0) {
        snprintf(c->built, sizeof c->built, "%s", value);
    }
}

static void read_state(const char *name, char *out, size_t size)
{
    char path[160], text[128] = "";
    snprintf(path, sizeof path, "/sys/fs/cgroup/app_%s/cgroup.events", name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, text, sizeof text - 1) : -1;
    if (fd >= 0) {
        close(fd);
    }
    text[n > 0 ? n : 0] = '\0';
    snprintf(out, size, "%s", !strstr(text, "populated 1") ? "closed" :
             strstr(text, "frozen 1") ? "frozen" : "running");
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct container *)a)->name, ((const struct container *)b)->name);
}

static int by_cpkg(const void *a, const void *b)
{
    return strcmp(((const struct cpkg *)a)->name, ((const struct cpkg *)b)->name);
}

/* The packages its build installed (STATE/NAME/upper/var/lib/pacman/local,
 * as locachy-pkg reads them: the rest it has from the host), into cpkgs.
 * An entry without a desc (ALPM_DB_VERSION, the whiteout of a host package
 * it replaced) is none. */
static void load_packages(struct container *c)
{
    char path[512], desc[1024], *line = NULL;
    size_t cap = 0;
    snprintf(path, sizeof path, "%s/%s/upper/var/lib/pacman/local", STATE, c->name);
    c->pkg_first = ncpkgs;
    DIR *dir = opendir(path);
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL) {
        snprintf(desc, sizeof desc, "%s/%s/desc", path, e->d_name);
        FILE *f = e->d_name[0] != '.' ? fopen(desc, "re") : NULL;
        struct cpkg p = { 0 };
        int tag = 0; /* the value under: 1 %NAME%, 2 %VERSION%, 3 %SIZE% */
        while (f && getline(&line, &cap, f) > 0) {
            line[strcspn(line, "\n")] = '\0';
            if (line[0] == '%') {
                tag = strcmp(line, "%NAME%") == 0 ? 1 : strcmp(line, "%VERSION%") == 0 ? 2 :
                      strcmp(line, "%SIZE%") == 0 ? 3 : 0;
                continue;
            }
            if (tag == 1) {
                snprintf(p.name, sizeof p.name, "%.63s", line);
            } else if (tag == 2) {
                snprintf(p.version, sizeof p.version, "%.63s", line);
            } else if (tag == 3) {
                p.bytes = atoll(line);
            }
            tag = 0;
        }
        if (f) {
            fclose(f);
        }
        if (!p.name[0]) {
            continue;
        }
        c->pkg_count++;
        c->pkg_bytes += p.bytes;
        if (ncpkgs < MAX_CPKGS) {
            cpkgs[ncpkgs++] = p;
            c->pkg_listed++;
        }
    }
    free(line);
    if (dir) {
        closedir(dir);
    }
    qsort(cpkgs + c->pkg_first, (size_t)c->pkg_listed, sizeof cpkgs[0], by_cpkg);
}

static void load_containers(void)
{
    nconts = ncpkgs = 0;
    DIR *dir = opendir(CONF_DIR);
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL && nconts < MAX_TARGETS) {
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= sizeof conts[0].name || strcmp(e->d_name + len - 5, ".conf") != 0) {
            continue;
        }
        struct container *c = &conts[nconts++];
        char path[512];
        struct stat st;
        memset(c, 0, sizeof *c);
        snprintf(c->name, sizeof c->name, "%.*s", (int)(len - 5), e->d_name);
        snprintf(path, sizeof path, "%s/%s", CONF_DIR, e->d_name);
        conf_read(path, container_value, c);
        if (c->host) { /* a host program: its package is the host's (the Host page) */
            nconts--;
            continue;
        }
        snprintf(path, sizeof path, "%s/%s/built", STATE, c->name);
        conf_read(path, built_value, c);
        snprintf(path, sizeof path, "%s/%s/upper.prev", STATE, c->name);
        c->prev = stat(path, &st) == 0;
        read_state(c->name, c->state, sizeof c->state);
        load_packages(c);
    }
    if (dir) {
        closedir(dir);
    }
    qsort(conts, (size_t)nconts, sizeof conts[0], by_name);
}

static void load_host(void)
{
    char line[256], *f[4];
    nleaves = nothers = nmissing = ndrift = 0;
    closure[0] = '\0';
    installed = -1;
    foreign = installed_bytes = 0;
    char cmd[600];
    snprintf(cmd, sizeof cmd, "%s status --machine 2>/dev/null", PKG);
    FILE *in = popen(cmd, "r");
    while (in && fgets(line, sizeof line, in)) {
        int n = fields(line, f, 4);
        if (n < 2) {
            continue;
        }
        if (strcmp(f[0], "leaf") == 0 && nleaves < MAX_LEAVES) {
            snprintf(leaves[nleaves], sizeof leaves[0], "%s", f[1]);
            snprintf(leaf_versions[nleaves], sizeof leaf_versions[0], "%s", n > 2 ? f[2] : "");
            leaf_bytes[nleaves++] = n > 3 ? atoll(f[3]) : 0;
        } else if (strcmp(f[0], "closure") == 0) {
            snprintf(closure, sizeof closure, "%s", f[1]);
        } else if (strcmp(f[0], "installed") == 0) {
            installed = atoll(f[1]);
        } else if (strcmp(f[0], "foreign") == 0) {
            foreign = atoll(f[1]);
        } else if (strcmp(f[0], "bytes") == 0) {
            installed_bytes = atoll(f[1]);
        } else if (nothers < MAX_LEAVES) {
            nmissing += strcmp(f[0], "missing") == 0;
            ndrift += strcmp(f[0], "drift") == 0;
            snprintf(others[nothers++], sizeof others[0], "%-8.8s %.70s", f[0], f[1]);
        }
    }
    if (in) {
        pclose(in);
    }
}

/* bytes as people read them: 1.4 GiB. */
static void human_size(long long bytes, char *out, size_t size)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    snprintf(out, size, u ? "%.1f %s" : "%.0f %s", v, units[u]);
}

/* What is installed on the host, in a line: how many, how much, of what. */
static void installed_summary(char *line, size_t size)
{
    char bytes[32];
    if (installed < 0) {
        snprintf(line, size, "%s", _("Installed: unknown (locachy-pkg status gave nothing)"));
        return;
    }
    human_size(installed_bytes, bytes, sizeof bytes);
    snprintf(line, size, P_("Installed: %lld package, %s   %d declared, %s with what they need, "
                            "%lld foreign (AUR, LOCachy)   drift %d, missing %d",
                            "Installed: %lld packages, %s   %d declared, %s with what they need, "
                            "%lld foreign (AUR, LOCachy)   drift %d, missing %d", installed),
             installed, bytes, nleaves, closure[0] ? closure : "?", foreign, ndrift, nmissing);
}

static void load_all(void)
{
    load_plan();
    load_containers();
    load_host();
}

static const char *state_label(const char *state)
{
    return strcmp(state, "closed") == 0 ? _("closed") :
           strcmp(state, "frozen") == 0 ? _("frozen") :
           strcmp(state, "running") == 0 ? _("running") : state;
}

/* The job: argv, its output in JOB_LOG, out of this program's task. */
static void start_job(const char *label, char *const argv[])
{
    if (job) {
        say_notice(1, _("Wait: %s is still running."), job_label);
        return;
    }
    mkdir("/run/locachy", 0755);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int cg = open("/sys/fs/cgroup/cgroup.procs", O_WRONLY | O_CLOEXEC);
        if (cg >= 0) {
            if (write(cg, "0", 1) < 0) {
            }
            close(cg);
        }
        int out = open(JOB_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            dup2(out, STDERR_FILENO);
        }
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) {
        say_notice(1, _("Cannot start %s."), label);
        return;
    }
    job = pid;
    job_ours = 1;
    job_result = -1;
    log_from = 0;
    show_log = 1;
    snprintf(job_label, sizeof job_label, "%s", label);
    FILE *f = fopen(JOB_PID, "we");
    if (f) {
        fprintf(f, "%d %s\n", (int)pid, label);
        fclose(f);
    }
    say_notice(0, _("%s: running; it goes on if you leave."), label);
}

/* A job left going by a previous run of this program. */
static void find_job(void)
{
    FILE *f = fopen(JOB_PID, "re");
    int pid;
    char label[160] = "";
    if (f && fscanf(f, "%d %159[^\n]", &pid, label) >= 1 && pid > 1 && kill(pid, 0) == 0) {
        job = pid;
        job_ours = 0;
        snprintf(job_label, sizeof job_label, "%s", label);
        say_notice(0, _("%s: still running."), label);
    }
    if (f) {
        fclose(f);
    }
}

/* The command that trusts the source signing keys a build stopped for
 * (locachy-build's "Trust them: ..." in the job's output), or "". */
static void trust_command(char *out, size_t size)
{
    char buf[65536];
    out[0] = '\0';
    int fd = open(JOB_LOG, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    off_t end = lseek(fd, 0, SEEK_END);
    ssize_t got = pread(fd, buf, sizeof buf - 1, end > (off_t)sizeof buf - 1 ? end - (off_t)sizeof buf + 1 : 0);
    close(fd);
    if (got <= 0) return;
    buf[got] = '\0';
    for (char *s = strstr(buf, "Trust them: "); s; s = strstr(s + 1, "Trust them: ")) {
        if (s != buf && s[-1] != '\n') continue;
        size_t n = strcspn(s + 12, "\r\n");
        snprintf(out, size, "%.*s", (int)n, s + 12);
    }
}

static void check_job(void)
{
    int status = 0;
    if (!job) {
        return;
    }
    if (job_ours ? waitpid(job, &status, WNOHANG) != job : kill(job, 0) == 0) {
        return;
    }
    int ok = job_ours && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    job_result = !job_ours ? 2 : ok ? 0 : 1;
    if (!job_ours) {
        say_notice(0, _("%s: ended; its output is below."), job_label);
    } else if (ok) {
        say_notice(0, _("%s: done."), job_label);
    } else {
        char trust[512];
        trust_command(trust, sizeof trust);
        if (trust[0]) {
            /* Not trusted by anything but a person: the keys are in the output. */
            say_notice(1, _("Trust the keys below as root, after checking them: %s; then apply again."), trust);
        } else {
            say_notice(1, _("%s: failed; its output is below (and in %s)."), job_label, JOB_LOG);
        }
    }
    job = 0;
    unlink(JOB_PID);
    load_all();
    char build_prefix[32];
    snprintf(build_prefix, sizeof build_prefix, _("build %s"), "");
    size_t blen = strlen(build_prefix);
    for (int i = 0; ok && blen > 0 && strncmp(job_label, build_prefix, blen) == 0 && i < nconts; i++) {
        if (strcmp(conts[i].name, job_label + blen) == 0 && !conts[i].command[0]) {
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            say_notice(0, _("%s built; its command is not set: Enter, then Command, lists what its packages run."),
                       conts[i].name);
        }
    }
}

/* The last lines of the job's output (a line rewritten with \r: its last
 * form), from the end, skipping from of them; n of them at most. How many
 * lines there are (of its last 32 KiB) into total. */
static int log_lines(char lines[][256], int n, int from, int *total)
{
    static char buf[32768];
    *total = 0;
    int fd = open(JOB_LOG, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    off_t end = lseek(fd, 0, SEEK_END);
    off_t start = end > (off_t)sizeof buf - 1 ? end - (off_t)sizeof buf + 1 : 0;
    ssize_t got = pread(fd, buf, sizeof buf - 1, start);
    close(fd);
    if (got <= 0) {
        return 0;
    }
    buf[got] = '\0';
    char *all[2048];
    int count = 0;
    for (char *s = strtok(buf, "\n"); s && count < 2048; s = strtok(NULL, "\n")) {
        char *cr = strrchr(s, '\r');
        while (cr && cr[1] == '\0' && cr > s) { /* a line ending in \r */
            *cr = '\0';
            cr = strrchr(s, '\r');
        }
        all[count++] = cr ? cr + 1 : s;
    }
    *total = count;
    int last = count - from, first = last - n < 0 ? 0 : last - n, k = 0;
    for (int i = first; i < last && i >= 0; i++) {
        snprintf(lines[k++], 256, "%s", all[i]);
    }
    return k;
}

static int list_len(void)
{
    return page == UPDATES ? ntargets : page == CONTAINERS ? nconts : nleaves;
}

static void row_text(int i, char *line, size_t size, int *dim)
{
    char sbuf[32], nbuf[32];
    *dim = 0;
    if (page == UPDATES) {
        struct target *t = &targets[i];
        char what[512] = "";
        int total = t->repo + t->aur;
        if (total) {
            snprintf(what, sizeof what, !t->aur ? P_("%d update", "%d updates", total) :
                     t->repo ? P_("%d update, some from the AUR", "%d updates, some from the AUR", total) :
                     P_("%d update, from the AUR", "%d updates, from the AUR", total), total);
        }
        if (t->missing) {
            size_t n = strlen(what);
            if (n) {
                snprintf(what + n, sizeof what - n, "; ");
                n = strlen(what);
            }
            snprintf(what + n, sizeof what - n,
                     P_("%d declared to install", "%d declared to install", t->missing), t->missing);
        }
        if (t->rebuild) {
            size_t n = strlen(what);
            snprintf(what + n, sizeof what - n, n ? _("; rebuild: %s") : _("rebuild: %s"), t->why);
        }
        if (t->note[0]) { /* first: the rest of the row may not fit */
            char rest[300];
            memcpy(rest, what, sizeof rest);
            snprintf(what, sizeof what, "%.200s%s%.290s", t->note, rest[0] ? "; " : "", rest);
        }
        if (!what[0]) {
            snprintf(what, sizeof what, "%s", checked[0] ? _("up to date") : _("not checked yet"));
            *dim = 1;
        }
        ui_pad(sbuf, sizeof sbuf, strcmp(t->state, "closed") == 0 ? "" : state_label(t->state), 8);
        snprintf(line, size, "[%c] %-12.12s %s %s", t->marked ? 'x' : ' ', t->name, sbuf, what);
    } else if (page == CONTAINERS) {
        struct container *c = &conts[i];
        char bbuf[96], bpad[96], cbuf[32], pbuf[96], ppad[96], bytes[32];
        ui_pad(sbuf, sizeof sbuf, state_label(c->state), 8);
        ui_pad(nbuf, sizeof nbuf, strcmp(c->network, "1") == 0 ? C_("row", "network") : "", 8);
        ui_pad(cbuf, sizeof cbuf, strcmp(c->camera, "1") == 0 ? C_("row", "camera") : "", 7);
        snprintf(bbuf, sizeof bbuf, _("built %s"), ui_pad(bpad, sizeof bpad, c->built[0] ? c->built : _("never"), 19));
        pbuf[0] = '\0';
        if (c->built[0] || c->pkg_count) { /* what its build installed over the host's */
            human_size(c->pkg_bytes, bytes, sizeof bytes);
            snprintf(pbuf, sizeof pbuf, P_("%d package, %s", "%d packages, %s", c->pkg_count), c->pkg_count, bytes);
        }
        snprintf(line, size, "%-12.12s %s %s %s %s %s%s  %s %.200s", c->name, sbuf, nbuf, cbuf,
                 ui_pad(ppad, sizeof ppad, pbuf, 22), bbuf, c->prev ? _(" (previous kept)") : "", c->package, c->extras);
    } else {
        char vbuf[96], bytes[32];
        human_size(leaf_bytes[i], bytes, sizeof bytes);
        *dim = !leaf_versions[i][0];
        snprintf(line, size, "%-32.32s %s %10s", leaves[i],
                 ui_pad(vbuf, sizeof vbuf, leaf_versions[i][0] ? leaf_versions[i] : _("not installed"), 28),
                 leaf_versions[i][0] ? bytes : "");
    }
}

static uint32_t log_color(const char *s)
{
    if (strncmp(s, "===>", 4) == 0 || strncmp(s, "==>", 3) == 0 || strncmp(s, "::", 2) == 0) {
        return ACCENT;
    }
    if (strcasestr(s, "error") || strcasestr(s, "failed") || strcasestr(s, "fatal")) {
        return ERROR;
    }
    return strcasestr(s, "warning") ? WARN : FG;
}

/* The pane's title: a bar across, apart from the lists above. */
static void pane_title(int row, const char *text)
{
    screen_fill(0, row, screen_cols(), 1, DIM);
    screen_text(1, row, text, BG, DIM);
}

#define CONT_ITEMS 10 /* the lines of a container's configuration and build in its details */

/* How many lines a container's details have: its configuration and build,
 * what its build installed in all, then those packages (under a header),
 * one a line. */
static int detail_lines(const struct container *c)
{
    return CONT_ITEMS + 1 + (c->pkg_listed ? 1 + c->pkg_listed : 0);
}

/* The i-th line of a container's details into line: its colour. */
static uint32_t detail_line(const struct container *c, int i, char *line, size_t size)
{
    char kbuf[48], vbuf[96], bytes[32];
    if (i < CONT_ITEMS) {
        /* TRANSLATORS: r is the r key; keep it. */
        const char *items[CONT_ITEMS][2] = {
            { _("package"), c->package }, { _("extras"), c->extras },
            { _("command"), c->command }, { _("arguments"), c->arguments },
            { C_("detail", "network"), strcmp(c->network, "1") == 0 ? _("yes (locachy-net)") : _("no") },
            { C_("detail", "camera"), strcmp(c->camera, "1") == 0 ? _("yes (the cameras, as they come and go)") : _("no") },
            { _("directories"), c->directories },
            { _("state"), state_label(c->state) }, { _("built"), c->built[0] ? c->built : _("never") },
            { _("previous build"), c->prev ? _("kept (r rolls back to it)") : _("none") },
        };
        snprintf(line, size, "%s %s", ui_pad(kbuf, sizeof kbuf, items[i][0], 15), items[i][1]);
        return FG;
    }
    human_size(c->pkg_bytes, bytes, sizeof bytes);
    if (i == CONT_ITEMS && !c->built[0] && !c->pkg_count) {
        snprintf(line, size, "%s", _("Not built yet: no build has installed anything"));
        return DIM;
    } else if (i == CONT_ITEMS && !c->pkg_count) {
        snprintf(line, size, "%s", _("Its build installed nothing: all its packages are the host's"));
        return DIM;
    } else if (i == CONT_ITEMS) {
        snprintf(line, size, P_("Its build installed %d package, %s; the rest is the host's",
                                "Its build installed %d packages, %s; the rest is the host's", c->pkg_count),
                 c->pkg_count, bytes);
        return ACCENT;
    } else if (i == CONT_ITEMS + 1) { /* a header over the columns, as the Host page's */
        snprintf(line, size, "%-32.32s %s %10s", _("Package"), ui_pad(vbuf, sizeof vbuf, _("Version"), 28), _("Size"));
        return DIM;
    }
    const struct cpkg *p = &cpkgs[c->pkg_first + i - CONT_ITEMS - 2];
    human_size(p->bytes, bytes, sizeof bytes);
    snprintf(line, size, "%-32.32s %s %10s", p->name, ui_pad(vbuf, sizeof vbuf, p->version, 28), bytes);
    return FG;
}

/* The lower pane, on a background of its own under a title bar: the job's
 * output (while it runs, or l), else the selected row's details (on Host:
 * how the system stands to its manifest; of a container, its packages too,
 * scrolled by details_from). */
static void draw_lower(int top, int height)
{
    char line[512];
    int r = top + 1;
    screen_fill(0, top, screen_cols(), height, PANE);
    if (job || show_log) {
        char lines[64][256];
        int total, n = log_lines(lines, height - 1 < 64 ? height - 1 : 64, log_from, &total);
        log_total = total;
        const char *how = job ? _("running") : job_result == 0 ? _("done") : job_result == 1 ? _("failed") :
                          job_result == 2 ? _("ended") : "";
        char where[64] = "";
        if (n) {
            int last = total - log_from;
            snprintf(where, sizeof where, _("lines %d-%d of %d"), last - n + 1, last, total);
        }
        /* TRANSLATORS: PgUp/PgDn and l are key names; keep them. */
        snprintf(line, sizeof line, _("Output: %s%s%s%s   %s   PgUp/PgDn: scroll   l: %s"),
                 job || job_result >= 0 ? job_label : n ? _("the last job") : _("no job yet"),
                 how[0] ? " (" : "", how, how[0] ? ")" : "", where, page == HOST ? _("the manifest") : _("details"));
        pane_title(top, line);
        for (int i = 0; i < n && r < top + height; i++) {
            screen_text(2, r++, lines[i], log_color(lines[i]), PANE);
        }
        return;
    }
    if (page == HOST) {
        /* TRANSLATORS: l is the l key; keep it. */
        snprintf(line, sizeof line, P_("%s: %d declared, %s installed with what they need   l: output",
                                       "%s: %d declared, %s installed with what they need   l: output", nleaves),
                 MANIFEST, nleaves, closure[0] ? closure : "?");
        pane_title(top, line);
        for (int i = 0; i < nothers && r < top + height; i++) {
            screen_text(2, r++, others[i], strncmp(others[i], "missing", 7) == 0 ? ERROR : FG, PANE);
        }
        if (nothers == 0) {
            screen_text(2, r++, _("The system is as the manifest says."), DIM, PANE);
        }
        return;
    }
    if (page == UPDATES && sel[page] < ntargets) {
        struct target *t = &targets[sel[page]];
        /* TRANSLATORS: l is the l key; keep it. */
        snprintf(line, sizeof line, _("Details: %s   l: output"), t->name);
        pane_title(top, line);
        for (int i = 0; i < nrows && r < top + height; i++) {
            struct row *w = &rows[i];
            if (strcmp(w->target, t->name) != 0 || strcmp(w->kind, "state") == 0) {
                continue;
            }
            if (strcmp(w->kind, "note") == 0) {
                screen_text(2, r++, w->what, WARN, PANE);
                continue;
            }
            if (strcmp(w->kind, "missing") == 0) {
                snprintf(line, sizeof line, _("install %-36.100s declared, not installed"), w->what);
            } else if (strcmp(w->kind, "local") == 0) {
                snprintf(line, sizeof line, _("local %-30.100s %.200s (installed %.80s)"), w->what, w->new_, w->old);
            } else if (strcmp(w->kind, "rebuild") == 0 && (w->old[0] || w->new_[0])) {
                snprintf(line, sizeof line, _("rebuild: %s (%s -> %s)"), w->what, w->old, w->new_);
            } else if (strcmp(w->kind, "rebuild") == 0) {
                snprintf(line, sizeof line, _("rebuild: %s"), w->what);
            } else {
                snprintf(line, sizeof line, "%-5s %-36s %s -> %s", w->kind, w->what, w->old, w->new_);
            }
            screen_text(2, r++, line, FG, PANE);
        }
    } else if (page == CONTAINERS && sel[page] < nconts) {
        struct container *c = &conts[sel[page]];
        if (strcmp(details_name, c->name) != 0) { /* another one: from its first line */
            snprintf(details_name, sizeof details_name, "%s", c->name);
            details_from = 0;
        }
        details_total = detail_lines(c);
        details_rows = height - 1 > 0 ? height - 1 : 0;
        int most = details_total > details_rows ? details_total - details_rows : 0;
        details_from = details_from < 0 ? 0 : details_from > most ? most : details_from;
        /* TRANSLATORS: l is the l key; keep it. */
        snprintf(line, sizeof line, _("Details: %s (%s/%s.conf)   l: output"), c->name, CONF_DIR, c->name);
        if (most > 0) { /* more than the pane holds: which lines, and how to see the rest */
            char where[64];
            size_t len = strlen(line);
            snprintf(where, sizeof where, _("lines %d-%d of %d"), details_from + 1, details_from + details_rows,
                     details_total);
            /* TRANSLATORS: PgUp/PgDn are key names; keep them. */
            snprintf(line + len, sizeof line - len, "   %s   %s", where, _("PgUp/PgDn: scroll"));
        }
        pane_title(top, line);
        for (int i = details_from; i < details_total && r < top + height; i++) {
            uint32_t color = detail_line(c, i, line, sizeof line);
            screen_text(2, r++, line, color, PANE);
        }
    } else {
        pane_title(top, page == CONTAINERS ? _("Details") : _("Details: nothing selected"));
    }
}

/* Each page's actions: label, then key; the buttons at the bottom. */
/* TRANSLATORS: The part before the colon (c, Space, a, l, /, n, Enter, b, r, d, i, x, s) is the key; keep it and ": ". */
static const char *const help[PAGES][8][2] = {
    { { N_("c: check"), "c" }, { N_("Space: mark"), " " }, { N_("a: apply the marked"), "a" },
      { N_("l: output/details"), "l" } },
    { { N_("/: search & install"), "/" }, { N_("n: new"), "n" }, { N_("Enter: edit"), "\n" }, { N_("b: build"), "b" },
      { N_("r: roll back"), "r" }, { N_("d: remove"), "d" }, { N_("l: output/details"), "l" } },
    { { N_("/: search"), "/" }, { N_("i: add a package"), "i" }, { N_("x: remove the selected one"), "x" },
      { N_("s: bring the system to the manifest"), "s" }, { N_("l: output/details"), "l" } },
};

static void draw(void)
{
    char line[512];
    int cols = screen_cols(), rows_n = screen_rows();
    screen_fill(0, 0, cols, rows_n, BG);
    ui_hits_clear();
    screen_bar(_("packages"), NULL);
    int x = 1, pending = 0;
    for (int i = 0; i < ntargets; i++) {
        pending += targets[i].repo || targets[i].aur || targets[i].rebuild;
    }
    const int counts[PAGES] = { pending, nconts, nleaves };
    for (int p = 0; p < PAGES; p++) {
        snprintf(line, sizeof line, " %s (%d) ", _(page_names[p]), counts[p]);
        int w = ui_cells(line);
        screen_text(x, 1, line, p == page ? BG : FG, p == page ? ACCENT : BG);
        ui_hit(x, 1, w, 1, HIT_TAB + p);
        x += w + 1;
    }
    if (page == UPDATES) {
        /* TRANSLATORS: c is the c key; keep it. */
        snprintf(line, sizeof line, _("checked %s"), checked[0] ? checked : _("never (c checks)"));
        screen_text(x + 2, 1, line, DIM, BG);
    }
    installed_summary(line, sizeof line);
    screen_text(1, 2, line, installed < 0 ? ERROR : FG, BG);
    int top = 3, height = (rows_n - 6) / 2, n = list_len();
    if (page == HOST) { /* a header over its columns */
        char vbuf[96], hbuf[160];
        snprintf(hbuf, sizeof hbuf, "%-32.32s %s %10s", _("Declared package"),
                 ui_pad(vbuf, sizeof vbuf, _("Version"), 28), _("Size"));
        screen_text(2, top++, hbuf, DIM, BG);
        height--;
    }
    int first = sel[page] >= height ? sel[page] - height + 1 : 0;
    if (n == 0) {
        /* TRANSLATORS: n is the n key; keep it. */
        screen_text(2, top, page == CONTAINERS ? _("No container yet: n makes one.") : _("(nothing)"), DIM, BG);
    }
    for (int i = first; i < n && i < first + height; i++) {
        int dim, on = i == sel[page];
        row_text(i, line, sizeof line, &dim);
        screen_fill(0, top + i - first, cols, 1, on ? ACCENT : BG);
        screen_text(2, top + i - first, line, on ? BG : dim ? DIM : FG, on ? ACCENT : BG);
        ui_hit(0, top + i - first, cols, 1, HIT_ROW + i);
    }
    draw_lower(top + height, rows_n - 2 - (top + height));
    ui_hit(0, top + height, cols, rows_n - 2 - (top + height), HIT_LOWER);
    x = 1;
    for (int b = 0; b < 8 && help[page][b][0]; b++) {
        x = ui_button(x, rows_n - 2, _(help[page][b][0]), HIT_KEY + help[page][b][1][0]);
    }
    /* TRANSLATORS: Tab is the Tab key; keep "Tab: ". */
    x = ui_button(x, rows_n - 2, _("Tab: page"), HIT_KEY + '\t');
    /* TRANSLATORS: q is the q key; keep "q: ". */
    ui_button(x, rows_n - 2, _("q: back"), HIT_KEY + 'q');
    screen_text(1, rows_n - 1, notice, notice_error ? ERROR : ACCENT, BG);
    screen_flush();
}

__attribute__((weak)) int clipboard_get(int primary, int *fd, size_t *size);
__attribute__((weak)) int keys_modifiers(void);

static void paste_into(char *text, size_t size, size_t *pos, int primary)
{
    int fd = -1;
    size_t csize = 0;
    if (!clipboard_get || clipboard_get(primary, &fd, &csize) != 0 || fd < 0) {
        return;
    }
    char buf[512];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        size_t len = strlen(text);
        if (c < 0x20 || c == 0x7f || c == '"' || len + 1 >= size) {
            continue;
        }
        memmove(text + *pos + 1, text + *pos, len - *pos + 1);
        text[(*pos)++] = (char)c;
    }
}

static int edit_line_key(int key, char *text, size_t size, size_t *pos)
{
    size_t len = strlen(text);
    const char *typed;
    if (*pos > len) *pos = len;
    if (key == K_LEFT) {
        if (*pos > 0) {
            (*pos)--;
            while (*pos > 0 && ((unsigned char)text[*pos] & 0xC0) == 0x80) (*pos)--;
        }
        return 1;
    } else if (key == K_RIGHT) {
        if (*pos < len) {
            (*pos)++;
            while (*pos < len && ((unsigned char)text[*pos] & 0xC0) == 0x80) (*pos)++;
        }
        return 1;
    } else if (key == K_BOL || key == 1) {
        *pos = 0;
        return 1;
    } else if (key == K_EOL || key == 5) {
        *pos = len;
        return 1;
    } else if (key == '\b' || key == 8) {
        if (*pos > 0) {
            size_t prev = *pos - 1;
            while (prev > 0 && ((unsigned char)text[prev] & 0xC0) == 0x80) prev--;
            memmove(text + prev, text + *pos, len - *pos + 1);
            *pos = prev;
        }
        return 1;
    } else if (key == K_DEL || key == 4) {
        if (*pos < len) {
            size_t next = *pos + 1;
            while (next < len && ((unsigned char)text[next] & 0xC0) == 0x80) next++;
            memmove(text + *pos, text + next, len - next + 1);
        }
        return 1;
    } else if (key == 21) { /* Ctrl+U */
        memmove(text, text + *pos, len - *pos + 1);
        *pos = 0;
        return 1;
    } else if (key == 11) { /* Ctrl+K */
        text[*pos] = '\0';
        return 1;
    } else if (key == 23) { /* Ctrl+W */
        size_t prev = *pos;
        while (prev > 0 && text[prev - 1] == ' ') prev--;
        while (prev > 0 && text[prev - 1] != ' ') prev--;
        memmove(text + prev, text + *pos, len - *pos + 1);
        *pos = prev;
        return 1;
    } else if (key == 22 || (key == K_INS && keys_modifiers && (keys_modifiers() & KEYS_SHIFT))) {
        paste_into(text, size, pos, 0);
        return 1;
    } else if (key == K_POINTER) {
        const struct pointer_event *e = pointer_last();
        if (e && e->pressed && e->button == BTN_MIDDLE) {
            paste_into(text, size, pos, 1);
            return 1;
        }
    } else if (key != '"' && (typed = keys_typed(key)) != NULL && len + strlen(typed) < size) {
        size_t n = strlen(typed);
        memmove(text + *pos + n, text + *pos, len - *pos + 1);
        memcpy(text + *pos, typed, n);
        *pos += n;
        return 1;
    }
    return 0;
}

/* A line typed at the bottom, starting as text: 1 with Enter, 0 with Esc. */
static int prompt(const char *label, char *text, size_t size)
{
    char line[512];
    size_t pos = strlen(text);
    for (;;) {
        size_t len = strlen(text);
        if (pos > len) pos = len;
        snprintf(line, sizeof line, "%s %.*s_%s", label, (int)pos, text, text + pos);
        screen_fill(0, screen_rows() - 1, screen_cols(), 1, BG);
        screen_text(1, screen_rows() - 1, line, ACCENT, BG);
        screen_flush();
        int key = key_get();
        if (key == 27) {
            return 0;
        } else if (key == '\n') {
            return 1;
        } else {
            edit_line_key(key, text, size, &pos);
        }
    }
}

static int yes(const char *question)
{
    char line[512];
    snprintf(line, sizeof line, "%s [y/N] ", question);
    int row = screen_rows() - 1;
    screen_fill(0, row, screen_cols(), 1, BG);
    screen_text(1, row, line, ACCENT, BG);
    /* TRANSLATORS: Y is the Y key; keep "Y: ". */
    int c = ui_button(2 + ui_cells(line), row, _("Y: yes"), HIT_KEY + 'y');
    /* TRANSLATORS: N is the N key; keep "N: ". */
    ui_button(c, row, _("N: no"), HIT_KEY + 'n');
    screen_flush();
    for (;;) {
        int key = key_get();
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (!e->pressed) continue; /* a motion is no answer */
            key = ui_hit_at(e->col, e->row) == HIT_KEY + 'y' ? 'y' : 'n';
        }
        return key == 'y' || key == 'Y';
    }
}

static int valid_name(const char *s)
{
    if (!isalnum((unsigned char)s[0]) || strlen(s) > 63) {
        return 0;
    }
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-' && *s != '.') {
            return 0;
        }
    }
    return 1;
}

/* One of n items, on a screen of their own: its index, -1 with Esc or q. */
static int choose(const char *title, char items[][320], int n)
{
    int at = 0;
    for (;;) {
        int height = screen_rows() - 6, first = at >= height ? at - height + 1 : 0;
        screen_fill(0, 0, screen_cols(), screen_rows(), BG);
        screen_bar(_("packages"), NULL);
        screen_text(1, 1, title, ACCENT, BG);
        for (int i = first; i < n && i < first + height; i++) {
            screen_fill(0, 3 + i - first, screen_cols(), 1, i == at ? ACCENT : BG);
            screen_text(2, 3 + i - first, items[i], i == at ? BG : FG, i == at ? ACCENT : BG);
        }
        /* TRANSLATORS: Up/Down, Enter and Esc are key names; keep them. */
        screen_text(1, screen_rows() - 2, _("Up/Down or the wheel   Enter or a double click: choose   Esc: back"),
                    FG, BG);
        screen_flush();
        int key = key_get();
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (e->wheel) {
                at -= e->wheel;
                at = at < 0 ? 0 : at >= n ? n - 1 : at;
            } else if (e->button == BTN_LEFT && e->pressed && e->row >= 3 && e->row - 3 < height &&
                       first + e->row - 3 < n) {
                at = first + e->row - 3;
                if (e->clicks == 2) return at;
            }
            continue;
        }
        if (key == 27 || key == 'q') {
            return -1;
        } else if (key == K_UP && at > 0) {
            at--;
        } else if (key == K_DOWN && at < n - 1) {
            at++;
        } else if (key == K_PGUP) {
            at = at > height ? at - height : 0;
        } else if (key == K_PGDN && n > 0) {
            at = at + height < n ? at + height : n - 1;
        } else if (key == K_BOL) {
            at = 0;
        } else if (key == K_EOL && n > 0) {
            at = n - 1;
        } else if (key == '\n' && n > 0) {
            return at;
        }
    }
}

static int by_string(const void *a, const void *b)
{
    return strcmp(a, b);
}

/* The presets' names (PRESETS/NAME.conf), sorted: how many. */
static int preset_names(char names[][64], int max)
{
    int n = 0;
    DIR *dir = opendir(PRESETS);
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL && n < max) {
        size_t len = strlen(e->d_name);
        if (len > 5 && len - 5 < 64 && strcmp(e->d_name + len - 5, ".conf") == 0) {
            snprintf(names[n++], 64, "%.*s", (int)(len - 5), e->d_name);
        }
    }
    if (dir) {
        closedir(dir);
    }
    qsort(names, (size_t)n, 64, by_string);
    return n;
}

/* The name of the preset at path (PRESETS/NAME.conf). */
static const char *preset_name(const char *path)
{
    static char name[64];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t len = strlen(base);
    snprintf(name, sizeof name, "%.*s", (int)(len > 5 ? len - 5 : len), base);
    return name;
}

static int copy_file(const char *from, const char *to)
{
    char buf[4096];
    FILE *in = fopen(from, "re"), *out = in ? fopen(to, "we") : NULL;
    size_t n;
    int ok = in && out;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) {
        ok = fwrite(buf, 1, n, out) == n;
    }
    if (in) {
        fclose(in);
    }
    if (out && fclose(out) != 0) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

/* ---- A container's form: what it is made of, a line each ---- */

#define MAX_PKGS 32
#define MAX_DIRS 16
#define FORM_LINES 64
#define FORM_TOP 3    /* the screen row of its first line */
#define FORM_VALUE 16 /* the column of the values */
#define FORM_INFO 58  /* the column of what is known of them */

struct pkg_info { /* a line of locachy-pkg info */
    char name[128], kind[12], repo[40], version[80], description[200], host[80];
};

struct form {
    int is_new, built, network, camera, changed;
    char preset[400]; /* the preset it was filled from ("": none) */
    char name[64], package[128], extras[VALUE_MAX + 1], command[256], arguments[256];
    char dirs[MAX_DIRS][256];
    int ndirs;
    struct pkg_info info[MAX_PKGS];
    int ninfo;
    int sel, first, button; /* the selected line, the first shown, the button */
};

enum field { F_PRESET, F_NAME, F_PACKAGE, F_EXTRAS, F_COMMAND, F_ARGUMENTS, F_NETWORK, F_CAMERA, F_DIRS, F_ALWAYS, F_BUTTONS };
static const char *const field_names[] = {
    N_("Preset"), N_("Name"), N_("Package"), N_("Extras"), N_("Command"), N_("Arguments"), N_("Network"), N_("Camera"),
    N_("Directories"), N_("Always"), "",
};
enum { B_SAVE, B_BUILD, B_CANCEL, BUTTONS };
static const char *const button_names[BUTTONS] = { N_("Save"), N_("Save and build"), N_("Cancel") };

/* A line of the form: an extra and a directory have one each. */
struct form_line {
    enum field field;
    int index;
};

/* The i-th word of s (separated by spaces) into out; 0 if it has fewer. */
static int nth_word(const char *s, int i, char *out, size_t size)
{
    for (;;) {
        s += strspn(s, " ");
        size_t len = strcspn(s, " ");
        if (len == 0) {
            return 0;
        }
        if (i-- == 0) {
            snprintf(out, size, "%.*s", (int)len, s);
            return 1;
        }
        s += len;
    }
}

/* Package names separated by spaces, max of them at most: letters, digits,
 * @ . _ + - (not first: no option), which makes them safe in a command line. */
static int valid_packages(const char *s, int max)
{
    int words = 0;
    for (const char *p = s; *p; p++) {
        int starts = *p != ' ' && (p == s || p[-1] == ' ');
        if (*p != ' ' && !isalnum((unsigned char)*p) && !strchr("@._+-", *p)) {
            return 0;
        }
        if (starts && (*p == '-' || *p == '.')) {
            return 0;
        }
        words += starts;
    }
    return words <= max;
}

static void form_value(const char *name, const char *value, void *arg)
{
    struct form *f = arg;
    if (strcmp(name, "package") == 0) {
        snprintf(f->package, sizeof f->package, "%s", value);
    } else if (strcmp(name, "extras") == 0) {
        snprintf(f->extras, sizeof f->extras, "%s", value);
    } else if (strcmp(name, "command") == 0) {
        snprintf(f->command, sizeof f->command, "%s", value);
    } else if (strcmp(name, "arguments") == 0) {
        snprintf(f->arguments, sizeof f->arguments, "%s", value);
    } else if (strcmp(name, "network") == 0) {
        f->network = strcmp(value, "1") == 0;
    } else if (strcmp(name, "camera") == 0) {
        f->camera = strcmp(value, "1") == 0;
    } else if (strcmp(name, "directories") == 0) {
        f->ndirs = 0;
        for (const char *s = value; *s && f->ndirs < MAX_DIRS;) {
            size_t len = strcspn(s, ":");
            if (len > 0 && len < sizeof f->dirs[0]) {
                snprintf(f->dirs[f->ndirs++], sizeof f->dirs[0], "%.*s", (int)len, s);
            }
            s += len + (s[len] == ':');
        }
    }
}

static void add_line(struct form_line *l, int *n, enum field field, int index)
{
    if (*n < FORM_LINES) {
        l[*n].field = field;
        l[*n].index = index;
        (*n)++;
    }
}

static int form_lines(const struct form *f, struct form_line *l)
{
    char word[128];
    int n = 0, extras = 0;
    while (nth_word(f->extras, extras, word, sizeof word)) {
        extras++;
    }
    if (f->is_new) {
        add_line(l, &n, F_PRESET, 0);
    }
    add_line(l, &n, F_NAME, 0);
    add_line(l, &n, F_PACKAGE, 0);
    for (int i = 0; i < (extras ? extras : 1); i++) {
        add_line(l, &n, F_EXTRAS, i);
    }
    add_line(l, &n, F_COMMAND, 0);
    add_line(l, &n, F_ARGUMENTS, 0);
    add_line(l, &n, F_NETWORK, 0);
    add_line(l, &n, F_CAMERA, 0);
    for (int i = 0; i <= f->ndirs; i++) { /* the last: to add one */
        add_line(l, &n, F_DIRS, i);
    }
    add_line(l, &n, F_ALWAYS, 0);
    add_line(l, &n, F_BUTTONS, 0);
    return n;
}

/* What is known of package name (locachy-pkg info) into out: its colour. */
static uint32_t package_text(const struct form *f, const char *name, char *out, size_t size)
{
    const struct pkg_info *p = NULL;
    for (int i = 0; i < f->ninfo && !p; i++) {
        p = strcmp(f->info[i].name, name) == 0 ? &f->info[i] : NULL;
    }
    char host[120] = "";
    if (p && p->host[0]) {
        snprintf(host, sizeof host, _(", the host has %.80s"), p->host);
    }
    if (!p) {
        snprintf(out, size, "%s", _("not looked up"));
        return DIM;
    } else if (strcmp(p->kind, "repo") == 0) {
        snprintf(out, size, "%s %s%s  %s", p->repo, p->version, host, p->description);
        return FG;
    } else if (strcmp(p->kind, "aur") == 0) {
        snprintf(out, size, "AUR %s%s  %s", p->version, host, p->description);
        return FG;
    } else if (strcmp(p->kind, "local") == 0) {
        snprintf(out, size, "LOCachy %s%s  %s", p->version, host, p->description);
        return FG;
    } else if (strcmp(p->kind, "unknown") == 0) {
        snprintf(out, size, "%s", _("not in the repositories; the AUR did not answer (no network?)"));
        return DIM;
    }
    snprintf(out, size, "%s", _("neither the repositories nor the AUR have it"));
    return ERROR;
}

/* Where its command is (in its build, the host's) into out: its colour. */
static uint32_t command_text(const struct form *f, char *out, size_t size)
{
    char path[600];
    struct stat st;
    if (!f->command[0]) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(out, size, "%s", f->built ? _("not set: Enter lists what its packages run")
                                           : _("not set: once it is built, Enter lists what its packages run"));
        return f->built ? ERROR : DIM;
    }
    snprintf(path, sizeof path, "%s/%s/upper%s", STATE, f->name, f->command);
    if (f->built && lstat(path, &st) == 0) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(out, size, "%s", _("in its build   Enter: what its packages run"));
        return FG;
    }
    if (lstat(f->command, &st) == 0) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(out, size, "%s", f->built ? _("the host's   Enter: what its packages run") : _("the host's"));
        return FG;
    }
    snprintf(out, size, "%s", f->built ? _("neither in its build nor on the host") : _("not on the host: from its build?"));
    return f->built ? ERROR : DIM;
}

/* s cut to width columns, keeping its end ("...end"): a path says most there. */
static void fit(char *s, size_t width)
{
    if (width <= 3 || (size_t)ui_cells(s) <= width) {
        return;
    }
    size_t len = strlen(s), start = len;
    int w = 0;
    while (start > 0) {
        size_t prev = start - 1;
        while (prev > 0 && ((unsigned char)s[prev] & 0xC0) == 0x80) prev--;
        char ch[8] = "";
        for (size_t k = 0; prev + k < start && k < 7; k++) ch[k] = s[prev + k];
        int cw = ui_cells(ch);
        if ((size_t)(w + cw) > width - 3) break;
        w += cw;
        start = prev;
    }
    memmove(s + 3, s + start, len - start + 1);
    memcpy(s, "...", 3);
}

/* A line's value and what is known of it: the latter's colour. */
static uint32_t form_row(const struct form *f, const struct form_line *l, char *value, size_t vsize, char *info,
                         size_t isize)
{
    char home_[256], dirs_[256], word[128];
    value[0] = info[0] = '\0';
    switch (l->field) {
    case F_PRESET:
        snprintf(value, vsize, "%s", f->preset[0] ? preset_name(f->preset) : _("(none)"));
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(info, isize, "%s", _("Enter: choose one; it fills the lines below"));
        return DIM;
    case F_NAME:
        snprintf(value, vsize, "%s", f->name[0] ? f->name : _("(none)"));
        snprintf(info, isize, "%s", f->is_new ? _("letters, digits, . _ - (from a letter or digit)") : _("fixed once made"));
        return DIM;
    case F_PACKAGE:
        snprintf(value, vsize, "%s", f->package[0] ? f->package : _("(none)"));
        if (!f->package[0]) {
            snprintf(info, isize, "%s", _("its program's: from the repositories or the AUR"));
            return DIM;
        }
        return package_text(f, f->package, info, isize);
    case F_EXTRAS:
        if (!nth_word(f->extras, l->index, word, sizeof word)) {
            snprintf(value, vsize, "%s", _("(none)"));
            snprintf(info, isize, "%s", _("more packages, separated by spaces"));
            return DIM;
        }
        snprintf(value, vsize, "%s", word);
        return package_text(f, word, info, isize);
    case F_COMMAND:
        snprintf(value, vsize, "%s", f->command[0] ? f->command : _("(none)"));
        return command_text(f, info, isize);
    case F_ARGUMENTS:
        snprintf(value, vsize, "%s", f->arguments[0] ? f->arguments : _("(none)"));
        snprintf(info, isize, "%s", _("separated by spaces; a file or URL it opens goes after them"));
        return DIM;
    case F_NETWORK:
        snprintf(value, vsize, "%s", f->network ? _("yes (locachy-net)") : _("no"));
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(info, isize, "%s", _("Enter: switch"));
        return DIM;
    case F_CAMERA:
        snprintf(value, vsize, "%s", f->camera ? _("yes") : _("no"));
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(info, isize, "%s", _("Enter: switch; the cameras (video calls), as they come and go; sound it has always"));
        return DIM;
    case F_DIRS:
        if (l->index == f->ndirs) {
            snprintf(value, vsize, "%s", _("(add one)"));
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(info, isize, "%s", _("Enter: a folder of your home, chosen in files"));
            return DIM;
        }
        snprintf(value, vsize, "%s", f->dirs[l->index]);
        /* TRANSLATORS: Delete is the Delete key; keep it. */
        snprintf(info, isize, "%s", _("at the same path in it   Delete: remove"));
        return DIM;
    case F_ALWAYS:
        open_user_dirs(home_, sizeof home_, dirs_, sizeof dirs_);
        snprintf(value, vsize, "%s", _("display, sound, GPU, fonts, its own bus"));
        snprintf(info, isize, _("and of your home: %s"), dirs_);
        return DIM;
    default:
        return FG;
    }
}

static void form_draw(struct form *f, const struct form_line *l, int n)
{
    char line[600], value[600], info[600];
    int cols = screen_cols(), rows_n = screen_rows(), height = rows_n - FORM_TOP - 3;
    if (f->sel < f->first) {
        f->first = f->sel;
    } else if (f->sel >= f->first + height) {
        f->first = f->sel - height + 1;
    }
    screen_fill(0, 0, cols, rows_n, BG);
    ui_hits_clear();
    screen_bar(_("packages"), NULL);
    snprintf(line, sizeof line, f->is_new ? _("A new container: %s   %s/%s.conf") : _("Container %s   %s/%s.conf"),
             f->name[0] ? f->name : _("(no name yet)"), CONF_DIR, f->name[0] ? f->name : "NAME");
    screen_text(1, 1, line, ACCENT, BG);
    for (int i = f->first; i < n && i < f->first + height; i++) {
        int r = FORM_TOP + i - f->first, on = i == f->sel;
        if (l[i].field == F_BUTTONS) {
            int x = FORM_VALUE;
            for (int b = 0; b < BUTTONS; b++) {
                int lit = on && b == f->button;
                snprintf(line, sizeof line, "[ %s ]", _(button_names[b]));
                int w = ui_cells(line);
                screen_text(x, r, line, lit ? BG : FG, lit ? ACCENT : BG);
                ui_hit(x, r, w, 1, HIT_BUTTON + b);
                x += w + 3;
            }
            continue;
        }
        uint32_t color = form_row(f, &l[i], value, sizeof value, info, sizeof info);
        fit(value, FORM_INFO - FORM_VALUE - 2);
        const char *label = i > 0 && l[i - 1].field == l[i].field ? "" : _(field_names[l[i].field]);
        screen_fill(0, r, cols, 1, on ? ACCENT : BG);
        screen_text(2, r, label, on ? BG : ACCENT, on ? ACCENT : BG);
        screen_text(FORM_VALUE, r, value, on ? BG : FG, on ? ACCENT : BG);
        screen_text(FORM_INFO, r, info, on ? BG : color, on ? ACCENT : BG);
        ui_hit(0, r, cols, 1, HIT_ROW + i);
    }
    /* TRANSLATORS: Up/Down, Enter, Delete, Left/Right and Esc are key names; keep them. */
    screen_text(1, rows_n - 2, _("Up/Down: line   Enter: edit, choose or switch   Delete: remove a directory   "
                                 "Left/Right: button   Esc: back, not saved"), FG, BG);
    screen_text(1, rows_n - 1, notice, notice_error ? ERROR : ACCENT, BG);
    screen_flush();
}

static void form_show(struct form *f)
{
    struct form_line l[FORM_LINES];
    form_draw(f, l, form_lines(f, l));
}

/* The selected line's value, edited where it is shown: 1 with Enter (text
 * holds it then), 0 with Esc (text as it was). Printable ASCII but ". */
static int form_edit(const struct form *f, char *text, size_t size)
{
    char copy[VALUE_MAX + 1], line[VALUE_MAX + 8];
    int row = FORM_TOP + f->sel - f->first, width = screen_cols() - FORM_VALUE - 2;
    snprintf(copy, sizeof copy, "%s", text);
    size_t pos = strlen(copy), limit = size < sizeof copy ? size : sizeof copy;
    for (;;) {
        size_t len = strlen(copy);
        if (pos > len) pos = len;
        int start = width > 1 && (int)pos >= width ? (int)pos - width + 1 : 0;
        int before = (int)pos - start;
        int after = (int)len - (int)pos;
        if (width > 1 && before + 1 + after > width) {
            after = width - before - 1;
        }
        snprintf(line, sizeof line, "%.*s_%.*s", before, copy + start, after > 0 ? after : 0, copy + pos);
        screen_fill(FORM_VALUE, row, screen_cols() - FORM_VALUE, 1, BG);
        screen_text(FORM_VALUE, row, line, ACCENT, BG);
        screen_flush();
        int key = key_get();
        if (key == 27) {
            return 0;
        } else if (key == '\n') {
            snprintf(text, size, "%s", copy);
            return 1;
        } else {
            edit_line_key(key, copy, limit, &pos);
        }
    }
}

/* Where its package and extras are, as they are now (locachy-pkg info: the
 * AUR is asked on the network, which may take a while). */
static void form_lookup(struct form *f)
{
    char cmd[1024], line[1024], *w[6];
    f->ninfo = 0;
    if (!f->package[0] && !f->extras[0]) {
        return;
    }
    if (!valid_packages(f->package, 1) || !valid_packages(f->extras, MAX_PKGS - 1)) {
        say_notice(1, "%s", _("Its packages are not package names: not looked up."));
        return;
    }
    say_notice(0, _("Looking up %s %s in the repositories and the AUR..."), f->package, f->extras);
    form_show(f);
    snprintf(cmd, sizeof cmd, "%s info %s %s 2>/dev/null", PKG, f->package, f->extras);
    FILE *in = popen(cmd, "r");
    while (in && f->ninfo < MAX_PKGS && fgets(line, sizeof line, in)) {
        int k = fields(line, w, 6);
        if (k < 2) {
            continue;
        }
        struct pkg_info *p = &f->info[f->ninfo++];
        snprintf(p->name, sizeof p->name, "%.127s", w[0]);
        snprintf(p->kind, sizeof p->kind, "%.11s", w[1]);
        snprintf(p->repo, sizeof p->repo, "%.39s", k > 2 ? w[2] : "");
        snprintf(p->version, sizeof p->version, "%.79s", k > 3 ? w[3] : "");
        snprintf(p->description, sizeof p->description, "%.199s", k > 4 ? w[4] : "");
        snprintf(p->host, sizeof p->host, "%.79s", k > 5 ? w[5] : "");
    }
    if (in) {
        pclose(in);
    }
    say_notice(0, "%s", "");
    for (int i = 0; i < f->ninfo; i++) {
        if (strcmp(f->info[i].kind, "none") == 0) {
            say_notice(1, _("Neither the repositories nor the AUR have %s."), f->info[i].name);
        }
    }
}

/* Its lines filled from a preset (or emptied, blank); the name too, unless
 * one was typed. */
static void form_preset(struct form *f)
{
    static char items[64][320];
    char names[63][64], old[64], typed[64];
    int n = preset_names(names, 63);
    for (int i = 0; i < n; i++) {
        char path[400];
        struct container c = { 0 };
        snprintf(path, sizeof path, "%s/%.63s.conf", PRESETS, names[i]);
        conf_read(path, container_value, &c);
        snprintf(items[i], sizeof items[i], "%-16.63s %.100s %.150s", names[i], c.package, c.extras);
    }
    snprintf(items[n], sizeof items[n], "%s", _("(blank)"));
    int at = choose(_("A new container: from a preset (its lines filled in; then it is yours), or blank"), items, n + 1);
    if (at < 0) {
        return;
    }
    snprintf(old, sizeof old, "%s", f->preset[0] ? preset_name(f->preset) : "");
    snprintf(typed, sizeof typed, "%s", f->name[0] && strcmp(f->name, old) != 0 ? f->name : "");
    f->preset[0] = f->package[0] = f->extras[0] = f->command[0] = f->arguments[0] = '\0';
    f->network = f->camera = f->ndirs = f->ninfo = 0;
    if (at < n) {
        snprintf(f->preset, sizeof f->preset, "%s/%.63s.conf", PRESETS, names[at]);
        conf_read(f->preset, form_value, f);
    }
    snprintf(f->name, sizeof f->name, "%.63s", typed[0] ? typed : at < n ? names[at] : "");
    f->changed = 1;
    form_lookup(f);
}

static void edit_command(struct form *f)
{
    char text[256];
    snprintf(text, sizeof text, "%s", f->command);
    if (!form_edit(f, text, sizeof text) || strcmp(text, f->command) == 0) {
        return;
    }
    if (strchr(text, ' ')) {
        say_notice(1, "%s", _("The command is one program; its arguments go on the next line."));
        return;
    }
    snprintf(f->command, sizeof f->command, "%s", text);
    f->changed = 1;
}

/* Built: what its packages run (locachy-pkg commands: their desktop entries,
 * then their programs), to choose from, or one typed. Not built: typed. */
static void choose_command(struct form *f)
{
    static char items[64][320], programs[64][256], arguments[64][256];
    char cmd[400], line[1024], title[200], *w[3];
    int n = 0;
    if (!f->built) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        say_notice(0, "%s", _("Once it is built, Enter here lists what its packages run."));
        edit_command(f);
        return;
    }
    snprintf(cmd, sizeof cmd, "%s commands %s 2>/dev/null", PKG, f->name);
    FILE *in = popen(cmd, "r");
    while (in && n < 63 && fgets(line, sizeof line, in)) {
        int k = fields(line, w, 3);
        if (!w[0][0]) {
            continue;
        }
        snprintf(programs[n], sizeof programs[n], "%.255s", w[0]);
        snprintf(arguments[n], sizeof arguments[n], "%.255s", k > 1 ? w[1] : "");
        snprintf(items[n], sizeof items[n], "%-24.24s %.150s %.120s", k > 2 ? w[2] : "", programs[n], arguments[n]);
        n++;
    }
    if (in) {
        pclose(in);
    }
    snprintf(items[n++], sizeof items[0], "%s", _("(type one)"));
    snprintf(title, sizeof title, _("The command of %s: what its packages run, as last built"), f->name);
    int at = choose(title, items, n);
    if (at == n - 1) {
        edit_command(f);
    } else if (at >= 0) {
        snprintf(f->command, sizeof f->command, "%s", programs[at]);
        snprintf(f->arguments, sizeof f->arguments, "%s", arguments[at]);
        f->changed = 1;
    }
}

/* Why path cannot be one of a container's directories (as locachy-app
 * refuses it when it starts) into why: 1; 0 if it can. */
static int directory_refused(const char *path, char *why, size_t size)
{
    char home_[256], dirs_[256], real[PATH_MAX], word[64];
    open_user_dirs(home_, sizeof home_, dirs_, sizeof dirs_);
    size_t hlen = strlen(home_);
    if (!home_[0] || strncmp(path, home_, hlen) != 0 || path[hlen] != '/' || !path[hlen + 1]) {
        snprintf(why, size, _("%.200s is not in your home (%.60s)."), path, home_);
        return 1;
    }
    if (strpbrk(path, ":\"")) {
        snprintf(why, size, _("%.200s has a : or a \" in its path."), path);
        return 1;
    }
    if (!realpath(path, real) || strcmp(real, path) != 0) {
        snprintf(why, size, _("%.200s goes through a link."), path);
        return 1;
    }
    const char *rel = path + hlen + 1;
    size_t len = strcspn(rel, "/");
    for (int i = 0; nth_word(dirs_, i, word, sizeof word); i++) {
        if (strlen(word) == len && strncmp(rel, word, len) == 0) {
            snprintf(why, size, strcmp(word, "Drives") == 0 ?
                     _("%.200s is in %s, which every container has already (as a whole: a drive in it could not be ejected).") :
                     _("%.200s is in %s, which every container has already."), path, word);
            return 1;
        }
    }
    return 0;
}

static void add_directory(struct form *f)
{
    char home_[256], dirs_[256], path[PATH_MAX], why[400], what[160];
    if (f->ndirs == MAX_DIRS) {
        say_notice(1, P_("%d directory at most.", "%d directories at most.", MAX_DIRS), MAX_DIRS);
        return;
    }
    open_user_dirs(home_, sizeof home_, dirs_, sizeof dirs_);
    /* TRANSLATORS: o is the o key; keep it. */
    snprintf(what, sizeof what, _("A folder of your home for %.60s: o on it."),
             f->name[0] ? f->name : _("the container"));
    if (!files_pick(home_, NULL, "inode/directory", what, path, sizeof path)) {
        return;
    }
    if (directory_refused(path, why, sizeof why)) {
        say_notice(1, "%s", why);
        return;
    }
    if (strlen(path) >= sizeof f->dirs[0]) {
        say_notice(1, _("%.200s: its path is too long."), path);
        return;
    }
    for (int i = 0; i < f->ndirs; i++) {
        if (strcmp(f->dirs[i], path) == 0) {
            say_notice(1, _("%.200s is there already."), path);
            return;
        }
    }
    snprintf(f->dirs[f->ndirs++], sizeof f->dirs[0], "%.255s", path);
    f->changed = 1;
    say_notice(0, _("%.200s: at the same path in it, from its next start."), path);
}

/* Its configuration written (CONF_DIR/NAME.conf; a new one from its preset,
 * whose comment it keeps): 0, or -1 with the notice saying why not. */
static int form_save(struct form *f)
{
    char path[320], dirs_[VALUE_MAX + 1] = "";
    size_t len = 0;
    struct stat st;
    if (!valid_name(f->name)) {
        say_notice(1, "%s", _("A name is letters, digits, . _ - (from a letter or digit)."));
        return -1;
    }
    snprintf(path, sizeof path, "%s/%s.conf", CONF_DIR, f->name);
    if (f->is_new && stat(path, &st) == 0) {
        say_notice(1, _("There is a container %s already."), f->name);
        return -1;
    }
    if (!f->package[0]) {
        say_notice(1, "%s", _("Its package is not set."));
        return -1;
    }
    for (int i = 0; i < f->ndirs; i++) {
        if (len + strlen(f->dirs[i]) + 1 > VALUE_MAX) {
            say_notice(1, P_("Its directories are more than a configuration line holds (%d character).",
                             "Its directories are more than a configuration line holds (%d characters).", VALUE_MAX),
                       VALUE_MAX);
            return -1;
        }
        len += (size_t)snprintf(dirs_ + len, sizeof dirs_ - len, "%s%s", i ? ":" : "", f->dirs[i]);
    }
    if (f->is_new && f->preset[0] && copy_file(f->preset, path) < 0) {
        say_notice(1, _("Cannot write %s."), path);
        return -1;
    }
    const char *values[][2] = {
        { "package", f->package }, { "extras", f->extras }, { "command", f->command },
        { "arguments", f->arguments }, { "network", f->network ? "1" : "0" }, { "camera", f->camera ? "1" : "0" },
        { "directories", dirs_ },
    };
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        if (conf_set(path, values[i][0], values[i][1]) < 0) {
            if (f->is_new) {
                unlink(path);
            }
            say_notice(1, _("Cannot write %s."), path);
            return -1;
        }
    }
    conf_set(path, "packages", NULL); /* the key from before package and extras */
    say_notice(0, _("%s saved (%s): it is so from its next start, its packages from its next build."), f->name, path);
    f->is_new = f->changed = 0;
    return 0;
}

/* Enter on line l: 1 when the form is done. */
static int form_enter(struct form *f, const struct form_line *l)
{
    char text[VALUE_MAX + 1];
    if (l->field == F_PRESET) {
        form_preset(f);
    } else if (l->field == F_NAME && !f->is_new) {
        say_notice(0, "%s", _("The name of a container is fixed once it is made."));
    } else if (l->field == F_NAME) {
        snprintf(text, sizeof text, "%s", f->name);
        if (!form_edit(f, text, sizeof f->name)) {
            return 0;
        } else if (text[0] && !valid_name(text)) {
            say_notice(1, "%s", _("A name is letters, digits, . _ - (from a letter or digit)."));
        } else {
            snprintf(f->name, sizeof f->name, "%.63s", text);
            f->changed = 1;
        }
    } else if (l->field == F_PACKAGE || l->field == F_EXTRAS) {
        int one = l->field == F_PACKAGE;
        char *to = one ? f->package : f->extras;
        size_t size = one ? sizeof f->package : sizeof f->extras;
        snprintf(text, sizeof text, "%s", to);
        if (!form_edit(f, text, size) || strcmp(text, to) == 0) {
            return 0;
        } else if (!valid_packages(text, one ? 1 : MAX_PKGS - 1)) {
            if (one) {
                say_notice(1, "%s", _("One package: letters, digits and @ . _ + -."));
            } else {
                say_notice(1, P_("Packages separated by spaces, %d at most: letters, digits and @ . _ + -.",
                                 "Packages separated by spaces, %d at most: letters, digits and @ . _ + -.", MAX_PKGS - 1),
                           MAX_PKGS - 1);
            }
        } else {
            snprintf(to, size, "%s", text);
            f->changed = 1;
            form_lookup(f);
        }
    } else if (l->field == F_COMMAND) {
        choose_command(f);
    } else if (l->field == F_ARGUMENTS) {
        snprintf(text, sizeof text, "%s", f->arguments);
        if (form_edit(f, text, sizeof f->arguments) && strcmp(text, f->arguments) != 0) {
            snprintf(f->arguments, sizeof f->arguments, "%.255s", text);
            f->changed = 1;
        }
    } else if (l->field == F_NETWORK) {
        f->network = !f->network;
        f->changed = 1;
    } else if (l->field == F_CAMERA) {
        f->camera = !f->camera;
        f->changed = 1;
    } else if (l->field == F_DIRS) {
        add_directory(f);
    } else if (l->field == F_BUTTONS && f->button == B_CANCEL) {
        if (f->changed) {
            say_notice(0, _("%s: not saved."), f->name[0] ? f->name : _("The new container"));
        }
        return 1;
    } else if (l->field == F_BUTTONS) {
        if (form_save(f) < 0) {
            return 0;
        }
        if (f->button == B_BUILD) {
            char state[16], label[160];
            char *argv[] = { APP, "build", f->name, NULL };
            read_state(f->name, state, sizeof state);
            if (strcmp(state, "closed") != 0) {
                /* TRANSLATORS: b is the b key; keep it. */
                say_notice(1, _("%s saved; it is %s: stop it (home), then b builds it."), f->name, state_label(state));
            } else {
                snprintf(label, sizeof label, _("build %s"), f->name);
                start_job(label, argv);
            }
        }
        return 1;
    }
    return 0;
}

/* The form of the container name (NULL: a new one, or pre-filled from seed),
 * until it is left. */
static void edit_container_with(const char *name, const struct form *seed)
{
    static struct form f;
    struct form_line l[FORM_LINES];
    char path[320];
    struct stat st;
    memset(&f, 0, sizeof f);
    f.is_new = name == NULL;
    if (seed) {
        f = *seed;
        form_lookup(&f);
        int n0 = form_lines(&f, l);
        for (int i = 0; i < n0; i++) {
            if (l[i].field == F_BUTTONS) f.sel = i;
        }
    } else if (name) {
        snprintf(f.name, sizeof f.name, "%s", name);
        snprintf(path, sizeof path, "%s/%s.conf", CONF_DIR, name);
        conf_read(path, form_value, &f);
        snprintf(path, sizeof path, "%s/%s/built", STATE, name);
        f.built = stat(path, &st) == 0;
        form_lookup(&f);
    }
    int quiet = 0; /* a motion: nothing to draw */
    for (;;) {
        int n = form_lines(&f, l);
        if (f.sel >= n) {
            f.sel = n - 1;
        }
        if (!quiet) form_draw(&f, l, n);
        quiet = 0;
        int key = key_poll_timeout(-1, job ? 500 : 2000);
        check_job();
        if (key == K_POINTER) { /* a click picks a line (a double click: Enter), or a button */
            const struct pointer_event *e = pointer_last();
            int id = e->button == BTN_LEFT && e->pressed ? ui_hit_at(e->col, e->row) : -1;
            key = e->wheel > 0 ? K_UP : e->wheel < 0 ? K_DOWN : 0;
            if (id >= HIT_BUTTON && id < HIT_KEY) {
                for (int i = 0; i < n; i++) if (l[i].field == F_BUTTONS) f.sel = i;
                f.button = id - HIT_BUTTON;
                key = '\n';
            } else if (id >= HIT_ROW && id < n && l[id].field != F_ALWAYS && l[id].field != F_BUTTONS) {
                f.sel = id;
                key = e->clicks == 2 ? '\n' : 0;
            } else if (!e->wheel) {
                quiet = 1;
                continue;
            }
        }
        if (key == 27 || key == 'q') {
            if (f.changed) {
                say_notice(0, _("%s: not saved."), f.name[0] ? f.name : _("The new container"));
            }
            break;
        } else if (key == K_UP || key == K_DOWN) {
            int step = key == K_UP ? -1 : 1, to = f.sel + step;
            if (to >= 0 && to < n && l[to].field == F_ALWAYS) {
                to += step;
            }
            if (to >= 0 && to < n) {
                f.sel = to;
            }
        } else if ((key == K_LEFT || key == K_RIGHT) && l[f.sel].field == F_BUTTONS) {
            f.button = (f.button + (key == K_LEFT ? BUTTONS - 1 : 1)) % BUTTONS;
        } else if (key == K_DEL && l[f.sel].field == F_DIRS && l[f.sel].index < f.ndirs) {
            int i = l[f.sel].index;
            memmove(f.dirs[i], f.dirs[i + 1], sizeof f.dirs[0] * (size_t)(f.ndirs - i - 1));
            f.ndirs--;
            f.changed = 1;
        } else if (key == '\n' && form_enter(&f, &l[f.sel])) {
            break;
        }
    }
    load_containers();
    for (int i = 0; i < nconts; i++) {
        if (strcmp(conts[i].name, f.name) == 0) {
            sel[CONTAINERS] = i;
        }
    }
}

static void edit_container(const char *name)
{
    edit_container_with(name, NULL);
}

struct search_hit {
    char name[64], kind[16], package[128], extras[256], command[256], arguments[256];
    int network;
};

static int run_search(const char *prompt_label, struct search_hit *hits, char items[][320], int max, char *query,
                      size_t qsize)
{
    query[0] = '\0';
    if (!prompt(prompt_label, query, qsize) || !query[0]) {
        return -1;
    }
    if (!valid_packages(query, 8)) {
        say_notice(1, "%s", _("Search terms: letters, digits and @ . _ + -."));
        return -1;
    }
    say_notice(0, _("Searching recipes, repositories and the AUR for %s..."), query);
    draw();
    char cmd[1024], line[1024], *w[8];
    snprintf(cmd, sizeof cmd, "LOCACHY_PRESETS=%s %s search %s 2>/dev/null", PRESETS, PKG, query);
    FILE *in = popen(cmd, "r");
    int n = 0;
    while (in && n < max && fgets(line, sizeof line, in)) {
        int k = fields(line, w, 8);
        if (k < 3 || !w[0][0]) continue;
        struct search_hit *h = &hits[n];
        memset(h, 0, sizeof *h);
        snprintf(h->name, sizeof h->name, "%.63s", w[0]);
        snprintf(h->kind, sizeof h->kind, "%.15s", w[1]);
        snprintf(h->package, sizeof h->package, "%.127s", w[2]);
        snprintf(h->extras, sizeof h->extras, "%.255s", k > 3 ? w[3] : "");
        snprintf(h->command, sizeof h->command, "%.255s", k > 4 ? w[4] : "");
        snprintf(h->arguments, sizeof h->arguments, "%.255s", k > 5 ? w[5] : "");
        h->network = k <= 6 || strcmp(w[6], "0") != 0;
        snprintf(items[n], 320, "[%-6.6s] %-22.22s %.260s", h->kind, h->name, k > 7 ? w[7] : h->package);
        n++;
    }
    if (in) pclose(in);
    if (n == 0) {
        say_notice(1, _("Nothing matched %s."), query);
        return -1;
    }
    say_notice(0, "%s", "");
    char title[240];
    /* TRANSLATORS: Enter is the Enter key; keep it. */
    snprintf(title, sizeof title, _("Search results for \"%s\" (Enter: choose)"), query);
    return choose(title, items, n);
}

static void search_container(void)
{
    static struct search_hit hits[64];
    static char items[64][320];
    char query[128];
    int at = run_search(_("Search container recipes, repositories and the AUR:"), hits, items, 64, query, sizeof query);
    if (at < 0) return;
    struct form seed = { 0 };
    seed.is_new = 1;
    seed.changed = 1;
    seed.button = B_BUILD;
    snprintf(seed.name, sizeof seed.name, "%s", hits[at].name);
    for (char *c = seed.name; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.') *c = '-';
    }
    if (strcmp(hits[at].kind, "recipe") == 0) {
        snprintf(seed.preset, sizeof seed.preset, "%s/%.63s.conf", PRESETS, hits[at].name);
    }
    snprintf(seed.package, sizeof seed.package, "%s", hits[at].package);
    snprintf(seed.extras, sizeof seed.extras, "%s", hits[at].extras);
    snprintf(seed.command, sizeof seed.command, "%s", hits[at].command);
    snprintf(seed.arguments, sizeof seed.arguments, "%s", hits[at].arguments);
    seed.network = hits[at].network;
    edit_container_with(NULL, &seed);
}

static void container_key(int key)
{
    if (key == '/') {
        search_container();
        return;
    } else if (key == 'n') {
        edit_container(NULL);
        return;
    } else if (key == 'l') {
        show_log = !show_log;
        return;
    }
    if (sel[CONTAINERS] >= nconts) {
        return;
    }
    struct container *c = &conts[sel[CONTAINERS]];
    char text[400], label[160];
    if (key == '\n') {
        char name[64];
        snprintf(name, sizeof name, "%s", c->name); /* conts is loaded anew meanwhile */
        edit_container(name);
    } else if (key == 'b' || key == 'r') {
        char *argv[] = { APP, key == 'b' ? "build" : "rollback", c->name, NULL };
        snprintf(label, sizeof label, key == 'b' ? _("build %s") : _("roll back %s"), c->name);
        if (strcmp(c->state, "closed") != 0) {
            say_notice(1, _("%s is %s: stop it (home) first."), c->name, state_label(c->state));
        } else if (key == 'b' || yes(_("Roll back to its previous build?"))) {
            start_job(label, argv);
        }
    } else if (key == 'd') {
        snprintf(text, sizeof text, _("Remove %s (its configuration and builds)?"), c->name);
        if (strcmp(c->state, "closed") != 0) {
            say_notice(1, _("%s is %s: stop it (home) first."), c->name, state_label(c->state));
        } else if (yes(text)) {
            snprintf(text, sizeof text, _("Its home too (%s/%s/data: its settings, its files)?"), STATE, c->name);
            int data = yes(text);
            char *argv[] = { APP, "remove", c->name, data ? "--data" : NULL, NULL };
            snprintf(label, sizeof label, data ? _("remove %s and its home") : _("remove %s"), c->name);
            start_job(label, argv);
        }
    }
}

static void host_key(int key)
{
    char text[128] = "", label[160];
    if (key == '/') {
        static struct search_hit hits[64];
        static char items[64][320];
        int at = run_search(_("Search repositories and the AUR:"), hits, items, 64, text, sizeof text);
        if (at >= 0 && hits[at].package[0]) {
            char *argv[] = { PKG, "install", hits[at].package, NULL };
            snprintf(label, sizeof label, _("add %s to the host"), hits[at].package);
            start_job(label, argv);
        }
    } else if (key == 'i') {
        if (prompt(_("Package to add (repositories or the AUR):"), text, sizeof text) && text[0]) {
            char *argv[] = { PKG, "install", text, NULL };
            snprintf(label, sizeof label, _("add %s to the host"), text);
            start_job(label, argv);
        }
    } else if (key == 'x' && sel[HOST] < nleaves) {
        char *name = leaves[sel[HOST]];
        snprintf(text, sizeof text, _("Remove %s from the host (and what only it needs)?"), name);
        if (yes(text)) {
            char *argv[] = { PKG, "remove", name, NULL };
            snprintf(label, sizeof label, _("remove %s from the host"), name);
            start_job(label, argv);
        }
    } else if (key == 's' && yes(_("Install what the manifest names and remove what it does not?"))) {
        char *argv[] = { PKG, "sync", NULL };
        start_job(_("bring the host to its manifest"), argv);
    }
}

static void updates_key(int key)
{
    if (key == 'c') {
        char *argv[] = { PKG, "plan", NULL };
        start_job(_("check for updates"), argv);
    } else if ((key == ' ' || key == '\n') && sel[UPDATES] < ntargets) {
        targets[sel[UPDATES]].marked = !targets[sel[UPDATES]].marked;
    } else if (key == 'a') {
        char *argv[MAX_TARGETS + 3], question[512];
        snprintf(question, sizeof question, "%s", _("Apply:"));
        int n = 0;
        argv[n++] = PKG;
        argv[n++] = "apply";
        for (int i = 0; i < ntargets; i++) {
            if (targets[i].marked) {
                argv[n++] = targets[i].name;
                strncat(question, " ", sizeof question - strlen(question) - 1);
                strncat(question, targets[i].name, sizeof question - strlen(question) - 1);
            }
        }
        argv[n] = NULL;
        if (n == 2) {
            /* TRANSLATORS: Space is the Space key; keep it. */
            say_notice(1, "%s", _("Nothing is marked (Space marks)."));
        } else if (yes(question)) {
            start_job(question, argv);
        }
    }
}

/* What a pointer event does on a page, as a key: -1 to redraw, 0 nothing. */
static int pointer_key(int n)
{
    const struct pointer_event *e = pointer_last();
    if (e->wheel) {
        if (ui_hit_at(e->col, e->row) == HIT_LOWER && page == CONTAINERS && !job && !show_log) {
            details_from -= 3 * e->wheel; /* draw_lower keeps it in range */
        } else if (ui_hit_at(e->col, e->row) == HIT_LOWER) {
            log_from += 3 * e->wheel;
            log_from = log_from < 0 ? 0 : log_from > log_total - 1 ? (log_total > 0 ? log_total - 1 : 0) : log_from;
        } else {
            sel[page] -= e->wheel;
            sel[page] = sel[page] < 0 ? 0 : sel[page] >= n ? (n > 0 ? n - 1 : 0) : sel[page];
        }
        return -1;
    }
    if (e->button != BTN_LEFT || !e->pressed) return 0;
    int id = ui_hit_at(e->col, e->row);
    if (id >= HIT_KEY) return id - HIT_KEY;
    if (id >= HIT_TAB && id < HIT_LOWER) {
        page = id - HIT_TAB;
        return -1;
    }
    if (id >= HIT_ROW && id < n) {
        sel[page] = id;
        return e->clicks == 2 ? '\n' : -1;
    }
    return 0;
}

int main(void)
{
    screen_open(BASE_FONT);
    keys_open();
    find_job();
    load_all();
    if (!notice[0]) {
        /* TRANSLATORS: c is the c key; keep it. */
        say_notice(0, "%s", checked[0] ? _("The last check is shown; c checks again.") : _("Not checked yet: c checks."));
    }
    draw();
    for (;;) {
        int key = key_poll_timeout(-1, job ? 500 : 2000);
        check_job();
        int n = list_len();
        if (key == K_POINTER) {
            key = pointer_key(n);
            if (key == 0) continue;
        }
        if (key == 'q' || key == 27) {
            return 0;
        } else if (key == '\t') {
            page = (page + 1) % PAGES;
        } else if (key == K_UP && sel[page] > 0) {
            sel[page]--;
        } else if (key == K_DOWN && sel[page] < n - 1) {
            sel[page]++;
        } else if ((key == K_PGUP || key == K_PGDN) && (job || show_log)) { /* the output pane scrolls */
            int step = (screen_rows() - 6) / 2 - 2;
            step = step > 0 ? step : 1;
            log_from += key == K_PGUP ? step : -step;
            log_from = log_from < 0 ? 0 : log_from > log_total - 1 ? (log_total > 0 ? log_total - 1 : 0) : log_from;
        } else if ((key == K_PGUP || key == K_PGDN) && page == CONTAINERS && n > 0 &&
                   details_total > details_rows) { /* its details, more than they fit, scroll */
            int step = details_rows > 1 ? details_rows - 1 : 1;
            details_from += key == K_PGUP ? -step : step; /* draw_lower keeps it in range */
        } else if ((key == K_PGUP || key == K_PGDN) && n > 0) {
            int step = (screen_rows() - 6) / 2;
            step = step > 0 ? step : 1;
            sel[page] += key == K_PGUP ? -step : step;
            sel[page] = sel[page] < 0 ? 0 : sel[page] >= n ? n - 1 : sel[page];
        } else if (key == K_BOL) {
            sel[page] = 0;
        } else if (key == K_EOL && n > 0) {
            sel[page] = n - 1;
        } else if (key == 'l') {
            show_log = !show_log;
        } else if (key == K_SHOWN) {
            load_all();
        } else if (key > 0 && page == UPDATES) {
            updates_key(key);
        } else if (key > 0 && page == CONTAINERS) {
            container_key(key);
        } else if (key > 0 && page == HOST) {
            host_key(key);
        }
        draw();
    }
}
