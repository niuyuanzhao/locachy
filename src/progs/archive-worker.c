/*
 * archive-worker: one operation on an archive for archive (archive-proto.h),
 * as the user in its sandbox. libarchive reads and writes what it can, as
 * Ark's libarchive plugin does: its extraction flags and path rules, the
 * overwrite question, adding and removing by writing the archive again.
 * 7-Zip (ARCHIVE_7Z, the 7zip optional component) does what libarchive
 * cannot: encrypted 7z and RAR, and 7z with a password or an encrypted file
 * list. A password reaches 7-Zip on its standard input, never in argv.
 *   archive-worker list ARCHIVE
 *   archive-worker extract [-p] [-s] [-n NAME] [-b BASE] ARCHIVE DEST [ENTRY...]
 *       -p keep the paths, -s a subfolder as Ark's "Extract here" decides,
 *       -n into subfolder NAME, -b BASE/ taken off the paths
 *   archive-worker test ARCHIVE
 *   archive-worker create [-l LEVEL] [-v BYTES] [-P] [-H] ARCHIVE DIR FILE...
 *       FILE relative to DIR; -P a password (the first line of stdin),
 *       -H the file list encrypted too (7z), -v volumes of BYTES
 *   archive-worker add [-i INSIDE] ARCHIVE DIR FILE...
 *   archive-worker delete ARCHIVE ENTRY...
 *   archive-worker hash ARCHIVE
 *   archive-worker remove DIR      (a preview's folder, in ARCHIVE_PREVIEW_DIR)
 * ENTRY is a path as list gave it (escaped).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <openssl/evp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "archive-proto.h"

#ifdef ARCHIVE_7Z_TEST_PATH
#undef ARCHIVE_7Z
#define ARCHIVE_7Z ARCHIVE_7Z_TEST_PATH
#endif

#define MAX_VOLUMES 999
#define BLOCK 65536

static const char *archive_path;
static char *vols[MAX_VOLUMES + 1];
static int nvols;
static char password[1024];
static int have_password, password_refused, password_asks;
static volatile sig_atomic_t stop;
static char temp_path[PATH_MAX]; /* removed if the operation does not finish */
static long long done_bytes;

static void cleanup_output(void);

static void on_term(int sig)
{
    (void)sig;
    stop = 1;
}

/* ---- Messages ---- */

/* A line of fields (NULL ends them), each escaped. */
static void emit(const char *type, ...)
{
    static char field[ARCHIVE_LINE];
    va_list ap;
    va_start(ap, type);
    fputs(type, stdout);
    for (const char *f = va_arg(ap, const char *); f; f = va_arg(ap, const char *)) {
        archive_escape(f, field, sizeof field);
        putchar('\t');
        fputs(field, stdout);
    }
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

__attribute__((format(printf, 1, 2), noreturn))
static void fail(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (temp_path[0]) unlink(temp_path);
    cleanup_output();
    emit("X", msg, NULL);
    exit(WORKER_FAILED);
}

__attribute__((noreturn))
static void finish(int status)
{
    if (temp_path[0]) unlink(temp_path);
    cleanup_output();
    exit(status);
}

/* snprintf, where a cut path would be a wrong one: the operation fails. */
__attribute__((format(printf, 3, 4)))
static void path_fmt(char *out, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size) fail("a name or path is too long");
}

static void check_stop(void)
{
    if (stop) finish(WORKER_CANCELLED);
}

/* archive's answer to a question: a line, without its newline. */
static void answer(char *out, size_t size)
{
    if (!fgets(out, (int)size, stdin)) {
        snprintf(out, size, "c");
    }
    out[strcspn(out, "\n")] = '\0';
}

static int ask_password(void)
{
    char line[sizeof password + 2];
    emit("Q", "password", password_asks++ ? "again" : NULL, NULL);
    answer(line, sizeof line);
    if (line[0] != 'p') {
        password_refused = 1;
        return 0;
    }
    path_fmt(password, sizeof password, "%s", line + 1);
    have_password = 1;
    return 1;
}

static void progress(const char *path, long long add)
{
    static long long last;
    done_bytes += add;
    if (done_bytes - last >= 256 * 1024 || add == 0) {
        char n[32];
        snprintf(n, sizeof n, "%lld", done_bytes);
        emit("P", n, path, NULL);
        last = done_bytes;
    }
}

/* ---- Names ---- */

static char *absolute(const char *path)
{
    char cwd[PATH_MAX], *p = NULL;
    if (path[0] == '/') {
        p = strdup(path);
    } else if (getcwd(cwd, sizeof cwd) && asprintf(&p, "%s/%s", cwd, path) < 0) {
        p = NULL;
    }
    if (!p) fail("out of memory");
    return p;
}


static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int ends_with(const char *s, const char *end)
{
    return archive_ends_with(s, end);
}

/* The volume suffix of name (".001", ".part1.rar"): its length, or 0. */
static size_t volume_suffix(const char *name)
{
    size_t n = strlen(name);
    if (n > 4 && name[n - 4] == '.' && isdigit((unsigned char)name[n - 3]) &&
        isdigit((unsigned char)name[n - 2]) && isdigit((unsigned char)name[n - 1])) {
        return 4;
    }
    if (ends_with(name, ".rar")) {
        size_t i = n - 4;
        while (i > 0 && isdigit((unsigned char)name[i - 1])) i--;
        if (i < n - 4 && i >= 5 && strncasecmp(name + i - 5, ".part", 5) == 0) {
            return n - (i - 5) - 4; /* ".partN", the ".rar" stays */
        }
    }
    return 0;
}

/* The archive's name without its archive suffixes (Ark's completeBaseName):
 * "photos" for photos.tar.gz, photos.7z.001 or photos.part1.rar. */
static void stem(const char *path, char *out, size_t size)
{
    snprintf(out, size, "%s", base_name(path));
    size_t v = volume_suffix(out), n = strlen(out);
    if (v == 4) {
        out[n - 4] = '\0';
    } else if (v) {
        memmove(out + n - 4 - v, out + n - 4, 5);
    }
    for (int i = 0; archive_suffixes[i]; i++) {
        if (ends_with(out, archive_suffixes[i])) {
            out[strlen(out) - strlen(archive_suffixes[i])] = '\0';
            return;
        }
    }
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}

static void suggest_name(const char *dir, const char *name, char *out, size_t size)
{
    if (archive_suggest_name(dir, name, out, size) < 0) fail("a name or path is too long");
}

/* The volumes of archive_path, in order: name.001 name.002 ..., or
 * name.part1.rar name.part2.rar ..., or name.rar name.r00 name.r01 ...; just
 * the archive otherwise. */
static void find_volumes(void)
{
    char base[PATH_MAX], path[PATH_MAX];
    struct stat st;
    size_t n = strlen(archive_path), v = volume_suffix(archive_path);
    nvols = 0;
    if (v == 4) {
        snprintf(base, sizeof base, "%.*s", (int)(n - 4), archive_path);
        for (int i = 1; i <= MAX_VOLUMES; i++) {
            path_fmt(path, sizeof path, "%s.%03d", base, i);
            if (stat(path, &st) != 0) break;
            vols[nvols++] = strdup(path);
        }
    } else if (v) {
        size_t digits = v - 5;
        snprintf(base, sizeof base, "%.*s", (int)(n - 4 - digits), archive_path);
        for (int i = 1; i <= MAX_VOLUMES; i++) {
            path_fmt(path, sizeof path, "%s%0*d.rar", base, (int)digits, i);
            if (stat(path, &st) != 0) break;
            vols[nvols++] = strdup(path);
        }
    } else if (ends_with(archive_path, ".rar")) {
        vols[nvols++] = strdup(archive_path);
        snprintf(base, sizeof base, "%.*s", (int)(n - 4), archive_path);
        for (int i = 0; i < 100; i++) {
            path_fmt(path, sizeof path, "%s.r%02d", base, i);
            if (stat(path, &st) != 0) break;
            vols[nvols++] = strdup(path);
        }
    }
    if (nvols == 0) {
        vols[nvols++] = strdup(archive_path);
    }
    vols[nvols] = NULL;
}

