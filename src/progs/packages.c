/*
 * packages: installs and updates of the host and the containers, shown before
 * they are done. The work is locachy-pkg's, locachy-app's and
 * locachy-build's; this shows what they would do and starts them. Three
 * pages, Tab between them:
 *   Updates     where updates come from (LOCachy's source, the app
 *               presets), then the last plan (locachy-pkg plan): a row for
 *               the host, for LOCachy, for the app presets and for each
 *               container, with what an update would do there; c checks
 *               again (on the network), Space marks, a applies the marked
 *               ones, Enter shows a row's packages
 *   Containers  their configurations (/etc/locachy/containers) and builds:
 *               n new (from a preset: the system's host programs, the app
 *               presets' catalog), Enter edits one: its form, a line for each thing it
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
 * What takes time is a system job (locachy-jobs, tools/JOBS.md): queued with
 * the others, one at a time, in a cgroup of its own; it goes on while home
 * is on the screen, or after this program has ended, is never frozen, and
 * home lists it. This program follows one of its own at a time; reopened,
 * it shows the last one it asked for. Its output is shown in the lower pane
 * (a background of its own, under a title bar: what runs, how it ended,
 * which lines; PgUp/PgDn scroll it).
 * The lower pane and the notice can be selected with the pointer, as in term
 * (textsel.h), and copied: y or Ctrl+C what is selected (else the command
 * below, else the notice), Y all the pane holds.
 * A build that stops for source signing keys (locachy-build's "Source key to
 * trust" lines, exit status 3) is not a failure but a person's decision: the
 * pane lists the keys (each one's package, fingerprint, and the recipe whose
 * validpgpkeys name it) and the command that trusts them, which y copies; a
 * person checks them and runs it as root, and g then continues the job: a
 * new job, queued, with what is left of it.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base.h"
#include "files.h"
#include "jobs.h"
#include "textsel.h"

#define FG     0xD0D0D0
#define BG     0x101820
#define ACCENT 0x80C0FF
#define DIM    0x808890
#define ERROR  0xFF9090
#define WARN   0xE0C060
#define PANE   0x05080C /* the output pane: a terminal's black, apart from the lists */

#ifndef PKG /* the places: another for each in tools/test-packages.c */
/* The presets: the system's (its host programs), the app presets' catalog
 * (from their own repository; CATALOG in it says which). */
#define SYSTEM_PRESETS "/usr/local/share/locachy/containers"
#define APPS_CATALOG   "/var/lib/locachy/apps/catalog"
#define SOURCE_COMMIT  "/usr/local/lib/locachy/source-commit" /* the LOCachy source installed */
#define SOURCE_PENDING "/usr/local/lib/locachy/source-pending" /* in place, its update not finished */
#define RC_FILE RC_CONF /* where updates come from: update_repo, update_branch, apps_repo, apps_branch */
#define STATE   "/var/lib/locachy/containers"
#define CONF_DIR CONTAINERS_CONF
#define PKG "/usr/local/bin/locachy-pkg"
#define APP "/usr/local/bin/locachy-app"
#define UNTRUSTED_KEYS "/var/lib/locachy/builder/untrusted-keys" /* locachy-build's: the keys not trusted yet */
#define RECIPES "/usr/local/src/locachy/pkgbuilds" /* LOCachy's own recipes */
#endif
#define TRUST_TOOL "locachy-build --trust-key"
#define MANIFEST "/etc/locachy/packages"
/* As locachy-pkg takes them when rc.conf does not say. */
#define DEFAULT_UPDATE_REPO "https://github.com/niuyuanzhao/locachy.git"
#define DEFAULT_APPS_REPO   "https://github.com/niuyuanzhao/locachy-apps.git"

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

/* The plan: target, kind (repo, aur, rebuild, state; the app presets'
 * catalog, merge), what, old, new. */
struct row {
    char target[64], kind[16], what[200], old[80], new_[80];
};
static struct row rows[MAX_ROWS];
static int nrows;
static char checked[40];

struct target {
    char name[64], state[16], why[200], note[300]; /* note: what the check could not do */
    int repo, aur, rebuild, missing, merge, marked; /* missing: declared leaves to install; merge: configurations
                                                        that follow their new presets */
};
static struct target targets[MAX_TARGETS];
static int ntargets;

/* Where updates come from, a line each above the list: LOCachy's source
 * (its commit installed) and the app presets (their catalog's commit and
 * origin); apps_on: rc.conf does not turn the app presets' updates off. */
static char source_line[600], apps_line[600];
static int apps_on, apps_missing;

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

/* The job: the queue's id of the one this program follows, until it has
 * ended (0: none); its output (the last one asked for, ended or not); how
 * the last one ended (-1 none yet, 0 done, 1 failed, 2 stopped from home);
 * while it has not ended, its state as the queue said it last (queued,
 * running, stopping). */
static int job;
static int job_result = -1;
static char job_label[160], job_log[512], job_state[16];
/* Its kind and arguments (each followed by a NUL in job_args), to continue
 * it; job_kind "": it cannot be. */
#define JOB_ARGV (MAX_TARGETS + 8)
static char job_kind[24], job_args[4096], *job_argv[JOB_ARGV + 1];

/* The source signing keys the last job stopped for, in its output's order;
 * trusted: no longer in UNTRUSTED_KEYS. */
#define MAX_KEYS 32
struct wanted_key {
    char name[64], fpr[72];
    int trusted;
};
static struct wanted_key wanted[MAX_KEYS];
static int nwanted;

static char said[512]; /* the notice to copy: the last one said, not what a copy says of itself */

