/*
 * edit's syntax highlighting (edit-syntax.h), with tree-sitter as its
 * highlighter (tree-sitter-highlight) uses it: the grammar parses the text,
 * again after each edit only where it changed (ts_tree_edit), and the
 * highlights query is run over what is shown only. A node captured by
 * several patterns takes the last one's name, an inner node is drawn over
 * the outer one, and a name is matched to the theme's as tree-sitter matches
 * them (the theme's name with the most dot-separated parts that are all in
 * the capture's name: function.method takes function's colour).
 *
 * The query predicates #match?, #eq?, #any-of? (and their not- and any-
 * forms) are checked, #lua-match? with its pattern made a regular one;
 * #set! and #is-not? (local variables) and others pass, as they do in
 * tree-sitter's highlighter. C++'s query is C's followed by its own, as its
 * tree-sitter.json says. Markdown's inline parts (emphasis, code spans,
 * links) are parsed with its inline grammar, each inline node shown on its
 * own.
 *
 * The colours are those of Kate's default styles in its Breeze Dark theme
 * (KSyntaxHighlighting's breeze-dark.theme). The framebuffer font has one
 * weight, so Keyword (bold text of the normal colour there) is white.
 * syntax_styles separately reports the real font attributes, without
 * changing the foreground API used by older renderers.
 *
 * Folding uses the SAME completed tree, never a brace scan. Packaged
 * folds.scm takes precedence over the explicit per-grammar node rules.
 * Regions form a non-crossing hierarchy for Kate's current/top-level and
 * recursive actions; edits invalidate the whole exported index immediately.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <tree_sitter/api.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "edit-syntax.h"

/* Larger texts are not highlighted: tree-sitter keeps a tree of all of the
 * text, up to some 60 times its size for dense code (measured: 7 MiB of C,
 * 430 MiB), so 8 MiB of it can take half a GiB. Kate has no such limit: its
 * highlighting goes line by line, as far as is shown. */
#define SYNTAX_LIMIT (8u << 20)

struct lang {
    const char *name;      /* Kate's name for it */
    const char *grammar;   /* libtree-sitter-GRAMMAR.so, tree_sitter_GRAMMAR() */
    const char *queries;   /* the folder of its highlights.scm (with C's first for C++) */
    const char *exts;      /* name patterns, space separated: *.c, PKGBUILD */
    const char *programs;  /* interpreters of #! lines */
    const char *vim;       /* vim's filetype names */
    const char *line_mark; /* as Kate's syntax definitions have them */
    int after_space;
    const char *start_mark, *end_mark;
};

static const struct lang langs[] = {
    { "Bash", "bash", "bash",
      "*.sh *.bash *.ebuild *.eclass *.exlib *.exheres-0 .bashrc .bash_profile .bash_login .profile .envrc PKGBUILD APKBUILD",
      "sh bash dash ksh", "sh bash", "#", 1, NULL, NULL },
    { "C", "c", "c", "*.c *.h", "", "c", "//", 1, "/*", "*/" },
    { "C++", "cpp", "cpp",
      "*.c++ *.cxx *.cpp *.cc *.C *.cu *.hh *.H *.h++ *.hxx *.hpp *.hcc *.cuh *.inl *.ino *.pde *.moc *.mpp *.ixx *.cppm *.cxxm *.c++m *.ccm",
      "", "cpp", "//", 1, "/*", "*/" },
    { "JavaScript", "javascript", "javascript", "*.js *.mjs *.cjs *.kwinscript *.julius", "node nodejs gjs",
      "javascript javascriptreact", "//", 1, "/*", "*/" },
    { "Lua", "lua", "lua", "*.lua *.rockspec", "lua luajit", "lua", "--", 0, "--[[", "]]" },
    { "Markdown", "markdown", "markdown", "*.md *.mmd *.markdown *.md.html", "", "markdown", NULL, 0, "<!--", "-->" },
    { "Python", "python", "python", "*.py *.pyw *.pyi SConstruct SConscript *.FCMacro *.sage", "python python2 python3",
      "python", "#", 1, NULL, NULL },
    { "Tree-sitter Query", "query", "query", "*.scm", "", "query", ";", 0, NULL, NULL },
    { "Rust", "rust", "rust", "*.rs", "", "rust", "//", 1, "/*", "*/" },
    { "Vim", "vim", "vim", "*.vim .vimrc .gvimrc _vimrc .exrc", "", "vim", "\"", 0, NULL, NULL },
    { "Vim Help", "vimdoc", "vimdoc", "", "", "help vimdoc", NULL, 0, NULL, NULL },
};
#define NLANGS ((int)(sizeof langs / sizeof langs[0]))
#define MARKDOWN_INLINE NLANGS /* the loaded[] slot of markdown's inline grammar */

