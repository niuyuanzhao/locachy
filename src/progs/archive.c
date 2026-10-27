/*
 * archive: the archive manager, laid out and keyed as KDE's Ark is. The
 * archive's entries as a tree (Name, Size, Modified, Permissions, Owner);
 * archive-worker reads and writes the archives (archive-proto.h), as the
 * user in its sandbox: this program, root for the screen, parses none.
 *   Enter, Ctrl+P: preview (in view or edit, the framebuffer program for the
 *   type; a folder opens or closes)   o: open in its application
 *   O: open with...   Ctrl+E: extract...   Ctrl+Shift+E: extract all...
 *   Alt+A: add files...   Delete: remove from the archive   Alt+T: test
 *   integrity   Alt+Return: properties   Ctrl+N: new   Ctrl+O: open
 *   Ctrl+F /: find   Space: mark   Ctrl+A: mark all   Left/Right: close,
 *   open a folder   q Ctrl+Q: quit   Esc: the find text away, or quit
 * The pointer: a click selects, a double click previews, the wheel
 * scrolls, the right button gives the menu, the toolbar's buttons click.
 * What the file manager asks for (Ark's command line, Dolphin's menu):
 *   archive --batch --autodestination --autosubfolder FILE...   Extract here
 *   archive --batch --dialog FILE                               Extract to...
 *   archive --add-to ARCHIVE --changetofirstpath FILE...        Compress to "NAME"
 *   archive --add --changetofirstpath FILE...                   Compress to...
 * Its status then: 0 done, 1 failed, 2 cancelled.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "base.h"
#include "archive-proto.h"
#include "files.h"

/* Tests run a worker they built; never chosen at run time (archive is root). */
#ifdef ARCHIVE_WORKER_TEST_PATH
#define WORKER_PATH ARCHIVE_WORKER_TEST_PATH
#else
#define WORKER_PATH ARCHIVE_WORKER_PATH
#endif

#define FG     0xD0D0D0
#define BG     0x101820
#define ACCENT 0x80C0FF
#define DIM    0x808890
#define ERROR  0xFF9090
#define MARK   0x2C4A68
#define BAR_BG 0x1C2836
#define LIST_TOP 4      /* the first row of entries */
#define HIT_ROW 0       /* + a shown row */
#define HIT_FORM 500000 /* + a dialog's line */
#define HIT_KEY 0x1000000 /* + the key a button stands for, with MODS(its modifiers) */
#define MODS(m) ((m) << 16)
#define MAX_WARNINGS 64

struct node {
    char *key;   /* its path as the worker said it, escaped: given back to it */
    char *name;  /* the last part of it, to show */
    char *user, *group, *link;
    char type;   /* d f l h o */
    long long size;
    time_t mtime;
    unsigned mode;
    int enc, implicit; /* encrypted; a folder only its entries' paths have */
    int parent, first, next, kids, open, marked, match;
};

static struct node *nodes; /* nodes[0]: the archive's top */
static int nnodes, node_cap;
static int *hash;           /* open addressing: key -> node + 1 */
static size_t hash_cap;
static int *rows, *depth, nrows, row_cap, sel, top;

static struct {
    char path[PATH_MAX];
    char format[128], filters[128], backend[16], stem[NAME_MAX + 1];
    int headers, readonly, volumes, files, dirs, encrypted;
    long long bytes, packed;
    char password[1024];
    int have_password;
} arc;

static char filter[128], msg[600], user_home[PATH_MAX];
static int msg_error, quit, preview_seq;
static uid_t user_uid = ARCHIVE_USER;
static gid_t user_gid = ARCHIVE_USER;

__attribute__((format(printf, 2, 3)))
static void report(int error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    msg_error = error;
}

/* snprintf where what is cut is only cut: text to show, and names and paths
 * whose lengths the kernel and the worker limit anyway. -1 if it was. */
__attribute__((format(printf, 3, 4)))
static int cut_fmt(char *out, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static void as_user(int on)
{
    setfsgid(on ? user_gid : 0);
    setfsuid(on ? user_uid : 0);
}

static int clampi(int x, int lo, int hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void dir_of(const char *path, char *out, size_t size)
{
    snprintf(out, size, "%s", path);
    char *s = strrchr(out, '/');
    if (s == out) s[1] = '\0';
    else if (s) *s = '\0';
    else snprintf(out, size, ".");
}

static void human(long long n, char *out, size_t size)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    if (u == 0) snprintf(out, size, "%lld B", n);
    else snprintf(out, size, "%.1f %s", v, units[u]);
}

static void mode_text(const struct node *n, char *out)
{
    static const char bits[] = "rwxrwxrwx";
    out[0] = n->type == 'd' ? 'd' : n->type == 'l' ? 'l' : '-';
    for (int i = 0; i < 9; i++) out[1 + i] = (n->mode >> (8 - i)) & 1 ? bits[i] : '-';
    out[10] = '\0';
}

/* ---- The tree ---- */

static size_t key_hash(const char *k)
{
    size_t h = 1469598103934665603ull;
    for (; *k; k++) h = (h ^ (unsigned char)*k) * 1099511628211ull;
    return h;
}

static int find_node(const char *key)
{
    if (!hash_cap) return -1;
    for (size_t i = key_hash(key) & (hash_cap - 1);; i = (i + 1) & (hash_cap - 1)) {
        if (!hash[i]) return -1;
        if (strcmp(nodes[hash[i] - 1].key, key) == 0) return hash[i] - 1;
    }
}

static void hash_put(int n)
{
    if ((size_t)(nnodes + 1) * 2 >= hash_cap) {
        size_t cap = hash_cap ? hash_cap * 2 : 1024;
        int *h = calloc(cap, sizeof *h);
        if (!h) return;
        free(hash);
        hash = h;
        hash_cap = cap;
        for (int i = 0; i < nnodes; i++) {
            if (i == n) continue;
            size_t k = key_hash(nodes[i].key) & (cap - 1);
            while (hash[k]) k = (k + 1) & (cap - 1);
            hash[k] = i + 1;
        }
    }
    size_t k = key_hash(nodes[n].key) & (hash_cap - 1);
    while (hash[k]) k = (k + 1) & (hash_cap - 1);
    hash[k] = n + 1;
}

static void free_tree(void)
{
    for (int i = 0; i < nnodes; i++) {
        free(nodes[i].key);
        free(nodes[i].name);
        free(nodes[i].user);
        free(nodes[i].group);
        free(nodes[i].link);
    }
    free(hash);
    hash = NULL;
    hash_cap = 0;
    nnodes = nrows = sel = top = 0;
}

static int new_node(const char *key, int parent)
{
    if (nnodes == node_cap) {
        int cap = node_cap ? node_cap * 2 : 1024;
        struct node *n = realloc(nodes, (size_t)cap * sizeof *n);
        if (!n) return -1;
        nodes = n;
        node_cap = cap;
    }
    struct node *n = &nodes[nnodes];
    char name[NAME_MAX * 4 + 1];
    memset(n, 0, sizeof *n);
    archive_display(base_name(key), name, sizeof name);
    n->key = strdup(key);
    n->name = strdup(name);
    n->parent = parent;
    n->first = n->next = -1;
    n->type = 'd';
    n->implicit = 1;
    if (!n->key || !n->name) return -1;
    if (parent >= 0) {
        n->next = nodes[parent].first;
        nodes[parent].first = nnodes;
        nodes[parent].kids++;
    }
    hash_put(nnodes);
    return nnodes++;
}

/* The node of key, with the folders above it (made as needed). */
static int node_at(const char *key)
{
    int n = find_node(key);
    if (n >= 0) return n;
    const char *slash = strrchr(key, '/');
    int parent = 0;
    if (slash) {
        char up[PATH_MAX];
        snprintf(up, sizeof up, "%.*s", (int)(slash - key), key);
        parent = node_at(up);
        if (parent < 0) return -1;
    }
    return new_node(key, parent);
}

static void start_tree(void)
{
    free_tree();
    new_node("", -1);
    nodes[0].open = 1;
}

static int by_kind_name(const void *a, const void *b)
{
    const struct node *x = &nodes[*(const int *)a], *y = &nodes[*(const int *)b];
    if ((x->type == 'd') != (y->type == 'd')) return x->type == 'd' ? -1 : 1;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

/* Each folder's entries in Ark's order: folders first, by name. */
static void sort_tree(void)
{
    int *kids = NULL, cap = 0;
    for (int p = 0; p < nnodes; p++) {
        if (nodes[p].kids < 2) continue;
        if (nodes[p].kids > cap) {
            int *k = realloc(kids, (size_t)nodes[p].kids * sizeof *k);
            if (!k) break;
            kids = k;
            cap = nodes[p].kids;
        }
        int n = 0;
        for (int c = nodes[p].first; c >= 0; c = nodes[c].next) kids[n++] = c;
        qsort(kids, (size_t)n, sizeof *kids, by_kind_name);
        nodes[p].first = kids[0];
        for (int i = 0; i < n; i++) nodes[kids[i]].next = i + 1 < n ? kids[i + 1] : -1;
    }
    free(kids);
}

static int has_text(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; *s; s++) {
        if (strncasecmp(s, needle, n) == 0) return 1;
    }
    return n == 0;
}

static void add_row(int n, int d)
{
    if (nrows == row_cap) {
        int cap = row_cap ? row_cap * 2 : 1024;
        int *r = realloc(rows, (size_t)cap * sizeof *r);
        if (!r) return;
        rows = r;
        int *dp = realloc(depth, (size_t)cap * sizeof *dp);
        if (!dp) return;
        depth = dp;
        row_cap = cap;
    }
    rows[nrows] = n;
    depth[nrows++] = d;
}

static void add_rows(int parent, int d)
{
    for (int c = nodes[parent].first; c >= 0; c = nodes[c].next) {
        if (filter[0] && !nodes[c].match) continue;
        add_row(c, d);
        if (nodes[c].type == 'd' && (nodes[c].open || filter[0])) add_rows(c, d + 1);
    }
}

/* The rows shown: open folders' entries; with a find text, what has it in
 * its name and the folders down to it (Ark's filter). keep: the node to
 * stay selected (-1: the one selected now). */
static void build_rows(int keep)
{
    int was = keep >= 0 ? keep : sel < nrows ? rows[sel] : -1;
    if (filter[0]) {
        for (int i = 0; i < nnodes; i++) nodes[i].match = 0;
        for (int i = nnodes - 1; i > 0; i--) {
            if (has_text(nodes[i].name, filter)) nodes[i].match = 1;
            if (nodes[i].match) nodes[nodes[i].parent].match = 1;
        }
    }
    nrows = 0;
    add_rows(0, 0);
    sel = 0;
    for (int i = 0; i < nrows; i++) {
        if (rows[i] == was) sel = i;
    }
}

/* ---- The worker ---- */

/* The worker as the user, in a sandbox like view-render's: its own session,
 * no network, no supplementary group, no new privileges (as another user
 * than root, in tests: just that user). Only async-signal-safe calls after
 * the fork: everything was looked up before it. */
static pid_t spawn_worker(char *const argv[], int *to, int *from)
{
    static char env_home[PATH_MAX + 8], env_user[256 + 8];
    struct passwd *pw = getpwuid(user_uid);
    int in[2], out[2];
    int root = geteuid() == 0;
    if (!pw) return -1;
    snprintf(env_home, sizeof env_home, "HOME=%s", user_home);
    snprintf(env_user, sizeof env_user, "USER=%s", pw->pw_name);
    char *const env[] = { env_home, env_user, "LANG=C.UTF-8", "PATH=/usr/bin", NULL };
    const char *home = env_home + 5;
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    if (pipe2(in, O_CLOEXEC) < 0) return -1;
    if (pipe2(out, O_CLOEXEC) < 0) {
        close(in[0]);
        close(in[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t none;
        int err = open("/dev/null", O_WRONLY);
        sigemptyset(&none);
        if (dup2(in[0], 0) < 0 || dup2(out[1], 1) < 0 || err < 0 || dup2(err, 2) < 0 ||
            close_range(3, ~0u, 0) < 0 || setsid() < 0 || sigprocmask(SIG_SETMASK, &none, NULL) < 0) {
            _exit(127);
        }
        if (root && (unshare(CLONE_NEWNET) < 0 || setgroups(0, NULL) < 0 || setgid(gid) < 0 ||
                     setuid(uid) < 0 || getuid() != uid || geteuid() != uid || setuid(0) == 0)) {
            _exit(127);
        }
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 || chdir(home) < 0) _exit(127);
        execve(WORKER_PATH, argv, env);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]);
        close(out[0]);
        return -1;
    }
    *to = in[1];
    *from = out[0];
    return pid;
}

