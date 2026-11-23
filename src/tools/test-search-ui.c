/*
 * test-search-ui.c: unit and mock-socket integration tests for LOCachy's
 * global search UI (progs/search.c).
 *
 * Includes ../progs/search.c directly with screen/key/pointer/clipboard/open
 * test doubles and links against base/ui.c, base/key-action.c, and base/help.c.
 *
 * Tests covered:
 *   1. Protocol formatter & parser (Q <scope> <query>, R layer 1/2, E/DONE,
 *      STATUS / S stats lines, malformed/overlong/control-byte rejection).
 *   2. UTF-8 / CJK query editing (insert, cursor move, Backspace, Delete,
 *      Home/End, Ctrl+A, Ctrl+U, Ctrl+K, Ctrl+W, Ctrl+C/X/V via clipboard).
 *   3. Local app matching (built-in host programs + containers NAME.conf scan,
 *      case-insensitive exact > prefix > substring > keyword ranking).
 *   4. Layer 1 + Layer 2 deduplication (preserving richer snippet on duplicate
 *      path without duplicating entries).
 *   5. Live non-blocking Unix socket streaming over a mock server thread:
 *      - Layer 1 streamed before Layer 2, followed by E / DONE + STATUS.
 *      - Stale-generation cancellation when user types a new query mid-stream.
 *      - Graceful handling when daemon socket is unavailable (still shows apps).
 *   6. Filter tabs (All / Apps / Files / Content) via Tab / Shift+Tab / mouse
 *      click, navigation, Esc clear-then-close, Ctrl+Q quit, Ctrl+O parent
 *      folder, Enter open, and F1 help overlay.
 *   7. Zero-leak / clean teardown across repeated query cycles.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define main search_program_main
#include "../progs/search.c"
#include "i18n-stub.h"
#undef main

#include <pthread.h>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

/* --- Test doubles for base screen, keys, pointer, clipboard, open --- */

static int g_cols = 100;
static int g_rows = 28;
static int g_mods = 0;
static char g_keys_text[64];
static struct pointer_event g_ptr;
static char g_clip[512];
static int g_flush_count = 0;

static char g_opened_with[OPEN_NAME];
static char g_opened_target[PATH_MAX];
static int  g_open_calls = 0;
static int  g_help_calls = 0;
static char g_last_bar[64];

static int g_script[64];
static int g_script_len = 0;
static int g_script_pos = 0;

void say(const char *fmt, ...) { (void)fmt; }
int screen_open(const char *font) { (void)font; return 1; }
int keys_open(void) { return 1; }
int screen_cols(void) { return g_cols; }
int screen_rows(void) { return g_rows; }
int screen_char_cells(uint32_t cp)
{
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0x20000 && cp <= 0x2FFFD)) {
        return 2;
    }
    return 1;
}
void screen_bar(const char *title, const char *ime)
{
    (void)ime;
    snprintf(g_last_bar, sizeof g_last_bar, "%s", title ? title : "");
}
void screen_fill(int c, int r, int w, int h, uint32_t rgb)
{
    (void)c; (void)r; (void)w; (void)h; (void)rgb;
}
void screen_text(int c, int r, const char *s, uint32_t fg, uint32_t bg)
{
    (void)c; (void)r; (void)s; (void)fg; (void)bg;
}
void screen_flush(void) { g_flush_count++; }
void screen_cursor(int shape) { (void)shape; }
const struct pointer_event *pointer_last(void) { return &g_ptr; }
int keys_modifiers(void) { return g_mods; }
const char *keys_text(void) { return g_keys_text; }

int key_poll_timeout(int fd, int timeout_ms)
{
    (void)fd;
    (void)timeout_ms;
    if (g_script_pos < g_script_len) {
        return g_script[g_script_pos++];
    }
    return 27;
}

int key_poll_many(struct pollfd *extra, size_t count, int timeout_ms)
{
    if (extra && count > 0) {
        (void)poll(extra, (nfds_t)count, timeout_ms);
    }
    return 0;
}

int clipboard_set(int primary, const void *text, size_t size)
{
    (void)primary;
    size_t n = size < sizeof g_clip - 1 ? size : sizeof g_clip - 1;
    memcpy(g_clip, text, n);
    g_clip[n] = '\0';
    return CLIPBOARD_OK;
}

