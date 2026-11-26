/*
 * Opening files and addresses: which container opens what (base.h).
 *
 * A target is an absolute path or a URL. Its type is a MIME type,
 * inode/directory for a folder, or x-scheme-handler/SCHEME for a URL.
 * The containers that can open a type are those whose installed .desktop
 * files list it, or its major type with a * (image/ then *), in MimeType.
 * OPEN_RULES holds the user's choices, TYPE=CONTAINER or TYPE=ask, a line
 * per type (a major type with * too); settings and the choosers write it.
 *
 * Only files in the shared user directories (rc.conf's user_dirs, in the
 * home of the user, uid 1000) are in every container, at the same path.
 *
 * Types are found and related as GIO does it (glib's content types, over
 * shared-mime-info's database): a file's type from its name, else its first
 * bytes; a type opened by what opens a type it is a kind of.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <mntent.h>
#include <limits.h>

#include "base.h"
#include "config.h"

#ifndef OPEN_CONTAINERS
#define OPEN_CONTAINERS "/var/lib/locachy/containers" /* their state: locachy-app's CONT_BASE */
#endif

static int valid_name(const char *name)
{
    if (!isalnum((unsigned char)name[0]) || strlen(name) >= OPEN_NAME || strncmp(name, "window-", 7) == 0) {
        return 0;
    }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.') {
            return 0;
        }
    }
    return 1;
}

static void user_dirs_setting(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "user_dirs") == 0) {
        snprintf(arg, 256, "%s", value);
    }
}

void open_user_dirs(char *home, size_t hsize, char *dirs, size_t dsize)
{
    struct passwd *pw = getpwuid(1000);
    char list[256] = "Documents Downloads Music Pictures Videos";
    snprintf(home, hsize, "%s", pw && pw->pw_dir ? pw->pw_dir : "");
    rc_read(user_dirs_setting, list);
    snprintf(dirs, dsize, "%s Drives", list); /* USB drives (drives.c), shared too */
}

int open_shared(const char *path, char *why, size_t size)
{
    char home[256], dirs[256], list[256], real[4096];
    open_user_dirs(home, sizeof home, dirs, sizeof dirs);
    snprintf(list, sizeof list, "%s", dirs);
    if (!home[0] || realpath(path, real) == NULL) {
        snprintf(why, size, "%s does not exist", path);
        return 0;
    }
    size_t hlen = strlen(home);
    if (strncmp(real, home, hlen) == 0 && real[hlen] == '/') {
        const char *rest = real + hlen + 1;
        for (char *d = strtok(dirs, " "); d; d = strtok(NULL, " ")) {
            size_t dlen = strlen(d);
            if (strncmp(rest, d, dlen) == 0 && (rest[dlen] == '/' || rest[dlen] == '\0')) {
                return 1;
            }
        }
    }
    snprintf(why, size, "%s is not in a shared folder of %s (%s)", path,
             home[0] ? home : "the user", list);
    return 0;
}

/* file:///a%20b -> /a b */
static int file_url_path(const char *url, char *path, size_t size)
{
    const char *s = url + 7;
    size_t n = 0;
    if (strncmp(s, "localhost/", 10) == 0) {
        s += 9;
    }
    if (*s != '/') {
        return -1;
    }
    for (; *s && n + 1 < size; s++) {
        if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            unsigned long v = strtoul(hex, NULL, 16);
            if (v == 0) {
                return -1;
            }
            path[n++] = (char)v;
            s += 2;
        } else {
            path[n++] = *s;
        }
    }
    path[n] = '\0';
    return 0;
}

int open_normalize(const char *target, char *out, size_t size)
{
    int r = -1;
    if (strncmp(target, "file://", 7) == 0) {
        r = file_url_path(target, out, size);
    } else if (target[0] == '/' || isalpha((unsigned char)target[0])) {
        snprintf(out, size, "%s", target);
        r = 0;
    }
    return r == 0 && !strpbrk(out, "\r\n") ? 0 : -1;
}

static int is_url(const char *target)
{
    const char *s = target;
    if (!isalpha((unsigned char)*s)) {
        return 0;
    }
    while (isalnum((unsigned char)*s) || *s == '+' || *s == '-' || *s == '.') {
        s++;
    }
    return *s == ':';
}

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

struct dir_stamp {
    int exists;
    dev_t dev;
    ino_t ino;
    time_t mtime_sec;
    long mtime_nsec;
    time_t ctime_sec;
    long ctime_nsec;
};

static struct dir_stamp read_stamp(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return (struct dir_stamp){ 0 };
    }
    return (struct dir_stamp){
        1, st.st_dev, st.st_ino,
        st.st_mtim.tv_sec, st.st_mtim.tv_nsec,
        st.st_ctim.tv_sec, st.st_ctim.tv_nsec
    };
}