/* What one run of the worker ended with. */
struct job {
    int status;
    char error[512], done[PATH_MAX], hashes[3][160];
    char warnings[MAX_WARNINGS][300];
    int nwarnings, more_warnings;
    long long bytes;      /* progress */
    char at[PATH_MAX];
    const char *title;
    void (*on_entry)(char **f, int n);
    int quiet;            /* nothing shown */
};

static int split_fields(char *line, char **f, int max)
{
    int n = 0;
    for (char *p = line; n < max;) {
        f[n++] = p;
        p = strchr(p, '\t');
        if (!p) break;
        *p++ = '\0';
    }
    return n;
}

static int edit_text(const char *label, char *text, size_t size, int masked);
static void ask_exists(const char *path, int dir, char *answer, size_t size);
static int yes(const char *question);

static void draw_job(const struct job *j)
{
    int cols = screen_cols(), r = screen_rows() / 2 - 1;
    char line[64], at[PATH_MAX];
    screen_fill(0, 0, cols, screen_rows(), BG);
    ui_hits_clear();
    screen_bar(_("archive"), NULL);
    screen_text(2, r, j->title, ACCENT, BG);
    if (arc.bytes > 0 && j->bytes > 0) {
        int pct = clampi((int)(j->bytes * 100 / arc.bytes), 0, 100), w = cols - 12;
        screen_fill(2, r + 1, w, 1, BAR_BG);
        screen_fill(2, r + 1, w * pct / 100, 1, ACCENT);
        snprintf(line, sizeof line, "%3d%%", pct);
        screen_text(w + 3, r + 1, line, FG, BG);
    }
    archive_display(j->at, at, sizeof at);
    screen_text(2, r + 2, at, DIM, BG);
    /* TRANSLATORS: Esc is the key; keep it. */
    screen_text(2, r + 4, _("Esc: cancel"), DIM, BG);
    screen_flush();
}

/* A question the worker asked: the answer line written to it. */
static void answer_question(char **f, int n, int to)
{
    char line[sizeof arc.password + 8], path[PATH_MAX];
    if (n >= 2 && strcmp(f[1], "password") == 0) {
        int again = n >= 3 && strcmp(f[2], "again") == 0;
        if (!again && arc.have_password) {
            snprintf(line, sizeof line, "p%s\n", arc.password);
        } else {
            char pw[sizeof arc.password] = "", label[NAME_MAX * 4 + 64];
            snprintf(label, sizeof label, again ? _("Wrong password. Password for %s:") : _("Password for %s:"),
                     base_name(arc.path));
            if (edit_text(label, pw, sizeof pw, 1) && pw[0]) {
                snprintf(arc.password, sizeof arc.password, "%s", pw);
                arc.have_password = 1;
                snprintf(line, sizeof line, "p%s\n", pw);
            } else {
                arc.have_password = 0;
                snprintf(line, sizeof line, "c\n");
            }
            explicit_bzero(pw, sizeof pw);
        }
    } else if (n >= 4 && strcmp(f[1], "exists") == 0) {
        char a[NAME_MAX * 4 + 2];
        archive_display(f[2], path, sizeof path);
        ask_exists(path, f[3][0] == '1', a, sizeof a);
        snprintf(line, sizeof line, "%s\n", a);
    } else {
        snprintf(line, sizeof line, "c\n");
    }
    size_t len = strlen(line);
    if (write(to, line, len) != (ssize_t)len) {}
    explicit_bzero(line, sizeof line);
}

static void take_message(struct job *j, char *line, int to)
{
    char *f[16];
    int n = split_fields(line, f, 16);
    char type = f[0][0];
    if (type == 'E' && j->on_entry && n >= 10) {
        j->on_entry(f, n);
    } else if (type == 'F' && n >= 9) {
        archive_display(f[1], arc.format, sizeof arc.format);
        archive_display(f[2], arc.filters, sizeof arc.filters);
        archive_display(f[3], arc.backend, sizeof arc.backend);
        arc.headers = f[4][0] == '1';
        arc.readonly = f[5][0] == '1';
        arc.volumes = atoi(f[6]);
        arc.packed = atoll(f[7]);
        archive_display(f[8], arc.stem, sizeof arc.stem);
    } else if (type == 'P' && n >= 3) {
        j->bytes = atoll(f[1]);
        snprintf(j->at, sizeof j->at, "%s", f[2]);
    } else if (type == 'Q') {
        answer_question(f, n, to);
    } else if (type == 'W' && n >= 3) {
        if (j->nwarnings < MAX_WARNINGS) {
            char p[PATH_MAX], why[256];
            archive_display(f[1], p, sizeof p);
            archive_display(f[2], why, sizeof why);
            cut_fmt(j->warnings[j->nwarnings++], sizeof j->warnings[0], "%s%s%s", p, p[0] ? ": " : "", why);
        } else {
            j->more_warnings++;
        }
    } else if (type == 'D' && n >= 2) {
        snprintf(j->done, sizeof j->done, "%s", f[1]);
        archive_unescape(j->done);
    } else if (type == 'H' && n >= 4) {
        for (int i = 0; i < 3; i++) archive_display(f[1 + i], j->hashes[i], sizeof j->hashes[i]);
    } else if (type == 'X' && n >= 2) {
        archive_display(f[1], j->error, sizeof j->error);
    }
}

/* archive-worker ARGS, showing title and its progress meanwhile (Esc
 * cancels), answering what it asks; first: a line it reads first. Its
 * status (WORKER_*), what it said in j. */
static int run_worker(struct job *j, const char *title, char *const args[], const char *first)
{
    char *argv[512];
    int n = 0, to, from;
    argv[n++] = "archive-worker";
    for (int i = 0; args[i] && n < 511; i++) argv[n++] = args[i];
    argv[n] = NULL;
    void (*on_entry)(char **, int) = j->on_entry;
    int quiet = j->quiet;
    memset(j, 0, sizeof *j);
    j->on_entry = on_entry;
    j->quiet = quiet;
    j->title = title;
    pid_t pid = spawn_worker(argv, &to, &from);
    if (pid < 0) {
        snprintf(j->error, sizeof j->error, _("Cannot start archive-worker: %s."), strerror(errno));
        return j->status = WORKER_FAILED;
    }
    if (first) {
        size_t len = strlen(first);
        if (write(to, first, len) != (ssize_t)len || write(to, "\n", 1) != 1) {}
    }
    static char buf[ARCHIVE_LINE * 2];
    size_t len = 0;
    double shown = 0;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double t = (double)now.tv_sec + now.tv_nsec / 1e9;
        if (!j->quiet && t - shown > 0.2) {
            draw_job(j);
            shown = t;
        }
        struct pollfd p = { from, POLLIN, 0 };
        int key = key_poll_many(&p, 1, 200);
        /* TRANSLATORS: the question when Esc is pressed while the archive is worked on. */
        if (key == 27 && yes(_("Cancel it?"))) kill(-pid, SIGTERM);
        if (!(p.revents & (POLLIN | POLLHUP | POLLERR))) continue;
        ssize_t got = read(from, buf + len, sizeof buf - len - 1);
        if (got <= 0) break;
        len += (size_t)got;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', len - (size_t)(start - buf))) != NULL) {
            *nl = '\0';
            take_message(j, start, to);
            start = nl + 1;
        }
        len -= (size_t)(start - buf);
        memmove(buf, start, len);
        if (len == sizeof buf - 1) len = 0; /* a line longer than any: not the worker's */
    }
    close(from);
    close(to);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    j->status = WIFEXITED(st) && WEXITSTATUS(st) <= WORKER_NO_PASSWORD ? WEXITSTATUS(st) : WORKER_FAILED;
    if (j->status == WORKER_FAILED && !j->error[0]) {
        snprintf(j->error, sizeof j->error, _("archive-worker stopped (status %d)."),
                 WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    }
    if (j->status == WORKER_NO_PASSWORD) arc.have_password = 0;
    return j->status;
}

