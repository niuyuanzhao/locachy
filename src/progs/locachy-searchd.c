/*
 * locachy-searchd (/usr/local/lib/locachy/locachy-searchd): global search
 * indexing and query daemon (tools/SEARCH.md).
 *
 * Service in /etc/locachy/rc.conf (service_search). Runs as root at nice 19
 * and IOPRIO_CLASS_IDLE, indexing files under rc.conf search_roots (default:
 * the uid-1000 user's home directory from /etc/passwd) into a single SQLite/FTS5
 * database (/var/lib/locachy/search/index.db).
 *
 * Design highlights:
 * - Never crosses filesystem mount boundaries (st_dev change) or walks FUSE
 *   or network mounts (/proc/self/mountinfo; ~/Drives is never crawled).
 * - Skips hidden directories, .git, node_modules, .cache, .local/share/Trash.
 * - Live inotify updates up to half of /proc/sys/fs/inotify/max_user_watches;
 *   events are batched into one SQLite transaction per ~250 ms.
 * - Directories beyond the watch budget are re-checked every 10 minutes via
 *   directory mtime comparison.
 * - Full crawls advance the generation counter and sweep offline deletions.
 * - Document text extraction is delegated to /usr/local/bin/locachy-docscan
 *   (5 s timeout, 1 MiB stdout cap, 8 MiB file size cap, queue cap 512), paused
 *   when search_docs="0" or on battery below 20%.
 * - Serves Unix socket /run/locachy/search.sock (SO_PEERCRED uid 0 or 1000),
 *   streaming layer 1 (names) rows before layer 2 (content) rows.
 * - Zero idle wakeups: poll() blocks with infinite timeout when idle.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "search-index.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SEARCH_SOCK_PATH
#define SEARCH_SOCK_PATH "/run/locachy/search.sock"
#endif
#ifndef SEARCH_DB_PATH
#define SEARCH_DB_PATH "/var/lib/locachy/search/index.db"
#endif
#ifndef SEARCH_RC_CONF
#define SEARCH_RC_CONF "/etc/locachy/rc.conf"
#endif
#ifndef SEARCH_DOCSCAN_BIN
#define SEARCH_DOCSCAN_BIN "/usr/local/bin/locachy-docscan"
#endif
#ifndef SEARCH_MOUNTINFO_PATH
#define SEARCH_MOUNTINFO_PATH "/proc/self/mountinfo"
#endif
#ifndef SEARCH_MAX_WATCHES_PATH
#define SEARCH_MAX_WATCHES_PATH "/proc/sys/fs/inotify/max_user_watches"
#endif
#ifndef SEARCH_POWER_SUPPLY_DIR
#define SEARCH_POWER_SUPPLY_DIR "/sys/class/power_supply"
#endif
#ifndef SEARCH_USER_UID
#define SEARCH_USER_UID 1000
#endif

#define SEARCH_BATCH_MS          250
#define SEARCH_SWEEP_SEC         600     /* 10 min sweep for unwatched dirs */
#define SEARCH_BATTERY_POLL_SEC  60      /* re-check battery while extracting */
#define SEARCH_DOCSCAN_TIMEOUT_MS 5000   /* 5 s per file */
#define SEARCH_DOCSCAN_MAX_OUT   (1024 * 1024)
#define SEARCH_DOCSCAN_MAX_FILE  (8LL * 1024 * 1024)
#define SEARCH_DOC_QUEUE_CAP     512
#define SEARCH_ROOTS_MAX         32
#define SEARCH_CLIENTS_MAX       16
#define SEARCH_REQ_LINE_MAX      256
#define SEARCH_GHOST_TOP_N       20
#define SEARCH_CRAWL_SLICE_DIRS  16
#define SEARCH_WATCH_MIN_BUDGET  64
#define SEARCH_WATCH_DEFAULT_MAX 4096
#define SEARCH_VACUUM_PAGES      64

/* Linux ioprio constants (avoid depending on <linux/ioprio.h> availability). */
#ifndef IOPRIO_WHO_PROCESS
#define IOPRIO_WHO_PROCESS 1
#endif
#ifndef IOPRIO_CLASS_SHIFT
#define IOPRIO_CLASS_SHIFT 13
#endif
#ifndef IOPRIO_CLASS_IDLE
#define IOPRIO_CLASS_IDLE 3
#endif
#ifndef IOPRIO_PRIO_VALUE
#define IOPRIO_PRIO_VALUE(class, data) (((class) << IOPRIO_CLASS_SHIFT) | (data))
#endif

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;
static volatile sig_atomic_t g_chld = 0;

__attribute__((format(printf, 1, 2)))
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("locachy-searchd: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- Low-priority scheduling (nice 19, IOPRIO_CLASS_IDLE) --- */

static void lower_priority(void)
{
    errno = 0;
    if (setpriority(PRIO_PROCESS, 0, 19) != 0 && errno != 0) {
        say("setpriority(19) failed: %s", strerror(errno));
    }
#ifdef SYS_ioprio_set
    if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0,
                IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0)) != 0) {
        say("ioprio_set(IDLE) failed: %s", strerror(errno));
    }
#endif
}

/* --- Directory creation helper --- */

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    size_t len = strnlen(path, sizeof tmp);
    if (len == 0 || len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int ensure_parent_dir(const char *filepath, mode_t mode)
{
    char tmp[PATH_MAX];
    size_t len = strnlen(filepath, sizeof tmp);
    if (len == 0 || len >= sizeof tmp) return -1;
    memcpy(tmp, filepath, len + 1);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return 0;
    *slash = '\0';
    return mkdir_p(tmp, mode);
}

/* --- Configuration (rc.conf) --- */

struct search_config {
    char roots[SEARCH_ROOTS_MAX][PATH_MAX];
    size_t root_count;
    int search_docs; /* 1 = enabled (default), 0 = paused */
};

static int default_user_home(char *out, size_t out_size)
{
    struct passwd *pw = getpwuid((uid_t)SEARCH_USER_UID);
    if (pw && pw->pw_dir && pw->pw_dir[0] == '/') {
        if (strlen(pw->pw_dir) + 1 <= out_size) {
            snprintf(out, out_size, "%s", pw->pw_dir);
            return 0;
        }
    }
    return -1;
}

static void add_root_clean(struct search_config *cfg, const char *raw)
{
    if (!raw || raw[0] != '/' || cfg->root_count >= SEARCH_ROOTS_MAX) return;
    char buf[PATH_MAX];
    size_t len = strnlen(raw, sizeof buf);
    if (len == 0 || len >= sizeof buf) return;
    memcpy(buf, raw, len + 1);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';

    /* Deduplicate roots. */
    for (size_t i = 0; i < cfg->root_count; i++) {
        if (strcmp(cfg->roots[i], buf) == 0) return;
    }
    memcpy(cfg->roots[cfg->root_count], buf, len + 1);
    cfg->root_count++;
}

static void parse_roots_list(struct search_config *cfg, const char *val)
{
    /* Accept ':' or whitespace separated absolute paths. */
    const char *p = val;
    while (*p) {
        while (*p == ':' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ':' && *p != '\n' && *p != '\r') p++;
        size_t len = (size_t)(p - start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
        if (len > 0 && len < PATH_MAX) {
            char item[PATH_MAX];
            memcpy(item, start, len);
            item[len] = '\0';
            add_root_clean(cfg, item);
        }
    }
}

static void load_config(struct search_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->search_docs = 1;

    const char *rc_path = getenv("LOCACHY_RC_CONF");
    if (!rc_path || !rc_path[0]) rc_path = SEARCH_RC_CONF;

    char roots_val[4096] = "";
    FILE *f = fopen(rc_path, "re");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *s = line + strspn(line, " \t");
            if (*s == '\0' || *s == '#') continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = '\0';
            char *name = s;
            size_t nlen = strlen(name);
            while (nlen > 0 && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t')) {
                name[--nlen] = '\0';
            }
            char *val = eq + 1;
            if (*val == '"') {
                val++;
                char *end = strchr(val, '"');
                if (end) *end = '\0';
            } else {
                val[strcspn(val, " \t#")] = '\0';
            }
            if (strcmp(name, "search_roots") == 0) {
                snprintf(roots_val, sizeof roots_val, "%s", val);
            } else if (strcmp(name, "search_docs") == 0) {
                cfg->search_docs = (strcmp(val, "0") != 0);
            }
        }
        fclose(f);
    }

    if (roots_val[0]) {
        parse_roots_list(cfg, roots_val);
    }
    if (cfg->root_count == 0) {
        char home[PATH_MAX];
        if (default_user_home(home, sizeof home) == 0) {
            add_root_clean(cfg, home);
        }
    }
}

/* --- Mountinfo table (skip FUSE, network, and virtual mounts) --- */

struct skip_mount {
    char  path[PATH_MAX];
    dev_t dev;
    int   has_dev;
};

struct mount_table {
    struct skip_mount *items;
    size_t count;
    size_t cap;
};

static void mount_table_free(struct mount_table *mt)
{
    free(mt->items);
    mt->items = NULL;
    mt->count = 0;
    mt->cap = 0;
}

