/*
 * search (/usr/local/bin/search): LOCachy global search UI (tools/SEARCH.md).
 *
 * Runs both on the direct host framebuffer and as a managed Wayland window
 * (LOCACHY_BACKEND=wayland, via locachy-seat --window search). Combines instant
 * local matching of installed containers and host programs with non-blocking
 * two-layer queries streamed over locachy-searchd's Unix socket
 * (/run/locachy/search.sock):
 *   - Layer 1 (names): file names returned first and shown immediately.
 *   - Layer 2 (content): document and text body matches with snippets, merged
 *     in place (deduplicating paths already shown from layer 1 while keeping
 *     the richer snippet).
 *
 * Protocol with locachy-searchd (AF_UNIX SOCK_STREAM):
 *   Request:  Q <scope> <query>\n   (scope: names | content | all; <= 256 bytes)
 *   Response: zero or more lines of
 *               R <layer> <kind>\t<path>\t<name>\t<snippet>\n
 *             terminated by E\n (or DONE\n in legacy test fixtures).
 *   Optional status lines ("S <files>\t<docs>\t<indexing>" or "STATUS ...")
 *   are also accepted if a peer emits them.
 *
 * Keys (all standard actions via base/key-action.c):
 *   Typing / UTF-8 / IME   Edit the query
 *   Left, Right, Home, End Move cursor in query
 *   Backspace, Delete      Delete character
 *   Ctrl+A / Ctrl+U / Ctrl+K / Ctrl+W  Select all / clear before / after / word
 *   Ctrl+C / Ctrl+X / Ctrl+V           Copy / cut / paste query (clipboard)
 *   Up, Down, PgUp, PgDn   Move selection in results (or mouse wheel)
 *   Tab, Shift+Tab         Cycle category filter (All, Apps, Files, Content)
 *   Enter / double-click   Open selected app, file, or folder
 *   Ctrl+O                 Open selected result's parent folder in files
 *   Esc                    Clear non-empty query; close when query is empty
 *   Ctrl+Q, Alt+F4, Close  Close search
 *   F1                     Help overlay (base/help.c)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "base.h"
#include "clipboard.h"
#include "ime.h"
#include "wayland-backend.h"
#include "search-index.h"

#ifndef SEARCH_SOCK_PATH
#define SEARCH_SOCK_PATH "/run/locachy/search.sock"
#endif
#ifndef SEARCH_CONTAINERS_DIR
#define SEARCH_CONTAINERS_DIR CONTAINERS_CONF
#endif
#ifndef SEARCH_SEAT_BIN
#define SEARCH_SEAT_BIN "/usr/local/bin/locachy-seat"
#endif
#ifndef SEARCH_APP_BIN
#define SEARCH_APP_BIN "/usr/local/bin/locachy-app"
#endif
#ifndef SEARCH_FILES_BIN
#define SEARCH_FILES_BIN "/usr/local/bin/files"
#endif

/* Weak optional symbols so unit tests can link without full base/keys/clipboard. */
__attribute__((weak)) int keys_modifiers(void);
__attribute__((weak)) const struct pointer_event *pointer_last(void);
__attribute__((weak)) void screen_cursor(int shape);
__attribute__((weak)) void key_close_kept(void);
__attribute__((weak)) int clipboard_get(int primary, int *fd, size_t *size);
__attribute__((weak)) int clipboard_set(int primary, const void *text, size_t size);
__attribute__((weak)) const char *clipboard_error(int status);
__attribute__((weak)) int open_containers(char names[][OPEN_NAME], int max);
__attribute__((weak)) void open_label(const char *name, const char *type, char *label, size_t size);
__attribute__((weak)) int open_in_home(const char *with, const char *target);
__attribute__((weak)) int open_request_publish(const char *with, const char *target);
__attribute__((weak)) int open_type(const char *target, char *type, size_t size);
__attribute__((weak)) int open_rule(const char *type, char *name, size_t size);
__attribute__((weak)) int open_candidates(const char *type, char names[][OPEN_NAME], int max);
__attribute__((weak)) int open_program(const char *name, const char *target, char *spec, size_t size,
                                       char *argv[], int max);
__attribute__((weak)) int rc_read(void (*fn)(const char *name, const char *value, void *arg), void *arg);
__attribute__((weak)) int conf_read(const char *path,
                                    void (*fn)(const char *name, const char *value, void *arg), void *arg);
__attribute__((weak)) int base_app_action(const char *action, const char *name, char *why, size_t size);
__attribute__((weak)) int base_app_open(const char *name, const char *target, char *why, size_t size);
__attribute__((weak)) void base_open(char *const argv[], char *why, size_t size);
__attribute__((weak)) int base_call(char *const argv[]);

/* Theme colors matching LOCachy's palette tokens. */
#define FG           theme_rgb(THEME_FG, 0xD0D0D0)
#define BG           theme_rgb(THEME_BG, 0x101820)
#define ACCENT       theme_rgb(THEME_ACCENT, 0x80C0FF)
#define ACCENT_TEXT  theme_rgb(THEME_ACCENT_TEXT, 0x80C0FF)
#define ON_ACCENT    theme_rgb(THEME_ON_ACCENT, 0x101820)
#define SURFACE      theme_rgb(THEME_SURFACE, 0x1C2836)
#define CONTROL      theme_rgb(THEME_CONTROL, 0x26384C)
#define BORDER       theme_rgb(THEME_BORDER, 0x385068)
#define DIM          theme_rgb(THEME_DIM, 0x808890)
#define SELECTION    theme_rgb(THEME_SELECTION, 0x2C4A68)
#define ERROR        theme_rgb(THEME_ERROR, 0xFF9090)
#define SUCCESS      theme_rgb(THEME_SUCCESS, 0x80E0A0)

/* Timing and sizing constants. */
#define SEARCH_DEBOUNCE_MS      40
#define SEARCH_FAST_TYPE_MS     180
#define SEARCH_BLINK_MS         530
#define SEARCH_REQ_MAX          256
#define SEARCH_QUERY_BUF        240 /* fits "Q content " + query + "\n" inside 256 bytes */
#define SEARCH_MAX_APPS         128
#define SEARCH_MAX_DAEMON_HITS  SEARCH_MAX_RESULTS
#define SEARCH_MAX_VIEW_HITS    (SEARCH_MAX_APPS + SEARCH_MAX_DAEMON_HITS)
#define SEARCH_SOCK_BUF         16384

/* Pointer hit regions. */
#define HIT_INPUT       1
#define HIT_CLEAR       2
#define HIT_TAB_BASE    10      /* + enum search_tab */
#define HIT_ROW_BASE    1000    /* + visible item index in filtered view */

enum search_tab {
    TAB_ALL = 0,
    TAB_APPS,
    TAB_FILES,
    TAB_CONTENT,
    TAB_COUNT
};

enum result_source {
    RES_APP = 0,     /* container or built-in host program */
    RES_FILE = 1,    /* layer 1 daemon hit (file / directory name) */
    RES_CONTENT = 2  /* layer 2 daemon hit (document / text content with snippet) */
};

enum app_kind {
    APP_BUILTIN = 0,
    APP_CONTAINER = 1
};

struct app_entry {
    enum app_kind kind;
    char id[OPEN_NAME];       /* container name or host program key ("files", "settings", ...) */
    char label[128];          /* display name */
    char keywords[128];       /* extra search terms (command / section / package) */
    char exec_spec[256];      /* command line from rc.conf or builtin path */
    int  has_window;          /* supported by locachy-seat --window */
};

struct search_result {
    enum result_source source;
    int  layer;                         /* 0 = app, 1 = names, 2 = content */
    int  layers_mask;                   /* bit 1 = layer 1 (names), bit 2 = layer 2 (content) */
    int  kind;                          /* enum search_kind for files/content */
    int  app_idx;                       /* index in app catalog if source == RES_APP */
    int  score;                         /* local ranking score for apps */
    char path[SEARCH_PATH_MAX];         /* full path (empty for apps) */
    char name[SEARCH_NAME_MAX];         /* display name */
    char snippet[SEARCH_SNIPPET_MAX];   /* layer 2 excerpt, or subtitle for apps */
};

struct query_edit {
    char   text[SEARCH_QUERY_BUF];
    size_t pos;       /* byte offset of cursor */
    int    sel_all;   /* 1 when Ctrl+A selected the entire query */
};

struct daemon_stats {
    int     known;
    int64_t files;
    int64_t docs;
    int     indexing;
};

struct search_state {
    struct query_edit q;
    enum search_tab tab;

    /* Local app catalog (containers + host programs). */
    struct app_entry apps[SEARCH_MAX_APPS];
    size_t napp_entries;

    /* Daemon results for the latest active generation (deduplicated by path). */
    struct search_result *daemon_hits;
    size_t daemon_count;
    size_t daemon_cap;

    /* Merged + filtered view results currently displayed. */
    struct search_result *view;
    size_t view_count;
    size_t view_cap;

    int sel;          /* selected index in view */
    int top;          /* first visible index in view */

    /* Non-blocking socket state. */
    int      sock_fd;
    int      sock_connecting;
    int      daemon_down;
    int      query_inflight;
    uint64_t generation;          /* bumped on every query change */
    uint64_t inflight_gen;        /* generation of the current request on sock_fd */
    char     sent_query[SEARCH_QUERY_BUF];
    enum search_tab sent_tab;

    /* Write and read buffers for non-blocking socket I/O. */
    char   outbuf[SEARCH_REQ_MAX + 16];
    size_t out_off;
    size_t out_len;
    char   inbuf[SEARCH_SOCK_BUF];
    size_t in_len;
    int    in_discard; /* draining remainder of an overlong line until '\n' */

    /* Debounce + cursor blink timers (monotonic milliseconds). */
    int     pending_query;
    int64_t pending_at_ms;
    int64_t last_edit_ms;
    int64_t blink_ms;
    int     cursor_on;

    /* Optional index statistics from STATUS / S line. */
    struct daemon_stats stats;

    /* User home directory (for '~' path compaction). */
    char home_dir[PATH_MAX];

    /* Status / error banner and lifecycle flags. */
    char msg[256];
    int  msg_error;
    int  dirty;
    int  quit;
    int  launched; /* 1 when exiting after opening a result (status 2 for PID 1) */

    /* Last drawn floating card bounds (for click-outside dismiss on framebuffer). */
    int  card_x;
    int  card_y;
    int  card_w;
    int  card_h;

    /* Optional LOCACHY_REQUEST_FD from apps.c ("close\n" toggles floating overlay). */
    int  req_fd;
};

static const struct ui_help_line search_help_lines[] = {
    { "Type", N_("Search apps, file names, and document contents") },
    { "Up, Down, PgUp, PgDn", N_("Move selection in the results list") },
    { "Enter", N_("Open the selected app, file, or folder") },
    { "Ctrl+O", N_("Open the selected result's parent folder in files") },
    { "Tab, Shift+Tab", N_("Switch filter: All, Apps, Files, Content") },
    { "Left, Right, Home, End", N_("Move the cursor in the search box") },
    { "Ctrl+U, Ctrl+K, Ctrl+W", N_("Delete to start, to end, or previous word") },
    { "Ctrl+A, Ctrl+C, Ctrl+V", N_("Select all, copy, or paste in the search box") },
    { "Esc", N_("Clear the query, or close search when empty") },
    { "Ctrl+Q", N_("Close search") },
    { NULL, NULL },
};