/* ---- Lines typed, questions ---- */

/* s cut to cells (an ellipsis where it was cut) and padded to them. */
static void fit(char *out, size_t size, const char *s, int cells)
{
    size_t i = 0;
    int used = 0;
    if (cells <= 0) {
        if (size) out[0] = '\0';
        return;
    }
    if (ui_cells(s) > cells) {
        while (s[i]) {
            unsigned char b = (unsigned char)s[i];
            size_t len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
            uint32_t cp = b & (0x3Fu >> (len - 1));
            for (size_t k = 1; k < len && s[i + k]; k++) cp = cp << 6 | ((unsigned char)s[i + k] & 0x3F);
            int w = len == 1 ? 1 : screen_char_cells(cp);
            if (used + w > cells - 1 || i + len + 4 >= size) break;
            used += w;
            i += len;
        }
        char cut[PATH_MAX];
        snprintf(cut, sizeof cut, "%.*s\xe2\x80\xa6", (int)i, s);
        ui_pad(out, size, cut, cells);
        return;
    }
    ui_pad(out, size, s, cells);
}

/* Left/Right/Home/End/Backspace/Delete, Ctrl+U, Ctrl+K and what keys type
 * (keys_typed) on text at *pos. */
static void edit_key(int key, char *text, size_t size, size_t *pos)
{
    size_t len = strlen(text);
    const char *typed;
    if (*pos > len) *pos = len;
    if (key == K_LEFT && *pos > 0) {
        do (*pos)--;
        while (*pos > 0 && ((unsigned char)text[*pos] & 0xC0) == 0x80);
    } else if (key == K_RIGHT && *pos < len) {
        do (*pos)++;
        while (*pos < len && ((unsigned char)text[*pos] & 0xC0) == 0x80);
    } else if (key == K_BOL || key == 1) {
        *pos = 0;
    } else if (key == K_EOL || key == 5) {
        *pos = len;
    } else if ((key == '\b' || key == 127) && *pos > 0) {
        size_t prev = *pos - 1;
        while (prev > 0 && ((unsigned char)text[prev] & 0xC0) == 0x80) prev--;
        memmove(text + prev, text + *pos, len - *pos + 1);
        *pos = prev;
    } else if (key == K_DEL && *pos < len) {
        size_t next = *pos + 1;
        while (next < len && ((unsigned char)text[next] & 0xC0) == 0x80) next++;
        memmove(text + *pos, text + next, len - next + 1);
    } else if (key == 21) {
        memmove(text, text + *pos, len - *pos + 1);
        *pos = 0;
    } else if (key == 11) {
        text[*pos] = '\0';
    } else if ((typed = keys_typed(key)) != NULL && len + strlen(typed) < size) {
        size_t n = strlen(typed);
        memmove(text + *pos + n, text + *pos, len - *pos + 1);
        memcpy(text + *pos, typed, n);
        *pos += n;
    }
}

/* A line typed at the bottom, after label; masked: shown as stars. 1 with
 * Enter, 0 with Esc. */
static int edit_text(const char *label, char *text, size_t size, int masked)
{
    size_t pos = strlen(text);
    for (;;) {
        char shown[1100], line[2400];
        size_t len = strlen(text);
        if (masked) {
            size_t n = len < sizeof shown - 1 ? len : sizeof shown - 1;
            memset(shown, '*', n);
            shown[n] = '\0';
        } else {
            snprintf(shown, sizeof shown, "%s", text);
        }
        if (pos > strlen(shown)) pos = strlen(shown);
        snprintf(line, sizeof line, "%s %.*s_%s", label, (int)pos, shown, shown + pos);
        int row = screen_rows() - 1;
        screen_fill(0, row, screen_cols(), 1, BG);
        screen_text(1, row, line, ACCENT, BG);
        screen_flush();
        int key = key_get();
        if (key == 27) return 0;
        if (key == '\n') return 1;
        edit_key(key, text, size, &pos);
    }
}

static int yes(const char *question)
{
    char line[1024];
    /* TRANSLATORS: y and N are the keys answering; keep them. */
    snprintf(line, sizeof line, _("%s [y/N]"), question);
    int row = screen_rows() - 1;
    screen_fill(0, row, screen_cols(), 1, BG);
    screen_text(1, row, line, ACCENT, BG);
    screen_flush();
    int key = key_get();
    return key == 'y' || key == 'Y';
}

/* Ark's overwrite question: the worker's answer in answer. */
static void ask_exists(const char *path, int dir, char *answer, size_t size)
{
    char line[PATH_MAX + 200];
    snprintf(line, sizeof line, dir ? _("%s is there and not a folder: o overwrite  O all  s skip  S all  Esc cancel")
                                    : _("%s is there already: o overwrite  O all  s skip  S all  r another name  Esc cancel"),
             path);
    for (;;) {
        int row = screen_rows() - 1;
        screen_fill(0, row, screen_cols(), 1, BG);
        screen_text(1, row, line, ACCENT, BG);
        screen_flush();
        int key = key_get();
        if (key == 'o' || key == 'O' || key == 's' || key == 'S') {
            snprintf(answer, size, "%c", key);
            return;
        } else if (key == 27) {
            snprintf(answer, size, "c");
            return;
        } else if (key == 'r' && !dir) {
            char name[NAME_MAX + 1];
            cut_fmt(name, sizeof name, "%s", base_name(path));
            if (edit_text(_("Another name:"), name, sizeof name, 0) && name[0] && !strchr(name, '/')) {
                snprintf(answer, size, "r%s", name);
                return;
            }
        }
    }
}

/* What went wrong with a job, a screen of it: the error and the entries
 * that failed. Any key goes back. */
static void show_problems(const struct job *j, const char *what)
{
    int cols = screen_cols(), rows_n = screen_rows();
    char line[PATH_MAX];
    screen_fill(0, 0, cols, rows_n, BG);
    ui_hits_clear();
    screen_bar(_("archive"), NULL);
    screen_text(1, 1, what, ERROR, BG);
    if (j->error[0]) {
        fit(line, sizeof line, j->error, cols - 2);
        screen_text(1, 2, line, ERROR, BG);
    }
    int r = 4;
    for (int i = 0; i < j->nwarnings && r < rows_n - 2; i++, r++) {
        fit(line, sizeof line, j->warnings[i], cols - 4);
        screen_text(3, r, line, FG, BG);
    }
    if (j->more_warnings) {
        snprintf(line, sizeof line, P_("and %d more", "and %d more", j->more_warnings), j->more_warnings);
        screen_text(3, r, line, DIM, BG);
    }
    screen_text(1, rows_n - 1, _("Any key: back"), DIM, BG);
    screen_flush();
    key_get();
}

/* ---- The archive ---- */

static void on_entry(char **f, int n)
{
    (void)n;
    if (strcmp(f[1], ".") == 0 || !f[1][0]) return;
    int i = node_at(f[1]);
    if (i <= 0) return;
    struct node *e = &nodes[i];
    char text[PATH_MAX];
    if (!e->implicit) return; /* the same path twice: the first one */
    e->implicit = 0;
    e->type = strchr("dflho", f[2][0]) ? f[2][0] : 'o';
    e->size = atoll(f[3]);
    e->mtime = (time_t)atoll(f[4]);
    e->mode = (unsigned)strtoul(f[5], NULL, 8);
    archive_display(f[6], text, sizeof text);
    e->user = strdup(text);
    archive_display(f[7], text, sizeof text);
    e->group = strdup(text);
    archive_display(f[8], text, sizeof text);
    e->link = strdup(text);
    e->enc = f[9][0] == '1';
    if (e->type == 'd') arc.dirs++;
    else arc.files++;
    arc.bytes += e->type == 'd' ? 0 : e->size;
    arc.encrypted |= e->enc;
}

/* The archive at path, read again: its tree, what it is. 0, or -1 (said). */
static int load_archive(const char *where)
{
    struct job j = { .on_entry = on_entry };
    char title[PATH_MAX + 32], keep[PATH_MAX] = "", path[PATH_MAX];
    char **was_open = NULL;
    int nopen = 0;
    snprintf(path, sizeof path, "%s", where);
    if (strcmp(path, arc.path) != 0) {
        explicit_bzero(arc.password, sizeof arc.password);
        arc.have_password = 0;
    } else {
        /* The same archive again (after adding or removing): as it was shown. */
        if (sel < nrows) snprintf(keep, sizeof keep, "%s", nodes[rows[sel]].key);
        was_open = calloc((size_t)nnodes + 1, sizeof *was_open);
        for (int i = 1; was_open && i < nnodes; i++) {
            if (nodes[i].open) was_open[nopen++] = strdup(nodes[i].key);
        }
    }
    snprintf(arc.path, sizeof arc.path, "%s", path);
    arc.files = arc.dirs = arc.encrypted = 0;
    arc.bytes = 0;
    arc.format[0] = '\0';
    start_tree();
    snprintf(title, sizeof title, _("Loading %s..."), base_name(path));
    char *args[] = { "list", arc.path, NULL };
    if (run_worker(&j, title, args, NULL) != WORKER_DONE) {
        report(1, "%s: %s", base_name(path), j.status == WORKER_NO_PASSWORD ? _("no password: not opened.") : j.error);
        for (int i = 0; i < nopen; i++) free(was_open[i]);
        free(was_open);
        start_tree();
        return -1;
    }
    sort_tree();
    for (int i = 0; i < nopen; i++) {
        int n = was_open[i] ? find_node(was_open[i]) : -1;
        if (n > 0) nodes[n].open = 1;
        free(was_open[i]);
    }
    free(was_open);
    build_rows(keep[0] ? find_node(keep) : -1);
    report(0, "%s", "");
    return 0;
}