static int stamp_same(const struct dir_stamp *a, const struct dir_stamp *b)
{
    if (a->exists != b->exists) return 0;
    if (!a->exists) return 1;
    return a->dev == b->dev && a->ino == b->ino &&
           a->mtime_sec == b->mtime_sec && a->mtime_nsec == b->mtime_nsec &&
           a->ctime_sec == b->ctime_sec && a->ctime_nsec == b->ctime_nsec;
}

/* A stamp taken within the last two seconds cannot show a later change made in
 * the same clock tick (file times move in jiffies), so it keeps no answer. */
static int stamp_racy(const struct dir_stamp *s)
{
    time_t now = time(NULL);
    return s->exists && (s->mtime_sec >= now - 2 || s->ctime_sec >= now - 2);
}

/* Cached mount table for slow-mount detection, refreshed at most every 5 s or
 * when /proc/self/mounts signals a mount-table change via POLLERR/POLLPRI. */
struct mount_entry {
    char *dir;
    size_t len;
    int slow;
};
static struct {
    struct mount_entry *entries;
    size_t n, cap;
    double loaded_at;
    int loaded;
    int watch_fd;
} mount_cache = { .watch_fd = -1 };

static int mount_table_changed(void)
{
    if (mount_cache.watch_fd < 0) {
        mount_cache.watch_fd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
        return 0;
    }
    struct pollfd p = { .fd = mount_cache.watch_fd, .events = POLLPRI | POLLERR };
    return poll(&p, 1, 0) > 0 && (p.revents & (POLLPRI | POLLERR)) != 0;
}

static void mount_cache_refresh(double now)
{
    if (mount_cache.watch_fd >= 0) {
        /* Linux /proc/self/mounts POLLPRI|POLLERR stays latched until rewound/reopened. */
        close(mount_cache.watch_fd);
    }
    mount_cache.watch_fd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
    for (size_t i = 0; i < mount_cache.n; i++) {
        free(mount_cache.entries[i].dir);
    }
    mount_cache.n = 0;
    char buffer[8192];
    FILE *mounts = setmntent("/proc/self/mounts", "re");
    struct mntent entry, *mount;
    static const char *const network[] = { "nfs", "nfs4", "cifs", "smb3", "smbfs", "ceph", "9p", "autofs" };
    while (mounts && (mount = getmntent_r(mounts, &entry, buffer, sizeof buffer)) != NULL) {
        int slow = strncmp(mount->mnt_type, "fuse.", 5) == 0 && strncmp(mount->mnt_fsname, "/dev/", 5) != 0;
        for (size_t i = 0; !slow && i < sizeof network / sizeof network[0]; i++) {
            slow = strcmp(mount->mnt_type, network[i]) == 0;
        }
        if (mount_cache.n == mount_cache.cap) {
            size_t cap = mount_cache.cap ? mount_cache.cap * 2 : 32;
            struct mount_entry *more = realloc(mount_cache.entries, cap * sizeof *more);
            if (!more) break;
            mount_cache.entries = more;
            mount_cache.cap = cap;
        }
        char *dir = strdup(mount->mnt_dir);
        if (!dir) break;
        mount_cache.entries[mount_cache.n++] = (struct mount_entry){ dir, strlen(dir), slow };
    }
    if (mounts) endmntent(mounts);
    mount_cache.loaded = 1;
    mount_cache.loaded_at = now;
}

static int path_is_slow_mount(const char *path)
{
    double now = mono_now();
    if (!mount_cache.loaded || now - mount_cache.loaded_at >= 5.0 || mount_table_changed()) {
        mount_cache_refresh(now);
    }
    int slow = 0;
    size_t longest = 0;
    char resolved[PATH_MAX];
    const char *target = realpath(path, resolved) ? resolved : path;
    for (size_t i = 0; i < mount_cache.n; i++) {
        const struct mount_entry *m = &mount_cache.entries[i];
        if (m->len < longest || strncmp(target, m->dir, m->len) != 0 ||
            (m->len != 1 && target[m->len] && target[m->len] != '/')) {
            continue;
        }
        longest = m->len;
        slow = m->slow;
    }
    return slow;
}

/* The type GIO gives a local file (g_file_query_info's content type): its
 * name's, by the globs, unless that is none or more than one; then its first
 * bytes', by the magic (shared-mime-info's "Recommended checking order"). */