static int is_blocked_fstype(const char *fs)
{
    if (!fs || !fs[0]) return 0;
    if (strncmp(fs, "fuse", 4) == 0) return 1; /* fuse, fuse.rclone, fuse.sshfs, fuseblk, ... */
    if (strncmp(fs, "nfs", 3) == 0) return 1;  /* nfs, nfs4 */
    if (strncmp(fs, "cifs", 4) == 0) return 1;
    if (strncmp(fs, "smb", 3) == 0) return 1;  /* smb3, smbfs */
    if (strcmp(fs, "sshfs") == 0) return 1;
    if (strcmp(fs, "9p") == 0) return 1;
    if (strcmp(fs, "davfs") == 0) return 1;
    if (strcmp(fs, "afs") == 0) return 1;
    if (strcmp(fs, "ceph") == 0) return 1;
    if (strcmp(fs, "glusterfs") == 0) return 1;
    if (strcmp(fs, "autofs") == 0) return 1;
    if (strcmp(fs, "proc") == 0 || strcmp(fs, "sysfs") == 0 ||
        strcmp(fs, "devtmpfs") == 0 || strcmp(fs, "devpts") == 0 ||
        strcmp(fs, "cgroup") == 0 || strcmp(fs, "cgroup2") == 0 ||
        strcmp(fs, "debugfs") == 0 || strcmp(fs, "tracefs") == 0) {
        return 1;
    }
    return 0;
}

/* Unescape octal escapes (\040, \011, \012, \134) used in /proc/self/mountinfo. */
static void unescape_mount_path(const char *src, char *dst, size_t dst_size)
{
    size_t w = 0;
    for (size_t i = 0; src[i] && w + 1 < dst_size; i++) {
        if (src[i] == '\\' &&
            src[i + 1] >= '0' && src[i + 1] <= '3' &&
            src[i + 2] >= '0' && src[i + 2] <= '7' &&
            src[i + 3] >= '0' && src[i + 3] <= '7') {
            int val = ((src[i + 1] - '0') << 6) |
                      ((src[i + 2] - '0') << 3) |
                      (src[i + 3] - '0');
            dst[w++] = (char)val;
            i += 3;
        } else {
            dst[w++] = src[i];
        }
    }
    while (w > 1 && dst[w - 1] == '/') w--;
    dst[w] = '\0';
}

static void mount_table_reload(struct mount_table *mt)
{
    mt->count = 0;
    FILE *f = fopen(SEARCH_MOUNTINFO_PATH, "re");
    if (!f) return;

    char line[4096];
    while (fgets(line, sizeof line, f)) {
        /* Format: id parent major:minor root mount_point opts [optional...] - fstype source superopts */
        char *sep = strstr(line, " - ");
        if (!sep) continue;
        *sep = '\0';
        char *after = sep + 3;

        /* Extract fstype (first token after " - "). */
        char *fs_end = after + strcspn(after, " \t\r\n");
        *fs_end = '\0';
        if (!is_blocked_fstype(after)) continue;

        /* Extract mount_point (5th space-separated field before " - "). */
        char *p = line;
        char *fields[5];
        int ok = 1;
        for (int i = 0; i < 5; i++) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) { ok = 0; break; }
            fields[i] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        if (!ok) continue;

        char mpath[PATH_MAX];
        unescape_mount_path(fields[4], mpath, sizeof mpath);
        if (mpath[0] != '/') continue;

        if (mt->count == mt->cap) {
            size_t ncap = mt->cap ? mt->cap * 2 : 32;
            struct skip_mount *nitems = realloc(mt->items, ncap * sizeof *nitems);
            if (!nitems) break;
            mt->items = nitems;
            mt->cap = ncap;
        }
        struct skip_mount *sm = &mt->items[mt->count++];
        snprintf(sm->path, sizeof sm->path, "%s", mpath);
        sm->has_dev = 0;
        sm->dev = 0;
        /* Avoid stat() on potentially hung network/FUSE mounts: parse major:minor
         * directly from mountinfo for st_dev checks alongside path prefix checks. */
        unsigned maj = 0, min = 0;
        if (sscanf(fields[2], "%u:%u", &maj, &min) == 2) {
            sm->dev = makedev(maj, min);
            sm->has_dev = 1;
        }
    }
    fclose(f);
}

static int path_is_blocked_mount(const struct mount_table *mt, const char *path)
{
    size_t plen = strlen(path);
    for (size_t i = 0; i < mt->count; i++) {
        const char *mp = mt->items[i].path;
        size_t mlen = strlen(mp);
        if (mlen == 1 && mp[0] == '/') continue; /* never block "/" itself unless root is blocked */
        if (plen == mlen && strcmp(path, mp) == 0) return 1;
        if (plen > mlen && strncmp(path, mp, mlen) == 0 && path[mlen] == '/') return 1;
    }
    return 0;
}

static int dev_is_blocked_mount(const struct mount_table *mt, dev_t dev)
{
    for (size_t i = 0; i < mt->count; i++) {
        if (mt->items[i].has_dev && mt->items[i].dev == dev) return 1;
    }
    return 0;
}

/* --- Exclusion rules and file classification --- */

static int has_control_or_tab(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20u || *p == 0x7Fu) return 1;
    }
    return 0;
}

static int should_skip_dir_name(const char *parent_path, const char *name,
                                const struct search_config *cfg)
{
    if (!name || !name[0]) return 1;
    if (name[0] == '.') return 1; /* skips ., .., .git, .cache, .local, all hidden dirs */
    if (has_control_or_tab(name)) return 1;
    if (strcmp(name, "node_modules") == 0) return 1;
    if (strcmp(name, "__pycache__") == 0) return 1;
    /* Guard .local/share/Trash even if a caller explicitly configures .local as a root. */
    if (strcmp(name, "Trash") == 0) {
        size_t plen = strlen(parent_path);
        static const char tail[] = "/.local/share";
        size_t tlen = sizeof tail - 1;
        if (plen >= tlen && strcmp(parent_path + plen - tlen, tail) == 0) return 1;
    }
    /* Explicit guard for rclone / USB mount root ~/Drives even before a mount is active. */
    if (strcmp(name, "Drives") == 0) {
        for (size_t i = 0; i < cfg->root_count; i++) {
            if (strcmp(parent_path, cfg->roots[i]) == 0) return 1;
        }
    }
    return 0;
}

static int should_skip_file_name(const char *name)
{
    if (!name || !name[0]) return 1;
    if (name[0] == '.') return 1; /* hidden files */
    if (has_control_or_tab(name)) return 1;
    return 0;
}

static const char *file_ext_lower(const char *name, char *buf, size_t buf_size)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) return "";
    dot++;
    size_t i = 0;
    while (dot[i] && i + 1 < buf_size) {
        buf[i] = (char)tolower((unsigned char)dot[i]);
        i++;
    }
    buf[i] = '\0';
    return buf;
}

static int in_list(const char *ext, const char *const *list)
{
    if (!ext || !ext[0]) return 0;
    for (size_t i = 0; list[i]; i++) {
        if (strcmp(ext, list[i]) == 0) return 1;
    }
    return 0;
}

static void classify_file(const char *name, int64_t size, int *kind_out, int *text_state_out)
{
    static const char *const text_exts[] = {
        "txt", "md", "markdown", "rst", "org", "c", "h", "cc", "cpp", "cxx",
        "hpp", "hh", "py", "sh", "bash", "zsh", "fish", "conf", "cfg", "ini",
        "toml", "yaml", "yml", "json", "csv", "tsv", "xml", "html", "htm",
        "css", "tex", "log", "rs", "go", "js", "mjs", "cjs", "ts", "tsx",
        "jsx", "java", "kt", "kts", "php", "rb", "pl", "pm", "lua", "sql",
        "vim", "el", "lisp", "scm", "hs", "ml", "zig", "nim", "awk", "sed",
        "mk", "cmake", "diff", "patch", "desktop", "service", "nix", "svelte",
        "vue", "swift", "m", "mm", "s", "asm", NULL
    };
    static const char *const doc_exts[] = {
        "pdf", "docx", "odt", "epub", NULL
    };
    static const char *const image_exts[] = {
        "png", "jpg", "jpeg", "gif", "webp", "bmp", "svg", "ico", "tif",
        "tiff", "heif", "heic", "avif", "jxl", "raw", "dng", "cr2", "nef", NULL
    };
    static const char *const media_exts[] = {
        "mp3", "flac", "ogg", "opus", "wav", "m4a", "aac", "mp4", "mkv",
        "webm", "avi", "mov", "m4v", "mpg", "mpeg", NULL
    };
    static const char *const archive_exts[] = {
        "zip", "tar", "gz", "tgz", "bz2", "xz", "zst", "7z", "rar", "iso",
        "deb", "rpm", "pkg", NULL
    };

    char ext[24];
    file_ext_lower(name, ext, sizeof ext);

    int extractable = 0;
    int kind = SEARCH_KIND_FILE;

    if (in_list(ext, doc_exts)) {
        kind = SEARCH_KIND_DOC;
        extractable = 1;
    } else if (in_list(ext, text_exts) ||
               strcmp(name, "Makefile") == 0 ||
               strcmp(name, "Dockerfile") == 0 ||
               strcmp(name, "README") == 0 ||
               strcmp(name, "LICENSE") == 0) {
        kind = SEARCH_KIND_TEXT;
        extractable = 1;
    } else if (in_list(ext, image_exts)) {
        kind = SEARCH_KIND_IMAGE;
    } else if (in_list(ext, media_exts)) {
        kind = SEARCH_KIND_MEDIA;
    } else if (in_list(ext, archive_exts)) {
        kind = SEARCH_KIND_ARCHIVE;
    }

    *kind_out = kind;
    if (extractable && size > 0 && size <= SEARCH_DOCSCAN_MAX_FILE) {
        *text_state_out = SEARCH_TEXT_PENDING;
    } else {
        *text_state_out = SEARCH_TEXT_SKIPPED;
    }
}

/* --- Cheap battery check (/sys/class/power_supply) --- */

static int read_small_sysfs(const char *dir, const char *sub, const char *attr,
                            char *out, size_t out_size)
{
    char path[512];
    int n = snprintf(path, sizeof path, "%s/%s/%s", dir, sub, attr);
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, out_size - 1);
    close(fd);
    if (r <= 0) return -1;
    out[r] = '\0';
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