/* The theme: capture names as tree-sitter matches them, and their colour. */
static const struct style {
    const char *name;
    uint32_t colour;
    uint8_t flags;
} theme[] = {
    { "attribute", 0x2980B9, 0 },           /* Attribute */
    { "boolean", 0x27AEAE, SYNTAX_STYLE_BOLD },             /* Constant */
    { "character", 0x3DAEE9, 0 },           /* Char */
    { "character.special", 0x3DAEE9, 0 },   /* SpecialChar */
    { "comment", 0x7A7C7D, 0 },             /* Comment */
    { "comment.documentation", 0xA43340, 0 }, /* Documentation */
    { "comment.error", 0xDA4453, 0 },       /* Warning */
    { "comment.warning", 0xDA4453, 0 },
    { "comment.hint", 0xC45B00, 0 },        /* Information */
    { "comment.note", 0xC45B00, 0 },
    { "conditional", 0xFDBC4B, SYNTAX_STYLE_BOLD },         /* ControlFlow */
    { "constant", 0x27AEAE, SYNTAX_STYLE_BOLD },            /* Constant */
    { "constant.builtin", 0x27AEAE, SYNTAX_STYLE_BOLD },
    { "constructor", 0x8E44AD, 0 },         /* Function */
    { "delimiter", 0x3F8058, 0 },           /* Operator (Kate's symbols) */
    { "embedded", COLOUR_NORMAL, 0 },
    { "escape", 0x3DAEE9, 0 },              /* SpecialChar */
    { "exception", 0xFDBC4B, SYNTAX_STYLE_BOLD },           /* ControlFlow */
    { "field", COLOUR_NORMAL, 0 },
    { "float", 0xF67400, 0 },               /* Float */
    { "function", 0x8E44AD, 0 },            /* Function */
    { "function.builtin", 0x609CA0, 0 },    /* BuiltIn */
    { "function.macro", 0x27AE60, 0 },      /* Preprocessor */
    { "function.special", 0x27AE60, 0 },
    { "include", 0x27AE60, 0 },             /* Import */
    { "keyword", 0xFFFFFF, SYNTAX_STYLE_BOLD },             /* Keyword: bold Normal in the theme */
    { "keyword.conditional", 0xFDBC4B, SYNTAX_STYLE_BOLD }, /* ControlFlow */
    { "keyword.repeat", 0xFDBC4B, SYNTAX_STYLE_BOLD },
    { "keyword.return", 0xFDBC4B, SYNTAX_STYLE_BOLD },
    { "keyword.exception", 0xFDBC4B, SYNTAX_STYLE_BOLD },
    { "keyword.directive", 0x27AE60, 0 },   /* Preprocessor */
    { "keyword.import", 0x27AE60, 0 },      /* Import */
    { "label", 0x27AE60, 0 },               /* Others */
    { "markup.heading", 0x8E44AD, SYNTAX_STYLE_BOLD },      /* Markdown's headers: Function */
    { "markup.link", 0x27AE60, SYNTAX_STYLE_UNDERLINE },         /* Others */
    { "markup.raw", 0xC45B00, 0 },          /* Markdown's code: Information */
    { "markup.strong", 0xFFFFFF, SYNTAX_STYLE_BOLD },
    { "module", COLOUR_NORMAL, 0 },
    { "none", COLOUR_NORMAL, 0 },
    { "number", 0xF67400, 0 },              /* DecVal */
    { "operator", 0x3F8058, 0 },            /* Operator */
    { "preproc", 0x27AE60, 0 },             /* Preprocessor */
    { "property", COLOUR_NORMAL, 0 },
    { "punctuation", 0x3F8058, 0 },         /* Operator */
    { "punctuation.special", 0xDA4453, 0 }, /* Markdown's lists and markers: SpecialString */
    { "repeat", 0xFDBC4B, SYNTAX_STYLE_BOLD },              /* ControlFlow */
    { "string", 0xF44F4F, 0 },              /* String */
    { "string.escape", 0x3DAEE9, 0 },       /* SpecialChar */
    { "string.regexp", 0xDA4453, 0 },       /* SpecialString */
    { "string.special", 0xDA4453, 0 },
    { "string.special.url", 0x27AE60, SYNTAX_STYLE_UNDERLINE },  /* Others */
    { "tag", 0xFFFFFF, SYNTAX_STYLE_BOLD },
    { "text.emphasis", COLOUR_NORMAL, SYNTAX_STYLE_ITALIC },
    { "text.literal", 0xC45B00, 0 },        /* Information */
    { "text.reference", 0x27AE60, SYNTAX_STYLE_UNDERLINE },      /* Others */
    { "text.strong", 0xFFFFFF, SYNTAX_STYLE_BOLD },
    { "text.title", 0x8E44AD, SYNTAX_STYLE_BOLD },          /* Function */
    { "text.uri", 0x27AE60, SYNTAX_STYLE_UNDERLINE },            /* Others */
    { "type", 0x2980B9, 0 },                /* DataType */
    { "type.builtin", 0x2980B9, 0 },
    { "variable", COLOUR_NORMAL, 0 },
    { "variable.builtin", 0x609CA0, 0 },    /* BuiltIn */
    { "variable.parameter", COLOUR_NORMAL, 0 },
};
#define NTHEME ((int)(sizeof theme / sizeof theme[0]))

enum { PRED_EQ, PRED_MATCH, PRED_ANY_OF };

struct predicate {
    int kind, negate, any;
    uint32_t capture;          /* the capture tested */
    int other;                 /* PRED_EQ with a second capture: its id, else -1 */
    char *text;                /* PRED_EQ's string */
    pcre2_code *re;            /* PRED_MATCH */
    char **words;              /* PRED_ANY_OF */
    int nwords;
};

struct pattern {
    struct predicate *p;
    int n;
};

/* A grammar and its query, loaded once for all documents. */
struct loaded {
    int tried;
    void *dl;
    const TSLanguage *language;
    TSQuery *query;
    int *style;                /* the theme's index for each capture, or -1 */
    struct pattern *patterns;
    struct loaded *folds;     /* optional folds.scm, same grammar/predicates */
    char why[640];
};
static struct loaded loaded[NLANGS + 1];

struct syntax {
    int lang;
    TSParser *parser;
    TSTree *tree;
    int dirty, off, partial;
    char off_why[128];
    const char *last_capture;
    TSParser *inline_parser;   /* Markdown's */
    char *scratch;             /* a read across the buffer's gap */
    struct syntax_fold *folds;
    size_t nfolds;
    int folds_ready;
    uint64_t epoch;
};

static void invalidate_folds(struct syntax *s)
{
    free(s->folds);
    s->folds = NULL;
    s->nfolds = 0;
    s->folds_ready = 0;
    s->epoch++;
}

uint64_t syntax_epoch(const struct syntax *s)
{
    return s ? s->epoch : 0;
}

int syntax_count(void)
{
    return NLANGS;
}

const char *syntax_name(int lang)
{
    return lang >= 0 && lang < NLANGS ? langs[lang].name : "Normal";
}