/* ---- Drawing ---- */

static const struct {
    const char *label;
    int key;
} tools[] = {
    { N_("Open"), MODS(KEYS_CTRL) + ('o' & 0x1F) },
    { N_("New"), MODS(KEYS_CTRL) + ('n' & 0x1F) },
    { N_("Extract"), MODS(KEYS_CTRL) + ('e' & 0x1F) },
    { N_("Preview"), MODS(KEYS_CTRL) + ('p' & 0x1F) },
    { N_("Add Files"), MODS(KEYS_ALT) + 'a' },
    { N_("Remove"), K_DEL },
    { N_("Test"), MODS(KEYS_ALT) + 't' },
    { N_("Properties"), MODS(KEYS_ALT) + '\n' },
};

/* The columns' widths: Name takes what the others leave. */
static void columns(int *name, int *size, int *mtime, int *perm, int *owner)
{
    int cols = screen_cols();
    *size = 11;
    *mtime = 17;
    *perm = cols >= 80 ? 11 : 0;
    *owner = cols >= 110 ? 18 : 0;
    *name = cols - 1 - *size - *mtime - *perm - *owner;
}

static int list_height(void)
{
    return screen_rows() - LIST_TOP - 2;
}

static void draw_row(int r, int i)
{
    const struct node *e = &nodes[rows[i]];
    int wn, ws, wm, wp, wo, on = i == sel;
    char cell[PATH_MAX], text[PATH_MAX], tm[32], perm[16];
    uint32_t bg = on ? ACCENT : e->marked ? MARK : BG, fg = on ? BG : e->implicit ? DIM : FG;
    columns(&wn, &ws, &wm, &wp, &wo);
    screen_fill(0, r, screen_cols(), 1, bg);
    snprintf(text, sizeof text, "%*s%s%s%s", depth[i] * 2, "",
             e->type == 'd' ? (e->open || filter[0] ? "- " : "+ ") : "  ", e->name, e->enc ? " *" : "");
    fit(cell, sizeof cell, text, wn - 1);
    screen_text(1, r, cell, fg, bg);
    int c = 1 + wn;
    if (e->type == 'd') {
        snprintf(text, sizeof text, P_("%d item", "%d items", e->kids), e->kids);
    } else {
        human(e->size, text, sizeof text);
    }
    snprintf(cell, sizeof cell, "%*s", ws - 1, text);
    screen_text(c, r, cell, fg, bg);
    c += ws;
    if (e->mtime && !e->implicit) {
        struct tm t;
        localtime_r(&e->mtime, &t);
        strftime(tm, sizeof tm, "%Y-%m-%d %H:%M", &t);
        screen_text(c + 1, r, tm, fg, bg);
    }
    c += wm;
    if (wp && !e->implicit) {
        mode_text(e, perm);
        screen_text(c, r, perm, fg, bg);
    }
    c += wp;
    if (wo && e->user && (e->user[0] || e->group[0])) {
        snprintf(text, sizeof text, "%s:%s", e->user, e->group);
        fit(cell, sizeof cell, text, wo - 1);
        screen_text(c, r, cell, fg, bg);
    }
    ui_hit(0, r, screen_cols(), 1, HIT_ROW + i);
}

static void status_line(char *out, size_t size)
{
    char a[32], b[32];
    int marked = 0;
    long long bytes = 0;
    for (int i = 1; i < nnodes; i++) {
        if (nodes[i].marked) {
            marked++;
            bytes += nodes[i].size;
        }
    }
    if (marked) {
        human(bytes, a, sizeof a);
        snprintf(out, size, P_("%d entry marked (%s)", "%d entries marked (%s)", marked), marked, a);
        return;
    }
    if (sel < nrows) {
        const struct node *e = &nodes[rows[sel]];
        char path[PATH_MAX];
        archive_display(e->key, path, sizeof path);
        if (e->link && e->link[0]) {
            snprintf(out, size, "%s -> %s", path, e->link);
        } else {
            human(e->size, a, sizeof a);
            snprintf(out, size, "%s   %s%s", path, e->type == 'd' ? "" : a, e->enc ? _("   encrypted") : "");
        }
        return;
    }
    human(arc.bytes, a, sizeof a);
    human(arc.packed, b, sizeof b);
    snprintf(out, size, _("%d files, %d folders: %s (%s packed)"), arc.files, arc.dirs, a, b);
}

static void draw(void)
{
    int cols = screen_cols(), rows_n = screen_rows(), h = list_height();
    int wn, ws, wm, wp, wo;
    char line[PATH_MAX + 256], name[PATH_MAX];
    screen_fill(0, 0, cols, rows_n, BG);
    ui_hits_clear();
    screen_bar(_("archive"), NULL);
    screen_fill(0, 1, cols, 1, BAR_BG);
    int c = 1;
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++) {
        c = ui_button(c, 1, _(tools[i].label), HIT_KEY + tools[i].key);
    }
    if (arc.path[0]) {
        cut_fmt(line, sizeof line, "%s   %s%s%s%s%s", base_name(arc.path), arc.format, arc.filters[0] ? ", " : "",
                 arc.filters, arc.readonly ? _("   read-only") : "", arc.encrypted || arc.headers ? _("   encrypted") : "");
    } else {
        snprintf(line, sizeof line, "%s", _("No archive: Ctrl+O opens one, Ctrl+N makes a new one."));
    }
    fit(name, sizeof name, line, cols - 2);
    screen_text(1, 2, name, ACCENT, BG);
    if (filter[0]) {
        snprintf(line, sizeof line, _("Find: %s"), filter);
        screen_text(cols - ui_cells(line) - 1 > 0 ? cols - ui_cells(line) - 1 : 0, 2, line, BG, ACCENT);
    }
    columns(&wn, &ws, &wm, &wp, &wo);
    screen_fill(0, 3, cols, 1, BAR_BG);
    screen_text(1, 3, _("Name"), DIM, BAR_BG);
    snprintf(line, sizeof line, "%*s", ws - 1, _("Size"));
    screen_text(1 + wn, 3, line, DIM, BAR_BG);
    screen_text(2 + wn + ws, 3, _("Modified"), DIM, BAR_BG);
    if (wp) screen_text(1 + wn + ws + wm, 3, _("Permissions"), DIM, BAR_BG);
    if (wo) screen_text(1 + wn + ws + wm + wp, 3, _("Owner"), DIM, BAR_BG);
    if (sel >= nrows) sel = nrows ? nrows - 1 : 0;
    if (sel < top) top = sel;
    if (sel >= top + h) top = sel - h + 1;
    if (top > (nrows > h ? nrows - h : 0)) top = nrows > h ? nrows - h : 0;
    if (top < 0) top = 0;
    for (int i = top; i < nrows && i < top + h; i++) draw_row(LIST_TOP + i - top, i);
    if (arc.path[0] && nrows == 0) {
        screen_text(3, LIST_TOP + 1, filter[0] ? _("Nothing has that in its name.") : _("The archive is empty."), DIM, BG);
    }
    status_line(line, sizeof line);
    fit(name, sizeof name, line, cols - 2);
    screen_fill(0, rows_n - 2, cols, 1, BAR_BG);
    screen_text(1, rows_n - 2, name, FG, BAR_BG);
    if (msg[0]) {
        fit(name, sizeof name, msg, cols - 2);
        screen_text(1, rows_n - 1, name, msg_error ? ERROR : ACCENT, BG);
    } else {
        /* TRANSLATORS: key names (Enter, Ctrl+E, Alt+A, Del, Alt+T, Alt+Return, q); keep them. */
        fit(name, sizeof name, _("Enter preview  o open  Ctrl+E extract  Alt+A add  Del remove  "
                                 "Alt+T test  Alt+Return properties  Ctrl+F find  q quit"), cols - 2);
        screen_text(1, rows_n - 1, name, DIM, BG);
    }
    screen_flush();
}

/* ---- What is acted on ---- */

/* The marked entries' keys, else the selected one's: how many. */
static int targets(char **keys, int max)
{
    int n = 0;
    for (int i = 1; i < nnodes && n < max; i++) {
        if (nodes[i].marked) keys[n++] = nodes[i].key;
    }
    if (n == 0 && sel < nrows && max > 0) keys[n++] = nodes[rows[sel]].key;
    return n;
}

/* The archive's folder where what is added goes: the selected folder, or
 * the selected entry's (Ark's Add Files); "" for its top. */
static const char *add_folder(void)
{
    if (sel >= nrows) return "";
    const struct node *e = &nodes[rows[sel]];
    return e->type == 'd' ? e->key : nodes[e->parent].key;
}

static int writable(void)
{
    if (!arc.path[0]) {
        report(1, "%s", _("No archive is open."));
        return 0;
    }
    if (arc.readonly) {
        report(1, "%s", _("The archive can only be read."));
        return 0;
    }
    return 1;
}

/* ---- Preview and open ---- */

/* The selected file extracted alone into the user's ARCHIVE_PREVIEW_DIR
 * (a folder of this run's): its path there in out. 0, or -1. */
