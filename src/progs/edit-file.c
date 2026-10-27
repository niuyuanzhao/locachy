/*
 * edit's files (edit-file.h). Every file system call between file_as_user(1)
 * and file_as_user(0) has the user's permissions (fsuid and fsgid 1000, as
 * files.c's as_user): edit runs as root, and must not read or write what the
 * user could not.
 *
 * The encoding is found in KTextEditor's order as far as it applies here
 * (TextBuffer::load, katetextbuffer.cpp): the default encoding, UTF-8, when
 * all of the file is valid UTF-8 (a leading byte order mark taken off and
 * remembered); else a byte order mark (UTF-16LE/BE), as its detection step
 * finds it; else the fallback encoding, which LOCachy's plan makes GB18030
 * (Kate's is ISO-8859-1; its encoding prober is not here). Bytes that
 * neither reads stay as they are and the document is read-only, as Kate does
 * with a broken encoding (the decoder's error, TextLoader::readLine); NUL
 * bytes are no error there, and nothing special here. The line ends are KTextEditor's loader's choice: \r\n
 * if there is one, else \n if there is one, else \r; all become "\n" here.
 * Lines longer than the limit are wrapped as its Line Length Limit wraps them:
 * after a space or punctuation in the limit's last tenth, else at the limit.
 *
 * KTextEditor writes into the file in place (TextBuffer::save). LOCachy's
 * plan wants Qt's QSaveFile instead, which is also what KTextEditor's root
 * helper does: a symbolic link is followed to its target, a new file
 * NAME.XXXXXX is written beside it, synced, given the old file's mode (and
 * its owner and group where the user may), and renamed over it. As writing
 * in place would, it refuses a file that the user may not write.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edit-file.h"

void file_as_user(int on)
{
    setfsgid(on ? EDIT_USER : 0);
    setfsuid(on ? EDIT_USER : 0);
}

/* SHA-1, for the git blob checksum. */
struct sha1 {
    uint32_t h[5];
    uint64_t bytes;
    unsigned char block[64];
    size_t n;
};

static uint32_t rol(uint32_t x, int k)
{
    return (x << k) | (x >> (32 - k));
}