static void say_notice(int error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say_notice(int error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(notice, sizeof notice, fmt, ap);
    va_end(ap);
    notice_error = error;
    snprintf(said, sizeof said, "%s", notice);
}

static int valid_packages(const char *s, int max);

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

/* rc.conf's update_repo, update_branch, apps_repo, apps_branch into arg's
 * four values (none there: what they already are). */
static void source_value(const char *name, const char *value, void *arg)
{
    static const char *const keys[4] = { "update_repo", "update_branch", "apps_repo", "apps_branch" };
    char (*values)[256] = arg;
    for (int i = 0; i < 4; i++) {
        if (strcmp(name, keys[i]) == 0) {
            snprintf(values[i], 256, "%s", value);
        }
    }
}

/* The app presets' CATALOG: its origin and commit into arg's two values. */
static void catalog_value(const char *name, const char *value, void *arg)
{
    char (*values)[64] = arg;
    if (strcmp(name, "origin") == 0) {
        snprintf(values[0], 64, "%s", value);
    } else if (strcmp(name, "commit") == 0) {
        snprintf(values[1], 64, "%s", value);
    }
}

/* rc.conf's "", "0" or "off": no updates from there. */
static int repo_off(const char *repo)
{
    return !repo[0] || strcmp(repo, "0") == 0 || strcmp(repo, "off") == 0;
}

/* A repository and its branch as an update follows them, or off. */
static void follows(char *out, size_t size, const char *repo, const char *branch)
{
    if (repo_off(repo)) {
        snprintf(out, size, "%s", _("off"));
    } else {
        snprintf(out, size, "%.250s (%.120s)", repo, branch[0] ? branch : "main");
    }
}

/* The two lines of where updates come from: what is installed first, the
 * repository last (a long one cuts only itself). */
/* The first word of file path's first line, or "". */
static void read_word(const char *path, char *out, size_t size)
{
    out[0] = '\0';
    FILE *f = fopen(path, "re");
    if (f) {
        if (!fgets(out, (int)size, f)) {
            out[0] = '\0';
        }
        fclose(f);
    }
    out[strcspn(out, " \t\r\n")] = '\0';
}

static void load_sources(void)
{
    char rc[4][256] = { DEFAULT_UPDATE_REPO, "", DEFAULT_APPS_REPO, "" }, catalog[2][64] = { "", "" };
    char from[400], commit[64], pending[64], installed[160], path[512];
    conf_read(RC_FILE, source_value, rc);
    read_word(SOURCE_COMMIT, commit, sizeof commit);
    read_word(SOURCE_PENDING, pending, sizeof pending);
    follows(from, sizeof from, rc[0], rc[1]);
    if (commit[0] && pending[0]) { /* an update that stopped: in place, not all applied */
        snprintf(source_line, sizeof source_line, _("LOCachy source: %.7s installed, %.7s not finished; updates: %s"),
                 commit, pending, from);
    } else if (pending[0]) {
        snprintf(source_line, sizeof source_line,
                 _("LOCachy source: its commit is not recorded, %.7s not finished; updates: %s"), pending, from);
    } else if (commit[0]) {
        snprintf(source_line, sizeof source_line, _("LOCachy source: %.7s installed; updates: %s"), commit, from);
    } else {
        snprintf(source_line, sizeof source_line, _("LOCachy source: its commit is not recorded; updates: %s"), from);
    }
    snprintf(path, sizeof path, "%s/CATALOG", APPS_CATALOG);
    apps_missing = conf_read(path, catalog_value, catalog) < 0;
    if (apps_missing) {
        snprintf(installed, sizeof installed, "%s", _("none installed"));
    } else if (strcmp(catalog[0], "bootstrap") == 0) {
        snprintf(installed, sizeof installed, "%s", _("from the system, not yet from the apps repository"));
    } else if (strcmp(catalog[0], "medium") == 0) {
        snprintf(installed, sizeof installed, _("%.7s from the installation medium"), catalog[1][0] ? catalog[1] : "?");
    } else {
        snprintf(installed, sizeof installed, _("%.7s from the apps repository"), catalog[1][0] ? catalog[1] : "?");
    }
    follows(from, sizeof from, rc[2], rc[3]);
    apps_on = !repo_off(rc[2]);
    snprintf(apps_line, sizeof apps_line, _("App presets: %s; updates: %s"), installed, from);
}

static void load_plan(void)
{
    char line[600], *f[5];
    nrows = ntargets = 0;
    checked[0] = '\0';
    target("host");
    target("locachy");
    if (apps_on) {
        target("apps");
    }
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
        } else if (strcmp(r->kind, "merge") == 0) {
            t->merge++;
        } else if (strcmp(r->kind, "state") == 0) {
            snprintf(t->state, sizeof t->state, "%s", r->what);
        } else if (strcmp(r->kind, "note") == 0) { /* each one: the app presets can have two */
            size_t len = strlen(t->note);
            snprintf(t->note + len, sizeof t->note - len, "%s%s", len ? "; " : "", r->what);
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
        snprintf(path, sizeof path, "%s/%s/built", STATE, c->name);
        conf_read(path, built_value, c);
        snprintf(path, sizeof path, "%s/%s/upper.prev", STATE, c->name);
        c->prev = stat(path, &st) == 0;
        read_state(c->name, c->state, sizeof c->state);
        if (!c->host) load_packages(c);
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
    load_sources();
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

/* The job's kind and arguments, kept to continue it. Too many to keep:
 * none, it cannot be continued. */
static void keep_args(const char *kind, char *const args[])
{
    size_t len = 0;
    int n = 0;
    snprintf(job_kind, sizeof job_kind, "%s", kind);
    for (; args[n]; n++) {
        size_t size = strlen(args[n]) + 1;
        if (n == JOB_ARGV || len + size > sizeof job_args) {
            job_kind[0] = '\0';
            n = 0;
            break;
        }
        memcpy(job_args + len, args[n], size);
        job_argv[n] = job_args + len;
        len += size;
    }
    job_argv[n] = NULL;
}

/* The queue's kind of argv (PKG's or APP's command, its own arguments from
 * argv[2]); NULL if it is none. */
static const char *job_kind_of(char *const argv[])
{
    static const char *const pkg[][2] = { { "install", "pkg-install" }, { "remove", "pkg-remove" },
                                          { "sync", "pkg-sync" }, { "plan", "pkg-plan" }, { "apply", "pkg-apply" } };
    static const char *const app[][2] = { { "build", "app-build" }, { "rollback", "app-rollback" },
                                          { "remove", "app-remove" } };
    int of_pkg = strcmp(argv[0], PKG) == 0;
    if (!argv[1] || (!of_pkg && strcmp(argv[0], APP) != 0)) {
        return NULL;
    }
    for (size_t i = 0; of_pkg && i < sizeof pkg / sizeof pkg[0]; i++) {
        if (strcmp(argv[1], pkg[i][0]) == 0) return pkg[i][1];
    }
    for (size_t i = 0; !of_pkg && i < sizeof app / sizeof app[0]; i++) {
        if (strcmp(argv[1], app[i][0]) == 0) return app[i][1];
    }
    return NULL;
}

/* A system job of kind with args (continued: what was left of one that
 * stopped for keys), the one this program follows now. */
static void submit_job(const char *label, const char *kind, char *const args[], int continued)
{
    char why[256] = "";
    int id = 0, position = 0;
    if (job) {
        say_notice(1, _("Wait: %s is still running."), job_label);
        return;
    }
    if (jobs_submit(kind, (const char *const *)args, &id, &position, why, sizeof why) < 0) {
        say_notice(1, _("Cannot start %s: %s"), label, why);
        return;
    }
    job = id;
    job_result = -1;
    snprintf(job_state, sizeof job_state, "%s", position ? "queued" : "running");
    log_from = 0;
    show_log = 1;
    nwanted = 0;
    snprintf(job_label, sizeof job_label, "%s", label);
    jobs_log_path(id, job_log, sizeof job_log);
    keep_args(kind, args);
    if (continued) {
        say_notice(0, _("%s: continuing; it goes on if you leave."), label);
    } else if (position) {
        say_notice(0, _("%s: queued (%d before it); it goes on if you leave."), label, position);
    } else {
        say_notice(0, _("%s: running; it goes on if you leave."), label);
    }
}

/* The job argv (PKG's or APP's command). */
static void start_job(const char *label, char *const argv[])
{
    const char *kind = job_kind_of(argv);
    if (!kind) {
        say_notice(1, _("Cannot start %s."), label);
        return;
    }
    submit_job(label, kind, argv + 2, 0);
}

/* A fingerprint, as locachy-build writes them: 40 or 64 hexadecimal digits,
 * upper case (nothing else goes into a command to copy). */
static int valid_fpr(const char *s)
{
    size_t n = strspn(s, "0123456789ABCDEF");
    return (n == 40 || n == 64) && s[n] == '\0';
}

/* The source signing keys the last job stopped for: locachy-build's "Source
 * key to trust: NAME FINGERPRINT" lines in its output; how many. */
static int parse_keys(void)
{
    char *line = NULL, name[64], fpr[72], more;
    size_t cap = 0;
    FILE *f = job_log[0] ? fopen(job_log, "re") : NULL;
    nwanted = 0;
    while (f && getline(&line, &cap, f) > 0) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "Source key to trust: ", 21) != 0 || sscanf(line + 21, "%63s %71s %c", name, fpr, &more) != 2 ||
            !valid_fpr(fpr) || !valid_packages(name, 1)) {
            continue;
        }
        int known = 0;
        for (int i = 0; i < nwanted && !known; i++) {
            known = strcmp(wanted[i].name, name) == 0 && strcmp(wanted[i].fpr, fpr) == 0;
        }
        if (!known && nwanted < MAX_KEYS) {
            memcpy(wanted[nwanted].name, name, sizeof name);
            memcpy(wanted[nwanted].fpr, fpr, sizeof fpr);
            wanted[nwanted++].trusted = 0;
        }
    }
    free(line);
    if (f) {
        fclose(f);
    }
    return nwanted;
}