/* Returns 1 if on battery AND system battery capacity is below 20%. */
static int battery_below_20(void)
{
    const char *root = getenv("LOCACHY_POWER_SUPPLY_DIR");
    if (!root || !root[0]) root = SEARCH_POWER_SUPPLY_DIR;

    DIR *d = opendir(root);
    if (!d) return 0;

    int ac_online = 0;
    int any_bat = 0;
    int max_cap = -1;
    int discharging = 0;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char type[32];
        if (read_small_sysfs(root, e->d_name, "type", type, sizeof type) != 0) continue;

        if (strcmp(type, "Mains") == 0) {
            char online[16];
            if (read_small_sysfs(root, e->d_name, "online", online, sizeof online) == 0 &&
                strcmp(online, "1") == 0) {
                ac_online = 1;
            }
        } else if (strcmp(type, "Battery") == 0) {
            /* Ignore peripheral device batteries (mice/keyboards). */
            char scope[32];
            if (read_small_sysfs(root, e->d_name, "scope", scope, sizeof scope) == 0 &&
                strcmp(scope, "Device") == 0) {
                continue;
            }
            char present[16];
            if (read_small_sysfs(root, e->d_name, "present", present, sizeof present) == 0 &&
                strcmp(present, "0") == 0) {
                continue;
            }
            char cap_s[16];
            if (read_small_sysfs(root, e->d_name, "capacity", cap_s, sizeof cap_s) != 0) continue;
            int cap = atoi(cap_s);
            if (cap < 0 || cap > 100) continue;
            any_bat = 1;
            if (cap > max_cap) max_cap = cap;

            char status[32];
            if (read_small_sysfs(root, e->d_name, "status", status, sizeof status) == 0 &&
                strcmp(status, "Discharging") == 0) {
                discharging = 1;
            }
        }
    }
    closedir(d);

    if (!any_bat || ac_online) return 0;
    if (!discharging && max_cap >= 20) return 0;
    return max_cap < 20;
}

/* --- Watch table (inotify) and unwatched directory list --- */

struct watch_entry {
    int     wd;
    int64_t dir_id;
    dev_t   root_dev;
    char   *path;
};

struct unwatched_entry {
    int64_t dir_id;
    dev_t   root_dev;
    int64_t mtime;
    char   *path;
};

struct watch_state {
    int inotify_fd;
    size_t budget;

    struct watch_entry *watches;
    size_t watch_count;
    size_t watch_cap;

    struct unwatched_entry *unwatched;
    size_t unwatched_count;
    size_t unwatched_cap;
};

static size_t read_watch_budget(void)
{
    FILE *f = fopen(SEARCH_MAX_WATCHES_PATH, "re");
    if (!f) return SEARCH_WATCH_DEFAULT_MAX;
    char buf[64];
    size_t max_w = SEARCH_WATCH_DEFAULT_MAX;
    if (fgets(buf, sizeof buf, f)) {
        long long v = strtoll(buf, NULL, 10);
        if (v > 0) max_w = (size_t)(v / 2);
    }
    fclose(f);
    if (max_w < SEARCH_WATCH_MIN_BUDGET) max_w = SEARCH_WATCH_MIN_BUDGET;
    return max_w;
}

static void watch_state_clear(struct watch_state *ws)
{
    for (size_t i = 0; i < ws->watch_count; i++) {
        if (ws->inotify_fd >= 0 && ws->watches[i].wd >= 0) {
            inotify_rm_watch(ws->inotify_fd, ws->watches[i].wd);
        }
        free(ws->watches[i].path);
    }
    free(ws->watches);
    ws->watches = NULL;
    ws->watch_count = 0;
    ws->watch_cap = 0;

    for (size_t i = 0; i < ws->unwatched_count; i++) {
        free(ws->unwatched[i].path);
    }
    free(ws->unwatched);
    ws->unwatched = NULL;
    ws->unwatched_count = 0;
    ws->unwatched_cap = 0;
}

static struct watch_entry *watch_find_by_wd(struct watch_state *ws, int wd)
{
    for (size_t i = 0; i < ws->watch_count; i++) {
        if (ws->watches[i].wd == wd) return &ws->watches[i];
    }
    return NULL;
}

static struct watch_entry *watch_find_by_dir_id(struct watch_state *ws, int64_t dir_id)
{
    for (size_t i = 0; i < ws->watch_count; i++) {
        if (ws->watches[i].dir_id == dir_id) return &ws->watches[i];
    }
    return NULL;
}

static void unwatched_add(struct watch_state *ws, int64_t dir_id, dev_t root_dev,
                          int64_t mtime, const char *path)
{
    for (size_t i = 0; i < ws->unwatched_count; i++) {
        if (ws->unwatched[i].dir_id == dir_id) {
            ws->unwatched[i].mtime = mtime;
            if (strcmp(ws->unwatched[i].path, path) != 0) {
                char *dup = strdup(path);
                if (dup) {
                    free(ws->unwatched[i].path);
                    ws->unwatched[i].path = dup;
                }
            }
            return;
        }
    }
    if (ws->unwatched_count == ws->unwatched_cap) {
        size_t ncap = ws->unwatched_cap ? ws->unwatched_cap * 2 : 64;
        struct unwatched_entry *n = realloc(ws->unwatched, ncap * sizeof *n);
        if (!n) return;
        ws->unwatched = n;
        ws->unwatched_cap = ncap;
    }
    char *dup = strdup(path);
    if (!dup) return;
    ws->unwatched[ws->unwatched_count++] = (struct unwatched_entry){
        .dir_id = dir_id,
        .root_dev = root_dev,
        .mtime = mtime,
        .path = dup
    };
}

static void watch_add_dir(struct watch_state *ws, int64_t dir_id, dev_t root_dev,
                          int64_t mtime, const char *path)
{
    struct watch_entry *existing = watch_find_by_dir_id(ws, dir_id);
    if (existing) {
        if (strcmp(existing->path, path) != 0) {
            char *dup = strdup(path);
            if (dup) {
                free(existing->path);
                existing->path = dup;
            }
        }
        return;
    }

    if (ws->inotify_fd >= 0 && ws->watch_count < ws->budget) {
        uint32_t mask = IN_CREATE | IN_DELETE | IN_CLOSE_WRITE |
                        IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
                        IN_MOVE_SELF | IN_ATTRIB | IN_ONLYDIR | IN_DONT_FOLLOW;
        int wd = inotify_add_watch(ws->inotify_fd, path, mask);
        if (wd >= 0) {
            /* inotify_add_watch returns the same wd if path was already watched. */
            struct watch_entry *by_wd = watch_find_by_wd(ws, wd);
            if (by_wd) {
                by_wd->dir_id = dir_id;
                by_wd->root_dev = root_dev;
                if (strcmp(by_wd->path, path) != 0) {
                    char *dup = strdup(path);
                    if (dup) {
                        free(by_wd->path);
                        by_wd->path = dup;
                    }
                }
                return;
            }
            if (ws->watch_count == ws->watch_cap) {
                size_t ncap = ws->watch_cap ? ws->watch_cap * 2 : 128;
                struct watch_entry *n = realloc(ws->watches, ncap * sizeof *n);
                if (!n) {
                    inotify_rm_watch(ws->inotify_fd, wd);
                    unwatched_add(ws, dir_id, root_dev, mtime, path);
                    return;
                }
                ws->watches = n;
                ws->watch_cap = ncap;
            }
            char *dup = strdup(path);
            if (!dup) {
                inotify_rm_watch(ws->inotify_fd, wd);
                return;
            }
            ws->watches[ws->watch_count++] = (struct watch_entry){
                .wd = wd,
                .dir_id = dir_id,
                .root_dev = root_dev,
                .path = dup
            };
            return;
        }
    }

    /* Beyond budget or watch failed (ENOSPC): track for 10-minute mtime sweep. */
    unwatched_add(ws, dir_id, root_dev, mtime, path);
}

static void watch_remove_idx(struct watch_state *ws, size_t idx, int rm_kernel)
{
    if (idx >= ws->watch_count) return;
    if (rm_kernel && ws->inotify_fd >= 0 && ws->watches[idx].wd >= 0) {
        inotify_rm_watch(ws->inotify_fd, ws->watches[idx].wd);
    }
    free(ws->watches[idx].path);
    ws->watches[idx] = ws->watches[--ws->watch_count];
}

static void watch_remove_prefix(struct watch_state *ws, const char *prefix)
{
    size_t plen = strlen(prefix);
    size_t i = 0;
    while (i < ws->watch_count) {
        const char *p = ws->watches[i].path;
        if (strcmp(p, prefix) == 0 ||
            (strncmp(p, prefix, plen) == 0 && p[plen] == '/')) {
            watch_remove_idx(ws, i, 1);
            continue;
        }
        i++;
    }
    i = 0;
    while (i < ws->unwatched_count) {
        const char *p = ws->unwatched[i].path;
        if (strcmp(p, prefix) == 0 ||
            (strncmp(p, prefix, plen) == 0 && p[plen] == '/')) {
            free(ws->unwatched[i].path);
            ws->unwatched[i] = ws->unwatched[--ws->unwatched_count];
            continue;
        }
        i++;
    }
}

