/* Asynchronous file search: a permanently unprivileged worker and a bounded,
 * fragmented result stream. Text and replacements use edit-search unchanged. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base.h"
#include "edit-filesearch.h"
#include "edit-search.h"
#include <glib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define FRAME_MAX 65536U
#define FILE_MAX (64U * 1024U * 1024U)
#define RESULTS_MAX 100000U
#define RESULTS_BYTES (64U * 1024U * 1024U)
#define READ_BUDGET (256U * 1024U)
#define PREVIEW_MAX 512U
#define SOURCE_MAX 100000U
/* A separate socket belongs to each search, so an old worker cannot publish
 * results into a newer generation, including after cancellation. */
enum { MSG_MATCH = 1, MSG_WARNING, MSG_DONE, MSG_ERROR };
struct edit_filesearch {
    pid_t pid;
    int fd, done, saw_done;
    char error[256], warning[512];
    unsigned char input[FRAME_MAX + 4];
    size_t used, count, capacity, bytes;
    struct edit_filematch *matches;
    struct edit_filesearch_stats stats;
    char *pattern;
    int mode, match_case;
};
struct worker {
    int fd, stopped, warned;
    const struct edit_filesearch_options *o;
    struct search search;
    struct edit_filesearch_stats stats;
    size_t matches, bytes;
    GHashTable *directories, *overlays;
};

static void message(char *out, size_t size, const char *text)
{
    if (out && size) snprintf(out, size, "%s", text);
}
static void put32(unsigned char *p, uint32_t n)
{
    for (int i = 3; i >= 0; i--) { p[i] = (unsigned char)n; n >>= 8; }
}
static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put64(unsigned char *p, uint64_t n)
{
    for (int i = 7; i >= 0; i--) { p[i] = (unsigned char)n; n >>= 8; }
}
static uint64_t get64(const unsigned char *p)
{
    uint64_t n = 0;
    for (int i = 0; i < 8; i++) n = (n << 8) | p[i];
    return n;
}
static void checksum(const char *text, size_t len, uint8_t out[20])
{
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA1);
    char header[40];
    int n = snprintf(header, sizeof header, "blob %zu", len);
    g_checksum_update(c, (const guchar *)header, (gssize)n + 1);
    g_checksum_update(c, (const guchar *)text, (gssize)len);
    gsize size = 20;
    g_checksum_get_digest(c, out, &size);
    g_checksum_free(c);
}

/* No fsuid-only impersonation: supplementary groups and saved IDs are dropped
 * too. Filesystem enumeration starts only after this succeeds. */
static int drop_user(void)
{
    struct passwd *pw = getpwuid(EDIT_USER);
    if (!pw) return EACCES;
    gid_t gid = pw->pw_gid;
    int n = 16;
    gid_t *groups = malloc((size_t)n * sizeof *groups);
    if (!groups) return ENOMEM;
    if (getgrouplist(pw->pw_name, gid, groups, &n) < 0) {
        if (n <= 0 || n > 65536) { free(groups); return EACCES; }
        gid_t *more = realloc(groups, (size_t)n * sizeof *groups);
        if (!more) { free(groups); return ENOMEM; }
        groups = more;
        if (getgrouplist(pw->pw_name, gid, groups, &n) < 0) { free(groups); return EACCES; }
    }
    if (geteuid() == 0) {
        if (setgroups((size_t)n, groups) < 0 || setresgid(gid, gid, gid) < 0 ||
            setresuid(EDIT_USER, EDIT_USER, EDIT_USER) < 0) {
            int e = errno; free(groups); return e;
        }
    } else {
        int have = getgroups(0, NULL);
        gid_t *actual = have > 0 ? malloc((size_t)have * sizeof *actual) : NULL;
        int ok = getuid() == EDIT_USER && geteuid() == EDIT_USER && getgid() == gid && getegid() == gid;
        if (have < 0 || (have && (!actual || getgroups(have, actual) < 0))) ok = 0;
        for (int i = 0; i < have && ok; i++) {
            int found = 0;
            for (int j = 0; j < n; j++) if (actual[i] == groups[j]) found = 1;
            if (!found) ok = 0;
        }
        free(actual);
        if (!ok || setresgid(gid, gid, gid) < 0 || setresuid(EDIT_USER, EDIT_USER, EDIT_USER) < 0) {
            free(groups); return EACCES;
        }
    }
    free(groups);
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) return errno;
    return 0;
}
static void close_inherited(int keep)
{
#ifdef SYS_close_range
    if ((keep <= 3 || syscall(SYS_close_range, 3U, (unsigned)keep - 1, 0U) == 0) &&
        syscall(SYS_close_range, (unsigned)keep + 1, ~0U, 0U) == 0) {
        for (int fd = 0; fd < 3; fd++) if (fd != keep) close(fd);
        return;
    }
#endif
    struct rlimit r;
    rlim_t max = getrlimit(RLIMIT_NOFILE, &r) == 0 ? r.rlim_cur : 65536;
    if (max == RLIM_INFINITY) max = 1048576;
    for (rlim_t fd = 0; fd < max; fd++) if ((int)fd != keep) close((int)fd);
}
static int send_all(int fd, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}
static int frame(int fd, const void *payload, size_t n)
{
    unsigned char header[4];
    if (!n || n > FRAME_MAX) return -1;
    put32(header, (uint32_t)n);
    return send_all(fd, header, 4) || send_all(fd, payload, n) ? -1 : 0;
}
static void text_frame(int fd, int type, const char *text)
{
    unsigned char payload[1024];
    size_t n = strlen(text);
    if (n > sizeof payload - 1) n = sizeof payload - 1;
    payload[0] = (unsigned char)type;
    memcpy(payload + 1, text, n);
    (void)frame(fd, payload, n + 1);
}
static void warning(struct worker *w, const char *path, int err)
{
    w->stats.errors++;
    if (!w->warned) {
        char text[1000];
        snprintf(text, sizeof text, "%s: %.850s%s", strerror(err), path, strlen(path) > 850 ? "..." : "");
        text_frame(w->fd, MSG_WARNING, text);
        w->warned = 1;
    }
}