/* Which of them a person has trusted since: locachy-build --trust-key takes
 * each one it trusts out of UNTRUSTED_KEYS. Without that file none is taken
 * as trusted. */
static void keys_refresh(void)
{
    char text[8192];
    FILE *f = fopen(UNTRUSTED_KEYS, "re");
    size_t n = f ? fread(text, 1, sizeof text - 2, f) : 0;
    if (n && text[n - 1] != '\n') {
        text[n++] = '\n';
    }
    text[n] = '\0';
    for (int i = 0; i < nwanted; i++) {
        char line[80];
        snprintf(line, sizeof line, " %s\n", wanted[i].fpr);
        wanted[i].trusted = f && !strstr(text, line);
    }
    if (f) {
        fclose(f);
    }
}

static int by_text(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The command that trusts those not trusted yet, as locachy-build prints it
 * ("Trust them: ..."): their fingerprints sorted, each once; "" if none is
 * left. */
#define COMMAND_MAX (sizeof TRUST_TOOL + MAX_KEYS * 65)
static void trust_command(char *out, size_t size)
{
    const char *fprs[MAX_KEYS];
    int n = 0;
    for (int i = 0; i < nwanted; i++) {
        int known = wanted[i].trusted;
        for (int k = 0; k < n && !known; k++) {
            known = strcmp(fprs[k], wanted[i].fpr) == 0;
        }
        if (!known) {
            fprs[n++] = wanted[i].fpr;
        }
    }
    qsort(fprs, (size_t)n, sizeof fprs[0], by_text);
    size_t len = (size_t)snprintf(out, size, "%s", n ? TRUST_TOOL : "");
    for (int i = 0; i < n && len < size; i++) {
        len += (size_t)snprintf(out + len, size - len, " %s", fprs[i]);
    }
}

/* A fingerprint as gpg shows it: groups of four, two spaces at the middle. */
static void group_fpr(const char *fpr, char *out, size_t size)
{
    size_t n = strlen(fpr), len = 0;
    for (size_t i = 0; i < n && len + 3 < size; i++) {
        if (i && i % 4 == 0) {
            out[len++] = ' ';
            if (i == n / 2) {
                out[len++] = ' ';
            }
        }
        out[len++] = fpr[i];
    }
    out[len] = '\0';
}

/* Where a key comes from: the validpgpkeys of the recipe it was named in,
 * LOCachy's own of that name, else the AUR's (of that package base). */
static void key_source(const char *name, char *out, size_t size)
{
    char path[400];
    struct stat st;
    snprintf(path, sizeof path, "%s/%.63s", RECIPES, name);
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* TRANSLATORS: validpgpkeys is a recipe's (PKGBUILD's) field; keep it. */
        snprintf(out, size, _("validpgpkeys of LOCachy's recipe: %s"), path);
    } else {
        snprintf(path, sizeof path, "https://aur.archlinux.org/cgit/aur.git/tree/PKGBUILD?h=%.63s", name);
        /* TRANSLATORS: validpgpkeys is a recipe's (PKGBUILD's) field; keep it. */
        snprintf(out, size, _("validpgpkeys of the AUR's recipe: %s"), path);
    }
}

/* The targets of locachy-pkg apply targets[0..n) its output says it has
 * applied ("===> NAME: done"): done[i] for targets[i]. */
static void targets_done(char *const targets[], int n, int *done)
{
    char *line = NULL;
    size_t cap = 0;
    FILE *f = job_log[0] ? fopen(job_log, "re") : NULL;
    while (f && getline(&line, &cap, f) > 0) {
        line[strcspn(line, "\r\n")] = '\0';
        size_t len = strlen(line);
        if (strncmp(line, "===> ", 5) != 0 || len < 11 || strcmp(line + len - 6, ": done") != 0) {
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (strlen(targets[i]) == len - 11 && strncmp(line + 5, targets[i], len - 11) == 0) {
                done[i] = 1;
            }
        }
    }
    free(line);
    if (f) {
        fclose(f);
    }
}

/* g: the job that stopped for source signing keys, once a person has
 * trusted them, on from where it stopped (locachy-build checks them again,
 * and stops again for any not trusted), as a new job. locachy-pkg apply
 * keeps the plan it stopped with: the targets it applied are not applied
 * again, the others are, from that plan. Another job (a container's build,
 * an install) is asked for again as it was: its builds check their keys
 * before anything is built. */
static void continue_job(void)
{
    char args[sizeof job_args], *argv[JOB_ARGV + 1], label[160], kind[sizeof job_kind];
    int n = 0, done[JOB_ARGV] = { 0 };
    if (!job_kind[0]) {
        say_notice(1, _("%s cannot be continued: its arguments were not kept."), job_label);
        return;
    }
    memcpy(args, job_args, sizeof args); /* submit_job keeps the new ones in job_args */
    for (; job_argv[n]; n++) {
        argv[n] = args + (job_argv[n] - job_args);
    }
    argv[n] = NULL;
    if (n > 0 && strcmp(job_kind, "pkg-apply") == 0) {
        int kept = 0;
        targets_done(argv, n, done);
        for (int i = 0; i < n; i++) {
            if (!done[i]) {
                argv[kept++] = argv[i];
            }
        }
        if (kept > 0) { /* none left: all of them again, never apply without targets (all of the plan) */
            argv[kept] = NULL;
        }
    }
    snprintf(label, sizeof label, "%s", job_label);
    snprintf(kind, sizeof kind, "%s", job_kind);
    submit_job(label, kind, argv, 1);
}

/* A job's label as start_job's callers give it, from its kind and
 * arguments (a new run of this program knows the job from the queue). */
static void label_of(const struct jobs_row *r, char *out, size_t size)
{
    char name[sizeof r->args];
    const char *a = r->args;
    size_t len = strlen(a);
    int host = 0, data = len > 7 && strcmp(a + len - 7, " --data") == 0;
    snprintf(name, sizeof name, "%.*s", (int)(data ? len - 7 : len), a); /* a container's name alone */
    for (int i = 0; i < nconts; i++) {
        host |= strcmp(conts[i].name, a) == 0 && conts[i].host;
    }
    if (strcmp(r->kind, "pkg-apply") == 0) {
        snprintf(out, size, "%s %s", _("Apply:"), a);
    } else if (strcmp(r->kind, "pkg-plan") == 0) {
        snprintf(out, size, "%s", _("check for updates"));
    } else if (strcmp(r->kind, "pkg-sync") == 0) {
        snprintf(out, size, "%s", _("bring the host to its manifest"));
    } else if (strcmp(r->kind, "pkg-install") == 0) {
        snprintf(out, size, _("add %s to the host"), a);
    } else if (strcmp(r->kind, "pkg-remove") == 0) {
        snprintf(out, size, _("remove %s from the host"), a);
    } else if (strcmp(r->kind, "pkg-swap") == 0) {
        snprintf(out, size, "swap %s", a);
    } else if (strcmp(r->kind, "kernel-select") == 0) {
        snprintf(out, size, "kernel select %s", a);
    } else if (strcmp(r->kind, "kernel-headers") == 0) {
        snprintf(out, size, "kernel headers %s", a);
    } else if (strcmp(r->kind, "repos-rank") == 0) {
        snprintf(out, size, "%s", "locachy-repos rank");
    } else if (strcmp(r->kind, "repos-set") == 0) {
        snprintf(out, size, "locachy-repos %s", a);
    } else if (strcmp(r->kind, "boot") == 0) {
        snprintf(out, size, "%s", "locachy-boot");
    } else if (strcmp(r->kind, "app-build") == 0) {
        snprintf(out, size, host ? _("install %s on the host") : _("build %s"), a);
    } else if (strcmp(r->kind, "app-rollback") == 0) {
        snprintf(out, size, _("roll back %s"), a);
    } else if (strcmp(r->kind, "app-remove") == 0) {
        snprintf(out, size, data ? _("remove %s and its home") : _("remove %s"), name);
    } else {
        snprintf(out, size, "%s %s", r->kind, a);
    }
}