static void watch_rename_prefix(struct watch_state *ws, const char *old_prefix,
                                const char *new_prefix)
{
    size_t olen = strlen(old_prefix);
    size_t nlen = strlen(new_prefix);
    for (size_t i = 0; i < ws->watch_count; i++) {
        const char *p = ws->watches[i].path;
        if (strcmp(p, old_prefix) == 0) {
            char *dup = strdup(new_prefix);
            if (dup) {
                free(ws->watches[i].path);
                ws->watches[i].path = dup;
            }
        } else if (strncmp(p, old_prefix, olen) == 0 && p[olen] == '/') {
            size_t tail = strlen(p + olen);
            char *np = malloc(nlen + tail + 1);
            if (np) {
                memcpy(np, new_prefix, nlen);
                memcpy(np + nlen, p + olen, tail + 1);
                free(ws->watches[i].path);
                ws->watches[i].path = np;
            }
        }
    }
    for (size_t i = 0; i < ws->unwatched_count; i++) {
        const char *p = ws->unwatched[i].path;
        if (strcmp(p, old_prefix) == 0) {
            char *dup = strdup(new_prefix);
            if (dup) {
                free(ws->unwatched[i].path);
                ws->unwatched[i].path = dup;
            }
        } else if (strncmp(p, old_prefix, olen) == 0 && p[olen] == '/') {
            size_t tail = strlen(p + olen);
            char *np = malloc(nlen + tail + 1);
            if (np) {
                memcpy(np, new_prefix, nlen);
                memcpy(np + nlen, p + olen, tail + 1);
                free(ws->unwatched[i].path);
                ws->unwatched[i].path = np;
            }
        }
    }
}

/* --- Incremental crawl queue (keeps poll loop responsive to queries) --- */

struct crawl_item {
    int64_t parent_id;
    dev_t   root_dev;
    char   *name;      /* full root path if parent_id == 0, else basename */
    char   *full_path;
};

struct crawl_queue {
    struct crawl_item *items;
    size_t head;
    size_t tail;
    size_t cap;
    int    active;
    int    full_crawl; /* 1: run generation sweep when queue drains */
    int64_t gen;
};

static void crawl_queue_clear(struct crawl_queue *cq)
{
    for (size_t i = cq->head; i < cq->tail; i++) {
        free(cq->items[i].name);
        free(cq->items[i].full_path);
    }
    free(cq->items);
    cq->items = NULL;
    cq->head = 0;
    cq->tail = 0;
    cq->cap = 0;
    cq->active = 0;
    cq->full_crawl = 0;
}

static int crawl_queue_push(struct crawl_queue *cq, int64_t parent_id, dev_t root_dev,
                            const char *name, const char *full_path)
{
    if (cq->tail == cq->cap) {
        size_t live = cq->tail - cq->head;
        if (cq->head > 0 && live * 2 <= cq->cap) {
            memmove(cq->items, cq->items + cq->head, live * sizeof *cq->items);
            cq->head = 0;
            cq->tail = live;
        } else {
            size_t ncap = cq->cap ? cq->cap * 2 : 128;
            struct crawl_item *n = realloc(cq->items, ncap * sizeof *n);
            if (!n) return -1;
            cq->items = n;
            cq->cap = ncap;
        }
    }
    char *n_dup = strdup(name);
    char *p_dup = strdup(full_path);
    if (!n_dup || !p_dup) {
        free(n_dup);
        free(p_dup);
        return -1;
    }
    cq->items[cq->tail++] = (struct crawl_item){
        .parent_id = parent_id,
        .root_dev = root_dev,
        .name = n_dup,
        .full_path = p_dup
    };
    return 0;
}

static int crawl_queue_pop(struct crawl_queue *cq, struct crawl_item *out)
{
    if (cq->head >= cq->tail) return 0;
    *out = cq->items[cq->head++];
    if (cq->head == cq->tail) {
        cq->head = 0;
        cq->tail = 0;
    }
    return 1;
}

/* --- Pending move correlation (inotify IN_MOVED_FROM / IN_MOVED_TO) --- */

struct pending_move {
    uint32_t cookie;
    int      is_dir;
    int64_t  dir_id;   /* parent directory id of the source */
    int64_t  item_id;  /* file_id or moved dir_id */
    int64_t  ts_ms;
    char     old_path[PATH_MAX];
    char     name[NAME_MAX + 1];
};

#define PENDING_MOVES_MAX 32

/* --- Document extraction queue and active worker --- */

struct doc_job {
    int64_t file_id;
    char    path[PATH_MAX];
};

struct doc_worker {
    pid_t   pid;
    int     pipe_fd;
    int64_t file_id;
    int64_t start_ms;
    char   *buf;
    size_t  len;
    size_t  cap;
    int     truncated;
};

/* --- Query client connections --- */

struct client_conn {
    int    fd;
    char   inbuf[SEARCH_REQ_LINE_MAX + 8];
    size_t inlen;
};

/* --- Main daemon state --- */

struct daemon_state {
    struct search_index *idx;
    struct search_config cfg;
    struct mount_table mounts;
    struct watch_state ws;
    struct crawl_queue crawl;

    int listen_fd;
    int sweep_tfd; /* 10-minute timerfd only armed when unwatched dirs exist */

    struct client_conn clients[SEARCH_CLIENTS_MAX];
    size_t client_count;

    struct pending_move moves[PENDING_MOVES_MAX];
    size_t move_count;

    /* Deferred transaction batching (~250 ms). */
    int     tx_dirty;
    int64_t tx_deadline_ms;

    /* Ghost removal queue (from queries). */
    int64_t ghost_ids[256];
    size_t  ghost_count;

    /* Document extraction queue (capped at 512) and worker state. */
    struct doc_job doc_queue[SEARCH_DOC_QUEUE_CAP];
    size_t doc_head;
    size_t doc_tail;
    size_t doc_count;
    int    doc_db_exhausted; /* 1 when DB has no more pending docs */
    int    doc_paused_bat;
    int64_t next_bat_check_ms;
    struct doc_worker worker;
};

/* --- Transaction batch helpers --- */

static int ensure_tx(struct daemon_state *ds)
{
    if (!search_index_in_tx(ds->idx)) {
        if (search_index_begin(ds->idx) != SEARCH_OK) {
            say("begin tx failed: %s", search_index_errmsg(ds->idx));
            return -1;
        }
        ds->tx_deadline_ms = now_ms() + SEARCH_BATCH_MS;
    }
    ds->tx_dirty = 1;
    return 0;
}

static void flush_tx(struct daemon_state *ds)
{
    if (search_index_in_tx(ds->idx)) {
        if (search_index_commit(ds->idx) != SEARCH_OK) {
            say("commit tx failed: %s", search_index_errmsg(ds->idx));
            (void)search_index_rollback(ds->idx);
        }
    }
    ds->tx_dirty = 0;
    ds->tx_deadline_ms = 0;
}

static void arm_sweep_timer(struct daemon_state *ds)
{
    if (ds->sweep_tfd < 0) return;
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    if (ds->ws.unwatched_count > 0) {
        its.it_value.tv_sec = SEARCH_SWEEP_SEC;
        its.it_interval.tv_sec = SEARCH_SWEEP_SEC;
    }
    /* When unwatched_count == 0, it_value = 0 disarms timerfd -> zero wakeups. */
    (void)timerfd_settime(ds->sweep_tfd, 0, &its, NULL);
}

/* --- Document queue helpers --- */

static void doc_queue_push(struct daemon_state *ds, int64_t file_id, const char *path)
{
    if (file_id <= 0 || !path || !path[0]) return;
    if (ds->doc_count >= SEARCH_DOC_QUEUE_CAP) {
        /* DB still holds text_state=PENDING/STALE; refill will pick it up later. */
        ds->doc_db_exhausted = 0;
        return;
    }
    /* Avoid duplicate entries in queue. */
    for (size_t i = 0; i < ds->doc_count; i++) {
        size_t slot = (ds->doc_head + i) % SEARCH_DOC_QUEUE_CAP;
        if (ds->doc_queue[slot].file_id == file_id) return;
    }
    struct doc_job *j = &ds->doc_queue[ds->doc_tail];
    j->file_id = file_id;
    snprintf(j->path, sizeof j->path, "%s", path);
    ds->doc_tail = (ds->doc_tail + 1) % SEARCH_DOC_QUEUE_CAP;
    ds->doc_count++;
    ds->doc_db_exhausted = 0;
}

static int doc_queue_pop(struct daemon_state *ds, struct doc_job *out)
{
    if (ds->doc_count == 0) return 0;
    *out = ds->doc_queue[ds->doc_head];
    ds->doc_head = (ds->doc_head + 1) % SEARCH_DOC_QUEUE_CAP;
    ds->doc_count--;
    return 1;
}

static void doc_queue_refill(struct daemon_state *ds)
{
    if (ds->doc_count > 0 || ds->doc_db_exhausted) return;
    struct search_pending_doc batch[64];
    size_t got = 0;
    if (search_index_next_pending_docs(ds->idx, batch, 64, &got) != SEARCH_OK) return;
    if (got == 0) {
        ds->doc_db_exhausted = 1;
        return;
    }
    for (size_t i = 0; i < got; i++) {
        doc_queue_push(ds, batch[i].file_id, batch[i].path);
    }
}

/* --- Crawl operations --- */

static int join_path(const char *a, const char *b, char *out, size_t out_size)
{
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    int need_slash = (alen > 0 && a[alen - 1] != '/');
    if (alen + (size_t)need_slash + blen + 1 > out_size) return -1;
    memcpy(out, a, alen);
    size_t pos = alen;
    if (need_slash) out[pos++] = '/';
    memcpy(out + pos, b, blen + 1);
    return 0;
}

static void index_single_file(struct daemon_state *ds, int64_t dir_id,
                              const char *dir_path, const char *name,
                              dev_t root_dev, int64_t gen)
{
    if (should_skip_file_name(name)) return;
    char full[PATH_MAX];
    if (join_path(dir_path, name, full, sizeof full) != 0) return;

    struct stat st;
    if (lstat(full, &st) != 0) return;
    if (!S_ISREG(st.st_mode)) return;
    if (st.st_dev != root_dev) return;

    int kind = SEARCH_KIND_FILE;
    int tstate = SEARCH_TEXT_SKIPPED;
    classify_file(name, (int64_t)st.st_size, &kind, &tstate);

    if (ensure_tx(ds) != 0) return;
    int64_t fid = search_index_upsert_file(ds->idx, dir_id, name,
                                           (int64_t)st.st_size,
                                           (int64_t)st.st_mtime,
                                           kind, gen, tstate);
    if (fid > 0 && tstate == SEARCH_TEXT_PENDING) {
        ds->doc_db_exhausted = 0;
    }
}