static int extract_for_preview(char *out, size_t size)
{
    struct job j = { 0 };
    char dir[PATH_MAX], key[PATH_MAX], title[PATH_MAX + 32];
    if (sel >= nrows) return -1;
    const struct node *e = &nodes[rows[sel]];
    if (e->type == 'd') return -1;
    if (cut_fmt(dir, sizeof dir, "%s/%s/%d-%d", user_home, ARCHIVE_PREVIEW_DIR, (int)getpid(), ++preview_seq) < 0) {
        return -1;
    }
    snprintf(key, sizeof key, "%s", e->key);
    snprintf(title, sizeof title, _("Extracting %s..."), e->name);
    char *args[] = { "extract", arc.path, dir, key, NULL };
    if (run_worker(&j, title, args, NULL) != WORKER_DONE) {
        if (j.status != WORKER_CANCELLED) show_problems(&j, _("It could not be extracted."));
        return -1;
    }
    archive_unescape(key);
    return cut_fmt(out, size, "%s/%s", dir, base_name(key));
}

/* Preview (the framebuffer program for its type, as Ark's internal
 * viewer), open (its application), or open with (home asks). */
enum { OPEN_PREVIEW, OPEN_APP, OPEN_ASK };

static void open_entry(int how)
{
    char path[PATH_MAX], type[OPEN_TYPE], names[8][OPEN_NAME];
    const char *with = how == OPEN_ASK ? "ask" : NULL;
    if (sel < nrows && nodes[rows[sel]].type == 'd') {
        nodes[rows[sel]].open = !nodes[rows[sel]].open;
        build_rows(rows[sel]);
        return;
    }
    if (extract_for_preview(path, sizeof path) < 0) return;
    if (how == OPEN_PREVIEW) {
        as_user(1);
        int typed = open_type(path, type, sizeof type) == 0;
        as_user(0);
        int n = typed ? open_candidates(type, names, 8) : 0;
        for (int i = 0; i < n && !with; i++) {
            if (strncmp(names[i], OPEN_PROGRAM, strlen(OPEN_PROGRAM)) == 0) with = names[i];
        }
        if (!with) {
            /* TRANSLATORS: o is the key; keep it. */
            report(1, _("Nothing here previews %s: o opens it in its application."), typed ? type : base_name(path));
            return;
        }
    }
    if (open_in_home(with, path) < 0) {
        report(1, _("Cannot ask home to open %s."), base_name(path));
    } else {
        report(0, _("Opened %s."), base_name(path));
    }
}

/* ---- Dialogs ---- */

/* A dialog: lines of a label and a value, buttons at the end. */
struct dialog_line {
    const char *label;
    char value[PATH_MAX + 64];
    int id;
};

static void draw_dialog(const char *title, const struct dialog_line *l, int n, int cur, const char *const *buttons,
                        int button, const char *hint)
{
    int cols = screen_cols(), rows_n = screen_rows(), vx = 34;
    char text[PATH_MAX + 64];
    screen_fill(0, 0, cols, rows_n, BG);
    ui_hits_clear();
    screen_bar(_("archive"), NULL);
    screen_text(1, 1, title, ACCENT, BG);
    for (int i = 0; i < n; i++) {
        int r = 3 + i, on = i == cur;
        screen_fill(0, r, cols, 1, on ? ACCENT : BG);
        screen_text(2, r, l[i].label, on ? BG : DIM, on ? ACCENT : BG);
        fit(text, sizeof text, l[i].value, cols - vx - 1);
        screen_text(vx, r, text, on ? BG : FG, on ? ACCENT : BG);
        ui_hit(0, r, cols, 1, HIT_FORM + i);
    }
    int x = vx, r = 4 + n;
    for (int b = 0; buttons[b]; b++) {
        int lit = cur == n && b == button;
        snprintf(text, sizeof text, "[ %s ]", buttons[b]);
        screen_text(x, r, text, lit ? BG : FG, lit ? ACCENT : BG);
        ui_hit(x, r, ui_cells(text), 1, HIT_FORM + n + 1 + b);
        x += ui_cells(text) + 2;
    }
    screen_text(1, rows_n - 2, hint, DIM, BG);
    if (msg[0]) screen_text(1, rows_n - 1, msg, msg_error ? ERROR : ACCENT, BG);
    screen_flush();
}

/* The next key of a dialog, as keys: a click on a line selects it (*cur),
 * a double click or one on a button acts as Enter. */
static int dialog_key(int *cur, int *button, int n)
{
    int key = key_get();
    if (key != K_POINTER) return key;
    const struct pointer_event *e = pointer_last();
    if (!e->button || !e->pressed) return 0;
    int id = ui_hit_at(e->col, e->row) - HIT_FORM;
    if (id >= 0 && id < n) {
        *cur = id;
        return e->clicks == 2 ? '\n' : 0;
    }
    if (id > n) {
        *cur = n;
        *button = id - n - 1;
        return '\n';
    }
    return 0;
}

/* Ark's extraction dialog. */
struct extraction {
    char dest[PATH_MAX], sub[NAME_MAX + 1];
    int use_sub, selected, preserve, close_after;
};

static int pick_folder(char *dir, size_t size, const char *what)
{
    char out[PATH_MAX];
    if (!files_pick(dir, NULL, "inode/directory", what, out, sizeof out)) return 0;
    snprintf(dir, size, "%s", out);
    return 1;
}

static int extraction_dialog(struct extraction *x, int can_select)
{
    static const char *buttons[3];
    int cur = 0, button = 0;
    buttons[0] = _("Extract");
    buttons[1] = _("Cancel");
    buttons[2] = NULL;
    report(0, "%s", "");
    for (;;) {
        struct dialog_line l[5] = {
            { _("Destination:"), "", 0 },
            { _("Extraction into subfolder:"), "", 1 },
            { _("Extract:"), "", 2 },
            { _("Preserve paths when extracting:"), "", 3 },
            { _("Close after extraction:"), "", 4 },
        };
        snprintf(l[0].value, sizeof l[0].value, "%s", x->dest);
        snprintf(l[1].value, sizeof l[1].value, "[%c] %s", x->use_sub ? 'x' : ' ', x->sub);
        snprintf(l[2].value, sizeof l[2].value, "%s", x->selected ? _("Selected files only") : _("All files"));
        snprintf(l[3].value, sizeof l[3].value, "[%c]", x->preserve ? 'x' : ' ');
        snprintf(l[4].value, sizeof l[4].value, "[%c]", x->close_after ? 'x' : ' ');
        /* TRANSLATORS: key names; keep them. */
        draw_dialog(_("Extract"), l, 5, cur, buttons, button,
                    _("Up/Down: line   Enter: choose or edit   Space: switch   F2: type the destination   Esc: cancel"));
        int key = dialog_key(&cur, &button, 5);
        if (key == 27) return 0;
        if (key == K_UP) cur = clampi(cur - 1, 0, 5);
        else if (key == K_DOWN || key == '\t') cur = clampi(cur + 1, 0, 5);
        else if (cur == 5 && (key == K_LEFT || key == K_RIGHT)) button = !button;
        else if (cur == 5 && key == '\n') {
            if (button) return 0;
            if (!x->dest[0]) {
                report(1, "%s", _("Where to? No destination."));
                continue;
            }
            if (x->use_sub && (!x->sub[0] || strchr(x->sub, '/'))) {
                report(1, "%s", _("The subfolder needs a name (without /)."));
                continue;
            }
            return 1;
        } else if (cur == 0 && key == '\n') {
            pick_folder(x->dest, sizeof x->dest, _("Extract to: o chooses the selected folder."));
        } else if (cur == 0 && key == K_F1 + 1) {
            edit_text(_("Destination:"), x->dest, sizeof x->dest, 0);
        } else if (cur == 1 && key == '\n') {
            if (edit_text(_("Subfolder:"), x->sub, sizeof x->sub, 0)) x->use_sub = 1;
        } else if (key == ' ' || key == '\n') {
            if (cur == 1) x->use_sub = !x->use_sub;
            if (cur == 2 && can_select) x->selected = !x->selected;
            if (cur == 3) x->preserve = !x->preserve;
            if (cur == 4) x->close_after = !x->close_after;
        }
    }
}

/* The defaults as Ark's: here (the archive's folder), into a subfolder
 * named as the archive when it holds more than one thing. */
static void extraction_defaults(struct extraction *x, int selected)
{
    memset(x, 0, sizeof *x);
    dir_of(arc.path, x->dest, sizeof x->dest);
    int single_folder = nodes[0].kids == 1 && nodes[nodes[0].first].type == 'd';
    snprintf(x->sub, sizeof x->sub, "%s", single_folder ? nodes[nodes[0].first].name : arc.stem);
    x->use_sub = nodes[0].kids > 1;
    x->selected = selected;
    x->preserve = 1;
}

/* Extract as x says: the worker's status. */
static int extract_with(const struct extraction *x)
{
    struct job j = { 0 };
    char *args[520], *keys[500], title[PATH_MAX + 32];
    int n = 0;
    args[n++] = "extract";
    if (x->preserve) args[n++] = "-p";
    if (x->use_sub) {
        args[n++] = "-n";
        args[n++] = (char *)x->sub;
    }
    args[n++] = arc.path;
    args[n++] = (char *)x->dest;
    int k = x->selected ? targets(keys, 500) : 0;
    for (int i = 0; i < k; i++) args[n++] = keys[i];
    args[n] = NULL;
    snprintf(title, sizeof title, _("Extracting %s..."), base_name(arc.path));
    run_worker(&j, title, args, NULL);
    if (j.status == WORKER_DONE) {
        report(0, _("Extracted to %s."), j.done);
    } else if (j.status == WORKER_CANCELLED) {
        report(1, "%s", _("Extraction cancelled."));
    } else {
        show_problems(&j, j.status == WORKER_NO_PASSWORD ? _("No password: nothing extracted.")
                                                          : _("Extraction failed."));
        report(1, "%s", _("Extraction failed."));
    }
    return j.status;
}

