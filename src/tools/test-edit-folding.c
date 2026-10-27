/* Real packaged parsers, shared with edit's highlighter: fold hierarchy,
 * incremental invalidation, budget/off cases, and Breeze font metadata.
 * There is intentionally no mock grammar or brace scanner in this test. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../progs/edit-syntax.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

int screen_char_cells(uint32_t cp)
{
    (void)cp;
    return 1;
}

static struct syntax *document(struct buffer *b, const char *language, const char *text)
{
    char why[640];
    CHECK(buffer_init(b) == 0 && buffer_set(b, text, strlen(text)) == 0);
    int lang = syntax_by_name(language);
    CHECK(lang >= 0);
    struct syntax *s = syntax_new(lang, why, sizeof why);
    if (!s) {
        fprintf(stderr, "FAIL real %s parser: %s\n", language, why);
        exit(1);
    }
    const struct syntax_fold *f = (void *)1;
    CHECK(syntax_folds(s, b, &f) == 0 && f == NULL);
    CHECK(syntax_parse(s, b, 0) == 1);
    return s;
}

static size_t folds(struct syntax *s, struct buffer *b, const struct syntax_fold **out)
{
    size_t n = syntax_folds(s, b, out);
    for (size_t i = 0; i < n; i++) {
        const struct syntax_fold *f = &(*out)[i];
        CHECK(f->start_byte < f->end_byte && f->end_byte <= buffer_length(b));
        CHECK(f->start_line == buffer_line_of(b, f->start_byte));
        CHECK(f->end_line == buffer_line_of(b, f->end_byte - 1));
        CHECK(f->start_line < f->end_line && f->end_line < b->lines);
        CHECK(i == 0 || (*out)[i - 1].start_line < f->start_line);
        if (f->parent == SIZE_MAX) {
            CHECK(f->depth == 0);
        } else {
            CHECK(f->parent < i);
            const struct syntax_fold *p = &(*out)[f->parent];
            CHECK(f->depth == p->depth + 1);
            CHECK(p->start_byte <= f->start_byte && f->end_byte <= p->end_byte);
            CHECK(p->start_line < f->start_line && f->end_line <= p->end_line);
        }
        /* A UI's only hidden lines are start+1..end, not start. */
        size_t first_hidden = f->start_line + 1;
        CHECK(first_hidden > f->start_line && first_hidden <= f->end_line);
    }
    const struct syntax_fold *again = NULL;
    CHECK(syntax_folds(s, b, &again) == n && again == *out);
    return n;
}

static size_t range(const struct syntax_fold *f, size_t n, size_t start, size_t end)
{
    for (size_t i = 0; i < n; i++) {
        if (f[i].start_line == start && f[i].end_line == end) {
            return i;
        }
    }
    fprintf(stderr, "Missing fold [%zu,%zu]; actual:", start, end);
    for (size_t i = 0; i < n; i++) {
        fprintf(stderr, " [%zu,%zu](p=%zu,d=%u)", f[i].start_line, f[i].end_line, f[i].parent, f[i].depth);
    }
    fputc('\n', stderr);
    exit(1);
}

static void nested(const char *lang, const char *text, size_t a, size_t z, size_t inner_a, size_t inner_z)
{
    struct buffer b;
    struct syntax *s = document(&b, lang, text);
    const struct syntax_fold *f;
    size_t n = folds(s, &b, &f), outer = range(f, n, a, z), inner = range(f, n, inner_a, inner_z);
    CHECK(f[outer].parent == SIZE_MAX && f[inner].parent == outer);
    CHECK(syntax_fold_at(f, n, inner_a) == inner);
    size_t current = syntax_fold_at(f, n, inner_z);
    CHECK(current != SIZE_MAX);
    while (current != inner && current != SIZE_MAX) {
        current = f[current].parent;
    }
    CHECK(current == inner); /* a deeper region may end on the same line */
    CHECK(syntax_fold_at(f, n, a) == outer && syntax_fold_at(f, n, z + 1) == SIZE_MAX);
    CHECK(syntax_folds(s, &b, NULL) == n);
    printf("PASS %s: parser-backed nested regions, current/top-level/recursive hierarchy\n", lang);
    syntax_free(s);
    buffer_free(&b);
}

