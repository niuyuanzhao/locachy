/*
 * edit's text, undo, search and files without a screen (edit-buffer.c,
 * edit-doc.c, edit-search.c, edit-file.c): typing as edit types (an edit
 * per key), Kate's undo grouping, search and replace, and files read and
 * written as the user, byte for byte.
 *   test-edit-core DIR   (DIR: made for the files; as root, given to uid 1000)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base.h"
#include "i18n-stub.h"
#include "../progs/edit-buffer.h"
#include "../progs/edit-doc.h"
#include "../progs/edit-file.h"
#include "../progs/edit-search.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

int screen_char_cells(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE4F) || (cp >= 0xFF01 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x2FA1F) ? 2 : 1;
}

static char dir[256];
static int is_root;

static char *text_of(struct buffer *b)
{
    return buffer_dup(b, 0, buffer_length(b));
}

static int text_is(struct buffer *b, const char *want)
{
    char *t = text_of(b);
    int ok = t && strlen(t) == buffer_length(b) && strcmp(t, want) == 0;
    if (!ok) {
        printf("  text is \"%s\", not \"%s\"\n", t ? t : "(null)", want);
    }
    free(t);
    return ok;
}

/* One key typed as edit types it: an edit of its own. */
static size_t type(struct doc *d, size_t at, const char *s)
{
    doc_begin(d, at, at);
    doc_insert(d, at, s, strlen(s));
    doc_end(d, at + strlen(s), at + strlen(s));
    return at + strlen(s);
}

static size_t backspace(struct doc *d, size_t at)
{
    size_t p = buffer_prev_char(&d->buf, at);
    doc_begin(d, at, at);
    doc_remove(d, p, at - p);
    doc_end(d, p, p);
    return p;
}

static void undo_all(struct doc *d, int *steps)
{
    size_t c, a;
    *steps = 0;
    while (doc_undo(d, &c, &a)) {
        (*steps)++;
    }
}

static void test_buffer(void)
{
    struct buffer b;
    CHECK(buffer_init(&b) == 0);
    CHECK(buffer_insert(&b, 0, "ab\n中文\ncd", strlen("ab\n中文\ncd")) == 0);
    CHECK(b.lines == 3 && buffer_line_start(&b, 1) == 3 && buffer_line_length(&b, 1) == 6);
    CHECK(buffer_line_start(&b, 2) == 10 && buffer_line_of(&b, 9) == 1 && buffer_line_of(&b, 10) == 2);
    /* Into the middle of the CJK line, a line break and more CJK. */
    CHECK(buffer_insert(&b, 6, "X\n字", strlen("X\n字")) == 0);
    CHECK(text_is(&b, "ab\n中X\n字文\ncd") && b.lines == 4);
    /* Across lines: from inside line 0 to inside line 2. */
    buffer_delete(&b, 1, 10);
    CHECK(text_is(&b, "a文\ncd") && b.lines == 2 && buffer_line_start(&b, 1) == 5);
    /* Many at once (replace all). */
    struct replacement r[] = { { 0, 1, "AAA", 3 }, { 5, 2, "", 0 } };
    CHECK(buffer_replace_many(&b, r, 2) == 0 && text_is(&b, "AAA文\n") && b.lines == 2);
    buffer_free(&b);
    puts("PASS buffer: inserts and deletes across lines and CJK, the line index, replacements at once");

    /* Characters: a 3-byte CJK, a 4-byte emoji, a byte that is none, and
     * clusters (e and a combining acute; a ZWJ emoji sequence). */
    const char *s = "中😀\xff" "e\xcc\x81" "x" "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb" "y";
    CHECK(buffer_init(&b) == 0 && buffer_insert(&b, 0, s, strlen(s)) == 0);
    size_t at = 0, steps[16], n = 0;
    while (at < buffer_length(&b)) {
        at = buffer_next_char(&b, at);
        steps[n++] = at;
    }
    CHECK(n == 10 && steps[0] == 3 && steps[1] == 7 && steps[2] == 8 && steps[3] == 9 && steps[4] == 11);
    for (size_t i = n; i > 1; i--) {
        CHECK(buffer_prev_char(&b, steps[i - 1]) == steps[i - 2]);
    }
    size_t c1 = buffer_next_cluster(&b, 8), c2 = buffer_next_cluster(&b, 12);
    CHECK(c1 == 11 && c2 == 23 && buffer_prev_cluster(&b, 11) == 8 && buffer_prev_cluster(&b, 23) == 12);
    size_t k;
    CHECK(buffer_char(&b, 7, &k) == '?' && k == 1); /* drawn as screen_text draws it */
    buffer_free(&b);
    puts("PASS characters: by code point over CJK, emoji and invalid bytes; clusters keep combining marks and ZWJ");
}