/* An entry's path as Ark takes it: no "./" or "/" at the start, none at the
 * end; "/" and "//" (static libraries) are not entries: 0 then. */
static int clean_path(const char *in, char *out, size_t size)
{
    if (strcmp(in, "/") == 0 || strcmp(in, "//") == 0) return 0;
    while (in[0] == '.' && in[1] == '/') in += 2;
    while (in[0] == '/') in++;
    snprintf(out, size, "%s", in[0] ? in : ".");
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
    return 1;
}

/* path is entry, or inside it. */
static int under(const char *path, const char *entry)
{
    size_t n = strlen(entry);
    return strncmp(path, entry, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static int chosen(const char *path, char **entries, int n)
{
    if (n == 0) return 1;
    for (int i = 0; i < n; i++) {
        if (under(path, entries[i])) return 1;
    }
    return 0;
}

static int mkdirs(const char *path)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            if (mkdir(p, 0755) != 0 && errno != EEXIST) return -1;
            *s = '/';
        }
    }
    return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* ---- 7-Zip ---- */

static int have_7z(void)
{
    return access(ARCHIVE_7Z, X_OK) == 0;
}

/* 7-Zip with argv (after the program), in dir (NULL: here), the password on
 * its standard input; each line of its output to on_line. Its exit status. */
static int run_7z(const char *const *args, const char *dir, void (*on_line)(char *, void *), void *arg)
{
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) < 0 || pipe2(out, O_CLOEXEC) < 0) fail("pipe: %s", strerror(errno));
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        if (dir && chdir(dir) != 0) _exit(127);
        const char *argv[64];
        int n = 0;
        argv[n++] = ARCHIVE_7Z;
        for (int i = 0; args[i] && n < 63; i++) argv[n++] = args[i];
        argv[n] = NULL;
        execv(ARCHIVE_7Z, (char *const *)argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) fail("fork: %s", strerror(errno));
    /* One line: 7-Zip asks once (it does not ask again to verify). */
    if (have_password) {
        size_t n = strlen(password);
        if (write(in[1], password, n) != (ssize_t)n || write(in[1], "\n", 1) != 1) {}
    }
    close(in[1]);
    FILE *f = fdopen(out[0], "r");
    char line[ARCHIVE_LINE];
    while (f && fgets(line, sizeof line, f)) {
        if (stop) kill(pid, SIGTERM);
        line[strcspn(line, "\r\n")] = '\0';
        if (on_line) on_line(line, arg);
    }
    if (f) fclose(f);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    check_stop();
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

struct sz_out {
    int wrong_password;
    char error[512];
};

/* 7-Zip's lines that matter in any operation: its errors. */
static void sz_errors(char *line, struct sz_out *o)
{
    if (strstr(line, "Wrong password") || strstr(line, "Can not open encrypted archive")) {
        o->wrong_password = 1;
    }
    if ((strncmp(line, "ERROR: ", 7) == 0 || strncmp(line, "ERRORS:", 7) == 0) && !o->error[0]) {
        snprintf(o->error, sizeof o->error, "%s", line + (line[6] == ' ' ? 7 : 0));
    }
}

/* 7-Zip again until the password works or the user gives up: the status of
 * the last run. */
static int run_7z_password(const char *const *args, const char *dir, void (*on_line)(char *, void *),
                           struct sz_out *o)
{
    for (;;) {
        memset(o, 0, sizeof *o);
        int rc = run_7z(args, dir, on_line, o);
        if (!o->wrong_password) return rc;
        if (!ask_password()) finish(WORKER_NO_PASSWORD);
    }
}

/* ---- Reading with libarchive ---- */

static const char *on_passphrase(struct archive *a, void *arg)
{
    (void)a;
    (void)arg;
    return !password_refused && ask_password() ? password : NULL;
}

/* A file that is only compressed (gzip, xz...): one entry, the file as it
 * was. Ark's libarchive plugin reads such types (by their names) with
 * libarchive's raw format alone, every other one with all formats but it. */
static int compressed_file(void)
{
    static const char *const comp[] = { ".gz", ".bz2", ".xz", ".zst", ".lz4", ".lzma", ".lz", ".Z", ".lzo", NULL };
    char name[PATH_MAX];
    snprintf(name, sizeof name, "%s", base_name(archive_path));
    for (int i = 0; comp[i]; i++) {
        if (ends_with(name, comp[i])) {
            name[strlen(name) - strlen(comp[i])] = '\0';
            return !ends_with(name, ".tar");
        }
    }
    return 0;
}

static struct archive *open_reader(void)
{
    struct archive *a = archive_read_new();
    if (!a) fail("out of memory");
    archive_read_support_filter_all(a);
    if (compressed_file()) archive_read_support_format_raw(a);
    else archive_read_support_format_all(a);
    archive_read_set_passphrase_callback(a, NULL, on_passphrase);
    if (have_password) archive_read_add_passphrase(a, password);
    if (archive_read_open_filenames(a, (const char **)vols, BLOCK) != ARCHIVE_OK) {
        fail("%s", archive_error_string(a));
    }
    return a;
}

/* The next entry of a: ARCHIVE_OK, ARCHIVE_EOF or ARCHIVE_FATAL. */
static int next_entry(struct archive *a, struct archive_entry **e)
{
    for (;;) {
        int r = archive_read_next_header(a, e);
        if (r == ARCHIVE_RETRY) continue;
        return r == ARCHIVE_WARN ? ARCHIVE_OK : r;
    }
}

static int writable_format(int format)
{
    int base = format & ARCHIVE_FORMAT_BASE_MASK;
    return base == ARCHIVE_FORMAT_TAR || base == ARCHIVE_FORMAT_ZIP || base == ARCHIVE_FORMAT_7ZIP;
}

/* libarchive cannot take the password of these (7-Zip can). */
static int sevenzip_only(int format)
{
    int base = format & ARCHIVE_FORMAT_BASE_MASK;
    return base == ARCHIVE_FORMAT_7ZIP || base == ARCHIVE_FORMAT_RAR || base == ARCHIVE_FORMAT_RAR_V5;
}

/* A file that is only compressed: its one entry is named as the file was
 * before (Ark: uncompressedFileName). */
static void raw_name(char *out, size_t size)
{
    static const char *const comp[] = { ".gz", ".bz2", ".xz", ".zst", ".lz4", ".lzma", ".lz", ".Z", ".lzo", NULL };
    snprintf(out, size, "%s", base_name(archive_path));
    for (int i = 0; comp[i]; i++) {
        if (ends_with(out, comp[i])) {
            out[strlen(out) - strlen(comp[i])] = '\0';
            return;
        }
    }
    snprintf(out + strlen(out), size - strlen(out), ".out");
}

/* What an archive is, from its entries' headers. */
struct scan {
    int format, raw, encrypted, headers, entries, sevenzip;
    char format_name[64], filters[128];
    char top[PATH_MAX];  /* the first part of the entries' paths... */
    int single_top, top_dir; /* ...the same for all, and a folder */
    long long bytes;
};

static void count_entry(struct scan *s, const char *path, int dir, long long size, int enc)
{
    size_t n = strcspn(path, "/");
    if (s->entries == 0) {
        snprintf(s->top, sizeof s->top, "%.*s", (int)n, path);
        s->single_top = 1;
    } else if (strlen(s->top) != n || strncmp(s->top, path, n) != 0) {
        s->single_top = 0;
    }
    s->top_dir |= path[n] == '/' || dir;
    s->entries++;
    s->bytes += size > 0 ? size : 0;
    s->encrypted |= enc;
}