/* The selection, as Ark's, is what is marked; with nothing marked,
 * "Selected files only" is the entry at the cursor. */
static void extract(int all)
{
    struct extraction x;
    int marked = 0;
    if (!arc.path[0]) return;
    for (int i = 1; i < nnodes; i++) marked |= nodes[i].marked;
    extraction_defaults(&x, !all && marked);
    if (!extraction_dialog(&x, nrows > 0)) return;
    if (extract_with(&x) == WORKER_DONE && x.close_after) quit = 1;
}

/* Ark's create dialog (with its compression options). */
struct creation {
    char folder[PATH_MAX], name[NAME_MAX + 1], password[sizeof arc.password];
    int type, level, encrypt_list, volumes;
    long long volume_mb;
};

static const struct archive_type *ctype(const struct creation *c)
{
    return &archive_types[c->type];
}

static int creation_path(const struct creation *c, char *out, size_t size)
{
    const char *suffix = ctype(c)->suffix;
    int has = archive_ends_with(c->name, suffix);
    return snprintf(out, size, "%s/%s%s", c->folder, c->name, has ? "" : suffix) < (int)size ? 0 : -1;
}

static int creation_dialog(struct creation *c, const char *title)
{
    static const char *buttons[3];
    int cur = 1, button = 0, ids[8];
    buttons[0] = _("Create");
    buttons[1] = _("Cancel");
    buttons[2] = NULL;
    report(0, "%s", "");
    for (;;) {
        const struct archive_type *t = ctype(c);
        struct dialog_line l[8];
        int n = 0;
        if (c->password[0] && !t->password) c->password[0] = '\0';
        if (!c->password[0] || t->password != 2) c->encrypt_list = 0;
#define LINE(lab, id_, ...) (l[n].label = (lab), l[n].id = (id_), snprintf(l[n].value, sizeof l[n].value, __VA_ARGS__), ids[n] = (id_), n++)
        LINE(_("Folder:"), 0, "%s", c->folder);
        LINE(_("Filename:"), 1, "%s%s", c->name, archive_ends_with(c->name, t->suffix) ? "" : t->suffix);
        LINE(_("Type:"), 2, "%s (%s)", _(t->name), t->suffix);
        if (t->lmax) LINE(_("Compression level:"), 3, "%d   (%d-%d)", c->level, t->lmin, t->lmax);
        if (t->password) LINE(_("Password:"), 4, "%s", c->password[0] ? "********" : _("(none)"));
        if (t->password == 2 && c->password[0]) {
            LINE(_("Encrypt the file list too:"), 5, "[%c]", c->encrypt_list ? 'x' : ' ');
        }
        if (t->password == 1 && c->password[0]) LINE(_("Encryption method:"), 7, "%s", "AES-256");
        LINE(_("Multi-volume archive:"), 6, c->volumes ? _("[x] volumes of %lld MB") : "[ ]", c->volume_mb);
#undef LINE
        /* TRANSLATORS: key names; keep them. */
        draw_dialog(title, l, n, cur, buttons, button,
                    _("Up/Down: line   Enter: edit or choose   Left/Right: type, level   Space: switch   Esc: cancel"));
        int key = dialog_key(&cur, &button, n), id = cur < n ? ids[cur] : -1;
        if (key == 27) return 0;
        if (key == K_UP) cur = clampi(cur - 1, 0, n);
        else if (key == K_DOWN || key == '\t') cur = clampi(cur + 1, 0, n);
        else if (cur == n && (key == K_LEFT || key == K_RIGHT)) button = !button;
        else if (cur == n && key == '\n') {
            char path[PATH_MAX];
            struct stat st;
            if (button) return 0;
            if (!c->name[0] || strchr(c->name, '/') || creation_path(c, path, sizeof path) < 0) {
                report(1, "%s", _("The archive needs a name (without /)."));
                continue;
            }
            as_user(1);
            int there = lstat(path, &st) == 0;
            as_user(0);
            if (there) {
                report(1, _("%s is there already."), base_name(path));
                continue;
            }
            if (t->password == 2 && c->password[0] && access(ARCHIVE_7Z, X_OK) != 0) {
                report(1, "%s", _("A 7z archive with a password needs 7-Zip (settings: Optional Components)."));
                continue;
            }
            return 1;
        } else if (id == 0 && key == '\n') {
            pick_folder(c->folder, sizeof c->folder, _("Create in: o chooses the selected folder."));
        } else if (id == 1 && key == '\n') {
            edit_text(_("Filename:"), c->name, sizeof c->name, 0);
        } else if (id == 2 && (key == K_LEFT || key == K_RIGHT || key == '\n')) {
            int ntypes = 0;
            while (archive_types[ntypes].suffix) ntypes++;
            if (key == '\n') {
                const char *items[16];
                for (int i = 0; i < ntypes && i < 15; i++) items[i] = _(archive_types[i].name);
                items[ntypes < 15 ? ntypes : 15] = NULL;
                int got = ui_menu(34, 5, items);
                if (got >= 0) c->type = got;
            } else {
                c->type = (c->type + (key == K_RIGHT ? 1 : ntypes - 1)) % ntypes;
            }
            c->level = ctype(c)->ldef;
        } else if (id == 3 && (key == K_LEFT || key == K_RIGHT)) {
            c->level = clampi(c->level + (key == K_RIGHT ? 1 : -1), t->lmin, t->lmax);
        } else if (id == 4 && key == '\n') {
            char pw[sizeof c->password] = "", again[sizeof c->password] = "";
            if (edit_text(_("Password:"), pw, sizeof pw, 1) &&
                (!pw[0] || edit_text(_("Confirm password:"), again, sizeof again, 1))) {
                if (strcmp(pw, again) == 0 || !pw[0]) snprintf(c->password, sizeof c->password, "%s", pw);
                else report(1, "%s", _("The passwords do not match."));
            }
            explicit_bzero(pw, sizeof pw);
            explicit_bzero(again, sizeof again);
        } else if (id == 5 && (key == ' ' || key == '\n')) {
            c->encrypt_list = !c->encrypt_list;
        } else if (id == 6 && key == ' ') {
            c->volumes = !c->volumes;
        } else if (id == 6 && key == '\n') {
            char text[32];
            snprintf(text, sizeof text, "%lld", c->volume_mb);
            if (edit_text(_("Volume size (MB):"), text, sizeof text, 0) && atoll(text) > 0) {
                c->volume_mb = atoll(text);
                c->volumes = 1;
            }
        }
    }
}

static void creation_defaults(struct creation *c, const char *folder, const char *name)
{
    memset(c, 0, sizeof *c);
    snprintf(c->folder, sizeof c->folder, "%s", folder);
    snprintf(c->name, sizeof c->name, "%s", name);
    c->level = archive_types[0].ldef;
    c->volume_mb = 100;
}

/* The archive c describes made of names in dir: the worker's status (done:
 * its path in made). */
static int create_with(const struct creation *c, const char *dir, char **names, int n, char *made, size_t size)
{
    struct job j = { 0 };
    char *args[520], path[PATH_MAX], level[16], vol[32], title[PATH_MAX + 32];
    int k = 0;
    if (creation_path(c, path, sizeof path) < 0) return WORKER_FAILED;
    args[k++] = "create";
    if (ctype(c)->lmax) {
        snprintf(level, sizeof level, "%d", c->level);
        args[k++] = "-l";
        args[k++] = level;
    }
    if (c->volumes) {
        snprintf(vol, sizeof vol, "%lld", c->volume_mb * 1000 * 1000);
        args[k++] = "-v";
        args[k++] = vol;
    }
    if (c->password[0]) args[k++] = "-P";
    if (c->encrypt_list) args[k++] = "-H";
    args[k++] = path;
    args[k++] = (char *)dir;
    for (int i = 0; i < n && k < 518; i++) args[k++] = names[i];
    args[k] = NULL;
    snprintf(title, sizeof title, _("Compressing into %s..."), base_name(path));
    run_worker(&j, title, args, c->password[0] ? c->password : NULL);
    if (j.status == WORKER_DONE) {
        snprintf(made, size, "%s", j.done);
    } else if (j.status != WORKER_CANCELLED) {
        show_problems(&j, _("The archive could not be made."));
    }
    return j.status;
}

/* What files_pick chose to add: a file or a folder (asked first). 1 with
 * its folder in dir and its name in name. */
static int pick_to_add(char *dir, size_t dsize, char *name, size_t nsize)
{
    const char *items[] = { _("Add a file..."), _("Add a folder..."), NULL };
    char out[PATH_MAX], from[PATH_MAX];
    int got = ui_menu(2, 2, items);
    if (got < 0) return 0;
    if (arc.path[0]) dir_of(arc.path, from, sizeof from);
    else snprintf(from, sizeof from, "%s", user_home);
    if (!files_pick(from, NULL, got ? "inode/directory" : "", got ? _("Add: o chooses the selected folder.")
                                                              : _("Add: Enter chooses the selected file."),
                    out, sizeof out)) {
        return 0;
    }
    dir_of(out, dir, dsize);
    cut_fmt(name, nsize, "%s", base_name(out));
    return 1;
}

static void new_archive(void)
{
    struct creation c;
    char from[PATH_MAX], dir[PATH_MAX], name[NAME_MAX + 1], made[PATH_MAX];
    snprintf(from, sizeof from, "%s", user_home);
    if (arc.path[0]) dir_of(arc.path, from, sizeof from);
    creation_defaults(&c, from, _("Archive"));
    if (!creation_dialog(&c, _("New archive"))) return;
    /* As Ark's: written when something is added to it. */
    if (!pick_to_add(dir, sizeof dir, name, sizeof name)) {
        report(1, "%s", _("Nothing added: no archive made."));
        return;
    }
    char *names[] = { name };
    if (create_with(&c, dir, names, 1, made, sizeof made) == WORKER_DONE && load_archive(made) == 0 && c.password[0]) {
        /* As Ark's: the password it was made with, for this session. */
        snprintf(arc.password, sizeof arc.password, "%s", c.password);
        arc.have_password = 1;
    }
    explicit_bzero(c.password, sizeof c.password);
}