static void test_undo(void)
{
    struct doc d;
    CHECK(doc_init(&d) == 0);
    size_t at = 0, c, a;
    int steps;
    at = type(&d, at, "a");
    at = type(&d, at, "b");
    at = type(&d, at, " ");
    at = type(&d, at, "c");
    CHECK(d.count == 1 && doc_modified(&d));
    CHECK(doc_undo(&d, &c, &a) && text_is(&d.buf, "") && c == 0);
    CHECK(doc_redo(&d, &c, &a) && text_is(&d.buf, "ab c") && c == 4);
    puts("PASS undo: typed characters, a space among them, are one step (KateUndoGroup::merge)");

    at = type(&d, at, "\n");
    at = type(&d, at, "d");
    at = type(&d, at, "e");
    CHECK(d.count == 3);
    CHECK(doc_undo(&d, &c, &a) && text_is(&d.buf, "ab c\n"));
    CHECK(doc_undo(&d, &c, &a) && text_is(&d.buf, "ab c"));
    CHECK(doc_redo(&d, &c, &a) && doc_redo(&d, &c, &a) && text_is(&d.buf, "ab c\nde"));
    at = backspace(&d, at);
    at = backspace(&d, at);
    CHECK(d.count == 4 && text_is(&d.buf, "ab c\n"));
    at = type(&d, at, "f");
    CHECK(d.count == 5);
    CHECK(doc_undo(&d, &c, &a) && doc_undo(&d, &c, &a) && text_is(&d.buf, "ab c\nde"));
    CHECK(doc_redo(&d, &c, &a) && doc_redo(&d, &c, &a) && text_is(&d.buf, "ab c\nf"));
    puts("PASS undo: a line break, Backspaces and typing after them are steps of their own");

    doc_safe_point(&d); /* the cursor moved */
    at = type(&d, at, "g");
    CHECK(d.count == 6);
    CHECK(doc_undo(&d, &c, &a) && text_is(&d.buf, "ab c\nf"));
    at = buffer_length(&d.buf);
    at = type(&d, at, "x");
    CHECK(d.count == 6 && !doc_redo(&d, &c, &a) && text_is(&d.buf, "ab c\nfx"));
    puts("PASS undo: a cursor move ends the step (undoSafePoint); a new edit after undo drops the redo");

    doc_mark_saved(&d);
    CHECK(!doc_modified(&d));
    at = type(&d, at, "y");
    CHECK(doc_modified(&d) && d.count == 7); /* not merged into the saved step */
    CHECK(doc_undo(&d, &c, &a) && !doc_modified(&d));
    CHECK(doc_redo(&d, &c, &a) && doc_modified(&d));
    CHECK(doc_undo(&d, &c, &a) && doc_undo(&d, &c, &a) && doc_modified(&d));
    puts("PASS undo: saving is a safe point, undo and redo back to it make the document unmodified");

    /* Replace all: one step, undone and redone. */
    doc_set_text(&d, "one two one", 11);
    struct replacement r[] = { { 0, 3, "1", 1 }, { 8, 3, "111", 3 } };
    CHECK(doc_replace_many(&d, r, 2) == 0 && text_is(&d.buf, "1 two 111") && d.count == 1);
    CHECK(doc_undo(&d, &c, &a) && text_is(&d.buf, "one two one"));
    CHECK(doc_redo(&d, &c, &a) && text_is(&d.buf, "1 two 111"));
    undo_all(&d, &steps);
    CHECK(steps == 1);
    doc_free(&d);
    puts("PASS undo: replacements at once are one step, undone and redone");
}

static void find(struct search *s, struct buffer *b, size_t at, int back, size_t *x, size_t *y, int want)
{
    CHECK(search_find(s, b, at, back, 0, buffer_length(b), x, y) == want);
}

static char *replace_with(struct search *s, struct buffer *b, const char *r, int counter)
{
    size_t n;
    char *t = search_replacement(s, b, r, counter, &n);
    CHECK(t && strlen(t) == n);
    return t;
}

