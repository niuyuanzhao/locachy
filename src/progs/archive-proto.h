/*
 * archive-proto.h: between archive (the screen and keyboard; root) and
 * archive-worker (the archives: libarchive, or 7-Zip where libarchive cannot,
 * running as the user, uid 1000, with no_new_privs and an empty network
 * namespace). Archives are parsed only in archive-worker: a malformed or
 * hostile one can at most take over a process with the user's file access.
 *
 * One worker runs one operation, its argv (archive-worker OP ...). It writes
 * lines to its stdout, fields separated by tabs, each field escaped
 * (archive_escape); nothing it says is trusted by archive:
 *   F FORMAT FILTERS BACKEND HEADERS READONLY VOLUMES PACKED STEM
 *                           what the archive is: libarchive's names, "7-Zip" or
 *                           "libarchive", its file list encrypted, read-only,
 *                           volumes and their bytes, its name without suffixes
 *   E PATH TYPE SIZE MTIME MODE USER GROUP LINK ENCRYPTED   an entry (TYPE: d f l h o)
 *   P BYTES PATH            progress: bytes done so far, the entry at it
 *   Q password [again]      asks: archive answers "pPASSWORD" or "c"
 *   Q exists PATH ISDIR     asks: "o" overwrite, "O" all, "s" skip, "S" all,
 *                           "rNAME" another name (not for a folder), "c" cancel
 *   W PATH MESSAGE          an entry that failed (test, extract)
 *   D PATH                  done: where the files went, or the new archive
 *   H MD5 SHA1 SHA256       the archive file's hashes
 *   X MESSAGE               the operation failed
 * Entry paths go back to the worker (argv) as they came, escaped.
 * Its exit status: 0 done, 1 failed (X said why), 2 cancelled, 3 no or wrong
 * password.
 */
#ifndef LOCACHY_ARCHIVE_PROTO_H
#define LOCACHY_ARCHIVE_PROTO_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define ARCHIVE_WORKER_PATH "/usr/local/lib/locachy/archive-worker"
#define ARCHIVE_USER 1000          /* the user it runs as */
#define ARCHIVE_7Z "/usr/bin/7z"   /* 7-Zip (the 7zip optional component) */
/* Where previews are extracted, in the user's home (containers see it). */
#define ARCHIVE_PREVIEW_DIR ".cache/locachy/archive"
#define ARCHIVE_LINE 16384         /* a message line at most */

enum { WORKER_DONE, WORKER_FAILED, WORKER_CANCELLED, WORKER_NO_PASSWORD };

#ifndef N_
#define N_(s) (s)
#endif

/* What a new archive can be (Ark's Type list, with the levels of its
 * libarchive and libzip plugins); names as shared-mime-info's comments. */
struct archive_type {
    const char *suffix, *name;
    int lmin, lmax, ldef; /* compression levels; lmax 0: none to choose */
    int password;         /* 1: AES-256 (libarchive), 2: 7-Zip's (with -H too) */
};
static const struct archive_type archive_types[] = {
    { ".tar.gz", N_("Tar archive (gzip-compressed)"), 1, 9, 6, 0 },
    { ".tar.bz2", N_("Tar archive (bzip-compressed)"), 1, 9, 9, 0 },
    { ".tar.xz", N_("Tar archive (XZ-compressed)"), 0, 9, 6, 0 },
    { ".tar.zst", N_("Tar archive (Zstandard-compressed)"), 1, 22, 3, 0 },
    { ".tar.lz4", N_("Tar archive (LZ4-compressed)"), 1, 9, 1, 0 },
    { ".tar.lzma", N_("Tar archive (LZMA-compressed)"), 0, 9, 6, 0 },
    { ".tar.lz", N_("Tar archive (lzip-compressed)"), 0, 9, 6, 0 },
    { ".tar.Z", N_("Tar archive (compressed)"), 0, 0, 0, 0 },
    { ".tar", N_("Tar archive"), 0, 0, 0, 0 },
    { ".zip", N_("Zip archive"), 0, 9, 6, 1 },
    { ".7z", N_("7-zip archive"), 1, 9, 5, 2 },
    { NULL, NULL, 0, 0, 0, 0 },
};