static int file_type(const char *path, const struct stat *st, char *type, size_t size)
{
    const char *kind = S_ISDIR(st->st_mode) ? "inode/directory" : S_ISFIFO(st->st_mode) ? "inode/fifo" :
                       S_ISCHR(st->st_mode) ? "inode/chardevice" : S_ISBLK(st->st_mode) ? "inode/blockdevice" :
                       S_ISSOCK(st->st_mode) ? "inode/socket" : NULL;
    if (kind) {
        snprintf(type, size, "%s", kind);
        return 0;
    }
    /* Fast path: check filename globs first. When GIO is certain (the vast
     * majority of files with extensions), skip mount checks and content reads. */
    gboolean uncertain = TRUE;
    gchar *guess = g_content_type_guess(path, NULL, 0, &uncertain);
    if (!uncertain && guess && strchr(guess, '/')) {
        int ok = snprintf(type, size, "%s", guess) < (int)size;
        g_free(guess);
        return ok ? 0 : -1;
    }
    g_free(guess);
    /* As KIO's slow mounts: drawing a type must not download every cloud
     * file. Match the actual enclosing mount, not a folder named Drives. */
    int slow = path_is_slow_mount(path);
    guchar data[4096]; /* GIO's sniffing length for local files */
    int fd = slow ? -1 : open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    ssize_t n = fd >= 0 ? read(fd, data, sizeof data) : -1;
    if (fd >= 0) {
        close(fd);
    }
    guess = g_content_type_guess(path, data, n > 0 ? (gsize)n : 0, NULL);
    int ok = guess && strchr(guess, '/') && snprintf(type, size, "%s", guess) < (int)size;
    g_free(guess);
    return ok ? 0 : -1;
}

int open_type(const char *target, char *type, size_t size)
{
    struct stat st;
    if (is_url(target)) {
        snprintf(type, size, "x-scheme-handler/%.*s", (int)strcspn(target, ":"), target);
        for (char *p = type; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        return 0;
    }
    if (target[0] != '/' || stat(target, &st) != 0) {
        return -1;
    }
    return file_type(target, &st, type, size);
}

/* Does the MimeType line (a;b;c;) list type, a type it is a kind of (GIO's
 * g_content_type_is_a: aliases, subclasses, a major type with a star), or a
 * URL's scheme? */
static int lists(const char *mimetypes, const char *type)
{
    int scheme = strncmp(type, "x-scheme-handler/", 17) == 0, found = 0;
    gchar **entries = g_strsplit(mimetypes, ";", -1);
    for (gchar **e = entries; *e && !found; e++) {
        found = **e && (scheme ? strcmp(*e, type) == 0 : g_content_type_is_a(type, *e));
    }
    g_strfreev(entries);
    return found;
}

struct program_conf {
    int host;
    int vm;
    char desktop[256], command[256];
    char vm_kind[32], vm_name[OPEN_NAME];
};

static void on_program(const char *key, const char *value, void *arg)
{
    struct program_conf *c = arg;
    if (strcmp(key, "host") == 0) {
        c->host = strcmp(value, "1") == 0;
    } else if (strcmp(key, "vm") == 0) {
        c->vm = strcmp(value, "1") == 0;
    } else if (strcmp(key, "desktop") == 0) {
        snprintf(c->desktop, sizeof c->desktop, "%s", value);
    } else if (strcmp(key, "command") == 0) {
        snprintf(c->command, sizeof c->command, "%s", value);
    } else if (strcmp(key, "vm_kind") == 0) {
        snprintf(c->vm_kind, sizeof c->vm_kind, "%s", value);
    } else if (strcmp(key, "vm_name") == 0) {
        snprintf(c->vm_name, sizeof c->vm_name, "%s", value);
    }
}

static void program_conf(const char *name, struct program_conf *c)
{
    char path[256];
    memset(c, 0, sizeof *c);
    snprintf(path, sizeof path, CONTAINERS_CONF "/%s.conf", name);
    conf_read(path, on_program, c);
}

int open_host(const char *name)
{
    struct program_conf c;
    if (!valid_name(name)) {
        return 0;
    }
    program_conf(name, &c);
    return c.host;
}

int open_vm(const char *name)
{
    struct program_conf c;
    if (!valid_name(name)) {
        return 0;
    }
    program_conf(name, &c);
    return c.vm;
}

/* A framebuffer program's name without "program:", or NULL if name is not
 * one's (program_desktop and program_files are not home's programs). */
static const char *program_name(const char *name)
{
    size_t len = strlen(OPEN_PROGRAM);
    const char *p = name + len;
    return strncmp(name, OPEN_PROGRAM, len) == 0 && valid_name(p) &&
           strcmp(p, "desktop") != 0 && strcmp(p, "files") != 0 ? p : NULL;
}

struct command {
    const char *name; /* program_NAME's */
    char *spec;
    size_t size;
    int found;
};

static void on_command(const char *key, const char *value, void *arg)
{
    struct command *c = arg;
    if (strncmp(key, "program_", 8) == 0 && strcmp(key + 8, c->name) == 0 && value[0]) {
        snprintf(c->spec, c->size, "%s", value);
        c->found = 1;
    }
}

int open_program(const char *name, const char *target, char *spec, size_t size, char *argv[], int max)
{
    struct command c = { program_name(name), spec, size, 0 };
    int argc = 0;
    if (c.name == NULL || max < 3) {
        return 0;
    }
    rc_read(on_command, &c);
    for (char *w = c.found ? strtok(spec, " \t") : NULL; w && argc < max - 2; w = strtok(NULL, " \t")) {
        argv[argc++] = w;
    }
    if (argc == 0) {
        return 0;
    }
    argv[argc++] = (char *)target;
    argv[argc] = NULL;
    return argc;
}

/* The .desktop files of name: a host program's, those its desktop= names in
 * /usr/share/applications (not all of them: they are every program's); a
 * container's, those in its upper (what its own packages added); a
 * framebuffer program's, its own in OPEN_PROGRAM_DESKTOP. */
static int desktop_files(const char *name, char paths[][512], int max)
{
    struct program_conf c;
    int n = 0;
    if (program_name(name)) {
        snprintf(paths[0], 512, OPEN_PROGRAM_DESKTOP "/%s.desktop", program_name(name));
        return max > 0;
    }
    program_conf(name, &c);
    if (c.host) {
        char *save = NULL;
        for (char *f = strtok_r(c.desktop, " ", &save); f && n < max; f = strtok_r(NULL, " ", &save)) {
            size_t len = strlen(f);
            if (len > 8 && !strchr(f, '/') && strcmp(f + len - 8, ".desktop") == 0) {
                snprintf(paths[n++], 512, "/usr/share/applications/%s", f);
            }
        }
        return n;
    }
    char dir_path[256];
    snprintf(dir_path, sizeof dir_path, OPEN_CONTAINERS "/%s/upper/usr/share/applications", name);
    DIR *dir = opendir(dir_path);
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL && n < max) {
        size_t len = strlen(e->d_name);
        if (len > 8 && strcmp(e->d_name + len - 8, ".desktop") == 0) {
            snprintf(paths[n++], 512, "%s/%s", dir_path, e->d_name);
        }
    }
    if (dir) {
        closedir(dir);
    }
    return n;
}