static void test_search(void)
{
    struct buffer b;
    struct search s = { 0 };
    const char *text = "Foo foo food\nbar FOO\n  x = 12; y = 345;\n中文 search 中文\n";
    CHECK(buffer_init(&b) == 0 && buffer_insert(&b, 0, text, strlen(text)) == 0);
    size_t x, y;
    CHECK(search_compile(&s, "foo", SEARCH_PLAIN, 0) == 0);
    find(&s, &b, 0, 0, &x, &y, 1);
    CHECK(x == 0 && y == 3);
    find(&s, &b, 1, 0, &x, &y, 1);
    CHECK(x == 4);
    find(&s, &b, buffer_length(&b), 1, &x, &y, 1);
    CHECK(x == 17); /* backwards: the last one before the end, FOO */
    CHECK(search_compile(&s, "foo", SEARCH_PLAIN, 1) == 0);
    find(&s, &b, 5, 0, &x, &y, 1);
    CHECK(x == 8); /* case: "food" */
    CHECK(search_compile(&s, "foo", SEARCH_WORDS, 0) == 0);
    find(&s, &b, 5, 0, &x, &y, 1);
    CHECK(x == 17); /* whole words: not food */
    CHECK(search_compile(&s, "文 s", SEARCH_PLAIN, 1) == 0);
    find(&s, &b, 0, 0, &x, &y, 1);
    CHECK(x == strlen("Foo foo food\nbar FOO\n  x = 12; y = 345;\n中"));
    puts("PASS search: plain, matching case, whole words, backwards, CJK");

    CHECK(search_compile(&s, "([a-z]) = ([0-9]+)", SEARCH_REGEX, 1) == 0);
    find(&s, &b, 0, 0, &x, &y, 1);
    char *r = replace_with(&s, &b, "\\2=\\U\\1\\E\\{2}\\n\\#", 7);
    CHECK(strcmp(r, "12=X12\n7") == 0);
    free(r);
    r = replace_with(&s, &b, "\\u\\1\\9-\\x0041\\0101\\0", 1);
    CHECK(strcmp(r, "X9-AAx = 12") == 0); /* \9: no such group, its number */
    free(r);
    /* A \{n} past int names no group: its digits as text. Forty #s: the counter
     * padded to forty digits, far past the 32-byte buffer it once had. */
    char hashes[41], rep[128], want[128];
    memset(hashes, '#', 40);
    hashes[40] = '\0';
    snprintf(rep, sizeof rep, "\\{99999999999999999999}:\\%s", hashes);
    snprintf(want, sizeof want, "99999999999999999999:%0*d", 40, 42);
    r = replace_with(&s, &b, rep, 42);
    CHECK(strcmp(r, want) == 0);
    free(r);
    CHECK(search_compile(&s, "\\s+=", SEARCH_REGEX, 1) == 0 && !s.multiline);
    CHECK(search_compile(&s, "FOO$\\n", SEARCH_REGEX, 1) == 0 && s.multiline);
    find(&s, &b, 0, 0, &x, &y, 1);
    CHECK(x == 17 && y == 21);
    CHECK(search_compile(&s, "^bar", SEARCH_REGEX, 1) == 0);
    find(&s, &b, 0, 0, &x, &y, 1);
    CHECK(x == 13);
    puts("PASS regular expressions: ^ and $ per line, \\n across lines; groups, \\U \\E \\u, \\{n}, \\n, \\#, \\x, \\0ooo");

    CHECK(search_compile(&s, "a\\tb\\n", SEARCH_ESCAPES, 1) == 0 && s.multiline);
    CHECK(search_compile(&s, "foo", SEARCH_PLAIN, 1) == 0);
    find(&s, &b, 0, 0, &x, &y, 1);
    r = replace_with(&s, &b, "\\1x", 1);
    CHECK(strcmp(r, "\\1x") == 0); /* plain text: the replacement as it is */
    free(r);
    CHECK(search_compile(&s, "(", SEARCH_REGEX, 1) == -1 && strstr(s.error, "parenthes"));
    CHECK(!search_valid("(", SEARCH_REGEX) && search_valid("(", SEARCH_PLAIN) && !search_valid(" foo", SEARCH_WORDS));
    CHECK(!search_valid("", SEARCH_PLAIN));
    printf("  an invalid pattern: \"%s\"\n", s.error);
    search_free(&s);
    buffer_free(&b);
    puts("PASS invalid patterns: refused with PCRE2's reason, no crash; escape sequences mode; plain replacement");
}

