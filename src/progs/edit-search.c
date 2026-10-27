/*
 * edit's search (edit-search.h) with PCRE2, as KTextEditor's search bar
 * searches (src/search/katesearchbar.cpp, kateregexpsearch.cpp,
 * kateplaintextsearch.cpp): plain text as it is typed; whole words as the
 * plain text between \b marks; escape sequences as plain text with Kate's
 * escapes read first; and regular expressions with Unicode properties for \w
 * and \b. Matching case is a choice. The text is matched a line at a time,
 * so that ^ and $ are a line's start and end and nothing matches a line
 * break, unless the pattern may match one (\n, \x...., \0...), as
 * KateRegExpSearch does; there, as in Kate, \s is [ \t].
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "base.h"
#include "edit-search.h"

void search_free(struct search *s)
{
    pcre2_match_data_free(s->match);
    pcre2_code_free(s->re);
    s->re = s->match = NULL;
}

/* The output of buildReplacement, with its case conversion. */
enum { KEEP, UPPER, UPPER_FIRST, LOWER, LOWER_FIRST };
struct out {
    char *s;
    size_t n, size;
    int conv;
};

static void out_raw(struct out *o, const char *p, size_t n)
{
    if (o->n + n + 8 > o->size) {
        size_t size = (o->n + n + 8) * 2;
        char *s = realloc(o->s, size);
        if (!s) {
            return;
        }
        o->s = s;
        o->size = size;
    }
    memcpy(o->s + o->n, p, n);
    o->n += n;
}

/* A piece of text, converted as the last \U \u \L \l \E says (\u and \l
 * only its first character, and then no more). */
