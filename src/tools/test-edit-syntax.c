/*
 * edit's highlighting (edit-syntax.c) with the grammars as packaged and the
 * queries (the packages', else edit/queries): each language loads, its
 * highlights.scm compiles, and a sample's keyword and string (or what the
 * language has instead) get their captures and colours. Then the choice of
 * language, the predicates, and the bounds on big texts.
 *   test-edit-syntax DIR
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base.h"
#include "i18n-stub.h"
#include "../progs/edit-buffer.h"
#include "../progs/edit-syntax.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

int screen_char_cells(uint32_t cp)
{
    (void)cp;
    return 1;
}

static double ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6;
}

static long rss_kib(const char *field)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, field, strlen(field)) == 0) {
            v = atol(line + strlen(field) + 1);
        }
    }
    if (f) {
        fclose(f);
    }
    return v;
}

struct sample {
    const char *lang, *text;
    const char *word1, *capture1; /* a keyword (or the language's nearest) */
    const char *word2, *capture2; /* a string */
    uint32_t colour1, colour2;
};

static const struct sample samples[] = {
    { "Bash", "if [ -n \"$x\" ]; then echo \"hi\"; fi\n", "if", "keyword", "\"hi\"", "string", 0xFFFFFF, 0xF44F4F },
    { "C", "static const char *s = \"hi\";\n", "static", "keyword", "\"hi\"", "string", 0xFFFFFF, 0xF44F4F },
    /* namespace is C++'s query's, the string C's: both are read. */
    { "C++", "namespace n { const char *s = \"x\"; }\n", "namespace", "keyword", "\"x\"", "string", 0xFFFFFF,
      0xF44F4F },
    { "JavaScript", "const s = \"hi\";\n", "const", "keyword", "\"hi\"", "string", 0xFFFFFF, 0xF44F4F },
    { "Lua", "local s = \"hi\"\n", "local", "keyword", "\"hi\"", "string", 0xFFFFFF, 0xF44F4F },
    /* Markdown has headings and code: the heading is the block grammar's,
     * the code span the inline grammar's. */
    { "Markdown", "# Title\n\nsome `code` here\n", "Title", "text.title", "code", "text.literal", 0x8E44AD, 0xC45B00 },
    { "Python", "def f():\n    return \"hi\"\n", "def", "keyword", "\"hi\"", "string", 0xFFFFFF, 0xF44F4F },
    { "Tree-sitter Query", "(MISSING identifier) @x\n((identifier) @v (#eq? @v \"str\"))\n", "MISSING", "keyword",
      "\"str\"", "string", 0xFFFFFF, 0xF44F4F },
    { "Rust", "fn main() { let s = \"hi\"; }\n", "fn", "keyword", "\"hi\"", "string", 0xFFFFFF, 0xF44F4F },
    { "Vim", "let s = \"hi\"\n", "let", "keyword", "\"hi\"", "string", 0xFFFFFF, 0xF44F4F },
    /* Vim's help: a tag and a link to one. */
    { "Vim Help", "*tag*\tsee |link|\n", "tag", "label", "link", "markup.link", 0x27AE60, 0x27AE60 },
};

static void check_sample(const struct sample *s)
{
    int lang = syntax_by_name(s->lang);
    CHECK(lang >= 0);
    char why[640];
    struct syntax *sx = syntax_new(lang, why, sizeof why);
    if (!sx) {
        printf("FAIL %s: %s\n", s->lang, why);
        exit(1);
    }
    struct buffer b;
    CHECK(buffer_init(&b) == 0 && buffer_insert(&b, 0, s->text, strlen(s->text)) == 0);
    CHECK(syntax_parse(sx, &b, 0) == 1);
    const char *w[2] = { s->word1, s->word2 }, *c[2] = { s->capture1, s->capture2 };
    uint32_t want[2] = { s->colour1, s->colour2 };
    for (int i = 0; i < 2; i++) {
        const char *at = strstr(s->text, w[i]);
        CHECK(at);
        size_t off = (size_t)(at - s->text);
        const char *got = syntax_capture_at(sx, &b, off);
        uint32_t colour;
        syntax_colours(sx, &b, off, off + 1, &colour);
        if (strcmp(got, c[i]) != 0 || colour != want[i]) {
            printf("FAIL %s: \"%s\" is @%s (#%06X), not @%s (#%06X)\n", s->lang, w[i], got, colour, c[i], want[i]);
            exit(1);
        }
    }
    printf("PASS %s: grammar and highlights.scm load; \"%s\" @%s #%06X, %s @%s #%06X\n", s->lang, s->word1,
           s->capture1, s->colour1, s->word2, s->capture2, s->colour2);
    buffer_free(&b);
    syntax_free(sx);
}