static void bind_job_row(const struct jobs_row *r)
{
    char text[sizeof r->args], *args[JOB_ARGV + 1], *save = NULL;
    int k = 0;
    label_of(r, job_label, sizeof job_label);
    jobs_log_path(r->id, job_log, sizeof job_log);
    snprintf(text, sizeof text, "%s", r->args);
    for (char *w = strtok_r(text, " ", &save); w && k < JOB_ARGV; w = strtok_r(NULL, " ", &save)) {
        args[k++] = w;
    }
    args[k] = NULL;
    keep_args(r->kind, args);
}

/* Pick the running/stopping job first, else the earliest queued job, else (when
 * allow_ended) the newest ended job of our own. */
static const struct jobs_row *pick_job_row(const struct jobs_row *rows, int n, int allow_ended)
{
    const struct jobs_row *queued_row = NULL;
    for (int i = 0; i < n; i++) {
        if (!jobs_ended(&rows[i]) && rows[i].position == 0) return &rows[i];
        if (!jobs_ended(&rows[i]) && !queued_row) queued_row = &rows[i];
    }
    if (queued_row) return queued_row;
    return allow_ended && n > 0 ? &rows[n - 1] : NULL;
}

/* The active job in the queue (including one started from settings or
 * firmware), or the last job this program asked for (stopped for source
 * signing keys, or its output for l). */
static void find_job(void)
{
    static struct jobs_row rows[JOBS_ROWS_MAX];
    int n = jobs_mine(rows, JOBS_ROWS_MAX);
    const struct jobs_row *r = pick_job_row(rows, n, 1);
    if (!r) {
        return;
    }
    bind_job_row(r);
    if (!jobs_ended(r)) {
        job = r->id;
        snprintf(job_state, sizeof job_state, "%s", r->state);
        say_notice(0, _("%s: %s; it goes on if you leave."), job_label, jobs_state_label(r->state));
    } else if (strcmp(r->state, "keys") == 0 && parse_keys()) {
        job_result = 1;
        show_log = 1;
        say_notice(1, _("%s: stopped for source signing keys to trust (below); none is trusted by itself."), job_label);
    }
}