static void primary_languages(void)
{
    nested("C", "void f(void) {\n  if (1) {\n    x();\n  }\n}\n", 0, 4, 1, 3);
    nested("C++", "namespace n {\n  class X {\n    void f() {\n      if (true) {\n        x();\n      }\n    }\n  };\n}\n", 0, 8, 1, 7);
    nested("Python", "def f():\n    if True:\n        for x in xs:\n            print(x)\n    return 0\n", 0, 4, 1, 3);
    nested("Markdown", "# Main\n\ntext\n\n## Child\n\n```c\nif (x) {\n}\n```\n", 0, 9, 4, 9);
}

static void packaged_languages(void)
{
    nested("Bash", "f() {\n  if true; then\n    echo yes\n  fi\n}\n", 0, 4, 1, 3);
    nested("JavaScript", "function f() {\n  if (true) {\n    x();\n  }\n}\n", 0, 4, 1, 3);
    nested("Lua", "function f()\n  if true then\n    x()\n  end\nend\n", 0, 4, 1, 3);
    nested("Rust", "fn f() {\n  if true {\n    x();\n  }\n}\n", 0, 4, 1, 3);
    nested("Tree-sitter Query", "(function_definition\n  body: (compound_statement\n    (return_statement)\n  )\n) @fold\n", 0, 4, 1, 3);
    nested("Vim", "function! F()\n  if 1\n    echo 'yes'\n  endif\nendfunction\n", 0, 4, 1, 3);
    /* The packaged folds.scm deliberately has no while_statement: unlike
     * the fallback rule it must produce no fold for this valid Vim node. */
    struct buffer b;
    struct syntax *s = document(&b, "Vim", "while 1\n  break\nendwhile\n");
    const struct syntax_fold *f;
    CHECK(folds(s, &b, &f) == 0 && f == NULL);
    syntax_free(s);
    buffer_free(&b);
    s = document(&b, "Vim Help", "INTRODUCTION\n\nThis is the introduction.\n\nEXAMPLE~\n>\n    :echo 'hello'\n    :echo 'world'\n<\n");
    size_t n = folds(s, &b, &f);
    printf("PASS Vim Help: real parser, %zu valid multi-line regions\n", n);
    syntax_free(s);
    buffer_free(&b);
    CHECK(syntax_count() == 11 && syntax_by_name("COBOL") == -1);
    CHECK(syntax_new(-1, (char[64]){0}, 64) == NULL);
    puts("PASS language availability is the actual 11-grammar map, not 400+ definitions");
}

static void incremental(void)
{
    struct buffer b;
    struct syntax *s = document(&b, "C", "void f() {\n  if (1) {\n    x();\n  }\n}\n");
    const struct syntax_fold *f;
    CHECK(folds(s, &b, &f) == 2);
    uint64_t epoch = syntax_epoch(s);
    CHECK(buffer_insert(&b, 0, "// prefix\n", 10) == 0);
    syntax_edit(s, 0, 0, 10, 0, 0, 0, 0, 1, 0);
    CHECK(syntax_epoch(s) != epoch && syntax_pending(s));
    CHECK(syntax_folds(s, &b, &f) == 0 && f == NULL);
    epoch = syntax_epoch(s);
    CHECK(syntax_parse(s, &b, 0) == 1 && syntax_epoch(s) != epoch);
    size_t n = folds(s, &b, &f);
    range(f, n, 1, 5);
    range(f, n, 2, 4);
    CHECK(f[0].start_byte == 19);
    /* Delete the whole inner block; the old nested range must not escape
     * between the edit notification and completed incremental parse. */
    size_t start = buffer_line_start(&b, 2), end = buffer_line_start(&b, 5);
    buffer_delete(&b, start, end - start);
    syntax_edit(s, start, end, start, 2, 0, 5, 0, 2, 0);
    CHECK(syntax_folds(s, &b, &f) == 0 && f == NULL);
    CHECK(syntax_parse(s, &b, 0) == 1);
    n = folds(s, &b, &f);
    CHECK(n == 1);
    range(f, n, 1, 2);
    /* A malformed document is still parsed: no invalid/crossing exported
     * ranges, and reset immediately destroys even a valid old fold index. */
    epoch = syntax_epoch(s);
    CHECK(buffer_set(&b, "void f( {\n  if ( {\n", 19) == 0);
    syntax_reset(s);
    CHECK(syntax_epoch(s) != epoch && syntax_folds(s, &b, &f) == 0 && f == NULL);
    CHECK(syntax_parse(s, &b, 0) == 1);
    folds(s, &b, &f);
    CHECK(buffer_set(&b, "int x;\n", 7) == 0);
    syntax_reset(s);
    CHECK(syntax_parse(s, &b, 0) == 1 && folds(s, &b, &f) == 0);
    syntax_free(s);
    buffer_free(&b);
    puts("PASS incremental edits/reset invalidate epochs and ranges immediately, reparse updates endpoints");
}