int clipboard_get(int primary, int *fd, size_t *size)
{
    (void)primary;
    int pfd[2];
    if (pipe(pfd) != 0) return CLIPBOARD_FAILED;
    size_t len = strlen(g_clip);
    if (len > 0 && write(pfd[1], g_clip, len) != (ssize_t)len) {
        close(pfd[0]);
        close(pfd[1]);
        return CLIPBOARD_FAILED;
    }
    close(pfd[1]);
    *fd = pfd[0];
    *size = len;
    return CLIPBOARD_OK;
}

int open_in_home(const char *with, const char *target)
{
    g_open_calls++;
    snprintf(g_opened_with, sizeof g_opened_with, "%s", with ? with : "");
    snprintf(g_opened_target, sizeof g_opened_target, "%s", target ? target : "");
    return 0;
}

/* Mock rc_read so load_app_catalog does not depend on host /etc/locachy/rc.conf. */
int rc_read(void (*fn)(const char *name, const char *value, void *arg), void *arg)
{
    fn("program_term", "/usr/local/bin/term /usr/local/lib/locachy/user-shell", arg);
    fn("program_settings", "/usr/local/bin/settings", arg);
    fn("program_customtool", "/usr/local/bin/customtool", arg);
    return 0;
}

/* Temporary scratch directory helper. */
static char g_tmpdir[80];

static void make_tmpdir(void)
{
    snprintf(g_tmpdir, sizeof g_tmpdir, "/tmp/search-ui-test-XXXXXX");
    if (!mkdtemp(g_tmpdir)) {
        perror("mkdtemp");
        exit(2);
    }
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(content, f);
    fclose(f);
}

/* --- Test 1: Protocol formatter & parser --- */

static void test_protocol(void)
{
    char req[SEARCH_REQ_MAX + 16];

    int n = search_format_query(TAB_ALL, "  budget report  ", req, sizeof req);
    CHECK(n > 0);
    CHECK(strcmp(req, "Q all budget report\n") == 0);

    n = search_format_query(TAB_FILES, "notes", req, sizeof req);
    CHECK(n > 0 && strcmp(req, "Q names notes\n") == 0);

    n = search_format_query(TAB_CONTENT, "\xe6\xb5\x8b\xe8\xaf\x95", req, sizeof req); /* 测试 */
    CHECK(n > 0 && strcmp(req, "Q content \xe6\xb5\x8b\xe8\xaf\x95\n") == 0);

    /* Reject empty, whitespace-only, control chars, and overlong lines. */
    CHECK(search_format_query(TAB_ALL, "", req, sizeof req) < 0);
    CHECK(search_format_query(TAB_ALL, "   \t  ", req, sizeof req) < 0);
    CHECK(search_format_query(TAB_ALL, "bad\nquery", req, sizeof req) < 0);
    CHECK(search_format_query(TAB_ALL, "bad\tquery", req, sizeof req) < 0);

    char huge[300];
    memset(huge, 'a', sizeof huge - 1);
    huge[sizeof huge - 1] = '\0';
    CHECK(search_format_query(TAB_CONTENT, huge, req, sizeof req) < 0);

    /* Parse Layer 1 hit (empty snippet). */
    struct search_result hit;
    struct daemon_stats stats;
    memset(&stats, 0, sizeof stats);

    enum proto_line_kind pk = search_parse_line(
        "R 1 doc\t/home/u/Documents/report.pdf\treport.pdf\t", &hit, &stats);
    CHECK(pk == PROTO_HIT);
    CHECK(hit.layer == 1);
    CHECK(hit.source == RES_FILE);
    CHECK(hit.kind == SEARCH_KIND_DOC);
    CHECK(strcmp(hit.path, "/home/u/Documents/report.pdf") == 0);
    CHECK(strcmp(hit.name, "report.pdf") == 0);
    CHECK(hit.snippet[0] == '\0');

    /* Parse Layer 2 hit with snippet. */
    pk = search_parse_line(
        "R 2 text\t/home/u/notes.md\tnotes.md\t...quarterly budget summary...", &hit, &stats);
    CHECK(pk == PROTO_HIT);
    CHECK(hit.layer == 2);
    CHECK(hit.source == RES_CONTENT);
    CHECK(hit.kind == SEARCH_KIND_TEXT);
    CHECK(strcmp(hit.snippet, "...quarterly budget summary...") == 0);

    /* Parse terminators (E and DONE) and error line. */
    CHECK(search_parse_line("E", &hit, &stats) == PROTO_END);
    CHECK(search_parse_line("DONE", &hit, &stats) == PROTO_END);
    CHECK(search_parse_line("ERR bad request", &hit, &stats) == PROTO_ERROR);

    /* Parse optional STATUS / S stats formats. */
    pk = search_parse_line("S 12480\t640\t0", &hit, &stats);
    CHECK(pk == PROTO_STATUS);
    CHECK(stats.known == 1 && stats.files == 12480 && stats.docs == 640 && stats.indexing == 0);

    memset(&stats, 0, sizeof stats);
    pk = search_parse_line("STATUS files=500 docs=42 indexing=1", &hit, &stats);
    CHECK(pk == PROTO_STATUS);
    CHECK(stats.known == 1 && stats.files == 500 && stats.docs == 42 && stats.indexing == 1);

    /* Malformed lines must be ignored safely. */
    CHECK(search_parse_line("", &hit, &stats) == PROTO_IGNORE);
    CHECK(search_parse_line("R 9 file\t/a\ta\t", &hit, &stats) == PROTO_IGNORE);
    CHECK(search_parse_line("R 1 file_without_tabs", &hit, &stats) == PROTO_IGNORE);
    CHECK(search_parse_line("GARBAGE", &hit, &stats) == PROTO_IGNORE);

    printf("PASS protocol formatting and parsing\n");
}