/* path's bytes. */
static char *bytes_of(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    char *s = malloc(1 << 20);
    *n = fread(s, 1, 1 << 20, f);
    fclose(f);
    return s;
}

static void put(const char *path, const void *data, size_t n, mode_t mode, int users)
{
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(data, 1, n, f) == n && fclose(f) == 0);
    CHECK(chmod(path, mode) == 0);
    if (is_root && users) {
        CHECK(chown(path, EDIT_USER, EDIT_USER) == 0);
    }
}

/* Read and written back unchanged: the same bytes. */
static void round_trip(const char *name, const void *data, size_t n, int encoding, int bom, int eol)
{
    char path[700], final[700];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    put(path, data, n, 0644, 1);
    char *text;
    size_t len, longest, got;
    struct file_format f;
    int flags;
    struct stat st;
    uint8_t dg[DIGEST_SIZE], dg2[DIGEST_SIZE];
    CHECK(file_load(path, &text, &len, &f, &flags, &st, dg, 10000, &longest) == 0);
    CHECK(f.encoding == encoding && f.bom == bom && f.eol == eol && !(flags & FILE_BROKEN));
    CHECK(file_save(path, text, len, &f, &st, final, sizeof final, dg2) == 0);
    char *back = bytes_of(path, &got);
    CHECK(got == n && memcmp(back, data, n) == 0 && memcmp(dg, dg2, DIGEST_SIZE) == 0);
    free(back);
    free(text);
}