static void syntax_not_braces(void)
{
    struct buffer b;
    const struct syntax_fold *f;
    struct syntax *s = document(&b, "C", "char *s = \"{\";\nint x;\nchar *t = \"}\";\n");
    CHECK(folds(s, &b, &f) == 0);
    const char *siblings = "void a() {\n  x();\n} void b() {\n  y();\n}\n";
    CHECK(buffer_set(&b, siblings, strlen(siblings)) == 0);
    syntax_reset(s);
    CHECK(syntax_parse(s, &b, 0) == 1 && folds(s, &b, &f) == 2);
    size_t a = range(f, 2, 0, 2), z = range(f, 2, 2, 4);
    CHECK(f[a].parent == SIZE_MAX && f[z].parent == SIZE_MAX);
    CHECK(buffer_set(&b, "/* {\n comment }\n */\n", 20) == 0);
    syntax_reset(s);
    CHECK(syntax_parse(s, &b, 0) == 1);
    CHECK(folds(s, &b, &f) == 1);
    range(f, 1, 0, 2);
    syntax_free(s);
    buffer_free(&b);
    s = document(&b, "Python", "x = 1\n    not_a_block = 2\n");
    CHECK(folds(s, &b, &f) == 0); /* not invented indentation folds for arbitrary text */
    syntax_free(s);
    buffer_free(&b);
    f = (void *)1;
    CHECK(syntax_folds(NULL, NULL, &f) == 0 && f == NULL && syntax_epoch(NULL) == 0);
    CHECK(syntax_fold_at(NULL, 0, 0) == SIZE_MAX);
    puts("PASS braces in strings are not folds, multi-line comment is one syntax region; no fake Normal folds");
}

static void style(const char *lang, const char *text, const char *word, uint8_t want)
{
    struct buffer b;
    struct syntax *s = document(&b, lang, text);
    size_t len = buffer_length(&b), at = (size_t)(strstr(text, word) - text);
    uint8_t *all = malloc(len + 2);
    uint32_t *before = malloc(len * sizeof *before), *after = malloc(len * sizeof *after);
    CHECK(all && before && after);
    memset(all, 0xAD, len + 2);
    syntax_colours(s, &b, 0, len, before);
    syntax_styles(s, &b, 0, len, all + 1);
    syntax_colours(s, &b, 0, len, after);
    CHECK(memcmp(before, after, len * sizeof *before) == 0 && all[0] == 0xAD && all[len + 1] == 0xAD);
    uint8_t one = 0xFF;
    syntax_styles(s, &b, at, at + 1, &one);
    if (all[at + 1] != want || one != want) {
        fprintf(stderr, "FAIL %s '%s' @%s flags=%u (one=%u), expected %u\n", lang, word,
                syntax_capture_at(s, &b, at), all[at + 1], one, want);
        exit(1);
    }
    syntax_styles(s, &b, at, at, &one);
    CHECK(one == want);
    free(all);
    free(before);
    free(after);
    syntax_free(s);
    buffer_free(&b);
}

static void styles(void)
{
    style("C", "static int FOO = 1; /* comment */\n", "static", SYNTAX_STYLE_BOLD);
    style("C", "static int FOO = 1; /* comment */\n", "int", 0); /* DataType */
    style("C", "static int FOO = 1; /* comment */\n", "FOO", SYNTAX_STYLE_BOLD); /* Constant */
    style("C", "/* comment */\n", "comment", 0); /* Breeze Comment is not italic */
    style("C++", "/** documentation */\n", "documentation", 0); /* no invented doc italics */
    style("Rust", "/// documentation\nfn f() {}\n", "documentation", 0); /* @comment.documentation */
    style("Python", "def f():\n    \"\"\"docstring\n    continuation\"\"\"\n    return 0\n", "docstring", 0);
    style("Python", "def f():\n    return 0\n", "def", SYNTAX_STYLE_BOLD);
    style("Markdown", "# Heading\n", "Heading", SYNTAX_STYLE_BOLD);
    style("Markdown", "*emphasis*\n", "emphasis", SYNTAX_STYLE_ITALIC);
    style("Markdown", "**strong**\n", "strong", SYNTAX_STYLE_BOLD);
    style("Markdown", "***both***\n", "both", SYNTAX_STYLE_BOLD | SYNTAX_STYLE_ITALIC);
    style("Markdown", "[link](https://example.org)\n", "link", SYNTAX_STYLE_UNDERLINE);
    uint8_t normal[4] = { 9, 9, 9, 9 };
    syntax_styles(NULL, NULL, 0, 4, normal);
    CHECK(normal[0] == 0 && normal[3] == 0);
    puts("PASS real highlight captures: Breeze keyword/constant bold, comment/docstring/type normal; Markdown bold/italic/underline compose");
}