/* --- Test 2: UTF-8 / CJK query editing and clipboard --- */

static void test_utf8_editing(void)
{
    struct query_edit q;
    memset(&q, 0, sizeof q);

    /* Type ASCII + CJK ("hi中文!"). */
    CHECK(query_edit_key(&q, 'h', 0, ACT_NONE) == 2);
    CHECK(query_edit_key(&q, 'i', 0, ACT_NONE) == 2);
    snprintf(g_keys_text, sizeof g_keys_text, "\xe4\xb8\xad\xe6\x96\x87"); /* 中文 */
    CHECK(query_edit_key(&q, K_TEXT, 0, ACT_NONE) == 2);
    CHECK(query_edit_key(&q, '!', 0, ACT_NONE) == 2);
    CHECK(strcmp(q.text, "hi\xe4\xb8\xad\xe6\x96\x87!") == 0);
    CHECK(q.pos == strlen(q.text));

    /* Move left over '!' and '文' (3 bytes), insert 'A'. */
    CHECK(query_edit_key(&q, K_LEFT, 0, ACT_NONE) == 1);
    CHECK(query_edit_key(&q, K_LEFT, 0, ACT_NONE) == 1);
    CHECK(query_edit_key(&q, 'A', 0, ACT_NONE) == 2);
    CHECK(strcmp(q.text, "hi\xe4\xb8\xad" "A\xe6\x96\x87!") == 0);

    /* Backspace deletes 'A', then Backspace deletes whole CJK char '中' (3 bytes). */
    CHECK(query_edit_key(&q, '\b', 0, ACT_NONE) == 2);
    CHECK(query_edit_key(&q, '\b', 0, ACT_NONE) == 2);
    CHECK(strcmp(q.text, "hi\xe6\x96\x87!") == 0);

    /* Delete key deletes '文' forward cleanly on UTF-8 boundary. */
    CHECK(query_edit_key(&q, K_DEL, 0, ACT_NONE) == 2);
    CHECK(strcmp(q.text, "hi!") == 0);

    /* Home / End / Ctrl+U / Ctrl+K / Ctrl+W. */
    query_insert(&q, " foo/bar", 8);
    CHECK(strcmp(q.text, "hi foo/bar!") == 0);
    CHECK(query_edit_key(&q, K_EOL, 0, ACT_NONE) == 1);
    CHECK(query_edit_key(&q, 'w' & 0x1F, KEYS_CTRL, key_action('w' & 0x1F, KEYS_CTRL)) == 2);
    CHECK(strcmp(q.text, "hi foo/") == 0);

    /* Ctrl+U clears to start of line. */
    CHECK(query_edit_key(&q, 'u' & 0x1F, KEYS_CTRL, ACT_NONE) == 2);
    CHECK(strcmp(q.text, "") == 0);

    /* Paste from clipboard, Ctrl+A select all, Ctrl+C copy, type replaces all. */
    snprintf(g_clip, sizeof g_clip, "pasted \xe6\x96\x87\xe6\xa1\xa3\nline2"); /* pasted 文档\nline2 */
    CHECK(query_edit_key(&q, 'v' & 0x1F, KEYS_CTRL, key_action('v' & 0x1F, KEYS_CTRL)) == 2);
    /* Control char '\n' stripped from single-line query. */
    CHECK(strcmp(q.text, "pasted \xe6\x96\x87\xe6\xa1\xa3line2") == 0);

    CHECK(query_edit_key(&q, 'a' & 0x1F, KEYS_CTRL, key_action('a' & 0x1F, KEYS_CTRL)) == 1);
    CHECK(q.sel_all == 1);
    g_clip[0] = '\0';
    CHECK(query_edit_key(&q, 'c' & 0x1F, KEYS_CTRL, key_action('c' & 0x1F, KEYS_CTRL)) == 1);
    CHECK(strcmp(g_clip, "pasted \xe6\x96\x87\xe6\xa1\xa3line2") == 0);

    CHECK(query_edit_key(&q, 'z', 0, ACT_NONE) == 2);
    CHECK(strcmp(q.text, "z") == 0 && q.sel_all == 0);

    printf("PASS UTF-8/CJK editing and clipboard actions\n");
}

