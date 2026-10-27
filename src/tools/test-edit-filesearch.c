/* Generated fixtures only: never enumerate a real home directory. Including
 * the backend also tests its private framed parser with arbitrary fragments. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base.h"
#include "i18n-stub.h"
#include "../progs/edit-filesearch.c"
#include <assert.h>
#include <time.h>

static char root[256];
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static double now(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void name(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", root, rel); }
static void directory(const char *rel, mode_t mode)
{
    char p[1024]; name(p, sizeof p, rel); assert(mkdir(p, mode) == 0); assert(chown(p, EDIT_USER, EDIT_USER) == 0);
}
static void file_bytes(const char *rel, const char *text, size_t len, mode_t mode)
{
    char p[1024]; name(p, sizeof p, rel);
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, mode); assert(fd >= 0);
    assert(write(fd, text, len) == (ssize_t)len); assert(fchmod(fd, mode) == 0); close(fd);
    assert(chown(p, EDIT_USER, EDIT_USER) == 0);
}
static void file_text(const char *rel, const char *text) { file_bytes(rel, text, strlen(text), 0644); }
static struct edit_filesearch_options options(const char *pattern)
{
    return (struct edit_filesearch_options){.root = root, .pattern = pattern, .mode = SEARCH_PLAIN,
        .match_case = 1, .recursive = 1, .include = "*.txt"};
}
static struct edit_filesearch *start(struct edit_filesearch_options o)
{
    char error[256]; struct edit_filesearch *s = edit_filesearch_start(&o, error, sizeof error);
    if (!s) { fprintf(stderr, "start failed: %s\n", error); failures++; }
    return s;
}
static void finish(struct edit_filesearch *s)
{
    assert(s); double deadline = now() + 15;
    while (!edit_filesearch_done(s) && now() < deadline) {
        double before = now(); (void)edit_filesearch_read(s);
        CHECK(now() - before < 0.2);
        struct pollfd p = {edit_filesearch_fd(s), POLLIN, 0}; (void)poll(&p, 1, 5);
    }
    CHECK(edit_filesearch_done(s));
    /* Completion also kills/reaps the worker; retaining results leaks no child. */
    (void)edit_filesearch_read(s);
    CHECK(s->pid == 0 && edit_filesearch_fd(s) == -1);
}
static size_t hits(struct edit_filesearch *s, const char *rel)
{
    size_t n = 0; char p[1024]; name(p, sizeof p, rel);
    for (size_t i = 0; i < s->count; i++) if (!strcmp(s->matches[i].path, p)) n++;
    return n;
}
static void fixtures(void)
{
    snprintf(root, sizeof root, "/tmp/edit-filesearch-fixture-XXXXXX"); assert(mkdtemp(root));
    assert(chmod(root, 0755) == 0); assert(chown(root, EDIT_USER, EDIT_USER) == 0);
    directory("sub", 0755); directory("omit", 0755); directory(".secret", 0755);
    file_text("a.txt", "needle one\nneedle two\nneedles\n");
    file_text("b.c", "needle code\n"); file_text("sub/c.txt", "needle sub\n");
    file_text("omit/o.txt", "needle excluded\n"); file_text(".dot.txt", "needle hidden\n");
    file_text(".secret/s.txt", "needle secret\n");
    file_bytes("nul.txt", "needle\0payload", 14, 0644);
    file_bytes("control.txt", "needle\001payload", 14, 0644);
    const char utf16[] = "\xff\xfe" "n\0e\0e\0d\0l\0e\0\r\0\n\0";
    file_bytes("utf16.txt", utf16, sizeof utf16 - 1, 0644);
    file_bytes("locked.txt", "needle forbidden\n", 17, 0000);
    char p[1024]; name(p, sizeof p, "root-only.txt");
    int fd = open(p, O_CREAT | O_WRONLY, 0640); assert(fd >= 0); assert(write(fd, "needle root\n", 12) == 12); close(fd);
    assert(chown(p, 0, 0) == 0);
    name(p, sizeof p, "sub/loop"); assert(symlink("..", p) == 0);
    name(p, sizeof p, "alias.txt"); assert(symlink("a.txt", p) == 0);
    name(p, sizeof p, "pipe.txt"); assert(mkfifo(p, 0600) == 0);
    /* A valid near-NAME_MAX name must not be truncated. */
    char longname[256]; memset(longname, 'x', 245); memcpy(longname + 245, ".txt", 5);
    file_text(longname, "needle long\n");
}
static void test_tree(void)
{
    struct edit_filesearch_options o = options("needle"); o.exclude = "omit/;root-only.txt";
    struct edit_filesearch *s = start(o); finish(s);
    CHECK(!*s->error); CHECK(hits(s, "a.txt") == 3); CHECK(hits(s, "sub/c.txt") == 1);
    CHECK(!hits(s, "b.c") && !hits(s, "omit/o.txt") && !hits(s, ".dot.txt") && !hits(s, "alias.txt"));
    CHECK(!hits(s, "nul.txt") && !hits(s, "control.txt") && !hits(s, "locked.txt"));
    CHECK(hits(s, "utf16.txt") == 1); CHECK(s->stats.binary == 2); CHECK(s->stats.errors >= 1);
    CHECK(strstr(s->warning, "Permission denied") != NULL);
    char a[1024]; name(a, sizeof a, "a.txt");
    for (size_t i = 0; i < s->count; i++) if (!strcmp(s->matches[i].path, a)) {
        struct edit_filematch *m = &s->matches[i];
        CHECK(m->checked && !m->from_buffer && m->column == 1);
        CHECK(m->line >= 1 && m->line <= 3); CHECK(m->start == (m->line - 1) * 11); CHECK(m->end - m->start == 6);
        CHECK(strstr(m->preview, "needle") != NULL);
    }
    edit_filesearch_free(s);
    o.recursive = 0; s = start(o); finish(s); CHECK(!hits(s, "sub/c.txt")); edit_filesearch_free(s);
    o.recursive = o.hidden = o.follow_links = 1; o.exclude = "omit/*;root-only.txt";
    s = start(o); finish(s); CHECK(!*s->error); CHECK(hits(s, ".dot.txt") == 1 && hits(s, ".secret/s.txt") == 1);
    CHECK(hits(s, "alias.txt") == 3); CHECK(s->count < 30); edit_filesearch_free(s);
    o.include = "sub/*.txt"; o.hidden = o.follow_links = 0; o.exclude = "";
    s = start(o); finish(s); CHECK(s->count == 1 && hits(s, "sub/c.txt") == 1); edit_filesearch_free(s);
    o = options("needle"); o.mode = SEARCH_WORDS; o.include = "a.txt";
    s = start(o); finish(s); CHECK(s->count == 2); edit_filesearch_free(s);
    o.pattern = "NEEDLE"; o.match_case = 0; s = start(o); finish(s); CHECK(s->count == 2); edit_filesearch_free(s);
    puts("PASS tree filters, hidden, recursion, symlinks/loops, binary/UTF-16, permission boundaries, positions");
}
static void test_replacement(void)
{
    file_text("replace.txt", "foo12 foo34\r\nfoo56\r\n");
    struct edit_filesearch_options o = options("foo([0-9]+)"); o.mode = SEARCH_REGEX; o.include = "replace.txt";
    struct edit_filesearch *s = start(o); finish(s); CHECK(s->count == 3 && !*s->error);
    char path[1024], error[256]; name(path, sizeof path, "replace.txt");
    struct edit_filesearch_plan p;
    s->matches[1].checked = 0;
    CHECK(edit_filesearch_prepare(s, path, "bar\\1-\\#", NULL, 0, &p, error, sizeof error) == 0);
    CHECK(p.replacements == 2 && !strcmp(p.text, "bar12-1 foo34\nbar56-2\n"));
    CHECK(p.format.eol == EOL_CRLF); CHECK(edit_filesearch_plan_validate(&p, NULL, 0) == 0);
    /* Preparing has not saved anything. Explicit file_save preserves CRLF. */
    char *text = NULL; size_t len; struct file_format f; uint8_t d[20];
    CHECK(user_snapshot(path, &text, &len, &f, d) == 0); CHECK(!strcmp(text, "foo12 foo34\nfoo56\n")); free(text);
    struct stat st; char final[4096];
    CHECK(file_save(path, p.text, p.length, &p.format, &st, final, sizeof final, d) == 0);
    CHECK(edit_filesearch_plan_validate(&p, NULL, 0) == ESTALE);
    edit_filesearch_plan_free(&p);
    CHECK(edit_filesearch_prepare(s, path, "x", NULL, 0, &p, error, sizeof error) == ESTALE);
    CHECK(strstr(error, "changed") != NULL); edit_filesearch_free(s);
    /* Staleness includes EOL-only changes even with identical normalized text. */
    file_text("replace.txt", "foo12\r\n"); s = start(o); finish(s); file_text("replace.txt", "foo12\n");
    CHECK(edit_filesearch_prepare(s, path, "x", NULL, 0, &p, error, sizeof error) == ESTALE); edit_filesearch_free(s);
    puts("PASS checked regex capture/counter replacement plans, no implicit writes, encoding/EOL, stale disk refusal");
}
static void test_sources(void)
{
    char path[1024]; name(path, sizeof path, "a.txt");
    const char *text = "\xe4\xb8\xad needle\nneedle!\n";
    struct edit_search_source source[] = {{path, text, strlen(text)}};
    struct edit_filesearch_options o = options("needle"); o.sources = source; o.nsources = 1;
    struct edit_filesearch *s = start(o); finish(s); CHECK(s->count == 2);
    CHECK(s->matches[0].from_buffer && s->matches[0].column == 3 && s->matches[0].start == 4);
    struct edit_filesearch_plan p; char error[256];
    CHECK(edit_filesearch_prepare(s, path, "word", text, strlen(text), &p, error, sizeof error) == 0);
    CHECK(!strcmp(p.text, "\xe4\xb8\xad word\nword!\n"));
    CHECK(edit_filesearch_plan_validate(&p, text, strlen(text)) == 0);
    CHECK(edit_filesearch_plan_validate(&p, "changed", 7) == ESTALE);
    CHECK(edit_filesearch_plan_validate(&p, NULL, 0) == EINVAL);
    edit_filesearch_plan_free(&p);
    CHECK(edit_filesearch_prepare(s, path, "word", NULL, 0, &p, error, sizeof error) == EINVAL);
    CHECK(edit_filesearch_prepare(s, path, "word", "changed", 7, &p, error, sizeof error) == ESTALE);
    edit_filesearch_free(s);
    struct edit_search_source duplicates[] = {{path, NULL, 0}, {path, text, strlen(text)}};
    o.sources = duplicates; o.nsources = 2; s = start(o); finish(s);
    CHECK(s->count == 2 && s->matches[0].from_buffer); edit_filesearch_free(s);
    o.nsources = 0; s = start(o); finish(s); CHECK(s->count == 0); edit_filesearch_free(s);
    puts("PASS open-document-only scope, buffer precedence, Unicode columns, stale refusal and non-mutation");
}
static void test_folder_overlays(void)
{
    char path[1024], outside[1024], sub[1024], hidden[1024];
    name(path, sizeof path, "a.txt"); name(outside, sizeof outside, "outside-missing.txt");
    name(sub, sizeof sub, "sub/c.txt"); name(hidden, sizeof hidden, ".dot.txt");
    const char *text = "only-unsaved-token\n";
    struct edit_search_source overlay[] = {
        {path, text, strlen(text)}, {outside, text, strlen(text)},
        {sub, text, strlen(text)}, {hidden, text, strlen(text)}
    };
    struct edit_filesearch_options o = options("only-unsaved-token");
    o.overlays = overlay; o.noverlays = 4; o.include = "a.txt";
    struct edit_filesearch *s = start(o); finish(s);
    CHECK(s->count == 1 && s->matches[0].from_buffer && hits(s, "a.txt") == 1);
    struct edit_filesearch_plan p; char error[256];
    CHECK(edit_filesearch_prepare(s, path, "changed", text, strlen(text), &p, error, sizeof error) == 0);
    CHECK(!strcmp(p.text, "changed\n")); edit_filesearch_plan_free(&p); edit_filesearch_free(s);
    o.pattern = "needle"; s = start(o); finish(s); CHECK(s->count == 0); edit_filesearch_free(s);
    o.pattern = "only-unsaved-token"; o.include = "*"; o.recursive = 0;
    s = start(o); finish(s); CHECK(s->count == 1); edit_filesearch_free(s);
    o.recursive = 1; s = start(o); finish(s); CHECK(s->count == 2 && hits(s, "sub/c.txt") == 1); edit_filesearch_free(s);
    o.exclude = "sub/"; o.hidden = 1; s = start(o); finish(s);
    CHECK(s->count == 2 && hits(s, ".dot.txt") == 1 && !hits(s, "sub/c.txt")); edit_filesearch_free(s);
    /* Lexical root spellings still select the same open buffer. */
    char rootdot[512]; snprintf(rootdot, sizeof rootdot, "%s/.", root);
    o.root = rootdot; o.include = "a.txt"; s = start(o); finish(s);
    CHECK(s->count == 1 && s->matches[0].from_buffer && !strcmp(s->matches[0].path, path)); edit_filesearch_free(s);
    puts("PASS folder/project buffer overlays: unsaved text replaces disk, respects scope/globs/hidden/recursion, checked plan");
}
static void test_errors(void)
{
    struct edit_filesearch_options o = options("("); o.mode = SEARCH_REGEX;
    struct edit_filesearch *s = start(o); finish(s); CHECK(*s->error && !s->count); edit_filesearch_free(s);
    char error[256]; o = options(""); CHECK(!edit_filesearch_start(&o, error, sizeof error)); CHECK(*error);
    o = options("x"); o.include = "[unterminated"; CHECK(!edit_filesearch_start(&o, error, sizeof error));
    o.include = "trailing\\"; CHECK(!edit_filesearch_start(&o, error, sizeof error));
    o = options("x"); o.root = "/tmp/edit-filesearch-nonexistent-fixture";
    s = start(o); finish(s); CHECK(s->stats.errors == 1 && *s->warning && !s->count); edit_filesearch_free(s);
    /* Over-PATH_MAX roots must produce a diagnostic, never truncate/scan elsewhere. */
    char *longpath = malloc(8193); memset(longpath, 'q', 8192); longpath[0] = '/'; longpath[8192] = 0;
    o.root = longpath; s = start(o); finish(s); CHECK(s->stats.errors == 1 && *s->warning); edit_filesearch_free(s); free(longpath);
    puts("PASS malformed patterns/globs, nonexistent folder and overlong path diagnostics");
}
static void test_limits_and_zero_width(void)
{
    char path[1024]; name(path, sizeof path, "oversize.txt");
    int fd = open(path, O_CREAT | O_WRONLY, 0644); assert(fd >= 0);
    assert(ftruncate(fd, (off_t)FILE_MAX + 1) == 0); close(fd); assert(chown(path, EDIT_USER, EDIT_USER) == 0);
    struct edit_filesearch_options o = options("needle"); o.include = "oversize.txt";
    struct edit_filesearch *s = start(o); finish(s);
    CHECK(!s->count && s->stats.skipped == 1 && strstr(s->warning, "File too large")); edit_filesearch_free(s);
    const char *text = "one\ntwo\n";
    struct edit_search_source source = {"/generated-zero.txt", text, strlen(text)};
    o = options("^"); o.mode = SEARCH_REGEX; o.sources = &source; o.nsources = 1;
    s = start(o); finish(s); CHECK(s->count == 3);
    struct edit_filesearch_plan p; char error[256];
    CHECK(edit_filesearch_prepare(s, source.path, ">", text, strlen(text), &p, error, sizeof error) == 0);
    CHECK(!strcmp(p.text, ">one\n>two\n>")); edit_filesearch_plan_free(&p); edit_filesearch_free(s);
    /* PCRE2 multiline/escape behavior is shared with the editor search bar. */
    source.text = "first\nsecond\n"; source.length = strlen(source.text);
    o.pattern = "first\\nsecond"; s = start(o); finish(s); CHECK(s->count == 1 && s->matches[0].end == 12);
    edit_filesearch_free(s); o.mode = SEARCH_ESCAPES; s = start(o); finish(s); CHECK(s->count == 1); edit_filesearch_free(s);
    source.text = "\\E needle"; source.length = strlen(source.text); o.mode = SEARCH_PLAIN; o.pattern = "\\E";
    s = start(o); finish(s); CHECK(s->count == 1 && s->matches[0].end == 2); edit_filesearch_free(s);
    char *many = malloc(RESULTS_MAX + 2); assert(many); memset(many, 'x', RESULTS_MAX + 1); many[RESULTS_MAX + 1] = 0;
    source.text = many; source.length = RESULTS_MAX + 1; o.pattern = "x";
    s = start(o); finish(s); CHECK(s->count > 0 && s->count <= RESULTS_MAX && strstr(s->error, "limit"));
    CHECK(edit_filesearch_prepare(s, source.path, "y", many, source.length, &p, error, sizeof error) == EINVAL);
    edit_filesearch_free(s); free(many);
    puts("PASS file/result bounds, incomplete-search replacement refusal, zero-width/multiline/escape semantics");
}
static void test_protocol(void)
{
    for (int fragment = 1; fragment <= 83; fragment++) {
        int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
        struct edit_filesearch *s = calloc(1, sizeof *s); assert(s); s->fd = pair[0];
        int fl = fcntl(s->fd, F_GETFL); assert(fcntl(s->fd, F_SETFL, fl | O_NONBLOCK) == 0);
        unsigned char wire[125] = {0};
        put32(wire, 88); wire[4] = MSG_MATCH; put32(wire + 6, 5); put32(wire + 10, 1);
        put64(wire + 14, 0); put64(wire + 22, 1); put64(wire + 30, 1); put64(wire + 38, 1);
        memcpy(wire + 86, "/test", 5); wire[91] = 'x';
        put32(wire + 92, 29); /* Intentionally malformed completion size. */
        wire[96] = MSG_DONE;
        size_t offset = 0;
        while (offset < 92) {
            size_t n = (size_t)fragment; if (n > 92 - offset) n = 92 - offset;
            assert(send_all(pair[1], wire + offset, n) == 0); (void)edit_filesearch_read(s); offset += n;
        }
        CHECK(s->count == 1 && !strcmp(s->matches[0].path, "/test") && !strcmp(s->matches[0].preview, "x"));
        assert(send_all(pair[1], wire + 92, 33) == 0); CHECK(edit_filesearch_read(s) == -1); CHECK(*s->error);
        close(pair[1]); edit_filesearch_free(s);
    }
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    struct edit_filesearch *s = calloc(1, sizeof *s); assert(s); s->fd = pair[0];
    unsigned char header[4]; put32(header, FRAME_MAX + 1); assert(send_all(pair[1], header, 4) == 0);
    CHECK(edit_filesearch_read(s) == -1); close(pair[1]); edit_filesearch_free(s);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0); s = calloc(1, sizeof *s); assert(s); s->fd = pair[0];
    put32(header, 10); assert(send_all(pair[1], header, 4) == 0); close(pair[1]); CHECK(edit_filesearch_read(s) == -1); edit_filesearch_free(s);
    puts("PASS framed transport fragmentation (1..83 bytes), malformed/oversized/truncated frames");
}
static void test_cancellation(void)
{
    char *text = malloc(8U * 1024U * 1024U + 1); assert(text);
    memset(text, 'x', 8U * 1024U * 1024U); text[8U * 1024U * 1024U] = 0;
    struct edit_search_source source = {"/fixture-only-many-matches.txt", text, 8U * 1024U * 1024U};
    struct edit_filesearch_options o = options("x"); o.sources = &source; o.nsources = 1;
    for (int i = 0; i < 20; i++) {
        double before = now(); struct edit_filesearch *s = start(o); pid_t pid = s->pid;
        (void)edit_filesearch_read(s); edit_filesearch_free(s);
        CHECK(now() - before < 0.5); CHECK(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD);
    }
    /* Old queued results must not appear in a newer search generation. */
    struct edit_filesearch *old = start(o);
    struct edit_filesearch_options newer = options("no-such-pattern"); newer.include = "a.txt";
    struct edit_filesearch *s = start(newer); edit_filesearch_free(old); finish(s); CHECK(!s->count && !*s->error); edit_filesearch_free(s);
    /* Even under a continuously writable worker, each read has a byte budget. */
    s = start(o); double deadline = now() + 1;
    while (!s->done && now() < deadline) {
        struct pollfd p = {s->fd, POLLIN, 0}; (void)poll(&p, 1, 5);
        double before = now(); (void)edit_filesearch_read(s); CHECK(now() - before < 0.2);
    }
    edit_filesearch_free(s); free(text);
    puts("PASS cancellation kill/wait, no zombies/stale generations, bounded nonblocking UI drains");
}
int main(void)
{
    if (getuid() != 0) { fprintf(stderr, "Run in the disposable Linux root as root\n"); return 2; }
    fixtures(); test_tree(); test_replacement(); test_sources(); test_folder_overlays(); test_errors(); test_limits_and_zero_width(); test_protocol(); test_cancellation();
    /* Cleanup only the exact mkdtemp fixture, using nftw would need extra test
     * scaffolding; the disposable root also removes /tmp when it exits. */
    if (failures) fprintf(stderr, "%d failures\n", failures);
    else puts("PASS all asynchronous file-search backend tests");
    return failures ? 1 : 0;
}