static void check_job(void)
{
    struct jobs_row r;
    char why[256];
    if (!job) {
        static struct jobs_row rows[JOBS_ROWS_MAX];
        int n = jobs_mine(rows, JOBS_ROWS_MAX);
        const struct jobs_row *active_row = pick_job_row(rows, n, 0);
        if (active_row) {
            bind_job_row(active_row);
            job = active_row->id;
            job_result = -1;
            nwanted = 0;
            snprintf(job_state, sizeof job_state, "%s", active_row->state);
            say_notice(0, _("%s: %s; it goes on if you leave."), job_label, jobs_state_label(active_row->state));
        }
        return;
    }
    if (jobs_status(job, &r, why, sizeof why) < 0) {
        return;
    }
    if (!jobs_ended(&r)) {
        snprintf(job_state, sizeof job_state, "%s", r.state); /* queued until the one before it ends */
        return;
    }
    int ok = strcmp(r.state, "done") == 0;
    job_result = ok ? 0 : strcmp(r.state, "cancelled") == 0 ? 2 : 1;
    job = 0;
    if (!ok && parse_keys()) {
        /* Not a failure: a person's decision, which nothing makes by itself. */
        say_notice(1, _("%s: stopped for source signing keys to trust (below); none is trusted by itself."), job_label);
    } else if (job_result == 2) {
        /* TRANSLATORS: home is the program's name; keep it. */
        say_notice(1, _("%s: stopped from home; its output is below."), job_label);
    } else if (ok) {
        say_notice(0, _("%s: done."), job_label);
    } else {
        say_notice(1, _("%s: failed; its output is below (and in %s)."), job_label, job_log);
    }
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

/* The job's output as the pane has it: its last 32 KiB, read into log_buf,
 * a line each in log_all (a line rewritten with \r: its last form); how
 * many. */
#define LOG_LINES 2048
static char log_buf[32768], *log_all[LOG_LINES];

static int log_split(void)
{
    int fd = job_log[0] ? open(job_log, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) {
        return 0;
    }
    off_t end = lseek(fd, 0, SEEK_END);
    off_t start = end > (off_t)sizeof log_buf - 1 ? end - (off_t)sizeof log_buf + 1 : 0;
    ssize_t got = pread(fd, log_buf, sizeof log_buf - 1, start);
    close(fd);
    if (got <= 0) {
        return 0;
    }
    log_buf[got] = '\0';
    int count = 0;
    char *save = NULL;
    for (char *s = strtok_r(log_buf, "\n", &save); s && count < LOG_LINES; s = strtok_r(NULL, "\n", &save)) {
        char *cr = strrchr(s, '\r');
        while (cr && cr[1] == '\0' && cr > s) { /* a line ending in \r */
            *cr = '\0';
            cr = strrchr(s, '\r');
        }
        log_all[count++] = cr ? cr + 1 : s;
    }
    return count;
}

/* The last lines of the job's output, from the end, skipping from of them;
 * n of them at most. How many lines there are into total. */
static int log_lines(char lines[][256], int n, int from, int *total)
{
    int count = log_split();
    char **all = log_all;
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
        if (t->merge) { /* the app presets: configurations that follow them */
            char merged[160];
            size_t n = strlen(what);
            snprintf(merged, sizeof merged, P_("%d configuration follows its new preset",
                                               "%d configurations follow their new presets", t->merge), t->merge);
            snprintf(what + n, sizeof what - n, "%s%s", n ? "; " : "", merged);
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
        ui_pad(cbuf, sizeof cbuf, c->host ? _("host") : strcmp(c->camera, "1") == 0 ? C_("row", "camera") : "", 7);
        snprintf(bbuf, sizeof bbuf, _("built %s"), ui_pad(bpad, sizeof bpad, c->built[0] ? c->built : _("never"), 19));
        pbuf[0] = '\0';
        if (c->host) {
            snprintf(pbuf, sizeof pbuf, "%s", _("packages on the host"));
        } else if (c->built[0] || c->pkg_count) { /* what its build installed over the host's */
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

/* The pane's title: a bar across, apart from the lists above; with Y when
 * the pane has something to copy. */
static void pane_title(int row, const char *text, int copy)
{
    char line[700];
    /* TRANSLATORS: Y is the Y key (Shift+y); keep "Y: ". */
    snprintf(line, sizeof line, "%s%s%s", text, copy ? "   " : "", copy ? _("Y: copy all") : "");
    screen_fill(0, row, screen_cols(), 1, DIM);
    textsel_put(1, row, line, BG, DIM);
}

/* A line of the pane at row *r (and the row after it next), above bottom. */
static void pane_put(int *r, int bottom, int col, const char *text, uint32_t color)
{
    if (*r < bottom) {
        textsel_put(col, (*r)++, text, color, PANE);
    }
}

/* text from row *r at col, cut into lines as wide as the pane has room for
 * from col (by cells: a fingerprint's origin, an address with no space to
 * break at). */
static void pane_wrap(int *r, int bottom, int col, const char *text, uint32_t color)
{
    char line[600], ch[5];
    int width = screen_cols() - col - 1 > 8 ? screen_cols() - col - 1 : 8;
    while (*text && *r < bottom) {
        size_t n = 0;
        int cells = 0;
        while (text[n]) {
            size_t len = 1;
            while (len < 4 && ((unsigned char)text[n + len] & 0xC0) == 0x80) len++;
            memcpy(ch, text + n, len);
            ch[len] = '\0';
            int w = ui_cells(ch);
            if (n > 0 && (cells + w > width || n + len >= sizeof line)) break;
            cells += w;
            n += len;
        }
        snprintf(line, sizeof line, "%.*s", (int)n, text);
        pane_put(r, bottom, col, line, color);
        text += n;
    }
}

/* Where the command's line starting at command is cut, width cells wide: at
 * a space, never inside a fingerprint. */
static size_t command_cut(const char *command, int width)
{
    size_t len = strlen(command), cut = len;
    if ((int)len > width && width > 2) {
        for (cut = (size_t)width - 2; cut > 0 && command[cut] != ' '; cut--) {
        }
        cut = cut ? cut : strcspn(command, " ");
    }
    return cut;
}

/* How many rows draw_command takes for command. */
static int command_rows(const char *command)
{
    int n = 0;
    while (*command) {
        size_t cut = command_cut(command, screen_cols() - 8);
        command += cut + (command[cut] == ' ');
        n++;
    }
    return n;
}

/* The command that trusts the keys, from row r: a line, or more when it is
 * wider than the pane, each but the last ending in " \" (a shell takes them
 * as one, selected and pasted together): the row after it. */
static int draw_command(int r, int bottom, const char *command)
{
    char line[COMMAND_MAX + 8];
    int width = screen_cols() - 8, col = 4;
    while (*command && r < bottom) {
        size_t cut = command_cut(command, width);
        snprintf(line, sizeof line, "%.*s%s", (int)cut, command, command[cut] ? " \\" : "");
        pane_put(&r, bottom, col, line, ACCENT);
        command += cut + (command[cut] == ' ');
        col = 6;
    }
    return r;
}

/* The source signing keys the job stopped for, from row r of the pane: each
 * one's package, fingerprint and where it comes from; the command that
 * trusts those not trusted yet, or, all trusted, how to go on. The row after
 * them. */
/* Key i's line (package, fingerprint as gpg groups it, whether trusted now)
 * into line, its origin's into origin; the origin's column after the line's
 * start. */
static int key_lines(int i, char *line, size_t lsize, char *origin, size_t osize)
{
    char fpr[96], pad[160];
    group_fpr(wanted[i].fpr, fpr, sizeof fpr);
    ui_pad(pad, sizeof pad, wanted[i].name, 24);
    snprintf(line, lsize, "%s %s%s%s", pad, fpr, wanted[i].trusted ? "   " : "",
             wanted[i].trusted ? _("trusted now") : "");
    key_source(wanted[i].name, origin, osize);
    return ui_cells(pad) + 1;
}

static const char *keys_waiting(void)
{
    return _("Waiting for you: source signing keys to trust. None is trusted by itself.");
}

static const char *keys_all_trusted(void)
{
    /* TRANSLATORS: g is the g key; keep it. */
    return _("All of them are trusted now: g continues where it stopped.");
}

static const char *keys_check(void)
{
    return _("Check each with its upstream project; then run this as root (admin, from home):");
}

static const char *keys_then(void)
{
    /* TRANSLATORS: y and g are the y and g keys; keep them. */
    return _("y copies the command; once they are trusted, g continues where it stopped.");
}

/* The keys and their origins whole (each origin wrapped to the pane, not
 * cut at its edge), the rows of what follows them kept for it: the command
 * and how to go on are never pushed out of a short pane by long origins. */
static int draw_keys(int r, int bottom)
{
    char line[600], origin[600], command[COMMAND_MAX];
    keys_refresh();
    trust_command(command, sizeof command);
    pane_put(&r, bottom, 2, keys_waiting(), WARN);
    int tail = command[0] ? command_rows(command) + 2 : 1;
    int keys_bottom = bottom - tail > r ? bottom - tail : bottom;
    for (int i = 0; i < nwanted; i++) {
        int at = key_lines(i, line, sizeof line, origin, sizeof origin);
        pane_put(&r, keys_bottom, 4, line, wanted[i].trusted ? DIM : FG);
        pane_wrap(&r, keys_bottom, 4 + at, origin, DIM);
    }
    if (!command[0]) {
        pane_put(&r, bottom, 2, keys_all_trusted(), ACCENT);
        return r;
    }
    pane_put(&r, bottom, 2, keys_check(), FG);
    r = draw_command(r, bottom, command);
    pane_put(&r, bottom, 2, keys_then(), DIM);
    return r;
}

/* What draw_keys shows, a line each, as text (the command on one line). */
static void key_list_text(FILE *out)
{
    char line[600], origin[600], command[COMMAND_MAX];
    keys_refresh();
    trust_command(command, sizeof command);
    fprintf(out, "%s\n", keys_waiting());
    for (int i = 0; i < nwanted; i++) {
        int at = key_lines(i, line, sizeof line, origin, sizeof origin);
        fprintf(out, "  %s\n  %*s%s\n", line, at, "", origin);
    }
    if (command[0]) {
        fprintf(out, "%s\n  %s\n%s\n", keys_check(), command, keys_then());
    } else {
        fprintf(out, "%s\n", keys_all_trusted());
    }
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
            { c->host ? _("Access") : C_("detail", "camera"), c->host ? _("real system and full home") :
                strcmp(c->camera, "1") == 0 ? _("yes (the cameras, as they come and go)") : _("no") },
            { c->host ? _("devices") : _("directories"), c->host ? _("by your permissions") : c->directories },
            { _("state"), state_label(c->state) }, { _("built"), c->built[0] ? c->built : _("never") },
            { _("previous build"), c->prev ? _("kept (r rolls back to it)") : _("none") },
        };
        snprintf(line, size, "%s %s", ui_pad(kbuf, sizeof kbuf, items[i][0], 15), items[i][1]);
        return FG;
    }
    human_size(c->pkg_bytes, bytes, sizeof bytes);
    if (i == CONT_ITEMS && c->host) {
        snprintf(line, size, "%s", _("Its packages are on the host; their versions and sizes are on the Host page."));
        return DIM;
    } else if (i == CONT_ITEMS && !c->built[0] && !c->pkg_count) {
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

/* A row of the plan as the details show it into line: its colour. */
static uint32_t update_line(const struct row *w, char *line, size_t size)
{
    if (strcmp(w->kind, "note") == 0) {
        snprintf(line, size, "%s", w->what);
        return WARN;
    } else if (strcmp(w->kind, "missing") == 0) {
        snprintf(line, size, _("install %-36.100s declared, not installed"), w->what);
    } else if (strcmp(w->kind, "local") == 0) {
        snprintf(line, size, _("local %-30.100s %.200s (installed %.80s)"), w->what, w->new_, w->old);
    } else if (strcmp(w->kind, "rebuild") == 0 && (w->old[0] || w->new_[0])) {
        snprintf(line, size, _("rebuild: %s (%s -> %s)"), w->what, w->old, w->new_);
    } else if (strcmp(w->kind, "rebuild") == 0) {
        snprintf(line, size, _("rebuild: %s"), w->what);
    } else if (strcmp(w->kind, "catalog") == 0) { /* the app presets: which commit */
        snprintf(line, size, "%.120s: %.40s -> %.7s", w->what, w->old, w->new_);
    } else if (strcmp(w->kind, "merge") == 0) {
        snprintf(line, size, _("%s follows its new preset: %s"), w->what, w->old);
    } else {
        snprintf(line, size, "%-5s %-36s %s -> %s", w->kind, w->what, w->old, w->new_);
    }
    return FG;
}

/* Line i of the lower pane when it does not show the job's output: on Host,
 * how the system stands to its manifest; else the selected row's details.
 * Into line: its colour, 0 past the last line. */
static uint32_t pane_line(int i, char *line, size_t size)
{
    if (page == HOST && i < nothers) {
        snprintf(line, size, "%s", others[i]);
        return strncmp(others[i], "missing", 7) == 0 ? ERROR : FG;
    } else if (page == HOST && i == 0) {
        snprintf(line, size, "%s", _("The system is as the manifest says."));
        return DIM;
    } else if (page == UPDATES && sel[page] < ntargets) {
        const struct target *t = &targets[sel[page]];
        for (int k = 0; k < nrows; k++) {
            const struct row *w = &rows[k];
            if (strcmp(w->target, t->name) == 0 && strcmp(w->kind, "state") != 0 && i-- == 0) {
                return update_line(w, line, size);
            }
        }
    } else if (page == CONTAINERS && sel[page] < nconts && i < detail_lines(&conts[sel[page]])) {
        return detail_line(&conts[sel[page]], i, line, size);
    }
    return 0;
}

/* The lower pane, on a background of its own under a title bar: the job's
 * output (while it runs, or l; stopped for source signing keys, those
 * first), else the selected row's details (on Host: how the system stands
 * to its manifest; of a container, its packages too, scrolled by
 * details_from). */
static void draw_lower(int top, int height)
{
    char line[512];
    int r = top + 1, bottom = top + height;
    screen_fill(0, top, screen_cols(), height, PANE);
    if (job || show_log) {
        char lines[64][256];
        int stopped = !job && nwanted;
        if (stopped) {
            r = draw_keys(r, bottom);
        }
        int total, n = log_lines(lines, bottom - r < 0 ? 0 : bottom - r < 64 ? bottom - r : 64, log_from, &total);
        log_total = total;
        const char *how = job ? jobs_state_label(job_state[0] ? job_state : "running") :
                          stopped ? _("stopped") : job_result == 0 ? _("done") :
                          job_result == 1 ? _("failed") : job_result == 2 ? _("ended") : "";
        char where[64] = "";
        if (n) {
            int last = total - log_from;
            snprintf(where, sizeof where, _("lines %d-%d of %d"), last - n + 1, last, total);
        }
        /* TRANSLATORS: PgUp/PgDn and l are key names; keep them. */
        snprintf(line, sizeof line, _("Output: %s%s%s%s   %s   PgUp/PgDn: scroll   l: %s"),
                 job || job_result >= 0 ? job_label : n ? _("the last job") : _("no job yet"),
                 how[0] ? " (" : "", how, how[0] ? ")" : "", where, page == HOST ? _("the manifest") : _("details"));
        pane_title(top, line, total > 0);
        for (int i = 0; i < n; i++) {
            pane_put(&r, bottom, 2, lines[i], log_color(lines[i]));
        }
        return;
    }
    if (page == HOST) {
        /* TRANSLATORS: l is the l key; keep it. */
        snprintf(line, sizeof line, P_("%s: %d declared, %s installed with what they need   l: output",
                                       "%s: %d declared, %s installed with what they need   l: output", nleaves),
                 MANIFEST, nleaves, closure[0] ? closure : "?");
        pane_title(top, line, 1);
        uint32_t color;
        for (int i = 0; r < bottom && (color = pane_line(i, line, sizeof line)) != 0; i++) {
            pane_put(&r, bottom, 2, line, color);
        }
        return;
    }
    if (page == UPDATES && sel[page] < ntargets) {
        struct target *t = &targets[sel[page]];
        /* TRANSLATORS: l is the l key; keep it. */
        char first[512];
        snprintf(line, sizeof line, _("Details: %s   l: output"), t->name);
        pane_title(top, line, pane_line(0, first, sizeof first) != 0);
        uint32_t color;
        for (int i = 0; r < bottom && (color = pane_line(i, line, sizeof line)) != 0; i++) {
            pane_put(&r, bottom, 2, line, color);
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
        snprintf(line, sizeof line, _("Details: %s   l: output"), c->name);
        if (most > 0) { /* more than the pane holds: which lines, and how to see the rest */
            char where[64];
            size_t len = strlen(line);
            snprintf(where, sizeof where, _("lines %d-%d of %d"), details_from + 1, details_from + details_rows,
                     details_total);
            /* TRANSLATORS: PgUp/PgDn are key names; keep them. */
            snprintf(line + len, sizeof line - len, "   %s   %s", where, _("PgUp/PgDn: scroll"));
        }
        pane_title(top, line, 1);
        for (int i = details_from; i < details_total && r < bottom; i++) {
            uint32_t color = detail_line(c, i, line, sizeof line);
            pane_put(&r, bottom, 2, line, color);
        }
    } else {
        pane_title(top, page == CONTAINERS ? _("Details") : _("Details: nothing selected"), 0);
    }
}

static void show_notice(int error, const char *text)
{
    snprintf(notice, sizeof notice, "%s", text);
    notice_error = error;
}

/* text to the clipboard, or (primary) to the primary selection; the notice
 * says what was copied, or why not, without becoming the notice to copy. */
static void copy_text(int primary, const char *text)
{
    char msg[512];
    int failed = textsel_copy(primary, text, msg, sizeof msg);
    if (msg[0]) {
        show_notice(failed, msg);
    }
}

/* y, Ctrl+C: what is selected; else, stopped for source signing keys, the
 * command that trusts those not trusted yet; else the notice. */
static void copy_key(void)
{
    char command[COMMAND_MAX];
    char *text = textsel_get();
    command[0] = '\0';
    if (!job && nwanted) {
        keys_refresh();
        trust_command(command, sizeof command);
    }
    if (text && text[0]) {
        copy_text(0, text);
    } else if (command[0]) {
        copy_text(0, command);
    } else if (said[0]) {
        copy_text(0, said);
    } else {
        show_notice(0, _("Nothing selected: drag over text, or double-click a word, or triple-click a line."));
    }
    free(text);
}

/* Y: all the lower pane holds, a line each: the job's output (its lines the
 * pane counts), after the source signing keys it stopped for, with where
 * they come from; or the details. */
static void copy_pane(void)
{
    char *text = NULL, line[512];
    size_t size = 0;
    FILE *out = open_memstream(&text, &size);
    if (!out) {
        return;
    }
    if (job || show_log) {
        if (!job && nwanted) {
            key_list_text(out);
        }
        int n = log_split();
        for (int i = 0; i < n; i++) {
            fprintf(out, "%s%s", i ? "\n" : "", log_all[i]);
        }
    } else {
        for (int i = 0; pane_line(i, line, sizeof line); i++) {
            fprintf(out, "%s%s", i ? "\n" : "", line);
        }
    }
    if (fclose(out) == 0 && text[0]) {
        copy_text(0, text);
    } else {
        show_notice(0, _("Nothing to copy here."));
    }
    free(text);
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
    textsel_frame();
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
    } else if (page == UPDATES) { /* where updates come from, over the list; the pane stays where it is */
        screen_text(2, top++, source_line, DIM, BG);
        screen_text(2, top++, apps_line, apps_missing ? WARN : DIM, BG);
        height -= 2;
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
    if (!job && nwanted) {
        /* TRANSLATORS: g is the g key; keep "g: ". */
        x = ui_button(x, rows_n - 2, _("g: continue"), HIT_KEY + 'g');
    }
    /* TRANSLATORS: y is the y key; keep "y: ". */
    x = ui_button(x, rows_n - 2, _("y: copy"), HIT_KEY + 'y');
    /* TRANSLATORS: Tab is the Tab key; keep "Tab: ". */
    x = ui_button(x, rows_n - 2, _("Tab: page"), HIT_KEY + '\t');
    /* TRANSLATORS: q is the q key; keep "q: ". */
    ui_button(x, rows_n - 2, _("q: back"), HIT_KEY + 'q');
    textsel_put(1, rows_n - 1, notice, notice_error ? ERROR : ACCENT, BG);
    textsel_draw(BG, ACCENT);
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
        } else if (key == K_SHOWN) {
            draw();
        } else {
            edit_line_key(key, text, size, &pos);
        }
    }
}

static int yes(const char *question)
{
    char line[512];
    snprintf(line, sizeof line, "%s [y/N] ", question);
    int redraw = 1;
    for (;;) {
        if (redraw) {
            int row = screen_rows() - 1;
            screen_fill(0, row, screen_cols(), 1, BG);
            screen_text(1, row, line, ACCENT, BG);
            /* TRANSLATORS: Y is the Y key; keep "Y: ". */
            int c = ui_button(2 + ui_cells(line), row, _("Y: yes"), HIT_KEY + 'y');
            /* TRANSLATORS: N is the N key; keep "N: ". */
            ui_button(c, row, _("N: no"), HIT_KEY + 'n');
            screen_flush();
            redraw = 0;
        }
        int key = key_get();
        if (key == K_SHOWN) {
            draw();
            redraw = 1;
            continue;
        }
        if (key == 0) continue;
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
    if (!isalnum((unsigned char)s[0]) || strlen(s) > 63 || strncmp(s, "window-", 7) == 0) {
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

/* A preset: its name and its file (DIR/NAME.conf). */
struct preset {
    char name[64], path[400];
};

static int by_preset(const void *a, const void *b)
{
    return strcmp(((const struct preset *)a)->name, ((const struct preset *)b)->name);
}

/* The presets of dir after the n in p, sorted, a name once (those before
 * it are kept): how many in all. The catalog was validated when installed;
 * a recipe may ask for real system access, granted only when saved. */
static int presets_of(const char *dir, struct preset *p, int n, int max)
{
    int first = n;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && n < max) {
        size_t len = strlen(e->d_name);
        int known = 0;
        if (len <= 5 || len - 5 >= sizeof p[0].name || strcmp(e->d_name + len - 5, ".conf") != 0) {
            continue;
        }
        snprintf(p[n].name, sizeof p[n].name, "%.*s", (int)(len - 5), e->d_name);
        snprintf(p[n].path, sizeof p[n].path, "%s/%s", dir, e->d_name);
        for (int i = 0; i < first && !known; i++) {
            known = strcmp(p[i].name, p[n].name) == 0;
        }
        if (!known) {
            n++;
        }
    }
    if (d) {
        closedir(d);
    }
    qsort(p + first, (size_t)(n - first), sizeof p[0], by_preset);
    return n;
}

/* The presets: the system's (its host programs) first, then the app
 * presets' catalog's: how many. */
static int preset_list(struct preset *p, int max)
{
    return presets_of(APPS_CATALOG, p, presets_of(SYSTEM_PRESETS, p, 0, max), max);
}

/* The name of the preset at path (DIR/NAME.conf). */
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
    int is_new, built, network, camera, changed, host;
    char preset[400]; /* the preset it was filled from ("": none) */
    char name[64], package[128], extras[VALUE_MAX + 1], command[256], arguments[256];
    char dirs[MAX_DIRS][256];
    int ndirs;
    struct pkg_info info[MAX_PKGS];
    int ninfo;
    int sel, first, button; /* the selected line, the first shown, the button */
};

enum field { F_PRESET, F_NAME, F_PACKAGE, F_EXTRAS, F_COMMAND, F_ARGUMENTS, F_NETWORK, F_CAMERA, F_DIRS, F_ACCESS,
             F_ALWAYS, F_BUTTONS };
static const char *const field_names[] = {
    N_("Preset"), N_("Name"), N_("Package"), N_("Extras"), N_("Command"), N_("Arguments"), N_("Network"), N_("Camera"),
    N_("Directories"), N_("Access"), N_("Always"), "",
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
    } else if (strcmp(name, "host") == 0) {
        f->host = strcmp(value, "1") == 0;
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
    if (!f->host) {
        add_line(l, &n, F_CAMERA, 0);
        for (int i = 0; i <= f->ndirs; i++) { /* the last: to add one */
            add_line(l, &n, F_DIRS, i);
        }
    }
    add_line(l, &n, F_ACCESS, 0);
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
    if (!f->host && f->built && lstat(path, &st) == 0) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(out, size, "%s", _("in its build   Enter: what its packages run"));
        return FG;
    }
    if (lstat(f->command, &st) == 0) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(out, size, "%s", f->built ? _("the host's   Enter: what its packages run") : _("the host's"));
        return FG;
    }
    snprintf(out, size, "%s", f->host ? _("not installed on the host") :
             f->built ? _("neither in its build nor on the host") : _("not on the host: from its build?"));
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
    case F_ACCESS:
        snprintf(value, vsize, "%s", f->host ? _("real system and full home") : _("isolated filesystem"));
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(info, isize, "%s", f->host ? _("Enter: switch; packages on the host, commands as you") :
                 _("Enter: switch; private app data and shared folders"));
        return f->host ? WARN : DIM;
    case F_ALWAYS:
        snprintf(value, vsize, "%s", _("display, sound, GPU, fonts, its own bus"));
        if (f->host) {
            snprintf(info, isize, "%s", _("devices by your permissions; home starts, freezes and stops it"));
        } else {
            open_user_dirs(home_, sizeof home_, dirs_, sizeof dirs_);
            snprintf(info, isize, _("and of your home: %s"), dirs_);
        }
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
                snprintf(line, sizeof line, "[ %s ]", f->host && b == B_BUILD ? _("Save and install") : _(button_names[b]));
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
static int form_edit(struct form *f, char *text, size_t size)
{
    char copy[VALUE_MAX + 1], line[VALUE_MAX + 8];
    snprintf(copy, sizeof copy, "%s", text);
    size_t pos = strlen(copy), limit = size < sizeof copy ? size : sizeof copy;
    for (;;) {
        int row = FORM_TOP + f->sel - f->first, width = screen_cols() - FORM_VALUE - 2;
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
        } else if (key == K_SHOWN) {
            form_show(f);
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
    static struct preset presets[63];
    char old[64], typed[64];
    int n = preset_list(presets, 63);
    for (int i = 0; i < n; i++) {
        struct container c = { 0 };
        conf_read(presets[i].path, container_value, &c);
        snprintf(items[i], sizeof items[i], "%-16.63s %.100s %.150s", presets[i].name, c.package, c.extras);
    }
    snprintf(items[n], sizeof items[n], "%s", _("(blank)"));
    int at = choose(_("A new container: from a preset (its lines filled in; then it is yours), or blank"), items, n + 1);
    if (at < 0) {
        return;
    }
    snprintf(old, sizeof old, "%s", f->preset[0] ? preset_name(f->preset) : "");
    snprintf(typed, sizeof typed, "%s", f->name[0] && strcmp(f->name, old) != 0 ? f->name : "");
    f->preset[0] = f->package[0] = f->extras[0] = f->command[0] = f->arguments[0] = '\0';
    f->network = f->camera = f->host = f->ndirs = f->ninfo = 0;
    if (at < n) {
        snprintf(f->preset, sizeof f->preset, "%s", presets[at].path);
        conf_read(f->preset, form_value, f);
    }
    snprintf(f->name, sizeof f->name, "%.63s", typed[0] ? typed : at < n ? presets[at].name : "");
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
                     _("%.200s is in %s, which every container has already."), rel, word);
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

/* Wrap a disclosure by display cells, including translations without spaces. */
static int access_text(int row, const char *text)
{
    int width = screen_cols() - 4;
    while (*text && row < screen_rows() - 4) {
        char line[512];
        size_t n = 0, last_space = 0;
        int cells = 0;
        while (text[n] && n + 5 < sizeof line) {
            size_t next = n + 1;
            while (((unsigned char)text[next] & 0xC0) == 0x80) next++;
            char cp[8] = "";
            memcpy(cp, text + n, next - n);
            int cw = ui_cells(cp);
            if (cells + cw > width) break;
            if (text[n] == ' ') last_space = n;
            cells += cw;
            n = next;
        }
        if (text[n] && last_space) n = last_space;
        if (!n) break;
        memcpy(line, text, n);
        line[n] = '\0';
        screen_text(2, row++, line, FG, BG);
        text += n;
        text += strspn(text, " ");
    }
    return row + 1;
}

/* A new grant of real system access is a person's choice, never a side
 * effect of selecting a recipe. Keep the disclosure on its own screen. */
static int grant_host_access(const struct form *f)
{
    static const char *const details[] = {
        N_("Packages will be installed on the real system."),
        N_("It can read and change your full home and system files as you."),
        N_("It can run terminal commands; sudo still requires authentication."),
        N_("Home still starts, freezes, resumes and stops its whole process tree."),
    };
    for (;;) {
        char title[200];
        screen_fill(0, 0, screen_cols(), screen_rows(), BG);
        ui_hits_clear();
        screen_bar(_("packages"), NULL);
        snprintf(title, sizeof title, _("Allow %s to access your real system?"), f->name);
        screen_text(1, 1, title, WARN, BG);
        int row = 4;
        for (size_t i = 0; i < sizeof details / sizeof details[0]; i++) {
            row = access_text(row, _(details[i]));
        }
        /* TRANSLATORS: Y and N are the Y and N keys; keep them. */
        screen_text(1, screen_rows() - 3, _("Y: allow and save   N or Esc: cancel without saving"), WARN, BG);
        /* TRANSLATORS: Y is the Y key; keep "Y: ". */
        int x = ui_button(1, screen_rows() - 2, _("Y: yes"), HIT_KEY + 'y');
        /* TRANSLATORS: N is the N key; keep "N: ". */
        ui_button(x, screen_rows() - 2, _("N: no"), HIT_KEY + 'n');
        screen_flush();
        int key = key_get();
        if (key == 0 || key == K_SHOWN) continue;
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (!e->pressed) continue;
            key = ui_hit_at(e->col, e->row) == HIT_KEY + 'y' ? 'y' : 'n';
        }
        return key == 'y' || key == 'Y';
    }
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
    struct form current = { 0 };
    if (!f->is_new) conf_read(path, form_value, &current);
    if (f->host && !current.host && !grant_host_access(f)) {
        say_notice(0, _("%s: access not granted; nothing saved or installed."), f->name);
        return -1;
    }
    if (f->is_new && f->preset[0] && copy_file(f->preset, path) < 0) {
        say_notice(1, _("Cannot write %s."), path);
        return -1;
    }
    const char *values[][2] = {
        { "package", f->package }, { "extras", f->extras }, { "command", f->command },
        { "arguments", f->arguments }, { "network", f->network ? "1" : "0" }, { "camera", f->camera ? "1" : "0" },
        { "directories", dirs_ }, { "host", f->host ? "1" : "0" },
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
    } else if (l->field == F_ACCESS) {
        f->host = !f->host;
        f->changed = 1;
        /* The number of rows changes when camera/directories are hidden. */
        struct form_line lines[FORM_LINES];
        int n = form_lines(f, lines);
        for (int i = 0; i < n; i++) if (lines[i].field == F_ACCESS) f->sel = i;
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
                snprintf(label, sizeof label, f->host ? _("install %s on the host") : _("build %s"), f->name);
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
    snprintf(cmd, sizeof cmd, "LOCACHY_APPS_CATALOG=%s %s search %s 2>/dev/null", APPS_CATALOG, PKG, query);
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
    if (strcmp(hits[at].kind, "recipe") == 0) { /* its whole configuration, including access and shared directories */
        snprintf(seed.preset, sizeof seed.preset, "%s/%.63s.conf", APPS_CATALOG, hits[at].name);
        if (conf_read(seed.preset, form_value, &seed) < 0) {
            say_notice(1, _("Cannot read %s; nothing saved or installed."), seed.preset);
            return;
        }
    } else {
        snprintf(seed.package, sizeof seed.package, "%s", hits[at].package);
        snprintf(seed.extras, sizeof seed.extras, "%s", hits[at].extras);
        snprintf(seed.command, sizeof seed.command, "%s", hits[at].command);
        snprintf(seed.arguments, sizeof seed.arguments, "%s", hits[at].arguments);
        seed.network = hits[at].network;
    }
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
        snprintf(label, sizeof label, key == 'b' ? c->host ? _("install %s on the host") : _("build %s") :
                 _("roll back %s"), c->name);
        if (key == 'r' && c->host) {
            say_notice(1, "%s", _("Its packages are on the host; there is no private build to roll back."));
        } else if (strcmp(c->state, "closed") != 0) {
            say_notice(1, _("%s is %s: stop it (home) first."), c->name, state_label(c->state));
        } else if (key == 'b' || yes(_("Roll back to its previous build?"))) {
            start_job(label, argv);
        }
    } else if (key == 'd') {
        snprintf(text, sizeof text, c->host ? _("Remove %s from home? Host packages and your files will be kept.") :
                 _("Remove %s (its configuration and builds)?"), c->name);
        if (strcmp(c->state, "closed") != 0) {
            say_notice(1, _("%s is %s: stop it (home) first."), c->name, state_label(c->state));
        } else if (yes(text)) {
            int data = 0;
            if (!c->host) {
                snprintf(text, sizeof text, _("Its home too (%s/%s/data: its settings, its files)?"), STATE, c->name);
                data = yes(text);
            }
            char *argv[] = { APP, "remove", c->name, data ? "--data" : NULL, NULL };
            snprintf(label, sizeof label, data ? _("remove %s and its home") : _("remove %s"), c->name);
            start_job(label, argv);
        }
    }
}

static void host_key(int key)
{
    char text[512] = "", label[160];
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
        /* One or more names, separated by spaces: each its own argument. */
        if (prompt(_("Packages to add, separated by spaces (repositories or the AUR):"), text, sizeof text) &&
            text[0]) {
            char *argv[32 + 3], *save = NULL;
            int n = 0;
            if (!valid_packages(text, 32)) {
                say_notice(1, "%s", _("Package names: letters, digits and @ . _ + -, at most 32."));
                return;
            }
            snprintf(label, sizeof label, _("add %s to the host"), text);
            argv[n++] = PKG;
            argv[n++] = "install";
            for (char *w = strtok_r(text, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
                argv[n++] = w;
            }
            argv[n] = NULL;
            if (n > 2) {
                start_job(label, argv);
            }
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
    int id = ui_hit_at(e->col, e->row);
    if (e->wheel) {
        if (id == HIT_LOWER && page == CONTAINERS && !job && !show_log) {
            details_from -= 3 * e->wheel; /* draw_lower keeps it in range */
        } else if (id == HIT_LOWER) {
            log_from += 3 * e->wheel;
            log_from = log_from < 0 ? 0 : log_from > log_total - 1 ? (log_total > 0 ? log_total - 1 : 0) : log_from;
        } else {
            sel[page] -= e->wheel;
            sel[page] = sel[page] < 0 ? 0 : sel[page] >= n ? (n > 0 ? n - 1 : 0) : sel[page];
        }
        return -1;
    }
    /* The lower pane's text and the notice: selected as in term, offered as
     * the primary selection once made. */
    int got = textsel_pointer(e, id == HIT_LOWER || e->row == screen_rows() - 1);
    if (got == TEXTSEL_MADE) {
        char *text = textsel_get();
        if (text && text[0]) {
            copy_text(1, text);
        }
        free(text);
    }
    if (got != TEXTSEL_NONE) return -1;
    if (e->button != BTN_LEFT || !e->pressed) return 0;
    if (id >= HIT_KEY) return id - HIT_KEY;
    textsel_clear(); /* a row or a page: the pane shows another's */
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
    load_all();
    find_job(); /* after the containers: a build's label says whether it installs on the host */
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
        if (key > 0 && key != 'y' && key != 'Y' && key != 3) {
            textsel_clear(); /* what the pane shows may change */
        }
        if (key == 'q' || key == 27) {
            return 0;
        } else if (key == 'y' || key == 3) { /* 3: Ctrl+C */
            copy_key();
        } else if (key == 'Y') {
            copy_pane();
        } else if (key == 'g' && !job && nwanted) {
            continue_job();
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