static void sha1_block(struct sha1 *s, const unsigned char *p)
{
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

static void sha1_init(struct sha1 *s)
{
    *s = (struct sha1){ { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 }, 0, { 0 }, 0 };
}

static void sha1_add(struct sha1 *s, const void *data, size_t len)
{
    const unsigned char *p = data;
    s->bytes += len;
    while (len > 0) {
        size_t k = 64 - s->n < len ? 64 - s->n : len;
        memcpy(s->block + s->n, p, k);
        s->n += k;
        p += k;
        len -= k;
        if (s->n == 64) {
            sha1_block(s, s->block);
            s->n = 0;
        }
    }
}

static void sha1_end(struct sha1 *s, uint8_t out[DIGEST_SIZE])
{
    uint64_t bits = s->bytes * 8;
    unsigned char pad = 0x80, zero = 0, len[8];
    sha1_add(s, &pad, 1);
    while (s->n != 56) {
        sha1_add(s, &zero, 1);
    }
    for (int i = 0; i < 8; i++) {
        len[i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    sha1_add(s, len, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

/* git's blob header, "blob SIZE" and a NUL, before the bytes. */
static void blob_start(struct sha1 *s, size_t size)
{
    char header[32];
    int n = snprintf(header, sizeof header, "blob %zu", size);
    sha1_init(s);
    sha1_add(s, header, (size_t)n + 1);
}

static void blob_sum(const void *data, size_t len, uint8_t out[DIGEST_SIZE])
{
    struct sha1 s;
    blob_start(&s, len);
    sha1_add(&s, data, len);
    sha1_end(&s, out);
}

void file_digest_hex(const uint8_t d[DIGEST_SIZE], char out[2 * DIGEST_SIZE + 1])
{
    for (int i = 0; i < DIGEST_SIZE; i++) {
        snprintf(out + 2 * i, 3, "%02x", d[i]);
    }
}

/* All of it valid UTF-8 (no overlong forms, surrogates or values past
 * U+10FFFF). */
static int valid_utf8(const unsigned char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || i + (size_t)n >= len) {
            return 0;
        }
        uint32_t cp = c & (0x3Fu >> n);
        for (int k = 1; k <= n; k++) {
            if ((s[i + (size_t)k] & 0xC0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (s[i + (size_t)k] & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return 0;
        }
        i += (size_t)n + 1;
    }
    return 1;
}

/* UTF-16 as UTF-8; -1 if it is cut or has a lone surrogate. */
static int from_utf16(const unsigned char *s, size_t len, int big, char **out, size_t *out_len)
{
    if (len % 2) {
        return -1;
    }
    char *o = malloc(len / 2 * 3 + 1), *p = o;
    if (!o) {
        return -1;
    }
    for (size_t i = 0; i < len; i += 2) {
        uint32_t u = big ? (uint32_t)(s[i] << 8 | s[i + 1]) : (uint32_t)(s[i + 1] << 8 | s[i]);
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 3 >= len) {
                free(o);
                return -1;
            }
            uint32_t v = big ? (uint32_t)(s[i + 2] << 8 | s[i + 3]) : (uint32_t)(s[i + 3] << 8 | s[i + 2]);
            if (v < 0xDC00 || v > 0xDFFF) {
                free(o);
                return -1;
            }
            u = 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00);
            i += 2;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            free(o);
            return -1;
        }
        if (u < 0x80) {
            *p++ = (char)u;
        } else if (u < 0x800) {
            *p++ = (char)(0xC0 | (u >> 6));
            *p++ = (char)(0x80 | (u & 0x3F));
        } else if (u < 0x10000) {
            *p++ = (char)(0xE0 | (u >> 12));
            *p++ = (char)(0x80 | ((u >> 6) & 0x3F));
            *p++ = (char)(0x80 | (u & 0x3F));
        } else {
            *p++ = (char)(0xF0 | (u >> 18));
            *p++ = (char)(0x80 | ((u >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((u >> 6) & 0x3F));
            *p++ = (char)(0x80 | (u & 0x3F));
        }
    }
    *p = '\0';
    *out = o;
    *out_len = (size_t)(p - o);
    return 0;
}

/* UTF-8 as UTF-16. */
static char *to_utf16(const char *s, size_t len, int big, size_t *out_len)
{
    unsigned char *o = malloc(len * 2 + 2), *p = o;
    if (!o) {
        return NULL;
    }
    for (size_t i = 0; i < len;) {
        const unsigned char *c = (const unsigned char *)s + i;
        uint32_t u = c[0];
        size_t n = 1;
        if (c[0] >= 0xC0 && c[0] < 0xE0 && i + 1 < len) {
            u = (uint32_t)(c[0] & 0x1F) << 6 | (c[1] & 0x3F);
            n = 2;
        } else if (c[0] >= 0xE0 && c[0] < 0xF0 && i + 2 < len) {
            u = (uint32_t)(c[0] & 0x0F) << 12 | (uint32_t)(c[1] & 0x3F) << 6 | (c[2] & 0x3F);
            n = 3;
        } else if (c[0] >= 0xF0 && i + 3 < len) {
            u = (uint32_t)(c[0] & 0x07) << 18 | (uint32_t)(c[1] & 0x3F) << 12 | (uint32_t)(c[2] & 0x3F) << 6 |
                (c[3] & 0x3F);
            n = 4;
        }
        i += n;
        uint32_t units[2] = { u, 0 };
        int count = 1;
        if (u >= 0x10000) {
            units[0] = 0xD800 + ((u - 0x10000) >> 10);
            units[1] = 0xDC00 + ((u - 0x10000) & 0x3FF);
            count = 2;
        }
        for (int k = 0; k < count; k++) {
            *p++ = (unsigned char)(big ? units[k] >> 8 : units[k] & 0xFF);
            *p++ = (unsigned char)(big ? units[k] & 0xFF : units[k] >> 8);
        }
    }
    *out_len = (size_t)(p - o);
    return (char *)o;
}

/* s in encoding from as encoding to through iconv: -1 if a byte cannot be
 * read or a character cannot be written. */
static int convert(const char *to, const char *from, const char *s, size_t len, char **out, size_t *out_len)
{
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) {
        return -1;
    }
    size_t size = len * 2 + 16;
    char *o = malloc(size);
    char *in = (char *)s, *p = o;
    size_t left = len, room = size - 1;
    int ok = o != NULL;
    while (ok && left > 0) {
        if (iconv(cd, &in, &left, &p, &room) != (size_t)-1) {
            break;
        }
        if (errno != E2BIG) {
            ok = 0;
            break;
        }
        size_t used = (size_t)(p - o);
        size *= 2;
        char *bigger = realloc(o, size);
        if (!bigger) {
            ok = 0;
            break;
        }
        o = bigger;
        p = o + used;
        room = size - 1 - used;
    }
    if (ok && iconv(cd, NULL, NULL, &p, &room) == (size_t)-1) {
        ok = 0;
    }
    iconv_close(cd);
    if (!ok) {
        free(o);
        return -1;
    }
    *p = '\0';
    *out = o;
    *out_len = (size_t)(p - o);
    return 0;
}

/* Every \r\n and \r of s as \n, in place, and which the file's are: as
 * KTextEditor's TextLoader sets it, \r\n wins over \n, which wins over \r. */
static size_t to_lf(char *s, size_t len, int *eol)
{
    size_t o = 0;
    int dos = 0, lf = 0, mac = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r') {
            if (i + 1 < len && s[i + 1] == '\n') {
                dos = 1;
                i++;
            } else {
                mac = 1;
            }
            s[o++] = '\n';
        } else {
            lf |= s[i] == '\n';
            s[o++] = s[i];
        }
    }
    s[o] = '\0';
    *eol = dos ? EOL_CRLF : lf ? EOL_LF : mac ? EOL_CR : EOL_LF;
    return o;
}

/* Lines of s longer than limit characters wrapped (in a new text), as
 * KTextEditor's TextLoader does: after the last space or punctuation in
 * the limit's last tenth, else after limit characters. NULL if none was. */
static char *wrap_long(const char *s, size_t len, size_t limit, size_t *out_len, size_t *longest)
{
    *longest = 0;
    size_t chars = 0, extra = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s[i] == '\n') {
            if (chars > limit) {
                extra += chars / limit + 1;
                *longest = chars > *longest ? chars : *longest;
            }
            chars = 0;
        } else if (((unsigned char)s[i] & 0xC0) != 0x80) {
            chars++;
        }
    }
    if (!*longest) {
        return NULL;
    }
    char *o = malloc(len + extra + 1);
    if (!o) {
        return NULL;
    }
    size_t n = 0;
    for (size_t start = 0; start <= len;) {
        size_t end = start;
        while (end < len && s[end] != '\n') {
            end++;
        }
        /* The line [start, end), cut while it is too long. */
        size_t at = start;
        for (;;) {
            size_t count = 0, k = at, cut_char = 0, cut = 0, last_good = 0;
            while (k < end && count < limit) {
                size_t next = k + 1;
                while (next < end && ((unsigned char)s[next] & 0xC0) == 0x80) {
                    next++;
                }
                unsigned char c = (unsigned char)s[k];
                if (count >= limit - limit / 10 && c < 0x80 && (isspace(c) || ispunct(c))) {
                    cut_char = count + 1;
                    last_good = next;
                }
                count++;
                k = next;
            }
            if (k >= end) {
                break;
            }
            cut = cut_char ? last_good : k;
            memcpy(o + n, s + at, cut - at);
            n += cut - at;
            o[n++] = '\n';
            at = cut;
        }
        memcpy(o + n, s + at, end - at);
        n += end - at;
        if (end < len) {
            o[n++] = '\n';
        }
        start = end + 1;
    }
    o[n] = '\0';
    *out_len = n;
    return o;
}

