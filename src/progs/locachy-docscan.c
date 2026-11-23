/*
 * locachy-docscan: sandboxed document text extractor for LOCachy search.
 *
 * Usage: locachy-docscan FILE
 *
 * Reads at most 8 MiB from a regular file (never following symlinks) and writes
 * at most 1 MiB of UTF-8 text to stdout. Exits 0 on success; on failure writes
 * at most one short line to stderr and exits non-zero.
 *
 * Supported formats:
 *   - Plain text (known text/code extensions or UTF-8 / NUL-free sniff;
 *     invalid UTF-8 bytes replaced by U+FFFD).
 *   - PDF via libmupdf (compiled in by default; disable with -DDOCSCAN_NO_MUPDF
 *     when libmupdf is absent).
 *   - DOCX (word/document.xml), ODT (content.xml), EPUB (*.xhtml / *.html)
 *     via libarchive plus an XML/HTML tag stripper and entity decoder.
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
#include <grp.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef DOCSCAN_NO_MUPDF
#define HAVE_MUPDF 0
#else
#define HAVE_MUPDF 1
#include <mupdf/fitz.h>
#endif

/* Limits enforced by the search contract. */
#define MAX_INPUT_BYTES    (8u * 1024u * 1024u)
#define MAX_OUTPUT_BYTES   (1u * 1024u * 1024u)
#define MAX_MEMBER_BYTES   (16u * 1024u * 1024u)
#define MAX_UNPACKED_TOTAL (32u * 1024u * 1024u)
#define MAX_ZIP_ENTRIES    4096u
#define MAX_PDF_PAGES      2000
#define SNIFF_BYTES        8192u
#define RLIMIT_AS_BYTES    (512ull * 1024ull * 1024ull)
#define RLIMIT_CPU_SECS    10u

/* Exit codes (non-zero on any failure). */
enum {
    EXIT_OK          = 0,
    EXIT_ERR         = 1,
    EXIT_USAGE       = 2,
    EXIT_TOO_LARGE   = 3,
    EXIT_UNSUPPORTED = 4,
    EXIT_SANDBOX     = 5,
    EXIT_NOT_REGULAR = 6,
};

static const char docscan_usage[] = "usage: locachy-docscan FILE\n";

static int stderr_written;

__attribute__((format(printf, 2, 3), noreturn))
static void die(int code, const char *fmt, ...)
{
    if (!stderr_written && fmt && *fmt) {
        char line[512];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(line, sizeof line, fmt, ap);
        va_end(ap);
        if (n < 0) n = 0;
        size_t len = (size_t)n < sizeof line - 1 ? (size_t)n : sizeof line - 1;
        /* Keep stderr strictly to one short line. */
        for (size_t i = 0; i < len; i++) {
            if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';
        }
        line[len] = '\0';
        if (dprintf(STDERR_FILENO, "locachy-docscan: %s\n", line) < 0) {}
        stderr_written = 1;
    }
    _exit(code);
}

/* ---- Bounded stdout writer ---- */

struct outbuf {
    unsigned char buf[16384];
    size_t len;
    size_t total;
    int full;
    int pipe_closed;
};

static int out_flush(struct outbuf *o)
{
    size_t off = 0;
    while (off < o->len) {
        ssize_t w = write(STDOUT_FILENO, o->buf + off, o->len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) {
                o->len = 0;
                o->pipe_closed = 1;
                return -1;
            }
            die(EXIT_ERR, "write stdout: %s", strerror(errno));
        }
        if (w == 0) {
            o->len = 0;
            o->pipe_closed = 1;
            return -1;
        }
        off += (size_t)w;
    }
    o->len = 0;
    return 0;
}

/* Write one complete UTF-8 byte sequence (len 1..4) without exceeding the
 * 1 MiB output cap or splitting a multi-byte character across the cap. */
static int out_put_bytes(struct outbuf *o, const unsigned char *p, size_t n)
{
    if (o->full || o->pipe_closed || n == 0) return o->pipe_closed ? -1 : 0;
    if (n > MAX_OUTPUT_BYTES - o->total) {
        o->full = 1;
        return 0;
    }
    if (o->len + n > sizeof o->buf) {
        if (out_flush(o) < 0) return -1;
    }
    if (n > sizeof o->buf) {
        size_t off = 0;
        while (off < n) {
            size_t chunk = n - off;
            if (chunk > sizeof o->buf) chunk = sizeof o->buf;
            memcpy(o->buf, p + off, chunk);
            o->len = chunk;
            if (out_flush(o) < 0) return -1;
            off += chunk;
        }
    } else {
        memcpy(o->buf + o->len, p, n);
        o->len += n;
    }
    o->total += n;
    if (o->total >= MAX_OUTPUT_BYTES) o->full = 1;
    return 0;
}

static int out_put_byte(struct outbuf *o, unsigned char c)
{
    return out_put_bytes(o, &c, 1);
}