/* --- Monotonic clock and UTF-8 / string helpers --- */

static int64_t search_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static size_t utf8_prev(const char *s, size_t pos)
{
    if (pos == 0) return 0;
    do {
        pos--;
    } while (pos > 0 && ((unsigned char)s[pos] & 0xC0u) == 0x80u);
    return pos;
}

static size_t utf8_next(const char *s, size_t len, size_t pos)
{
    if (pos >= len) return len;
    do {
        pos++;
    } while (pos < len && ((unsigned char)s[pos] & 0xC0u) == 0x80u);
    return pos;
}

static int str_contains_ci(const char *hay, const char *needle)
{
    if (!needle || !needle[0]) return 1;
    if (!hay || !hay[0]) return 0;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i]) {
            unsigned char a = (unsigned char)p[i];
            unsigned char b = (unsigned char)needle[i];
            if (a < 0x80u) a = (unsigned char)tolower(a);
            if (b < 0x80u) b = (unsigned char)tolower(b);
            if (a != b) break;
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

static int str_prefix_ci(const char *hay, const char *needle)
{
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    for (size_t i = 0; needle[i]; i++) {
        if (!hay[i]) return 0;
        unsigned char a = (unsigned char)hay[i];
        unsigned char b = (unsigned char)needle[i];
        if (a < 0x80u) a = (unsigned char)tolower(a);
        if (b < 0x80u) b = (unsigned char)tolower(b);
        if (a != b) return 0;
    }
    return 1;
}

static int str_equal_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    for (size_t i = 0;; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca < 0x80u) ca = (unsigned char)tolower(ca);
        if (cb < 0x80u) cb = (unsigned char)tolower(cb);
        if (ca != cb) return 0;
        if (ca == 0) return 1;
    }
}

static void trim_query_copy(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    size_t a = 0;
    while (a < len && (src[a] == ' ' || src[a] == '\t')) a++;
    size_t b = len;
    while (b > a && (src[b - 1] == ' ' || src[b - 1] == '\t')) b--;
    size_t n = b - a;
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src + a, n);
    dst[n] = '\0';
}

/* Strip control characters (including CR/LF/NUL/DEL) from displayed text. */
static void sanitize_display(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && w + 1 < dst_size; p++) {
        if (*p < 0x20u || *p == 0x7Fu) {
            if (w > 0 && dst[w - 1] != ' ') dst[w++] = ' ';
        } else {
            dst[w++] = (char)*p;
        }
    }
    while (w > 0 && dst[w - 1] == ' ') w--;
    dst[w] = '\0';
}

/* Format path with home directory abbreviated as '~'. */
static void compact_parent_path(const char *full_path, const char *home_dir,
                                char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!full_path || !full_path[0]) return;

    char dir[SEARCH_PATH_MAX];
    const char *slash = strrchr(full_path, '/');
    if (!slash) {
        snprintf(out, out_size, ".");
        return;
    }
    if (slash == full_path) {
        snprintf(out, out_size, "/");
        return;
    }
    size_t dlen = (size_t)(slash - full_path);
    if (dlen >= sizeof dir) dlen = sizeof dir - 1;
    memcpy(dir, full_path, dlen);
    dir[dlen] = '\0';

    if (home_dir && home_dir[0]) {
        size_t hlen = strlen(home_dir);
        while (hlen > 1 && home_dir[hlen - 1] == '/') hlen--;
        if (hlen > 1 && strncmp(dir, home_dir, hlen) == 0 &&
            (dir[hlen] == '\0' || dir[hlen] == '/')) {
            snprintf(out, out_size, "~%s", dir + hlen);
            return;
        }
    }
    snprintf(out, out_size, "%s", dir);
}

/* Truncate UTF-8 text to fit max_cells display cells (with ellipsis if cut). */
static void fit_cells(char *out, size_t out_size, const char *s, int max_cells)
{
    if (!out || out_size == 0) return;
    if (!s || max_cells <= 0) {
        out[0] = '\0';
        return;
    }
    if (ui_cells(s) <= max_cells) {
        snprintf(out, out_size, "%s", s);
        return;
    }
    int limit = max_cells > 1 ? max_cells - 1 : 0;
    size_t i = 0;
    int used = 0;
    while (s[i]) {
        unsigned char b = (unsigned char)s[i];
        size_t clen = b < 0x80u ? 1 :
                      (b & 0xE0u) == 0xC0u ? 2 :
                      (b & 0xF0u) == 0xE0u ? 3 :
                      (b & 0xF8u) == 0xF0u ? 4 : 1;
        uint32_t cp = b & (0x3Fu >> (clen - 1));
        size_t step = 1;
        for (; step < clen && s[i + step]; step++) {
            cp = (cp << 6) | ((unsigned char)s[i + step] & 0x3Fu);
        }
        int w = step == 1 ? 1 : screen_char_cells(cp);
        if (used + w > limit || i + step + 4 >= out_size) break;
        used += w;
        i += step;
    }
    if (max_cells > 1 && i + 4 <= out_size) {
        memcpy(out, s, i);
        memcpy(out + i, "\xe2\x80\xa6", 4); /* UTF-8 horizontal ellipsis + NUL */
    } else {
        if (i >= out_size) i = out_size - 1;
        memcpy(out, s, i);
        out[i] = '\0';
    }
}

/* --- Query editor (UTF-8 safe) --- */

static void query_insert(struct query_edit *q, const char *s, size_t slen)
{
    if (!q || !s || slen == 0) return;
    size_t len = strlen(q->text);
    if (q->pos > len) q->pos = len;
    if (q->sel_all) {
        q->text[0] = '\0';
        len = 0;
        q->pos = 0;
        q->sel_all = 0;
    }
    char clean[SEARCH_QUERY_BUF];
    size_t w = 0;
    for (size_t i = 0; i < slen;) {
        unsigned char b = (unsigned char)s[i];
        size_t bytes = b < 0x80u ? 1 :
                       (b >= 0xC2u && b < 0xE0u) ? 2 :
                       (b >= 0xE0u && b < 0xF0u) ? 3 :
                       (b >= 0xF0u && b <= 0xF4u) ? 4 : 0;
        if (bytes == 0 || i + bytes > slen) break;
        int valid = 1;
        for (size_t k = 1; k < bytes; k++) {
            if (((unsigned char)s[i + k] & 0xC0u) != 0x80u) {
                valid = 0;
                break;
            }
        }
        if (!valid) break;
        /* Ignore control bytes and newlines in single-line search input. */
        if (b >= 0x20u && b != 0x7Fu) {
            if (len + w + bytes >= sizeof q->text) break;
            memcpy(clean + w, s + i, bytes);
            w += bytes;
        }
        i += bytes;
    }
    if (w == 0) return;
    memmove(q->text + q->pos + w, q->text + q->pos, len - q->pos + 1);
    memcpy(q->text + q->pos, clean, w);
    q->pos += w;
}

static int query_edit_key(struct query_edit *q, int key, int mods, int act)
{
    size_t len = strlen(q->text);
    if (q->pos > len) q->pos = len;

    if (act == ACT_SELECT_ALL) {
        q->sel_all = len > 0;
        q->pos = len;
        return 1;
    }
    if (act == ACT_DESELECT) {
        q->sel_all = 0;
        return 1;
    }
    if (act == ACT_COPY || act == ACT_CUT) {
        if (len > 0 && clipboard_set) {
            (void)clipboard_set(0, q->text, len);
        }
        if (act == ACT_CUT && len > 0) {
            q->text[0] = '\0';
            q->pos = 0;
            q->sel_all = 0;
            return 2;
        }
        return 1;
    }
    if (act == ACT_PASTE) {
        if (clipboard_get) {
            int fd = -1;
            size_t sz = 0;
            if (clipboard_get(0, &fd, &sz) == CLIPBOARD_OK && fd >= 0) {
                char buf[SEARCH_QUERY_BUF];
                size_t want = sz < sizeof buf - 1 ? sz : sizeof buf - 1;
                ssize_t n = read(fd, buf, want);
                close(fd);
                if (n > 0) {
                    query_insert(q, buf, (size_t)n);
                    return 2;
                }
            } else if (fd >= 0) {
                close(fd);
            }
        }
        return 1;
    }

    const char *typed = keys_typed(key);
    if (q->sel_all && (key == K_LEFT || key == K_RIGHT || key == K_BOL || key == K_EOL)) {
        q->pos = (key == K_LEFT || key == K_BOL) ? 0 : len;
        q->sel_all = 0;
        return 1;
    }

    /* Ctrl+W comes through key_action as ACT_CLOSE_TAB in LOCachy's table. */
    int is_ctrl_u = (mods & KEYS_CTRL) && !(mods & KEYS_ALT) && key == ('u' & 0x1F);
    int is_ctrl_k = (mods & KEYS_CTRL) && !(mods & KEYS_ALT) && key == ('k' & 0x1F);
    int is_ctrl_e = (mods & KEYS_CTRL) && !(mods & KEYS_ALT) && key == ('e' & 0x1F);

    if (q->sel_all && (typed || key == '\b' || key == 127 || key == K_DEL ||
                       act == ACT_CLOSE_TAB || is_ctrl_u || is_ctrl_k)) {
        q->text[0] = '\0';
        len = 0;
        q->pos = 0;
        q->sel_all = 0;
        if (!typed) return 2;
    }

    if (key == K_LEFT) {
        q->pos = utf8_prev(q->text, q->pos);
        return 1;
    }
    if (key == K_RIGHT) {
        q->pos = utf8_next(q->text, len, q->pos);
        return 1;
    }
    if (key == K_BOL) {
        q->pos = 0;
        return 1;
    }
    if (key == K_EOL || is_ctrl_e) {
        q->pos = len;
        return 1;
    }
    if (key == '\b' || key == 127) {
        if (q->pos == 0) return 1;
        size_t prev = utf8_prev(q->text, q->pos);
        memmove(q->text + prev, q->text + q->pos, len - q->pos + 1);
        q->pos = prev;
        return 2;
    }
    if (key == K_DEL) {
        if (q->pos >= len) return 1;
        size_t next = utf8_next(q->text, len, q->pos);
        memmove(q->text + q->pos, q->text + next, len - next + 1);
        return 2;
    }
    if (act == ACT_CLOSE_TAB) { /* Ctrl+W: delete word before cursor */
        if (q->pos == 0) return 1;
        size_t prev = q->pos;
        while (prev > 0 && q->text[prev - 1] == ' ') prev--;
        while (prev > 0 && q->text[prev - 1] != ' ' && q->text[prev - 1] != '/') {
            prev = utf8_prev(q->text, prev);
        }
        memmove(q->text + prev, q->text + q->pos, len - q->pos + 1);
        q->pos = prev;
        return 2;
    }
    if (is_ctrl_u) {
        if (q->pos == 0) return 1;
        memmove(q->text, q->text + q->pos, len - q->pos + 1);
        q->pos = 0;
        return 2;
    }
    if (is_ctrl_k) {
        if (q->pos >= len) return 1;
        q->text[q->pos] = '\0';
        return 2;
    }
    if (typed && !(mods & (KEYS_CTRL | KEYS_ALT | KEYS_META))) {
        query_insert(q, typed, strlen(typed));
        return 2;
    }
    return 0;
}