static void add_files(void)
{
    struct job j = { 0 };
    char dir[PATH_MAX], name[NAME_MAX + 1], inside[PATH_MAX], title[PATH_MAX + 32];
    if (!writable() || !pick_to_add(dir, sizeof dir, name, sizeof name)) return;
    snprintf(inside, sizeof inside, "%s", add_folder());
    char *args[] = { "add", "-i", inside, arc.path, dir, name, NULL };
    char *noin[] = { "add", arc.path, dir, name, NULL };
    snprintf(title, sizeof title, _("Adding %s..."), name);
    run_worker(&j, title, inside[0] ? args : noin, NULL);
    if (j.status == WORKER_DONE) {
        load_archive(arc.path);
        report(0, _("%s added."), name);
    } else if (j.status != WORKER_CANCELLED) {
        show_problems(&j, _("Nothing was added."));
    }
}

static void remove_entries(void)
{
    struct job j = { 0 };
    char *args[520], question[256];
    int k = 0;
    if (!writable()) return;
    args[k++] = "delete";
    args[k++] = arc.path;
    int n = targets(args + k, 500);
    if (n == 0) return;
    k += n;
    args[k] = NULL;
    snprintf(question, sizeof question, P_("Remove %d entry from the archive?", "Remove %d entries from the archive?", n), n);
    if (!yes(question)) return;
    run_worker(&j, _("Removing..."), args, NULL);
    if (j.status == WORKER_DONE) {
        load_archive(arc.path);
        report(0, P_("%d entry removed.", "%d entries removed.", n), n);
    } else if (j.status != WORKER_CANCELLED) {
        show_problems(&j, _("Nothing was removed."));
    }
}

static void test_archive(void)
{
    struct job j = { 0 };
    char *args[] = { "test", arc.path, NULL };
    if (!arc.path[0]) return;
    run_worker(&j, _("Testing the archive..."), args, NULL);
    if (j.status == WORKER_DONE) {
        report(0, "%s", _("The archive passed the integrity test."));
    } else if (j.status != WORKER_CANCELLED) {
        show_problems(&j, _("The archive failed the integrity test."));
        report(1, "%s", _("The archive failed the integrity test."));
    }
}

/* A line of the properties: the label padded to a column (in cells). */
static void prop_line(char *out, size_t size, const char *label, const char *value)
{
    char pad[256];
    cut_fmt(out, size, "%s %s", ui_pad(pad, sizeof pad, label, 24), value);
}

/* Ark's properties dialog. */
static void properties(void)
{
    struct job j = { 0 };
    struct stat st;
    char *args[] = { "hash", arc.path, NULL };
    char lines[16][PATH_MAX], a[32], b[32], when[64] = "", type[OPEN_TYPE] = "", kind[300], vols[16];
    int n = 0;
    if (!arc.path[0]) return;
    run_worker(&j, _("Reading the archive..."), args, NULL);
    as_user(1);
    if (stat(arc.path, &st) == 0) strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&st.st_mtime));
    if (open_type(arc.path, type, sizeof type) != 0) type[0] = '\0';
    as_user(0);
    human(arc.bytes, a, sizeof a);
    human(arc.packed, b, sizeof b);
    cut_fmt(kind, sizeof kind, "%s%s%s", arc.format, arc.filters[0] ? ", " : "", arc.filters);
    char count[128], ratio[32];
    cut_fmt(count, sizeof count, _("%d (%d files, %d folders)"), arc.files + arc.dirs, arc.files, arc.dirs);
    snprintf(ratio, sizeof ratio, "%.1f%%", arc.bytes > 0 ? 100.0 * (double)arc.packed / (double)arc.bytes : 0.0);
    snprintf(vols, sizeof vols, "%d", arc.volumes);
#define PROP(label, value) prop_line(lines[n++], sizeof lines[0], (label), (value))
    PROP(_("Archive name:"), base_name(arc.path));
    PROP(_("Archive type:"), kind);
    PROP(_("Mime-type:"), type);
    PROP(_("Opened by:"), arc.backend);
    PROP(_("Opened read-only:"), arc.readonly ? _("yes") : _("no"));
    PROP(_("Password-protected:"), arc.encrypted || arc.headers ? _("yes") : _("no"));
    PROP(_("Number of entries:"), count);
    PROP(_("Unpacked size:"), a);
    PROP(_("Packed size:"), b);
    if (arc.bytes > 0) PROP(_("Compression ratio:"), ratio);
    PROP(_("Last modified:"), when);
    PROP(_("Multi-volume:"), arc.volumes > 1 ? _("yes") : _("no"));
    if (arc.volumes > 1) PROP(_("Number of volumes:"), vols);
    PROP(_("MD5 hash:"), j.hashes[0]);
    PROP(_("SHA-1 hash:"), j.hashes[1]);
    PROP(_("SHA-256 hash:"), j.hashes[2]);
#undef PROP
    int cols = screen_cols();
    char cut[PATH_MAX];
    screen_fill(0, 0, cols, screen_rows(), BG);
    ui_hits_clear();
    screen_bar(_("archive"), NULL);
    screen_text(1, 1, _("Properties"), ACCENT, BG);
    for (int i = 0; i < n; i++) {
        fit(cut, sizeof cut, lines[i], cols - 4);
        screen_text(2, 3 + i, cut, FG, BG);
    }
    screen_text(1, screen_rows() - 1, _("Any key: back"), DIM, BG);
    screen_flush();
    key_get();
}

/* ---- Keys and the pointer ---- */

static void absolute(const char *path, char *out, size_t size)
{
    char real[PATH_MAX];
    as_user(1);
    int ok = realpath(path, real) != NULL;
    as_user(0);
    snprintf(out, size, "%s", ok ? real : path);
}

static void find(void)
{
    char text[sizeof filter];
    snprintf(text, sizeof text, "%s", filter);
    if (!edit_text(_("Find:"), text, sizeof text, 0)) return;
    snprintf(filter, sizeof filter, "%s", text);
    build_rows(-1);
}

static void open_other(void)
{
    char from[PATH_MAX], out[PATH_MAX];
    if (arc.path[0]) dir_of(arc.path, from, sizeof from);
    else snprintf(from, sizeof from, "%s", user_home);
    if (!files_pick(from, arc.path[0] ? base_name(arc.path) : NULL, "", _("Open an archive: Enter chooses it."),
                    out, sizeof out)) {
        return;
    }
    filter[0] = '\0';
    load_archive(out);
}

/* The menu of the selected entry (Ark's context menu), as a key. */
static int entry_menu(int col, int row)
{
    const char *items[12];
    int keys[12], n = 0;
#define ITEM(label, k) (items[n] = (label), keys[n++] = (k))
    if (sel < nrows && nodes[rows[sel]].type != 'd') {
        ITEM(_("Preview"), MODS(KEYS_CTRL) + ('p' & 0x1F));
        ITEM(_("Open in External Application"), 'o');
        ITEM(_("Open With..."), 'O');
    }
    ITEM(_("Extract..."), MODS(KEYS_CTRL) + ('e' & 0x1F));
    ITEM(_("Extract All..."), MODS(KEYS_CTRL | KEYS_SHIFT) + ('e' & 0x1F));
    if (!arc.readonly) {
        ITEM(_("Add Files..."), MODS(KEYS_ALT) + 'a');
        ITEM(_("Remove from Archive"), K_DEL);
    }
    ITEM(_("Test Integrity"), MODS(KEYS_ALT) + 't');
    ITEM(_("Properties"), MODS(KEYS_ALT) + '\n');
#undef ITEM
    items[n] = NULL;
    int got = ui_menu(col, row, items);
    return got < 0 ? 0 : keys[got];
}

static void move_sel(int to)
{
    if (nrows) sel = clampi(to, 0, nrows - 1);
}

/* One key, with MODS() of the modifiers held. */
static void handle(int key)
{
    int mods = (key >> 16) & 0xF, ctrl = mods & KEYS_CTRL, alt = mods & KEYS_ALT;
    struct node *e = sel < nrows ? &nodes[rows[sel]] : NULL;
    key &= 0xFFFF;
    if (key != K_SHOWN) report(0, "%s", "");
    if (key == K_UP) {
        move_sel(sel - 1);
    } else if (key == K_DOWN) {
        move_sel(sel + 1);
    } else if (key == K_PGUP) {
        move_sel(sel - list_height());
    } else if (key == K_PGDN) {
        move_sel(sel + list_height());
    } else if (key == K_BOL) {
        move_sel(0);
    } else if (key == K_EOL) {
        move_sel(nrows - 1);
    } else if (key == K_LEFT && e) {
        if (e->type == 'd' && e->open && !filter[0]) {
            e->open = 0;
            build_rows(rows[sel]);
        } else if (e->parent > 0) {
            build_rows(e->parent);
        }
    } else if (key == K_RIGHT && e && e->type == 'd') {
        if (!e->open) {
            e->open = 1;
            build_rows(rows[sel]);
        } else {
            move_sel(sel + 1);
        }
    } else if (key == '\n' && alt) {
        properties();
    } else if (key == '\n' || (key == ('p' & 0x1F) && ctrl)) {
        open_entry(OPEN_PREVIEW);
    } else if (key == 'o' && !ctrl && !alt) {
        open_entry(OPEN_APP);
    } else if (key == 'O' && !ctrl && !alt) {
        open_entry(OPEN_ASK);
    } else if (key == ('e' & 0x1F) && ctrl) {
        extract(mods & KEYS_SHIFT);
    } else if ((key == 'a' || key == 'A') && alt) {
        add_files();
    } else if (key == K_DEL) {
        remove_entries();
    } else if ((key == 't' || key == 'T') && alt) {
        test_archive();
    } else if (key == ('n' & 0x1F) && ctrl) {
        new_archive();
    } else if (key == ('o' & 0x1F) && ctrl) {
        open_other();
    } else if ((key == ('f' & 0x1F) && ctrl) || key == '/') {
        find();
    } else if (key == ' ' && e) {
        e->marked = !e->marked;
        move_sel(sel + 1);
    } else if (key == ('a' & 0x1F) && ctrl) {
        int all = 1;
        for (int i = 1; i < nnodes; i++) all &= nodes[i].marked;
        for (int i = 1; i < nnodes; i++) nodes[i].marked = !all;
    } else if (key == 'q' || (key == ('q' & 0x1F) && ctrl)) {
        quit = 1;
    } else if (key == 27) {
        if (filter[0]) {
            filter[0] = '\0';
            build_rows(-1);
        } else {
            quit = 1;
        }
    } else if (key == K_F1 + 9 && (mods & KEYS_SHIFT)) {
        int k = entry_menu(2, LIST_TOP + sel - top);
        if (k) handle(k);
    }
}