static void test_files(void)
{
    /* The checksum: git's blob SHA-1 (git hash-object of "hello\n"). */
    char path[700], final[700], hex[41];
    uint8_t dg[DIGEST_SIZE];
    snprintf(path, sizeof path, "%s/hello", dir);
    put(path, "hello\n", 6, 0644, 1);
    CHECK(file_digest(path, dg) == 0);
    file_digest_hex(dg, hex);
    CHECK(strcmp(hex, "ce013625030ba8dba906f756967f9e9ca394464a") == 0);
    puts("PASS checksum: git's blob SHA-1, as KTextEditor's");

    round_trip("crlf.txt", "one\r\ntwo\r\n\r\n", 12, ENC_UTF8, 0, EOL_CRLF);
    round_trip("bom.txt", "\xef\xbb\xbfh\xc3\xa9llo\n", 10, ENC_UTF8, 1, EOL_LF);
    round_trip("utf16le.txt", "\xff\xfeh\0i\0\n\0>\xd8\x00\xdd\n\0", 14, ENC_UTF16LE, 1, EOL_LF);
    round_trip("utf16be.txt", "\xfe\xff\0h\0i\0\r\0\n", 10, ENC_UTF16BE, 1, EOL_CRLF);
    /* Two Chinese characters and CRLF in GB18030 (not valid UTF-8). */
    round_trip("gb.txt", "\xd6\xd0\xce\xc4\r\n\x81\x30\x81\x30\r\n", 12, ENC_GB18030, 0, EOL_CRLF);
    round_trip("mac.txt", "a\rb\r", 4, ENC_UTF8, 0, EOL_CR);
    puts("PASS round trips byte for byte: CRLF, UTF-8 with BOM, UTF-16LE/BE with BOM, GB18030, CR");

    char *text;
    size_t len, longest;
    struct file_format f;
    int flags;
    struct stat st;
    snprintf(path, sizeof path, "%s/mixed.txt", dir);
    put(path, "a\nb\r\nc\rd", 8, 0644, 1);
    CHECK(file_load(path, &text, &len, &f, &flags, &st, dg, 10000, &longest) == 0 && f.eol == EOL_CRLF);
    CHECK(len == 7 && memcmp(text, "a\nb\nc\nd", 7) == 0);
    free(text);
    snprintf(path, sizeof path, "%s/broken.txt", dir);
    put(path, "ok\x80\xff\xfe!", 7, 0644, 1);
    CHECK(file_load(path, &text, &len, &f, &flags, &st, dg, 10000, &longest) == 0 && (flags & FILE_BROKEN));
    CHECK(len == 7 && memcmp(text, "ok\x80\xff\xfe!", 7) == 0);
    free(text);
    /* NUL bytes: valid UTF-8, nothing special (KTextEditor's loader). */
    snprintf(path, sizeof path, "%s/binary", dir);
    put(path, "ELF\0\0\1abc", 9, 0644, 1);
    CHECK(file_load(path, &text, &len, &f, &flags, &st, dg, 10000, &longest) == 0 && flags == 0);
    CHECK(f.encoding == ENC_UTF8 && len == 9 && memcmp(text, "ELF\0\0\1abc", 9) == 0);
    free(text);
    snprintf(path, sizeof path, "%s/new.txt", dir);
    CHECK(file_load(path, &text, &len, &f, &flags, &st, dg, 10000, &longest) == 0 && st.st_ino == 0 && len == 0);
    free(text);
    puts("PASS loading: CRLF wins the line ends, broken bytes kept (read-only), NUL bytes as UTF-8, a new file");

    static const struct {
        const char *bytes;
        size_t len;
        int encoding;
    } explicit[] = {
        { "h\0i\0\n\0", 6, ENC_UTF16LE },
        { "\0h\0i\0\n", 6, ENC_UTF16BE },
        { "caf\xe9\r\n", 6, ENC_LATIN1 },
        { "\x80 price\n", 8, ENC_WINDOWS1252 },
        { "\xd6\xd0\xce\xc4\n", 5, ENC_GB18030 }
    };
    for (size_t i = 0; i < sizeof explicit / sizeof *explicit; i++) {
        snprintf(path, sizeof path, "%s/explicit-%zu", dir, i);
        put(path, explicit[i].bytes, explicit[i].len, 0644, 1);
        CHECK(file_load_encoding(path, &text, &len, &f, &flags, &st, dg, 10000, &longest,
                                 explicit[i].encoding) == 0);
        CHECK(!flags && f.encoding == explicit[i].encoding && !f.bom);
        CHECK(file_save(path, text, len, &f, &st, final, sizeof final, dg) == 0);
        size_t got;
        char *back = bytes_of(path, &got);
        CHECK(got == explicit[i].len && memcmp(back, explicit[i].bytes, got) == 0);
        free(back);
        free(text);
    }
    snprintf(path, sizeof path, "%s/explicit-invalid", dir);
    put(path, "bad \xff\n", 6, 0644, 1);
    CHECK(file_load_encoding(path, &text, &len, &f, &flags, &st, dg, 10000, &longest, ENC_UTF8) == 0);
    CHECK((flags & FILE_BROKEN) && f.encoding == ENC_UTF8 && len == 6);
    free(text);
    puts("PASS explicit reopen encodings: UTF-16 without BOM, Latin1, Windows-1252 and GB18030 roundtrip; invalid UTF-8 stays protected");

    /* Line Length Limit: 25000 characters, a space at 9500 and a comma at 19000. */
    char *line = malloc(25002);
    memset(line, 'x', 25000);
    line[9500] = ' ';
    line[19000] = ',';
    line[25000] = '\n';
    snprintf(path, sizeof path, "%s/long.txt", dir);
    put(path, line, 25001, 0644, 1);
    CHECK(file_load(path, &text, &len, &f, &flags, &st, dg, 10000, &longest) == 0 && (flags & FILE_WRAPPED));
    CHECK(longest == 25000 && len == 25003 && text[9501] == '\n' && text[19002] == '\n');
    free(text);
    CHECK(file_load(path, &text, &len, &f, &flags, &st, dg, 32768, &longest) == 0 && !(flags & FILE_WRAPPED));
    free(text);
    free(line);
    puts("PASS line length limit: wrapped after a space or punctuation in the last tenth, as KTextEditor's loader");

    /* The user's file in the user's 0700 folder: written as the user, its
     * mode and owner kept; the new file goes in beside it and is renamed. */
    char home[300];
    snprintf(home, sizeof home, "%s/home", dir);
    CHECK(mkdir(home, 0700) == 0);
    if (is_root) {
        CHECK(chown(home, EDIT_USER, EDIT_USER) == 0);
    }
    snprintf(path, sizeof path, "%s/notes.txt", home);
    put(path, "old\n", 4, 0640, 1);
    struct stat before;
    CHECK(stat(path, &before) == 0);
    int ino = inotify_init1(IN_NONBLOCK);
    CHECK(ino >= 0 && inotify_add_watch(ino, home, IN_CREATE | IN_MOVED_FROM | IN_MOVED_TO) >= 0);
    struct file_format plain = { ENC_UTF8, 0, EOL_LF };
    CHECK(file_save(path, "new\n", 4, &plain, &st, final, sizeof final, dg) == 0);
    char ev[4096];
    ssize_t n = read(ino, ev, sizeof ev);
    int created = 0, moved_from = 0, moved_to = 0;
    for (char *p = ev; n > 0 && p < ev + n;) {
        struct inotify_event *e = (struct inotify_event *)p;
        if (e->len && strncmp(e->name, "notes.txt.", 10) == 0 && strlen(e->name) == 16) {
            created |= (e->mask & IN_CREATE) != 0;
            moved_from |= (e->mask & IN_MOVED_FROM) != 0;
        }
        if (e->len && strcmp(e->name, "notes.txt") == 0 && (e->mask & IN_MOVED_TO)) {
            moved_to = 1;
        }
        p += sizeof *e + e->len;
    }
    close(ino);
    CHECK(created && moved_from && moved_to);
    struct stat after;
    CHECK(stat(path, &after) == 0 && after.st_ino != before.st_ino && (after.st_mode & 07777) == 0640);
    CHECK(!is_root || (after.st_uid == EDIT_USER && after.st_gid == EDIT_USER));
    size_t got;
    char *back = bytes_of(path, &got);
    CHECK(got == 4 && memcmp(back, "new\n", 4) == 0);
    free(back);
    puts("PASS saving: NAME.XXXXXX beside the file, then renamed over it; mode 0640 and owner 1000 kept");

    /* Written through a symbolic link to its target, the link kept. */
    char link[700], target[700];
    snprintf(target, sizeof target, "%s/target.txt", home);
    snprintf(link, sizeof link, "%s/link.txt", home);
    put(target, "t\n", 2, 0600, 1);
    CHECK(symlink("target.txt", link) == 0);
    if (is_root) {
        CHECK(lchown(link, EDIT_USER, EDIT_USER) == 0);
    }
    CHECK(file_save(link, "via link\n", 9, &plain, &st, final, sizeof final, dg) == 0 && strcmp(final, target) == 0);
    struct stat ls;
    CHECK(lstat(link, &ls) == 0 && S_ISLNK(ls.st_mode));
    back = bytes_of(target, &got);
    CHECK(got == 9 && memcmp(back, "via link\n", 9) == 0);
    free(back);
    puts("PASS a symbolic link is written through to its target and stays a link (QSaveFile)");

    if (is_root) {
        /* Root's own file, even in the user's folder: neither read nor written. */
        char secret[700];
        snprintf(secret, sizeof secret, "%s/root-only", home);
        put(secret, "secret\n", 7, 0600, 0);
        CHECK(file_load(secret, &text, &len, &f, &flags, &st, dg, 10000, &longest) == EACCES);
        CHECK(file_save(secret, "pwned\n", 6, &plain, &st, final, sizeof final, dg) == EACCES);
        back = bytes_of(secret, &got);
        CHECK(got == 7 && memcmp(back, "secret\n", 7) == 0);
        free(back);
        struct stat rs;
        CHECK(stat(secret, &rs) == 0 && rs.st_uid == 0);
        /* No temporary file left behind. */
        DIR *dd = opendir(home);
        struct dirent *de;
        int left = 0;
        while (dd && (de = readdir(dd)) != NULL) {
            left += strncmp(de->d_name, "root-only.", 10) == 0;
        }
        closedir(dd);
        CHECK(left == 0);
        /* Root's folder: not even a new file there. */
        char rdir[300], rfile[400];
        snprintf(rdir, sizeof rdir, "%s/rootdir", dir);
        CHECK(mkdir(rdir, 0755) == 0);
        snprintf(rfile, sizeof rfile, "%s/new.txt", rdir);
        CHECK(file_save(rfile, "x\n", 2, &plain, &st, final, sizeof final, dg) == EACCES && access(rfile, F_OK) != 0);
        /* And the permissions are root's again afterwards. */
        CHECK(access(secret, R_OK | W_OK) == 0);
        puts("PASS as root: a 0600 root file is neither read nor written (EACCES), root's folder refuses a new file");
    } else {
        puts("SKIP the root-only checks: not run as root");
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    is_root = geteuid() == 0;
    CHECK(mkdir(argv[1], 0755) == 0);
    snprintf(dir, sizeof dir, "%s", argv[1]);
    if (is_root) {
        CHECK(chown(dir, EDIT_USER, EDIT_USER) == 0);
    }
    test_buffer();
    test_undo();
    test_search();
    test_files();
    puts("ALL PASSED");
    return 0;
}