/* In-memory cache for desktop_entry() and container listings: invalidated on
 * open_rules(), after 5 s TTL, or whenever a watched directory/file stamp changes. */
#define OPEN_LABEL_CACHE_SIZE 64
#define OPEN_DEP_MAX 40 /* the paths one cached lookup depends on, at most */
struct cache_dep {
    char path[512];
    struct dir_stamp st;
};
struct label_cache_entry {
    int used, has_type, found;
    char name[OPEN_NAME];
    char type[OPEN_TYPE];
    char label[128];
    int ndeps;
    struct cache_dep *deps; /* what the answer was read from, stamped */
};
static struct {
    struct label_cache_entry entries[OPEN_LABEL_CACHE_SIZE];
    unsigned next_slot;
    double valid_since;
    int valid;
    struct dir_stamp conf_dir_stamp;
    struct dir_stamp apps_dir_stamp;
    struct dir_stamp prog_dir_stamp;
    char (*containers)[OPEN_NAME];
    int ncontainers;
    int containers_valid;
} open_cache;

static void label_entry_drop(struct label_cache_entry *e)
{
    free(e->deps);
    e->deps = NULL;
    e->ndeps = 0;
    e->used = 0;
}

static void open_cache_invalidate(void)
{
    for (int i = 0; i < OPEN_LABEL_CACHE_SIZE; i++) {
        label_entry_drop(&open_cache.entries[i]);
    }
    open_cache.next_slot = 0;
    open_cache.valid = 0;
    open_cache.containers_valid = 0;
}

static void open_cache_check(void)
{
    double now = mono_now();
    struct dir_stamp conf_st = read_stamp(CONTAINERS_CONF);
    struct dir_stamp apps_st = read_stamp("/usr/share/applications");
    struct dir_stamp prog_st = read_stamp(OPEN_PROGRAM_DESKTOP);
    if (!open_cache.valid || now - open_cache.valid_since >= 5.0 ||
        !stamp_same(&conf_st, &open_cache.conf_dir_stamp) ||
        !stamp_same(&apps_st, &open_cache.apps_dir_stamp) ||
        !stamp_same(&prog_st, &open_cache.prog_dir_stamp)) {
        open_cache_invalidate();
        open_cache.conf_dir_stamp = conf_st;
        open_cache.apps_dir_stamp = apps_st;
        open_cache.prog_dir_stamp = prog_st;
        open_cache.valid_since = now;
        open_cache.valid = !(stamp_racy(&conf_st) || stamp_racy(&apps_st) || stamp_racy(&prog_st));
    }
}

/* The Name of the first .desktop file of name that lists type (any .desktop
 * file if type is NULL); 1 if there is one. paths: the files listed (npaths). */