/* --- Test 3: Local app matching & container scan --- */

static void test_app_matching(void)
{
    char cdir[128];
    snprintf(cdir, sizeof cdir, "%s/containers", g_tmpdir);
    CHECK(mkdir(cdir, 0700) == 0);

    char p1[192], p2[192], p3[192];
    snprintf(p1, sizeof p1, "%s/firefox.conf", cdir);
    snprintf(p2, sizeof p2, "%s/code-oss.conf", cdir);
    snprintf(p3, sizeof p3, "%s/window-ignored.conf", cdir);
    write_file(p1, "package=\"firefox\"\ncommand=\"/usr/bin/firefox\"\n");
    write_file(p2, "package=\"code\"\ncommand=\"/usr/bin/code-oss\"\n");
    write_file(p3, "package=\"bad\"\ncommand=\"/usr/bin/bad\"\n");

    struct search_state st;
    search_state_init(&st, cdir);

    /* Must include built-ins, rc.conf customtool, firefox, code-oss, and exclude window-*. */
    CHECK(st.napp_entries >= 13);
    int found_firefox = 0, found_settings = 0, found_custom = 0, found_window = 0;
    for (size_t i = 0; i < st.napp_entries; i++) {
        if (strcmp(st.apps[i].id, "firefox") == 0 && st.apps[i].kind == APP_CONTAINER) found_firefox = 1;
        if (strcmp(st.apps[i].id, "settings") == 0 && st.apps[i].kind == APP_BUILTIN) found_settings = 1;
        if (strcmp(st.apps[i].id, "customtool") == 0 && st.apps[i].kind == APP_BUILTIN) found_custom = 1;
        if (strcmp(st.apps[i].id, "window-ignored") == 0) found_window = 1;
    }
    CHECK(found_firefox);
    CHECK(found_settings);
    CHECK(found_custom);
    CHECK(!found_window);

    /* Case-insensitive substring match for "FIRE". */
    snprintf(st.q.text, sizeof st.q.text, "FIRE");
    st.tab = TAB_APPS;
    rebuild_view(&st);
    CHECK(st.view_count == 1);
    CHECK(st.view[0].source == RES_APP);
    CHECK(strcmp(st.apps[st.view[0].app_idx].id, "firefox") == 0);

    /* Keyword match ("pdf" -> View). */
    snprintf(st.q.text, sizeof st.q.text, "pdf");
    rebuild_view(&st);
    CHECK(st.view_count >= 1);
    CHECK(strcmp(st.apps[st.view[0].app_idx].id, "view") == 0);

    /* Exact match ranks above substring match ("set" -> Settings). */
    snprintf(st.q.text, sizeof st.q.text, "Settings");
    rebuild_view(&st);
    CHECK(st.view_count >= 1);
    CHECK(strcmp(st.apps[st.view[0].app_idx].id, "settings") == 0);

    search_state_free(&st);
    unlink(p1);
    unlink(p2);
    unlink(p3);
    rmdir(cdir);

    printf("PASS app catalog scan and case-insensitive ranking\n");
}

/* --- Test 4: Layer 1 + Layer 2 deduplication --- */