static int out_put_rune(struct outbuf *o, uint32_t cp)
{
    unsigned char u[4];
    size_t n;
    if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        cp = 0xFFFDu;
    }
    if (cp < 0x80u) {
        u[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800u) {
        u[0] = (unsigned char)(0xC0u | (cp >> 6));
        u[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp < 0x10000u) {
        u[0] = (unsigned char)(0xE0u | (cp >> 12));
        u[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        u[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else {
        u[0] = (unsigned char)(0xF0u | (cp >> 18));
        u[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
        u[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        u[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 4;
    }
    return out_put_bytes(o, u, n);
}

/* ---- UTF-8 validation and sanitizing ---- */

/* Validate one UTF-8 sequence starting at s[0..n-1]. Returns its byte length
 * (1..4) if valid, or 0 if invalid. */
static size_t utf8_valid_seq(const unsigned char *s, size_t n)
{
    if (n == 0) return 0;
    unsigned c = s[0];
    if (c < 0x80u) return 1;
    if (c >= 0xC2u && c <= 0xDFu) {
        if (n < 2) return 0;
        return (s[1] & 0xC0u) == 0x80u ? 2 : 0;
    }
    if (c >= 0xE0u && c <= 0xEFu) {
        if (n < 3) return 0;
        if ((s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u) return 0;
        if (c == 0xE0u && s[1] < 0xA0u) return 0; /* overlong */
        if (c == 0xEDu && s[1] >= 0xA0u) return 0; /* UTF-16 surrogates */
        return 3;
    }
    if (c >= 0xF0u && c <= 0xF4u) {
        if (n < 4) return 0;
        if ((s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u || (s[3] & 0xC0u) != 0x80u) return 0;
        if (c == 0xF0u && s[1] < 0x90u) return 0; /* overlong */
        if (c == 0xF4u && s[1] >= 0x90u) return 0; /* > U+10FFFF */
        return 4;
    }
    return 0;
}

/* Emit s[0..n-1] as valid UTF-8, replacing any invalid byte or NUL with U+FFFD. */
static int out_put_utf8_sanitized(struct outbuf *o, const unsigned char *s, size_t n)
{
    static const unsigned char fffd[3] = { 0xEF, 0xBF, 0xBD };
    size_t i = 0;
    while (i < n && !o->full && !o->pipe_closed) {
        if (s[i] == 0) {
            if (out_put_bytes(o, fffd, sizeof fffd) < 0) return -1;
            i++;
            continue;
        }
        size_t len = utf8_valid_seq(s + i, n - i);
        if (len > 0) {
            /* Batch consecutive valid non-NUL bytes for throughput. */
            size_t start = i;
            i += len;
            while (i < n && s[i] != 0) {
                size_t next = utf8_valid_seq(s + i, n - i);
                if (next == 0) break;
                i += next;
            }
            /* Emit per codepoint only near the output cap so we never split UTF-8. */
            size_t run = i - start;
            if (run <= MAX_OUTPUT_BYTES - o->total) {
                if (out_put_bytes(o, s + start, run) < 0) return -1;
            } else {
                size_t p = start;
                while (p < i && !o->full && !o->pipe_closed) {
                    size_t clen = utf8_valid_seq(s + p, i - p);
                    if (clen == 0) break;
                    if (out_put_bytes(o, s + p, clen) < 0) return -1;
                    p += clen;
                }
            }
        } else {
            if (out_put_bytes(o, fffd, sizeof fffd) < 0) return -1;
            i++;
        }
    }
    return o->pipe_closed ? -1 : 0;
}

/* ---- Sandbox & privilege drop ---- */

static void close_extra_fds(void)
{
    if (close_range(3u, ~0u, 0u) == 0) return;
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        int dfd = dirfd(d);
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
            char *end = NULL;
            errno = 0;
            long fd = strtol(de->d_name, &end, 10);
            if (errno == 0 && end && *end == '\0' && fd >= 3 && fd <= INT_MAX && fd != dfd) {
                (void)close((int)fd);
            }
        }
        closedir(d);
        return;
    }
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 65536;
    for (int fd = 3; fd < (int)max_fd; fd++) (void)close(fd);
}

static void apply_rlimit(int resource, rlim_t value, const char *name)
{
    struct rlimit rl;
    if (getrlimit(resource, &rl) == 0) {
        /* Only lower or match an existing stricter hard limit. */
        rlim_t target = value;
        if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < target) {
            target = rl.rlim_max;
        }
        rl.rlim_cur = target;
        rl.rlim_max = target;
    } else {
        rl.rlim_cur = value;
        rl.rlim_max = value;
    }
    if (setrlimit(resource, &rl) != 0) {
        die(EXIT_SANDBOX, "setrlimit(%s): %s", name, strerror(errno));
    }
}

/* Drop privileges (if root) and enter sandbox BEFORE opening the target file.
 * Returns 1 if an expected owner was recorded from root pre-stat, 0 otherwise. */
static int drop_and_sandbox(const char *path, uid_t *expect_uid, gid_t *expect_gid)
{
    int dropped = 0;
    uid_t euid = geteuid();
    uid_t ruid = getuid();

    if (ruid == 0 || euid == 0) {
        struct stat lst;
        if (lstat(path, &lst) != 0) {
            die(EXIT_ERR, "lstat: %s", strerror(errno));
        }
        if (!S_ISREG(lst.st_mode)) {
            die(EXIT_NOT_REGULAR, "not a regular file");
        }
        if (lst.st_size < 0 || (uint64_t)lst.st_size > MAX_INPUT_BYTES) {
            die(EXIT_TOO_LARGE, "file exceeds 8 MiB limit");
        }
        *expect_uid = lst.st_uid;
        *expect_gid = lst.st_gid;

        /* Never parse untrusted document bytes as UID/GID 0: if the target file
         * is owned by root, drop to nobody (65534:65534) so only world-readable
         * files can be opened and parsers always run unprivileged. */
        uid_t run_uid = lst.st_uid == 0 ? (uid_t)65534 : lst.st_uid;
        gid_t run_gid = lst.st_gid == 0 ? (gid_t)65534 : lst.st_gid;

        if (setgid(run_gid) != 0) {
            die(EXIT_SANDBOX, "setgid(%u): %s", (unsigned)run_gid, strerror(errno));
        }
        if (setgroups(0, NULL) != 0) {
            die(EXIT_SANDBOX, "setgroups(0): %s", strerror(errno));
        }
        if (setuid(run_uid) != 0) {
            die(EXIT_SANDBOX, "setuid(%u): %s", (unsigned)run_uid, strerror(errno));
        }
        if (getuid() != run_uid || geteuid() != run_uid ||
            getgid() != run_gid || getegid() != run_gid ||
            getuid() == 0 || geteuid() == 0 || getgid() == 0 || getegid() == 0) {
            die(EXIT_SANDBOX, "credential verification failed");
        }
        dropped = 1;
    }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        die(EXIT_SANDBOX, "PR_SET_NO_NEW_PRIVS: %s", strerror(errno));
    }
    apply_rlimit(RLIMIT_AS, (rlim_t)RLIMIT_AS_BYTES, "RLIMIT_AS");
    apply_rlimit(RLIMIT_CPU, (rlim_t)RLIMIT_CPU_SECS, "RLIMIT_CPU");
    close_extra_fds();
    return dropped;
}

/* ---- File classification ---- */

static const char *file_extension(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return "";
    return dot + 1;
}

static int is_text_extension(const char *ext)
{
    static const char *const exts[] = {
        "txt", "md", "markdown", "c", "h", "cc", "cpp", "hpp",
        "py", "sh", "conf", "ini", "toml", "yaml", "yml", "json",
        "csv", "tsv", "xml", "html", "htm", "tex", "log", "rs",
        "go", "js", "ts", "java", "kt", "php", "rb", "pl", "lua",
        "sql", "rst", "org", "srt", "vtt",
        NULL
    };
    if (!ext || !*ext) return 0;
    for (size_t i = 0; exts[i]; i++) {
        if (strcasecmp(ext, exts[i]) == 0) return 1;
    }
    return 0;
}

/* Sniff up to SNIFF_BYTES: no NUL bytes and predominantly valid UTF-8 and
 * printable/whitespace bytes (tolerates a truncated UTF-8 sequence at the
 * sniff window edge and a tiny fraction of stray non-UTF-8 bytes). */
static int sniff_looks_like_text(const unsigned char *buf, size_t n)
{
    if (n == 0) return 1;
    size_t limit = n < SNIFF_BYTES ? n : SNIFF_BYTES;
    if (memchr(buf, 0, limit) != NULL) return 0;

    size_t i = 0;
    size_t bad_utf8 = 0;
    size_t ctrl_bytes = 0;
    size_t total_units = 0;

    /* Skip UTF-8 BOM if present. */
    if (limit >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        i = 3;
    }

    while (i < limit) {
        unsigned c = buf[i];
        if (c < 0x80u) {
            if (c < 0x20u && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v' && c != 0x1Bu) {
                ctrl_bytes++;
            } else if (c == 0x7Fu) {
                ctrl_bytes++;
            }
            total_units++;
            i++;
            continue;
        }
        size_t len = utf8_valid_seq(buf + i, limit - i);
        if (len > 0) {
            total_units++;
            i += len;
            continue;
        }
        /* Allow a truncated valid multi-byte sequence right at the sniff boundary. */
        if (limit < n && limit - i < 4) {
            size_t full_len = utf8_valid_seq(buf + i, n - i);
            if (full_len > 0) {
                total_units++;
                break;
            }
        }
        bad_utf8++;
        total_units++;
        i++;
    }

    if (total_units == 0) return 1;
    /* Reject binary payloads with excessive control characters or invalid UTF-8. */
    if (ctrl_bytes * 20u > total_units) return 0;  /* > 5% control chars */
    if (bad_utf8 * 20u > total_units) return 0;    /* > 5% invalid UTF-8 */
    return 1;
}

static int has_pdf_header(const unsigned char *buf, size_t n)
{
    return n >= 5 && memcmp(buf, "%PDF-", 5) == 0;
}

static int has_zip_magic(const unsigned char *buf, size_t n)
{
    if (n < 4) return 0;
    return buf[0] == 'P' && buf[1] == 'K' &&
           ((buf[2] == 0x03 && buf[3] == 0x04) ||
            (buf[2] == 0x05 && buf[3] == 0x06) ||
            (buf[2] == 0x07 && buf[3] == 0x08));
}

/* ---- Plain text extraction ---- */

static int extract_plain_text(const unsigned char *buf, size_t n, struct outbuf *o)
{
    size_t start = 0;
    if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        start = 3;
    }
    return out_put_utf8_sanitized(o, buf + start, n - start);
}

/* ---- XML / HTML tag stripper + entity decoder + whitespace collapser ---- */

enum xml_state {
    XS_TEXT = 0,
    XS_LT,          /* saw '<' */
    XS_TAG,         /* inside <...> */
    XS_TAG_SQ,      /* inside '...' in tag */
    XS_TAG_DQ,      /* inside "..." in tag */
    XS_BANG,        /* saw "<!" */
    XS_COMMENT,     /* inside <!-- ... --> */
    XS_CDATA,       /* inside <![CDATA[ ... ]]> */
    XS_PI,          /* inside <? ... ?> */
    XS_ENTITY,      /* inside &...; */
};

struct xml_stripper {
    struct outbuf *out;
    enum xml_state state;
    int html_mode;          /* skip <script>/<style> bodies in EPUB HTML/XHTML */
    int skip_tag;           /* 0=none, 1=script, 2=style */
    int need_space;         /* pending collapsed whitespace */
    int emitted_any;        /* whether any non-whitespace has been emitted */
    char tag_name[32];
    size_t tag_len;
    int tag_closing;
    int tag_self_closing;
    int tag_name_done;
    char bang_buf[12];
    size_t bang_len;
    int dash_count;         /* consecutive '-' in comment or ']' in CDATA */
    int prev_qmark;         /* saw '?' inside PI */
    char ent_buf[32];
    size_t ent_len;
};

static void xml_stripper_init(struct xml_stripper *x, struct outbuf *o, int html_mode)
{
    memset(x, 0, sizeof *x);
    x->out = o;
    x->html_mode = html_mode;
}

static void xml_stripper_reset_doc(struct xml_stripper *x)
{
    struct outbuf *o = x->out;
    int html_mode = x->html_mode;
    int emitted = x->emitted_any;
    memset(x, 0, sizeof *x);
    x->out = o;
    x->html_mode = html_mode;
    x->emitted_any = emitted;
    if (emitted) x->need_space = 1;
}

static int xml_emit_rune(struct xml_stripper *x, uint32_t cp)
{
    if (x->out->full || x->out->pipe_closed) return x->out->pipe_closed ? -1 : 0;
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v' || cp == 0xA0u) {
        if (x->emitted_any) x->need_space = 1;
        return 0;
    }
    if (x->need_space) {
        if (out_put_byte(x->out, ' ') < 0) return -1;
        x->need_space = 0;
        if (x->out->full) return 0;
    }
    x->emitted_any = 1;
    return out_put_rune(x->out, cp);
}

static int xml_emit_utf8_bytes(struct xml_stripper *x, const unsigned char *p, size_t n)
{
    if (x->out->full || x->out->pipe_closed) return x->out->pipe_closed ? -1 : 0;
    if (n == 1 && (p[0] == ' ' || p[0] == '\t' || p[0] == '\n' || p[0] == '\r' || p[0] == '\f' || p[0] == '\v')) {
        if (x->emitted_any) x->need_space = 1;
        return 0;
    }
    if (n == 2 && p[0] == 0xC2u && p[1] == 0xA0u) {
        if (x->emitted_any) x->need_space = 1;
        return 0;
    }
    if (x->need_space) {
        if (out_put_byte(x->out, ' ') < 0) return -1;
        x->need_space = 0;
        if (x->out->full) return 0;
    }
    x->emitted_any = 1;
    return out_put_bytes(x->out, p, n);
}

static int xml_decode_entity(const char *s, size_t len, uint32_t *cp)
{
    if (len == 0) return 0;
    if (s[0] == '#') {
        uint64_t v = 0;
        size_t i = 1;
        if (i < len && (s[i] == 'x' || s[i] == 'X')) {
            i++;
            if (i >= len) return 0;
            for (; i < len; i++) {
                unsigned char c = (unsigned char)s[i];
                unsigned d;
                if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
                else return 0;
                v = v * 16u + d;
                if (v > 0x10FFFFu) return 0;
            }
        } else {
            if (i >= len) return 0;
            for (; i < len; i++) {
                unsigned char c = (unsigned char)s[i];
                if (c < '0' || c > '9') return 0;
                v = v * 10u + (unsigned)(c - '0');
                if (v > 0x10FFFFu) return 0;
            }
        }
        if (v == 0 || (v >= 0xD800u && v <= 0xDFFFu)) {
            *cp = 0xFFFDu;
        } else {
            *cp = (uint32_t)v;
        }
        return 1;
    }
    if (len == 3 && strncmp(s, "amp", 3) == 0)  { *cp = '&';  return 1; }
    if (len == 2 && strncmp(s, "lt", 2) == 0)   { *cp = '<';  return 1; }
    if (len == 2 && strncmp(s, "gt", 2) == 0)   { *cp = '>';  return 1; }
    if (len == 4 && strncmp(s, "quot", 4) == 0) { *cp = '"';  return 1; }
    if (len == 4 && strncmp(s, "apos", 4) == 0) { *cp = '\''; return 1; }
    if (len == 4 && strncmp(s, "nbsp", 4) == 0) { *cp = ' ';  return 1; }
    return 0;
}

/* Decide whether a tag boundary should separate adjacent words (block/break
 * tags, paragraph/table/list elements in DOCX/ODT/XHTML). Inline spans such as
 * <w:r>, <w:t>, <text:span>, <b>, <i>, <em>, <span> should NOT inject spaces
 * mid-word. Note: ODT uses <text:s/> for an explicit space. */
static int is_separating_tag(const char *tag)
{
    const char *colon = strchr(tag, ':');
    const char *local = colon ? colon + 1 : tag;
    static const char *const sep[] = {
        "p", "br", "cr", "tab", "tr", "tc", "td", "th",
        "div", "li", "h", "h1", "h2", "h3", "h4", "h5", "h6",
        "blockquote", "pre", "section", "article", "header", "footer",
        "nav", "aside", "dt", "dd", "caption", "title", "hr",
        "line-break", "table-row", "table-cell", "list-item",
        NULL
    };
    if (strcasecmp(tag, "text:s") == 0) return 1;
    for (size_t i = 0; sep[i]; i++) {
        if (strcasecmp(local, sep[i]) == 0) return 1;
    }
    return 0;
}

static void xml_finish_tag(struct xml_stripper *x)
{
    x->tag_name[x->tag_len] = '\0';
    if (x->html_mode) {
        const char *colon = strchr(x->tag_name, ':');
        const char *local = colon ? colon + 1 : x->tag_name;
        if (!x->tag_closing && !x->tag_self_closing) {
            if (x->skip_tag == 0 && strcasecmp(local, "script") == 0) x->skip_tag = 1;
            else if (x->skip_tag == 0 && strcasecmp(local, "style") == 0) x->skip_tag = 2;
        } else if (x->tag_closing) {
            if (x->skip_tag == 1 && strcasecmp(local, "script") == 0) x->skip_tag = 0;
            else if (x->skip_tag == 2 && strcasecmp(local, "style") == 0) x->skip_tag = 0;
        }
    }
    if (x->skip_tag == 0 && x->emitted_any && is_separating_tag(x->tag_name)) {
        x->need_space = 1;
    }
}

static int xml_flush_raw_entity(struct xml_stripper *x)
{
    if (x->skip_tag) {
        x->ent_len = 0;
        return 0;
    }
    if (xml_emit_rune(x, '&') < 0) return -1;
    for (size_t i = 0; i < x->ent_len; i++) {
        if (xml_emit_rune(x, (unsigned char)x->ent_buf[i]) < 0) return -1;
    }
    x->ent_len = 0;
    return 0;
}

/* Feed one chunk of XML/XHTML bytes. Because libarchive streams arbitrary chunk
 * boundaries, UTF-8 handling is done via a tiny carry buffer in the caller or
 * directly on the whole bounded member buffer. Here we receive the complete
 * member buffer (already bounded to MAX_MEMBER_BYTES), so UTF-8 sequences never
 * span calls. */
static int xml_strip_buffer(struct xml_stripper *x, const unsigned char *buf, size_t n)
{
    static const unsigned char fffd[3] = { 0xEF, 0xBF, 0xBD };
    size_t i = 0;
    /* Skip leading UTF-8 BOM. */
    if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) i = 3;

    while (i < n && !x->out->full && !x->out->pipe_closed) {
        unsigned char c = buf[i];
        switch (x->state) {
        case XS_TEXT:
            if (c == '<') {
                x->state = XS_LT;
                x->tag_len = 0;
                x->tag_closing = 0;
                x->tag_self_closing = 0;
                x->tag_name_done = 0;
                i++;
            } else if (c == '&') {
                x->state = XS_ENTITY;
                x->ent_len = 0;
                i++;
            } else if (x->skip_tag) {
                i++;
            } else if (c == 0) {
                if (xml_emit_utf8_bytes(x, fffd, sizeof fffd) < 0) return -1;
                i++;
            } else if (c < 0x80u) {
                if (xml_emit_utf8_bytes(x, &c, 1) < 0) return -1;
                i++;
            } else {
                size_t len = utf8_valid_seq(buf + i, n - i);
                if (len > 0) {
                    if (xml_emit_utf8_bytes(x, buf + i, len) < 0) return -1;
                    i += len;
                } else {
                    if (xml_emit_utf8_bytes(x, fffd, sizeof fffd) < 0) return -1;
                    i++;
                }
            }
            break;

        case XS_ENTITY:
            if (c == ';') {
                uint32_t cp = 0;
                if (!x->skip_tag) {
                    if (xml_decode_entity(x->ent_buf, x->ent_len, &cp)) {
                        if (xml_emit_rune(x, cp) < 0) return -1;
                    } else {
                        if (xml_flush_raw_entity(x) < 0) return -1;
                        if (xml_emit_rune(x, ';') < 0) return -1;
                    }
                }
                x->ent_len = 0;
                x->state = XS_TEXT;
                i++;
            } else if (c == '<' || c == '&' || isspace(c) || x->ent_len >= sizeof(x->ent_buf) - 1) {
                if (xml_flush_raw_entity(x) < 0) return -1;
                x->state = XS_TEXT;
                /* Reprocess current byte in XS_TEXT. */
            } else {
                x->ent_buf[x->ent_len++] = (char)c;
                i++;
            }
            break;

        case XS_LT:
            if (c == '!') {
                x->state = XS_BANG;
                x->bang_len = 0;
                i++;
            } else if (c == '?') {
                x->state = XS_PI;
                x->prev_qmark = 0;
                i++;
            } else if (c == '/') {
                x->tag_closing = 1;
                x->state = XS_TAG;
                i++;
            } else if (c == '>') {
                x->state = XS_TEXT;
                i++;
            } else if (isalpha(c) || c == '_' || c == ':') {
                x->tag_name[x->tag_len++] = (char)c;
                x->state = XS_TAG;
                i++;
            } else {
                /* Stray '<' in text. */
                if (!x->skip_tag) {
                    if (xml_emit_rune(x, '<') < 0) return -1;
                }
                x->state = XS_TEXT;
                /* Reprocess c in XS_TEXT. */
            }
            break;

        case XS_TAG:
            if (c == '>') {
                xml_finish_tag(x);
                x->state = XS_TEXT;
                i++;
            } else if (c == '\'') {
                x->tag_name_done = 1;
                x->tag_self_closing = 0;
                x->state = XS_TAG_SQ;
                i++;
            } else if (c == '"') {
                x->tag_name_done = 1;
                x->tag_self_closing = 0;
                x->state = XS_TAG_DQ;
                i++;
            } else {
                if (c == '/') {
                    if (x->tag_len > 0) x->tag_name_done = 1;
                    x->tag_self_closing = 1;
                } else if (isspace(c)) {
                    if (x->tag_len > 0) x->tag_name_done = 1;
                } else {
                    x->tag_self_closing = 0;
                    if (!x->tag_name_done && x->tag_len < sizeof(x->tag_name) - 1) {
                        x->tag_name[x->tag_len++] = (char)c;
                    }
                }
                i++;
            }
            break;

        case XS_TAG_SQ:
            if (c == '\'') x->state = XS_TAG;
            i++;
            break;

        case XS_TAG_DQ:
            if (c == '"') x->state = XS_TAG;
            i++;
            break;

        case XS_BANG:
            if (x->bang_len < sizeof(x->bang_buf) - 1) {
                x->bang_buf[x->bang_len++] = (char)c;
            }
            if (x->bang_len == 2 && x->bang_buf[0] == '-' && x->bang_buf[1] == '-') {
                x->state = XS_COMMENT;
                x->dash_count = 0;
                i++;
            } else if (x->bang_len == 7 && memcmp(x->bang_buf, "[CDATA[", 7) == 0) {
                x->state = XS_CDATA;
                x->dash_count = 0;
                i++;
            } else if (c == '>') {
                x->state = XS_TEXT;
                i++;
            } else if (x->bang_len >= 7 ||
                       (x->bang_len == 1 && c != '-' && c != '[') ||
                       (x->bang_len == 2 && x->bang_buf[0] == '-' && c != '-') ||
                       (x->bang_buf[0] == '[' && memcmp(x->bang_buf, "[CDATA[", x->bang_len) != 0)) {
                /* <!DOCTYPE ...> or other declaration: skip until '>'. */
                x->tag_len = 0;
                x->tag_name_done = 1;
                x->state = XS_TAG;
                i++;
            } else {
                i++;
            }
            break;

        case XS_COMMENT:
            if (c == '-') {
                if (x->dash_count < 2) x->dash_count++;
            } else if (c == '>' && x->dash_count >= 2) {
                x->state = XS_TEXT;
                x->dash_count = 0;
            } else {
                x->dash_count = 0;
            }
            i++;
            break;

        case XS_CDATA:
            if (c == ']') {
                if (x->dash_count < 2) {
                    x->dash_count++;
                } else {
                    if (!x->skip_tag && xml_emit_rune(x, ']') < 0) return -1;
                }
                i++;
            } else if (c == '>' && x->dash_count == 2) {
                x->dash_count = 0;
                x->state = XS_TEXT;
                i++;
            } else {
                while (x->dash_count > 0) {
                    if (!x->skip_tag && xml_emit_rune(x, ']') < 0) return -1;
                    x->dash_count--;
                }
                if (x->skip_tag) {
                    i++;
                } else if (c == 0) {
                    if (xml_emit_utf8_bytes(x, fffd, sizeof fffd) < 0) return -1;
                    i++;
                } else if (c < 0x80u) {
                    if (xml_emit_utf8_bytes(x, &c, 1) < 0) return -1;
                    i++;
                } else {
                    size_t len = utf8_valid_seq(buf + i, n - i);
                    if (len > 0) {
                        if (xml_emit_utf8_bytes(x, buf + i, len) < 0) return -1;
                        i += len;
                    } else {
                        if (xml_emit_utf8_bytes(x, fffd, sizeof fffd) < 0) return -1;
                        i++;
                    }
                }
            }
            break;

        case XS_PI:
            if (x->prev_qmark && c == '>') {
                x->state = XS_TEXT;
                x->prev_qmark = 0;
            } else {
                x->prev_qmark = (c == '?');
            }
            i++;
            break;
        }
    }

    if (x->state == XS_ENTITY && !x->out->full && !x->out->pipe_closed) {
        if (xml_flush_raw_entity(x) < 0) return -1;
        x->state = XS_TEXT;
    }
    return x->out->pipe_closed ? -1 : 0;
}

/* ---- ZIP container extraction (DOCX / ODT / EPUB) via libarchive ---- */

enum zip_kind {
    ZK_DOCX = 1,
    ZK_ODT  = 2,
    ZK_EPUB = 3,
};

static const char *normalize_member_path(const char *p)
{
    if (!p) return "";
    while (p[0] == '.' && p[1] == '/') p += 2;
    while (p[0] == '/') p++;
    return p;
}

static int member_matches(enum zip_kind kind, const char *raw_name)
{
    const char *name = normalize_member_path(raw_name);
    if (!*name) return 0;
    if (kind == ZK_DOCX) {
        return strcasecmp(name, "word/document.xml") == 0;
    }
    if (kind == ZK_ODT) {
        return strcasecmp(name, "content.xml") == 0;
    }
    /* EPUB: *.xhtml or *.html */
    const char *ext = file_extension(name);
    return strcasecmp(ext, "xhtml") == 0 || strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0;
}

static int read_archive_member(struct archive *a, unsigned char **out_data, size_t *out_len,
                               size_t *total_unpacked)
{
    size_t cap = 16384;
    size_t len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) die(EXIT_ERR, "out of memory");

    for (;;) {
        if (len == cap) {
            if (cap >= MAX_MEMBER_BYTES) {
                /* Member exceeds per-member cap: stop reading further bytes. */
                break;
            }
            size_t ncap = cap * 2u;
            if (ncap > MAX_MEMBER_BYTES) ncap = MAX_MEMBER_BYTES;
            unsigned char *nbuf = realloc(buf, ncap);
            if (!nbuf) {
                free(buf);
                die(EXIT_ERR, "out of memory");
            }
            buf = nbuf;
            cap = ncap;
        }
        size_t want = cap - len;
        if (want > MAX_UNPACKED_TOTAL - *total_unpacked) {
            want = MAX_UNPACKED_TOTAL - *total_unpacked;
        }
        if (want == 0) break;
        la_ssize_t r = archive_read_data(a, buf + len, want);
        if (r < 0) {
            free(buf);
            return -1;
        }
        if (r == 0) break;
        len += (size_t)r;
        *total_unpacked += (size_t)r;
        if (len >= MAX_MEMBER_BYTES || *total_unpacked >= MAX_UNPACKED_TOTAL) break;
    }

    *out_data = buf;
    *out_len = len;
    return 0;
}

static int extract_zip_doc(const unsigned char *data, size_t size, enum zip_kind kind, struct outbuf *o)
{
    struct archive *a = archive_read_new();
    if (!a) die(EXIT_ERR, "out of memory");

    /* Only enable ZIP format (no external filter programs, no unexpected formats). */
    if (archive_read_support_format_zip(a) != ARCHIVE_OK) {
        archive_read_free(a);
        die(EXIT_ERR, "libarchive zip support unavailable");
    }
    if (archive_read_open_memory(a, data, size) != ARCHIVE_OK) {
        const char *err = archive_error_string(a);
        char msg[256];
        snprintf(msg, sizeof msg, "zip open failed: %s", err ? err : "invalid archive");
        archive_read_free(a);
        die(EXIT_ERR, "%s", msg);
    }

    struct xml_stripper xs;
    xml_stripper_init(&xs, o, kind == ZK_EPUB);

    struct archive_entry *e = NULL;
    size_t entries = 0;
    size_t matched = 0;
    size_t total_unpacked = 0;
    int r;

    while (!o->full && !o->pipe_closed && entries < MAX_ZIP_ENTRIES) {
        r = archive_read_next_header(a, &e);
        if (r == ARCHIVE_RETRY) continue;
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_WARN) {
            const char *err = archive_error_string(a);
            char msg[256];
            snprintf(msg, sizeof msg, "zip read failed: %s", err ? err : "corrupt archive");
            archive_read_free(a);
            die(EXIT_ERR, "%s", msg);
        }
        entries++;

        if (archive_entry_filetype_is_set(e) && archive_entry_filetype(e) != AE_IFREG) {
            continue;
        }
        const char *name = archive_entry_pathname_utf8(e);
        if (!name) name = archive_entry_pathname(e);
        if (!name || !member_matches(kind, name)) {
            continue;
        }
        if (archive_entry_is_encrypted(e)) {
            archive_read_free(a);
            die(EXIT_ERR, "archive member is encrypted");
        }
        /* Reject obvious zip bombs before decompressing when header declares a huge size. */
        if (archive_entry_size_is_set(e) && archive_entry_size(e) > (la_int64_t)MAX_MEMBER_BYTES) {
            archive_read_free(a);
            die(EXIT_TOO_LARGE, "archive member exceeds size limit");
        }
        if (total_unpacked >= MAX_UNPACKED_TOTAL) {
            break;
        }

        unsigned char *member_buf = NULL;
        size_t member_len = 0;
        if (read_archive_member(a, &member_buf, &member_len, &total_unpacked) < 0) {
            const char *err = archive_error_string(a);
            char msg[256];
            snprintf(msg, sizeof msg, "zip member read failed: %s", err ? err : "corrupt member");
            archive_read_free(a);
            die(EXIT_ERR, "%s", msg);
        }
        matched++;
        xml_stripper_reset_doc(&xs);
        int sr = xml_strip_buffer(&xs, member_buf, member_len);
        free(member_buf);
        if (sr < 0) {
            archive_read_free(a);
            return -1;
        }
        if (kind == ZK_DOCX || kind == ZK_ODT) {
            /* Single primary XML payload already extracted. */
            break;
        }
    }

    archive_read_free(a);
    if (matched == 0) {
        die(EXIT_UNSUPPORTED, "no extractable document content in archive");
    }
    return 0;
}