static int desktop_entry_uncached(const char *name, const char *type, char *label, size_t size,
                                  char paths[][512], int *npaths)
{
    int n = desktop_files(name, paths, 32), found = 0;
    *npaths = n;
    for (int i = 0; i < n && !found; i++) {
        char line[2048], entry_name[128] = "";
        int in_entry = 0, matches = type == NULL;
        FILE *in = fopen(paths[i], "re");
        while (in && fgets(line, sizeof line, in)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '[') {
                in_entry = strcmp(line, "[Desktop Entry]") == 0;
            } else if (in_entry && strncmp(line, "Name=", 5) == 0 && !entry_name[0]) {
                snprintf(entry_name, sizeof entry_name, "%.127s", line + 5);
            } else if (in_entry && type && strncmp(line, "MimeType=", 9) == 0) {
                matches = lists(line + 9, type);
            }
        }
        if (in) {
            fclose(in);
            if (matches) {
                snprintf(label, size, "%s", entry_name[0] ? entry_name : name);
                found = 1;
            }
        }
    }
    return found;
}

static int open_label_cache_lookup(const char *name, const char *type, char *label, size_t size, int *out_found)
{
    open_cache_check();
    int has_type = type != NULL;
    if (strlen(name) >= OPEN_NAME || (has_type && strlen(type) >= OPEN_TYPE)) {
        return 0;
    }
    for (int i = 0; i < OPEN_LABEL_CACHE_SIZE; i++) {
        struct label_cache_entry *e = &open_cache.entries[i];
        if (!e->used || e->has_type != has_type || strcmp(e->name, name) != 0) continue;
        if (has_type && strcmp(e->type, type) != 0) continue;
        /* Every path the answer came from must still have the stamp it had:
         * a file added, removed or changed (a fresh fixture included) misses. */
        for (int k = 0; k < e->ndeps; k++) {
            struct dir_stamp cur = read_stamp(e->deps[k].path);
            if (!stamp_same(&cur, &e->deps[k].st)) {
                label_entry_drop(e);
                return 0;
            }
        }
        if (e->found) snprintf(label, size, "%s", e->label);
        *out_found = e->found;
        return 1;
    }
    return 0;
}

static void open_label_cache_store(const char *name, const char *type, int found, const char *label,
                                   char paths[][512], int npaths)
{
    if (strlen(name) >= OPEN_NAME || (type && strlen(type) >= OPEN_TYPE) || 3 + npaths > OPEN_DEP_MAX) {
        return;
    }
    struct cache_dep *deps = malloc((size_t)(3 + npaths) * sizeof *deps);
    if (!deps) return;
    int k = 0;
    /* The containers' list, and for a container its own configuration (the
     * host flag and its desktop= list) and the folder its packages add to. */
    snprintf(deps[k++].path, sizeof deps[0].path, "%s", CONTAINERS_CONF);
    if (program_name(name)) {
        snprintf(deps[k++].path, sizeof deps[0].path, "%s", OPEN_PROGRAM_DESKTOP);
    } else {
        snprintf(deps[k++].path, sizeof deps[0].path, CONTAINERS_CONF "/%s.conf", name);
        snprintf(deps[k++].path, sizeof deps[0].path, OPEN_CONTAINERS "/%s/upper/usr/share/applications", name);
    }
    for (int i = 0; i < npaths; i++) {
        size_t plen = strlen(paths[i]);
        if (plen >= sizeof deps[0].path) {
            free(deps);
            return;
        }
        memcpy(deps[k++].path, paths[i], plen + 1);
    }
    for (int i = 0; i < k; i++) {
        deps[i].st = read_stamp(deps[i].path);
        if (stamp_racy(&deps[i].st)) {
            free(deps);
            return;
        }
    }
    struct label_cache_entry *slot = &open_cache.entries[open_cache.next_slot % OPEN_LABEL_CACHE_SIZE];
    open_cache.next_slot++;
    label_entry_drop(slot);
    slot->deps = deps;
    slot->ndeps = k;
    slot->has_type = type != NULL;
    snprintf(slot->name, sizeof slot->name, "%s", name);
    snprintf(slot->type, sizeof slot->type, "%s", type ? type : "");
    slot->found = found;
    snprintf(slot->label, sizeof slot->label, "%s", found ? label : "");
    slot->used = 1;
}

static int desktop_entry(const char *name, const char *type, char *label, size_t size)
{
    int cached_found = 0;
    if (open_label_cache_lookup(name, type, label, size, &cached_found)) {
        return cached_found;
    }
    char out[128] = "", paths[32][512];
    int npaths = 0;
    int found = desktop_entry_uncached(name, type, out, sizeof out, paths, &npaths);
    if (found) snprintf(label, size, "%s", out);
    open_label_cache_store(name, type, found, out, paths, npaths);
    return found;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(a, b);
}