/* --- Search protocol framing & parsing --- */

static const char *tab_scope_name(enum search_tab tab)
{
    switch (tab) {
    case TAB_FILES:   return "names";
    case TAB_CONTENT: return "content";
    case TAB_ALL:
    default:          return "all";
    }
}

/* Format a query line for locachy-searchd: "Q <scope> <query>\n".
 * Returns formatted byte length (> 0) or -1 if query is empty or too long. */
static int search_format_query(enum search_tab tab, const char *query, char *out, size_t out_size)
{
    if (!query || !out || out_size < 8) return -1;

    size_t raw_len = strlen(query);
    size_t a = 0;
    while (a < raw_len && (query[a] == ' ' || query[a] == '\t')) a++;
    size_t b = raw_len;
    while (b > a && (query[b - 1] == ' ' || query[b - 1] == '\t')) b--;
    size_t qlen = b - a;
    if (qlen == 0) return -1;

    const char *scope = tab_scope_name(tab);
    size_t slen = strlen(scope);
    /* Total line without trailing '\n': "Q " (2) + scope + " " (1) + query. */
    size_t line_len = 2 + slen + 1 + qlen;
    if (line_len > SEARCH_REQ_MAX || line_len + 2 > out_size) return -1;

    /* Reject embedded CR/LF/NUL/tab control bytes in request text. */
    for (size_t i = a; i < b; i++) {
        unsigned char c = (unsigned char)query[i];
        if (c < 0x20u || c == 0x7Fu) return -1;
    }

    int n = snprintf(out, out_size, "Q %s %.*s\n", scope, (int)qlen, query + a);
    if (n <= 0 || (size_t)n >= out_size) return -1;
    return n;
}

static int parse_search_kind(const char *s)
{
    if (!s) return SEARCH_KIND_FILE;
    if (strcmp(s, "text") == 0)    return SEARCH_KIND_TEXT;
    if (strcmp(s, "doc") == 0)     return SEARCH_KIND_DOC;
    if (strcmp(s, "image") == 0)   return SEARCH_KIND_IMAGE;
    if (strcmp(s, "media") == 0)   return SEARCH_KIND_MEDIA;
    if (strcmp(s, "archive") == 0) return SEARCH_KIND_ARCHIVE;
    if (strcmp(s, "dir") == 0)     return SEARCH_KIND_DIR;
    return SEARCH_KIND_FILE;
}

enum proto_line_kind {
    PROTO_IGNORE = 0,
    PROTO_HIT,
    PROTO_END,
    PROTO_STATUS,
    PROTO_ERROR
};

/* Parse one newline-stripped protocol line from locachy-searchd.
 * Expected daemon hit format:
 *   R <layer> <kind>\t<path>\t<name>\t<snippet>
 * Terminator:
 *   E   (or DONE)
 * Optional status:
 *   S <files>\t<docs>\t<indexing>
 *   STATUS files=<n> docs=<n> indexing=<0|1>
 */
static enum proto_line_kind search_parse_line(const char *line,
                                              struct search_result *hit_out,
                                              struct daemon_stats *stats_out)
{
    if (!line || !line[0]) return PROTO_IGNORE;
    if (strcmp(line, "E") == 0 || strcmp(line, "DONE") == 0) {
        return PROTO_END;
    }
    if (strncmp(line, "ERR", 3) == 0 && (line[3] == '\0' || line[3] == ' ' || line[3] == '\t')) {
        return PROTO_ERROR;
    }
    if (line[0] == 'S' && line[1] == ' ') {
        if (stats_out) {
            long long files = 0, docs = 0;
            int indexing = 0;
            if (sscanf(line + 2, "%lld\t%lld\t%d", &files, &docs, &indexing) >= 2 ||
                sscanf(line + 2, "%lld %lld %d", &files, &docs, &indexing) >= 2) {
                stats_out->known = 1;
                stats_out->files = files >= 0 ? files : 0;
                stats_out->docs = docs >= 0 ? docs : 0;
                stats_out->indexing = indexing != 0;
                return PROTO_STATUS;
            }
        }
        return PROTO_IGNORE;
    }
    if (strncmp(line, "STATUS ", 7) == 0) {
        if (stats_out) {
            long long files = 0, docs = 0;
            int indexing = 0;
            const char *pf = strstr(line + 7, "files=");
            const char *pd = strstr(line + 7, "docs=");
            const char *pi = strstr(line + 7, "indexing=");
            if (pf) files = strtoll(pf + 6, NULL, 10);
            if (pd) docs = strtoll(pd + 5, NULL, 10);
            if (pi) indexing = atoi(pi + 9);
            if (pf || pd) {
                stats_out->known = 1;
                stats_out->files = files >= 0 ? files : 0;
                stats_out->docs = docs >= 0 ? docs : 0;
                stats_out->indexing = indexing != 0;
                return PROTO_STATUS;
            }
        }
        return PROTO_IGNORE;
    }
    if (line[0] != 'R' || line[1] != ' ') {
        return PROTO_IGNORE;
    }

    /* R <layer> <kind>\t<path>\t<name>\t<snippet> */
    const char *p = line + 2;
    char *endptr = NULL;
    long layer = strtol(p, &endptr, 10);
    if (!endptr || endptr == p || *endptr != ' ' || (layer != 1 && layer != 2)) {
        return PROTO_IGNORE;
    }
    p = endptr + 1;

    const char *t1 = strchr(p, '\t');
    if (!t1 || t1 == p) return PROTO_IGNORE;
    const char *t2 = strchr(t1 + 1, '\t');
    if (!t2 || t2 == t1 + 1) return PROTO_IGNORE;
    const char *t3 = strchr(t2 + 1, '\t');

    char kind_tok[24];
    size_t klen = (size_t)(t1 - p);
    if (klen >= sizeof kind_tok) klen = sizeof kind_tok - 1;
    memcpy(kind_tok, p, klen);
    kind_tok[klen] = '\0';

    if (hit_out) {
        memset(hit_out, 0, sizeof *hit_out);
        hit_out->layer = (int)layer;
        hit_out->layers_mask = (layer == 2) ? 2 : 1;
        hit_out->source = (layer == 2) ? RES_CONTENT : RES_FILE;
        hit_out->kind = parse_search_kind(kind_tok);
        hit_out->app_idx = -1;

        size_t plen = (size_t)(t2 - (t1 + 1));
        if (plen >= sizeof hit_out->path) plen = sizeof hit_out->path - 1;
        memcpy(hit_out->path, t1 + 1, plen);
        hit_out->path[plen] = '\0';

        const char *name_start = t2 + 1;
        size_t nlen = t3 ? (size_t)(t3 - name_start) : strlen(name_start);
        if (nlen >= sizeof hit_out->name) nlen = sizeof hit_out->name - 1;
        memcpy(hit_out->name, name_start, nlen);
        hit_out->name[nlen] = '\0';

        if (!hit_out->name[0]) {
            const char *slash = strrchr(hit_out->path, '/');
            const char *base = (slash && slash[1]) ? slash + 1 : hit_out->path;
            size_t blen = strnlen(base, sizeof hit_out->name - 1);
            memcpy(hit_out->name, base, blen);
            hit_out->name[blen] = '\0';
        }

        if (t3 && t3[1]) {
            sanitize_display(t3 + 1, hit_out->snippet, sizeof hit_out->snippet);
        }
    }
    return PROTO_HIT;
}

/* --- Local catalog: built-in host programs + installed containers --- */

static const struct {
    const char *id;
    const char *label;
    const char *keywords;
    const char *default_spec;
    int         has_window;
} builtin_programs[] = {
    { "files",     N_("Files"),     "file manager folder browser explorer", "/usr/local/bin/files", 1 },
    { "settings",  N_("Settings"),  "preferences display network power sound keyboard", "/usr/local/bin/settings", 1 },
    { "packages",  N_("Packages"),  "software install update pacman aur apps", "/usr/local/bin/packages", 1 },
    { "firmware",  N_("Firmware"),  "bios ec fwupd hardware update", "/usr/local/bin/firmware", 1 },
    { "archive",   N_("Archive"),   "zip tar 7z extract compress ark", "/usr/local/bin/archive", 1 },
    { "term",      N_("Term"),      "terminal shell console command bash", "/usr/local/bin/term /usr/local/lib/locachy/user-shell", 1 },
    { "view",      N_("View"),      "document viewer pdf image epub okular", "/usr/local/bin/view", 1 },
    { "edit",      N_("Edit"),      "text code editor", "/usr/local/bin/edit", 1 },
    { "mixer",     N_("Mixer"),     "audio volume sound pipewire wiremix", "/usr/local/bin/term /usr/bin/wiremix", 1 },
    { "network",   N_("Network"),   "wifi ethernet vpn nmtui connection", "/usr/local/bin/term /usr/bin/nmtui", 1 },
    { "bluetooth", N_("Bluetooth"), "bt wireless audio bluetui", "/usr/local/bin/term /usr/bin/bluetui", 1 },
};

static int valid_container_name(const char *name)
{
    if (!name || !name[0] || strlen(name) >= OPEN_NAME || strncmp(name, "window-", 7) == 0) {
        return 0;
    }
    if (!isalnum((unsigned char)name[0])) return 0;
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.') return 0;
    }
    return 1;
}

static void add_app_entry(struct search_state *st, const struct app_entry *entry)
{
    if (!st || !entry || !entry->id[0] || st->napp_entries >= SEARCH_MAX_APPS) return;
    for (size_t i = 0; i < st->napp_entries; i++) {
        if (st->apps[i].kind == entry->kind && strcmp(st->apps[i].id, entry->id) == 0) {
            return;
        }
    }
    st->apps[st->napp_entries++] = *entry;
}

struct cont_meta {
    char package[128];
    char command[128];
};

static void on_container_conf(const char *key, const char *val, void *arg)
{
    struct cont_meta *m = arg;
    if (strcmp(key, "package") == 0) {
        snprintf(m->package, sizeof m->package, "%s", val);
    } else if (strcmp(key, "command") == 0) {
        snprintf(m->command, sizeof m->command, "%s", val);
    }
}

