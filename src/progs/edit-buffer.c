/*
 * edit's text (edit-buffer.h): a gap buffer, so that typing at one place
 * moves nothing but the gap, and the start of each line, so that a line is
 * found at once in a file of millions of them. An edit shifts the starts of
 * the lines after it: one addition each.
 */
#include <stdlib.h>
#include <string.h>

#include "edit-buffer.h"

#define MIN_GAP 4096

int buffer_init(struct buffer *b)
{
    memset(b, 0, sizeof *b);
    b->data = malloc(MIN_GAP);
    b->line = malloc(16 * sizeof *b->line);
    if (!b->data || !b->line) {
        free(b->data);
        free(b->line);
        memset(b, 0, sizeof *b);
        return -1;
    }
    b->size = b->gap_end = MIN_GAP;
    b->line_size = 16;
    b->line[0] = 0;
    b->lines = 1;
    return 0;
}

void buffer_free(struct buffer *b)
{
    free(b->data);
    free(b->line);
    memset(b, 0, sizeof *b);
}

size_t buffer_length(const struct buffer *b)
{
    return b->size - (b->gap_end - b->gap);
}

static void move_gap(struct buffer *b, size_t at)
{
    if (at < b->gap) {
        size_t n = b->gap - at;
        memmove(b->data + b->gap_end - n, b->data + at, n);
        b->gap -= n;
        b->gap_end -= n;
    } else if (at > b->gap) {
        size_t n = at - b->gap;
        memmove(b->data + b->gap, b->data + b->gap_end, n);
        b->gap += n;
        b->gap_end += n;
    }
}

/* Room for len more bytes in the gap. */
static int make_room(struct buffer *b, size_t len)
{
    if (b->gap_end - b->gap >= len) {
        return 0;
    }
    size_t text = buffer_length(b), size = text + len + MIN_GAP + (text + len) / 2;
    char *data = realloc(b->data, size);
    if (!data) {
        return -1;
    }
    size_t tail = b->size - b->gap_end;
    memmove(data + size - tail, data + b->gap_end, tail);
    b->data = data;
    b->gap_end = size - tail;
    b->size = size;
    return 0;
}

static int lines_room(struct buffer *b, size_t more)
{
    if (b->lines + more <= b->line_size) {
        return 0;
    }
    size_t size = (b->lines + more) + (b->lines + more) / 2 + 16;
    size_t *line = realloc(b->line, size * sizeof *line);
    if (!line) {
        return -1;
    }
    b->line = line;
    b->line_size = size;
    return 0;
}

/* The line starts of the whole text again. */
static int index_lines(struct buffer *b)
{
    size_t n = 1, len = buffer_length(b);
    for (size_t at = 0; at < len;) {
        size_t chunk;
        const char *p = buffer_chunk(b, at, &chunk), *end = p + chunk;
        for (const char *q = p; (q = memchr(q, '\n', (size_t)(end - q))) != NULL; q++) {
            n++;
        }
        at += chunk;
    }
    if (n > b->line_size) {
        size_t *line = realloc(b->line, (n + n / 2 + 16) * sizeof *line);
        if (!line) {
            return -1;
        }
        b->line = line;
        b->line_size = n + n / 2 + 16;
    }
    b->line[0] = 0;
    b->lines = 1;
    for (size_t at = 0; at < len;) {
        size_t chunk;
        const char *p = buffer_chunk(b, at, &chunk), *end = p + chunk;
        for (const char *q = p; (q = memchr(q, '\n', (size_t)(end - q))) != NULL; q++) {
            b->line[b->lines++] = at + (size_t)(q - p) + 1;
        }
        at += chunk;
    }
    return 0;
}

int buffer_set(struct buffer *b, const char *text, size_t len)
{
    size_t size = len + MIN_GAP;
    char *data = malloc(size);
    if (!data) {
        return -1;
    }
    memcpy(data, text, len);
    char *old = b->data;
    size_t og = b->gap, oge = b->gap_end, os = b->size;
    b->data = data;
    b->gap = len;
    b->gap_end = b->size = size;
    if (index_lines(b) < 0) {
        b->data = old;
        b->gap = og;
        b->gap_end = oge;
        b->size = os;
        free(data);
        return -1;
    }
    free(old);
    return 0;
}