static void refresh_container_list(void)
{
    open_cache_check();
    if (open_cache.containers_valid) return;
    DIR *dir = opendir(CONTAINERS_CONF);
    struct dirent *e;
    int n = 0, cap = 0;
    char (*list)[OPEN_NAME] = NULL;
    while (dir && (e = readdir(dir)) != NULL) {
        char name[OPEN_NAME];
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= OPEN_NAME || strcmp(e->d_name + len - 5, ".conf") != 0) {
            continue;
        }
        snprintf(name, sizeof name, "%.*s", (int)(len - 5), e->d_name);
        if (!valid_name(name)) {
            continue;
        }
        if (n == cap) {
            int ncap = cap ? cap * 2 : 16;
            char (*more)[OPEN_NAME] = realloc(list, (size_t)ncap * sizeof *more);
            if (!more) break;
            list = more;
            cap = ncap;
        }
        memcpy(list[n++], name, OPEN_NAME);
    }
    if (dir) {
        closedir(dir);
    }
    /* A stable order: by name. */
    if (n > 1) qsort(list, (size_t)n, OPEN_NAME, by_name);
    free(open_cache.containers);
    open_cache.containers = list;
    open_cache.ncontainers = n;
    open_cache.containers_valid = !stamp_racy(&open_cache.conf_dir_stamp);
}

/* The containers there are (CONTAINERS_CONF/NAME.conf), those that open
 * type if it is not NULL. */
static int each_container(const char *type, char names[][OPEN_NAME], int max)
{
    refresh_container_list();
    int n = 0;
    for (int i = 0; i < open_cache.ncontainers && n < max; i++) {
        char label[128];
        if (type == NULL || desktop_entry(open_cache.containers[i], type, label, sizeof label)) {
            memcpy(names[n++], open_cache.containers[i], OPEN_NAME);
        }
    }
    return n;
}

/* The framebuffer programs (rc.conf program_NAME) whose .desktop file lists
 * type, as "program:NAME". */
struct programs {
    const char *type;
    char (*names)[OPEN_NAME];
    int n, max;
};

static void on_program_setting(const char *key, const char *value, void *arg)
{
    struct programs *p = arg;
    char name[OPEN_NAME], label[128];
    (void)value;
    if (strncmp(key, "program_", 8) != 0 || p->n == p->max ||
        snprintf(name, sizeof name, OPEN_PROGRAM "%s", key + 8) >= (int)sizeof name ||
        !program_name(name) || !desktop_entry(name, p->type, label, sizeof label)) {
        return;
    }
    for (int i = 0; i < p->n; i++) {
        if (strcmp(p->names[i], name) == 0) {
            return; /* a setting given twice */
        }
    }
    memcpy(p->names[p->n++], name, OPEN_NAME);
}

int open_candidates(const char *type, char names[][OPEN_NAME], int max)
{
    struct programs p = { type, names, 0, max };
    rc_read(on_program_setting, &p);
    return p.n < max ? p.n + each_container(type, names + p.n, max - p.n) : p.n;
}

int open_containers(char names[][OPEN_NAME], int max)
{
    return each_container(NULL, names, max);
}

int open_window_ids(const char *name, char ids[][OPEN_WINDOW_ID], int max)
{
    char paths[32][512];
    if (!valid_name(name)) {
        return 0;
    }
    int files = desktop_files(name, paths, 32), n = 0;
    for (int i = 0; i < files && n < max; i++) {
        FILE *in = fopen(paths[i], "re");
        if (!in) {
            continue;
        }
        const char *slash = strrchr(paths[i], '/'), *base = slash ? slash + 1 : paths[i];
        snprintf(ids[n++], OPEN_WINDOW_ID, "%.*s", (int)(strlen(base) - 8), base);
        char line[256];
        while (fgets(line, sizeof line, in) && n < max) {
            if (strncmp(line, "StartupWMClass=", 15) == 0) {
                line[strcspn(line, "\r\n")] = '\0';
                snprintf(ids[n++], OPEN_WINDOW_ID, "%.127s", line + 15);
                break;
            }
        }
        fclose(in);
    }
    /* Its command's name: the app id GTK and Qt give a window when the
     * program sets none (g_get_prgname, QCoreApplication::applicationName:
     * argv[0]'s base name), as its dialogs have. */
    struct program_conf c;
    const char *slash;
    if (!program_name(name) && n < max) {
        program_conf(name, &c);
        slash = strrchr(c.command, '/');
        if (slash && slash[1]) {
            int seen = 0;
            for (int i = 0; i < n && !seen; i++) {
                seen = strcmp(ids[i], slash + 1) == 0;
            }
            if (!seen) {
                snprintf(ids[n++], OPEN_WINDOW_ID, "%.127s", slash + 1);
            }
        }
    }
    return n;
}

/* A QEMU window's app id starts with "qemu": GTK's is "qemu"; SDL's falls back
 * to the executable's name (qemu-system-x86_64) when no hint or variable sets
 * one, and QEMU's SDL code sets none (qemu/ui/sdl2.c). Only the prefix counts. */
