/*
 * rc.conf and other NAME="VALUE" files.  conf_read is intentionally the
 * reader used by existing programs; the writers below are the one shared
 * transaction protocol used by C, Python and the shell client.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base.h"
#include "config.h"

#define RC_LINE 512

int conf_read(const char *path, void (*fn)(const char *name, const char *value, void *arg), void *arg)
{
    char line[RC_LINE];
    const char *file = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    FILE *f = fopen(path, "re");

    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (strchr(line, '\n') == NULL && !feof(f)) {
            say("%s: a line is longer than %d bytes; left out", file, RC_LINE - 1);
            int c;
            while ((c = getc(f)) != '\n' && c != EOF) {
            }
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        char *s = line + strspn(line, " \t");
        if (*s == '\0' || *s == '#') {
            continue;
        }
        char *name = s;
        s += strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_");
        char *value = s + 1;
        if (s == name || *s != '=') {
            say("%s: cannot read \"%s\"", file, line);
            continue;
        }
        *s = '\0';
        char *end, *rest;
        if (*value == '"') {
            end = strchr(++value, '"');
            rest = end != NULL ? end + 1 : NULL;
        } else {
            end = rest = value + strcspn(value, " \t#");
        }
        if (rest != NULL) {
            rest += strspn(rest, " \t");
        }
        if (rest == NULL || (*rest != '\0' && *rest != '#')) {
            say("%s: cannot read the value of %s", file, name);
            continue;
        }
        *end = '\0';
        fn(name, value, arg);
    }
    fclose(f);
    return 0;
}

static void find_machine_key(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "machine") == 0) {
        snprintf(arg, 64, "%s", value);
    }
}

struct machine_pass {
    void (*fn)(const char *name, const char *value, void *arg);
    void *arg;
};

static void forward_machine_kv(const char *name, const char *value, void *arg)
{
    struct machine_pass *mp = arg;
    if (strcmp(name, "description") != 0 && strcmp(name, "machine") != 0) {
        mp->fn(name, value, mp->arg);
    }
}

int rc_read(void (*fn)(const char *name, const char *value, void *arg), void *arg)
{
    char mach[64] = "";
    if (conf_read(RC_CONF, find_machine_key, mach) < 0) {
        return -1;
    }
    if (mach[0] && strspn(mach, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == strlen(mach)) {
        char mpath[256];
        struct machine_pass mp = { fn, arg };
        snprintf(mpath, sizeof mpath, "%s/%s.conf", MACHINES_DIR, mach);
        conf_read(mpath, forward_machine_kv, &mp);
    }
    return conf_read(RC_CONF, fn, arg);
}

struct path_info {
    char dir[PATH_MAX];
    char name[NAME_MAX + 1];
    char path[PATH_MAX];
    char lock[PATH_MAX];
};

static int target_info(const char *path, struct path_info *out)
{
    if (!path || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1] || strlen(slash + 1) > NAME_MAX ||
        !strcmp(slash + 1, ".") || !strcmp(slash + 1, "..")) {
        errno = EINVAL;
        return -1;
    }
    char parent[PATH_MAX];
    size_t plen = (size_t)(slash - path);
    if (plen == 0) plen = 1;
    if (plen >= sizeof parent) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(parent, path, plen);
    parent[plen] = '\0';
    if (!realpath(parent, out->dir)) {
        return -1;
    }
    if (snprintf(out->name, sizeof out->name, "%s", slash + 1) >= (int)sizeof out->name ||
        snprintf(out->path, sizeof out->path, "%s/%s", out->dir, out->name) >= (int)sizeof out->path ||
        snprintf(out->lock, sizeof out->lock, "%s/%s.lock", out->dir, out->name) >= (int)sizeof out->lock) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int open_config_lock(const struct path_info *info)
{
    int fd = open(info->lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid()) {
        close(fd); errno = EINVAL; return -1;
    }
    if (fchmod(fd, 0600) < 0 || flock(fd, LOCK_EX) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int read_target(const struct path_info *info, char **data, size_t *length,
                       int *exists, struct stat *before)
{
    *data = NULL;
    *length = 0;
    *exists = 0;
    if (lstat(info->path, before) < 0) {
        if (errno == ENOENT) return 0;
        return -1;
    }
    if (!S_ISREG(before->st_mode)) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(info->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat opened;
    if (fstat(fd, &opened) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    if (opened.st_dev != before->st_dev || opened.st_ino != before->st_ino) {
        close(fd); errno = ESTALE; return -1;
    }
    size_t cap = opened.st_size > 0 && (uintmax_t)opened.st_size < SIZE_MAX - 1 ?
                 (size_t)opened.st_size + 1 : 4096;
    char *buf = malloc(cap ? cap : 1);
    if (!buf) { close(fd); return -1; }
    size_t used = 0;
    for (;;) {
        if (used == cap) {
            size_t next = cap < 4096 ? 4096 : cap > SIZE_MAX / 2 ? SIZE_MAX : cap * 2;
            if (next <= cap) { free(buf); close(fd); errno = EFBIG; return -1; }
            char *larger = realloc(buf, next);
            if (!larger) { int saved = errno; free(buf); close(fd); errno = saved; return -1; }
            buf = larger;
            cap = next;
        }
        ssize_t n = read(fd, buf + used, cap - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { int saved = errno; free(buf); close(fd); errno = saved; return -1; }
        if (!n) break;
        used += (size_t)n;
    }
    close(fd);
    if (memchr(buf, '\0', used)) { free(buf); errno = EINVAL; return -1; }
    buf[used] = '\0';
    *data = buf;
    *length = used;
    *exists = 1;
    return 0;
}

static int same_file(const struct stat *a, const struct stat *b)
{
#ifdef __APPLE__
    int times = a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
                a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    int times = a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
    return times && a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_mode == b->st_mode && a->st_uid == b->st_uid && a->st_gid == b->st_gid &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime;
}

static int write_published(const struct path_info *info, const char *data, size_t length,
                           int existed, const struct stat *before)
{
    struct stat now;
    if (lstat(info->path, &now) < 0) {
        if (existed || errno != ENOENT) return -1;
    } else if (!existed || !same_file(before, &now)) {
        errno = ESTALE;
        return -1;
    }
    int dirfd = open(info->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) return -1;
    static unsigned serial;
    char tmp[PATH_MAX];
    int fd = -1;
    for (unsigned tries = 0; tries < 100; tries++) {
        if (snprintf(tmp, sizeof tmp, ".%s.tmp.%ld.%u", info->name, (long)getpid(), serial++) >= (int)sizeof tmp) {
            close(dirfd); errno = ENAMETOOLONG; return -1;
        }
        fd = openat(dirfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST) { close(dirfd); return -1; }
    }
    if (fd < 0) { close(dirfd); errno = EEXIST; return -1; }
    int saved = 0;
    size_t done = 0;
    while (!saved && done < length) {
        ssize_t n = write(fd, data + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { saved = errno ? errno : EIO; break; }
        done += (size_t)n;
    }
    if (!saved && existed && fchown(fd, before->st_uid, before->st_gid) < 0) saved = errno;
    if (!saved && fchmod(fd, existed ? before->st_mode & 07777 : 0644) < 0) saved = errno;
    if (!saved && fsync(fd) < 0) saved = errno;
    if (close(fd) < 0 && !saved) saved = errno;
    if (!saved) {
        if (fstatat(dirfd, info->name, &now, AT_SYMLINK_NOFOLLOW) < 0) {
            if (existed || errno != ENOENT) saved = ESTALE;
        } else if (!existed || !same_file(before, &now)) {
            saved = ESTALE;
        }
    }
    if (!saved && renameat(dirfd, tmp, dirfd, info->name) < 0) saved = errno;
    if (!saved && fsync(dirfd) < 0) saved = errno;
    if (saved) unlinkat(dirfd, tmp, 0);
    close(dirfd);
    if (saved) { errno = saved; return -1; }
    return 0;
}

static int transaction_inner(const char *path, const char *expected, size_t expected_length,
                             int expected_exists, conf_transform transform, void *arg)
{
    struct path_info info;
    if (target_info(path, &info) < 0) return -1;
    int lock = open_config_lock(&info);
    if (lock < 0) return -1;
    char *original = NULL, *replacement = NULL;
    size_t length = 0, replacement_length = 0;
    int exists = 0, result = -1;
    struct stat before = { 0 };
    if (read_target(&info, &original, &length, &exists, &before) < 0) goto done;
    if (expected_exists != -1 && (exists != expected_exists ||
        (exists && (length != expected_length || memcmp(original, expected, length) != 0)))) {
        errno = ESTALE;
        goto done;
    }
    int action = transform(original ? original : "", length, exists, &replacement, &replacement_length, arg);
    if (action < 0) goto done;
    if (action == 1) { result = 0; goto done; }
    if (action != 0 || (replacement_length && !replacement) ||
        (replacement && memchr(replacement, '\0', replacement_length))) {
        errno = EINVAL;
        goto done;
    }
    result = write_published(&info, replacement ? replacement : "", replacement_length, exists, &before);
done:
    free(original);
    free(replacement);
    { int saved = errno; flock(lock, LOCK_UN); close(lock); errno = saved; }
    return result;
}

int conf_transaction(const char *path, conf_transform transform, void *arg)
{
    if (!transform) { errno = EINVAL; return -1; }
    return transaction_inner(path, NULL, 0, -1, transform, arg);
}

struct batch_arg { const struct conf_change *changes; size_t count; };
struct buffer { char *data; size_t length, capacity; };

static int buffer_add(struct buffer *b, const char *data, size_t n)
{
    if (n > SIZE_MAX - b->length - 1) { errno = EOVERFLOW; return -1; }
    size_t need = b->length + n + 1;
    if (need > b->capacity) {
        size_t cap = b->capacity ? b->capacity : 256;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) { cap = need; break; }
            cap *= 2;
        }
        char *p = realloc(b->data, cap);
        if (!p) return -1;
        b->data = p; b->capacity = cap;
    }
    memcpy(b->data + b->length, data, n);
    b->length += n; b->data[b->length] = '\0';
    return 0;
}

static int valid_name(const char *name)
{
    if (!name || !name[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    return 1;
}

static int change_index(const struct conf_change *changes, size_t count, const char *name, size_t length)
{
    for (size_t i = 0; i < count; i++)
        if (strlen(changes[i].name) == length && !memcmp(changes[i].name, name, length)) return (int)i;
    return -1;
}

static int batch_transform(const char *original, size_t length, int exists, char **replacement,
                           size_t *replacement_length, void *arg)
{
    struct batch_arg *batch = arg;
    struct buffer out = { 0 };
    unsigned char *done = calloc(batch->count ? batch->count : 1, 1);
    if (!done) return -1;
    for (size_t i = 0; i < batch->count; i++) {
        if (!valid_name(batch->changes[i].name) ||
            (batch->changes[i].value && strpbrk(batch->changes[i].value, "\"\r\n"))) {
            free(done); free(out.data); errno = EINVAL; return -1;
        }
        for (size_t j = i + 1; j < batch->count; j++)
            if (!strcmp(batch->changes[i].name, batch->changes[j].name)) {
                free(done); free(out.data); errno = EINVAL; return -1;
            }
    }
    size_t at = 0;
    while (at < length) {
        size_t end = at;
        while (end < length && original[end] != '\n') end++;
        size_t line_end = end < length ? end + 1 : end;
        size_t s = at;
        while (s < end && (original[s] == ' ' || original[s] == '\t')) s++;
        size_t name_end = s;
        while (name_end < end && ((original[name_end] >= 'A' && original[name_end] <= 'Z') ||
               (original[name_end] >= 'a' && original[name_end] <= 'z') ||
               (original[name_end] >= '0' && original[name_end] <= '9') || original[name_end] == '_')) name_end++;
        int idx = (name_end > s && name_end < end && original[name_end] == '=') ?
                  change_index(batch->changes, batch->count, original + s, name_end - s) : -1;
        if (idx < 0) {
            if (buffer_add(&out, original + at, line_end - at) < 0) goto fail;
        } else if (!done[idx]) {
            done[idx] = 1;
            if (batch->changes[idx].value) {
                if (buffer_add(&out, batch->changes[idx].name, strlen(batch->changes[idx].name)) < 0 ||
                    buffer_add(&out, "=\"", 2) < 0 ||
                    buffer_add(&out, batch->changes[idx].value, strlen(batch->changes[idx].value)) < 0 ||
                    buffer_add(&out, "\"\n", 2) < 0) goto fail;
            }
        }
        at = line_end;
    }
    if (out.length && out.data[out.length - 1] != '\n')
        if (buffer_add(&out, "\n", 1) < 0) goto fail;
    for (size_t i = 0; i < batch->count; i++) if (!done[i] && batch->changes[i].value) {
        if (buffer_add(&out, batch->changes[i].name, strlen(batch->changes[i].name)) < 0 ||
            buffer_add(&out, "=\"", 2) < 0 ||
            buffer_add(&out, batch->changes[i].value, strlen(batch->changes[i].value)) < 0 ||
            buffer_add(&out, "\"\n", 2) < 0) goto fail;
    }
    free(done);
    if (!exists && out.length == 0) {
        free(out.data);
        return 1; /* only removals from an absent file: nothing to create */
    }
    *replacement = out.data; *replacement_length = out.length;
    return 0;