int file_load(const char *path, char **text, size_t *len, struct file_format *format, int *flags, struct stat *st,
              uint8_t digest[DIGEST_SIZE], size_t limit, size_t *longest)
{
    return file_load_encoding(path, text, len, format, flags, st, digest, limit, longest, -1);
}

int file_load_encoding(const char *path, char **text, size_t *len, struct file_format *format, int *flags,
                       struct stat *st, uint8_t digest[DIGEST_SIZE], size_t limit, size_t *longest, int encoding)
{
    *text = NULL;
    *len = 0;
    *flags = 0;
    *longest = 0;
    *format = (struct file_format){ ENC_UTF8, 0, EOL_LF };
    memset(st, 0, sizeof *st);
    blob_sum("", 0, digest);
    file_as_user(1);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    int err = fd < 0 ? errno : 0;
    if (fd >= 0 && fstat(fd, st) < 0) {
        err = errno;
    } else if (fd >= 0 && S_ISDIR(st->st_mode)) {
        err = EISDIR;
    } else if (fd >= 0 && !S_ISREG(st->st_mode)) {
        err = EINVAL; /* a device or a pipe is not read as a text */
    }
    unsigned char *raw = NULL;
    size_t n = 0;
    if (fd >= 0 && !err) {
        size_t size = (size_t)st->st_size + 1;
        raw = malloc(size + 1);
        while (raw) {
            ssize_t got = read(fd, raw + n, size - n);
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got < 0) {
                err = errno;
                break;
            }
            if (got == 0) {
                break;
            }
            n += (size_t)got;
            if (n == size) {
                unsigned char *more = realloc(raw, size * 2 + 1);
                if (!more) {
                    err = ENOMEM;
                    break;
                }
                raw = more;
                size *= 2;
            }
        }
        if (!raw) {
            err = ENOMEM;
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    file_as_user(0);
    if (err == ENOENT) {
        memset(st, 0, sizeof *st);
        *text = calloc(1, 1);
        return *text ? 0 : ENOMEM;
    }
    if (err) {
        free(raw);
        return err;
    }
    raw[n] = '\0';
    blob_sum(raw, n, digest);

    char *t = NULL;
    size_t tl = 0, skip = n >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF ? 3 : 0;
    if (encoding >= 0) {
        if (encoding >= ENC_COUNT) {
            free(raw);
            return EINVAL;
        }
        format->encoding = encoding;
        skip = encoding == ENC_UTF8 ? skip : 0;
        if ((encoding == ENC_UTF16LE || encoding == ENC_UTF16BE) && n >= 2 &&
            raw[0] == (encoding == ENC_UTF16LE ? 0xFF : 0xFE) &&
            raw[1] == (encoding == ENC_UTF16LE ? 0xFE : 0xFF)) skip = 2;
        format->bom = skip != 0;
        if (encoding == ENC_UTF8 && valid_utf8(raw + skip, n - skip)) {
            memmove(raw, raw + skip, n - skip + 1);
            t = (char *)raw;
            tl = n - skip;
            raw = NULL;
        } else if (encoding != ENC_UTF8) {
            convert("UTF-8", file_encoding_name(format), (const char *)raw + skip, n - skip, &t, &tl);
        }
    } else if (valid_utf8(raw + skip, n - skip)) {
        format->bom = skip != 0;
        memmove(raw, raw + skip, n - skip + 1);
        t = (char *)raw;
        tl = n - skip;
        raw = NULL;
    } else if (n >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        *format = (struct file_format){ ENC_UTF16LE, 1, EOL_LF };
        if (from_utf16(raw + 2, n - 2, 0, &t, &tl) < 0) {
            t = NULL;
        }
    } else if (n >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        *format = (struct file_format){ ENC_UTF16BE, 1, EOL_LF };
        if (from_utf16(raw + 2, n - 2, 1, &t, &tl) < 0) {
            t = NULL;
        }
    } else if (!skip && convert("UTF-8", "GB18030", (const char *)raw, n, &t, &tl) == 0) {
        format->encoding = ENC_GB18030;
    }
    if (!t) {
        /* Nothing reads it: its bytes as they are, as UTF-8. */
        *flags |= FILE_BROKEN;
        *format = (struct file_format){ encoding >= 0 ? encoding : ENC_UTF8, skip != 0, EOL_LF };
        t = (char *)raw;
        tl = n - skip;
        memmove(t, t + skip, tl + 1);
        raw = NULL;
    }
    free(raw);
    tl = to_lf(t, tl, &format->eol);
    if (limit > 0) {
        size_t wl;
        char *w = wrap_long(t, tl, limit, &wl, longest);
        if (w) {
            free(t);
            t = w;
            tl = wl;
            *flags |= FILE_WRAPPED;
        }
    }
    *text = t;
    *len = tl;
    return 0;
}

void file_resolve(const char *path, char *out, size_t size)
{
    char cur[PATH_MAX];
    snprintf(cur, sizeof cur, "%s", path);
    for (int depth = 0; depth < 128; depth++) {
        struct stat st;
        char target[PATH_MAX];
        if (lstat(cur, &st) < 0 || !S_ISLNK(st.st_mode)) {
            break;
        }
        ssize_t n = readlink(cur, target, sizeof target - 1);
        if (n <= 0) {
            break;
        }
        target[n] = '\0';
        if (target[0] == '/') {
            snprintf(cur, sizeof cur, "%s", target);
        } else {
            char *slash = strrchr(cur, '/');
            char joined[PATH_MAX];
            if (slash && snprintf(joined, sizeof joined, "%.*s/%s", (int)(slash - cur), cur, target) <
                             (int)sizeof joined) {
                snprintf(cur, sizeof cur, "%s", joined);
            } else if (!slash) {
                snprintf(cur, sizeof cur, "%s", target);
            } else {
                break;
            }
        }
    }
    snprintf(out, size, "%s", cur);
}

/* The bytes of the file for text: line ends, encoding and its mark. */
static char *encode(const char *text, size_t len, const struct file_format *f, size_t *out_len)
{
    size_t lines = 0;
    for (const char *q = text; (q = memchr(q, '\n', len - (size_t)(q - text))) != NULL; q++) {
        lines++;
    }
    size_t size = len + (f->eol == EOL_CRLF ? lines : 0) + 3;
    char *s = malloc(size + 1), *p = s;
    if (!s) {
        return NULL;
    }
    if (f->encoding == ENC_UTF8 && f->bom) {
        memcpy(p, "\xEF\xBB\xBF", 3);
        p += 3;
    }
    for (size_t i = 0; i < len; i++) {
        if (text[i] != '\n') {
            *p++ = text[i];
        } else if (f->eol == EOL_CRLF) {
            *p++ = '\r';
            *p++ = '\n';
        } else {
            *p++ = f->eol == EOL_CR ? '\r' : '\n';
        }
    }
    *out_len = (size_t)(p - s);
    if (f->encoding == ENC_UTF8) {
        return s;
    }
    char *o = NULL;
    size_t ol = 0;
    if (f->encoding >= ENC_GB18030) {
        if (convert(file_encoding_name(f), "UTF-8", s, *out_len, &o, &ol) < 0) {
            o = NULL;
        }
    } else {
        int big = f->encoding == ENC_UTF16BE;
        char *u = to_utf16(s, *out_len, big, &ol);
        if (u) {
            size_t mark = f->bom ? 2 : 0;
            o = malloc(ol + mark + 1);
            if (o) {
                if (mark) {
                    o[0] = (char)(big ? 0xFE : 0xFF);
                    o[1] = (char)(big ? 0xFF : 0xFE);
                }
                memcpy(o + mark, u, ol);
                ol += mark;
            }
            free(u);
        }
    }
    free(s);
    *out_len = ol;
    return o;
}

int file_save(const char *path, const char *text, size_t len, const struct file_format *format, struct stat *st,
              char *final, size_t final_size, uint8_t digest[DIGEST_SIZE])
{
    size_t n;
    char *bytes = encode(text, len, format, &n);
    if (!bytes) {
        return format->encoding >= ENC_GB18030 ? EILSEQ : ENOMEM;
    }
    blob_sum(bytes, n, digest);
    char target[PATH_MAX], temp[PATH_MAX + 8];
    file_as_user(1);
    file_resolve(path, target, sizeof target);
    snprintf(final, final_size, "%s", target);
    struct stat old;
    int exists = stat(target, &old) == 0, err = 0, fd = -1;
    if (exists && !S_ISREG(old.st_mode)) {
        err = S_ISDIR(old.st_mode) ? EISDIR : EINVAL;
    } else if (exists && (fd = open(target, O_WRONLY | O_CLOEXEC | O_NOCTTY)) < 0) {
        err = errno; /* the user may not write it: not replaced either */
    } else if (snprintf(temp, sizeof temp, "%s.XXXXXX", target) >= (int)sizeof temp) {
        err = ENAMETOOLONG;
    }
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    if (!err && (fd = mkostemp(temp, O_CLOEXEC)) < 0) {
        err = errno;
    }
    if (!err) {
        mode_t mode;
        if (exists) {
            mode = old.st_mode & 07777;
        } else {
            mode_t mask = umask(022);
            umask(mask);
            mode = 0666 & ~mask;
        }
        for (size_t done = 0; done < n && !err;) {
            ssize_t w = write(fd, bytes + done, n - done);
            if (w < 0 && errno != EINTR) {
                err = errno;
            } else if (w > 0) {
                done += (size_t)w;
            }
        }
        if (!err && exists) {
            /* The old owner and group where the user may give them (the
             * group, if the user is in it); fchmod after, as chown clears
             * the set-id bits. */
            struct stat now;
            if (fstat(fd, &now) == 0 && (now.st_uid != old.st_uid || now.st_gid != old.st_gid) &&
                fchown(fd, old.st_uid, old.st_gid) < 0 && fchown(fd, (uid_t)-1, old.st_gid) < 0) {
                /* Neither: the user's own, as a new file of the user's. */
            }
        }
        if (!err && fchmod(fd, mode) < 0) {
            err = errno;
        }
        if (!err && fsync(fd) < 0) {
            err = errno;
        }
        if (close(fd) < 0 && !err) {
            err = errno;
        }
        if (!err && rename(temp, target) < 0) {
            err = errno;
        }
        if (err) {
            unlink(temp);
        }
    }
    if (!err && stat(target, st) < 0) {
        err = errno;
    }
    file_as_user(0);
    free(bytes);
    return err;
}

int file_digest(const char *path, uint8_t digest[DIGEST_SIZE])
{
    file_as_user(1);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY), err = 0;
    struct stat st;
    struct sha1 s;
    if (fd < 0) {
        err = errno;
    } else if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        err = EINVAL;
    } else {
        blob_start(&s, (size_t)st.st_size);
    }
    char buf[65536];
    ssize_t got;
    size_t total = 0;
    while (!err && (got = read(fd, buf, sizeof buf)) != 0) {
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = errno;
            break;
        }
        sha1_add(&s, buf, (size_t)got);
        total += (size_t)got;
    }
    if (fd >= 0) {
        close(fd);
    }
    file_as_user(0);
    if (!err && total != (size_t)st.st_size) {
        err = EAGAIN; /* it changed while read: read again later */
    }
    if (!err) {
        sha1_end(&s, digest);
    }
    return err;
}

/* As KTextEditor names them (QStringConverter's names). */
const char *file_encoding_name(const struct file_format *f)
{
    switch (f->encoding) {
    case ENC_UTF16LE:
        return "UTF-16LE";
    case ENC_UTF16BE:
        return "UTF-16BE";
    case ENC_GB18030:
        return "GB18030";
    case ENC_LATIN1:
        return "ISO-8859-1";
    case ENC_WINDOWS1252:
        return "WINDOWS-1252";
    default:
        return "UTF-8";
    }
}

const char *file_eol_name(int eol)
{
    return eol == EOL_CRLF ? "CRLF" : eol == EOL_CR ? "CR" : "LF";
}