/* ---- PDF extraction via libmupdf ---- */

#if HAVE_MUPDF
static void mupdf_silent_callback(void *user, const char *message)
{
    (void)user;
    (void)message;
}

static int extract_pdf(const unsigned char *data, size_t size, struct outbuf *o)
{
    fz_context *ctx = fz_new_context(NULL, NULL, 64u << 20);
    if (!ctx) die(EXIT_ERR, "cannot initialize MuPDF");

    /* Silence MuPDF's stderr diagnostics so stderr obeys the one-line contract. */
    fz_set_warning_callback(ctx, mupdf_silent_callback, NULL);
    fz_set_error_callback(ctx, mupdf_silent_callback, NULL);

    fz_stream *stm = NULL;
    fz_document *doc = NULL;
    fz_page *page = NULL;
    fz_stext_page *stext = NULL;
    fz_device *dev = NULL;
    fz_buffer *buf = NULL;
    int err_code = 0;
    char err_msg[256] = "";

    fz_var(stm);
    fz_var(doc);
    fz_var(page);
    fz_var(stext);
    fz_var(dev);
    fz_var(buf);
    fz_var(err_code);

    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        stm = fz_open_memory(ctx, data, size);
        doc = fz_open_document_with_stream(ctx, "application/pdf", stm);
        if (fz_needs_password(ctx, doc)) {
            fz_throw(ctx, FZ_ERROR_UNSUPPORTED, "PDF requires a password");
        }
        int npages = fz_count_pages(ctx, doc);
        if (npages > MAX_PDF_PAGES) npages = MAX_PDF_PAGES;

        for (int i = 0; i < npages && !o->full && !o->pipe_closed; i++) {
            page = fz_load_page(ctx, doc, i);
            fz_rect box = fz_bound_page(ctx, page);
            fz_stext_options opts;
            fz_init_stext_options(ctx, &opts);
            stext = fz_new_stext_page(ctx, box);
            dev = fz_new_stext_device(ctx, stext, &opts);
            fz_run_page(ctx, page, dev, fz_identity, NULL);
            fz_close_device(ctx, dev);
            fz_drop_device(ctx, dev);
            dev = NULL;

            buf = fz_new_buffer_from_stext_page(ctx, stext);
            unsigned char *txt = NULL;
            size_t tlen = fz_buffer_storage(ctx, buf, &txt);
            if (i > 0 && o->total > 0 && tlen > 0) {
                (void)out_put_byte(o, '\n');
            }
            if (tlen > 0) {
                (void)out_put_utf8_sanitized(o, txt, tlen);
            }

            fz_drop_buffer(ctx, buf);
            buf = NULL;
            fz_drop_stext_page(ctx, stext);
            stext = NULL;
            fz_drop_page(ctx, page);
            page = NULL;
        }
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
        fz_drop_device(ctx, dev);
        fz_drop_stext_page(ctx, stext);
        fz_drop_page(ctx, page);
        fz_drop_document(ctx, doc);
        fz_drop_stream(ctx, stm);
    }
    fz_catch(ctx) {
        err_code = 1;
        const char *m = fz_caught_message(ctx);
        snprintf(err_msg, sizeof err_msg, "pdf: %s", m && *m ? m : "extraction failed");
        fz_ignore_error(ctx);
    }

    fz_drop_context(ctx);
    if (err_code) {
        die(EXIT_ERR, "%s", err_msg);
    }
    return o->pipe_closed ? -1 : 0;
}
#else
static int extract_pdf(const unsigned char *data, size_t size, struct outbuf *o)
{
    (void)data;
    (void)size;
    (void)o;
    die(EXIT_UNSUPPORTED, "PDF support not built in (libmupdf unavailable)");
}
#endif