static void on_rc_program(const char *key, const char *val, void *arg)
{
    struct search_state *st = arg;
    if (strncmp(key, "program_", 8) != 0 || !key[8] || !val || !val[0]) return;
    const char *id = key + 8;
    if (strcmp(id, "desktop") == 0 || strcmp(id, "search") == 0) return;

    /* Update existing builtin's exec_spec if configured, else add custom program. */
    for (size_t i = 0; i < st->napp_entries; i++) {
        if (st->apps[i].kind == APP_BUILTIN && strcmp(st->apps[i].id, id) == 0) {
            snprintf(st->apps[i].exec_spec, sizeof st->apps[i].exec_spec, "%s", val);
            return;
        }
    }
    struct app_entry e;
    memset(&e, 0, sizeof e);
    e.kind = APP_BUILTIN;
    snprintf(e.id, sizeof e.id, "%s", id);
    snprintf(e.label, sizeof e.label, "%s", id);
    if (e.label[0] >= 'a' && e.label[0] <= 'z') e.label[0] = (char)(e.label[0] - 'a' + 'A');
    snprintf(e.exec_spec, sizeof e.exec_spec, "%s", val);
    add_app_entry(st, &e);
}

static void scan_containers_dir(struct search_state *st, const char *dir_path)
{
    DIR *d = opendir(dir_path);
    if (!d) return;

    char names[SEARCH_MAX_APPS][OPEN_NAME];
    size_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < SEARCH_MAX_APPS) {
        size_t len = strlen(ent->d_name);
        if (len <= 5 || len - 5 >= OPEN_NAME || strcmp(ent->d_name + len - 5, ".conf") != 0) {
            continue;
        }
        char cname[OPEN_NAME];
        snprintf(cname, sizeof cname, "%.*s", (int)(len - 5), ent->d_name);
        if (!valid_container_name(cname)) continue;
        memcpy(names[n++], cname, OPEN_NAME);
    }
    closedir(d);

    /* Sort alphabetically for deterministic display. */
    for (size_t i = 0; i + 1 < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (strcmp(names[i], names[j]) > 0) {
                char tmp[OPEN_NAME];
                memcpy(tmp, names[i], OPEN_NAME);
                memcpy(names[i], names[j], OPEN_NAME);
                memcpy(names[j], tmp, OPEN_NAME);
            }
        }
    }

    for (size_t i = 0; i < n; i++) {
        struct app_entry e;
        memset(&e, 0, sizeof e);
        e.kind = APP_CONTAINER;
        size_t idlen = strnlen(names[i], sizeof e.id - 1);
        memcpy(e.id, names[i], idlen);
        e.id[idlen] = '\0';
        if (open_label) {
            open_label(e.id, NULL, e.label, sizeof e.label);
        }
        if (!e.label[0]) {
            memcpy(e.label, e.id, idlen + 1);
        }

        char cpath[PATH_MAX];
        int cp = snprintf(cpath, sizeof cpath, "%s/%.63s.conf", dir_path, e.id);
        struct cont_meta m;
        memset(&m, 0, sizeof m);
        if (cp > 0 && (size_t)cp < sizeof cpath) {
            if (conf_read) {
                conf_read(cpath, on_container_conf, &m);
            } else {
                FILE *f = fopen(cpath, "re");
                if (f) {
                    char line[512];
                    while (fgets(line, sizeof line, f)) {
                        line[strcspn(line, "\r\n")] = '\0';
                        char *eq = strchr(line, '=');
                        if (!eq || line[0] == '#') continue;
                        *eq++ = '\0';
                        if (*eq == '"') {
                            eq++;
                            char *q = strchr(eq, '"');
                            if (q) *q = '\0';
                        }
                        on_container_conf(line, eq, &m);
                    }
                    fclose(f);
                }
            }
        }
        const char *cmd_base = strrchr(m.command, '/');
        cmd_base = cmd_base ? cmd_base + 1 : m.command;
        (void)snprintf(e.keywords, sizeof e.keywords, "%.62s %.62s", m.package, cmd_base);
        add_app_entry(st, &e);
    }
}

static void load_app_catalog(struct search_state *st, const char *containers_dir)
{
    st->napp_entries = 0;

    /* 1. Built-in host programs (always available for instant matching). */
    for (size_t i = 0; i < sizeof builtin_programs / sizeof builtin_programs[0]; i++) {
        struct app_entry e;
        memset(&e, 0, sizeof e);
        e.kind = APP_BUILTIN;
        snprintf(e.id, sizeof e.id, "%s", builtin_programs[i].id);
        snprintf(e.label, sizeof e.label, "%s", _(builtin_programs[i].label));
        snprintf(e.keywords, sizeof e.keywords, "%s", builtin_programs[i].keywords);
        snprintf(e.exec_spec, sizeof e.exec_spec, "%s", builtin_programs[i].default_spec);
        e.has_window = builtin_programs[i].has_window;
        add_app_entry(st, &e);
    }

    /* Overlay any custom program_* entries from rc.conf. */
    if (rc_read) {
        rc_read(on_rc_program, st);
    }

    /* 2. Installed containers from /etc/locachy/containers (NAME.conf), or test override. */
    const char *cdir = containers_dir;
    if (!cdir || !cdir[0]) {
        cdir = getenv("LOCACHY_CONTAINERS_CONF");
        if (!cdir || !cdir[0]) cdir = SEARCH_CONTAINERS_DIR;
    }
    scan_containers_dir(st, cdir);
}

/* Score how well an app matches query (> 0 if match, 0 if no match). */
static int score_app_match(const struct app_entry *a, const char *query)
{
    if (!query || !query[0]) return 10; /* empty query lists all apps */
    if (str_equal_ci(a->id, query) || str_equal_ci(a->label, query)) return 1000;
    if (str_prefix_ci(a->id, query) || str_prefix_ci(a->label, query)) return 600;
    if (str_contains_ci(a->id, query) || str_contains_ci(a->label, query)) return 300;
    if (a->keywords[0] && str_contains_ci(a->keywords, query)) return 120;
    return 0;
}

static int cmp_app_results(const void *a, const void *b)
{
    const struct search_result *ra = a;
    const struct search_result *rb = b;
    if (ra->score != rb->score) return rb->score - ra->score;
    return strcasecmp(ra->name, rb->name);
}

/* --- Daemon hit storage, deduplication, and view rebuilding --- */

static void daemon_hits_clear(struct search_state *st)
{
    st->daemon_count = 0;
}

/* Add a daemon hit, deduplicating by exact path.
 * If the path already exists:
 *  - When the new hit is layer 2 (has snippet) and the existing hit is layer 1,
 *    upgrade the existing entry in place so it keeps its position AND gains
 *    the snippet (marked as layer 2 / richer hit while preserving file info).
 *  - Otherwise keep the richer existing entry without creating a duplicate. */
static int daemon_hits_add(struct search_state *st, const struct search_result *hit)
{
    if (!st || !hit || !hit->path[0]) return 0;

    for (size_t i = 0; i < st->daemon_count; i++) {
        struct search_result *cur = &st->daemon_hits[i];
        if (strcmp(cur->path, hit->path) == 0) {
            cur->layers_mask |= hit->layers_mask;
            if (hit->snippet[0] && !cur->snippet[0]) {
                snprintf(cur->snippet, sizeof cur->snippet, "%s", hit->snippet);
            }
            if (hit->layer == 2 && cur->layer == 1) {
                /* Keep layer 1 match status while attaching layer 2 snippet. */
                cur->layer = 2;
                cur->source = RES_CONTENT;
            }
            return 0;
        }
    }

    if (st->daemon_count >= SEARCH_MAX_DAEMON_HITS) return 0;
    if (st->daemon_count == st->daemon_cap) {
        size_t ncap = st->daemon_cap ? st->daemon_cap * 2 : 64;
        if (ncap > SEARCH_MAX_DAEMON_HITS) ncap = SEARCH_MAX_DAEMON_HITS;
        struct search_result *n = realloc(st->daemon_hits, ncap * sizeof *n);
        if (!n) return -1;
        st->daemon_hits = n;
        st->daemon_cap = ncap;
    }
    st->daemon_hits[st->daemon_count++] = *hit;
    return 1;
}

static int view_push(struct search_state *st, const struct search_result *r)
{
    if (st->view_count >= SEARCH_MAX_VIEW_HITS) return 0;
    if (st->view_count == st->view_cap) {
        size_t ncap = st->view_cap ? st->view_cap * 2 : 64;
        if (ncap > SEARCH_MAX_VIEW_HITS) ncap = SEARCH_MAX_VIEW_HITS;
        struct search_result *n = realloc(st->view, ncap * sizeof *n);
        if (!n) return -1;
        st->view = n;
        st->view_cap = ncap;
    }
    st->view[st->view_count++] = *r;
    return 0;
}

static void rebuild_view(struct search_state *st)
{
    char norm[SEARCH_QUERY_BUF];
    trim_query_copy(st->q.text, norm, sizeof norm);

    /* Preserve selected path/app across streaming updates when possible. */
    char prev_key[SEARCH_PATH_MAX] = "";
    enum result_source prev_src = RES_APP;
    int had_sel = (st->sel >= 0 && (size_t)st->sel < st->view_count);
    if (had_sel) {
        prev_src = st->view[st->sel].source;
        snprintf(prev_key, sizeof prev_key, "%s",
                 prev_src == RES_APP ? st->view[st->sel].name : st->view[st->sel].path);
    }

    st->view_count = 0;

    /* 1. Matching apps (instant, 0 ms) for All and Apps tabs. */
    if (st->tab == TAB_ALL || st->tab == TAB_APPS) {
        struct search_result matched_apps[SEARCH_MAX_APPS];
        size_t nmatch = 0;
        for (size_t i = 0; i < st->napp_entries; i++) {
            const struct app_entry *a = &st->apps[i];
            int sc = score_app_match(a, norm);
            if (sc <= 0) continue;
            struct search_result r;
            memset(&r, 0, sizeof r);
            r.source = RES_APP;
            r.layer = 0;
            r.app_idx = (int)i;
            r.score = sc;
            snprintf(r.name, sizeof r.name, "%s", a->label);
            snprintf(r.snippet, sizeof r.snippet, "%s",
                     a->kind == APP_CONTAINER ? _("Container app") : _("Host program"));
            matched_apps[nmatch++] = r;
        }
        if (nmatch > 1) {
            qsort(matched_apps, nmatch, sizeof matched_apps[0], cmp_app_results);
        }
        for (size_t i = 0; i < nmatch; i++) {
            (void)view_push(st, &matched_apps[i]);
        }
    }

    /* 2. Daemon results (only when query is non-empty). */
    if (norm[0] && st->tab != TAB_APPS) {
        /* Layer 1 (file names) before Layer 2 (content). */
        if (st->tab == TAB_ALL || st->tab == TAB_FILES) {
            for (size_t i = 0; i < st->daemon_count; i++) {
                const struct search_result *h = &st->daemon_hits[i];
                if (h->layers_mask & 1) {
                    (void)view_push(st, h);
                }
            }
        }
        if (st->tab == TAB_ALL || st->tab == TAB_CONTENT) {
            for (size_t i = 0; i < st->daemon_count; i++) {
                const struct search_result *h = &st->daemon_hits[i];
                if (h->layers_mask & 2) {
                    /* Avoid duplicating entries already pushed in TAB_ALL above. */
                    if (st->tab == TAB_ALL && (h->layers_mask & 1)) continue;
                    (void)view_push(st, h);
                }
            }
        }
    }

    if (st->view_count == 0) {
        st->sel = 0;
        st->top = 0;
    } else if (had_sel && prev_key[0]) {
        int found = -1;
        for (size_t i = 0; i < st->view_count; i++) {
            const char *k = (st->view[i].source == RES_APP) ? st->view[i].name : st->view[i].path;
            if (st->view[i].source == prev_src && strcmp(k, prev_key) == 0) {
                found = (int)i;
                break;
            }
        }
        st->sel = found >= 0 ? found : clampi(st->sel, 0, (int)st->view_count - 1);
    } else {
        st->sel = clampi(st->sel, 0, (int)st->view_count - 1);
    }
    st->dirty = 1;
}