static void emit_entry(const char *path, char type, long long size, time_t mtime, unsigned mode,
                       const char *user, const char *group, const char *link, int enc)
{
    char t[2] = { type, 0 }, sz[32], mt[32], md[16];
    snprintf(sz, sizeof sz, "%lld", size);
    snprintf(mt, sizeof mt, "%lld", (long long)mtime);
    snprintf(md, sizeof md, "%o", mode & 07777);
    emit("E", path, t, sz, mt, md, user ? user : "", group ? group : "", link ? link : "", enc ? "1" : "0", NULL);
}

/* An entry's path for an archive's entry e (a compressed file's: the file's
 * own name). */
static int entry_path(struct archive_entry *e, char *out, size_t size)
{
    if (compressed_file()) {
        raw_name(out, size);
        return 1;
    }
    const char *p = archive_entry_pathname_utf8(e);
    if (!p) p = archive_entry_pathname(e);
    return p && clean_path(p, out, size);
}

static char entry_type(struct archive_entry *e)
{
    mode_t t = archive_entry_filetype(e);
    return archive_entry_hardlink(e) ? 'h' : t == AE_IFDIR ? 'd' : t == AE_IFREG ? 'f' : t == AE_IFLNK ? 'l' : 'o';
}

/* 7-Zip's -slt listing: blocks of "Key = value" lines. */
struct sz_list {
    struct sz_out out;
    struct scan *s;
    int show, any;
    char path[PATH_MAX], attr[64], user[64], group[64], link[PATH_MAX];
    long long size;
    time_t mtime;
    int folder, enc;
};

static void sz_flush(struct sz_list *l)
{
    char path[PATH_MAX];
    if (!l->any || !clean_path(l->path, path, sizeof path)) {
        l->any = 0;
        return;
    }
    int dir = l->folder || l->attr[0] == 'D';
    unsigned mode = dir ? 0755 : 0644;
    const char *unix_mode = strchr(l->attr, ' ');
    if (unix_mode && strlen(unix_mode + 1) >= 10) {
        static const char bits[] = "rwxrwxrwx";
        mode = 0;
        for (int i = 0; i < 9; i++) mode = (mode << 1) | (unix_mode[2 + i] == bits[i]);
        dir |= unix_mode[1] == 'd';
    }
    char type = dir ? 'd' : unix_mode && unix_mode[1] == 'l' ? 'l' : 'f';
    count_entry(l->s, path, dir, l->size, l->enc);
    if (l->show) emit_entry(path, type, l->size, l->mtime, mode, l->user, l->group, l->link, l->enc);
    memset(l->path, 0, sizeof l->path);
    l->attr[0] = l->user[0] = l->group[0] = l->link[0] = '\0';
    l->size = 0;
    l->mtime = 0;
    l->folder = l->enc = l->any = 0;
}

static void sz_list_line(char *line, void *arg)
{
    struct sz_list *l = arg;
    sz_errors(line, &l->out);
    char *eq = strstr(line, " = ");
    if (!line[0]) {
        sz_flush(l);
        return;
    }
    if (!eq) return;
    *eq = '\0';
    const char *key = line, *v = eq + 3;
    if (strcmp(key, "Path") == 0) {
        sz_flush(l);
        snprintf(l->path, sizeof l->path, "%s", v);
        l->any = 1;
    } else if (strcmp(key, "Size") == 0) {
        l->size = atoll(v);
    } else if (strcmp(key, "Modified") == 0) {
        struct tm tm = { 0 };
        if (strptime(v, "%Y-%m-%d %H:%M:%S", &tm)) {
            tm.tm_isdst = -1;
            l->mtime = mktime(&tm);
        }
    } else if (strcmp(key, "Attributes") == 0) {
        snprintf(l->attr, sizeof l->attr, "%s", v);
    } else if (strcmp(key, "Folder") == 0) {
        l->folder = v[0] == '+';
    } else if (strcmp(key, "Encrypted") == 0) {
        l->enc = v[0] == '+';
    } else if (strcmp(key, "User") == 0) {
        snprintf(l->user, sizeof l->user, "%s", v);
    } else if (strcmp(key, "Group") == 0) {
        snprintf(l->group, sizeof l->group, "%s", v);
    } else if (strcmp(key, "Symbolic Link") == 0 || strcmp(key, "Link") == 0) {
        snprintf(l->link, sizeof l->link, "%s", v);
    }
}

static void scan_7z(struct scan *s, int show)
{
    static struct sz_list l;
    const char *args[] = { "l", "-slt", "-ba", "-sccUTF-8", "--", archive_path, NULL };
    memset(&l, 0, sizeof l);
    l.s = s;
    l.show = show;
    if (s->headers && !have_password && !ask_password()) finish(WORKER_NO_PASSWORD);
    for (;;) {
        memset(&l.out, 0, sizeof l.out);
        int rc = run_7z(args, NULL, sz_list_line, &l);
        sz_flush(&l);
        if (!l.out.wrong_password) {
            if (rc != 0 && rc != 1) fail("7-Zip: %s", l.out.error[0] ? l.out.error : "cannot read the archive");
            break;
        }
        if (s->entries > 0 || !ask_password()) finish(WORKER_NO_PASSWORD);
    }
    s->sevenzip = 1;
}

/* What the archive is; with show, its entries said (E). An encrypted file
 * list is 7-Zip's to read (the password asked first), or it fails. */
static void scan(struct scan *s, int show)
{
    struct archive_entry *e;
    int r;
    memset(s, 0, sizeof *s);
    s->raw = compressed_file();
    struct archive *a = open_reader();
    while ((r = next_entry(a, &e)) == ARCHIVE_OK) {
        char path[PATH_MAX];
        check_stop();
        if (!entry_path(e, path, sizeof path)) continue;
        char type = entry_type(e);
        long long size = archive_entry_size_is_set(e) ? (long long)archive_entry_size(e) : 0;
        int enc = archive_entry_is_encrypted(e) != 0;
        count_entry(s, path, type == 'd', size, enc);
        if (show) {
            const char *link = archive_entry_hardlink(e) ? archive_entry_hardlink(e) : archive_entry_symlink(e);
            emit_entry(path, type, size, archive_entry_mtime(e), archive_entry_mode(e),
                       archive_entry_uname(e), archive_entry_gname(e), link, enc);
        }
    }
    s->format = archive_format(a);
    snprintf(s->format_name, sizeof s->format_name, "%s", archive_format_name(a) ? archive_format_name(a) : "");
    for (int i = 0; i < archive_filter_count(a); i++) {
        if (archive_filter_code(a, i) == ARCHIVE_FILTER_NONE) continue;
        size_t n = strlen(s->filters);
        snprintf(s->filters + n, sizeof s->filters - n, "%s%s", n ? "," : "", archive_filter_name(a, i));
    }
    if (r == ARCHIVE_FATAL) {
        const char *why = archive_error_string(a);
        int headers = why && strstr(why, "ncrypt") && sevenzip_only(s->format);
        char msg[512];
        snprintf(msg, sizeof msg, "%s", why ? why : "cannot read the archive");
        archive_read_free(a);
        if (!headers) fail("%s", msg);
        if (!have_7z()) fail("The file list of this archive is encrypted: 7-Zip opens it (settings, Optional Components)");
        s->headers = 1;
        s->entries = 0;
        scan_7z(s, show);
        return;
    }
    archive_read_free(a);
    s->sevenzip = s->encrypted && sevenzip_only(s->format) && have_7z();
}