static void out_text(struct out *o, const char *p, size_t n)
{
    if (n == 0) {
        return;
    }
    if (o->conv == KEEP) {
        out_raw(o, p, n);
        return;
    }
    for (size_t i = 0; i < n;) {
        size_t k;
        uint32_t cp = text_decode(p + i, n - i, &k);
        if (k == 1 && (unsigned char)p[i] >= 0x80) {
            out_raw(o, p + i, 1); /* a byte that is no character stays */
        } else {
            if (o->conv == UPPER || o->conv == UPPER_FIRST) {
                cp = (uint32_t)towupper((wint_t)cp);
            } else {
                cp = (uint32_t)towlower((wint_t)cp);
            }
            char e[4];
            out_raw(o, e, (size_t)text_put(e, cp));
        }
        i += k;
        if (o->conv == UPPER_FIRST || o->conv == LOWER_FIRST) {
            o->conv = KEEP;
            out_raw(o, p + i, n - i);
            return;
        }
    }
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* KateRegExpSearch::buildReplacement: goodies are the replacement's own
 * (case conversion, the counter); without them it reads escape sequences. */
static void build(struct out *o, const char *in, struct search *s, struct buffer *b, int counter, int goodies)
{
    size_t len = strlen(in);
    for (size_t i = 0; i < len;) {
        if (in[i] != '\\') {
            out_text(o, in + i, 1);
            i++;
            continue;
        }
        if (i + 1 >= len) {
            out_text(o, in + i, 1);
            i++;
            continue;
        }
        char c = in[i + 1];
        int cap = -1;
        size_t digits_from = 0, digits_to = 0; /* a \{nnn} number, as text when it names no group */
        if (c == '0') {
            /* \0ooo (first digit 0..3), else the whole match */
            if (i + 4 < len && in[i + 2] >= '0' && in[i + 2] <= '3' && in[i + 3] >= '0' &&
                in[i + 3] <= '7' && in[i + 4] >= '0' && in[i + 4] <= '7') {
                char e[4];
                uint32_t v = (uint32_t)((in[i + 2] - '0') * 64 + (in[i + 3] - '0') * 8 + (in[i + 4] - '0'));
                out_text(o, e, (size_t)text_put(e, v));
                i += 5;
                continue;
            }
            cap = 0;
            i += 2;
        } else if (c >= '1' && c <= '9') {
            cap = c - '0';
            i += 2;
        } else if (c == '{') {
            /* \{nnn}: any group. A number past int names no group: cap goes
             * to -1 and its digits are copied as text. */
            size_t k = i + 2;
            digits_from = k;
            cap = 0;
            while (k < len && in[k] >= '0' && in[k] <= '9') {
                cap = cap >= 0 && cap <= (INT_MAX - 9) / 10 ? cap * 10 + (in[k] - '0') : -1;
                k++;
            }
            digits_to = k;
            if (k < len && in[k] == '}') {
                k++;
            }
            i = k;
        } else if (strchr("ELlUu", c)) {
            if (!goodies) {
                out_text(o, in + i + 1, 1);
            } else {
                o->conv = c == 'L' ? LOWER : c == 'l' ? LOWER_FIRST : c == 'U' ? UPPER : c == 'u' ? UPPER_FIRST : KEEP;
            }
            i += 2;
            continue;
        } else if (c == '#') {
            if (!goodies) {
                out_text(o, "#", 1);
                i += 2;
                continue;
            }
            /* \#, \##...: the counter, a digit for each # at least, zero-padded
             * as %0*d pads it (the sign first); any number of #s. */
            int width = 1;
            while (i + 1 + (size_t)width < len && in[i + 1 + width] == '#') {
                width++;
            }
            char num[16];
            int n = snprintf(num, sizeof num, "%d", counter), sign = counter < 0;
            if (sign) {
                out_raw(o, "-", 1);
            }
            for (int pad = n - sign; pad < width; pad++) {
                out_raw(o, "0", 1);
            }
            out_raw(o, num + sign, (size_t)(n - sign));
            i += 1 + (size_t)width;
            continue;
        } else if (strchr("afnrtv", c)) {
            static const char codes[] = "\a\f\n\r\t\v", names[] = "afnrtv";
            out_text(o, &codes[strchr(names, c) - names], 1);
            i += 2;
            continue;
        } else if (c == 'x') {
            if (i + 5 < len && hexval(in[i + 2]) >= 0 && hexval(in[i + 3]) >= 0 && hexval(in[i + 4]) >= 0 &&
                hexval(in[i + 5]) >= 0) {
                uint32_t v = (uint32_t)(hexval(in[i + 2]) << 12 | hexval(in[i + 3]) << 8 | hexval(in[i + 4]) << 4 |
                                        hexval(in[i + 5]));
                char e[4];
                out_text(o, e, (size_t)text_put(e, v));
                i += 6;
            } else {
                out_text(o, "x", 1);
                i += 2;
            }
            continue;
        } else {
            /* any other \c: c (so \\ is \) */
            size_t k;
            text_decode(in + i + 1, len - i - 1, &k);
            out_text(o, in + i + 1, k);
            i += 1 + k;
            continue;
        }
        /* A group: its text, or its number when there is no such group. */
        if (s && b && cap >= 0 && cap < s->groups && cap < SEARCH_GROUPS) {
            if (s->group_start[cap] != (size_t)-1) {
                char *t = buffer_dup(b, s->group_start[cap], s->group_end[cap] - s->group_start[cap]);
                if (t) {
                    out_text(o, t, s->group_end[cap] - s->group_start[cap]);
                    free(t);
                }
            }
        } else if (cap < 0) {
            /* a \{nnn} too large for any group: its digits, without leading zeros */
            size_t z = digits_from;
            while (z + 1 < digits_to && in[z] == '0') {
                z++;
            }
            out_raw(o, in + z, digits_to - z);
        } else {
            char num[16];
            int n = snprintf(num, sizeof num, "%d", cap);
            out_raw(o, num, (size_t)n);
        }
    }
}

char *search_unescape(const char *text, size_t *len)
{
    struct out o = { NULL, 0, 0, KEEP };
    out_raw(&o, "", 0);
    build(&o, text, NULL, NULL, 0, 0);
    if (!o.s) {
        return NULL;
    }
    o.s[o.n] = '\0';
    *len = o.n;
    return o.s;
}

char *search_replacement(struct search *s, struct buffer *b, const char *replace, int counter, size_t *len)
{
    struct out o = { NULL, 0, 0, KEEP };
    out_raw(&o, "", 0);
    if ((s->mode == SEARCH_REGEX || s->mode == SEARCH_ESCAPES) && strchr(replace, '\\')) {
        build(&o, replace, s, b, counter, 1);
    } else {
        out_raw(&o, replace, strlen(replace));
    }
    if (!o.s) {
        return NULL;
    }
    o.s[o.n] = '\0';
    *len = o.n;
    return o.s;
}

/* KateRegExpSearch::repairPattern: \s as [ \t], and whether the pattern may
 * match a line break (\n, \x...., \0...). */
static char *repair(const char *p, int *multiline)
{
    size_t len = strlen(p), o = 0;
    char *out = malloc(len * 4 + 1);
    int in_class = 0;
    *multiline = 0;
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < len;) {
        if (p[i] == '\\' && i + 1 < len) {
            char c = p[i + 1];
            size_t take = 2;
            if (c == 'x' || c == '0') {
                *multiline = 1;
                take = c == 'x' ? (i + 5 < len ? 6 : 2) : (i + 4 < len ? 5 : 2);
            } else if (c == 's') {
                const char *with = in_class ? " \\t" : "[ \\t]";
                memcpy(out + o, with, strlen(with));
                o += strlen(with);
                i += 2;
                continue;
            } else if (c == 'n') {
                *multiline = 1;
            }
            memcpy(out + o, p + i, take);
            o += take;
            i += take;
            continue;
        }
        if (!in_class && p[i] == '[') {
            in_class = 1;
        } else if (in_class && p[i] == ']') {
            in_class = 0;
        }
        out[o++] = p[i++];
    }
    out[o] = '\0';
    return out;
}