int open_window_qemu(const char *app_id)
{
    return app_id && strncmp(app_id, "qemu", 4) == 0;
}

/* The prefixes of the -name each managed VM launcher gives QEMU, as they follow
 * "QEMU (" in the window title: locachy-app's custom VMs "locachy-vm[NAME]",
 * locachy-compute's "locachy-compute[VMNAME]", locachy-winmaint's "locachy-winmaint". */
static const char *const vm_tag_prefixes[] = { "locachy-vm[", "locachy-compute[", "locachy-winmaint" };

/* 1: a QEMU window of a managed VM, by its app id and title (a VM's tag). Such
 * a window is that VM's alone; no app id claims it. */
int open_window_vm_tagged(const char *app_id, const char *title)
{
    if (!open_window_qemu(app_id) || !title || strncmp(title, "QEMU (", 6) != 0) {
        return 0;
    }
    for (size_t i = 0; i < sizeof vm_tag_prefixes / sizeof *vm_tag_prefixes; i++) {
        if (strncmp(title + 6, vm_tag_prefixes[i], strlen(vm_tag_prefixes[i])) == 0) {
            return 1;
        }
    }
    return 0;
}

/* VM app name's tag (its launcher's -name), or 0 if it has none: a compute
 * VM's name (vm_name, else the app's) as locachy-compute accepts names. */
static int vm_tag(const char *name, const struct program_conf *c, char *tag, size_t size)
{
    if (strcmp(c->vm_kind, "winmaint") == 0) {
        return snprintf(tag, size, "locachy-winmaint") < (int)size;
    }
    if (strcmp(c->vm_kind, "compute") != 0) {
        return snprintf(tag, size, "locachy-vm[%s]", name) < (int)size;
    }
    const char *vname = c->vm_name[0] ? c->vm_name : name;
    for (const char *p = vname; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.') {
            return 0;
        }
    }
    return snprintf(tag, size, "locachy-compute[%s]", vname) < (int)size;
}

/* Does QEMU's window title carry exactly tag: GTK's "QEMU (TAG)", maybe
 * followed by " [Paused]" or ": LABEL", or SDL's "QEMU (TAG-N)..." (N its
 * console, qemu/ui/sdl2.c)? The closing bracket ends a name, so one VM's tag
 * is never a prefix of another's (debian, debian-12). */
static int vm_title_has_tag(const char *title, const char *tag)
{
    size_t len = strlen(tag);
    if (strncmp(title, "QEMU (", 6) != 0 || strncmp(title + 6, tag, len) != 0) {
        return 0;
    }
    const char *rest = title + 6 + len;
    if (*rest == ')') {
        return 1;
    }
    if (*rest == '-' && isdigit((unsigned char)rest[1])) {
        const char *p = rest + 1;
        while (isdigit((unsigned char)*p)) {
            p++;
        }
        return *p == ')';
    }
    return 0;
}