/* ---- Reading the target file ---- */

static unsigned char *read_bounded_file(int fd, size_t *out_len)
{
    struct stat st;
    if (fstat(fd, &st) != 0) {
        die(EXIT_ERR, "fstat: %s", strerror(errno));
    }
    if (!S_ISREG(st.st_mode)) {
        die(EXIT_NOT_REGULAR, "not a regular file");
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > MAX_INPUT_BYTES) {
        die(EXIT_TOO_LARGE, "file exceeds 8 MiB limit");
    }

    size_t expected = (size_t)st.st_size;
    /* Allocate +1 byte so we detect a file that grew past 8 MiB while reading. */
    size_t cap = expected > 0 ? expected + 1u : 4096u;
    if (cap > MAX_INPUT_BYTES + 1u) cap = MAX_INPUT_BYTES + 1u;

    unsigned char *buf = malloc(cap);
    if (!buf) die(EXIT_ERR, "out of memory");

    size_t len = 0;
    for (;;) {
        if (len == cap) {
            if (cap >= MAX_INPUT_BYTES + 1u) break;
            size_t ncap = cap * 2u;
            if (ncap > MAX_INPUT_BYTES + 1u) ncap = MAX_INPUT_BYTES + 1u;
            unsigned char *nbuf = realloc(buf, ncap);
            if (!nbuf) {
                free(buf);
                die(EXIT_ERR, "out of memory");
            }
            buf = nbuf;
            cap = ncap;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR) continue;
            int err = errno;
            free(buf);
            die(EXIT_ERR, "read: %s", strerror(err));
        }
        if (n == 0) break;
        len += (size_t)n;
        if (len > MAX_INPUT_BYTES) {
            free(buf);
            die(EXIT_TOO_LARGE, "file exceeds 8 MiB limit");
        }
    }

    *out_len = len;
    return buf;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    if (argc != 2 || !argv[1][0] || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fputs(docscan_usage, stderr);
        return EXIT_USAGE;
    }

    const char *path = argv[1];
    uid_t expect_uid = 0;
    gid_t expect_gid = 0;
    int dropped = drop_and_sandbox(path, &expect_uid, &expect_gid);

    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ELOOP) die(EXIT_NOT_REGULAR, "refusing to follow symlink");
        die(EXIT_ERR, "open: %s", strerror(errno));
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int err = errno;
        close(fd);
        die(EXIT_ERR, "fstat: %s", strerror(err));
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        die(EXIT_NOT_REGULAR, "not a regular file");
    }
    if (dropped && (st.st_uid != expect_uid || st.st_gid != expect_gid)) {
        close(fd);
        die(EXIT_SANDBOX, "file owner changed after privilege drop");
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > MAX_INPUT_BYTES) {
        close(fd);
        die(EXIT_TOO_LARGE, "file exceeds 8 MiB limit");
    }

    size_t len = 0;
    unsigned char *data = read_bounded_file(fd, &len);
    close(fd);

    const char *ext = file_extension(path);
    struct outbuf out;
    memset(&out, 0, sizeof out);

    int rc = 0;
    if (strcasecmp(ext, "pdf") == 0) {
        rc = extract_pdf(data, len, &out);
    } else if (strcasecmp(ext, "docx") == 0) {
        if (!has_zip_magic(data, len)) {
            free(data);
            die(EXIT_ERR, "not a valid docx zip archive");
        }
        rc = extract_zip_doc(data, len, ZK_DOCX, &out);
    } else if (strcasecmp(ext, "odt") == 0) {
        if (!has_zip_magic(data, len)) {
            free(data);
            die(EXIT_ERR, "not a valid odt zip archive");
        }
        rc = extract_zip_doc(data, len, ZK_ODT, &out);
    } else if (strcasecmp(ext, "epub") == 0) {
        if (!has_zip_magic(data, len)) {
            free(data);
            die(EXIT_ERR, "not a valid epub zip archive");
        }
        rc = extract_zip_doc(data, len, ZK_EPUB, &out);
    } else if (is_text_extension(ext)) {
        rc = extract_plain_text(data, len, &out);
    } else if (has_pdf_header(data, len)) {
        rc = extract_pdf(data, len, &out);
    } else if (sniff_looks_like_text(data, len)) {
        rc = extract_plain_text(data, len, &out);
    } else {
        free(data);
        die(EXIT_UNSUPPORTED, "unsupported binary file");
    }

    free(data);
    if (rc == 0 && !out.pipe_closed) {
        (void)out_flush(&out);
    }
    /* If stdout closed (SIGPIPE/EPIPE), exit quietly as required by contract. */
    return EXIT_OK;
}