fail:
    free(done); free(out.data); return -1;
}

int conf_update(const char *path, const struct conf_change *changes, size_t count)
{
    if (count && !changes) { errno = EINVAL; return -1; }
    struct batch_arg arg = { changes, count };
    return conf_transaction(path, batch_transform, &arg);
}

int rc_update(const struct conf_change *changes, size_t count)
{
    return conf_update(RC_CONF, changes, count);
}

int conf_compare_update(const char *path, const char *expected, size_t expected_length,
                        const struct conf_change *changes, size_t count)
{
    if ((expected_length && !expected) || (count && !changes)) { errno = EINVAL; return -1; }
    struct batch_arg arg = { changes, count };
    return transaction_inner(path, expected, expected_length, expected ? 1 : 0, batch_transform, &arg);
}

struct exchange_arg { const char *data; size_t length; };
static int exchange_transform(const char *original, size_t length, int exists, char **replacement,
                              size_t *replacement_length, void *arg)
{
    (void)original; (void)length; (void)exists;
    struct exchange_arg *x = arg;
    *replacement = x->length ? malloc(x->length) : NULL;
    if (x->length && !*replacement) return -1;
    if (x->length) memcpy(*replacement, x->data, x->length);
    *replacement_length = x->length;
    return 0;
}

int conf_compare_exchange(const char *path, const char *expected, size_t expected_length,
                          const char *replacement, size_t replacement_length)
{
    if ((expected_length && !expected) || (replacement_length && !replacement)) { errno = EINVAL; return -1; }
    struct exchange_arg arg = { replacement, replacement_length };
    return transaction_inner(path, expected, expected_length, expected ? 1 : 0, exchange_transform, &arg);
}

/* Compatibility wrapper. It now has the shared lock and durable publish. */
int conf_set(const char *path, const char *name, const char *value)
{
    struct conf_change change = { name, value };
    return conf_update(path, &change, 1);
}

int rc_set(const char *name, const char *value)
{
    return conf_set(RC_CONF, name, value);
}