/* Capture regular files with O_NONBLOCK, rejecting special-file races and
 * growth beyond the bound. Decode an immutable memfd through file_load so
 * encoding/EOL semantics remain the editor's, not a second implementation. */
static int load_snapshot(const char *path, int follow, char **text, size_t *len,
                         struct file_format *format, uint8_t digest[20], int *flags)
{
    *text = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | (follow ? 0 : O_NOFOLLOW));
    if (fd < 0) return errno;
    struct stat st;
    int err = 0, mem = -1;
    if (fstat(fd, &st) < 0) err = errno;
    else if (!S_ISREG(st.st_mode)) err = EINVAL;
    else if (st.st_size < 0 || (uintmax_t)st.st_size > FILE_MAX) err = EFBIG;
    if (!err && (mem = memfd_create("edit-search-snapshot", MFD_CLOEXEC)) < 0) err = errno;
    char bytes[32768];
    size_t total = 0;
    while (!err) {
        ssize_t n = read(fd, bytes, sizeof bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { err = errno; break; }
        if (!n) break;
        if ((size_t)n > FILE_MAX - total) { err = EFBIG; break; }
        total += (size_t)n;
        size_t at = 0;
        while (at < (size_t)n) {
            ssize_t k = write(mem, bytes + at, (size_t)n - at);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { err = errno ? errno : EIO; break; }
            at += (size_t)k;
        }
    }
    close(fd);
    if (!err) {
        char proc[64];
        snprintf(proc, sizeof proc, "/proc/self/fd/%d", mem);
        size_t longest;
        err = file_load(proc, text, len, format, flags, &st, digest, 0, &longest);
        if (!err && *len > FILE_MAX) { free(*text); *text = NULL; err = EFBIG; }
    }
    if (mem >= 0) close(mem);
    return err;
}
static int binary_text(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v') return 1;
    }
    return 0;
}
static int valid_globs(const char *list)
{
    if (!list) return 1;
    int bracket = 0;
    for (size_t i = 0; list[i]; i++) {
        if (list[i] == '\\') { if (!list[i + 1]) return 0; i++; continue; }
        if (list[i] == '[' && !bracket) {
            bracket = 1;
            /* A literal ']' may be the first member. */
            if (list[i + 1] == '!' || list[i + 1] == '^') i++;
            if (list[i + 1] == ']') i++;
        } else if (list[i] == ']' && bracket) bracket = 0;
        else if ((list[i] == ';' || list[i] == ',') && bracket) return 0;
    }
    return !bracket;
}
static int glob_list(const char *list, const char *rel, const char *name, int directory)
{
    if (!list || !*list) return 0;
    const char *p = list;
    while (*p) {
        const char *end = p + strcspn(p, ";,");
        size_t n = (size_t)(end - p);
        char *glob = strndup(p, n);
        if (!glob) return 0;
        if (directory && n && glob[n - 1] == '/') glob[--n] = '\0';
        const char *subject = strchr(glob, '/') ? rel : name;
        int hit = n && fnmatch(glob, subject, FNM_PATHNAME) == 0;
        free(glob);
        if (hit) return 1;
        p = *end ? end + 1 : end;
    }
    return 0;
}
static const char *base_name(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}
static const char *relative_path(const struct edit_filesearch_options *o, const char *path)
{
    if (o->root && *o->root) {
        size_t n = strlen(o->root);
        while (n > 1 && o->root[n - 1] == '/') n--;
        if (n == 1 && o->root[0] == '/' && path[0] == '/') return path + 1;
        if (!strncmp(path, o->root, n) && path[n] == '/') return path + n + 1;
    }
    return path;
}
static int selected(struct worker *w, const char *rel, const char *name, int dir)
{
    if (glob_list(w->o->exclude, rel, name, dir)) return 0;
    return dir || !w->o->include || !*w->o->include || glob_list(w->o->include, rel, name, 0);
}
static int emit_match(struct worker *w, const char *path, const char *preview, size_t start, size_t end,
                      size_t line, size_t column, const uint8_t digest[20], const uint8_t td[20], int from_buffer)
{
    size_t pn = strlen(path), vn = strlen(preview), n = 82 + pn + vn;
    if (n > FRAME_MAX) { warning(w, path, ENAMETOOLONG); return -1; }
    if (w->matches == RESULTS_MAX || n + sizeof(struct edit_filematch) > RESULTS_BYTES - w->bytes) {
        text_frame(w->fd, MSG_ERROR, _("Search result limit reached (100000 matches / 64 MiB)"));
        w->stopped = 1;
        return -1;
    }
    unsigned char *p = malloc(n);
    if (!p) { w->stopped = 1; text_frame(w->fd, MSG_ERROR, _("No memory")); return -1; }
    p[0] = MSG_MATCH; p[1] = (unsigned char)from_buffer;
    put32(p + 2, (uint32_t)pn); put32(p + 6, (uint32_t)vn);
    put64(p + 10, start); put64(p + 18, end); put64(p + 26, line); put64(p + 34, column);
    memcpy(p + 42, digest, 20); memcpy(p + 62, td, 20);
    memcpy(p + 82, path, pn); memcpy(p + 82 + pn, preview, vn);
    int rc = frame(w->fd, p, n);
    free(p);
    if (rc) w->stopped = 1;
    w->matches++; w->bytes += n + sizeof(struct edit_filematch);
    return rc;
}
static void scan_text(struct worker *w, const char *path, const char *text, size_t len, const uint8_t digest[20], int from_buffer)
{
    struct buffer b = {0};
    if (buffer_init(&b) < 0 || buffer_set(&b, text, len) < 0) {
        buffer_free(&b); warning(w, path, ENOMEM); return;
    }
    uint8_t td[20]; checksum(text, len, td);
    size_t at = 0, cursor = 0, line = 1, col = 1, start, end;
    while (!w->stopped && search_find(&w->search, &b, at, 0, 0, len, &start, &end)) {
        while (cursor < start) {
            size_t step;
            uint32_t cp = text_decode(text + cursor, len - cursor, &step);
            cursor += step;
            if (cp == '\n') { line++; col = 1; } else col++;
        }
        size_t ls = buffer_line_start(&b, buffer_line_of(&b, start));
        size_t le = ls + buffer_line_length(&b, buffer_line_of(&b, start));
        size_t first = start - ls > 160 ? start - 160 : ls;
        while (first < start && ((unsigned char)text[first] & 0xC0) == 0x80) first++;
        size_t stop = le - first > PREVIEW_MAX ? first + PREVIEW_MAX : le;
        while (stop > first && stop < len && ((unsigned char)text[stop] & 0xC0) == 0x80) stop--;
        char preview[PREVIEW_MAX + 1];
        memcpy(preview, text + first, stop - first); preview[stop - first] = '\0';
        for (size_t i = 0; i < stop - first; i++) if ((unsigned char)preview[i] < 32) preview[i] = ' ';
        if (emit_match(w, path, preview, start, end, line, col, digest, td, from_buffer)) break;
        if (end > start) at = end;
        else if (start < len) at = buffer_next_char(&b, start);
        else break;
    }
    buffer_free(&b);
}
static void scan_file(struct worker *w, const char *path, const char *source, size_t source_len)
{
    char *owned = NULL;
    const char *text = source;
    size_t len = source_len;
    uint8_t digest[20];
    if (source) {
        if (len > FILE_MAX) { w->stats.skipped++; warning(w, path, EFBIG); return; }
        checksum(source, len, digest);
    } else {
        struct file_format format;
        int flags;
        int err = load_snapshot(path, w->o->follow_links, &owned, &len, &format, digest, &flags);
        if (err) { w->stats.skipped++; warning(w, path, err); return; }
        text = owned;
        if ((flags & FILE_BROKEN) || binary_text(text, len)) {
            w->stats.binary++; free(owned); return;
        }
    }
    w->stats.files++;
    scan_text(w, path, text, len, digest, source != NULL);
    free(owned);
}
struct directory { char *path, *rel; };
static void directory_free(gpointer p)
{
    struct directory *d = p;
    free(d->path); free(d->rel); free(d);
}
static char *join(const char *a, const char *b)
{
    size_t an = strlen(a), bn = strlen(b);
    if (an > SIZE_MAX - bn - 2) return NULL;
    char *s = malloc(an + bn + 2);
    if (s) snprintf(s, an + bn + 2, "%s%s%s", a, an && a[an - 1] != '/' ? "/" : "", b);
    return s;
}
static int remember_directory(struct worker *w, const struct stat *st)
{
    char key[100];
    snprintf(key, sizeof key, "%ju:%ju", (uintmax_t)st->st_dev, (uintmax_t)st->st_ino);
    if (g_hash_table_contains(w->directories, key)) return 0;
    if (g_hash_table_size(w->directories) >= SOURCE_MAX) {
        text_frame(w->fd, MSG_ERROR, _("Directory limit reached (100000)")); w->stopped = 1; return 0;
    }
    g_hash_table_add(w->directories, g_strdup(key));
    return 1;
}
static void scan_tree(struct worker *w)
{
    GPtrArray *stack = g_ptr_array_new_with_free_func(directory_free);
    struct directory *root = calloc(1, sizeof *root);
    if (!root) { warning(w, w->o->root, ENOMEM); g_ptr_array_unref(stack); return; }
    root->path = strdup(w->o->root); root->rel = strdup("");
    if (!root->path || !root->rel) { directory_free(root); g_ptr_array_unref(stack); return; }
    g_ptr_array_add(stack, root);
    while (stack->len && !w->stopped) {
        struct directory *d = g_ptr_array_steal_index(stack, stack->len - 1);
        int fd = open(d->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK |
                      (w->o->follow_links ? 0 : O_NOFOLLOW));
        if (fd < 0) { warning(w, d->path, errno); directory_free(d); continue; }
        struct stat self;
        if (fstat(fd, &self) < 0) { warning(w, d->path, errno); close(fd); directory_free(d); continue; }
        if (!remember_directory(w, &self)) { close(fd); directory_free(d); continue; }
        DIR *dir = fdopendir(fd);
        if (!dir) { warning(w, d->path, errno); close(fd); directory_free(d); continue; }
        struct dirent *ent;
        while (!w->stopped) {
            errno = 0; ent = readdir(dir);
            if (!ent) { if (errno) warning(w, d->path, errno); break; }
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..") ||
                (!w->o->hidden && ent->d_name[0] == '.')) continue;
            char *path = join(d->path, ent->d_name), *rel = join(d->rel, ent->d_name);
            if (!path || !rel) { free(path); free(rel); warning(w, d->path, ENOMEM); break; }
            struct stat st;
            if (fstatat(fd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) warning(w, path, errno);
            else {
                int link = S_ISLNK(st.st_mode);
                if (link && !w->o->follow_links) { free(path); free(rel); continue; }
                if (link && fstatat(fd, ent->d_name, &st, 0) < 0) warning(w, path, errno);
                else if (S_ISDIR(st.st_mode)) {
                    if (w->o->recursive && selected(w, rel, ent->d_name, 1)) {
                        if (stack->len >= SOURCE_MAX) { text_frame(w->fd, MSG_ERROR, _("Directory queue limit reached")); w->stopped = 1; }
                        else {
                            struct directory *next = malloc(sizeof *next);
                            if (!next) warning(w, path, ENOMEM);
                            else { *next = (struct directory){path, rel}; g_ptr_array_add(stack, next); path = rel = NULL; }
                        }
                    }
                } else if (S_ISREG(st.st_mode) && selected(w, rel, ent->d_name, 0)) {
                    char *key = g_canonicalize_filename(path, NULL);
                    const struct edit_search_source *overlay = g_hash_table_lookup(w->overlays, key);
                    g_free(key);
                    /* Keep the buffer's canonical UI path so replacement finds
                     * the open document rather than a different path spelling. */
                    scan_file(w, overlay ? overlay->path : path, overlay ? overlay->text : NULL,
                              overlay ? overlay->length : 0);
                }
            }
            free(path); free(rel);
        }
        closedir(dir); directory_free(d);
    }
    g_ptr_array_unref(stack);
}
static void run_worker(int fd, const struct edit_filesearch_options *o)
{
    close_inherited(fd);
    int err = drop_user();
    if (err) { text_frame(fd, MSG_ERROR, _("Cannot permanently switch file search to uid 1000")); return; }
    struct worker w = {.fd = fd, .o = o};
    if (search_compile(&w.search, o->pattern, o->mode, o->match_case) < 0) {
        text_frame(fd, MSG_ERROR, w.search.error); search_free(&w.search); return;
    }
    w.directories = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    w.overlays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (size_t i = 0; i < o->noverlays; i++) {
        const struct edit_search_source *overlay = &o->overlays[i];
        if (!overlay->path || !*overlay->path || !overlay->text) {
            warning(&w, _("Invalid open-document overlay"), EINVAL); continue;
        }
        g_hash_table_replace(w.overlays, g_canonicalize_filename(overlay->path, NULL), (gpointer)overlay);
    }
    if (o->sources) {
        GHashTable *paths = g_hash_table_new(g_str_hash, g_str_equal);
        for (size_t i = 0; i < o->nsources; i++) {
            const struct edit_search_source *s = &o->sources[i];
            if (!s->path || !*s->path) { warning(&w, _("Open document without a path"), EINVAL); continue; }
            const struct edit_search_source *old = g_hash_table_lookup(paths, s->path);
            if (!old || (!old->text && s->text)) g_hash_table_insert(paths, (gpointer)s->path, (gpointer)s);
        }
        for (size_t i = 0; i < o->nsources && !w.stopped; i++) {
            const struct edit_search_source *s = &o->sources[i];
            if (!s->path || g_hash_table_lookup(paths, s->path) != s) continue;
            if (selected(&w, relative_path(o, s->path), base_name(s->path), 0)) scan_file(&w, s->path, s->text, s->length);
        }
        g_hash_table_unref(paths);
    } else scan_tree(&w);
    unsigned char done[33] = {MSG_DONE};
    put64(done + 1, w.stats.files); put64(done + 9, w.stats.binary);
    put64(done + 17, w.stats.skipped); put64(done + 25, w.stats.errors);
    if (!w.stopped) (void)frame(fd, done, sizeof done);
    g_hash_table_unref(w.overlays); g_hash_table_unref(w.directories); search_free(&w.search);
}