static int scan_readonly(const struct scan *s)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", archive_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    int writable = s->sevenzip ? (s->format & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_7ZIP
                               : writable_format(s->format) && !(s->encrypted && sevenzip_only(s->format));
    return !writable || s->raw || nvols > 1 || access(archive_path, W_OK) != 0 ||
           access(slash ? dir : ".", W_OK) != 0;
}

static void op_list(void)
{
    struct scan s;
    struct stat st;
    char vols_n[16], packed[32], name[NAME_MAX + 1];
    long long bytes = 0;
    scan(&s, 1);
    for (int i = 0; i < nvols; i++) {
        if (stat(vols[i], &st) == 0) bytes += st.st_size;
    }
    snprintf(vols_n, sizeof vols_n, "%d", nvols);
    snprintf(packed, sizeof packed, "%lld", bytes);
    stem(archive_path, name, sizeof name);
    emit("F", s.format_name, s.filters, s.sevenzip ? "7-Zip" : "libarchive", s.headers ? "1" : "0",
         scan_readonly(&s) ? "1" : "0", vols_n, packed, name, NULL);
}

/* ---- Extracting ---- */

struct extract {
    int preserve;          /* the entries' folders made too */
    const char *base;      /* taken off the paths (Ark: the root node) */
    char **entries;        /* the chosen ones (and what is in them); none: all */
    int nentries;
    int overwrite_all, skip_all, count, failed;
};

/* Where the entry at path goes, relative to the destination; 0: nowhere. */
static int target_path(const struct extract *x, const char *path, int dir, char *out, size_t size)
{
    if (x->base && x->base[0] && under(path, x->base)) {
        if (path[strlen(x->base)] == '\0') return 0;
        path += strlen(x->base) + 1;
    }
    if (!x->preserve) {
        if (dir) return 0;
        path = base_name(path);
    }
    snprintf(out, size, "%s", path);
    return out[0] && strcmp(out, ".") != 0;
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *f)
{
    (void)st;
    (void)flag;
    (void)f;
    remove(path);
    return 0;
}