static int detect(const char *path, const char *text)
{
    struct buffer b;
    CHECK(buffer_init(&b) == 0 && buffer_insert(&b, 0, text, strlen(text)) == 0);
    int lang = syntax_detect(path, &b);
    buffer_free(&b);
    return lang;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    CHECK(syntax_count() == (int)(sizeof samples / sizeof samples[0]));
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        check_sample(&samples[i]);
    }

    CHECK(detect("/h/a.c", "") == syntax_by_name("C") && detect("/h/a.hpp", "") == syntax_by_name("C++"));
    CHECK(detect("/h/PKGBUILD", "") == syntax_by_name("Bash") && detect("/h/x", "#!/usr/bin/env python3\n") ==
          syntax_by_name("Python"));
    CHECK(detect("/h/x", "#!/bin/sh\n") == syntax_by_name("Bash"));
    CHECK(detect("/h/x.txt", "text\n// kate: hl Lua;\n") == syntax_by_name("Lua"));
    CHECK(detect("/h/doc/x.txt", "*x.txt*\n vim:tw=78:ts=8:ft=help:norl:\n") == syntax_by_name("Vim Help"));
    CHECK(detect("/h/notes.txt", "plain\n") == -1);
    puts("PASS language by name, #! line, and modeline (kate: hl, vim's ft=)");

    /* A predicate: C's constant pattern (#match? "^[A-Z][A-Z\\d_]*$"). */
    int c = syntax_by_name("C");
    char why[640];
    struct syntax *sx = syntax_new(c, why, sizeof why);
    struct buffer b;
    const char *code = "int FOO = 1; int foo = 2; /* if (x) */\n";
    CHECK(sx && buffer_init(&b) == 0 && buffer_insert(&b, 0, code, strlen(code)) == 0 && syntax_parse(sx, &b, 0) == 1);
    CHECK(strcmp(syntax_capture_at(sx, &b, 4), "constant") == 0);
    CHECK(strcmp(syntax_capture_at(sx, &b, 17), "variable") == 0);
    CHECK(strcmp(syntax_capture_at(sx, &b, 30), "comment") == 0);
    /* An edit, parsed again from the tree (ts_tree_edit). */
    buffer_insert(&b, 0, "\"s\";", 4);
    syntax_edit(sx, 0, 0, 4, 0, 0, 0, 0, 0, 4);
    CHECK(syntax_pending(sx) && syntax_parse(sx, &b, 0) == 1 && strcmp(syntax_capture_at(sx, &b, 8), "constant") == 0);
    puts("PASS predicates (#match?), the innermost node and the last pattern win, an incremental edit");
    syntax_free(sx);
    buffer_free(&b);

    /* Big texts: parsed 30 ms at a time, so the program keeps answering;
     * past 8 MiB not highlighted (tree-sitter's tree is of all of it). */
    size_t unit = strlen("static int f(int a) { return a + \"s\"[0]; }\n"), n = (7u << 20) / unit;
    char *big = malloc(n * unit + 1);
    for (size_t i = 0; i < n; i++) {
        memcpy(big + i * unit, "static int f(int a) { return a + \"s\"[0]; }\n", unit);
    }
    CHECK(buffer_init(&b) == 0 && buffer_take(&b, big, n * unit) == 0);
    sx = syntax_new(c, why, sizeof why);
    long before = rss_kib("VmRSS:");
    double t0 = ms(), worst = 0;
    int slices = 0, r;
    do {
        double s0 = ms();
        r = syntax_parse(sx, &b, 30);
        double took = ms() - s0;
        worst = took > worst ? took : worst;
        slices++;
    } while (r == 0);
    double total = ms() - t0;
    CHECK(r == 1);
    uint32_t colour;
    syntax_colours(sx, &b, buffer_length(&b) - unit, buffer_length(&b) - unit + 1, &colour);
    printf("  7 MiB of C (%zu lines): parsed in %.0f ms as %d slices, the longest %.1f ms; RSS %ld -> %ld KiB\n", n,
           total, slices, worst, before, rss_kib("VmRSS:"));
    CHECK(worst < 250 && colour == 0xFFFFFF);
    syntax_free(sx);
    buffer_free(&b);
    size_t huge = (9u << 20) / unit + 1;
    big = malloc(huge * unit + 1);
    for (size_t i = 0; i < huge; i++) {
        memcpy(big + i * unit, "static int f(int a) { return a + \"s\"[0]; }\n", unit);
    }
    CHECK(buffer_init(&b) == 0 && buffer_take(&b, big, huge * unit) == 0);
    sx = syntax_new(c, why, sizeof why);
    t0 = ms();
    CHECK(syntax_parse(sx, &b, 30) == -1 && syntax_off(sx) && ms() - t0 < 50);
    printf("  9 MiB of C: \"%s\"\n", syntax_off(sx));
    syntax_free(sx);
    buffer_free(&b);
    printf("PASS big texts: a parse in slices of 30 ms (the longest above), none past %u MiB\n", 8u);
    printf("  peak memory (VmHWM) %ld KiB\n", rss_kib("VmHWM:"));
    puts("ALL PASSED");
    return 0;
}