struct edit_filesearch *edit_filesearch_start(const struct edit_filesearch_options *o, char *error, size_t size)
{
    message(error, size, "");
    if (!o || !o->pattern || !*o->pattern || o->mode < SEARCH_PLAIN || o->mode > SEARCH_REGEX ||
        (o->mode == SEARCH_WORDS && !search_valid(o->pattern, o->mode))) {
        message(error, size, _("Invalid or empty search pattern")); return NULL;
    }
    if (strlen(o->pattern) > FRAME_MAX || !valid_globs(o->include) || !valid_globs(o->exclude)) {
        message(error, size, _("Malformed glob filter or pattern too long")); return NULL;
    }
    if ((!o->sources && (!o->root || !*o->root)) || o->nsources > SOURCE_MAX || o->noverlays > SOURCE_MAX ||
        (o->nsources && !o->sources) || (o->noverlays && !o->overlays)) {
        message(error, size, _("Choose a search folder or open-document scope")); return NULL;
    }
    struct edit_filesearch *s = calloc(1, sizeof *s);
    if (!s) { message(error, size, _("No memory")); return NULL; }
    s->fd = -1; s->pattern = strdup(o->pattern); s->mode = o->mode; s->match_case = o->match_case;
    if (!s->pattern) { free(s); message(error, size, _("No memory")); return NULL; }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) {
        message(error, size, strerror(errno)); edit_filesearch_free(s); return NULL;
    }
    s->pid = fork();
    if (s->pid < 0) {
        message(error, size, strerror(errno)); close(pair[0]); close(pair[1]); edit_filesearch_free(s); return NULL;
    }
    if (!s->pid) { close(pair[0]); run_worker(pair[1], o); close(pair[1]); _exit(0); }
    close(pair[1]); s->fd = pair[0];
    int flags = fcntl(s->fd, F_GETFL);
    if (flags < 0 || fcntl(s->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        message(error, size, strerror(errno)); edit_filesearch_free(s); return NULL;
    }
    return s;
}
static void reap(struct edit_filesearch *s)
{
    if (s->pid > 0) {
        int status;
        pid_t p = waitpid(s->pid, &status, WNOHANG);
        if (p == s->pid || (p < 0 && errno == ECHILD)) s->pid = 0;
    }
}
static void stop_worker(struct edit_filesearch *s)
{
    if (s->pid > 0) {
        (void)kill(s->pid, SIGKILL);
        while (waitpid(s->pid, NULL, 0) < 0 && errno == EINTR) {}
        s->pid = 0;
    }
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
}
static int parse_frame(struct edit_filesearch *s, const unsigned char *p, size_t n)
{
    if (p[0] == MSG_MATCH) {
        if (n < 82 || p[1] > 1 || get32(p + 2) == 0 || (uint64_t)get32(p + 2) + get32(p + 6) != n - 82 ||
            get64(p + 10) > get64(p + 18) || get64(p + 18) > FILE_MAX || !get64(p + 26) || !get64(p + 34)) return -1;
        size_t pn = get32(p + 2), vn = get32(p + 6);
        if (memchr(p + 82, 0, pn + vn) || s->count == RESULTS_MAX ||
            n + sizeof(struct edit_filematch) > RESULTS_BYTES - s->bytes) return -1;
        if (s->count == s->capacity) {
            size_t cap = s->capacity ? s->capacity * 2 : 128;
            struct edit_filematch *m = realloc(s->matches, cap * sizeof *m);
            if (!m) return -1;
            s->matches = m; s->capacity = cap;
        }
        struct edit_filematch m = {0};
        m.path = strndup((const char *)p + 82, pn); m.preview = strndup((const char *)p + 82 + pn, vn);
        if (!m.path || !m.preview) { free(m.path); free(m.preview); return -1; }
        m.start = (size_t)get64(p + 10); m.end = (size_t)get64(p + 18);
        m.line = (size_t)get64(p + 26); m.column = (size_t)get64(p + 34);
        memcpy(m.digest, p + 42, 20); memcpy(m.text_digest, p + 62, 20);
        m.from_buffer = p[1]; m.checked = 1;
        s->matches[s->count++] = m; s->bytes += n + sizeof m;
    } else if (p[0] == MSG_WARNING || p[0] == MSG_ERROR) {
        if (n == 1 || memchr(p + 1, 0, n - 1)) return -1;
        char *out = p[0] == MSG_ERROR ? s->error : s->warning;
        size_t size = p[0] == MSG_ERROR ? sizeof s->error : sizeof s->warning;
        snprintf(out, size, "%.*s", (int)(n - 1), p + 1);
        if (p[0] == MSG_ERROR) s->done = 1;
    } else if (p[0] == MSG_DONE) {
        if (n != 33) return -1;
        s->stats.files = (size_t)get64(p + 1); s->stats.binary = (size_t)get64(p + 9);
        s->stats.skipped = (size_t)get64(p + 17); s->stats.errors = (size_t)get64(p + 25);
        s->saw_done = s->done = 1;
    } else return -1;
    return 0;
}
int edit_filesearch_read(struct edit_filesearch *s)
{
    if (!s) return -1;
    reap(s);
    if (s->done) return *s->error ? -1 : 0;
    size_t budget = READ_BUDGET;
    int changed = 0;
    while (!s->done) {
        while (s->used >= 4) {
            size_t n = get32(s->input);
            if (!n || n > FRAME_MAX) goto malformed;
            if (s->used < n + 4) break;
            if (parse_frame(s, s->input + 4, n)) goto malformed;
            s->used -= n + 4;
            memmove(s->input, s->input + n + 4, s->used);
            changed = 1;
            if (s->done) break;
        }
        if (s->done || !budget) break;
        size_t room = sizeof s->input - s->used;
        if (room > budget) room = budget;
        ssize_t n = recv(s->fd, s->input + s->used, room, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) {
            if (!s->saw_done) message(s->error, sizeof s->error, _("File search worker stopped before completing"));
            s->done = 1; changed = 1; break;
        }
        s->used += (size_t)n; budget -= (size_t)n;
    }
    reap(s);
    if (s->done) stop_worker(s);
    return *s->error ? -1 : changed;
malformed:
    message(s->error, sizeof s->error, _("Malformed or oversized file search result")); s->done = 1;
    stop_worker(s);
    return -1;
}
int edit_filesearch_fd(const struct edit_filesearch *s) { return s ? s->fd : -1; }
int edit_filesearch_done(const struct edit_filesearch *s) { return !s || s->done; }
const char *edit_filesearch_error(const struct edit_filesearch *s) { return s ? s->error : _("No search"); }
const char *edit_filesearch_warning(const struct edit_filesearch *s) { return s ? s->warning : ""; }
size_t edit_filesearch_count(const struct edit_filesearch *s) { return s ? s->count : 0; }
struct edit_filematch *edit_filesearch_match(struct edit_filesearch *s, size_t i) { return s && i < s->count ? &s->matches[i] : NULL; }
struct edit_filesearch_stats edit_filesearch_stats(const struct edit_filesearch *s)
{
    return s ? s->stats : (struct edit_filesearch_stats){0};
}
void edit_filesearch_free(struct edit_filesearch *s)
{
    if (!s) return;
    stop_worker(s);
    for (size_t i = 0; i < s->count; i++) { free(s->matches[i].path); free(s->matches[i].preview); }
    free(s->matches); free(s->pattern); free(s);
}