static void remove_tree(const char *path)
{
    nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* Whether target, which may be there already, is written: the user is asked
 * (Ark's overwrite query: overwrite, skip, all of either, another name;
 * a folder over a file is not renamed). Cancelling ends the operation. */
static int resolve(struct extract *x, char *target, size_t size, int dir)
{
    struct stat st;
    for (;;) {
        if (lstat(target, &st) != 0 || (dir && S_ISDIR(st.st_mode))) return 1;
        if (x->skip_all) return 0;
        if (x->overwrite_all) return 1;
        char a[PATH_MAX + 2];
        emit("Q", "exists", target, dir ? "1" : "0", NULL);
        answer(a, sizeof a);
        if (a[0] == 'o' || a[0] == 'O') {
            x->overwrite_all = a[0] == 'O';
            return 1;
        } else if (a[0] == 's' || a[0] == 'S') {
            x->skip_all = a[0] == 'S';
            return 0;
        } else if (a[0] == 'r' && !dir && a[1] && !strchr(a + 1, '/') && strcmp(a + 1, ".") != 0 &&
                   strcmp(a + 1, "..") != 0) {
            char *slash = strrchr(target, '/');
            size_t keep = slash ? (size_t)(slash - target) + 1 : 0;
            snprintf(target + keep, size - keep, "%s", a + 1);
        } else if (a[0] != 'r') {
            finish(WORKER_CANCELLED);
        }
    }
}

/* Where the files go: dest; with name, its subfolder name; with auto_sub,
 * as Ark's "Extract here" does: one folder in the archive comes out as it
 * is (into "name (1)" when there is one of its name already), more than
 * one thing into a folder named as the archive, one file as it is. */
static void destination(const char *dest, int auto_sub, const char *name, const struct scan *s,
                        char *out, size_t size)
{
    char sub[NAME_MAX + 1] = "", want[NAME_MAX + 1] = "";
    struct stat st;
    snprintf(out, size, "%s", dest);
    if (mkdirs(dest) != 0) fail("%s: %s", dest, strerror(errno));
    if (name && name[0]) {
        if (strchr(name, '/') || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) fail("%s: not a folder name", name);
        snprintf(out, size, "%s/%s", dest, name);
        if (mkdirs(out) != 0) fail("%s: %s", out, strerror(errno));
        return;
    }
    if (!auto_sub || s->entries == 0) return;
    int rpm = strstr(s->filters, "rpm") != NULL;
    if (s->single_top && s->top_dir && !(rpm && strcmp(s->top, "usr") == 0)) {
        path_fmt(out, size, "%s/%s", dest, s->top);
        if (lstat(out, &st) != 0) {
            snprintf(out, size, "%s", dest);
            return;
        }
        path_fmt(want, sizeof want, "%s", s->top);
    } else if (s->entries > 1 || rpm) {
        stem(archive_path, want, sizeof want);
    } else {
        return;
    }
    if (!want[0]) snprintf(want, sizeof want, "%s", base_name(archive_path));
    suggest_name(dest, want, sub, sizeof sub);
    snprintf(out, size, "%s/%s", dest, sub);
    if (mkdir(out, 0755) != 0 && errno != EEXIST) fail("%s: %s", out, strerror(errno));
}

static int copy_data(struct archive *a, struct archive *w, const char *path, int disk)
{
    const void *buf;
    size_t len;
    la_int64_t off;
    for (;;) {
        check_stop();
        int r = archive_read_data_block(a, &buf, &len, &off);
        if (r == ARCHIVE_EOF) return 0;
        if (r < ARCHIVE_WARN) return -1;
        if ((disk ? archive_write_data_block(w, buf, len, off) : archive_write_data(w, buf, len)) < 0) return -2;
        progress(path, (long long)len);
    }
}

static void extract_libarchive(struct extract *x)
{
    struct archive *a = open_reader(), *w = archive_write_disk_new();
    struct archive_entry *e;
    int r;
    if (!w) fail("out of memory");
    archive_write_disk_set_standard_lookup(w);
    while ((r = next_entry(a, &e)) == ARCHIVE_OK) {
        char path[PATH_MAX], target[PATH_MAX], link[PATH_MAX], to[PATH_MAX];
        check_stop();
        if (!entry_path(e, path, sizeof path) || !chosen(path, x->entries, x->nentries)) continue;
        int dir = archive_entry_filetype(e) == AE_IFDIR;
        if (!target_path(x, path, dir, target, sizeof target) || !resolve(x, target, sizeof target, dir)) continue;
        archive_entry_copy_pathname(e, target);
        if (archive_entry_hardlink(e) && clean_path(archive_entry_hardlink(e), link, sizeof link) &&
            target_path(x, link, 0, to, sizeof to)) {
            archive_entry_copy_hardlink(e, to);
        }
        /* Ark's flags: no absolute paths, no "..", nothing through a link. */
        int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS |
                    ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS;
        if (archive_entry_sparse_count(e) > 0) flags |= ARCHIVE_EXTRACT_SPARSE;
        archive_write_disk_set_options(w, flags);
        r = archive_write_header(w, e);
        if (r == ARCHIVE_FATAL) fail("%s: %s", target, archive_error_string(w));
        if (r < ARCHIVE_WARN) {
            emit("W", path, archive_error_string(w), NULL);
            x->failed++;
            continue;
        }
        int c = archive_entry_size(e) > 0 || !archive_entry_size_is_set(e) ? copy_data(a, w, path, 1) : 0;
        if (c < 0) {
            char why[512];
            snprintf(why, sizeof why, "%s", archive_error_string(c == -1 ? a : w));
            archive_write_finish_entry(w);
            if (!dir) unlink(target);
            if (password_refused) finish(WORKER_NO_PASSWORD);
            emit("W", path, why, NULL);
            x->failed++;
            continue;
        }
        archive_write_finish_entry(w);
        x->count++;
    }
    if (r == ARCHIVE_FATAL) fail("%s", archive_error_string(a));
    archive_write_free(w);
    archive_read_free(a);
}

/* What 7-Zip put in staging (relative to here, the destination), moved to
 * where it goes, asking as extract_libarchive asks. */
static void move_staged(struct extract *x, const char *staging, const char *rel)
{
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof dir_path, "%s%s%s", staging, rel[0] ? "/" : "", rel);
    DIR *d = opendir(dir_path);
    struct dirent *de;
    while (d && (de = readdir(d)) != NULL) {
        char path[PATH_MAX], from[PATH_MAX], target[PATH_MAX];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        snprintf(path, sizeof path, "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
        path_fmt(from, sizeof from, "%s/%s", staging, path);
        if (lstat(from, &st) != 0) continue;
        int dir = S_ISDIR(st.st_mode);
        if (target_path(x, path, dir, target, sizeof target) && resolve(x, target, sizeof target, dir)) {
            struct stat old;
            if (dir && lstat(target, &old) == 0 && !S_ISDIR(old.st_mode)) unlink(target);
            if (dir ? mkdir(target, (st.st_mode & 07777) | 0700) != 0 && errno != EEXIST : rename(from, target) != 0) {
                emit("W", path, strerror(errno), NULL);
                x->failed++;
            } else if (!dir) {
                x->count++;
            }
        }
        if (dir) move_staged(x, staging, path);
    }
    if (d) closedir(d);
}

static void sz_line(char *line, void *arg)
{
    sz_errors(line, arg);
}

/* 7-Zip's argv: fixed, then the entries (escaped ones made plain). */
static const char **sz_args(const char *const *fixed, char **entries, int n)
{
    int k = 0;
    while (fixed[k]) k++;
    const char **args = calloc((size_t)(k + n + 1), sizeof *args);
    if (!args) fail("out of memory");
    memcpy(args, fixed, (size_t)k * sizeof *args);
    for (int i = 0; i < n; i++) args[k + i] = entries[i];
    return args;
}

static void extract_7z(struct extract *x, const struct scan *s)
{
    char staging[] = ".archive-XXXXXX", out_opt[sizeof staging + 2];
    struct sz_out o;
    if (!mkdtemp(staging)) fail("%s", strerror(errno));
    snprintf(out_opt, sizeof out_opt, "-o%s", staging);
    const char *fixed[] = { "x", "-y", "-bd", "-spd", "-sccUTF-8", out_opt, "--", archive_path, NULL };
    const char **args = sz_args(fixed, x->entries, x->nentries);
    if (s->encrypted && !have_password && !ask_password()) {
        remove_tree(staging);
        finish(WORKER_NO_PASSWORD);
    }
    int rc = run_7z_password(args, NULL, sz_line, &o);
    free(args);
    if (rc >= 2) {
        remove_tree(staging);
        fail("7-Zip: %s", o.error[0] ? o.error : "the archive could not be extracted");
    }
    move_staged(x, staging, "");
    remove_tree(staging);
}

static void op_extract(int argc, char **argv)
{
    struct extract x = { 0 };
    struct scan s;
    const char *name = NULL;
    char out[PATH_MAX];
    int auto_sub = 0, c;
    while ((c = getopt(argc, argv, "psn:b:")) != -1) {
        if (c == 'p') x.preserve = 1;
        else if (c == 's') auto_sub = 1;
        else if (c == 'n') name = optarg;
        else if (c == 'b') x.base = optarg;
        else fail("extract: unknown option");
    }
    if (argc - optind < 2) fail("extract: ARCHIVE DEST [ENTRY...]");
    archive_path = absolute(argv[optind]);
    x.entries = argv + optind + 2;
    x.nentries = argc - optind - 2;
    for (int i = 0; i < x.nentries; i++) archive_unescape(x.entries[i]);
    if (x.base) archive_unescape((char *)x.base);
    find_volumes();
    scan(&s, 0);
    if (s.encrypted && sevenzip_only(s.format) && !s.sevenzip) {
        fail("This archive is encrypted: 7-Zip extracts it (settings, Optional Components)");
    }
    destination(argv[optind + 1], auto_sub, name, &s, out, sizeof out);
    if (chdir(out) != 0) fail("%s: %s", out, strerror(errno));
    if (s.sevenzip) extract_7z(&x, &s);
    else extract_libarchive(&x);
    emit("D", out, NULL);
    if (x.failed) fail("%d of the files could not be extracted", x.failed);
}

/* ---- Testing ---- */

static void sz_test_line(char *line, void *arg)
{
    sz_errors(line, arg);
    if (strncmp(line, "ERROR: ", 7) == 0) emit("W", "", line + 7, NULL);
}

static void op_test(void)
{
    struct scan s;
    struct archive_entry *e;
    int r, bad = 0, n = 0;
    scan(&s, 0);
    if (s.encrypted && sevenzip_only(s.format) && !s.sevenzip) {
        fail("This archive is encrypted: 7-Zip tests it (settings, Optional Components)");
    }
    if (s.sevenzip) {
        const char *args[] = { "t", "-bd", "-sccUTF-8", "--", archive_path, NULL };
        struct sz_out o;
        if (s.encrypted && !have_password && !ask_password()) finish(WORKER_NO_PASSWORD);
        if (run_7z_password(args, NULL, sz_test_line, &o) != 0) {
            fail("7-Zip: %s", o.error[0] ? o.error : "the archive is damaged");
        }
        emit("D", archive_path, NULL);
        return;
    }
    struct archive *a = open_reader();
    while ((r = next_entry(a, &e)) == ARCHIVE_OK) {
        char path[PATH_MAX];
        const void *buf;
        size_t len;
        la_int64_t off;
        if (!entry_path(e, path, sizeof path) || archive_entry_filetype(e) != AE_IFREG) continue;
        n++;
        while ((r = archive_read_data_block(a, &buf, &len, &off)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
            check_stop();
            progress(path, (long long)len);
        }
        if (r != ARCHIVE_EOF) {
            if (password_refused) finish(WORKER_NO_PASSWORD);
            emit("W", path, archive_error_string(a), NULL);
            bad++;
        }
    }
    if (r == ARCHIVE_FATAL) fail("%s", archive_error_string(a));
    archive_read_free(a);
    if (bad) fail("%d of %d files are damaged", bad, n);
    emit("D", archive_path, NULL);
}

/* ---- Writing ---- */

/* What archive_types' suffixes are to libarchive, and their other names. */
static const struct {
    const char *suffix, *type; /* the archive_types suffix it is */
    int format, filter;
} writers[] = {
    { ".tar.gz", ".tar.gz", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_GZIP },
    { ".tgz", ".tar.gz", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_GZIP },
    { ".tar.bz2", ".tar.bz2", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_BZIP2 },
    { ".tbz2", ".tar.bz2", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_BZIP2 },
    { ".tbz", ".tar.bz2", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_BZIP2 },
    { ".tar.xz", ".tar.xz", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_XZ },
    { ".txz", ".tar.xz", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_XZ },
    { ".tar.zst", ".tar.zst", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_ZSTD },
    { ".tzst", ".tar.zst", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_ZSTD },
    { ".tar.lz4", ".tar.lz4", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_LZ4 },
    { ".tar.lzma", ".tar.lzma", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_LZMA },
    { ".tlz", ".tar.lzma", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_LZMA },
    { ".tar.lz", ".tar.lz", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_LZIP },
    { ".tar.Z", ".tar.Z", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_COMPRESS },
    { ".taz", ".tar.Z", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_COMPRESS },
    { ".tar", ".tar", ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_NONE },
    { ".zip", ".zip", ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE },
    { ".7z", ".7z", ARCHIVE_FORMAT_7ZIP, ARCHIVE_FILTER_NONE },
};
#define NWRITERS ((int)(sizeof writers / sizeof writers[0]))

static const struct archive_type *type_of(const char *name)
{
    for (int i = 0; i < NWRITERS; i++) {
        if (!ends_with(name, writers[i].suffix)) continue;
        for (int t = 0; archive_types[t].suffix; t++) {
            if (strcmp(archive_types[t].suffix, writers[i].type) == 0) return &archive_types[t];
        }
    }
    return NULL;
}

static int writer_of(const char *name)
{
    for (int i = 0; i < NWRITERS; i++) {
        if (ends_with(name, writers[i].suffix)) return i;
    }
    return -1;
}

static void set_option(struct archive *w, const char *fmt, int value)
{
    char opt[96];
    snprintf(opt, sizeof opt, fmt, value);
    if (archive_write_set_options(w, opt) < ARCHIVE_WARN) fail("%s: %s", opt, archive_error_string(w));
}

/* Its compression level (-1: the format's own) and a password for what a
 * new archive of the name is to be written as. */
static void configure(struct archive *w, int format, int filter, int level, int encrypt)
{
    int base = format & ARCHIVE_FORMAT_BASE_MASK;
    if (level >= 0 && base == ARCHIVE_FORMAT_ZIP) {
        if (level == 0) {
            if (archive_write_set_options(w, "zip:compression=store") < ARCHIVE_WARN) fail("%s", archive_error_string(w));
        } else {
            set_option(w, "zip:compression-level=%d", level);
        }
    } else if (level >= 0 && base == ARCHIVE_FORMAT_7ZIP) {
        set_option(w, "7zip:compression-level=%d", level);
    } else if (level >= 0 && filter != ARCHIVE_FILTER_NONE && filter != ARCHIVE_FILTER_COMPRESS) {
        set_option(w, "compression-level=%d", level);
    }
    if (encrypt) {
        if (base != ARCHIVE_FORMAT_ZIP) fail("Only zip and 7z archives take a password");
        if (archive_write_set_options(w, "zip:encryption=aes256") < ARCHIVE_WARN ||
            archive_write_set_passphrase(w, password) != ARCHIVE_OK) {
            fail("%s", archive_error_string(w));
        }
    }
}

/* Where an archive is written: a hidden file beside it, renamed over it at
 * the end; or volumes NAME.001, NAME.002 ... of vol_size bytes each. */
static struct output {
    int fd, vol, committed;
    long long vol_size, in_vol;
    char final[PATH_MAX];
    dev_t dev;
    ino_t ino;
} out = { .fd = -1 };

static void cleanup_output(void)
{
    if (out.committed) return;
    if (out.vol_size) {
        for (int i = 1; i <= out.vol; i++) {
            char path[PATH_MAX];
            path_fmt(path, sizeof path, "%s.%03d", out.final, i);
            unlink(path);
        }
    }
}

static int next_volume(void)
{
    char path[PATH_MAX];
    struct stat st;
    if (out.fd >= 0 && (fsync(out.fd) != 0 || close(out.fd) != 0)) return -1;
    path_fmt(path, sizeof path, "%s.%03d", out.final, ++out.vol);
    out.fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    out.in_vol = 0;
    if (out.fd >= 0 && fstat(out.fd, &st) == 0) {
        out.dev = st.st_dev;
        out.ino = st.st_ino;
    }
    return out.fd < 0 ? -1 : 0;
}

static la_ssize_t out_write(struct archive *a, void *arg, const void *buf, size_t n)
{
    (void)arg;
    size_t done = 0;
    while (done < n) {
        if (out.vol_size && (out.fd < 0 || out.in_vol >= out.vol_size) && next_volume() != 0) {
            archive_set_error(a, errno, "%s", strerror(errno));
            return -1;
        }
        size_t chunk = n - done;
        if (out.vol_size && (long long)chunk > out.vol_size - out.in_vol) chunk = (size_t)(out.vol_size - out.in_vol);
        ssize_t w = write(out.fd, (const char *)buf + done, chunk);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            archive_set_error(a, errno, "%s", strerror(errno));
            return -1;
        }
        done += (size_t)w;
        out.in_vol += w;
    }
    return (la_ssize_t)n;
}

static int out_close(struct archive *a, void *arg)
{
    (void)arg;
    if (out.fd >= 0 && (fsync(out.fd) != 0 || close(out.fd) != 0)) {
        archive_set_error(a, errno, "%s", strerror(errno));
        out.fd = -1;
        return ARCHIVE_FATAL;
    }
    out.fd = -1;
    return ARCHIVE_OK;
}

static void open_output(struct archive *w, const char *final, long long vol_size)
{
    snprintf(out.final, sizeof out.final, "%s", final);
    out.vol_size = vol_size;
    if (!vol_size) {
        struct stat st;
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", final);
        *strrchr(dir, '/') = '\0';
        path_fmt(temp_path, sizeof temp_path, "%s/.%s.XXXXXX", dir, base_name(final));
        out.fd = mkostemp(temp_path, O_CLOEXEC);
        if (out.fd < 0) {
            temp_path[0] = '\0';
            fail("%s: %s", dir, strerror(errno));
        }
        if (fstat(out.fd, &st) == 0) {
            out.dev = st.st_dev;
            out.ino = st.st_ino;
        }
    }
    /* Not padded to a whole block: a compressed stream ends where it ends. */
    archive_write_set_bytes_in_last_block(w, 1);
    if (archive_write_open2(w, NULL, NULL, out_write, out_close, NULL) != ARCHIVE_OK) {
        fail("%s", archive_error_string(w));
    }
}

/* The archive written: in place of the old one (with its permissions). */
static void commit_output(struct archive *w, const char *old)
{
    struct stat st;
    if (archive_write_close(w) != ARCHIVE_OK) fail("%s", archive_error_string(w));
    archive_write_free(w);
    if (!out.vol_size) {
        mode_t mask = umask(0);
        umask(mask);
        chmod(temp_path, old && stat(old, &st) == 0 ? st.st_mode & 07777 : 0666 & ~mask);
        if (rename(temp_path, out.final) != 0) fail("%s: %s", out.final, strerror(errno));
        temp_path[0] = '\0';
    }
    out.committed = 1;
}

/* The paths written (sorted when looked up), so that the old entries of the
 * same paths are left out. */
static char **written;
static int nwritten, written_sorted;

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int was_written(const char *path)
{
    if (!written_sorted) {
        qsort(written, (size_t)nwritten, sizeof *written, by_name);
        written_sorted = 1;
    }
    return nwritten && bsearch(&path, written, (size_t)nwritten, sizeof *written, by_name) != NULL;
}

/* files (relative to here), their folders' contents too, into w, at
 * inside/ (NULL: the top). Links are stored as links. */
static void write_files(struct archive *w, char **files, int n, const char *inside)
{
    struct archive *d = archive_read_disk_new();
    if (!d) fail("out of memory");
    archive_read_disk_set_standard_lookup(d);
    archive_read_disk_set_symlink_physical(d);
    for (int i = 0; i < n; i++) {
        if (archive_read_disk_open(d, files[i]) != ARCHIVE_OK) fail("%s: %s", files[i], archive_error_string(d));
        for (;;) {
            struct archive_entry *e = archive_entry_new();
            char path[PATH_MAX], full[PATH_MAX];
            check_stop();
            int r = archive_read_next_header2(d, e);
            if (r == ARCHIVE_EOF) {
                archive_entry_free(e);
                break;
            }
            if (r < ARCHIVE_WARN) {
                emit("W", files[i], archive_error_string(d), NULL);
                archive_entry_free(e);
                if (r == ARCHIVE_FATAL) fail("%s: %s", files[i], archive_error_string(d));
                continue;
            }
            archive_read_disk_descend(d);
            const struct stat *st = archive_entry_stat(e);
            if ((st->st_dev == out.dev && st->st_ino == out.ino) ||
                !clean_path(archive_entry_pathname(e), path, sizeof path) || strcmp(path, ".") == 0) {
                archive_entry_free(e);
                continue;
            }
            path_fmt(full, sizeof full, "%s%s%s", inside && inside[0] ? inside : "", inside && inside[0] ? "/" : "", path);
            archive_entry_copy_pathname(e, full);
            char **more = realloc(written, (size_t)(nwritten + 1) * sizeof *written);
            if (!more || !(more[nwritten] = strdup(full))) fail("out of memory");
            written = more;
            nwritten++;
            written_sorted = 0;
            r = archive_write_header(w, e);
            if (r == ARCHIVE_FATAL) fail("%s: %s", full, archive_error_string(w));
            if (r < ARCHIVE_WARN) {
                emit("W", full, archive_error_string(w), NULL);
            } else if (S_ISREG(st->st_mode) && st->st_size > 0 && copy_data(d, w, full, 0) < 0) {
                fail("%s: %s", full, archive_error_string(w));
            }
            archive_entry_free(e);
        }
        archive_read_close(d);
    }
    archive_read_free(d);
}

/* ---- Creating, adding, removing ---- */

/* 7-Zip writes a 7z with a password (and its file list encrypted, with -H),
 * or volumes of it: in dir, the files relative to it. */
static void create_7z(const char *dir, char **files, int n, int level, long long vol_size, int headers)
{
    char mx[16], vol[32];
    struct sz_out o;
    const char *fixed[16];
    int k = 0;
    fixed[k++] = "a";
    fixed[k++] = "-t7z";
    fixed[k++] = "-bd";
    fixed[k++] = "-spd";
    fixed[k++] = "-sccUTF-8";
    if (level >= 0) {
        snprintf(mx, sizeof mx, "-mx=%d", level);
        fixed[k++] = mx;
    }
    if (vol_size) {
        snprintf(vol, sizeof vol, "-v%lldb", vol_size);
        fixed[k++] = vol;
    }
    if (have_password) fixed[k++] = "-p";
    if (headers) fixed[k++] = "-mhe=on";
    fixed[k++] = "--";
    fixed[k++] = archive_path;
    fixed[k] = NULL;
    const char **args = sz_args(fixed, files, n);
    int rc = run_7z(args, dir, sz_line, &o);
    free(args);
    if (rc >= 2) fail("7-Zip: %s", o.error[0] ? o.error : "the archive could not be written");
}

static void create_libarchive(const char *dir, char **files, int n, int level, long long vol_size, int encrypt);

static void op_create(int argc, char **argv)
{
    int level = -1, encrypt = 0, headers = 0, c;
    long long vol_size = 0;
    while ((c = getopt(argc, argv, "l:v:PH")) != -1) {
        if (c == 'l') level = atoi(optarg);
        else if (c == 'v') vol_size = atoll(optarg);
        else if (c == 'P') encrypt = 1;
        else if (c == 'H') headers = 1;
        else fail("create: unknown option");
    }
    if (argc - optind < 3) fail("create: ARCHIVE DIR FILE...");
    archive_path = absolute(argv[optind]);
    const char *dir = argv[optind + 1];
    char **files = argv + optind + 2;
    int n = argc - optind - 2;
    const struct archive_type *t = type_of(archive_path);
    struct stat st;
    if (!t) fail("%s: not a kind of archive that can be written", base_name(archive_path));
    if (lstat(archive_path, &st) == 0) fail("%s is there already", base_name(archive_path));
    if (level >= 0 && (level < t->lmin || level > t->lmax)) fail("compression level %d: not %d to %d", level, t->lmin, t->lmax);
    if (vol_size < 0 || (vol_size > 0 && vol_size < 65536)) fail("volumes of at least 64 KiB");
    if (encrypt) {
        answer(password, sizeof password);
        have_password = password[0] != '\0';
        if (!have_password) fail("no password");
        if (!t->password) fail("%s archives take no password", t->suffix);
    }
    if (headers && !have_password) fail("the file list is encrypted with a password");
    if (t->password == 2 && (have_password || headers)) {
        if (!have_7z()) fail("A 7z archive with a password is 7-Zip's to write (settings, Optional Components)");
        create_7z(dir, files, n, level, vol_size, headers);
    } else {
        create_libarchive(dir, files, n, level, vol_size, encrypt);
    }
    char first[PATH_MAX];
    snprintf(first, sizeof first, vol_size ? "%s.001" : "%s", archive_path);
    emit("D", first, NULL);
}

static void create_libarchive(const char *dir, char **files, int n, int level, long long vol_size, int encrypt)
{
    if (chdir(dir) != 0) fail("%s: %s", dir, strerror(errno));
    int i = writer_of(archive_path);
    struct archive *w = archive_write_new();
    if (!w || archive_write_set_format(w, writers[i].format) != ARCHIVE_OK ||
        (writers[i].filter != ARCHIVE_FILTER_NONE && archive_write_add_filter(w, writers[i].filter) < ARCHIVE_WARN)) {
        fail("%s", w ? archive_error_string(w) : "out of memory");
    }
    configure(w, writers[i].format, writers[i].filter, level, encrypt);
    open_output(w, archive_path, vol_size);
    write_files(w, files, n, NULL);
    commit_output(w, NULL);
}

/* The old archive's entries into w but those left out: written ones (add),
 * or the chosen ones and what is in them (delete). How many were left out. */
static int copy_entries(struct archive *r, struct archive_entry *e, struct archive *w, char **drop, int ndrop)
{
    int left = 0, st = ARCHIVE_OK;
    for (; st == ARCHIVE_OK; st = next_entry(r, &e)) {
        char path[PATH_MAX];
        check_stop();
        if (!entry_path(e, path, sizeof path)) continue;
        if ((ndrop && chosen(path, drop, ndrop)) || (!ndrop && was_written(path))) {
            left++;
            continue;
        }
        int h = archive_write_header(w, e);
        if (h == ARCHIVE_FATAL) fail("%s: %s", path, archive_error_string(w));
        if (h < ARCHIVE_WARN) {
            emit("W", path, archive_error_string(w), NULL);
            continue;
        }
        int c = archive_entry_size(e) > 0 ? copy_data(r, w, path, 0) : 0;
        if (c < 0) {
            if (password_refused) finish(WORKER_NO_PASSWORD);
            fail("%s: %s", path, archive_error_string(c == -1 ? r : w));
        }
    }
    if (st == ARCHIVE_FATAL) fail("%s", archive_error_string(r));
    archive_read_free(r);
    return left;
}

/* The archive written again (Ark's read-write libarchive plugin): the files
 * (relative to dir) at inside/, then the old entries; or the old entries
 * without the chosen ones. In its own format and compression; an encrypted
 * zip's entries all with its password. */
static void rewrite(const struct scan *s, const char *dir, char **files, int n, const char *inside,
                    char **drop, int ndrop)
{
    struct archive_entry *e;
    if (s->encrypted && !have_password && !ask_password()) finish(WORKER_NO_PASSWORD);
    struct archive *r = open_reader();
    int first = next_entry(r, &e);
    if (first == ARCHIVE_FATAL) fail("%s", archive_error_string(r));
    struct archive *w = archive_write_new();
    if (!w || archive_write_set_format(w, archive_format(r)) != ARCHIVE_OK) {
        fail("%s", w ? archive_error_string(w) : "out of memory");
    }
    for (int i = archive_filter_count(r) - 1; i >= 0; i--) {
        int code = archive_filter_code(r, i);
        if (code != ARCHIVE_FILTER_NONE && archive_write_add_filter(w, code) < ARCHIVE_WARN) {
            fail("%s", archive_error_string(w));
        }
    }
    configure(w, archive_format(r), ARCHIVE_FILTER_NONE, -1, s->encrypted);
    open_output(w, archive_path, 0);
    if (n) {
        if (chdir(dir) != 0) fail("%s: %s", dir, strerror(errno));
        write_files(w, files, n, inside);
    }
    int left = first == ARCHIVE_OK ? copy_entries(r, e, w, drop, ndrop) : 0;
    if (first != ARCHIVE_OK) archive_read_free(r);
    if (ndrop && left == 0) fail("nothing of that is in the archive");
    commit_output(w, archive_path);
    emit("D", archive_path, NULL);
}

/* 7-Zip adds to (or, with no files, removes from) a 7z it keeps encrypted:
 * at inside/ through links in a folder of its own. */
static void update_7z(const struct scan *s, const char *dir, char **files, int n, const char *inside,
                      char **drop, int ndrop)
{
    char staging[] = "/tmp/archive-XXXXXX", at[PATH_MAX];
    struct sz_out o;
    if (s->encrypted && !have_password && !ask_password()) finish(WORKER_NO_PASSWORD);
    const char *fixed[] = { n ? "a" : "d", "-bd", "-spd", "-sccUTF-8", s->headers ? "-mhe=on" : "-bb0", "--",
                            archive_path, NULL };
    const char **args;
    const char *in_dir = dir;
    if (n && inside && inside[0]) {
        if (!mkdtemp(staging)) fail("%s", strerror(errno));
        snprintf(at, sizeof at, "%s/%s", staging, inside);
        if (mkdirs(at) != 0) fail("%s: %s", at, strerror(errno));
        char **named = calloc((size_t)n, sizeof *named);
        if (!named) fail("out of memory");
        for (int i = 0; i < n; i++) {
            char link[PATH_MAX], target[PATH_MAX];
            path_fmt(link, sizeof link, "%s/%s", at, base_name(files[i]));
            snprintf(target, sizeof target, "%s/%s", dir, files[i]);
            if (symlink(target, link) != 0) fail("%s: %s", link, strerror(errno));
            if (asprintf(&named[i], "%s/%s", inside, base_name(files[i])) < 0) fail("out of memory");
        }
        args = sz_args(fixed, named, n);
        in_dir = staging;
    } else {
        args = sz_args(fixed, n ? files : drop, n ? n : ndrop);
    }
    int rc = run_7z_password(args, in_dir, sz_line, &o);
    free(args);
    if (in_dir == staging) remove_tree(staging);
    if (rc >= 2) fail("7-Zip: %s", o.error[0] ? o.error : "the archive could not be written");
    emit("D", archive_path, NULL);
}

static void op_add(int argc, char **argv)
{
    struct scan s;
    char *inside = NULL;
    int c;
    while ((c = getopt(argc, argv, "i:")) != -1) {
        if (c == 'i') inside = optarg;
        else fail("add: unknown option");
    }
    if (argc - optind < 3) fail("add: ARCHIVE DIR FILE...");
    if (inside) {
        char clean[PATH_MAX];
        archive_unescape(inside);
        if (strstr(inside, "..") || !clean_path(inside, clean, sizeof clean)) fail("%s: not a folder of the archive", inside);
        snprintf(inside, strlen(inside) + 1, "%s", strcmp(clean, ".") == 0 ? "" : clean);
    }
    archive_path = absolute(argv[optind]);
    find_volumes();
    scan(&s, 0);
    if (scan_readonly(&s)) fail("The archive can only be read");
    if (s.sevenzip) update_7z(&s, argv[optind + 1], argv + optind + 2, argc - optind - 2, inside, NULL, 0);
    else rewrite(&s, argv[optind + 1], argv + optind + 2, argc - optind - 2, inside, NULL, 0);
}

static void op_delete(int argc, char **argv)
{
    struct scan s;
    if (argc < 3) fail("delete: ARCHIVE ENTRY...");
    archive_path = absolute(argv[1]);
    for (int i = 2; i < argc; i++) archive_unescape(argv[i]);
    find_volumes();
    scan(&s, 0);
    if (scan_readonly(&s)) fail("The archive can only be read");
    if (s.sevenzip) update_7z(&s, NULL, NULL, 0, NULL, argv + 2, argc - 2);
    else rewrite(&s, NULL, NULL, 0, NULL, argv + 2, argc - 2);
}

/* ---- The rest ---- */

static void op_hash(void)
{
    static const char hex[] = "0123456789abcdef";
    const EVP_MD *md[3] = { EVP_md5(), EVP_sha1(), EVP_sha256() };
    EVP_MD_CTX *ctx[3];
    char text[3][EVP_MAX_MD_SIZE * 2 + 1];
    unsigned char buf[BLOCK], sum[EVP_MAX_MD_SIZE];
    int fd = open(archive_path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0) fail("%s: %s", archive_path, strerror(errno));
    for (int i = 0; i < 3; i++) {
        ctx[i] = EVP_MD_CTX_new();
        if (!ctx[i] || !EVP_DigestInit_ex(ctx[i], md[i], NULL)) fail("cannot hash");
    }
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        check_stop();
        for (int i = 0; i < 3; i++) EVP_DigestUpdate(ctx[i], buf, (size_t)n);
    }
    if (n < 0) fail("%s: %s", archive_path, strerror(errno));
    close(fd);
    for (int i = 0; i < 3; i++) {
        unsigned len = 0;
        EVP_DigestFinal_ex(ctx[i], sum, &len);
        EVP_MD_CTX_free(ctx[i]);
        for (unsigned k = 0; k < len; k++) {
            text[i][2 * k] = hex[sum[k] >> 4];
            text[i][2 * k + 1] = hex[sum[k] & 15];
        }
        text[i][2 * len] = '\0';
    }
    emit("H", text[0], text[1], text[2], NULL);
}