int buffer_take(struct buffer *b, char *text, size_t len)
{
    char *data = realloc(text, len + MIN_GAP);
    if (!data) {
        free(text);
        return -1;
    }
    char *old = b->data;
    size_t og = b->gap, oge = b->gap_end, os = b->size;
    b->data = data;
    b->gap = len;
    b->gap_end = b->size = len + MIN_GAP;
    if (index_lines(b) < 0) {
        b->data = old;
        b->gap = og;
        b->gap_end = oge;
        b->size = os;
        free(data);
        return -1;
    }
    free(old);
    return 0;
}

size_t buffer_line_of(const struct buffer *b, size_t at)
{
    size_t lo = 0, hi = b->lines;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (b->line[mid] <= at) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

int buffer_insert(struct buffer *b, size_t at, const char *text, size_t len)
{
    size_t total = buffer_length(b), k = 0;
    if (at > total) {
        at = total;
    }
    for (const char *q = text; (q = memchr(q, '\n', len - (size_t)(q - text))) != NULL; q++) {
        k++;
    }
    if (make_room(b, len) < 0 || lines_room(b, k) < 0) {
        return -1;
    }
    move_gap(b, at);
    memcpy(b->data + b->gap, text, len);
    b->gap += len;
    size_t l = buffer_line_of(b, at);
    if (k) {
        memmove(&b->line[l + 1 + k], &b->line[l + 1], (b->lines - l - 1) * sizeof *b->line);
        size_t i = l + 1;
        for (const char *q = text; (q = memchr(q, '\n', len - (size_t)(q - text))) != NULL; q++) {
            b->line[i++] = at + (size_t)(q - text) + 1;
        }
        b->lines += k;
    }
    for (size_t i = l + 1 + k; i < b->lines; i++) {
        b->line[i] += len;
    }
    return 0;
}

void buffer_delete(struct buffer *b, size_t at, size_t len)
{
    size_t total = buffer_length(b);
    if (at >= total || len == 0) {
        return;
    }
    if (len > total - at) {
        len = total - at;
    }
    size_t l1 = buffer_line_of(b, at), l2 = buffer_line_of(b, at + len);
    move_gap(b, at);
    b->gap_end += len;
    if (l2 > l1) {
        memmove(&b->line[l1 + 1], &b->line[l2 + 1], (b->lines - l2 - 1) * sizeof *b->line);
        b->lines -= l2 - l1;
    }
    for (size_t i = l1 + 1; i < b->lines; i++) {
        b->line[i] -= len;
    }
}

int buffer_replace_many(struct buffer *b, const struct replacement *r, size_t n)
{
    size_t len = buffer_length(b), out = len;
    for (size_t i = 0; i < n; i++) {
        out = out - r[i].len + r[i].new_len;
    }
    size_t size = out + MIN_GAP;
    char *data = malloc(size);
    if (!data) {
        return -1;
    }
    size_t from = 0, to = 0;
    for (size_t i = 0; i < n; i++) {
        buffer_copy(b, from, r[i].at - from, data + to);
        to += r[i].at - from;
        memcpy(data + to, r[i].text, r[i].new_len);
        to += r[i].new_len;
        from = r[i].at + r[i].len;
    }
    buffer_copy(b, from, len - from, data + to);
    struct buffer next = *b;
    next.data = data;
    next.gap = out;
    next.gap_end = next.size = size;
    if (index_lines(&next) < 0) {
        free(data);
        return -1;
    }
    free(b->data);
    *b = next;
    return 0;
}

int buffer_byte(const struct buffer *b, size_t at)
{
    if (at < b->gap) {
        return (unsigned char)b->data[at];
    }
    at += b->gap_end - b->gap;
    return at < b->size ? (unsigned char)b->data[at] : -1;
}

void buffer_copy(const struct buffer *b, size_t at, size_t len, char *out)
{
    if (at < b->gap) {
        size_t n = b->gap - at < len ? b->gap - at : len;
        memcpy(out, b->data + at, n);
        out += n;
        at += n;
        len -= n;
    }
    if (len) {
        memcpy(out, b->data + b->gap_end + (at - b->gap), len);
    }
}

char *buffer_dup(const struct buffer *b, size_t at, size_t len)
{
    char *s = malloc(len + 1);
    if (s) {
        buffer_copy(b, at, len, s);
        s[len] = '\0';
    }
    return s;
}

const char *buffer_chunk(const struct buffer *b, size_t at, size_t *len)
{
    if (at < b->gap) {
        *len = b->gap - at;
        return b->data + at;
    }
    size_t total = buffer_length(b);
    *len = at < total ? total - at : 0;
    return b->data + b->gap_end + (at - b->gap);
}

const char *buffer_text(struct buffer *b)
{
    move_gap(b, buffer_length(b));
    return b->data;
}

size_t buffer_line_start(const struct buffer *b, size_t line)
{
    return line < b->lines ? b->line[line] : buffer_length(b);
}

size_t buffer_line_length(const struct buffer *b, size_t line)
{
    if (line >= b->lines) {
        return 0;
    }
    size_t end = line + 1 < b->lines ? b->line[line + 1] - 1 : buffer_length(b);
    return end - b->line[line];
}

/* As screen.c's utf8(): a sequence whose bytes are all there and all
 * continuations, else the one byte as "?". */
uint32_t text_decode(const char *s, size_t len, size_t *bytes)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char c = p[0];
    *bytes = 1;
    if (c < 0x80) {
        return c;
    }
    int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    if (n == 0 || (size_t)n + 1 > len) {
        return '?';
    }
    uint32_t cp = c & (0x3Fu >> n);
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return '?';
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *bytes = (size_t)n + 1;
    return cp;
}