/* One-file preparation is explicit, separate from the asynchronous scan. The
 * privileged UI never opens a path here, even for snapshot revalidation. */
static int recv_exact(int fd, void *data, size_t n)
{
    unsigned char *p = data;
    while (n) {
        struct pollfd pollfd = {fd, POLLIN, 0};
        int rc;
        do rc = poll(&pollfd, 1, 10000); while (rc < 0 && errno == EINTR);
        if (rc <= 0) return rc == 0 ? ETIMEDOUT : errno;
        ssize_t k = recv(fd, p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return EIO;
        p += k; n -= (size_t)k;
    }
    return 0;
}
static int user_snapshot(const char *path, char **text, size_t *len, struct file_format *format, uint8_t digest[20])
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) return errno;
    pid_t pid = fork();
    if (pid < 0) { int e = errno; close(pair[0]); close(pair[1]); return e; }
    if (!pid) {
        close(pair[0]); close_inherited(pair[1]);
        unsigned char header[44] = {0};
        char *content = NULL; size_t n = 0; struct file_format f = {0}; uint8_t d[20] = {0}; int flags = 0;
        int err = drop_user();
        if (!err) err = load_snapshot(path, 1, &content, &n, &f, d, &flags);
        if (!err && ((flags & FILE_BROKEN) || binary_text(content, n))) err = EILSEQ;
        put32(header, (uint32_t)err); put64(header + 4, n);
        put32(header + 12, (uint32_t)f.encoding); put32(header + 16, (uint32_t)f.bom); put32(header + 20, (uint32_t)f.eol);
        memcpy(header + 24, d, 20);
        if (!send_all(pair[1], header, sizeof header) && !err) (void)send_all(pair[1], content, n);
        free(content); close(pair[1]); _exit(0);
    }
    close(pair[1]);
    unsigned char header[44];
    int err = recv_exact(pair[0], header, sizeof header);
    char *content = NULL;
    if (!err) err = (int)get32(header);
    size_t n = 0;
    if (!err) {
        if (get64(header + 4) > FILE_MAX || get32(header + 12) >= ENC_COUNT || get32(header + 16) > 1 || get32(header + 20) > EOL_CR) err = EPROTO;
        else {
            n = (size_t)get64(header + 4); content = malloc(n + 1);
            if (!content) err = ENOMEM;
            else err = recv_exact(pair[0], content, n);
        }
    }
    close(pair[0]);
    if (err) (void)kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    if (err) { free(content); return err; }
    content[n] = '\0'; *text = content; *len = n;
    *format = (struct file_format){(int)get32(header + 12), (int)get32(header + 16), (int)get32(header + 20)};
    memcpy(digest, header + 24, 20);
    return 0;
}
int edit_filesearch_prepare(struct edit_filesearch *s, const char *path, const char *replace,
                            const char *current, size_t len, struct edit_filesearch_plan *plan, char *error, size_t size)
{
    message(error, size, "");
    if (!plan) return EINVAL;
    memset(plan, 0, sizeof *plan);
    if (!s || !path || !replace || !s->done || *s->error) {
        message(error, size, _("Wait for a successful search before replacing")); return EINVAL;
    }
    struct edit_filematch *first = NULL;
    size_t count = 0;
    for (size_t i = 0; i < s->count; i++) if (s->matches[i].checked && !strcmp(s->matches[i].path, path)) {
        if (!first) first = &s->matches[i];
        count++;
    }
    if (!first) { message(error, size, _("No checked matches for this file")); return ENOENT; }
    if (!!current != first->from_buffer) { message(error, size, _("Open-document snapshot must be replaced in its buffer")); return EINVAL; }
    char *owned = NULL;
    struct file_format format = {ENC_UTF8, 0, EOL_LF};
    uint8_t digest[20], td[20];
    int err = 0;
    if (current) {
        if (len > FILE_MAX) err = EFBIG;
        else checksum(current, len, digest);
    } else {
        err = user_snapshot(path, &owned, &len, &format, digest);
        current = owned;
    }
    if (!err) {
        checksum(current, len, td);
        if (memcmp(first->digest, digest, 20) || memcmp(first->text_digest, td, 20)) err = ESTALE;
    }
    struct buffer b = {0}; struct search search = {0};
    struct replacement *r = NULL;
    if (!err && (buffer_init(&b) < 0 || buffer_set(&b, current, len) < 0)) err = ENOMEM;
    if (!err && search_compile(&search, s->pattern, s->mode, s->match_case) < 0) err = EINVAL;
    if (!err && !(r = calloc(count, sizeof *r))) err = ENOMEM;
    size_t used = 0, previous = 0, output_len = len;
    for (size_t i = 0; i < s->count && !err; i++) {
        struct edit_filematch *m = &s->matches[i];
        if (!m->checked || strcmp(m->path, path)) continue;
        size_t start, end;
        if (memcmp(m->digest, digest, 20) || memcmp(m->text_digest, td, 20) || m->from_buffer != first->from_buffer ||
            m->start < previous || m->end < m->start || m->end > len ||
            !search_find(&search, &b, m->start, 0, 0, len, &start, &end) || start != m->start || end != m->end) { err = ESTALE; break; }
        size_t rn;
        char *replacement = search_replacement(&search, &b, replace, (int)used + 1, &rn);
        if (!replacement) { err = ENOMEM; break; }
        if (rn > FILE_MAX - (output_len - (end - start))) { free(replacement); err = EFBIG; break; }
        output_len = output_len - (end - start) + rn;
        r[used++] = (struct replacement){start, end - start, replacement, rn}; previous = end;
    }
    if (!err && buffer_replace_many(&b, r, used) < 0) err = ENOMEM;
    if (!err) {
        plan->length = buffer_length(&b); plan->text = buffer_dup(&b, 0, plan->length); plan->path = strdup(path);
        if (!plan->text || !plan->path) err = ENOMEM;
        else {
            plan->replacements = used; plan->format = format; plan->from_buffer = first->from_buffer;
            memcpy(plan->digest, digest, 20); memcpy(plan->text_digest, td, 20);
        }
    }
    for (size_t i = 0; i < used; i++) free((char *)r[i].text);
    free(r); search_free(&search); buffer_free(&b); free(owned);
    if (err) {
        edit_filesearch_plan_free(plan);
        message(error, size, err == ESTALE ? _("File or buffer changed since the search; search again") : strerror(err));
    }
    return err;
}
int edit_filesearch_plan_validate(const struct edit_filesearch_plan *p, const char *current, size_t len)
{
    if (!p || !p->path || !p->text || !!current != p->from_buffer) return EINVAL;
    uint8_t digest[20], td[20];
    char *owned = NULL; struct file_format f;
    int err = 0;
    if (current) {
        if (len > FILE_MAX) return EFBIG;
        checksum(current, len, digest);
    } else {
        err = user_snapshot(p->path, &owned, &len, &f, digest); current = owned;
    }
    if (!err) {
        checksum(current, len, td);
        if (memcmp(p->digest, digest, 20) || memcmp(p->text_digest, td, 20)) err = ESTALE;
    }
    free(owned); return err;
}
void edit_filesearch_plan_free(struct edit_filesearch_plan *p)
{
    if (p) { free(p->path); free(p->text); memset(p, 0, sizeof *p); }
}
