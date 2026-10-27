/* edit's search and replace (edit-search.c), with PCRE2, in the modes of
 * Kate's search bar: plain text, whole words, escape sequences and regular
 * expressions, each with or without matching case. */
#ifndef LOCACHY_EDIT_SEARCH_H
#define LOCACHY_EDIT_SEARCH_H

#include <stddef.h>

#include "edit-buffer.h"

enum { SEARCH_PLAIN, SEARCH_WORDS, SEARCH_ESCAPES, SEARCH_REGEX };

#define SEARCH_GROUPS 100 /* \0 .. \99, and \{n} up to it */

struct search {
    void *re;             /* pcre2_code */
    void *match;          /* pcre2_match_data */
    int mode, match_case, multiline, groups;
    char error[160];      /* why the pattern cannot be used */
    /* The last match: its groups' bytes ((size_t)-1: did not take part). */
    size_t group_start[SEARCH_GROUPS], group_end[SEARCH_GROUPS];
};

/* Kate's isPatternValid: not empty, without spaces around it in whole words
 * mode, and a regular expression that compiles. */
int search_valid(const char *pattern, int mode);
/* pattern in mode: 0, or -1 with the reason in s->error. */
int search_compile(struct search *s, const char *pattern, int mode, int match_case);
void search_free(struct search *s);
/* The first match from at onwards (or the last one ending at or before at,
 * backwards) within [lo, hi): 1 with it in *start and *end, else 0. Lines
 * are matched one by one, as Kate does unless the pattern has a line break
 * (\n, or \x or \0 that may be one). */
int search_find(struct search *s, struct buffer *b, size_t at, int backwards, size_t lo, size_t hi,
                size_t *start, size_t *end);
/* The text replacing the last match: replace with Kate's escapes in the
 * regular expression and escape sequence modes when it has a backslash
 * (KateRegExpSearch::buildReplacement: \0..\9 and \{n} the groups, \0ooo,
 * \xhhhh, \a \f \n \r \t \v, \L \l \U \u \E, \# the counter), else as it
 * is. malloc'd, its length in *len. */
char *search_replacement(struct search *s, struct buffer *b, const char *replace, int counter, size_t *len);
/* Kate's escape sequences in text (as its escape sequence mode reads a
 * pattern): malloc'd, its length in *len. */
char *search_unescape(const char *text, size_t *len);

#endif