static void pointer(void)
{
    const struct pointer_event *e = pointer_last();
    if (e->wheel) {
        move_sel(sel - 3 * e->wheel);
        return;
    }
    if (!e->button || !e->pressed) return;
    int id = ui_hit_at(e->col, e->row);
    if (id >= HIT_KEY) {
        handle(id - HIT_KEY);
    } else if (id >= HIT_ROW && id < HIT_ROW + nrows) {
        sel = id - HIT_ROW;
        if (e->button == BTN_RIGHT) {
            int k = entry_menu(e->col, e->row);
            if (k) handle(k);
        } else if (e->clicks == 2) {
            handle('\n');
        }
    }
}

/* ---- What the file manager asks for ---- */

static int job_exit(int status)
{
    return status == WORKER_DONE ? 0 : status == WORKER_CANCELLED ? 2 : 1;
}

/* Ark's batch extraction: each archive into its folder (autodestination) or
 * destination, a subfolder as "Extract here" decides (autosubfolder); with
 * dialog, the extraction dialog first. */
static int batch_extract(char **files, int n, int dialog, int autosub, const char *destination)
{
    for (int i = 0; i < n; i++) {
        struct job j = { 0 };
        char path[PATH_MAX], dest[PATH_MAX], title[PATH_MAX + 32];
        absolute(files[i], path, sizeof path);
        if (dialog) {
            struct extraction x;
            if (load_archive(path) < 0) {
                cut_fmt(j.error, sizeof j.error, "%s", msg);
                show_problems(&j, _("The archive could not be opened."));
                return 1;
            }
            extraction_defaults(&x, 0);
            if (destination) snprintf(x.dest, sizeof x.dest, "%s", destination);
            if (!extraction_dialog(&x, 0)) return 2;
            int st = job_exit(extract_with(&x));
            if (st) return st;
            continue;
        }
        snprintf(arc.path, sizeof arc.path, "%s", path);
        arc.bytes = 0;
        if (destination) snprintf(dest, sizeof dest, "%s", destination);
        else dir_of(path, dest, sizeof dest);
        char *with_sub[] = { "extract", "-p", "-s", path, dest, NULL };
        char *without[] = { "extract", "-p", path, dest, NULL };
        snprintf(title, sizeof title, _("Extracting %s..."), base_name(path));
        if (run_worker(&j, title, autosub ? with_sub : without, NULL) != WORKER_DONE) {
            if (j.status != WORKER_CANCELLED) show_problems(&j, _("Extraction failed."));
            return job_exit(j.status);
        }
    }
    return 0;
}

/* Ark's --add-to ARCHIVE (made, or added to if it is there), --add (its
 * create dialog) or --autofilename SUFFIX: of files in one folder (Ark's
 * --changetofirstpath: their paths from it). */
static int batch_add(char **files, int n, const char *add_to, int dialog, const char *suffix)
{
    char dir[PATH_MAX], other[PATH_MAX], name[NAME_MAX + 1], made[PATH_MAX], path[PATH_MAX];
    static char name_of[500][NAME_MAX + 1];
    char *names[500];
    struct creation c;
    struct stat st;
    if (n <= 0 || n > 500) return 1;
    for (int i = 0; i < n; i++) {
        absolute(files[i], path, sizeof path);
        dir_of(path, i ? other : dir, PATH_MAX);
        if (i && strcmp(other, dir) != 0) {
            struct job j = { 0 };
            snprintf(j.error, sizeof j.error, "%s", _("The files to compress are to be in one folder."));
            show_problems(&j, _("Nothing was compressed."));
            return 1;
        }
        cut_fmt(name_of[i], sizeof name_of[i], "%s", base_name(path));
        names[i] = name_of[i];
    }
    archive_name_for((const char *const *)names, n, _("Archive"), name, sizeof name);
    creation_defaults(&c, dir, name);
    if (add_to) {
        struct job j = { 0 };
        absolute(add_to, path, sizeof path);
        as_user(1);
        int there = stat(path, &st) == 0;
        as_user(0);
        if (there) {
            char *args[510];
            int k = 0;
            args[k++] = "add";
            args[k++] = path;
            args[k++] = dir;
            for (int i = 0; i < n; i++) args[k++] = names[i];
            args[k] = NULL;
            snprintf(arc.path, sizeof arc.path, "%s", path);
            run_worker(&j, _("Compressing..."), args, NULL);
            if (j.status != WORKER_DONE && j.status != WORKER_CANCELLED) show_problems(&j, _("Nothing was added."));
            return job_exit(j.status);
        }
        dir_of(path, c.folder, sizeof c.folder);
        cut_fmt(c.name, sizeof c.name, "%s", base_name(path));
        c.type = -1;
        for (int t = 0; archive_types[t].suffix && c.type < 0; t++) {
            if (archive_ends_with(c.name, archive_types[t].suffix)) c.type = t;
        }
        if (c.type < 0) {
            snprintf(j.error, sizeof j.error, _("%s: not a kind of archive that can be written."), c.name);
            show_problems(&j, _("Nothing was compressed."));
            return 1;
        }
        c.level = ctype(&c)->ldef;
    } else if (suffix) {
        char want[NAME_MAX + 1], unique[NAME_MAX + 1];
        c.type = -1;
        for (int t = 0; archive_types[t].suffix && c.type < 0; t++) {
            if (strcmp(archive_types[t].suffix + 1, suffix) == 0) c.type = t;
        }
        if (c.type < 0) return 1;
        c.level = ctype(&c)->ldef;
        cut_fmt(want, sizeof want, "%s.%s", name, suffix);
        as_user(1);
        int ok = archive_suggest_name(dir, want, unique, sizeof unique) == 0;
        as_user(0);
        if (!ok) return 1;
        snprintf(c.name, sizeof c.name, "%s", unique);
    } else if (dialog && !creation_dialog(&c, _("Compress"))) {
        return 2;
    }
    int st_ = create_with(&c, dir, names, n, made, sizeof made);
    explicit_bzero(c.password, sizeof c.password);
    return job_exit(st_);
}

static void remove_previews(void)
{
    for (int i = 1; i <= preview_seq; i++) {
        struct job j = { .quiet = 1 };
        char dir[PATH_MAX];
        cut_fmt(dir, sizeof dir, "%s/%s/%d-%d", user_home, ARCHIVE_PREVIEW_DIR, (int)getpid(), i);
        char *args[] = { "remove", dir, NULL };
        run_worker(&j, "", args, NULL);
    }
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "batch", no_argument, NULL, 'b' },
        { "autodestination", no_argument, NULL, 'e' },
        { "autosubfolder", no_argument, NULL, 'a' },
        { "destination", required_argument, NULL, 'o' },
        { "dialog", no_argument, NULL, 'd' },
        { "add", no_argument, NULL, 'c' },
        { "add-to", required_argument, NULL, 't' },
        { "changetofirstpath", no_argument, NULL, 'p' },
        { "autofilename", required_argument, NULL, 'f' },
        { NULL, 0, NULL, 0 },
    };
    int is_batch = 0, autosub = 0, dialog = 0, add = 0, c, status = 0;
    const char *dest = NULL, *add_to = NULL, *suffix = NULL;
    while ((c = getopt_long(argc, argv, "beao:dct:pf:", opts, NULL)) != -1) {
        if (c == 'b') is_batch = 1;
        else if (c == 'a') autosub = 1;
        else if (c == 'o') dest = optarg;
        else if (c == 'd') dialog = 1;
        else if (c == 'c') add = 1;
        else if (c == 't') add_to = optarg;
        else if (c == 'f') suffix = optarg;
        else if (c != 'e' && c != 'p') return 1;
    }
    struct passwd *pw = getpwuid(geteuid() == 0 ? ARCHIVE_USER : geteuid());
    if (!pw || !pw->pw_dir) return 1;
    user_uid = pw->pw_uid;
    user_gid = pw->pw_gid;
    snprintf(user_home, sizeof user_home, "%s", pw->pw_dir);
    screen_open(BASE_FONT);
    keys_open();
    if (add || add_to || suffix) {
        status = batch_add(argv + optind, argc - optind, add_to, add, suffix);
    } else if (is_batch) {
        status = batch_extract(argv + optind, argc - optind, dialog, autosub, dest);
    } else {
        start_tree();
        if (optind < argc) {
            char path[PATH_MAX];
            absolute(argv[optind], path, sizeof path);
            load_archive(path);
        } else {
            open_other();
            if (!arc.path[0]) return 0; /* nothing chosen: back */
        }
        while (!quit) {
            draw();
            int key = key_get();
            if (key == K_POINTER) pointer();
            else if (key != K_SHOWN) handle(key | MODS(keys_modifiers()));
        }
    }
    remove_previews();
    explicit_bzero(arc.password, sizeof arc.password);
    return status;
}