int search_valid(const char *pattern, int mode)
{
    size_t n = strlen(pattern);
    if (n == 0) {
        return 0;
    }
    if (mode == SEARCH_WORDS) {
        return pattern[0] != ' ' && pattern[0] != '\t' && pattern[n - 1] != ' ' && pattern[n - 1] != '\t';
    }
    if (mode == SEARCH_REGEX) {
        int code;
        PCRE2_SIZE offset;
        pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, n, PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF, &code,
                                       &offset, NULL);
        pcre2_code_free(re);
        return re != NULL;
    }
    return 1;
}

int search_compile(struct search *s, const char *pattern, int mode, int match_case)
{
    search_free(s);
    s->error[0] = '\0';
    s->mode = mode;
    s->match_case = match_case;
    uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF | PCRE2_MULTILINE;
    if (!match_case) {
        options |= PCRE2_CASELESS;
    }
    char *p = NULL;
    if (mode == SEARCH_REGEX) {
        p = repair(pattern, &s->multiline);
    } else {
        /* Plain text: the characters as they are, between \b for words. */
        size_t n;
        char *text = mode == SEARCH_ESCAPES ? search_unescape(pattern, &n) : strdup(pattern);
        if (text) {
            n = strlen(text);
            p = malloc(n * 6 + 16);
        }
        if (p) {
            size_t o = 0;
            if (mode == SEARCH_WORDS) {
                o += (size_t)sprintf(p + o, "\\b");
            }
            o += (size_t)sprintf(p + o, "\\Q");
            for (size_t i = 0; i < n; i++) {
                if (text[i] == '\\' && i + 1 < n && text[i + 1] == 'E') {
                    o += (size_t)sprintf(p + o, "\\E\\\\\\Q"); /* \E would end \Q */
                } else if (text[i] == '\n') {
                    o += (size_t)sprintf(p + o, "\\E\\n\\Q");
                } else {
                    p[o++] = text[i];
                }
            }
            o += (size_t)sprintf(p + o, "\\E");
            if (mode == SEARCH_WORDS) {
                o += (size_t)sprintf(p + o, "\\b");
            }
            p[o] = '\0';
            s->multiline = strchr(text, '\n') != NULL;
        }
        free(text);
    }
    if (!p) {
        snprintf(s->error, sizeof s->error, "%s", _("No memory"));
        return -1;
    }
    int code;
    PCRE2_SIZE offset;
    s->re = pcre2_compile((PCRE2_SPTR)p, PCRE2_ZERO_TERMINATED, options, &code, &offset, NULL);
    free(p);
    if (!s->re) {
        PCRE2_UCHAR msg[120];
        pcre2_get_error_message(code, msg, sizeof msg);
        snprintf(s->error, sizeof s->error, "%s", (char *)msg);
        return -1;
    }
    pcre2_jit_compile(s->re, PCRE2_JIT_COMPLETE);
    s->match = pcre2_match_data_create_from_pattern(s->re, NULL);
    if (!s->match) {
        search_free(s);
        snprintf(s->error, sizeof s->error, "%s", _("No memory"));
        return -1;
    }
    uint32_t count = 0;
    pcre2_pattern_info(s->re, PCRE2_INFO_CAPTURECOUNT, &count);
    s->groups = mode == SEARCH_REGEX ? (int)count + 1 : 1;
    return 0;
}