int text_put(char *out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | ((cp >> 18) & 0x07));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* The combining blocks of Unicode, the variation selectors and the joiner:
 * what continues a grapheme cluster in the common scripts. */
int text_combining(uint32_t cp)
{
    static const uint32_t ranges[][2] = {
        { 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x0591, 0x05BD }, { 0x05BF, 0x05BF },
        { 0x05C1, 0x05C2 }, { 0x05C4, 0x05C5 }, { 0x05C7, 0x05C7 }, { 0x0610, 0x061A },
        { 0x064B, 0x065F }, { 0x0670, 0x0670 }, { 0x06D6, 0x06DC }, { 0x06DF, 0x06E4 },
        { 0x06E7, 0x06E8 }, { 0x06EA, 0x06ED }, { 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A },
        { 0x0E47, 0x0E4E }, { 0x1AB0, 0x1AFF }, { 0x1DC0, 0x1DFF }, { 0x200D, 0x200D },
        { 0x20D0, 0x20FF }, { 0x302A, 0x302F }, { 0x3099, 0x309A }, { 0xFE00, 0xFE0F },
        { 0xFE20, 0xFE2F }, { 0x1F3FB, 0x1F3FF }, { 0xE0100, 0xE01EF },
    };
    if (cp < 0x300) {
        return 0;
    }
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
        if (cp >= ranges[i][0] && cp <= ranges[i][1]) {
            return 1;
        }
    }
    return 0;
}

uint32_t buffer_char(const struct buffer *b, size_t at, size_t *bytes)
{
    char s[4];
    size_t total = buffer_length(b), n = total - at < 4 ? total - at : 4;
    if (n == 0) {
        *bytes = 0;
        return 0;
    }
    buffer_copy(b, at, n, s);
    return text_decode(s, n, bytes);
}

size_t buffer_next_char(const struct buffer *b, size_t at)
{
    size_t n;
    buffer_char(b, at, &n);
    return at + n;
}

size_t buffer_prev_char(const struct buffer *b, size_t at)
{
    if (at == 0) {
        return 0;
    }
    /* The lead byte up to three back whose sequence ends here; else one byte. */
    for (size_t k = 2; k <= 4 && k <= at; k++) {
        int c = buffer_byte(b, at - k + 1);
        if ((c & 0xC0) != 0x80) {
            break;
        }
        int lead = buffer_byte(b, at - k);
        if ((lead & 0xC0) != 0x80) {
            size_t n;
            buffer_char(b, at - k, &n);
            return n == k ? at - k : at - 1;
        }
    }
    return at - 1;
}

size_t buffer_next_cluster(const struct buffer *b, size_t at)
{
    size_t total = buffer_length(b), n;
    if (at >= total) {
        return total;
    }
    uint32_t cp = buffer_char(b, at, &n);
    at += n;
    if (cp == '\n') {
        return at;
    }
    while (at < total) {
        uint32_t next = buffer_char(b, at, &n);
        if (!text_combining(next) && !(cp == 0x200D && next != '\n')) {
            break;
        }
        cp = next;
        at += n;
    }
    return at;
}

size_t buffer_prev_cluster(const struct buffer *b, size_t at)
{
    while (at > 0) {
        size_t p = buffer_prev_char(b, at), n;
        uint32_t cp = buffer_char(b, p, &n);
        at = p;
        if (cp == '\n' || !text_combining(cp)) {
            /* A joiner before it joins it to what is before. */
            if (at > 0) {
                size_t q = buffer_prev_char(b, at);
                if (buffer_char(b, q, &n) == 0x200D && cp != '\n') {
                    continue;
                }
            }
            break;
        }
    }
    return at;
}