int syntax_by_name(const char *name)
{
    if (strcasecmp(name, "ISO C++") == 0) {
        name = "C++";
    }
    for (int i = 0; i < NLANGS; i++) {
        if (strcasecmp(langs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

int syntax_lang(const struct syntax *s)
{
    return s ? s->lang : -1;
}

void syntax_comment_marks(int lang, const char **line, int *after_space, const char **start, const char **end)
{
    *line = *start = *end = NULL;
    *after_space = 0;
    if (lang >= 0 && lang < NLANGS) {
        *line = langs[lang].line_mark;
        *after_space = langs[lang].after_space;
        *start = langs[lang].start_mark;
        *end = langs[lang].end_mark;
    }
}

/* name matches a pattern of exts: *.EXT or a whole name. */
static int name_matches(const char *name, const char *exts)
{
    char copy[256];
    snprintf(copy, sizeof copy, "%s", exts);
    for (char *save, *w = strtok_r(copy, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        if (w[0] == '*') {
            size_t n = strlen(name), k = strlen(w + 1);
            if (n > k && strcmp(name + n - k, w + 1) == 0) {
                return 1;
            }
        } else if (strcmp(name, w) == 0) {
            return 1;
        }
    }
    return 0;
}

static int word_in(const char *word, size_t len, const char *list)
{
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, " ");
        if (n == len && strncmp(p, word, len) == 0) {
            return 1;
        }
        p += n;
        p += *p == ' ';
    }
    return 0;
}

/* A modeline in line: Kate's "kate: hl NAME;" or vim's "vim: ... ft=NAME". */
static int modeline(const char *line)
{
    const char *k = strstr(line, "kate:");
    if (k) {
        const char *hl = strstr(k, " hl ");
        if (hl) {
            char name[64];
            hl += 4;
            size_t n = strcspn(hl, ";");
            snprintf(name, sizeof name, "%.*s", (int)(n < 63 ? n : 63), hl);
            for (char *e = name + strlen(name); e > name && e[-1] == ' '; *--e = '\0') {
            }
            int lang = syntax_by_name(name);
            if (lang >= 0) {
                return lang;
            }
        }
    }
    const char *v = strstr(line, "vim:");
    v = v ? v : strstr(line, "vi:");
    if (v) {
        const char *ft = strstr(v, "ft=");
        ft = ft ? ft + 3 : strstr(v, "filetype=") ? strstr(v, "filetype=") + 9 : NULL;
        if (ft) {
            size_t n = strcspn(ft, " :\t");
            for (int i = 0; i < NLANGS; i++) {
                if (word_in(ft, n, langs[i].vim)) {
                    return i;
                }
            }
        }
    }
    return -1;
}

int syntax_detect(const char *path, const struct buffer *b)
{
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path;
    /* Kate reads modelines in the first and last 10 lines. */
    for (size_t i = 0; i < b->lines; i++) {
        if (i == 10 && b->lines > 20) {
            i = b->lines - 10;
        }
        size_t len = buffer_line_length(b, i);
        char line[512];
        len = len < sizeof line - 1 ? len : sizeof line - 1;
        buffer_copy(b, buffer_line_start(b, i), len, line);
        line[len] = '\0';
        int lang = modeline(line);
        if (lang >= 0) {
            return lang;
        }
    }
    for (int i = 0; base && i < NLANGS; i++) {
        if (name_matches(base, langs[i].exts)) {
            return i;
        }
    }
    /* #!/bin/sh, #!/usr/bin/env python3 */
    char first[256];
    size_t len = buffer_line_length(b, 0);
    len = len < sizeof first - 1 ? len : sizeof first - 1;
    buffer_copy(b, 0, len, first);
    first[len] = '\0';
    if (first[0] == '#' && first[1] == '!') {
        char *p = first + 2, *prog;
        while (*p == ' ') {
            p++;
        }
        size_t n = strcspn(p, " ");
        prog = p;
        for (char *s = p; s < p + n; s++) {
            if (*s == '/') {
                prog = s + 1;
            }
        }
        n = strcspn(prog, " ");
        if (n == 3 && strncmp(prog, "env", 3) == 0) {
            prog += 3;
            while (*prog == ' ' || (*prog == '-' && prog[1] == 'S')) {
                prog += *prog == '-' ? 2 : 1;
            }
            n = strcspn(prog, " ");
        }
        /* python3.12 is python3's */
        size_t base_len = n;
        while (base_len > 0 && (isdigit((unsigned char)prog[base_len - 1]) || prog[base_len - 1] == '.')) {
            base_len--;
        }
        for (int i = 0; i < NLANGS; i++) {
            if (word_in(prog, n, langs[i].programs) || (base_len && word_in(prog, base_len, langs[i].programs))) {
                return i;
            }
        }
    }
    return -1;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *in = fopen(path, "re");
    if (!in) {
        return NULL;
    }
    char *s = NULL;
    size_t size = 0, n = 0;
    for (;;) {
        if (n + 4096 > size) {
            size = size * 2 + 8192;
            char *bigger = realloc(s, size);
            if (!bigger) {
                free(s);
                fclose(in);
                return NULL;
            }
            s = bigger;
        }
        size_t got = fread(s + n, 1, size - n - 1, in);
        n += got;
        if (got == 0) {
            break;
        }
    }
    fclose(in);
    s[n] = '\0';
    *len = n;
    return s;
}

/* The package's query, else edit's own copy. */
static char *read_named_query(const char *folder, const char *name, size_t *len, char *from, size_t size)
{
    snprintf(from, size, "%s/%s/%s", PACKAGED_QUERIES, folder, name);
    char *s = read_file(from, len);
    if (!s) {
        snprintf(from, size, "%s/%s/%s", EDIT_QUERIES, folder, name);
        s = read_file(from, len);
    }
    return s;
}

static char *read_query(const char *folder, size_t *len, char *from, size_t size)
{
    return read_named_query(folder, "highlights.scm", len, from, size);
}

/* A Lua pattern as a PCRE2 one: %a and the other classes, %x escapes, and
 * the lazy repetition "-". */
static void lua_to_pcre(const char *lua, size_t len, char *out, size_t size)
{
    size_t o = 0;
    int in_set = 0;
    for (size_t i = 0; i < len && o + 16 < size; i++) {
        char c = lua[i];
        if (c == '%' && i + 1 < len) {
            char k = lua[++i];
            const char *cls = NULL;
            switch (tolower((unsigned char)k)) {
            case 'a': cls = "[:alpha:]"; break;
            case 'd': cls = "[:digit:]"; break;
            case 's': cls = "[:space:]"; break;
            case 'w': cls = "[:alnum:]"; break;
            case 'l': cls = "[:lower:]"; break;
            case 'u': cls = "[:upper:]"; break;
            case 'p': cls = "[:punct:]"; break;
            case 'x': cls = "[:xdigit:]"; break;
            case 'c': cls = "[:cntrl:]"; break;
            }
            if (cls) {
                /* %A, not a letter, inside a set is taken as %a. */
                int neg = isupper((unsigned char)k) && !in_set;
                o += (size_t)snprintf(out + o, size - o, in_set ? "%s%s" : "[%s%s]", neg ? "^" : "", cls);
            } else {
                out[o++] = '\\';
                out[o++] = k;
            }
        } else if (c == '[' && !in_set) {
            in_set = 1;
            out[o++] = c;
        } else if (c == ']' && in_set) {
            in_set = 0;
            out[o++] = c;
        } else if (c == '-' && !in_set && i > 0) {
            out[o++] = '*';
            out[o++] = '?';
        } else {
            if (!in_set && strchr("\\{}|", c)) {
                out[o++] = '\\';
            }
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

static pcre2_code *compile(const char *pattern)
{
    int code;
    PCRE2_SIZE offset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, PCRE2_UTF | PCRE2_MATCH_INVALID_UTF,
                                   &code, &offset, NULL);
    if (re) {
        pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
    }
    return re;
}

/* The predicates of each pattern, ready to check. */
static void prepare(struct loaded *l)
{
    uint32_t n = ts_query_pattern_count(l->query);
    l->patterns = calloc(n ? n : 1, sizeof *l->patterns);
    for (uint32_t i = 0; l->patterns && i < n; i++) {
        uint32_t count;
        const TSQueryPredicateStep *st = ts_query_predicates_for_pattern(l->query, i, &count);
        for (uint32_t k = 0; k < count;) {
            uint32_t end = k;
            while (end < count && st[end].type != TSQueryPredicateStepTypeDone) {
                end++;
            }
            if (end > k + 1 && st[k].type == TSQueryPredicateStepTypeString &&
                st[k + 1].type == TSQueryPredicateStepTypeCapture) {
                uint32_t nl;
                const char *name = ts_query_string_value_for_id(l->query, st[k].value_id, &nl);
                struct predicate p = { .capture = st[k + 1].value_id, .other = -1 };
                const char *base = name;
                if (strncmp(base, "not-", 4) == 0) {
                    p.negate = 1;
                    base += 4;
                }
                if (strncmp(base, "any-", 4) == 0 && strcmp(base, "any-of?") != 0) {
                    p.any = 1;
                    base += 4;
                }
                if (strncmp(base, "not-", 4) == 0) {
                    p.negate = 1;
                    base += 4;
                }
                int ok = 0;
                if (strcmp(base, "eq?") == 0 && end == k + 3) {
                    p.kind = PRED_EQ;
                    if (st[k + 2].type == TSQueryPredicateStepTypeCapture) {
                        p.other = (int)st[k + 2].value_id;
                    } else {
                        uint32_t vl;
                        p.text = strdup(ts_query_string_value_for_id(l->query, st[k + 2].value_id, &vl));
                    }
                    ok = 1;
                } else if ((strcmp(base, "match?") == 0 || strcmp(base, "lua-match?") == 0 ||
                            strcmp(base, "vim-match?") == 0) && end == k + 3 &&
                           st[k + 2].type == TSQueryPredicateStepTypeString) {
                    uint32_t vl;
                    const char *re = ts_query_string_value_for_id(l->query, st[k + 2].value_id, &vl);
                    p.kind = PRED_MATCH;
                    if (strcmp(base, "lua-match?") == 0) {
                        char conv[1024];
                        lua_to_pcre(re, vl, conv, sizeof conv);
                        p.re = compile(conv);
                    } else {
                        p.re = compile(re);
                    }
                    ok = p.re != NULL;
                } else if (strcmp(base, "any-of?") == 0 && end > k + 2) {
                    p.kind = PRED_ANY_OF;
                    p.words = calloc(end - k - 2, sizeof *p.words);
                    for (uint32_t w = k + 2; p.words && w < end; w++) {
                        uint32_t vl;
                        if (st[w].type == TSQueryPredicateStepTypeString) {
                            p.words[p.nwords++] = strdup(ts_query_string_value_for_id(l->query, st[w].value_id, &vl));
                        }
                    }
                    ok = p.words != NULL;
                }
                struct pattern *pt = &l->patterns[i];
                struct predicate *more = ok ? realloc(pt->p, (size_t)(pt->n + 1) * sizeof *more) : NULL;
                if (more) {
                    pt->p = more;
                    pt->p[pt->n++] = p;
                }
            }
            k = end + 1;
        }
    }
}

/* Common query inheritance header used by tree-sitter query packages.
 * An unavailable optional parent is skipped; a bounded recursion also
 * prevents a malformed package's inheritance cycle from hanging edit. */
static char *read_folding_query(const char *folder, size_t *len, unsigned depth)
{
    if (depth > 8) {
        return NULL;
    }
    char from[512];
    char *source = read_named_query(folder, "folds.scm", len, from, sizeof from);
    if (!source) {
        return NULL;
    }
    const char *header = strstr(source, "; inherits:");
    if (header) {
        header += strlen("; inherits:");
        size_t n = strcspn(header, "\n\r");
        char parents[256];
        snprintf(parents, sizeof parents, "%.*s", (int)(n < 255 ? n : 255), header);
        for (char *save, *p = strtok_r(parents, ", ()\t", &save); p; p = strtok_r(NULL, ", ()\t", &save)) {
            size_t pn = 0;
            char *parent = read_folding_query(p, &pn, depth + 1);
            char *both = parent ? malloc(pn + *len + 2) : NULL;
            if (both) {
                memcpy(both, parent, pn);
                both[pn] = '\n';
                memcpy(both + pn + 1, source, *len + 1);
                free(source);
                source = both;
                *len += pn + 1;
            }
            free(parent);
        }
    }
    return source;
}

static void load_folding_query(struct loaded *l, const char *folder)
{
    size_t len = 0;
    char *source = read_folding_query(folder, &len, 0);
    if (!source) {
        return;
    }
    struct loaded *f = calloc(1, sizeof *f);
    uint32_t offset;
    TSQueryError error;
    if (f) {
        f->query = ts_query_new(l->language, source, (uint32_t)len, &offset, &error);
        if (f->query) {
            prepare(f);
            l->folds = f;
        } else {
            free(f); /* a stale query falls back to the grammar's node rules */
        }
    }
    free(source);
}

static const struct loaded *load(int lang, char *why, size_t size)
{
    struct loaded *l = &loaded[lang];
    if (l->tried) {
        if (!l->query) {
            snprintf(why, size, "%s", l->why);
        }
        return l->query ? l : NULL;
    }
    l->tried = 1;
    const char *grammar = lang == MARKDOWN_INLINE ? "markdown-inline" : langs[lang].grammar;
    char path[512], symbol[64];
    snprintf(path, sizeof path, "%s/libtree-sitter-%s.so", EDIT_GRAMMARS, grammar);
    snprintf(symbol, sizeof symbol, "tree_sitter_%s", grammar);
    for (char *p = symbol; *p; p++) {
        *p = *p == '-' ? '_' : *p;
    }
    l->dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const TSLanguage *(*fn)(void) = l->dl ? (const TSLanguage *(*)(void))dlsym(l->dl, symbol) : NULL;
    if (!fn) {
        snprintf(l->why, sizeof l->why, "no grammar %.500s", path);
        snprintf(why, size, "%s", l->why);
        return NULL;
    }
    l->language = fn();
    char from[512], from_c[512] = "";
    size_t len = 0, len_c = 0;
    const char *folder = lang == MARKDOWN_INLINE ? "markdown_inline" : langs[lang].queries;
    char *source = read_query(folder, &len, from, sizeof from);
    if (source && lang < NLANGS && strcmp(langs[lang].grammar, "cpp") == 0) {
        char *c = read_query("c", &len_c, from_c, sizeof from_c);
        char *both = c ? malloc(len_c + len + 2) : NULL;
        if (both) {
            memcpy(both, c, len_c);
            both[len_c] = '\n';
            memcpy(both + len_c + 1, source, len + 1);
            free(source);
            source = both;
            len += len_c + 1;
        }
        free(c);
    }
    if (!source) {
        snprintf(l->why, sizeof l->why, "no highlights.scm for %s", folder);
        snprintf(why, size, "%s", l->why);
        return NULL;
    }
    uint32_t offset;
    TSQueryError error;
    l->query = ts_query_new(l->language, source, (uint32_t)len, &offset, &error);
    free(source);
    if (!l->query) {
        snprintf(l->why, sizeof l->why, "%.500s: query error %d at byte %u", from, (int)error, offset);
        snprintf(why, size, "%s", l->why);
        return NULL;
    }
    uint32_t captures = ts_query_capture_count(l->query);
    l->style = calloc(captures ? captures : 1, sizeof *l->style);
    for (uint32_t i = 0; l->style && i < captures; i++) {
        uint32_t nl;
        const char *name = ts_query_capture_name_for_id(l->query, i, &nl);
        int best = -1, best_len = 0;
        for (int t = 0; t < NTHEME; t++) {
            /* Every part of the theme's name is a part of the capture's. */
            int parts = 0, all = 1;
            char copy[64];
            snprintf(copy, sizeof copy, "%s", theme[t].name);
            for (char *save, *part = strtok_r(copy, ".", &save); part; part = strtok_r(NULL, ".", &save)) {
                parts++;
                int found = 0;
                for (const char *q = name; q < name + nl;) {
                    size_t pl = strcspn(q, ".");
                    pl = pl > (size_t)(name + nl - q) ? (size_t)(name + nl - q) : pl;
                    if (pl == strlen(part) && strncmp(q, part, pl) == 0) {
                        found = 1;
                    }
                    q += pl + 1;
                }
                if (!found) {
                    all = 0;
                    break;
                }
            }
            if (all && parts > best_len) {
                best = t;
                best_len = parts;
            }
        }
        l->style[i] = best;
    }
    prepare(l);
    if (lang != MARKDOWN_INLINE) {
        load_folding_query(l, folder);
    }
    return l;
}

struct syntax *syntax_new(int lang, char *why, size_t size)
{
    why[0] = '\0';
    if (lang < 0 || lang >= NLANGS || !load(lang, why, size)) {
        return NULL;
    }
    struct syntax *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(why, size, "no memory");
        return NULL;
    }
    s->lang = lang;
    s->epoch = 1;
    s->parser = ts_parser_new();
    s->scratch = malloc(4096);
    if (!s->parser || !s->scratch || !ts_parser_set_language(s->parser, loaded[lang].language)) {
        snprintf(why, size, "the %s grammar does not fit this tree-sitter", langs[lang].name);
        syntax_free(s);
        return NULL;
    }
    if (strcmp(langs[lang].grammar, "markdown") == 0) {
        char ignore[128];
        const struct loaded *in = load(MARKDOWN_INLINE, ignore, sizeof ignore);
        if (in) {
            s->inline_parser = ts_parser_new();
            if (s->inline_parser) {
                ts_parser_set_language(s->inline_parser, in->language);
            }
        }
    }
    s->dirty = 1;
    return s;
}

void syntax_free(struct syntax *s)
{
    if (!s) {
        return;
    }
    if (s->tree) {
        ts_tree_delete(s->tree);
    }
    if (s->parser) {
        ts_parser_delete(s->parser);
    }
    if (s->inline_parser) {
        ts_parser_delete(s->inline_parser);
    }
    free(s->scratch);
    free(s->folds);
    free(s);
}

const char *syntax_off(const struct syntax *s)
{
    return s && s->off ? s->off_why : NULL;
}

void syntax_edit(struct syntax *s, size_t start, size_t old_end, size_t new_end, size_t start_line, size_t start_col,
                 size_t old_end_line, size_t old_end_col, size_t new_end_line, size_t new_end_col)
{
    if (!s) {
        return;
    }
    invalidate_folds(s);
    s->dirty = 1;
    if (s->partial) {
        ts_parser_reset(s->parser); /* the text changed under the parse: from the start again */
        s->partial = 0;
    }
    if (!s->tree || new_end > SYNTAX_LIMIT || old_end > SYNTAX_LIMIT) {
        return;
    }
    TSInputEdit e = {
        .start_byte = (uint32_t)start, .old_end_byte = (uint32_t)old_end, .new_end_byte = (uint32_t)new_end,
        .start_point = { (uint32_t)start_line, (uint32_t)start_col },
        .old_end_point = { (uint32_t)old_end_line, (uint32_t)old_end_col },
        .new_end_point = { (uint32_t)new_end_line, (uint32_t)new_end_col },
    };
    ts_tree_edit(s->tree, &e);
}

void syntax_reset(struct syntax *s)
{
    if (!s) {
        return;
    }
    invalidate_folds(s);
    if (s->tree) {
        ts_tree_delete(s->tree);
        s->tree = NULL;
    }
    if (s->partial) {
        ts_parser_reset(s->parser);
        s->partial = 0;
    }
    s->dirty = 1;
}

int syntax_pending(const struct syntax *s)
{
    return s && !s->off && s->dirty;
}

struct reading {
    struct syntax *s;
    struct buffer *b;
};

/* The text for tree-sitter: up to the gap, or a copy across it where a
 * character would be cut there. */
static const char *read_text(void *payload, uint32_t at, TSPoint point, uint32_t *bytes)
{
    (void)point;
    struct reading *r = payload;
    size_t len;
    const char *p = buffer_chunk(r->b, at, &len);
    size_t total = buffer_length(r->b);
    if (len < 4096 && at + len < total) {
        size_t n = total - at < 4096 ? total - at : 4096;
        buffer_copy(r->b, at, n, r->s->scratch);
        p = r->s->scratch;
        len = n;
    }
    *bytes = (uint32_t)len;
    return p;
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static bool parse_progress(TSParseState *state)
{
    return now_ms() > *(double *)state->payload; /* true: stop */
}

int syntax_parse(struct syntax *s, struct buffer *b, int budget_ms)
{
    if (!s || s->off) {
        return -1;
    }
    if (!s->dirty) {
        return 1;
    }
    if (buffer_length(b) > SYNTAX_LIMIT) {
        s->off = 1;
        snprintf(s->off_why, sizeof s->off_why, "Highlighting is off: the file is larger than %u MiB.",
                 SYNTAX_LIMIT >> 20);
        syntax_reset(s);
        s->dirty = 0;
        return -1;
    }
    struct reading r = { s, b };
    TSInput input = { .payload = &r, .read = read_text, .encoding = TSInputEncodingUTF8 };
    /* With a budget, the parse stops when it is spent, and the next call
     * goes on where it stopped (tree-sitter resumes a cancelled parse). */
    double deadline = now_ms() + (budget_ms > 0 ? budget_ms : 1e12);
    TSParseOptions options = { .payload = &deadline, .progress_callback = parse_progress };
    TSTree *tree = ts_parser_parse_with_options(s->parser, s->tree, input, options);
    if (!tree) {
        s->partial = 1;
        return 0;
    }
    s->partial = 0;
    if (s->tree) {
        ts_tree_delete(s->tree);
    }
    s->tree = tree;
    invalidate_folds(s);
    s->dirty = 0;
    return 1;
}

/* The text of node (at most size - 1 bytes of it). */
static const char *node_text(struct buffer *b, TSNode node, char *out, size_t size, size_t *len)
{
    size_t start = ts_node_start_byte(node), end = ts_node_end_byte(node), total = buffer_length(b);
    end = end > total ? total : end;
    size_t n = end > start ? end - start : 0;
    n = n < size - 1 ? n : size - 1;
    buffer_copy(b, start, n, out);
    out[n] = '\0';
    *len = n;
    return out;
}

static int check_one(const struct loaded *l, struct buffer *b, const struct predicate *p, const TSQueryMatch *m,
                     TSNode node)
{
    char text[1024];
    size_t len;
    node_text(b, node, text, sizeof text, &len);
    int ok = 0;
    if (p->kind == PRED_EQ) {
        if (p->other >= 0) {
            for (uint16_t c = 0; c < m->capture_count; c++) {
                if (m->captures[c].index == (uint32_t)p->other) {
                    char other[1024];
                    size_t ol;
                    node_text(b, m->captures[c].node, other, sizeof other, &ol);
                    ok = ol == len && memcmp(other, text, len) == 0;
                    break;
                }
            }
        } else {
            ok = strcmp(text, p->text) == 0;
        }
    } else if (p->kind == PRED_MATCH) {
        pcre2_match_data *md = pcre2_match_data_create_from_pattern(p->re, NULL);
        ok = md && pcre2_match(p->re, (PCRE2_SPTR)text, len, 0, 0, md, NULL) >= 0;
        pcre2_match_data_free(md);
    } else {
        for (int w = 0; w < p->nwords && !ok; w++) {
            ok = strcmp(text, p->words[w]) == 0;
        }
    }
    (void)l;
    return p->negate ? !ok : ok;
}

/* The pattern of match m holds for its text. */
static int holds(const struct loaded *l, struct buffer *b, const TSQueryMatch *m)
{
    if (!l->patterns) {
        return 0; /* a failed predicate allocation must not broaden matches */
    }
    const struct pattern *pt = &l->patterns[m->pattern_index];
    for (int i = 0; i < pt->n; i++) {
        const struct predicate *p = &pt->p[i];
        int seen = 0, any = 0, all = 1;
        for (uint16_t c = 0; c < m->capture_count; c++) {
            if (m->captures[c].index != p->capture) {
                continue;
            }
            seen = 1;
            int ok = check_one(l, b, p, m, m->captures[c].node);
            any |= ok;
            all &= ok;
        }
        if (seen && !(p->any ? any : all)) {
            return 0;
        }
    }
    return 1;
}

/* Deliberately explicit: these are node kinds of the grammars in langs[],
 * not a promise that uninstalled KSyntaxHighlighting languages have parsers.
 * Python folds its statements (whose starts include the header), not `block`
 * nodes starting on the first body line. Markdown sections are grammar nodes
 * including their subordinate headings, not a textual heading/brace scan. */
static const char *fold_types[] = {
    /* Bash */ "function_definition if_statement for_statement while_statement case_statement case_item "
               "compound_statement subshell do_group heredoc_body array comment",
    /* C */ "compound_statement field_declaration_list enumerator_list initializer_list "
            "preproc_if preproc_ifdef comment argument_list parameter_list",
    /* C++ */ "compound_statement field_declaration_list enumerator_list initializer_list declaration_list "
              "preproc_if preproc_ifdef comment argument_list parameter_list template_parameter_list "
              "raw_string_literal requires_expression",
    /* JavaScript */ "statement_block class_body switch_body object object_pattern array array_pattern "
                     "arguments formal_parameters template_string comment",
    /* Lua */ "function_declaration function_definition if_statement elseif_statement else_statement "
              "for_statement while_statement repeat_statement do_statement table_constructor comment string",
    /* Markdown */ "section list_item fenced_code_block indented_code_block block_quote html_block",
    /* Python */ "function_definition class_definition if_statement elif_clause else_clause for_statement "
                 "while_statement try_statement except_clause finally_clause with_statement match_statement "
                 "case_clause list list_comprehension dictionary dictionary_comprehension set set_comprehension "
                 "tuple argument_list parameters parenthesized_expression string",
    /* Tree-sitter Query */ "named_node predicate grouping list",
    /* Rust */ "block declaration_list field_declaration_list enum_variant_list match_block "
               "use_list token_tree struct_expression array_expression tuple_expression arguments "
               "parameters type_arguments type_parameters block_comment raw_string_literal",
    /* Vim */ "function_definition if_statement for_statement while_statement try_statement "
              "augroup_statement lua_statement python_statement ruby_statement",
    /* Vim Help */ "section block codeblock column_heading",
};
_Static_assert(sizeof fold_types / sizeof fold_types[0] == NLANGS, "fold rules must match the grammar map");

struct folding {
    struct syntax_fold *v;
    size_t n, cap;
    int failed;
};

static void add_fold(struct folding *f, struct buffer *b, size_t start, size_t end)
{
    if (f->failed || start >= end || end > buffer_length(b)) {
        return;
    }
    size_t first = buffer_line_of(b, start), last = buffer_line_of(b, end - 1);
    if (first >= last) {
        return;
    }
    if (f->n == f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 64;
        struct syntax_fold *v = realloc(f->v, cap * sizeof *v);
        if (!v) {
            f->failed = 1;
            return;
        }
        f->v = v;
        f->cap = cap;
    }
    f->v[f->n++] = (struct syntax_fold) { start, end, first, last, SIZE_MAX, 0 };
}

/* Iterative cursor traversal avoids C stack exhaustion on deeply nested or
 * adversarial input. Error/missing nodes themselves are never fold regions. */
static void node_folds(struct syntax *s, struct buffer *b, struct folding *f)
{
    TSTreeCursor walk = ts_tree_cursor_new(ts_tree_root_node(s->tree));
    for (;;) {
        TSNode node = ts_tree_cursor_current_node(&walk);
        const char *type = ts_node_type(node);
        if (ts_node_is_named(node) && !ts_node_is_error(node) && !ts_node_is_missing(node) &&
            word_in(type, strlen(type), fold_types[s->lang])) {
            add_fold(f, b, ts_node_start_byte(node), ts_node_end_byte(node));
        }
        if (f->failed) {
            break;
        }
        if (ts_tree_cursor_goto_first_child(&walk)) {
            continue;
        }
        while (!ts_tree_cursor_goto_next_sibling(&walk)) {
            if (!ts_tree_cursor_goto_parent(&walk)) {
                ts_tree_cursor_delete(&walk);
                return;
            }
        }
    }
    ts_tree_cursor_delete(&walk);
}

/* Standard @fold captures, with predicates checked just as highlights are.
 * Unrecognized/private captures are NOT regions. */
static void query_folds(struct syntax *s, struct buffer *b, struct folding *f)
{
    const struct loaded *q = loaded[s->lang].folds;
    TSQueryCursor *cur = ts_query_cursor_new();
    if (!cur) {
        f->failed = 1;
        return;
    }
    ts_query_cursor_exec(cur, q->query, ts_tree_root_node(s->tree));
    TSQueryMatch m;
    while (ts_query_cursor_next_match(cur, &m)) {
        if (!holds(q, b, &m)) {
            continue;
        }
        for (uint16_t c = 0; c < m.capture_count; c++) {
            uint32_t len;
            const char *name = ts_query_capture_name_for_id(q->query, m.captures[c].index, &len);
            if ((len == 4 && strncmp(name, "fold", 4) == 0) ||
                (len > 5 && strncmp(name, "fold.", 5) == 0)) {
                TSNode n = m.captures[c].node;
                if (!ts_node_is_error(n) && !ts_node_is_missing(n)) {
                    add_fold(f, b, ts_node_start_byte(n), ts_node_end_byte(n));
                }
            }
        }
        if (f->failed) {
            break;
        }
    }
    ts_query_cursor_delete(cur);
}

static int fold_order(const void *a, const void *b)
{
    const struct syntax_fold *x = a, *y = b;
    if (x->start_line != y->start_line) {
        return x->start_line < y->start_line ? -1 : 1;
    }
    /* The UI has one toggle per line: prefer its largest containing range. */
    if (x->end_line != y->end_line) {
        return x->end_line > y->end_line ? -1 : 1;
    }
    if (x->start_byte != y->start_byte) {
        return x->start_byte < y->start_byte ? -1 : 1;
    }
    return x->end_byte > y->end_byte ? -1 : x->end_byte < y->end_byte;
}

size_t syntax_folds(struct syntax *s, struct buffer *b, const struct syntax_fold **out)
{
    if (out) {
        *out = NULL;
    }
    if (!s || s->dirty || s->off || !s->tree || !b) {
        return 0;
    }
    if (!s->folds_ready) {
        struct folding f = { 0 };
        if (loaded[s->lang].folds) {
            query_folds(s, b, &f);
        } else {
            node_folds(s, b, &f);
        }
        if (f.failed) {
            free(f.v);
            return 0; /* retry on the next request, never publish half a tree */
        }
        if (f.n > 1) {
            qsort(f.v, f.n, sizeof *f.v, fold_order);
        }
        size_t n = 0, parent = SIZE_MAX;
        for (size_t i = 0; i < f.n; i++) {
            struct syntax_fold v = f.v[i];
            if (n && v.start_line == f.v[n - 1].start_line) {
                continue; /* same toggle line: keep the outermost range */
            }
            while (parent != SIZE_MAX && (v.start_line > f.v[parent].end_line ||
                   v.start_byte >= f.v[parent].end_byte)) {
                parent = f.v[parent].parent;
            }
            if (parent != SIZE_MAX && (v.end_line > f.v[parent].end_line ||
                v.start_byte < f.v[parent].start_byte || v.end_byte > f.v[parent].end_byte)) {
                continue; /* query regions may overlap; never export crossing folds */
            }
            v.parent = parent;
            v.depth = parent == SIZE_MAX ? 0 : f.v[parent].depth + 1;
            f.v[n] = v;
            parent = n++;
        }
        s->folds = f.v;
        s->nfolds = n;
        s->folds_ready = 1;
    }
    if (out && s->nfolds) {
        *out = s->folds;
    }
    return s->nfolds;
}

size_t syntax_fold_at(const struct syntax_fold *folds, size_t count, size_t line)
{
    size_t current = SIZE_MAX;
    for (size_t i = 0; folds && i < count && folds[i].start_line <= line; i++) {
        if (line <= folds[i].end_line) {
            current = i;
        }
    }
    return current;
}

struct span {
    uint32_t start, end;
    int style;
    const char *name;
    const void *id;
    size_t seq;
};

/* By start, the outer first, and else in the cursor's order (a node before
 * a child of the same bytes): the order to paint them in. */
static int by_place(const void *a, const void *b)
{
    const struct span *x = a, *y = b;
    if (x->start != y->start) {
        return x->start < y->start ? -1 : 1;
    }
    if (x->end != y->end) {
        return x->end > y->end ? -1 : 1;
    }
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* The spans of query l over the tree's root in [start, end), node by node
 * the last pattern's, in the order to paint them. */
static struct span *spans_of(const struct loaded *l, TSNode root, struct buffer *b, size_t start, size_t end,
                             size_t *count)
{
    TSQueryCursor *cur = ts_query_cursor_new();
    size_t n = 0, size = 256;
    struct span *spans = malloc(size * sizeof *spans);
    if (!cur || !spans) {
        if (cur) {
            ts_query_cursor_delete(cur);
        }
        free(spans);
        *count = 0;
        return NULL;
    }
    ts_query_cursor_set_byte_range(cur, (uint32_t)start, (uint32_t)end);
    ts_query_cursor_exec(cur, l->query, root);
    TSQueryMatch m;
    uint32_t ci;
    while (ts_query_cursor_next_capture(cur, &m, &ci)) {
        if (!holds(l, b, &m)) {
            continue;
        }
        TSQueryCapture c = m.captures[ci];
        uint32_t nl;
        const char *name = ts_query_capture_name_for_id(l->query, c.index, &nl);
        if (name[0] == '_') {
            continue; /* private captures, for predicates */
        }
        struct span sp = { ts_node_start_byte(c.node), ts_node_end_byte(c.node), l->style[c.index], name,
                           c.node.id, n };
        /* The same node again: a later pattern, which takes it. */
        if (n > 0 && spans[n - 1].id == sp.id && spans[n - 1].start == sp.start && spans[n - 1].end == sp.end) {
            sp.seq = spans[n - 1].seq;
            spans[n - 1] = sp;
            continue;
        }
        if (n == size) {
            size *= 2;
            struct span *more = realloc(spans, size * sizeof *more);
            if (!more) {
                break;
            }
            spans = more;
        }
        spans[n++] = sp;
    }
    ts_query_cursor_delete(cur);
    qsort(spans, n, sizeof *spans, by_place);
    *count = n;
    return spans;
}

static void paint(const struct span *spans, size_t n, size_t start, size_t end, uint32_t *colour,
                  uint8_t *styles, const char **names)
{
    for (size_t i = 0; i < n; i++) {
        if (spans[i].style < 0) {
            continue; /* a name the theme does not know: the outer colour stays */
        }
        size_t a = spans[i].start > start ? spans[i].start : start;
        size_t z = spans[i].end < end ? spans[i].end : end;
        for (size_t k = a; k < z; k++) {
            if (colour) {
                colour[k - start] = theme[spans[i].style].colour;
            }
            if (styles) {
                /* Markdown emphasis composes with a nested strong/link span;
                 * ordinary syntax captures replace, as their colour does. */
                const char *name = spans[i].name;
                uint8_t flags = theme[spans[i].style].flags;
                if (strncmp(name, "text.", 5) == 0 || strncmp(name, "markup.", 7) == 0) {
                    styles[k - start] |= flags;
                } else {
                    styles[k - start] = flags;
                }
            }
            if (names) {
                names[k - start] = spans[i].name;
            }
        }
    }
}

/* Markdown's inline nodes under the cursor's node in [start, end), each
 * parsed with the inline grammar and painted with its query. */
static void paint_inline(struct syntax *s, struct buffer *b, TSTreeCursor *walk, size_t start, size_t end,
                         uint32_t *colour, uint8_t *styles, const char **names)
{
    const struct loaded *in = &loaded[MARKDOWN_INLINE];
    if (ts_tree_cursor_goto_first_child_for_byte(walk, (uint32_t)start) < 0) {
        return;
    }
    do {
        TSNode node = ts_tree_cursor_current_node(walk);
        size_t ns = ts_node_start_byte(node), ne = ts_node_end_byte(node);
        if (ns >= end) {
            break;
        }
        if (ne <= start) {
            continue;
        }
        if (strcmp(ts_node_type(node), "inline") != 0) {
            paint_inline(s, b, walk, start, end, colour, styles, names);
            continue;
        }
        TSRange range = { ts_node_start_point(node), ts_node_end_point(node), (uint32_t)ns, (uint32_t)ne };
        ts_parser_set_included_ranges(s->inline_parser, &range, 1);
        struct reading r = { s, b };
        TSInput input = { .payload = &r, .read = read_text, .encoding = TSInputEncodingUTF8 };
        TSTree *t = ts_parser_parse(s->inline_parser, NULL, input);
        if (t) {
            size_t count;
            struct span *spans = spans_of(in, ts_tree_root_node(t), b, ns > start ? ns : start,
                                          ne < end ? ne : end, &count);
            paint(spans, count, start, end, colour, styles, names);
            free(spans);
            ts_tree_delete(t);
        }
    } while (ts_tree_cursor_goto_next_sibling(walk));
    ts_tree_cursor_goto_parent(walk);
}

static void colours(struct syntax *s, struct buffer *b, size_t start, size_t end, uint32_t *colour,
                    uint8_t *styles, const char **names)
{
    for (size_t i = start; i < end; i++) {
        if (colour) {
            colour[i - start] = COLOUR_NORMAL;
        }
        if (styles) {
            styles[i - start] = 0;
        }
        if (names) {
            names[i - start] = "";
        }
    }
    if (!s || s->off || !s->tree) {
        return;
    }
    size_t count;
    struct span *spans = spans_of(&loaded[s->lang], ts_tree_root_node(s->tree), b, start, end, &count);
    paint(spans, count, start, end, colour, styles, names);
    free(spans);
    if (s->inline_parser && loaded[MARKDOWN_INLINE].query) {
        TSTreeCursor walk = ts_tree_cursor_new(ts_tree_root_node(s->tree));
        paint_inline(s, b, &walk, start, end, colour, styles, names);
        ts_tree_cursor_delete(&walk);
    }
}

void syntax_colours(struct syntax *s, struct buffer *b, size_t start, size_t end, uint32_t *colour)
{
    colours(s, b, start, end, colour, NULL, NULL);
}

void syntax_styles(struct syntax *s, struct buffer *b, size_t start, size_t end, uint8_t *styles)
{
    if (styles) {
        colours(s, b, start, end, NULL, styles, NULL);
    }
}

const char *syntax_capture_at(struct syntax *s, struct buffer *b, size_t at)
{
    uint32_t c;
    const char *name = "";
    colours(s, b, at, at + 1, &c, NULL, &name);
    return name;
}