/* A match in subject [0, len) from from, starting before to: 1 with its
 * groups kept (offset by base), else 0. The subject ends at len, so $ is
 * there. */
static int match_in(struct search *s, const char *text, size_t len, size_t from, size_t to, size_t base)
{
    if (from > len) {
        return 0;
    }
    int rc = pcre2_match(s->re, (PCRE2_SPTR)text, len, from, 0, s->match, NULL);
    if (rc < 0) {
        return 0;
    }
    PCRE2_SIZE *ov = pcre2_get_ovector_pointer(s->match);
    if (ov[0] >= to) {
        return 0;
    }
    uint32_t pairs = pcre2_get_ovector_count(s->match);
    for (int g = 0; g < SEARCH_GROUPS; g++) {
        if ((uint32_t)g < pairs && g < rc && ov[2 * g] != PCRE2_UNSET) {
            s->group_start[g] = base + ov[2 * g];
            s->group_end[g] = base + ov[2 * g + 1];
        } else {
            s->group_start[g] = s->group_end[g] = (size_t)-1;
        }
    }
    return 1;
}

/* The last match in subject [0, len) starting from first and ending at or
 * before limit: 1 with its groups kept. */
static int last_in(struct search *s, const char *text, size_t len, size_t first, size_t limit, size_t base)
{
    size_t best = (size_t)-1;
    for (size_t from = first; from <= limit;) {
        if (!match_in(s, text, len, from, limit + 1, base) || s->group_end[0] - base > limit) {
            break;
        }
        best = s->group_start[0] - base;
        size_t e = s->group_end[0] - base;
        from = e > best ? e : best + 1;
    }
    if (best == (size_t)-1) {
        return 0;
    }
    return match_in(s, text, len, best, best + 1, base);
}

int search_find(struct search *s, struct buffer *b, size_t at, int backwards, size_t lo, size_t hi, size_t *start,
                size_t *end)
{
    if (!s->re) {
        return 0;
    }
    const char *text = buffer_text(b);
    size_t total = buffer_length(b);
    hi = hi > total ? total : hi;
    at = at < lo ? lo : at > hi ? hi : at;
    int found;
    if (s->multiline) {
        /* The range as one subject. */
        found = backwards ? last_in(s, text, hi, lo, at, 0) : match_in(s, text, hi, at, hi + 1, 0);
        if (found) {
            *start = s->group_start[0];
            *end = s->group_end[0];
        }
        return found;
    }
    size_t line = buffer_line_of(b, at);
    if (!backwards) {
        for (size_t l = line; l < b->lines; l++) {
            size_t ls = buffer_line_start(b, l), le = ls + buffer_line_length(b, l);
            if (ls > hi) {
                break;
            }
            size_t from = l == line ? at : ls, stop = le < hi ? le : hi;
            from = from < ls ? ls : from;
            if (from > stop) {
                continue;
            }
            /* The subject is the line: its start is ^, its end $. */
            if (match_in(s, text + ls, stop - ls, from - ls, stop - ls + 1, ls)) {
                *start = s->group_start[0];
                *end = s->group_end[0];
                return 1;
            }
        }
        return 0;
    }
    for (size_t l = line + 1; l-- > 0;) {
        size_t ls = buffer_line_start(b, l), le = ls + buffer_line_length(b, l);
        if (le < lo) {
            break;
        }
        size_t first = ls < lo ? lo : ls, stop = le < hi ? le : hi;
        size_t limit = l == line ? at : stop;
        if (first <= limit && last_in(s, text + ls, stop - ls, first - ls, limit - ls, ls)) {
            *start = s->group_start[0];
            *end = s->group_end[0];
            return 1;
        }
    }
    return 0;
}