int open_window_matches(const char *name, const char *app_id, const char *title)
{
    if (!valid_name(name)) {
        return 0;
    }
    struct program_conf c;
    program_conf(name, &c);
    if (open_window_vm_tagged(app_id, title)) {
        /* A managed VM's window: that VM's alone, never an app id's. */
        char tag[OPEN_NAME + 32];
        return c.vm && vm_tag(name, &c, tag, sizeof tag) && vm_title_has_tag(title, tag);
    }
    /* A VM without a command has no app id of its own (QEMU's is every
     * VM's); a command= VM is matched by its command's, as containers are. */
    if ((c.vm && !c.command[0]) || !app_id || !app_id[0]) {
        return 0;
    }
    char ids[16][OPEN_WINDOW_ID];
    int n = open_window_ids(name, ids, 16);
    for (int i = 0; i < n; i++) {
        if (strcasecmp(app_id, ids[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

void open_label(const char *name, const char *type, char *label, size_t size)
{
    char entry[128];
    const char *program = program_name(name);
    if (desktop_entry(name, type, entry, sizeof entry) || desktop_entry(name, NULL, entry, sizeof entry)) {
        snprintf(label, size, "%s (%s)", program ? program : name, entry);
    } else {
        snprintf(label, size, "%s", program ? program : name);
    }
}

int open_rule(const char *type, char *name, size_t size)
{
    FILE *in = fopen(OPEN_RULES, "re");
    char line[256], major[128];
    int found = OPEN_NO_RULE;
    snprintf(major, sizeof major, "%.*s/*", (int)strcspn(type, "/"), type);
    while (in && fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) {
            continue;
        }
        *eq++ = '\0';
        /* A rule written for an alias (text/x-c) is the type's. */
        int exact = g_content_type_equals(line, type);
        if (exact || (found == OPEN_NO_RULE && strcmp(line, major) == 0)) {
            found = strcmp(eq, "ask") == 0 ? OPEN_RULE_ASK :
                    valid_name(eq) || program_name(eq) ? OPEN_RULE_CONTAINER : OPEN_NO_RULE;
            if (found == OPEN_RULE_CONTAINER) {
                snprintf(name, size, "%s", eq);
            }
            if (exact) {
                break;
            }
        }
    }
    if (in) {
        fclose(in);
    }
    return found;
}

struct rule_change { const char *type, *value; };

static int change_rule(const char *original, size_t length, int exists,
                       char **replacement, size_t *replacement_length, void *arg)
{
    (void)exists;
    const struct rule_change *change = arg;
    FILE *out = open_memstream(replacement, replacement_length);
    if (!out) return -1;
    size_t tlen = strlen(change->type), at = 0;
    int ok = 1, newline = 1;
    while (at < length) {
        size_t end = at;
        while (end < length && original[end] != '\n') end++;
        if (end < length) end++;
        if (end - at <= tlen || memcmp(original + at, change->type, tlen) || original[at + tlen] != '=') {
            if (fwrite(original + at, 1, end - at, out) != end - at) ok = 0;
            newline = original[end - 1] == '\n';
        }
        at = end;
    }
    if (change->value) {
        if ((!newline && fputc('\n', out) == EOF) ||
            fprintf(out, "%s=%s\n", change->type, change->value) < 0) ok = 0;
    }
    if (fclose(out) != 0) ok = 0;
    return ok ? 0 : -1;
}

int open_set_rule(const char *type, const char *value)
{
    if (!type || !type[0] || strpbrk(type, "=\r\n") || (value && strpbrk(value, "\r\n"))) {
        errno = EINVAL;
        return -1;
    }
    struct rule_change change = { type, value };
    return conf_transaction(OPEN_RULES, change_rule, &change);
}

static int request_spool(void)
{
    struct stat st;
    if (mkdir(OPEN_REQUEST, 0700) != 0 && errno != EEXIST) return -1;
    int fd = open(OPEN_REQUEST, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 077)) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    return fd;
}

int open_request_publish(const char *with, const char *target)
{
    char tmp[96], final[96], data[OPEN_REQUEST_BYTES];
    static unsigned serial;
    struct timespec now;
    if (!target || strchr(target, '\n') || (with && (strchr(with, '\n') || strlen(with) >= OPEN_NAME)) ||
        strlen(target) >= PATH_MAX) {
        errno = EINVAL;
        return -1;
    }
    int n = snprintf(data, sizeof data, "%s\n%s\n", with ? with : "", target);
    if (n < 0 || (size_t)n >= sizeof data) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int dir = request_spool();
    if (dir < 0) return -1;
    int queued = 0;
    DIR *d = fdopendir(dup(dir));
    struct dirent *e;
    while (d && (e = readdir(d))) queued += strncmp(e->d_name, "req-", 4) == 0;
    if (d) closedir(d);
    if (queued >= OPEN_REQUEST_MAX) {
        close(dir);
        errno = EAGAIN;
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    unsigned long long mono = (unsigned long long)now.tv_sec * 1000000000ULL + (unsigned long long)now.tv_nsec;
    unsigned salt = (unsigned)getpid() ^ (unsigned)now.tv_nsec ^ (++serial * 2654435761U);
    snprintf(tmp, sizeof tmp, ".tmp-%ld-%llu-%08x", (long)getpid(), mono, salt);
    int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        close(dir);
        return -1;
    }
    int ok = write(fd, data, (size_t)n) == n && fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    for (int tries = 0; ok && tries < 16; tries++) {
        snprintf(final, sizeof final, "req-%020llu-%ld-%08x", mono, (long)getpid(), salt + (unsigned)tries);
        if (renameat2(dir, tmp, dir, final, RENAME_NOREPLACE) == 0) break;
        if (errno == EINVAL || errno == ENOSYS) {
            if (linkat(dir, tmp, dir, final, 0) == 0) {
                unlinkat(dir, tmp, 0);
                break;
            }
        }
        if (errno != EEXIST || tries == 15) ok = 0;
    }
    if (ok && fsync(dir) != 0) ok = 0;
    int saved = errno;
    if (!ok) unlinkat(dir, tmp, 0);
    close(dir);
    errno = saved;
    return ok ? 0 : -1;
}

int open_rules(char types[][OPEN_TYPE], char values[][OPEN_NAME], int max)
{
    open_cache_invalidate();
    FILE *in = fopen(OPEN_RULES, "re");
    char line[256];
    int n = 0;
    while (in && n < max && fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line) {
            continue;
        }
        *eq++ = '\0';
        snprintf(types[n], OPEN_TYPE, "%.127s", line);
        snprintf(values[n], OPEN_NAME, "%.63s", eq);
        n++;
    }
    if (in) {
        fclose(in);
    }
    return n;
}