static void budget_and_off(void)
{
    struct buffer b;
    CHECK(buffer_init(&b) == 0);
    const char *unit = "void f() {\n  if (1) {\n    x();\n  }\n}\n";
    size_t u = strlen(unit), count = (1u << 20) / u, len = count * u;
    char *text = malloc(len + 1);
    CHECK(text);
    for (size_t i = 0; i < count; i++) {
        memcpy(text + i * u, unit, u);
    }
    CHECK(buffer_take(&b, text, len) == 0);
    char why[640];
    struct syntax *s = syntax_new(syntax_by_name("C"), why, sizeof why);
    CHECK(s);
    const struct syntax_fold *f;
    uint64_t epoch = syntax_epoch(s);
    int r = syntax_parse(s, &b, 1);
    CHECK(r == 0 && syntax_pending(s)); /* large actual C parse is sliced */
    CHECK(syntax_epoch(s) == epoch && syntax_folds(s, &b, &f) == 0 && f == NULL);
    int slices = 1;
    while (r == 0 && slices < 10000) {
        r = syntax_parse(s, &b, 5);
        slices++;
    }
    CHECK(r == 1 && !syntax_pending(s) && syntax_epoch(s) != epoch);
    CHECK(folds(s, &b, &f) == count * 2);
    printf("PASS actual 1 MiB C parse uses %d budget slices; no stale/incomplete fold ranges\n", slices);
    /* Cancelling a partial parse with a second edit resets its parser state. */
    syntax_reset(s);
    CHECK(syntax_parse(s, &b, 1) == 0);
    CHECK(buffer_set(&b, "void x() {\n}\n", 13) == 0);
    syntax_edit(s, 0, len, 13, 0, 0, count * 5, 0, 2, 0);
    CHECK(syntax_parse(s, &b, 0) == 1 && folds(s, &b, &f) == 1);
    /* Above the existing highlighter limit both metadata APIs switch off. */
    len = (8u << 20) + 1;
    text = malloc(len + 1);
    CHECK(text);
    memset(text, 'x', len);
    CHECK(buffer_take(&b, text, len) == 0);
    syntax_reset(s);
    CHECK(syntax_parse(s, &b, 1) == -1 && syntax_off(s) && !syntax_pending(s));
    CHECK(syntax_folds(s, &b, &f) == 0 && f == NULL);
    uint8_t flags = 99;
    syntax_styles(s, &b, 0, 1, &flags);
    CHECK(flags == 0);
    CHECK(syntax_parse(s, &b, 0) == -1);
    syntax_free(s);
    buffer_free(&b);
    puts("PASS partial parse cancellation and >8 MiB off state never expose invalid folds/styles");
}

static void fixture_query(void)
{
    struct buffer b;
    struct syntax *s = document(&b, "C", "void f() {\n  x();\n}\n/* keep\n note\n*/\n/* reject\n note\n*/\n");
    const struct syntax_fold *f;
    CHECK(folds(s, &b, &f) == 1);
    range(f, 1, 3, 5);
    size_t at = buffer_line_start(&b, 3) + 3;
    CHECK(strcmp(syntax_capture_at(s, &b, at), "comment.documentation") == 0);
    uint8_t flags = 99;
    syntax_styles(s, &b, at, at + 1, &flags);
    CHECK(flags == 0); /* verified Breeze Documentation is not italic */
    syntax_free(s);
    buffer_free(&b);
    puts("PASS actual C tree with installed-style folds.scm: inheritance/predicates/private captures/query precedence and Documentation style");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "query") == 0) {
        fixture_query();
        return 0;
    }
    primary_languages();
    packaged_languages();
    incremental();
    syntax_not_braces();
    styles();
    budget_and_off();
    puts("PASS all parser-backed folding and style tests");
    return 0;
}