/* The suffixes archives have, longest first where one ends another. */
static const char *const archive_suffixes[] = {
    ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz4", ".tar.lzma", ".tar.lz", ".tar.Z",
    ".tar.lzo", ".tar.lrz", ".tgz", ".tbz2", ".tbz", ".txz", ".tzst", ".tlz", ".taz", ".tzo",
    ".tar", ".zip", ".7z", ".rar", ".cpio", ".iso", ".xar", ".cab", ".lha", ".lzh", ".ar",
    ".deb", ".rpm", ".jar", ".apk", ".cbz", ".cbr", ".cb7", ".cbt", ".epub", ".appimage",
    ".gz", ".bz2", ".xz", ".zst", ".lz4", ".lzma", ".lz", ".Z", ".lzo", NULL
};

static inline int archive_ends_with(const char *s, const char *end)
{
    size_t n = strlen(s), m = strlen(end);
    return n > m && strcasecmp(s + n - m, end) == 0;
}

/* name in dir, or another as KIO suggests one: "name (1)", "name (2).tar.gz".
 * 0, or -1: too long. */
static inline int archive_suggest_name(const char *dir, const char *name, char *out, size_t size)
{
    char path[4096];
    struct stat st;
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return -1;
    if (lstat(path, &st) != 0) return snprintf(out, size, "%s", name) < (int)size ? 0 : -1;
    const char *ext = NULL;
    for (int i = 0; archive_suffixes[i] && !ext; i++) {
        if (archive_ends_with(name, archive_suffixes[i])) ext = name + strlen(name) - strlen(archive_suffixes[i]);
    }
    if (!ext) {
        ext = strrchr(name, '.');
        if (!ext || ext == name) ext = name + strlen(name);
    }
    for (int i = 1; i < 10000; i++) {
        if (snprintf(out, size, "%.*s (%d)%s", (int)(ext - name), name, i, ext) >= (int)size ||
            snprintf(path, sizeof path, "%s/%s", dir, out) >= (int)sizeof path) {
            return -1;
        }
        if (lstat(path, &st) != 0) return 0;
    }
    return -1;
}

/* What files of these names are compressed into (Ark: the common start of
 * their names without their extensions; "Archive" for several with less
 * than five characters in common), without a suffix. */
static inline void archive_name_for(const char *const *names, int n, const char *several, char *out, size_t size)
{
    size_t common = 0;
    for (int i = 0; i < n; i++) {
        char base[256];
        snprintf(base, sizeof base, "%s", names[i]);
        for (int strip = 0; strip < 2; strip++) {
            char *dot = strrchr(base, '.');
            if (!dot || dot == base) break;
            *dot = '\0';
            if (!archive_ends_with(base, ".tar")) break;
        }
        if (i == 0) {
            snprintf(out, size, "%s", base);
            common = strlen(out);
        }
        size_t k = 0;
        while (k < common && base[k] == out[k]) k++;
        common = k;
        out[common] = '\0';
    }
    if (n > 1 && common < 5) snprintf(out, size, "%s", several);
}

/* s with backslash, tab, newline and other control characters escaped (\\
 * \t \n \xHH); s is cut where out would overflow. */
static inline void archive_escape(const char *s, char *out, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    for (; *s && n + 5 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\' || c == '\t' || c == '\n') {
            out[n++] = '\\';
            out[n++] = c == '\\' ? '\\' : c == '\t' ? 't' : 'n';
        } else if (c < 0x20 || c == 0x7f) {
            out[n++] = '\\';
            out[n++] = 'x';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        } else {
            out[n++] = (char)c;
        }
    }
    if (size) out[n] = '\0';
}

static inline int archive_hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The escaped s back as it was, in place. */
static inline void archive_unescape(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p != '\\' || !p[1]) {
            *o++ = *p;
            continue;
        }
        p++;
        if (*p == 't') {
            *o++ = '\t';
        } else if (*p == 'n') {
            *o++ = '\n';
        } else if (*p == 'x' && archive_hex(p[1]) >= 0 && archive_hex(p[2]) >= 0) {
            *o++ = (char)(archive_hex(p[1]) * 16 + archive_hex(p[2]));
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* The escaped s as text to show: control characters as '?', so that what
 * the worker said cannot move the cursor or change colours. */
static inline void archive_display(const char *s, char *out, size_t size)
{
    size_t n = 0;
    for (; *s && n + 1 < size; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            if (*s == 'x' && archive_hex(s[1]) >= 0 && archive_hex(s[2]) >= 0) s += 2;
            out[n++] = *s == '\\' ? '\\' : '?';
        } else {
            out[n++] = (unsigned char)*s < 0x20 || *s == 0x7f ? '?' : *s;
        }
    }
    if (size) out[n] = '\0';
}

#endif