/* A preview's folder, and only that: in ARCHIVE_PREVIEW_DIR of the home. */
static void op_remove(const char *dir)
{
    char base[PATH_MAX], real[PATH_MAX], real_base[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(base, sizeof base, "%s/%s", home ? home : "/nonexistent", ARCHIVE_PREVIEW_DIR);
    if (!realpath(dir, real) || !realpath(base, real_base) || !under(real, real_base) || strcmp(real, real_base) == 0) {
        fail("%s: not a preview's folder", dir);
    }
    remove_tree(real);
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    setlocale(LC_ALL, "");
    umask(022);
    if (argc < 3) {
        fprintf(stderr, "usage: archive-worker list|extract|test|create|add|delete|hash|remove ...\n");
        return WORKER_FAILED;
    }
    const char *op = argv[1];
    if (strcmp(op, "list") == 0 || strcmp(op, "test") == 0 || strcmp(op, "hash") == 0) {
        archive_path = absolute(argv[2]);
        find_volumes();
        if (op[0] == 'l') op_list();
        else if (op[0] == 't') op_test();
        else op_hash();
    } else if (strcmp(op, "extract") == 0) {
        op_extract(argc - 1, argv + 1);
    } else if (strcmp(op, "create") == 0) {
        op_create(argc - 1, argv + 1);
    } else if (strcmp(op, "add") == 0) {
        op_add(argc - 1, argv + 1);
    } else if (strcmp(op, "delete") == 0) {
        op_delete(argc - 1, argv + 1);
    } else if (strcmp(op, "remove") == 0) {
        op_remove(argv[2]);
    } else {
        fail("%s: no such operation", op);
    }
    return WORKER_DONE;
}