/* --- Non-blocking Unix socket client for locachy-searchd --- */

static const char *search_sock_path(void)
{
    const char *env = getenv("LOCACHY_SEARCH_SOCK");
    if (env && env[0]) return env;
    return SEARCH_SOCK_PATH;
}

static void sock_close(struct search_state *st)
{
    if (st->sock_fd >= 0) {
        close(st->sock_fd);
        st->sock_fd = -1;
    }
    st->sock_connecting = 0;
    st->query_inflight = 0;
    st->out_off = 0;
    st->out_len = 0;
    st->in_len = 0;
    st->in_discard = 0;
}

static int sock_connect_nb(struct search_state *st)
{
    if (st->sock_fd >= 0) return 0;

    const char *path = search_sock_path();
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        st->daemon_down = 1;
        return -1;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) {
        close(fd);
        st->daemon_down = 1;
        return -1;
    }
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        st->sock_fd = fd;
        st->sock_connecting = 0;
        st->daemon_down = 0;
        return 0;
    }
    if (errno == EINPROGRESS || errno == EAGAIN) {
        st->sock_fd = fd;
        st->sock_connecting = 1;
        st->daemon_down = 0;
        return 0;
    }

    close(fd);
    st->daemon_down = 1;
    return -1;
}

static void sock_flush_out(struct search_state *st)
{
    if (st->sock_fd < 0) return;

    if (st->sock_connecting) {
        int err = 0;
        socklen_t elen = sizeof err;
        if (getsockopt(st->sock_fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            sock_close(st);
            st->daemon_down = 1;
            st->dirty = 1;
            return;
        }
        st->sock_connecting = 0;
    }

    while (st->out_off < st->out_len) {
        ssize_t n = send(st->sock_fd, st->outbuf + st->out_off,
                         st->out_len - st->out_off, MSG_NOSIGNAL);
        if (n > 0) {
            st->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        sock_close(st);
        st->daemon_down = 1;
        st->dirty = 1;
        return;
    }
    if (st->out_off >= st->out_len) {
        st->out_off = 0;
        st->out_len = 0;
    }
}

/* Cancel any stale in-flight request (by reconnecting cleanly so old streamed
 * lines cannot be mistaken for the new generation) and dispatch the new query. */
static void dispatch_daemon_query(struct search_state *st)
{
    st->pending_query = 0;

    char norm[SEARCH_QUERY_BUF];
    trim_query_copy(st->q.text, norm, sizeof norm);
    if (!norm[0] || st->tab == TAB_APPS) {
        daemon_hits_clear(st);
        if (st->query_inflight) sock_close(st);
        st->sent_query[0] = '\0';
        rebuild_view(st);
        return;
    }

    /* If another query is currently in flight, drop the old connection so its
     * remaining response bytes are discarded immediately without stalling UI. */
    if (st->query_inflight) {
        sock_close(st);
    }

    daemon_hits_clear(st);
    rebuild_view(st);

    char req[SEARCH_REQ_MAX + 8];
    int rlen = search_format_query(st->tab, norm, req, sizeof req);
    if (rlen <= 0) return;

    if (sock_connect_nb(st) != 0) {
        st->dirty = 1;
        return;
    }

    memcpy(st->outbuf, req, (size_t)rlen);
    st->out_off = 0;
    st->out_len = (size_t)rlen;
    st->in_len = 0;
    st->query_inflight = 1;
    st->inflight_gen = st->generation;
    snprintf(st->sent_query, sizeof st->sent_query, "%s", norm);
    st->sent_tab = st->tab;

    sock_flush_out(st);
    /* If a kept-alive connection went stale (EPIPE/ECONNRESET on send),
     * reconnect once immediately so the user does not see a false disconnect. */
    if (st->sock_fd < 0 && st->daemon_down) {
        if (sock_connect_nb(st) == 0) {
            memcpy(st->outbuf, req, (size_t)rlen);
            st->out_off = 0;
            st->out_len = (size_t)rlen;
            st->in_len = 0;
            st->query_inflight = 1;
            st->inflight_gen = st->generation;
            sock_flush_out(st);
        }
    }
}

static void schedule_query(struct search_state *st, int immediate)
{
    st->generation++;
    /* Refresh local app matches at 0 ms latency immediately. */
    char norm[SEARCH_QUERY_BUF];
    trim_query_copy(st->q.text, norm, sizeof norm);
    if (!norm[0] || st->tab == TAB_APPS) {
        daemon_hits_clear(st);
        if (st->query_inflight) sock_close(st);
        st->pending_query = 0;
        st->sent_query[0] = '\0';
        rebuild_view(st);
        return;
    }

    rebuild_view(st);

    int64_t now = search_now_ms();
    int fast_typing = !immediate && (now - st->last_edit_ms < SEARCH_FAST_TYPE_MS);
    st->last_edit_ms = now;

    if (immediate || !fast_typing) {
        dispatch_daemon_query(st);
    } else {
        st->pending_query = 1;
        st->pending_at_ms = now + SEARCH_DEBOUNCE_MS;
    }
}

/* Read available bytes from sock_fd, parse complete lines, and merge hits for
 * the current generation (discarding stale-generation responses). */
static void sock_read_in(struct search_state *st)
{
    if (st->sock_fd < 0) return;

    int changed = 0;
    for (;;) {
        if (st->in_len >= sizeof st->inbuf - 1) {
            /* Overlong line without '\n': enter discard mode until the next '\n'
             * so the remainder is never parsed as a forged protocol frame. */
            st->in_len = 0;
            st->in_discard = 1;
        }
        ssize_t n = recv(st->sock_fd, st->inbuf + st->in_len,
                         sizeof st->inbuf - 1 - st->in_len, 0);
        if (n > 0) {
            st->in_len += (size_t)n;
            st->inbuf[st->in_len] = '\0';

            char *start = st->inbuf;
            if (st->in_discard) {
                char *nl0 = memchr(start, '\n', st->in_len);
                if (!nl0) {
                    st->in_len = 0;
                    continue;
                }
                start = nl0 + 1;
                st->in_discard = 0;
            }
            char *nl;
            while ((nl = memchr(start, '\n', st->in_len - (size_t)(start - st->inbuf))) != NULL) {
                *nl = '\0';
                if (nl > start && nl[-1] == '\r') nl[-1] = '\0';

                struct search_result hit;
                enum proto_line_kind pk = search_parse_line(start, &hit, &st->stats);
                if (pk == PROTO_HIT) {
                    if (st->query_inflight && st->inflight_gen == st->generation) {
                        if (daemon_hits_add(st, &hit) >= 0) changed = 1;
                    }
                } else if (pk == PROTO_END) {
                    if (st->inflight_gen == st->generation) {
                        st->query_inflight = 0;
                        changed = 1;
                    }
                } else if (pk == PROTO_STATUS) {
                    st->dirty = 1;
                } else if (pk == PROTO_ERROR) {
                    st->query_inflight = 0;
                    st->dirty = 1;
                }
                start = nl + 1;
            }
            size_t consumed = (size_t)(start - st->inbuf);
            st->in_len -= consumed;
            if (st->in_len > 0 && consumed > 0) {
                memmove(st->inbuf, start, st->in_len);
            }
            continue;
        }
        if (n == 0) {
            /* Peer closed cleanly. */
            int was_inflight = st->query_inflight;
            sock_close(st);
            if (was_inflight) {
                st->dirty = 1;
            }
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        sock_close(st);
        st->daemon_down = 1;
        st->dirty = 1;
        break;
    }

    if (changed) {
        rebuild_view(st);
    }
}

/* --- Opening results (both Wayland shared display and direct framebuffer) --- */

static void set_status(struct search_state *st, int is_error, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_status(struct search_state *st, int is_error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->msg, sizeof st->msg, fmt, ap);
    va_end(ap);
    st->msg_error = is_error;
    st->dirty = 1;
}

static int run_helper_sync(const char *bin, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        setsid();
        execv(bin, argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int launch_app(struct search_state *st, const struct app_entry *app)
{
    int in_wl = wl_backend_requested() > 0;
    if (app->kind == APP_CONTAINER) {
        if (in_wl) {
            char *argv[] = { "locachy-seat", "--open", (char *)app->id, NULL };
            if (run_helper_sync(SEARCH_SEAT_BIN, argv) == 0) return 0;
        } else if (base_app_action) {
            char why[256] = "";
            if (base_app_action("open", app->id, why, sizeof why) == 0) return 0;
        }
        set_status(st, 1, _("Cannot open container %s."), app->id);
        return -1;
    }

    /* Built-in / configured host program. */
    if (in_wl && app->has_window) {
        char *argv[] = { "locachy-seat", "--window", (char *)app->id, NULL };
        if (run_helper_sync(SEARCH_SEAT_BIN, argv) == 0) return 0;
        set_status(st, 1, _("Cannot open %s window."), app->label);
        return -1;
    }

    /* Direct host framebuffer: ask home via open_in_home("program:NAME", "")
     * (or open_in_home(NULL, home_dir) for built-in files). */
    if (strcmp(app->id, "files") == 0) {
        if (open_in_home && st->home_dir[0] && open_in_home(NULL, st->home_dir) == 0) {
            return 0;
        }
        if (base_call && access(SEARCH_FILES_BIN, X_OK) == 0) {
            char *argv[] = { SEARCH_FILES_BIN, st->home_dir[0] ? st->home_dir : NULL, NULL };
            if (base_call(argv) >= 0) return 0;
        }
    } else {
        char with[OPEN_NAME + 16];
        snprintf(with, sizeof with, OPEN_PROGRAM "%s", app->id);
        if (open_in_home && open_in_home(with, "") == 0) {
            return 0;
        }
    }
    set_status(st, 1, _("Cannot launch %s."), app->label);
    return -1;
}

static int open_parent_folder(struct search_state *st, const char *path)
{
    if (!path || path[0] != '/') return -1;
    char dir[SEARCH_PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        snprintf(dir, sizeof dir, "/");
    } else {
        size_t n = (size_t)(slash - path);
        if (n >= sizeof dir) n = sizeof dir - 1;
        memcpy(dir, path, n);
        dir[n] = '\0';
    }

    if (wl_backend_requested() > 0) {
        char *argv[] = { "locachy-seat", "--window", "files", dir, NULL };
        if (run_helper_sync(SEARCH_SEAT_BIN, argv) == 0) return 0;
        set_status(st, 1, _("Cannot open folder %s."), dir);
        return -1;
    }
    if (open_in_home && open_in_home(NULL, dir) == 0) {
        return 0;
    }
    if (base_call && access(SEARCH_FILES_BIN, X_OK) == 0) {
        char *argv[] = { SEARCH_FILES_BIN, dir, NULL };
        if (base_call(argv) >= 0) return 0;
    }
    set_status(st, 1, _("Cannot open folder %s."), dir);
    return -1;
}

static int open_result(struct search_state *st, const struct search_result *res, int parent_only)
{
    if (!res) return -1;
    if (res->source == RES_APP) {
        if (parent_only) {
            set_status(st, 0, _("Applications do not have a parent folder."));
            return -1;
        }
        if (res->app_idx >= 0 && (size_t)res->app_idx < st->napp_entries) {
            return launch_app(st, &st->apps[res->app_idx]);
        }
        return -1;
    }

    if (parent_only) {
        return open_parent_folder(st, res->path);
    }

    /* Directory hit: open directly in files. */
    struct stat sb;
    int is_dir = (res->kind == SEARCH_KIND_DIR) ||
                 (stat(res->path, &sb) == 0 && S_ISDIR(sb.st_mode));
    if (is_dir) {
        if (wl_backend_requested() > 0) {
            char *argv[] = { "locachy-seat", "--window", "files", (char *)res->path, NULL };
            if (run_helper_sync(SEARCH_SEAT_BIN, argv) == 0) return 0;
            set_status(st, 1, _("Cannot open folder %s."), res->name);
            return -1;
        }
        if (open_in_home && open_in_home(NULL, res->path) == 0) return 0;
        set_status(st, 1, _("Cannot open folder %s."), res->name);
        return -1;
    }

    /* Regular file: delegate to open_in_home(NULL, path), which routes to
     * locachy-bar's open socket on Wayland (handling default rules / chooser /
     * native windows) and to home's open-request spool on the direct host. */
    if (open_in_home && open_in_home(NULL, res->path) == 0) {
        return 0;
    }
    set_status(st, 1, _("Cannot open %s."), res->name);
    return -1;
}

/* --- Drawing --- */

static const char *result_badge(const struct search_result *r)
{
    if (r->source == RES_APP) return _("[App]");
    if (r->layer == 2) {
        if (r->kind == SEARCH_KIND_DOC)  return _("[Doc]");
        if (r->kind == SEARCH_KIND_TEXT) return _("[Text]");
        return _("[Match]");
    }
    switch (r->kind) {
    case SEARCH_KIND_DIR:     return _("[Dir]");
    case SEARCH_KIND_DOC:     return _("[Doc]");
    case SEARCH_KIND_TEXT:    return _("[Text]");
    case SEARCH_KIND_IMAGE:   return _("[Img]");
    case SEARCH_KIND_MEDIA:   return _("[Media]");
    case SEARCH_KIND_ARCHIVE: return _("[Arc]");
    case SEARCH_KIND_FILE:
    default:                  return _("[File]");
    }
}

static uint32_t badge_color(const struct search_result *r, int selected)
{
    if (selected) return ON_ACCENT;
    if (r->source == RES_APP) return SUCCESS;
    if (r->layer == 2 || r->kind == SEARCH_KIND_DOC) return ACCENT_TEXT;
    return DIM;
}

static void format_int_commas(int64_t v, char *out, size_t size)
{
    char raw[32];
    snprintf(raw, sizeof raw, "%lld", (long long)(v < 0 ? 0 : v));
    size_t len = strlen(raw);
    size_t w = 0;
    for (size_t i = 0; i < len && w + 2 < size; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[w++] = ',';
        out[w++] = raw[i];
    }
    out[w] = '\0';
}

/* Rows per result item: 2 when item carries a content snippet, 1 otherwise. */
static int item_height(const struct search_result *r)
{
    return (r->source == RES_CONTENT && r->snippet[0]) ? 2 : 1;
}

static void ensure_sel_visible(struct search_state *st, int list_rows)
{
    if (st->view_count == 0 || list_rows <= 0) {
        st->top = 0;
        return;
    }
    st->sel = clampi(st->sel, 0, (int)st->view_count - 1);
    if (st->top > st->sel) st->top = st->sel;
    if (st->top < 0) st->top = 0;

    /* Advance top until sel fits completely inside list_rows. */
    for (;;) {
        int used = 0;
        for (int i = st->top; i <= st->sel && i < (int)st->view_count; i++) {
            used += item_height(&st->view[i]);
        }
        if (used <= list_rows || st->top >= st->sel) break;
        st->top++;
    }
}

static void compute_card(int cols, int rows, int ime_rows, int wayland,
                         int *cx, int *cy, int *cw, int *ch)
{
    int avail_top = 1;
    int avail_bottom = rows - ime_rows;
    if (avail_bottom < avail_top + 6) avail_bottom = rows;
    int avail_h = avail_bottom - avail_top;

    if (wayland) {
        /* On Wayland, the wlr-layer-shell surface itself is centered on top
         * of the screen at 760x480; the card fills the visible window area. */
        *cx = 0;
        *cy = avail_top;
        *cw = cols;
        *ch = avail_h;
        return;
    }

    /* On the host framebuffer, float a centered card slightly above midpoint. */
    if (cols >= 56 && avail_h >= 14) {
        int w = cols - 6;
        if (w > 86) w = 86;
        int h = avail_h - 4;
        if (h > 24) h = 24;
        int x = (cols - w) / 2;
        int y = avail_top + (avail_h - h) / 3;
        if (y < avail_top + 1) y = avail_top + 1;
        *cx = x;
        *cy = y;
        *cw = w;
        *ch = h;
    } else {
        *cx = 0;
        *cy = avail_top;
        *cw = cols;
        *ch = avail_h;
    }
}

static void draw_hline(int x, int y, int w, const char *left, const char *mid, const char *right,
                       uint32_t fg, uint32_t bg)
{
    if (w < 2) return;
    screen_text(x, y, left, fg, bg);
    for (int c = 1; c < w - 1; c++) {
        screen_text(x + c, y, mid, fg, bg);
    }
    screen_text(x + w - 1, y, right, fg, bg);
}

static int is_wayland_backend(void)
{
    const char *b = getenv("LOCACHY_BACKEND");
    return b && strcmp(b, "wayland") == 0;
}

static void draw_search(struct search_state *st)
{
    int cols = screen_cols();
    int rows = screen_rows();
    if (cols < 16 || rows < 10) return;

    int wayland = is_wayland_backend();
    int ime_rows = 0;
    char ime_r1[512] = "", ime_r2[512] = "";
    if (!wayland && ime_get_overlay(ime_r1, ime_r2, sizeof ime_r1)) {
        ime_rows = ime_r2[0] ? 2 : 1;
    }

    int cx, cy, cw, ch;
    compute_card(cols, rows, ime_rows, wayland, &cx, &cy, &cw, &ch);
    st->card_x = cx;
    st->card_y = cy;
    st->card_w = cw;
    st->card_h = ch;

    int framed = (cw >= 24 && ch >= 10);
    int ix = framed ? cx + 1 : cx;
    int iw = framed ? cw - 2 : cw;

    uint32_t backdrop = (!wayland && (cx > 0 || cy > 1))
                        ? theme_rgb(THEME_OUTPUT, 0x05080C) : BG;
    screen_fill(0, 0, cols, rows, backdrop);
    ui_hits_clear();

    const char *ime_tag = ime_mode() != IME_MODE_RAW ? ime_mode_label() : NULL;
    screen_bar(_("search"), ime_tag);

    /* Fill card interior and draw crisp floating box border. */
    screen_fill(cx, cy, cw, ch, BG);
    if (framed) {
        draw_hline(cx, cy, cw, "┌", "─", "┐", BORDER, SURFACE);
        const char *title = _(" Search ");
        int tw = ui_cells(title);
        if (tw + 4 < cw) {
            screen_text(cx + 2, cy, title, ACCENT_TEXT, SURFACE);
        }
        for (int row = cy + 1; row < cy + ch - 1; row++) {
            screen_text(cx, row, "│", BORDER, BG);
            screen_text(cx + cw - 1, row, "│", BORDER, BG);
        }
        draw_hline(cx, cy + ch - 1, cw, "└", "─", "┘", BORDER, BG);
    }

    /* Search input row at top of floating card. */
    int box_row = framed ? cy + 1 : cy;
    screen_fill(ix, box_row, iw, 1, SURFACE);
    if (framed) {
        screen_text(cx, box_row, "│", BORDER, SURFACE);
        screen_text(cx + cw - 1, box_row, "│", BORDER, SURFACE);
    }
    const char *prompt = _("Search: ");
    int pcols = ui_cells(prompt);
    screen_text(ix + 1, box_row, prompt, ACCENT_TEXT, SURFACE);

    int clear_w = 0;
    const char *clear_lbl = _("[x]");
    if (st->q.text[0]) {
        clear_w = ui_cells(clear_lbl) + 1;
    }

    int input_col = ix + 1 + pcols;
    int input_w = iw - 2 - pcols - clear_w;
    if (input_w < 6) input_w = 6;
    if (input_col + input_w > ix + iw - 1) input_w = ix + iw - 1 - input_col;

    screen_fill(input_col, box_row, input_w, 1, CONTROL);
    ui_hit(input_col, box_row, input_w, 1, HIT_INPUT);

    size_t qlen = strlen(st->q.text);
    if (st->q.pos > qlen) st->q.pos = qlen;

    if (qlen == 0) {
        char ph[128];
        fit_cells(ph, sizeof ph, _("Type to search apps, files, and documents..."), input_w - 2);
        if (st->cursor_on) {
            screen_text(input_col, box_row, " ", ON_ACCENT, ACCENT);
            screen_text(input_col + 1, box_row, ph, DIM, CONTROL);
        } else {
            screen_text(input_col, box_row, ph, DIM, CONTROL);
        }
    } else if (st->q.sel_all) {
        char shown[SEARCH_QUERY_BUF + 8];
        fit_cells(shown, sizeof shown, st->q.text, input_w);
        screen_text(input_col, box_row, shown, ON_ACCENT, ACCENT);
    } else {
        /* Scroll input horizontally so cursor stays inside input_w. */
        size_t left = 0;
        while (left < st->q.pos) {
            char prefix[SEARCH_QUERY_BUF];
            size_t plen = st->q.pos - left;
            if (plen >= sizeof prefix) plen = sizeof prefix - 1;
            memcpy(prefix, st->q.text + left, plen);
            prefix[plen] = '\0';
            if (ui_cells(prefix) < input_w - 1) break;
            left = utf8_next(st->q.text, qlen, left);
        }

        char before[SEARCH_QUERY_BUF], at_ch[8] = " ", after[SEARCH_QUERY_BUF];
        size_t blen = st->q.pos - left;
        if (blen >= sizeof before) blen = sizeof before - 1;
        memcpy(before, st->q.text + left, blen);
        before[blen] = '\0';

        int col = input_col;
        if (before[0]) {
            screen_text(col, box_row, before, FG, CONTROL);
            col += ui_cells(before);
        }

        size_t after_off = st->q.pos;
        if (st->q.pos < qlen) {
            size_t next = utf8_next(st->q.text, qlen, st->q.pos);
            size_t clen = next - st->q.pos;
            if (clen >= sizeof at_ch) clen = sizeof at_ch - 1;
            memcpy(at_ch, st->q.text + st->q.pos, clen);
            at_ch[clen] = '\0';
            after_off = next;
        }
        if (col < input_col + input_w) {
            screen_text(col, box_row, at_ch,
                        st->cursor_on ? ON_ACCENT : FG,
                        st->cursor_on ? ACCENT : CONTROL);
            col += ui_cells(at_ch);
        }
        if (after_off < qlen && col < input_col + input_w) {
            fit_cells(after, sizeof after, st->q.text + after_off, input_col + input_w - col);
            screen_text(col, box_row, after, FG, CONTROL);
        }
    }

    if (st->q.text[0]) {
        int ccol = ix + iw - clear_w;
        screen_text(ccol, box_row, clear_lbl, DIM, SURFACE);
        ui_hit(ccol, box_row, ui_cells(clear_lbl), 1, HIT_CLEAR);
    }

    /* Category filter pills + result counter right below the search bar. */
    int tab_row = box_row + 1;
    screen_fill(ix, tab_row, iw, 1, BG);
    static const char *const tab_labels[TAB_COUNT] = {
        N_("All"), N_("Apps"), N_("Files"), N_("Content")
    };
    int tc = ix + 1;
    for (int t = 0; t < TAB_COUNT; t++) {
        char btn[48];
        snprintf(btn, sizeof btn, "[%s]", _(tab_labels[t]));
        int bw = ui_cells(btn);
        if (tc + bw >= ix + iw) break;
        int active = (st->tab == (enum search_tab)t);
        screen_text(tc, tab_row, btn, active ? ON_ACCENT : FG, active ? ACCENT : SURFACE);
        ui_hit(tc, tab_row, bw, 1, HIT_TAB_BASE + t);
        tc += bw + 1;
    }

    char count_lbl[96] = "";
    if (st->query_inflight) {
        snprintf(count_lbl, sizeof count_lbl,
                 P_("%zu result (searching...)", "%zu results (searching...)", st->view_count),
                 st->view_count);
    } else if (st->view_count > 0 || st->q.text[0]) {
        snprintf(count_lbl, sizeof count_lbl,
                 P_("%zu result", "%zu results", st->view_count),
                 st->view_count);
    }
    if (count_lbl[0]) {
        int cw_lbl = ui_cells(count_lbl);
        if (ix + iw - cw_lbl - 1 > tc + 1) {
            screen_text(ix + iw - cw_lbl - 1, tab_row, count_lbl, DIM, BG);
        }
    }

    /* Divider / daemon status banner between search header and results. */
    int sep_row = tab_row + 1;
    if (st->daemon_down) {
        screen_fill(ix, sep_row, iw, 1, BG);
        char warn[256];
        fit_cells(warn, sizeof warn,
                  _("Search index unavailable — showing programs only"), iw - 2);
        screen_text(ix + 1, sep_row, warn, ERROR, BG);
    } else if (framed) {
        draw_hline(cx, sep_row, cw, "├", "─", "┤", BORDER, BG);
    } else {
        screen_fill(ix, sep_row, iw, 1, BG);
        for (int c = 1; c < iw - 1; c++) {
            screen_text(ix + c, sep_row, "─", BORDER, BG);
        }
    }

    /* Results list inside the floating card. */
    int list_top = sep_row + 1;
    int footer_top = cy + ch - (framed ? 3 : 2);
    if (footer_top < list_top + 1) footer_top = list_top + 1;
    int list_rows = footer_top - list_top;

    ensure_sel_visible(st, list_rows);

    if (st->view_count == 0) {
        char empty[256];
        if (st->query_inflight) {
            snprintf(empty, sizeof empty, "%s", _("Searching..."));
        } else if (st->q.text[0]) {
            snprintf(empty, sizeof empty, "%s", _("No matching apps, files, or documents."));
        } else {
            snprintf(empty, sizeof empty, "%s", _("Start typing to search apps, files, and document contents."));
        }
        char cut[256];
        fit_cells(cut, sizeof cut, empty, iw - 4);
        screen_text(ix + 2, list_top + 1, cut, DIM, BG);
    } else {
        int r = list_top;
        for (int i = st->top; i < (int)st->view_count && r < footer_top; i++) {
            const struct search_result *it = &st->view[i];
            int ih = item_height(it);
            if (r + ih > footer_top && r > list_top) break;

            int is_sel = (i == st->sel);
            uint32_t rbg = is_sel ? ACCENT : BG;
            uint32_t rfg = is_sel ? ON_ACCENT : FG;
            uint32_t dcol = is_sel ? ON_ACCENT : DIM;
            uint32_t bcol = badge_color(it, is_sel);

            int draw_h = (r + ih <= footer_top) ? ih : (footer_top - r);
            screen_fill(ix, r, iw, draw_h, rbg);
            ui_hit(ix, r, iw, draw_h, HIT_ROW_BASE + i);

            const char *badge = result_badge(it);
            char badge_pad[24];
            ui_pad(badge_pad, sizeof badge_pad, badge, 8);
            screen_text(ix + 1, r, badge_pad, bcol, rbg);

            int name_col = ix + 10;
            int avail = (ix + iw) - name_col - 1;
            if (avail > 0) {
                char name_clean[SEARCH_NAME_MAX];
                sanitize_display(it->name, name_clean, sizeof name_clean);

                char parent[SEARCH_PATH_MAX] = "";
                if (it->source == RES_APP) {
                    snprintf(parent, sizeof parent, "%s", it->snippet);
                } else {
                    compact_parent_path(it->path, st->home_dir, parent, sizeof parent);
                }

                int nw = ui_cells(name_clean);
                int pw = ui_cells(parent);
                int max_name = avail;
                if (parent[0] && avail > 20) {
                    max_name = avail * 55 / 100;
                    if (max_name < 14) max_name = 14;
                    if (nw < max_name) max_name = nw;
                }
                char name_fit[SEARCH_NAME_MAX];
                fit_cells(name_fit, sizeof name_fit, name_clean, max_name);
                screen_text(name_col, r, name_fit, rfg, rbg);

                int after_name = name_col + ui_cells(name_fit) + 2;
                int p_avail = (ix + iw) - after_name - 1;
                if (parent[0] && p_avail > 4) {
                    char p_fit[SEARCH_PATH_MAX];
                    fit_cells(p_fit, sizeof p_fit, parent, p_avail);
                    int draw_col = after_name;
                    if (pw < p_avail) {
                        /* Right-align compact path when room permits. */
                        int right_col = ix + iw - 1 - ui_cells(p_fit);
                        if (right_col > after_name) draw_col = right_col;
                    }
                    screen_text(draw_col, r, p_fit, dcol, rbg);
                }
            }

            if (draw_h >= 2 && it->source == RES_CONTENT && it->snippet[0]) {
                char snip_fit[SEARCH_SNIPPET_MAX + 16];
                int s_avail = (ix + iw) - name_col - 2;
                if (s_avail > 4) {
                    fit_cells(snip_fit, sizeof snip_fit, it->snippet, s_avail);
                    screen_text(name_col, r + 1, snip_fit, is_sel ? ON_ACCENT : DIM, rbg);
                }
            }
            r += draw_h;
        }
    }

    /* Footer row 1: status message or index statistics. */
    screen_fill(ix, footer_top, iw, 1, SURFACE);
    if (framed) {
        screen_text(cx, footer_top, "│", BORDER, SURFACE);
        screen_text(cx + cw - 1, footer_top, "│", BORDER, SURFACE);
    }
    if (st->msg[0]) {
        char mfit[256];
        fit_cells(mfit, sizeof mfit, st->msg, iw - 2);
        screen_text(ix + 1, footer_top, mfit, st->msg_error ? ERROR : ACCENT_TEXT, SURFACE);
    } else if (st->stats.known) {
        char fnum[32], dnum[32], statline[160];
        format_int_commas(st->stats.files, fnum, sizeof fnum);
        format_int_commas(st->stats.docs, dnum, sizeof dnum);
        if (st->stats.indexing) {
            snprintf(statline, sizeof statline, _("%s files · %s docs · indexing..."), fnum, dnum);
        } else {
            snprintf(statline, sizeof statline, _("%s files · %s docs indexed"), fnum, dnum);
        }
        char sfit[160];
        fit_cells(sfit, sizeof sfit, statline, iw - 2);
        screen_text(ix + 1, footer_top, sfit, DIM, SURFACE);
    } else {
        char sline[160];
        if (st->daemon_down) {
            snprintf(sline, sizeof sline, P_("%zu program available", "%zu programs available",
                                             st->napp_entries), st->napp_entries);
        } else {
            snprintf(sline, sizeof sline, _("Ready · %zu programs · file & document index active"),
                     st->napp_entries);
        }
        char sfit[160];
        fit_cells(sfit, sizeof sfit, sline, iw - 2);
        screen_text(ix + 1, footer_top, sfit, DIM, SURFACE);
    }

    /* Footer row 2: key hints. */
    screen_fill(ix, footer_top + 1, iw, 1, BG);
    char hfit[256];
    /* TRANSLATORS: key names (Enter, Ctrl+O, Tab, F1, Esc); keep them. */
    fit_cells(hfit, sizeof hfit,
              _("Enter: open  Ctrl+O: folder  Tab: filter  F1: help  Esc: close"),
              iw - 2);
    screen_text(ix + 1, footer_top + 1, hfit, DIM, BG);

    /* Optional IME composition bar at the bottom on direct framebuffer. */
    if (ime_rows > 0) {
        int iy = rows - ime_rows;
        uint32_t ibg = theme_rgb(THEME_PANEL, 0x182848);
        screen_fill(0, iy, cols, ime_rows, ibg);
        screen_text(1, iy, ime_r1, FG, ibg);
        if (ime_rows == 2) {
            screen_text(1, iy + 1, ime_r2, ACCENT_TEXT, ibg);
        }
    }

    screen_flush();
    st->dirty = 0;
}

/* --- Input handling --- */

static void cycle_tab(struct search_state *st, int dir)
{
    int next = ((int)st->tab + dir + TAB_COUNT) % TAB_COUNT;
    st->tab = (enum search_tab)next;
    st->sel = 0;
    st->top = 0;
    schedule_query(st, 1);
}

static void move_selection(struct search_state *st, int delta)
{
    if (st->view_count == 0) return;
    st->sel = clampi(st->sel + delta, 0, (int)st->view_count - 1);
    st->dirty = 1;
}

static void handle_pointer(struct search_state *st)
{
    const struct pointer_event *e = pointer_last ? pointer_last() : NULL;
    if (!e || e->cancelled) return;

    if (e->wheel) {
        move_selection(st, -3 * e->wheel);
        return;
    }
    if (!e->pressed || e->button != BTN_LEFT) return;

    /* Clicking outside the floating card on the host framebuffer dismisses search. */
    if (st->card_w > 0 && st->card_h > 0 &&
        (e->col < st->card_x || e->col >= st->card_x + st->card_w ||
         e->row < st->card_y || e->row >= st->card_y + st->card_h)) {
        st->quit = 1;
        return;
    }

    int id = ui_hit_at(e->col, e->row);
    if (id == HIT_CLEAR) {
        if (st->q.text[0]) {
            st->q.text[0] = '\0';
            st->q.pos = 0;
            st->q.sel_all = 0;
            schedule_query(st, 1);
        }
        return;
    }
    if (id >= HIT_TAB_BASE && id < HIT_TAB_BASE + TAB_COUNT) {
        enum search_tab target = (enum search_tab)(id - HIT_TAB_BASE);
        if (target != st->tab) {
            st->tab = target;
            st->sel = 0;
            st->top = 0;
            schedule_query(st, 1);
        }
        return;
    }
    if (id >= HIT_ROW_BASE && id < HIT_ROW_BASE + (int)st->view_count) {
        int idx = id - HIT_ROW_BASE;
        st->sel = idx;
        st->dirty = 1;
        if (e->clicks >= 2) {
            if (open_result(st, &st->view[idx], 0) == 0) {
                st->launched = 1;
                st->quit = 1;
            }
        }
    }
}

static void handle_key(struct search_state *st, int key, int mods)
{
    if (key == K_SHOWN || key == K_REPAINT) {
        st->dirty = 1;
        return;
    }
    if (key == K_ACT_HOME) {
        return;
    }
    if (key == K_ACT_SEARCH) {
        st->quit = 1;
        return;
    }

    st->msg[0] = '\0';
    st->msg_error = 0;
    st->cursor_on = 1;
    st->blink_ms = search_now_ms();

    int act = key_action(key, mods);

    /* Window close, explicit quit, and F1 help take precedence over IME
     * composition so the user is never trapped in the search overlay. */
    if (act == ACT_QUIT) {
        st->quit = 1;
        return;
    }
    if (act == ACT_HELP) {
        if (ui_help(_("Search keys"), search_help_lines) == ACT_QUIT) {
            st->quit = 1;
        }
        st->dirty = 1;
        return;
    }

    /* IME mode / punctuation keys on direct framebuffer. */
    const char *ime_notice = NULL;
    if (ime_action(key, &ime_notice)) {
        const char *commit = ime_get_commit();
        if (commit && commit[0]) {
            query_insert(&st->q, commit, strlen(commit));
            schedule_query(st, 0);
        }
        if (ime_notice) set_status(st, 0, "%s", ime_notice);
        st->dirty = 1;
        return;
    }
    if (ime_mode() == IME_MODE_CN && ime_width_key(key, mods)) {
        const char *wmsg = ime_toggle_width();
        if (wmsg) set_status(st, 0, "%s", wmsg);
        st->dirty = 1;
        return;
    }

    /* Feed through active IME before normal editing. */
    if (ime_mode() != IME_MODE_RAW && key > 0) {
        int taken = ime_process_key(key);
        const char *commit = ime_get_commit();
        if (commit && commit[0]) {
            query_insert(&st->q, commit, strlen(commit));
            schedule_query(st, 0);
        }
        if (taken) {
            st->dirty = 1;
            return;
        }
    }
    if (act == ACT_CANCEL) {
        /* Esc clears non-empty query first; when empty, closes search overlay. */
        if (st->q.text[0]) {
            st->q.text[0] = '\0';
            st->q.pos = 0;
            st->q.sel_all = 0;
            schedule_query(st, 1);
        } else {
            st->quit = 1;
        }
        return;
    }
    if (act == ACT_NEXT_FIELD || act == ACT_NEXT_TAB) {
        cycle_tab(st, 1);
        return;
    }
    if (act == ACT_PREV_FIELD || act == ACT_PREV_TAB) {
        cycle_tab(st, -1);
        return;
    }
    if (act == ACT_OPEN) { /* Ctrl+O: open parent folder in files */
        if (st->view_count > 0 && st->sel >= 0 && (size_t)st->sel < st->view_count) {
            if (open_result(st, &st->view[st->sel], 1) == 0) {
                st->launched = 1;
                st->quit = 1;
            }
        }
        return;
    }

    /* Navigation in results list. */
    int page = screen_rows() > 8 ? screen_rows() - 6 : 5;
    if (key == K_UP) {
        move_selection(st, -1);
        return;
    }
    if (key == K_DOWN) {
        move_selection(st, 1);
        return;
    }
    if (key == K_PGUP) {
        move_selection(st, -page);
        return;
    }
    if (key == K_PGDN) {
        move_selection(st, page);
        return;
    }
    if (key == '\n' && !(mods & (KEYS_CTRL | KEYS_ALT | KEYS_META))) {
        if (st->view_count > 0 && st->sel >= 0 && (size_t)st->sel < st->view_count) {
            if (open_result(st, &st->view[st->sel], 0) == 0) {
                st->launched = 1;
                st->quit = 1;
            }
        }
        return;
    }

    /* Query box text editing (UTF-8, cursor, Ctrl+A/U/K/W/C/X/V). */
    int edit_rc = query_edit_key(&st->q, key, mods, act);
    if (edit_rc == 2) {
        schedule_query(st, 0);
    } else if (edit_rc == 1) {
        st->dirty = 1;
    }
}

/* --- Lifecycle --- */

static void resolve_user_home(char *out, size_t size)
{
    out[0] = '\0';
    struct passwd *pw = getpwuid(geteuid() == 0 ? (uid_t)1000 : geteuid());
    if (pw && pw->pw_dir && pw->pw_dir[0] == '/') {
        snprintf(out, size, "%s", pw->pw_dir);
        return;
    }
    const char *env = getenv("HOME");
    if (env && env[0] == '/') {
        snprintf(out, size, "%s", env);
    }
}

static void search_state_init(struct search_state *st, const char *containers_dir)
{
    memset(st, 0, sizeof *st);
    st->sock_fd = -1;
    st->req_fd = -1;
    st->tab = TAB_ALL;
    st->cursor_on = 1;
    st->blink_ms = search_now_ms();
    st->dirty = 1;
    resolve_user_home(st->home_dir, sizeof st->home_dir);
    load_app_catalog(st, containers_dir);
    rebuild_view(st);
}

static void req_open(struct search_state *st)
{
    const char *env = getenv("LOCACHY_REQUEST_FD");
    if (!env || !env[0]) return;
    char *end = NULL;
    long fd = strtol(env, &end, 10);
    if (!end || *end != '\0' || fd < 3 || fd > 1023) return;
    int fl = fcntl((int)fd, F_GETFL);
    if (fl < 0) return;
    (void)fcntl((int)fd, F_SETFL, fl | O_NONBLOCK);
    (void)fcntl((int)fd, F_SETFD, FD_CLOEXEC);
    st->req_fd = (int)fd;
}

static void req_read_in(struct search_state *st)
{
    if (st->req_fd < 0) return;
    char buf[128];
    for (;;) {
        ssize_t n = read(st->req_fd, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "close") != NULL) {
                st->quit = 1;
                return;
            }
        } else if (n == 0) {
            close(st->req_fd);
            st->req_fd = -1;
            return;
        } else if (errno == EINTR) {
            continue;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                close(st->req_fd);
                st->req_fd = -1;
            }
            return;
        }
    }
}

static void search_state_free(struct search_state *st)
{
    sock_close(st);
    if (st->req_fd >= 0) {
        close(st->req_fd);
        st->req_fd = -1;
    }
    free(st->daemon_hits);
    st->daemon_hits = NULL;
    st->daemon_count = 0;
    st->daemon_cap = 0;
    free(st->view);
    st->view = NULL;
    st->view_count = 0;
    st->view_cap = 0;
}

static int compute_wait_ms(const struct search_state *st, int64_t now)
{
    int timeout = SEARCH_BLINK_MS;
    if (st->pending_query) {
        int64_t rem = st->pending_at_ms - now;
        int t = rem > 0 ? (int)rem : 0;
        if (t < timeout) timeout = t;
    }
    int64_t blink_rem = (st->blink_ms + SEARCH_BLINK_MS) - now;
    int bt = blink_rem > 0 ? (int)blink_rem : 0;
    if (bt < timeout) timeout = bt;
    return timeout;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    if (!screen_open(BASE_FONT)) {
        return 1;
    }
    keys_open();
    ime_init(NULL, NULL);
    ime_set_focus(1);
    if (screen_cursor) screen_cursor(CURSOR_ARROW);

    static struct search_state st;
    search_state_init(&st, NULL);
    req_open(&st);

    /* Probe daemon availability non-blockingly at startup without blocking UI. */
    if (sock_connect_nb(&st) == 0) {
        /* Connection succeeded or in progress; keep socket ready for first query. */
    }

    /* Optional initial query passed as command-line arguments. */
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) query_insert(&st.q, " ", 1);
            query_insert(&st.q, argv[i], strlen(argv[i]));
        }
        schedule_query(&st, 1);
    }

    while (!st.quit) {
        int64_t now = search_now_ms();
        if (st.pending_query && now >= st.pending_at_ms) {
            dispatch_daemon_query(&st);
        }
        if (now - st.blink_ms >= SEARCH_BLINK_MS) {
            st.cursor_on = !st.cursor_on;
            st.blink_ms = now;
            st.dirty = 1;
        }

        if (st.dirty) {
            draw_search(&st);
        }

        int timeout = compute_wait_ms(&st, search_now_ms());
        int key = 0;
        struct pollfd pfds[2];
        int npfds = 0, sock_idx = -1, req_idx = -1;
        if (st.sock_fd >= 0) {
            short ev = POLLIN;
            if (st.sock_connecting || st.out_off < st.out_len) {
                ev |= POLLOUT;
            }
            sock_idx = npfds;
            pfds[npfds++] = (struct pollfd){ .fd = st.sock_fd, .events = ev, .revents = 0 };
        }
        if (st.req_fd >= 0) {
            req_idx = npfds;
            pfds[npfds++] = (struct pollfd){ .fd = st.req_fd, .events = POLLIN, .revents = 0 };
        }
        if (npfds > 0) {
            key = key_poll_many(pfds, (size_t)npfds, timeout);
            if (sock_idx >= 0) {
                if (pfds[sock_idx].revents & (POLLOUT | POLLERR | POLLHUP)) {
                    sock_flush_out(&st);
                }
                if (st.sock_fd >= 0 && (pfds[sock_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                    sock_read_in(&st);
                }
            }
            if (req_idx >= 0 && (pfds[req_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                req_read_in(&st);
            }
        } else {
            key = key_poll_timeout(-1, timeout);
        }

        if (key == K_POINTER) {
            handle_pointer(&st);
        } else if (key != 0) {
            int mods = keys_modifiers ? keys_modifiers() : 0;
            handle_key(&st, key, mods);
        }
    }

    int launched = st.launched;
    search_state_free(&st);
    ime_done();
    return launched ? 2 : 0;
}