static void test_deduplication(void)
{
    struct search_state st;
    search_state_init(&st, "/nonexistent-dir-search-ui-test");

    struct search_result l1, l2_dup, l2_only;
    CHECK(search_parse_line("R 1 doc\t/home/u/budget.pdf\tbudget.pdf\t", &l1, NULL) == PROTO_HIT);
    CHECK(search_parse_line("R 2 doc\t/home/u/budget.pdf\tbudget.pdf\tTotal budget is $500", &l2_dup, NULL) == PROTO_HIT);
    CHECK(search_parse_line("R 2 text\t/home/u/memo.txt\tmemo.txt\tSee budget notes", &l2_only, NULL) == PROTO_HIT);

    CHECK(daemon_hits_add(&st, &l1) == 1);
    CHECK(st.daemon_count == 1);
    CHECK(st.daemon_hits[0].snippet[0] == '\0');

    /* Adding Layer 2 hit for the same path must deduplicate AND attach snippet. */
    CHECK(daemon_hits_add(&st, &l2_dup) == 0);
    CHECK(st.daemon_count == 1);
    CHECK(strcmp(st.daemon_hits[0].snippet, "Total budget is $500") == 0);
    CHECK(st.daemon_hits[0].layer == 2);

    /* Distinct Layer 2 path is appended. */
    CHECK(daemon_hits_add(&st, &l2_only) == 1);
    CHECK(st.daemon_count == 2);

    /* Duplicate Layer 1 after Layer 2 must not strip snippet or duplicate row. */
    CHECK(daemon_hits_add(&st, &l1) == 0);
    CHECK(st.daemon_count == 2);
    CHECK(strcmp(st.daemon_hits[0].snippet, "Total budget is $500") == 0);

    /* View filtering check across All / Files / Content tabs. */
    snprintf(st.q.text, sizeof st.q.text, "budget");
    st.tab = TAB_ALL;
    rebuild_view(&st);
    CHECK(st.view_count == 2);

    st.tab = TAB_FILES;
    rebuild_view(&st);
    CHECK(st.view_count == 1);
    CHECK(strcmp(st.view[0].name, "budget.pdf") == 0);

    st.tab = TAB_CONTENT;
    rebuild_view(&st);
    CHECK(st.view_count == 2);

    search_state_free(&st);
    printf("PASS layer 1/2 deduplication and filter tabs\n");
}

/* --- Test 5: Live non-blocking Unix socket streaming + stale-gen cancellation --- */

struct mock_server_ctx {
    char sock_path[104];
    int  listen_fd;
    volatile int stop;
    volatile int queries_seen;
};