static void crawl_one_dir(struct daemon_state *ds, const struct crawl_item *item)
{
    if (path_is_blocked_mount(&ds->mounts, item->full_path)) return;

    struct stat dst;
    if (lstat(item->full_path, &dst) != 0) return;
    if (!S_ISDIR(dst.st_mode)) return;
    if (dst.st_dev != item->root_dev) return;
    if (dev_is_blocked_mount(&ds->mounts, dst.st_dev)) return;

    if (ensure_tx(ds) != 0) return;
    int64_t dir_id = search_index_upsert_dir(ds->idx, item->parent_id, item->name,
                                             (int64_t)dst.st_mtime, ds->crawl.gen);
    if (dir_id <= 0) return;

    watch_add_dir(&ds->ws, dir_id, item->root_dev, (int64_t)dst.st_mtime, item->full_path);

    int dfd = open(item->full_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return;
    /* Re-verify st_dev on the opened descriptor (avoid TOCTOU mount swap). */
    struct stat fdst;
    if (fstat(dfd, &fdst) != 0 || fdst.st_dev != item->root_dev) {
        close(dfd);
        return;
    }

    DIR *d = fdopendir(dfd);
    if (!d) {
        close(dfd);
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        struct stat cst;
        if (fstatat(dirfd(d), name, &cst, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (cst.st_dev != item->root_dev) continue; /* never cross mount boundary */

        if (S_ISDIR(cst.st_mode)) {
            if (should_skip_dir_name(item->full_path, name, &ds->cfg)) continue;
            char child[PATH_MAX];
            if (join_path(item->full_path, name, child, sizeof child) != 0) continue;
            if (path_is_blocked_mount(&ds->mounts, child)) continue;
            (void)crawl_queue_push(&ds->crawl, dir_id, item->root_dev, name, child);
        } else if (S_ISREG(cst.st_mode)) {
            if (should_skip_file_name(name)) continue;
            int kind = SEARCH_KIND_FILE;
            int tstate = SEARCH_TEXT_SKIPPED;
            classify_file(name, (int64_t)cst.st_size, &kind, &tstate);
            int64_t fid = search_index_upsert_file(ds->idx, dir_id, name,
                                                   (int64_t)cst.st_size,
                                                   (int64_t)cst.st_mtime,
                                                   kind, ds->crawl.gen, tstate);
            if (fid > 0 && tstate == SEARCH_TEXT_PENDING) {
                ds->doc_db_exhausted = 0;
            }
        }
    }
    closedir(d);
}

static void start_full_crawl(struct daemon_state *ds)
{
    crawl_queue_clear(&ds->crawl);
    mount_table_reload(&ds->mounts);
    watch_state_clear(&ds->ws);
    ds->ws.budget = read_watch_budget();

    if (ensure_tx(ds) != 0) return;
    int64_t gen = search_index_next_gen(ds->idx);
    if (gen <= 0) return;

    ds->crawl.gen = gen;
    ds->crawl.full_crawl = 1;
    ds->crawl.active = 1;

    for (size_t i = 0; i < ds->cfg.root_count; i++) {
        const char *root = ds->cfg.roots[i];
        if (path_is_blocked_mount(&ds->mounts, root)) {
            say("skipping blocked mount root: %s", root);
            continue;
        }
        struct stat st;
        if (lstat(root, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (dev_is_blocked_mount(&ds->mounts, st.st_dev)) {
            say("skipping blocked mount device for root: %s", root);
            continue;
        }
        (void)crawl_queue_push(&ds->crawl, 0, st.st_dev, root, root);
    }
}

static void step_crawl(struct daemon_state *ds)
{
    if (!ds->crawl.active) return;

    for (int i = 0; i < SEARCH_CRAWL_SLICE_DIRS; i++) {
        struct crawl_item item;
        if (!crawl_queue_pop(&ds->crawl, &item)) {
            int was_full = ds->crawl.full_crawl;
            int64_t gen = ds->crawl.gen;
            ds->crawl.active = 0;
            ds->crawl.full_crawl = 0;

            if (was_full) {
                int64_t df = 0, dd = 0;
                if (ensure_tx(ds) == 0) {
                    if (search_index_sweep_gen(ds->idx, gen, &df, &dd) != SEARCH_OK) {
                        say("generation sweep failed: %s", search_index_errmsg(ds->idx));
                    }
                    flush_tx(ds);
                }
                if (df > 0 || dd > 0) {
                    (void)search_index_incremental_vacuum(ds->idx, SEARCH_VACUUM_PAGES);
                }
                ds->doc_db_exhausted = 0;
            }
            arm_sweep_timer(ds);
            return;
        }
        crawl_one_dir(ds, &item);
        free(item.name);
        free(item.full_path);
    }
}

/* --- 10-minute unwatched directory sweep (skip unchanged mtimes) --- */

static void sweep_unwatched_dirs(struct daemon_state *ds)
{
    mount_table_reload(&ds->mounts);
    int64_t gen = search_index_current_gen(ds->idx);
    if (gen <= 0) gen = 1;

    size_t i = 0;
    while (i < ds->ws.unwatched_count) {
        struct unwatched_entry *ue = &ds->ws.unwatched[i];
        if (path_is_blocked_mount(&ds->mounts, ue->path)) {
            if (ensure_tx(ds) == 0) {
                (void)search_index_delete_subtree(ds->idx, ue->dir_id);
            }
            free(ue->path);
            ds->ws.unwatched[i] = ds->ws.unwatched[--ds->ws.unwatched_count];
            continue;
        }

        struct stat st;
        if (lstat(ue->path, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_dev != ue->root_dev) {
            if (ensure_tx(ds) == 0) {
                (void)search_index_delete_subtree(ds->idx, ue->dir_id);
            }
            free(ue->path);
            ds->ws.unwatched[i] = ds->ws.unwatched[--ds->ws.unwatched_count];
            continue;
        }

        if ((int64_t)st.st_mtime == ue->mtime) {
            i++;
            continue;
        }

        /* Directory mtime changed: reconcile direct children and crawl new subdirs. */
        int dfd = open(ue->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dfd < 0) {
            i++;
            continue;
        }
        struct stat fdst;
        if (fstat(dfd, &fdst) != 0 || fdst.st_dev != ue->root_dev) {
            close(dfd);
            i++;
            continue;
        }
        DIR *d = fdopendir(dfd);
        if (!d) {
            close(dfd);
            i++;
            continue;
        }

        if (ensure_tx(ds) == 0) {
            (void)search_index_mark_dir_children_stale(ds->idx, ue->dir_id);
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                const char *name = e->d_name;
                if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
                    continue;
                }
                struct stat cst;
                if (fstatat(dirfd(d), name, &cst, AT_SYMLINK_NOFOLLOW) != 0) continue;
                if (cst.st_dev != ue->root_dev) continue;

                if (S_ISDIR(cst.st_mode)) {
                    if (should_skip_dir_name(ue->path, name, &ds->cfg)) continue;
                    char child[PATH_MAX];
                    if (join_path(ue->path, name, child, sizeof child) != 0) continue;
                    if (path_is_blocked_mount(&ds->mounts, child)) continue;
                    int64_t old_mtime = -1;
                    int64_t cid = search_index_find_dir(ds->idx, ue->dir_id, name, &old_mtime);
                    if (cid <= 0) {
                        /* Newly discovered subdir: enqueue incremental crawl. */
                        ds->crawl.gen = gen;
                        ds->crawl.active = 1;
                        (void)crawl_queue_push(&ds->crawl, ue->dir_id, ue->root_dev, name, child);
                    } else {
                        (void)search_index_upsert_dir(ds->idx, ue->dir_id, name, old_mtime, gen);
                    }
                } else if (S_ISREG(cst.st_mode)) {
                    if (should_skip_file_name(name)) continue;
                    int kind = SEARCH_KIND_FILE;
                    int tstate = SEARCH_TEXT_SKIPPED;
                    classify_file(name, (int64_t)cst.st_size, &kind, &tstate);
                    int64_t fid = search_index_upsert_file(ds->idx, ue->dir_id, name,
                                                           (int64_t)cst.st_size,
                                                           (int64_t)cst.st_mtime,
                                                           kind, gen, tstate);
                    if (fid > 0 && tstate == SEARCH_TEXT_PENDING) {
                        ds->doc_db_exhausted = 0;
                    }
                }
            }
            (void)search_index_prune_dir_stale(ds->idx, ue->dir_id);
            ue->mtime = (int64_t)fdst.st_mtime;
        }
        closedir(d);
        i++;
    }
    arm_sweep_timer(ds);
}

/* --- inotify event handling --- */

static void expire_pending_moves(struct daemon_state *ds, int64_t now)
{
    size_t i = 0;
    while (i < ds->move_count) {
        if (now - ds->moves[i].ts_ms >= SEARCH_BATCH_MS) {
            struct pending_move pm = ds->moves[i];
            ds->moves[i] = ds->moves[--ds->move_count];
            /* Unmatched MOVED_FROM: item moved outside watched roots. */
            if (ensure_tx(ds) == 0) {
                if (pm.is_dir) {
                    (void)search_index_delete_subtree(ds->idx, pm.item_id);
                    watch_remove_prefix(&ds->ws, pm.old_path);
                } else {
                    (void)search_index_delete_file(ds->idx, pm.item_id);
                }
            }
            continue;
        }
        i++;
    }
}

static void record_pending_move(struct daemon_state *ds, uint32_t cookie,
                                int is_dir, int64_t dir_id, int64_t item_id,
                                const char *old_path, const char *name)
{
    if (ds->move_count >= PENDING_MOVES_MAX) {
        /* Evict oldest unmatched move as deletion. */
        struct pending_move pm = ds->moves[0];
        memmove(&ds->moves[0], &ds->moves[1], (--ds->move_count) * sizeof ds->moves[0]);
        if (ensure_tx(ds) == 0) {
            if (pm.is_dir) {
                (void)search_index_delete_subtree(ds->idx, pm.item_id);
                watch_remove_prefix(&ds->ws, pm.old_path);
            } else {
                (void)search_index_delete_file(ds->idx, pm.item_id);
            }
        }
    }
    struct pending_move *slot = &ds->moves[ds->move_count++];
    slot->cookie = cookie;
    slot->is_dir = is_dir;
    slot->dir_id = dir_id;
    slot->item_id = item_id;
    slot->ts_ms = now_ms();
    snprintf(slot->old_path, sizeof slot->old_path, "%s", old_path);
    snprintf(slot->name, sizeof slot->name, "%s", name);
}

static int take_pending_move(struct daemon_state *ds, uint32_t cookie,
                             struct pending_move *out)
{
    for (size_t i = 0; i < ds->move_count; i++) {
        if (ds->moves[i].cookie == cookie) {
            *out = ds->moves[i];
            ds->moves[i] = ds->moves[--ds->move_count];
            return 1;
        }
    }
    return 0;
}

static void handle_inotify_event(struct daemon_state *ds, const struct inotify_event *ev)
{
    if (ev->mask & IN_Q_OVERFLOW) {
        say("inotify queue overflow; scheduling full crawl");
        start_full_crawl(ds);
        return;
    }

    struct watch_entry *we = watch_find_by_wd(&ds->ws, ev->wd);
    if (!we) return;

    int64_t dir_id = we->dir_id;
    dev_t root_dev = we->root_dev;
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof dir_path, "%s", we->path);

    if (ev->mask & IN_IGNORED) {
        size_t idx = (size_t)(we - ds->ws.watches);
        watch_remove_idx(&ds->ws, idx, 0);
        return;
    }

    if (ev->mask & IN_DELETE_SELF) {
        size_t idx = (size_t)(we - ds->ws.watches);
        watch_remove_idx(&ds->ws, idx, 1);
        watch_remove_prefix(&ds->ws, dir_path);
        if (ensure_tx(ds) == 0) {
            (void)search_index_delete_subtree(ds->idx, dir_id);
        }
        return;
    }

    if (ev->len == 0 || !ev->name[0]) return;
    const char *name = ev->name;
    int is_dir = (ev->mask & IN_ISDIR) != 0;
    int64_t gen = search_index_current_gen(ds->idx);
    if (gen <= 0) gen = 1;

    char full[PATH_MAX];
    if (join_path(dir_path, name, full, sizeof full) != 0) return;

    if (ev->mask & IN_MOVED_FROM) {
        if (is_dir) {
            int64_t cid = search_index_find_dir(ds->idx, dir_id, name, NULL);
            if (cid > 0) {
                record_pending_move(ds, ev->cookie, 1, dir_id, cid, full, name);
            }
        } else {
            int64_t fid = search_index_find_file(ds->idx, dir_id, name);
            if (fid > 0) {
                record_pending_move(ds, ev->cookie, 0, dir_id, fid, full, name);
            }
        }
        return;
    }

    if (ev->mask & IN_MOVED_TO) {
        struct pending_move pm;
        if (ev->cookie != 0 && take_pending_move(ds, ev->cookie, &pm)) {
            if (pm.is_dir) {
                if (should_skip_dir_name(dir_path, name, &ds->cfg)) {
                    if (ensure_tx(ds) == 0) {
                        (void)search_index_delete_subtree(ds->idx, pm.item_id);
                    }
                    watch_remove_prefix(&ds->ws, pm.old_path);
                } else {
                    if (ensure_tx(ds) == 0) {
                        (void)search_index_move_dir(ds->idx, pm.item_id, dir_id, name);
                    }
                    watch_rename_prefix(&ds->ws, pm.old_path, full);
                }
            } else {
                if (should_skip_file_name(name)) {
                    if (ensure_tx(ds) == 0) {
                        (void)search_index_delete_file(ds->idx, pm.item_id);
                    }
                } else {
                    if (ensure_tx(ds) == 0) {
                        (void)search_index_move_file(ds->idx, pm.item_id, dir_id, name);
                    }
                }
            }
            return;
        }
        /* Unpaired MOVED_TO (moved in from outside watched roots): treat as create. */
        if (is_dir) {
            if (!should_skip_dir_name(dir_path, name, &ds->cfg) &&
                !path_is_blocked_mount(&ds->mounts, full)) {
                ds->crawl.gen = gen;
                ds->crawl.active = 1;
                (void)crawl_queue_push(&ds->crawl, dir_id, root_dev, name, full);
            }
        } else {
            index_single_file(ds, dir_id, dir_path, name, root_dev, gen);
        }
        return;
    }

    if (ev->mask & IN_DELETE) {
        if (is_dir) {
            int64_t cid = search_index_find_dir(ds->idx, dir_id, name, NULL);
            if (cid > 0 && ensure_tx(ds) == 0) {
                (void)search_index_delete_subtree(ds->idx, cid);
            }
            watch_remove_prefix(&ds->ws, full);
        } else {
            if (ensure_tx(ds) == 0) {
                (void)search_index_delete_file_by_name(ds->idx, dir_id, name);
            }
        }
        return;
    }

    if (ev->mask & (IN_CREATE | IN_CLOSE_WRITE | IN_ATTRIB)) {
        if (is_dir) {
            if ((ev->mask & IN_CREATE) &&
                !should_skip_dir_name(dir_path, name, &ds->cfg) &&
                !path_is_blocked_mount(&ds->mounts, full)) {
                ds->crawl.gen = gen;
                ds->crawl.active = 1;
                (void)crawl_queue_push(&ds->crawl, dir_id, root_dev, name, full);
            }
        } else {
            index_single_file(ds, dir_id, dir_path, name, root_dev, gen);
        }
    }
}

static void drain_inotify(struct daemon_state *ds)
{
    char buf[16384] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(ds->ws.inotify_fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        for (char *p = buf; p < buf + n;) {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            handle_inotify_event(ds, ev);
            p += sizeof *ev + ev->len;
        }
    }
}

/* --- Document extraction via /usr/local/bin/locachy-docscan --- */

static void worker_reset(struct doc_worker *w)
{
    if (w->pipe_fd >= 0) {
        close(w->pipe_fd);
        w->pipe_fd = -1;
    }
    free(w->buf);
    w->buf = NULL;
    w->len = 0;
    w->cap = 0;
    w->pid = 0;
    w->file_id = 0;
    w->start_ms = 0;
    w->truncated = 0;
}

static void worker_kill_and_reap(struct daemon_state *ds)
{
    struct doc_worker *w = &ds->worker;
    if (w->pid <= 0) return;
    (void)kill(w->pid, SIGKILL);
    int status = 0;
    while (waitpid(w->pid, &status, 0) < 0 && errno == EINTR) {
    }
    int64_t fid = w->file_id;
    worker_reset(w);
    if (fid > 0 && ensure_tx(ds) == 0) {
        (void)search_index_clear_text(ds->idx, fid, SEARCH_TEXT_SKIPPED);
    }
}

static int docscan_allowed(struct daemon_state *ds, int64_t now)
{
    if (!ds->cfg.search_docs) return 0;
    if (now >= ds->next_bat_check_ms) {
        ds->doc_paused_bat = battery_below_20();
        ds->next_bat_check_ms = now + SEARCH_BATTERY_POLL_SEC * 1000;
    }
    return !ds->doc_paused_bat;
}

static void maybe_start_docscan(struct daemon_state *ds, int64_t now)
{
    if (ds->worker.pid > 0) return;
    if (!docscan_allowed(ds, now)) return;

    if (ds->doc_count == 0) doc_queue_refill(ds);
    if (ds->doc_count == 0) return;

    struct doc_job job;
    while (doc_queue_pop(ds, &job)) {
        /* Skip files over 8 MiB or gone before spawning a child. */
        struct stat st;
        if (lstat(job.path, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size <= 0 || st.st_size > SEARCH_DOCSCAN_MAX_FILE) {
            if (ensure_tx(ds) == 0) {
                (void)search_index_clear_text(ds->idx, job.file_id, SEARCH_TEXT_SKIPPED);
            }
            continue;
        }

        const char *bin = getenv("LOCACHY_DOCSCAN_BIN");
        if (!bin || !bin[0]) bin = SEARCH_DOCSCAN_BIN;
        if (access(bin, X_OK) != 0) {
            /* Helper binary not installed yet; leave items pending without busy-looping. */
            ds->doc_db_exhausted = 1;
            ds->doc_count = 0;
            ds->doc_head = 0;
            ds->doc_tail = 0;
            return;
        }

        int pfd[2];
        if (pipe2(pfd, O_CLOEXEC) != 0) return;

        pid_t pid = fork();
        if (pid < 0) {
            close(pfd[0]);
            close(pfd[1]);
            return;
        }
        if (pid == 0) {
            sigset_t empty;
            sigemptyset(&empty);
            sigprocmask(SIG_SETMASK, &empty, NULL);
            signal(SIGPIPE, SIG_DFL);
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                (void)dup2(devnull, STDIN_FILENO);
                (void)dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
            (void)dup2(pfd[1], STDOUT_FILENO);
            close(pfd[0]);
            close(pfd[1]);

            char *const argv[] = {(char *)bin, job.path, NULL};
            execv(bin, argv);
            _exit(127);
        }

        close(pfd[1]);
        int flags = fcntl(pfd[0], F_GETFL, 0);
        if (flags >= 0) (void)fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

        ds->worker.pid = pid;
        ds->worker.pipe_fd = pfd[0];
        ds->worker.file_id = job.file_id;
        ds->worker.start_ms = now;
        ds->worker.len = 0;
        ds->worker.cap = 0;
        ds->worker.buf = NULL;
        ds->worker.truncated = 0;
        return;
    }
}

static void finish_docscan(struct daemon_state *ds)
{
    struct doc_worker *w = &ds->worker;
    if (w->pid <= 0) return;

    int status = 0;
    pid_t r = waitpid(w->pid, &status, WNOHANG);
    if (r == 0) {
        /* Pipe closed early; give child a brief chance to exit, else kill on timeout. */
        if (w->pipe_fd >= 0) {
            close(w->pipe_fd);
            w->pipe_fd = -1;
        }
        return;
    }
    if (r < 0 && errno != ECHILD) return;

    int64_t fid = w->file_id;
    int ok = (r > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    if (ensure_tx(ds) == 0) {
        if (ok && w->len > 0) {
            if (search_index_set_text(ds->idx, fid, w->buf, w->len) != SEARCH_OK) {
                say("set_text(%lld) failed: %s", (long long)fid,
                    search_index_errmsg(ds->idx));
                (void)search_index_clear_text(ds->idx, fid, SEARCH_TEXT_SKIPPED);
            }
        } else {
            (void)search_index_clear_text(ds->idx, fid, SEARCH_TEXT_SKIPPED);
        }
    }
    worker_reset(w);
}

static void drain_docscan_pipe(struct daemon_state *ds)
{
    struct doc_worker *w = &ds->worker;
    if (w->pipe_fd < 0) return;

    char tmp[8192];
    for (;;) {
        ssize_t n = read(w->pipe_fd, tmp, sizeof tmp);
        if (n > 0) {
            if (!w->truncated && w->len < SEARCH_DOCSCAN_MAX_OUT) {
                size_t take = (size_t)n;
                if (w->len + take > SEARCH_DOCSCAN_MAX_OUT) {
                    take = SEARCH_DOCSCAN_MAX_OUT - w->len;
                    w->truncated = 1;
                }
                if (w->len + take + 1 > w->cap) {
                    size_t ncap = w->cap ? w->cap * 2 : 16384;
                    while (ncap < w->len + take + 1) ncap *= 2;
                    if (ncap > SEARCH_DOCSCAN_MAX_OUT + 1) ncap = SEARCH_DOCSCAN_MAX_OUT + 1;
                    char *nb = realloc(w->buf, ncap);
                    if (!nb) {
                        w->truncated = 1;
                        continue;
                    }
                    w->buf = nb;
                    w->cap = ncap;
                }
                memcpy(w->buf + w->len, tmp, take);
                w->len += take;
                w->buf[w->len] = '\0';
            }
            continue;
        }
        if (n == 0) {
            close(w->pipe_fd);
            w->pipe_fd = -1;
            finish_docscan(ds);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        close(w->pipe_fd);
        w->pipe_fd = -1;
        finish_docscan(ds);
        return;
    }
}

/* --- Ghost queue flushing --- */

static void queue_ghost_ids(struct daemon_state *ds, const int64_t *ids, size_t n)
{
    for (size_t i = 0; i < n && ds->ghost_count < sizeof ds->ghost_ids / sizeof ds->ghost_ids[0]; i++) {
        int dup = 0;
        for (size_t j = 0; j < ds->ghost_count; j++) {
            if (ds->ghost_ids[j] == ids[i]) { dup = 1; break; }
        }
        if (!dup) ds->ghost_ids[ds->ghost_count++] = ids[i];
    }
}

static void flush_ghosts(struct daemon_state *ds)
{
    if (ds->ghost_count == 0) return;
    if (ensure_tx(ds) != 0) return;
    for (size_t i = 0; i < ds->ghost_count; i++) {
        (void)search_index_delete_file(ds->idx, ds->ghost_ids[i]);
    }
    ds->ghost_count = 0;
}

/* --- Unix socket server and query protocol --- */

static int write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd p = {.fd = fd, .events = POLLOUT};
                if (poll(&p, 1, 2000) <= 0) return -1;
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static void send_hits(int fd, const struct search_hit *hits, size_t count)
{
    char line[SEARCH_PATH_MAX + SEARCH_NAME_MAX + SEARCH_SNIPPET_MAX + 64];
    for (size_t i = 0; i < count; i++) {
        int n = snprintf(line, sizeof line, "R %d %s\t%s\t%s\t%s\n",
                         hits[i].layer,
                         search_kind_name(hits[i].kind),
                         hits[i].path,
                         hits[i].name,
                         hits[i].snippet);
        if (n <= 0 || (size_t)n >= sizeof line) continue;
        if (write_all(fd, line, (size_t)n) != 0) return;
    }
}

static void handle_query_line(struct daemon_state *ds, int fd, const char *line, size_t len)
{
    /* Protocol: Q <scope> <query>   where scope is names | content | all */
    if (len == 0 || len > SEARCH_REQ_LINE_MAX) {
        (void)write_all(fd, "E\n", 2);
        return;
    }
    if (len < 4 || line[0] != 'Q' || line[1] != ' ') {
        (void)write_all(fd, "E\n", 2);
        return;
    }

    const char *p = line + 2;
    const char *sp = strchr(p, ' ');
    if (!sp || sp == p) {
        (void)write_all(fd, "E\n", 2);
        return;
    }

    int want_names = 0;
    int want_content = 0;
    size_t slen = (size_t)(sp - p);
    if (slen == 5 && memcmp(p, "names", 5) == 0) {
        want_names = 1;
    } else if (slen == 7 && memcmp(p, "content", 7) == 0) {
        want_content = 1;
    } else if (slen == 3 && memcmp(p, "all", 3) == 0) {
        want_names = 1;
        want_content = 1;
    } else {
        (void)write_all(fd, "E\n", 2);
        return;
    }

    const char *query = sp + 1;
    if (!query[0] || strlen(query) > SEARCH_QUERY_MAX) {
        (void)write_all(fd, "E\n", 2);
        return;
    }

    /* Flush any open write transaction so readers see consistent state. */
    if (ds->tx_dirty) flush_tx(ds);

    struct search_hit *hits = malloc(SEARCH_MAX_RESULTS * sizeof *hits);
    if (!hits) {
        (void)write_all(fd, "E\n", 2);
        return;
    }

    size_t remaining = SEARCH_MAX_RESULTS;
    int64_t gone[SEARCH_GHOST_TOP_N];

    /* Layer 1: names, streamed first. */
    if (want_names && remaining > 0) {
        size_t count = 0;
        if (search_index_query_names(ds->idx, query, remaining, hits, &count) == SEARCH_OK &&
            count > 0) {
            size_t ngone = 0;
            (void)search_index_filter_ghosts(hits, &count, SEARCH_GHOST_TOP_N,
                                             gone, SEARCH_GHOST_TOP_N, &ngone);
            if (ngone > 0) queue_ghost_ids(ds, gone, ngone);
            send_hits(fd, hits, count);
            remaining = (count < remaining) ? (remaining - count) : 0;
        }
    }

    /* Layer 2: content, streamed after layer 1. */
    if (want_content && remaining > 0) {
        size_t count = 0;
        if (search_index_query_content(ds->idx, query, remaining, hits, &count) == SEARCH_OK &&
            count > 0) {
            size_t ngone = 0;
            (void)search_index_filter_ghosts(hits, &count, SEARCH_GHOST_TOP_N,
                                             gone, SEARCH_GHOST_TOP_N, &ngone);
            if (ngone > 0) queue_ghost_ids(ds, gone, ngone);
            send_hits(fd, hits, count);
        }
    }

    free(hits);
    (void)write_all(fd, "E\n", 2);
}

static int open_listen_socket(const char *path)
{
    if (ensure_parent_dir(path, 0755) != 0) {
        say("cannot create socket directory for %s: %s", path, strerror(errno));
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        say("socket failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) {
        close(fd);
        return -1;
    }
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);

    (void)unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        say("bind(%s) failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    /* Allow uid 0 and uid 1000 to connect; peer UID is enforced via SO_PEERCRED. */
    (void)chmod(path, 0666);
    if (listen(fd, 16) != 0) {
        say("listen failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void accept_clients(struct daemon_state *ds)
{
    for (;;) {
        int cfd = accept4(ds->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            break;
        }

        struct ucred cred;
        socklen_t clen = sizeof cred;
        if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0 ||
            (cred.uid != 0 && cred.uid != (uid_t)SEARCH_USER_UID)) {
            close(cfd);
            continue;
        }

        if (ds->client_count >= SEARCH_CLIENTS_MAX) {
            close(cfd);
            continue;
        }

        struct client_conn *c = &ds->clients[ds->client_count++];
        memset(c, 0, sizeof *c);
        c->fd = cfd;
    }
}

static void close_client(struct daemon_state *ds, size_t idx)
{
    if (idx >= ds->client_count) return;
    close(ds->clients[idx].fd);
    ds->clients[idx] = ds->clients[--ds->client_count];
}

static void read_client(struct daemon_state *ds, size_t idx)
{
    struct client_conn *c = &ds->clients[idx];
    char tmp[512];
    ssize_t n = recv(c->fd, tmp, sizeof tmp, 0);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        close_client(ds, idx);
        return;
    }

    for (ssize_t i = 0; i < n; i++) {
        char ch = tmp[i];
        if (ch == '\0') {
            /* NUL bytes are forbidden in requests; reject connection. */
            (void)write_all(c->fd, "E\n", 2);
            close_client(ds, idx);
            return;
        }
        if (ch == '\n') {
            if (c->inlen > 0 && c->inbuf[c->inlen - 1] == '\r') c->inlen--;
            c->inbuf[c->inlen] = '\0';
            handle_query_line(ds, c->fd, c->inbuf, c->inlen);
            c->inlen = 0;
        } else {
            if (c->inlen >= SEARCH_REQ_LINE_MAX) {
                /* Refuse lines exceeding 256 bytes immediately. */
                (void)write_all(c->fd, "E\n", 2);
                close_client(ds, idx);
                return;
            }
            c->inbuf[c->inlen++] = ch;
        }
    }
}

/* --- Poll timeout calculation (infinite when idle) --- */

static int compute_poll_timeout_ms(const struct daemon_state *ds, int64_t now)
{
    /* Active incremental crawl or pending ghost removals: return immediately. */
    if (ds->crawl.active || ds->ghost_count > 0) return 0;

    int timeout = -1; /* infinite by default -> zero idle wakeups */

    if (ds->tx_dirty && ds->tx_deadline_ms > 0) {
        int64_t rem = ds->tx_deadline_ms - now;
        int t = rem > 0 ? (int)rem : 0;
        if (timeout < 0 || t < timeout) timeout = t;
    }

    if (ds->move_count > 0) {
        int64_t earliest = ds->moves[0].ts_ms + SEARCH_BATCH_MS;
        for (size_t i = 1; i < ds->move_count; i++) {
            int64_t exp = ds->moves[i].ts_ms + SEARCH_BATCH_MS;
            if (exp < earliest) earliest = exp;
        }
        int64_t rem = earliest - now;
        int t = rem > 0 ? (int)rem : 0;
        if (timeout < 0 || t < timeout) timeout = t;
    }

    if (ds->worker.pid > 0) {
        int64_t deadline = ds->worker.start_ms + SEARCH_DOCSCAN_TIMEOUT_MS;
        int64_t rem = deadline - now;
        int t = rem > 0 ? (int)rem : 0;
        if (timeout < 0 || t < timeout) timeout = t;
    } else if (ds->cfg.search_docs && (ds->doc_count > 0 || !ds->doc_db_exhausted)) {
        if (ds->doc_paused_bat) {
            /* Only wake periodically to re-check battery while work is waiting. */
            int64_t rem = ds->next_bat_check_ms - now;
            int t = rem > 0 ? (int)rem : 0;
            if (timeout < 0 || t < timeout) timeout = t;
        } else {
            return 0;
        }
    }

    return timeout;
}

int main(void)
{
    sigset_t sigmask;
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGINT);
    sigaddset(&sigmask, SIGHUP);
    sigaddset(&sigmask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &sigmask, NULL) != 0) {
        say("sigprocmask failed: %s", strerror(errno));
        return 1;
    }
    int sig_fd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) {
        say("signalfd failed: %s", strerror(errno));
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    lower_priority();

    const char *db_path = getenv("LOCACHY_SEARCH_DB");
    if (!db_path || !db_path[0]) db_path = SEARCH_DB_PATH;
    const char *sock_path = getenv("LOCACHY_SEARCH_SOCK");
    if (!sock_path || !sock_path[0]) sock_path = SEARCH_SOCK_PATH;

    if (ensure_parent_dir(db_path, 0750) != 0) {
        say("cannot create index directory for %s: %s", db_path, strerror(errno));
        return 1;
    }

    static struct daemon_state ds; /* large: keep it out of the stack */
    memset(&ds, 0, sizeof ds);
    ds.ws.inotify_fd = -1;
    ds.listen_fd = -1;
    ds.sweep_tfd = -1;
    ds.worker.pipe_fd = -1;

    int rc = search_index_open(db_path, &ds.idx);
    if (rc != SEARCH_OK) {
        say("cannot open index %s (rc=%d)", db_path, rc);
        return 1;
    }

    ds.ws.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ds.ws.inotify_fd < 0) {
        say("inotify_init1 failed: %s (falling back to periodic sweeps)", strerror(errno));
    }

    ds.sweep_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (ds.sweep_tfd < 0) {
        say("timerfd_create failed: %s", strerror(errno));
    }

    ds.listen_fd = open_listen_socket(sock_path);
    if (ds.listen_fd < 0) {
        search_index_close(ds.idx);
        return 1;
    }

    load_config(&ds.cfg);
    start_full_crawl(&ds);

    while (!g_stop) {
        if (g_reload) {
            g_reload = 0;
            load_config(&ds.cfg);
            ds.next_bat_check_ms = 0;
            start_full_crawl(&ds);
        }

        int64_t now = now_ms();

        /* Reap or time out active docscan worker. */
        if (g_chld) {
            g_chld = 0;
            if (ds.worker.pid > 0 && ds.worker.pipe_fd < 0) {
                finish_docscan(&ds);
            }
        }
        if (ds.worker.pid > 0) {
            if (ds.worker.pipe_fd < 0) {
                finish_docscan(&ds);
            }
            if (ds.worker.pid > 0 && now - ds.worker.start_ms >= SEARCH_DOCSCAN_TIMEOUT_MS) {
                say("docscan timed out for file_id=%lld; killing", (long long)ds.worker.file_id);
                worker_kill_and_reap(&ds);
            }
        }

        /* Expire unmatched IN_MOVED_FROM entries. */
        if (ds.move_count > 0) expire_pending_moves(&ds, now);

        /* Flush batched write transaction every ~250 ms. */
        if (ds.tx_dirty && now >= ds.tx_deadline_ms) {
            flush_tx(&ds);
        }

        /* Process ghost removals queued by query path. */
        if (ds.ghost_count > 0) flush_ghosts(&ds);

        /* Advance incremental crawl slice (keeps queries low-latency). */
        if (ds.crawl.active) step_crawl(&ds);

        /* Start next document extraction when allowed and idle. */
        if (!ds.crawl.active && ds.worker.pid == 0) {
            maybe_start_docscan(&ds, now);
        }

        struct pollfd pfds[5 + SEARCH_CLIENTS_MAX];
        nfds_t nfds = 0;

        int idx_sig = -1, idx_listen = -1, idx_inotify = -1, idx_sweep = -1, idx_worker = -1;
        size_t client_pfd_start = 0;

        idx_sig = (int)nfds;
        pfds[nfds++] = (struct pollfd){.fd = sig_fd, .events = POLLIN};

        idx_listen = (int)nfds;
        pfds[nfds++] = (struct pollfd){.fd = ds.listen_fd, .events = POLLIN};

        if (ds.ws.inotify_fd >= 0) {
            idx_inotify = (int)nfds;
            pfds[nfds++] = (struct pollfd){.fd = ds.ws.inotify_fd, .events = POLLIN};
        }
        if (ds.sweep_tfd >= 0) {
            idx_sweep = (int)nfds;
            pfds[nfds++] = (struct pollfd){.fd = ds.sweep_tfd, .events = POLLIN};
        }
        if (ds.worker.pipe_fd >= 0) {
            idx_worker = (int)nfds;
            pfds[nfds++] = (struct pollfd){.fd = ds.worker.pipe_fd, .events = POLLIN};
        }

        client_pfd_start = (size_t)nfds;
        for (size_t i = 0; i < ds.client_count; i++) {
            pfds[nfds++] = (struct pollfd){.fd = ds.clients[i].fd, .events = POLLIN};
        }

        int timeout = compute_poll_timeout_ms(&ds, now_ms());
        int pr = poll(pfds, nfds, timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            say("poll failed: %s", strerror(errno));
            break;
        }
        if (pr == 0) continue;

        if (idx_sig >= 0 && (pfds[idx_sig].revents & POLLIN)) {
            struct signalfd_siginfo ssi;
            while (read(sig_fd, &ssi, sizeof ssi) == (ssize_t)sizeof ssi) {
                if (ssi.ssi_signo == SIGTERM || ssi.ssi_signo == SIGINT) g_stop = 1;
                else if (ssi.ssi_signo == SIGHUP) g_reload = 1;
                else if (ssi.ssi_signo == SIGCHLD) g_chld = 1;
            }
        }
        if (idx_listen >= 0 && (pfds[idx_listen].revents & POLLIN)) {
            accept_clients(&ds);
        }
        if (idx_inotify >= 0 && (pfds[idx_inotify].revents & POLLIN)) {
            drain_inotify(&ds);
        }
        if (idx_sweep >= 0 && (pfds[idx_sweep].revents & POLLIN)) {
            uint64_t ticks = 0;
            while (read(ds.sweep_tfd, &ticks, sizeof ticks) > 0) {
            }
            sweep_unwatched_dirs(&ds);
        }
        if (idx_worker >= 0 && (pfds[idx_worker].revents & (POLLIN | POLLHUP | POLLERR))) {
            drain_docscan_pipe(&ds);
        }

        /* Service client reads backwards so swap-remove stays valid. */
        size_t n_polled_clients = (size_t)nfds - client_pfd_start;
        for (size_t k = n_polled_clients; k > 0; k--) {
            size_t ci = k - 1;
            if (ci >= ds.client_count) continue;
            short rev = pfds[client_pfd_start + ci].revents;
            if (rev & (POLLIN | POLLHUP | POLLERR)) {
                read_client(&ds, ci);
            }
        }
    }

    worker_kill_and_reap(&ds);
    flush_ghosts(&ds);
    flush_tx(&ds);

    for (size_t i = 0; i < ds.client_count; i++) close(ds.clients[i].fd);
    if (ds.listen_fd >= 0) close(ds.listen_fd);
    if (sig_fd >= 0) close(sig_fd);
    (void)unlink(sock_path);
    if (ds.sweep_tfd >= 0) close(ds.sweep_tfd);
    watch_state_clear(&ds.ws);
    if (ds.ws.inotify_fd >= 0) close(ds.ws.inotify_fd);
    crawl_queue_clear(&ds.crawl);
    mount_table_free(&ds.mounts);
    search_index_close(ds.idx);
    return 0;
}