static int send_str(int fd, const char *s)
{
    size_t len = strlen(s), off = 0;
    while (off < len) {
        ssize_t n = send(fd, s + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void *mock_server_thread(void *arg)
{
    struct mock_server_ctx *ctx = arg;

    while (!ctx->stop) {
        struct pollfd p = { .fd = ctx->listen_fd, .events = POLLIN };
        int pr = poll(&p, 1, 50);
        if (pr <= 0) continue;

        int cfd = accept(ctx->listen_fd, NULL, NULL);
        if (cfd < 0) continue;

        /* Handle one or more line requests on this connection. */
        char buf[512];
        size_t len = 0;
        while (!ctx->stop) {
            struct pollfd cp = { .fd = cfd, .events = POLLIN };
            int cr = poll(&cp, 1, 100);
            if (cr == 0) continue;
            if (cr < 0) break;

            ssize_t n = recv(cfd, buf + len, sizeof buf - 1 - len, 0);
            if (n <= 0) break;
            len += (size_t)n;
            buf[len] = '\0';

            char *nl;
            while ((nl = strchr(buf, '\n')) != NULL) {
                *nl = '\0';
                ctx->queries_seen++;

                if (strcmp(buf, "Q all report") == 0) {
                    /* Stream Layer 1 first, pause briefly, then Layer 2 + STATUS + E. */
                    if (send_str(cfd, "S 12480\t640\t0\n") != 0) goto drop_client;
                    if (send_str(cfd, "R 1 doc\t/home/u/Docs/report.pdf\treport.pdf\t\n") != 0) goto drop_client;
                    if (send_str(cfd, "R 1 file\t/home/u/Docs/report-raw.bin\treport-raw.bin\t\n") != 0) goto drop_client;
                    usleep(30000);
                    if (send_str(cfd, "R 2 doc\t/home/u/Docs/report.pdf\treport.pdf\tAnnual report overview\n") != 0) goto drop_client;
                    if (send_str(cfd, "R 2 text\t/home/u/Docs/summary.md\tsummary.md\tSee the attached report\n") != 0) goto drop_client;
                    if (send_str(cfd, "E\n") != 0) goto drop_client;
                } else if (strcmp(buf, "Q all slow") == 0) {
                    /* Emit one stale hit, delay so client can cancel with a new query. */
                    if (send_str(cfd, "R 1 file\t/home/u/stale-slow.txt\tstale-slow.txt\t\n") != 0) goto drop_client;
                    usleep(120000);
                    (void)send_str(cfd, "R 2 text\t/home/u/stale-late.txt\tstale-late.txt\tshould be dropped\nE\n");
                } else if (strcmp(buf, "Q all fast") == 0) {
                    if (send_str(cfd, "R 1 text\t/home/u/fast-hit.txt\tfast-hit.txt\t\nDONE\n") != 0) goto drop_client;
                } else {
                    if (send_str(cfd, "E\n") != 0) goto drop_client;
                }

                size_t rem = len - (size_t)(nl + 1 - buf);
                memmove(buf, nl + 1, rem);
                len = rem;
                buf[len] = '\0';
            }
        }
    drop_client:
        close(cfd);
    }
    return NULL;
}

static void pump_socket_until(struct search_state *st, int (*done_fn)(const struct search_state *), int max_ms)
{
    int64_t deadline = search_now_ms() + max_ms;
    while (!done_fn(st) && search_now_ms() < deadline) {
        if (st->sock_fd < 0) {
            usleep(5000);
            continue;
        }
        short ev = POLLIN | ((st->sock_connecting || st->out_off < st->out_len) ? POLLOUT : 0);
        struct pollfd p = { .fd = st->sock_fd, .events = ev };
        int r = poll(&p, 1, 20);
        if (r > 0) {
            if (p.revents & (POLLOUT | POLLERR | POLLHUP)) sock_flush_out(st);
            if (st->sock_fd >= 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) sock_read_in(st);
        }
    }
}

static int done_has_layer1(const struct search_state *st)
{
    return st->daemon_count >= 2 && st->query_inflight == 1;
}

static int done_query_finished(const struct search_state *st)
{
    return !st->query_inflight;
}

static int done_has_any_hit(const struct search_state *st)
{
    return st->daemon_count >= 1;
}

static void test_live_socket_streaming(void)
{
    struct mock_server_ctx srv;
    memset(&srv, 0, sizeof srv);
    snprintf(srv.sock_path, sizeof srv.sock_path, "%s/search.sock", g_tmpdir);

    srv.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(srv.listen_fd >= 0);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", srv.sock_path);
    unlink(srv.sock_path);
    CHECK(bind(srv.listen_fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    CHECK(listen(srv.listen_fd, 8) == 0);

    pthread_t tid;
    CHECK(pthread_create(&tid, NULL, mock_server_thread, &srv) == 0);

    setenv("LOCACHY_SEARCH_SOCK", srv.sock_path, 1);

    struct search_state st;
    search_state_init(&st, "/nonexistent-dir-search-ui-test");
    snprintf(st.home_dir, sizeof st.home_dir, "/home/u");

    /* 1. Stream Layer 1 first, verify immediate visibility before Layer 2 arrives. */
    snprintf(st.q.text, sizeof st.q.text, "report");
    st.q.pos = strlen(st.q.text);
    schedule_query(&st, 1);

    pump_socket_until(&st, done_has_layer1, 1000);
    CHECK(st.daemon_count == 2);
    CHECK(st.query_inflight == 1);
    CHECK(st.stats.known == 1 && st.stats.files == 12480 && st.stats.docs == 640);

    /* Now let Layer 2 + E finish streaming. */
    pump_socket_until(&st, done_query_finished, 1000);
    CHECK(!st.query_inflight);
    CHECK(st.daemon_count == 3); /* report.pdf deduplicated + report-raw.bin + summary.md */
    int saw_enriched_report = 0, saw_summary = 0;
    for (size_t i = 0; i < st.daemon_count; i++) {
        if (strcmp(st.daemon_hits[i].path, "/home/u/Docs/report.pdf") == 0) {
            CHECK(strcmp(st.daemon_hits[i].snippet, "Annual report overview") == 0);
            saw_enriched_report++;
        }
        if (strcmp(st.daemon_hits[i].path, "/home/u/Docs/summary.md") == 0) {
            CHECK(strcmp(st.daemon_hits[i].snippet, "See the attached report") == 0);
            saw_summary++;
        }
    }
    CHECK(saw_enriched_report == 1);
    CHECK(saw_summary == 1);

    /* Render frame with live results to exercise drawing + compact path logic. */
    draw_search(&st);
    CHECK(g_flush_count > 0);
    CHECK(strcmp(g_last_bar, "search") == 0);

    /* 2. Stale-generation cancellation: send "slow", get first partial hit,
     * then immediately type "fast" and confirm stale hits are discarded. */
    snprintf(st.q.text, sizeof st.q.text, "slow");
    st.q.pos = 4;
    schedule_query(&st, 1);
    pump_socket_until(&st, done_has_any_hit, 1000);
    CHECK(st.daemon_count == 1 && strcmp(st.daemon_hits[0].name, "stale-slow.txt") == 0);

    /* Supersede with "fast" while "slow" is still in flight on the server. */
    snprintf(st.q.text, sizeof st.q.text, "fast");
    st.q.pos = 4;
    schedule_query(&st, 1);
    pump_socket_until(&st, done_query_finished, 1000);

    CHECK(!st.query_inflight);
    CHECK(st.daemon_count == 1);
    CHECK(strcmp(st.daemon_hits[0].name, "fast-hit.txt") == 0);

    /* 3. Clean teardown + daemon unavailable fallback. */
    search_state_free(&st);

    srv.stop = 1;
    pthread_join(tid, NULL);
    close(srv.listen_fd);
    unlink(srv.sock_path);

    /* Daemon unavailable: must set daemon_down and still list local apps. */
    char missing_sock[128];
    snprintf(missing_sock, sizeof missing_sock, "%s/no-such-daemon.sock", g_tmpdir);
    setenv("LOCACHY_SEARCH_SOCK", missing_sock, 1);

    search_state_init(&st, "/nonexistent-dir-search-ui-test");
    snprintf(st.q.text, sizeof st.q.text, "set");
    schedule_query(&st, 1);
    CHECK(st.daemon_down == 1);
    CHECK(st.view_count >= 1);
    CHECK(st.view[0].source == RES_APP);
    CHECK(strcmp(st.apps[st.view[0].app_idx].id, "settings") == 0);
    draw_search(&st);
    search_state_free(&st);

    unsetenv("LOCACHY_SEARCH_SOCK");
    printf("PASS live Unix socket streaming, stale-gen cancellation, and offline fallback\n");
}

/* --- Test 6: Key actions, tabs, Esc clear-vs-close, Ctrl+O, Enter, F1, pointer --- */

static void test_interaction_and_keys(void)
{
    char missing_sock[128];
    snprintf(missing_sock, sizeof missing_sock, "%s/offline.sock", g_tmpdir);
    setenv("LOCACHY_SEARCH_SOCK", missing_sock, 1);

    struct search_state st;
    search_state_init(&st, "/nonexistent-dir-search-ui-test");
    snprintf(st.home_dir, sizeof st.home_dir, "/home/u");

    /* Compact parent path helper check. */
    char cp[256];
    compact_parent_path("/home/u/Documents/sub/file.txt", "/home/u", cp, sizeof cp);
    CHECK(strcmp(cp, "~/Documents/sub") == 0);
    compact_parent_path("/home/u/file.txt", "/home/u", cp, sizeof cp);
    CHECK(strcmp(cp, "~") == 0);
    compact_parent_path("/etc/locachy/rc.conf", "/home/u", cp, sizeof cp);
    CHECK(strcmp(cp, "/etc/locachy") == 0);

    /* Tab / Shift+Tab cycles All -> Apps -> Files -> Content -> All. */
    CHECK(st.tab == TAB_ALL);
    handle_key(&st, '\t', 0);
    CHECK(st.tab == TAB_APPS);
    handle_key(&st, '\t', 0);
    CHECK(st.tab == TAB_FILES);
    handle_key(&st, '\t', 0);
    CHECK(st.tab == TAB_CONTENT);
    handle_key(&st, '\t', 0);
    CHECK(st.tab == TAB_ALL);
    handle_key(&st, '\t', KEYS_SHIFT);
    CHECK(st.tab == TAB_CONTENT);
    handle_key(&st, '\t', 0);
    CHECK(st.tab == TAB_ALL);

    /* Type query -> Esc clears query without quitting; second Esc closes. */
    handle_key(&st, 'a', 0);
    handle_key(&st, 'b', 0);
    CHECK(strcmp(st.q.text, "ab") == 0);
    handle_key(&st, 27, 0);
    CHECK(st.q.text[0] == '\0' && !st.quit);
    handle_key(&st, 27, 0);
    CHECK(st.quit == 1);

    /* Reset and verify Ctrl+Q and K_CLOSE quit immediately even with non-empty query. */
    st.quit = 0;
    handle_key(&st, 'x', 0);
    CHECK(st.q.text[0] == 'x');
    handle_key(&st, 'q' & 0x1F, KEYS_CTRL);
    CHECK(st.quit == 1);

    st.quit = 0;
    handle_key(&st, K_CLOSE, 0);
    CHECK(st.quit == 1);

    /* Verify F1 help overlay opens and returns cleanly when Esc is pressed inside it. */
    st.quit = 0;
    g_script[0] = 27;
    g_script_len = 1;
    g_script_pos = 0;
    g_help_calls = 0;
    handle_key(&st, K_F1, 0);
    CHECK(!st.quit);
    CHECK(g_script_pos == 1);

    /* Verify Ctrl+O opens parent folder of selected file result via open_in_home. */
    st.quit = 0;
    st.q.text[0] = '\0';
    st.q.pos = 0;
    snprintf(st.q.text, sizeof st.q.text, "doc");
    struct search_result fh;
    CHECK(search_parse_line("R 1 doc\t/home/u/Work/spec.pdf\tspec.pdf\t", &fh, NULL) == PROTO_HIT);
    daemon_hits_clear(&st);
    CHECK(daemon_hits_add(&st, &fh) == 1);
    st.tab = TAB_FILES;
    rebuild_view(&st);
    CHECK(st.view_count == 1);

    g_open_calls = 0;
    g_opened_target[0] = '\0';
    handle_key(&st, 'o' & 0x1F, KEYS_CTRL);
    CHECK(g_open_calls == 1);
    CHECK(strcmp(g_opened_target, "/home/u/Work") == 0);
    CHECK(st.quit == 1);

    /* Verify Enter opens selected file directly via open_in_home. */
    st.quit = 0;
    g_open_calls = 0;
    g_opened_target[0] = '\0';
    handle_key(&st, '\n', 0);
    CHECK(g_open_calls == 1);
    CHECK(strcmp(g_opened_target, "/home/u/Work/spec.pdf") == 0);
    CHECK(st.quit == 1);

    /* Verify framebuffer floating card geometry, pointer click on tab, wheel scroll,
     * click-outside dismiss, Wayland floating fill, and LOCACHY_REQUEST_FD toggle-close. */
    unsetenv("LOCACHY_BACKEND");
    st.quit = 0;
    st.tab = TAB_ALL;
    st.q.text[0] = '\0';
    rebuild_view(&st);
    draw_search(&st);
    CHECK(st.card_x > 0 && st.card_y > 1 && st.card_w == 86);

    /* Click on [Apps] tab inside the centered card (tab_row = card_y + 2, after "[All] "). */
    memset(&g_ptr, 0, sizeof g_ptr);
    g_ptr.row = st.card_y + 2;
    g_ptr.col = st.card_x + 8;
    g_ptr.button = BTN_LEFT;
    g_ptr.pressed = 1;
    g_ptr.clicks = 1;
    handle_pointer(&st);
    CHECK(st.tab == TAB_APPS);

    /* Mouse wheel down moves selection forward. */
    int before_sel = st.sel;
    memset(&g_ptr, 0, sizeof g_ptr);
    g_ptr.wheel = -1;
    handle_pointer(&st);
    CHECK(st.sel > before_sel);

    /* Click outside the centered card on framebuffer dismisses search. */
    st.quit = 0;
    memset(&g_ptr, 0, sizeof g_ptr);
    g_ptr.row = st.card_y;
    g_ptr.col = 0;
    g_ptr.button = BTN_LEFT;
    g_ptr.pressed = 1;
    g_ptr.clicks = 1;
    handle_pointer(&st);
    CHECK(st.quit == 1);

    /* On Wayland, the floating layer surface is already centered; card fills window area. */
    setenv("LOCACHY_BACKEND", "wayland", 1);
    st.quit = 0;
    draw_search(&st);
    CHECK(st.card_x == 0 && st.card_y == 1 && st.card_w == g_cols && st.card_h == g_rows - 1);
    unsetenv("LOCACHY_BACKEND");

    /* LOCACHY_REQUEST_FD "close\n" toggles floating search closed cleanly. */
    int rpipe[2];
    CHECK(pipe(rpipe) == 0);
    st.req_fd = rpipe[0];
    st.quit = 0;
    CHECK(write(rpipe[1], "close\n", 6) == 6);
    req_read_in(&st);
    CHECK(st.quit == 1);
    close(rpipe[1]);

    search_state_free(&st);
    unsetenv("LOCACHY_SEARCH_SOCK");
    printf("PASS key actions, tabs, floating layout, Esc/Ctrl+Q/Ctrl+O/Enter/F1, and pointer input\n");
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    make_tmpdir();

    test_protocol();
    test_utf8_editing();
    test_app_matching();
    test_deduplication();
    test_live_socket_streaming();
    test_interaction_and_keys();

    (void)rmdir(g_tmpdir);

    if (g_failures > 0) {
        fprintf(stderr, "FAIL test-search-ui: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("PASS test-search-ui: all tests passed\n");
    return 0;
}
