/*
 * Drive edit (edit.c) by its keys and the pointer over real files: the
 * screen is an in-memory grid of cells (screen_text's widths: 2 for wide
 * CJK, as screen.c with FreeType), the keys and pointer events are
 * scripted, the clipboard is captured, files_pick and Rime are stubs. As
 * root it checks that files are the user's (uid 1000).
 *   test-edit-ui DIR   (DIR: made for the files)
 * Build with CONFIG_IME (the stubbed input method), base/ui.c and edit's
 * modules; run in the development root (git, inotify).
 */
#define main edit_main
#ifndef EDIT_TEST_SOURCE
#define EDIT_TEST_SOURCE "../progs/edit.c"
#endif
#include EDIT_TEST_SOURCE
#undef main
#ifndef LOCACHY_I18N_STUB_H
#include "i18n-stub.h"
#endif
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>

static void dump_screen(void);
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); if (strstr(#c, "on_screen")) dump_screen(); exit(1); } } while (0)

/* The screen: COLS x ROWS cells of 10 x 20 pixels. */
#define COLS 136 /* the X230's 1366 pixels at scale 1 */
#define ROWS 30
struct cell {
    uint32_t cp, fg, bg;
};
static struct cell grid[ROWS][COLS];
static int caret_x = -1, caret_y = -1, caret_w;
static char bar_title[64];

void say(const char *fmt, ...)
{
    (void)fmt;
}
static double clock_now = 100.0;
double uptime(void)
{
    return clock_now;
}
int screen_open(const char *font)
{
    (void)font;
    return 1;
}
int screen_cols(void)
{
    return COLS;
}
int screen_rows(void)
{
    return ROWS;
}
void screen_cells(int *w, int *h, int *top)
{
    *w = 10;
    *h = 20;
    *top = 0;
}
/* As screen.c's is_wide_cjk (with FreeType). */
int screen_char_cells(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE4F) || (cp >= 0xFF01 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x2FA1F) ? 2 : 1;
}
void screen_fill(int col, int row, int w, int h, uint32_t rgb)
{
    for (int r = row; r < row + h; r++) {
        for (int c = col; c < col + w; c++) {
            if (r >= 0 && r < ROWS && c >= 0 && c < COLS) {
                grid[r][c] = (struct cell){ ' ', rgb, rgb };
            }
        }
    }
}
void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    if (row < 0 || row >= ROWS) {
        return;
    }
    for (const char *p = s; *p && col < COLS; col++) {
        size_t k;
        uint32_t cp = text_decode(p, strlen(p), &k);
        p += k;
        if (col < 0) {
            continue;
        }
        int wide = screen_char_cells(cp) == 2;
        if (wide && col + 1 >= COLS) {
            continue;
        }
        grid[row][col] = (struct cell){ cp, fg, bg };
        if (wide) {
            grid[row][++col] = (struct cell){ 0, fg, bg }; /* its second cell */
        }
    }
}
void screen_fill_px(int x, int y, int w, int h, uint32_t rgb)
{
    (void)h;
    (void)rgb;
    caret_x = x;
    caret_y = y;
    caret_w = w;
}
void screen_blit(int x, int y, int w, int h, const uint32_t *px, int stride)
{
    (void)x, (void)y, (void)w, (void)h, (void)px, (void)stride;
}
void screen_flush(void)
{
}
void screen_bar(const char *title, const char *ime)
{
    snprintf(bar_title, sizeof bar_title, "%s %s", title, ime ? ime : "");
}
void screen_cursor(int shape)
{
    (void)shape;
}
int keys_open(void)
{
    return 1;
}

/* Device fixture uses the existing cell renderer; FreeType's independent
 * raster/zoom behavior is exercised by test-content-font and boot fixtures.
 * A test may set font_stub: the content font is then a recording one, its
 * cells 6/10 of its size wide and its size + 2 high, each size asked for and
 * each run drawn (text, style, colour) kept; otherwise there is none, as on a
 * build without FreeType. */
struct content_font {
    int px;
};
static struct content_font font_stub_font;
static int font_stub, font_stub_news, font_stub_px;
struct font_run {
    char text[64];
    unsigned style;
    uint32_t fg;
    int x, y;
};
static struct font_run font_runs[4096];
static int nfont_runs;
struct content_font *content_font_new(const char *path, int size_px)
{
    (void)path;
    if (!font_stub) return NULL;
    font_stub_news++;
    font_stub_font.px = size_px;
    font_stub_px = size_px;
    return &font_stub_font;
}
void content_font_free(struct content_font *font) { (void)font; }
int content_font_set_size_px(struct content_font *font, int size_px)
{
    if (!font_stub || !font) return -1;
    size_px = size_px < CONTENT_FONT_MIN_PX ? CONTENT_FONT_MIN_PX : size_px > CONTENT_FONT_MAX_PX ? CONTENT_FONT_MAX_PX : size_px;
    font->px = size_px;
    font_stub_px = size_px;
    return 0;
}
void content_font_metrics(const struct content_font *font, int *w, int *h, int *baseline)
{
    int on = font_stub && font;
    if (w) *w = on ? font->px * 6 / 10 : 0;
    if (h) *h = on ? font->px + 2 : 0;
    if (baseline) *baseline = on ? font->px : 0;
}
void content_font_draw(struct content_font *font, int x, int y, const char *text,
                       int cx, int cy, int cw, int ch, uint32_t fg, uint32_t bg, unsigned style)
{
    (void)font; (void)cx; (void)cy; (void)cw; (void)ch; (void)bg;
    if (font_stub && nfont_runs < (int)(sizeof font_runs / sizeof font_runs[0])) {
        struct font_run *r = &font_runs[nfont_runs++];
        snprintf(r->text, sizeof r->text, "%s", text);
        r->style = style;
        r->fg = fg;
        r->x = x;
        r->y = y;
    }
}

/* The screens these suites check are laid out without the parts at the
 * sides of the text that Kate shows by default: the scrollbar's column (the
 * mini-map in its place) and the Documents tool view at the left of the
 * editing area. The suites built on this fixture state that layout here
 * (and reset() again hides the sidebar, with Projects as its tool view);
 * the program's defaults are kept for test-edit-kate, which checks them and
 * turns them back on. */
static int edit_defaults_scrollbar, edit_defaults_minimap, edit_defaults_marks, edit_defaults_icons;
static int edit_defaults_sidebar, edit_defaults_side_view;
__attribute__((constructor)) static void legacy_view_borders(void)
{
    edit_defaults_scrollbar = view_scrollbar_default;
    edit_defaults_minimap = view_minimap_default;
    edit_defaults_marks = view_marks_default;
    edit_defaults_icons = view_icons_default;
    edit_defaults_sidebar = sidebar;
    edit_defaults_side_view = side_view;
    view_scrollbar_default = view_minimap_default = 0;
    sidebar = 0;
    side_view = SIDE_PROJECTS;
}

/* The scripted keys: what key_poll_* return, with the modifiers held. */
struct event {
    int key, mods;
    const char *name;            /* keys_last_name's */
    struct pointer_event ptr;
};
static struct event queue[256];
static int qhead, qtail, mods_now;
static const char *name_now;
static struct pointer_event ptr_now;

int keys_modifiers(void)
{
    return mods_now;
}
int keys_last_name(char *out, size_t size)
{
    if (!name_now) {
        return -1;
    }
    snprintf(out, size, "%s", name_now);
    return 0;
}
const struct pointer_event *pointer_last(void)
{
    return &ptr_now;
}
static void push(int key, int mods, const char *name)
{
    queue[qtail++ % 256] = (struct event){ key, mods, name, { 0 } };
}
static int pop(int may_wait)
{
    if (qhead == qtail) {
        if (may_wait) {
            printf("FAIL a question waited for a key the script does not have\n");
            exit(1);
        }
        return 0;
    }
    struct event *e = &queue[qhead++ % 256];
    mods_now = e->mods;
    name_now = e->name;
    if (e->key == K_POINTER) {
        ptr_now = e->ptr;
    }
    return e->key;
}
int key_poll_timeout(int fd, int timeout)
{
    (void)fd;
    return pop(timeout < 0);
}
int key_poll_many(struct pollfd *extra, size_t n, int timeout)
{
    CHECK(poll(extra, n, 0) >= 0); /* real document/project inotify fds */
    (void)timeout;
    return pop(0);
}

/* The clipboard, captured. */
static char clip[2][4096];
static size_t clip_len[2];
static int clip_sets[2];
int clipboard_set(int primary, const void *text, size_t size)
{
    CHECK(size < sizeof clip[0]);
    memcpy(clip[primary], text, size);
    clip[primary][size] = '\0';
    clip_len[primary] = size;
    clip_sets[primary]++;
    return CLIPBOARD_OK;
}
int clipboard_get(int primary, int *fd, size_t *size)
{
    int f = memfd_create("clip", 0);
    CHECK(f >= 0 && write(f, clip[primary], clip_len[primary]) == (ssize_t)clip_len[primary]);
    *fd = f;
    *size = clip_len[primary];
    return CLIPBOARD_OK;
}
const char *clipboard_error(int status)
{
    (void)status;
    return "clipboard error";
}

/* files_pick: what the script chose. */
static char pick_result[PATH_MAX], pick_type[64];
static int picks;
int files_pick(const char *dir, const char *keep, const char *type, const char *what, char *out, size_t size)
{
    (void)dir, (void)keep, (void)what;
    picks++;
    snprintf(pick_type, sizeof pick_type, "%s", type);
    if (!pick_result[0]) {
        return 0;
    }
    snprintf(out, size, "%s", pick_result);
    return 1;
}
static char test_dir[256];
void open_user_dirs(char *home, size_t hsize, char *dirs, size_t dsize)
{
    snprintf(home, hsize, "%s", test_dir);
    snprintf(dirs, dsize, "Documents");
}

/* Rime, as a stub: in Chinese mode letters compose, a space commits the word below. */
static int ime_now = IME_MODE_RAW, composing;
static const char *commit;
int ime_init(const char *a, const char *b)
{
    (void)a, (void)b;
    return 0;
}
void ime_done(void)
{
}
void ime_set_focus(int f)
{
    (void)f;
}
int ime_has_focus(void)
{
    return 1;
}
int ime_cycle_mode(void)
{
    ime_now = (ime_now + 1) % 3;
    return ime_now;
}
void ime_set_mode(int m)
{
    ime_now = m;
}
int ime_mode(void)
{
    return ime_now;
}
const char *ime_mode_label(void)
{
    return ime_now == IME_MODE_CN ? "CN" : ime_now == IME_MODE_EN ? "EN" : "RAW";
}
const char *ime_mode_desc(void)
{
    return " [CN] Pinyin ";
}
const char *ime_toggle_width(void)
{
    return NULL;
}
int ime_action(int key, const char **notice) /* as ime.c's, on the stub's modes */
{
    *notice = NULL;
    if (key == K_CTRL_SPACE) {
        ime_cycle_mode();
    } else if (key == K_ACT_IME_CN) {
        ime_set_mode(IME_MODE_CN);
    } else if (key == K_ACT_IME_RAW) {
        ime_set_mode(IME_MODE_RAW);
    } else if (key == K_ACT_IME_WIDTH) {
        *notice = ime_toggle_width();
        return 1;
    } else {
        return 0;
    }
    *notice = ime_mode_desc();
    return 1;
}
int ime_process_key(int key)
{
    if (ime_now != IME_MODE_CN) {
        return 0;
    }
    if (key >= 'a' && key <= 'z') {
        composing = 1;
        return 1;
    }
    if (key == ' ' && composing) {
        composing = 0;
        commit = "你好";
        return 1;
    }
    return 0;
}
const char *ime_get_commit(void)
{
    const char *c = commit;
    commit = NULL;
    return c;
}
int ime_get_overlay(char *row1, char *row2, size_t size)
{
    snprintf(row1, size, "%s", composing ? "[CN] ni 1.你好" : "");
    row2[0] = '\0';
    return composing;
}

/* Helpers. */
static int is_root;

/* A key as keys.c gives it: Ctrl with a letter as its control character,
 * the name of the key for keys_last_name. */
static void key(int k, int mods)
{
    static char name[32];
    int code = k;
    name_now = NULL;
    if ((mods & KEYS_CTRL) && k >= 'a' && k <= 'z') {
        code = k & 0x1F;
        snprintf(name, sizeof name, "Ctrl+%s%s%c", mods & KEYS_ALT ? "Alt+" : "", mods & KEYS_SHIFT ? "Shift+" : "",
                 toupper(k));
        name_now = name;
    } else if ((mods & KEYS_CTRL) && (k == '\n' || k == '\t')) {
        snprintf(name, sizeof name, "Ctrl+%s", k == '\n' ? "Enter" : "Tab");
        name_now = name;
    }
    mods_now = mods;
    key_event(code);
    clock_now += 0.01;
}

static void keys(const char *s)
{
    for (const char *p = s; *p; p++) {
        key((unsigned char)*p, isupper((unsigned char)*p) ? KEYS_SHIFT : 0);
    }
}

static void click(int col, int row, int button, int pressed, int clicks)
{
    ptr_now = (struct pointer_event){ .x = col * 10 + 2, .y = row * 20 + 5, .col = col, .row = row, .button = button,
                                      .pressed = pressed, .clicks = clicks, .buttons = pressed && button == BTN_LEFT };
    mods_now = 0;
    key_event(K_POINTER);
}

static char *text_of(struct doc *d)
{
    return buffer_dup(&d->buf, 0, doc_len(d));
}

static int text_is(struct doc *d, const char *want)
{
    char *t = text_of(d);
    int ok = t && strcmp(t, want) == 0;
    if (!ok) {
        printf("  the text is \"%s\", not \"%s\"\n", t ? t : "(null)", want);
    }
    free(t);
    return ok;
}

/* A screen row as text (the second cells of wide characters left out). */
static void row_text(int row, char *out, size_t size)
{
    size_t o = 0;
    for (int c = 0; c < COLS && o + 5 < size; c++) {
        if (grid[row][c].cp) {
            o += (size_t)text_put(out + o, grid[row][c].cp);
        }
    }
    out[o] = '\0';
}

static void dump_screen(void)
{
    for (int r = 0; r < ROWS; r++) {
        char line[COLS * 4 + 1];
        row_text(r, line, sizeof line);
        printf("  |%s|\n", line);
    }
}

static int on_screen(const char *s)
{
    for (int r = 0; r < ROWS; r++) {
        char line[COLS * 4 + 1];
        row_text(r, line, sizeof line);
        if (strstr(line, s)) {
            return r;
        }
    }
    return -1;
}

static void put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f && fputs(text, f) >= 0 && fclose(f) == 0);
    if (is_root) {
        CHECK(chown(path, EDIT_USER, EDIT_USER) == 0);
    }
}

static char *file_bytes(const char *path)
{
    FILE *f = fopen(path, "r");
    CHECK(f);
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static const char *at_dir(const char *rel)
{
    static char path[4][PATH_MAX];
    static int i;
    i = (i + 1) % 4;
    snprintf(path[i], PATH_MAX, "%s/%s", test_dir, rel);
    return path[i];
}

/* Back to no documents. */
static void reset(void)
{
    while (ndocs > 0) {
        free_doc(docs[0]);
    }
    for (int s = 0; s < MAX_SPACES; s++) {
        free(spaces[s].tabs);
        memset(&spaces[s], 0, sizeof spaces[s]);
    }
    nspaces = 1;
    active = 0;
    edit_layout_free(&view_layout);
    CHECK(edit_layout_init(&view_layout) == 0);
    split_drag = NULL;
    sidebar_drag = 0;
    sidebar_width = SIDE_DEFAULT;
    spaces[0] = (struct space){ .cur = -1, .share = 1000 };
    bar_kind = BAR_NONE;
    bar.focus = FOCUS_VIEW;
    bar.find.s[0] = '\0';
    bar.find.len = bar.find.cur = 0;
    sidebar = tree_focus = 0; /* the layout these suites check: no tool view shown */
    side_view = SIDE_PROJECTS;
    docs_top = 0;
    scroll_drag.on = 0;
    icon_press.on = 0;
    preview_hide();
    if (switcher_open()) switcher_close(0);
    drag.mode = DRAG_NONE;
    qhead = qtail = 0;
    pick_result[0] = '\0';
    mods_now = 0;
}

static double ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6;
}

static long status_kib(const char *field)
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

static struct tab *open_text(const char *rel, const char *text)
{
    put_file(at_dir(rel), text);
    CHECK(open_file(at_dir(rel)));
    return cur_tab();
}

static void test_cursor(void)
{
    reset();
    struct tab *t = open_text("chars.txt", "a中b\xcc\x81" "c\n");
    struct doc *d = t->doc;
    size_t want[] = { 1, 4, 7, 8 };
    for (int i = 0; i < 4; i++) {
        key(K_RIGHT, 0);
        CHECK(t->cursor == want[i]);
    }
    draw();
    int y0, h;
    text_rows(0, &y0, &h);
    /* The gutter is 4 columns; a 1, a wide character 2, b 1, its accent 1, c 1. */
    CHECK(grid[y0][4].cp == 'a' && grid[y0][5].cp == 0x4E2D && grid[y0][6].cp == 0 && grid[y0][7].cp == 'b');
    CHECK(grid[y0][8].cp == 0x301 && grid[y0][9].cp == 'c');
    CHECK(caret_x == (4 + 6) * 10 && caret_y == y0 * 20);
    key(K_LEFT, 0);
    key(K_LEFT, 0);
    CHECK(t->cursor == 4); /* before b: its accent is not a place for the cursor */
    draw();
    CHECK(caret_x == (4 + 3) * 10 && on_screen("1:3") == ROWS - 1);
    keys("X");
    CHECK(text_is(d, "a中Xb\xcc\x81" "c\n"));
    key('\b', 0);
    key('\b', 0);
    CHECK(text_is(d, "ab\xcc\x81" "c\n") && t->cursor == 1);
    key(K_RIGHT, 0);
    key('\b', 0); /* Backspace takes a code point: the accent only */
    CHECK(text_is(d, "abc\n"));
    key(K_LEFT, 0);
    type_text(active, t, "e\xcc\x81", 3); /* as Rime would commit it */
    CHECK(text_is(d, "ae\xcc\x81" "bc\n"));
    key(K_LEFT, 0);
    key(K_DEL, 0); /* Delete takes the cluster */
    CHECK(text_is(d, "abc\n"));
    puts("PASS cursor: by clusters over CJK and combining marks, at the cells screen_text draws; Backspace a code "
         "point, Delete a cluster");

    reset();
    t = open_text("undo.txt", "");
    d = t->doc;
    keys("ab c");
    key('\n', 0);
    keys("de");
    key('z', KEYS_CTRL);
    CHECK(text_is(d, "ab c\n") && t->cursor == 5);
    key('z', KEYS_CTRL);
    CHECK(text_is(d, "ab c"));
    key('z', KEYS_CTRL);
    CHECK(text_is(d, "") && t->cursor == 0);
    key('z', KEYS_CTRL | KEYS_SHIFT);
    key('z', KEYS_CTRL | KEYS_SHIFT);
    CHECK(text_is(d, "ab c\n") && t->cursor == 5);
    keys("x");
    key('z', KEYS_CTRL | KEYS_SHIFT); /* the redo of "de" was dropped */
    CHECK(text_is(d, "ab c\nx"));
    key(K_LEFT, 0);
    key(K_RIGHT, 0);
    keys("y");
    key('z', KEYS_CTRL);
    CHECK(text_is(d, "ab c\nx")); /* the cursor moved between x and y: two steps */
    key('y', KEYS_CTRL);
    CHECK(text_is(d, "ab c\nx")); /* Ctrl+Y is no redo in KDE */
    puts("PASS undo by keys: typing one step, Enter its own, Ctrl+Shift+Z redo, redo dropped by a new edit, a "
         "cursor move ends a step, the cursor put back");
}

static void test_indent(void)
{
    reset();
    struct tab *t = open_text("indent.txt", "    foo\n");
    struct doc *d = t->doc;
    key(K_EOL, 0);
    key('\n', 0);
    CHECK(text_is(d, "    foo\n    \n") && t->cursor == 12);
    keys("bar");
    key('\n', KEYS_CTRL); /* Ctrl+Return: no indentation */
    CHECK(text_is(d, "    foo\n    bar\n\n"));
    key('\n', 0);
    /* The nearest line above that is not empty: "    bar". */
    CHECK(text_is(d, "    foo\n    bar\n\n    \n"));
    reset();
    t = open_text("smart.txt", "  // note\n");
    d = t->doc;
    key(K_EOL, 0);
    key('\n', KEYS_SHIFT);
    CHECK(text_is(d, "  // note\n  // \n"));
    puts("PASS auto-indent (normal): the previous non-empty line's indentation; Ctrl+Return none; Shift+Return "
         "the leading non-word characters");

    reset();
    t = open_text("tab.txt", "ab\n  x\n");
    d = t->doc;
    key(K_RIGHT, 0);
    key('\t', 0); /* not in the leading space: spaces to the next stop */
    CHECK(text_is(d, "a   b\n  x\n") && t->cursor == 4);
    key(K_DOWN, 0);
    key(K_BOL, 0);
    key(K_BOL, 0);
    CHECK(t->cursor == line_start(d, 1));
    key('\t', 0); /* in the leading space: indent to the next multiple */
    CHECK(text_is(d, "a   b\n    x\n"));
    key(K_EOL, 0);
    key(K_BOL, 0); /* Smart Home: the first non-space */
    CHECK(t->cursor == line_start(d, 1) + 4);
    key('\b', 0); /* only spaces to its left: one level less */
    CHECK(text_is(d, "a   b\nx\n"));
    key('i', KEYS_CTRL);
    key('i', KEYS_CTRL);
    CHECK(text_is(d, "a   b\n        x\n"));
    key('\t', KEYS_SHIFT);
    CHECK(text_is(d, "a   b\n    x\n"));
    key('a', KEYS_CTRL);
    key('\t', 0); /* a selection over lines: indented */
    CHECK(text_is(d, "    a   b\n        x\n"));
    puts("PASS Tab, Shift+Tab, Ctrl+I, Backspace in the indentation, Smart Home (tabSmart, spaces of 4)");
}

static void test_brackets_comments(void)
{
    reset();
    struct tab *t = open_text("b.c", "f(\"(\", a[1]);\n  int x;\n  int y;\n");
    struct doc *d = t->doc;
    size_t a, b;
    CHECK(d->syntax);
    syntax_parse(d->syntax, &d->buf, 0);
    t->cursor = t->anchor = 1;
    CHECK(bracket_pair(t, &a, &b) && a == 1 && b == 11); /* not the ( in the string */
    key(K_CTRL_0 + 6, KEYS_CTRL);
    CHECK(t->cursor == 12);
    t->cursor = t->anchor = 1;
    key(0x1E, KEYS_CTRL | KEYS_SHIFT); /* Ctrl+Shift+6 */
    CHECK(t->anchor == 1 && t->cursor == 12);
    t->cursor = t->anchor = 10; /* after [1: the one before the cursor */
    CHECK(bracket_pair(t, &a, &b) && a == 10 && b == 8);
    draw();
    int y0, h;
    text_rows(0, &y0, &h);
    CHECK(grid[y0][4 + 8].bg == C_BRACKET && grid[y0][4 + 10].bg == C_BRACKET);
    puts("PASS brackets: after the cursor then before it, the same colour only, Ctrl+6 and Ctrl+Shift+6, drawn");
    /* The colours on the screen: f( black on the bracket, "(" a string. */
    CHECK(grid[y0][4 + 2].fg == 0xF44F4F && grid[y0][4 + 6].fg == C_TEXT);
    CHECK(grid[y0 + 1][4 + 2].fg == 0x2980B9); /* int: a type (DataType) */
    puts("PASS highlighting drawn: a string #F44F4F, a type #2980B9, other text the normal colour");

    t->cursor = t->anchor = line_start(d, 1) + 3;
    key('d', KEYS_CTRL);
    CHECK(text_is(d, "f(\"(\", a[1]);\n  // int x;\n  int y;\n")); /* C: after the indentation */
    key('d', KEYS_CTRL | KEYS_SHIFT);
    CHECK(text_is(d, "f(\"(\", a[1]);\n  int x;\n  int y;\n"));
    key('/', KEYS_CTRL);
    key('/', KEYS_CTRL);
    CHECK(text_is(d, "f(\"(\", a[1]);\n  int x;\n  int y;\n"));
    t->anchor = line_start(d, 1);
    t->cursor = line_start(d, 3);
    key('/', KEYS_CTRL); /* lines: at their least indentation, not the line the selection ends at */
    CHECK(text_is(d, "f(\"(\", a[1]);\n  // int x;\n  // int y;\n"));
    key('/', KEYS_CTRL);
    CHECK(text_is(d, "f(\"(\", a[1]);\n  int x;\n  int y;\n"));
    reset();
    t = open_text("c.lua", "  x = 1\n");
    key('d', KEYS_CTRL);
    CHECK(text_is(t->doc, "--   x = 1\n")); /* Lua's mark: at the line's start */
    puts("PASS comments: Ctrl+D, Ctrl+Shift+D, Ctrl+/ toggle, a selection, each language's mark and place");
}

static void test_search_ui(void)
{
    reset();
    struct tab *t = open_text("s.txt", "foo bar foo\nFoo baz\nlast line here\n");
    struct doc *d = t->doc;
    key('f', KEYS_CTRL);
    CHECK(bar_kind == BAR_FIND && strcmp(bar.find.s, "foo") == 0 && bar.find.all);
    keys("baz");
    CHECK(strcmp(bar.find.s, "baz") == 0 && t->anchor == 16 && t->cursor == 19 && bar.match == MATCH_FOUND);
    key('\n', 0);
    CHECK(t->anchor == 16 && bar.match == MATCH_WRAPPED_BOTTOM);
    draw();
    CHECK(on_screen("Search wrapped") >= 0 && on_screen("Reached bottom, continued from top") >= 0);
    key(27, 0);
    CHECK(bar_kind == BAR_NONE);
    t->cursor = t->anchor = 0;
    key('f', KEYS_CTRL);
    key('a', KEYS_CTRL);
    keys("Foo");
    CHECK(t->anchor == 0); /* not matching case: foo */
    bar.item = ITEM_CASE;
    key(' ', 0);
    CHECK(bar.inc_case && t->anchor == 12);
    key(27, 0);
    key(K_F1 + 2, 0); /* F3 */
    CHECK(t->anchor == 12); /* the only Foo, again */
    puts("PASS incremental search (Ctrl+F): the word at the cursor, as typed, Enter, wrapping with Kate's "
         "messages, Match case, Esc, F3");

    t->cursor = t->anchor = 0;
    key('r', KEYS_CTRL);
    CHECK(bar_kind == BAR_POWER && bar.power_case);
    key('a', KEYS_CTRL);
    keys("(f|F)oo");
    key('m', KEYS_ALT);
    key('m', KEYS_ALT);
    key('m', KEYS_ALT);
    CHECK(bar.mode == SEARCH_REGEX);
    key('l', KEYS_ALT);
    keys("<\\U\\1>");
    key('a', KEYS_ALT); /* Replace All */
    CHECK(text_is(d, "<F> bar <F>\n<F> baz\nlast line here\n"));
    draw();
    CHECK(on_screen("3 replacements made") >= 0);
    key('z', KEYS_CTRL);
    CHECK(text_is(d, "foo bar foo\nFoo baz\nlast line here\n"));
    key('d', KEYS_ALT);
    key('a', KEYS_CTRL);
    keys("(");
    key('\n', 0);
    CHECK(bar.match == MATCH_INVALID && !pattern_ok());
    draw();
    CHECK(on_screen("Invalid regular expression: missing closing parenthesis") >= 0);
    key('a', KEYS_CTRL);
    keys("ba");
    key('m', KEYS_ALT);
    key('m', KEYS_ALT); /* regular expression, plain text, whole words */
    key('d', KEYS_ALT); /* the Find field again */
    key('\n', 0);
    CHECK(bar.mode == SEARCH_WORDS && bar.match == MATCH_MISMATCH);
    draw();
    CHECK(on_screen("Not found") >= 0);
    key('m', KEYS_ALT);
    key('m', KEYS_ALT);
    key('m', KEYS_ALT); /* plain text */
    key('d', KEYS_ALT);
    key('a', KEYS_CTRL);
    keys("foo");
    key('l', KEYS_ALT);
    key('a', KEYS_CTRL);
    keys("X");
    t->cursor = t->anchor = 0;
    key('r', KEYS_ALT); /* Replace: finds the first */
    CHECK(t->anchor == 0 && t->cursor == 3 && text_is(d, "foo bar foo\nFoo baz\nlast line here\n"));
    key('r', KEYS_ALT); /* then replaces it and finds the next */
    CHECK(text_is(d, "X bar foo\nFoo baz\nlast line here\n") && t->anchor == 6);
    key('f', KEYS_ALT); /* Find All */
    CHECK(ui_of(d)->nhl == 1);
    draw();
    CHECK(on_screen("1 match found") >= 0);
    puts("PASS power search (Ctrl+R): regular expression, \\U\\1, Replace All as one undo step and its count, "
         "an invalid pattern's error, whole words, Replace, Find All");

    key(27, 0);
    key('g', KEYS_CTRL);
    CHECK(bar_kind == BAR_GOTO && strcmp(bar.line.s, "1") == 0);
    keys("-1");
    key('\n', 0);
    CHECK(bar_kind == BAR_NONE && t->cursor == line_start(d, 3));
    key('g', KEYS_CTRL);
    keys("2");
    key('\n', 0);
    CHECK(t->cursor == line_start(d, 1));
    key('g', KEYS_CTRL);
    keys("0");
    key('\n', 0);
    CHECK(t->cursor == 0);
    puts("PASS go to line (Ctrl+G): N, 0 the first, -1 the last, the cursor at the line's start");
}

static void test_view(void)
{
    reset();
    char text[512] = "ab ";
    for (int i = 0; i < 70; i++) {
        strcat(text, "中");
    }
    strcat(text, "\n    indented ");
    for (int i = 0; i < 30; i++) {
        strcat(text, "words ");
    }
    strcat(text, "\n");
    struct tab *t = open_text("wrap.txt", text);
    int shift, n = layout(t, text_width(0, t), 0, row_buf, MAX_ROWS, &shift);
    /* 132 cells: "ab " and 64 wide characters (131), the 65th on the next row. */
    CHECK(n == 2 && row_buf[1] == 3 + 64 * 3 && shift == 0);
    n = layout(t, text_width(0, t), 1, row_buf, MAX_ROWS, &shift);
    CHECK(n == 2 && shift == 4 && byte_at(t->doc, row_buf[1]) == 'w' && byte_at(t->doc, row_buf[1] - 1) == ' ');
    draw();
    int y0, h;
    text_rows(0, &y0, &h);
    CHECK(grid[y0 + 1][4].cp == 0x4E2D && grid[y0 + 3][4 + 4].cp == 'w'); /* indented as the line is */
    CHECK(grid[y0 + 1][0].cp == ' ' && grid[y0 + 2][2].cp == '2'); /* numbers only on a line's first row */
    t->cursor = t->anchor = 3 + 64 * 3;
    draw();
    CHECK(caret_y == (y0 + 1) * 20 && caret_x == 40);
    t->cursor = t->anchor = 3 + 66 * 3;
    key(K_BOL, 0);
    CHECK(t->cursor == 3 + 64 * 3); /* a continuation row's start */
    key(K_BOL, 0);
    CHECK(t->cursor == 0); /* there already: Smart Home of the line */
    key(K_DOWN, 0);
    CHECK(t->cursor == 3 + 64 * 3);
    key(K_UP, 0);
    CHECK(t->cursor == 0);
    key(K_EOL, 0);
    CHECK(t->cursor == 3 + 63 * 3); /* the row's end, before its last character */
    key(K_EOL, 0);
    CHECK(t->cursor == line_end(t->doc, 0));
    puts("PASS dynamic word wrap: CJK and words, continuation rows indented (80%), Home/End by rows, the cursor's "
         "cell on the wrapped row");

    key(K_F1 + 10, 0); /* F11 */
    draw();
    /* Without line numbers: the folding markers' column and, the document
     * having a file, the line modification markers' (Line Modification). */
    CHECK(!t->numbers && gutter(t) == 2 && grid[y0][2].cp == 'a');
    key(K_F1 + 10, 0);
    key(K_F1 + 9, 0); /* F10 */
    CHECK(!t->wrap && line_rows(t, text_width(0, t), 0) == 1);
    key(K_F1 + 9, 0);
    draw();
    CHECK(on_screen("INSERT") == ROWS - 1 && on_screen("Soft Tabs: 4") == ROWS - 1 && on_screen("UTF-8") == ROWS - 1);
    CHECK(on_screen(" LF ") == ROWS - 1 && on_screen("Normal") == ROWS - 1);
    key(K_INS, 0);
    draw();
    CHECK(on_screen("OVERWRITE") == ROWS - 1 && t->doc->overwrite);
    t->cursor = t->anchor = 0;
    keys("XY");
    CHECK(byte_at(t->doc, 0) == 'X' && byte_at(t->doc, 1) == 'Y' && byte_at(t->doc, 2) == ' ');
    key(K_INS, 0);
    puts("PASS F11 line numbers, F10 dynamic word wrap, the status bar (line:column, INSERT/OVERWRITE, tabs, "
         "encoding, line ends, mode), overwrite typing");
}

static void test_tabs(void)
{
    reset();
    key('n', KEYS_CTRL);
    struct tab *t = cur_tab();
    CHECK(t && ndocs == 1);
    keys("hello world");
    draw();
    CHECK(on_screen("*Untitled (hello world)") == space_rect(active).y);
    key('n', KEYS_CTRL);
    CHECK(spaces[0].ntabs == 2 && spaces[0].cur == 1);
    draw();
    CHECK(on_screen(" Untitled ") == space_rect(active).y);
    key(K_LEFT, KEYS_ALT);
    CHECK(spaces[0].cur == 0);
    key(K_PGDN, KEYS_CTRL);
    CHECK(spaces[0].cur == 1);
    key(K_RIGHT, KEYS_ALT);
    CHECK(spaces[0].cur == 0); /* round to the first */
    key('2', KEYS_ALT);
    CHECK(spaces[0].cur == 1);
    key('1', KEYS_ALT);
    push('c', 0, NULL); /* Cancel */
    key('w', KEYS_CTRL);
    CHECK(ndocs == 2 && qhead == qtail);
    push('d', 0, NULL); /* Discard */
    key('w', KEYS_CTRL);
    CHECK(ndocs == 1 && spaces[0].ntabs == 1);
    puts("PASS tabs: Ctrl+N Untitled (named by its first line), Alt+Left/Right and Ctrl+PgUp/PgDn round, Alt+N, "
         "Ctrl+W asks for a modified one (Cancel keeps it, Discard closes)");

    /* Save As of an Untitled one: a folder, then its name. */
    CHECK(mkdir(at_dir("Documents"), 0755) == 0);
    if (is_root) {
        CHECK(chown(at_dir("Documents"), EDIT_USER, EDIT_USER) == 0);
    }
    t = cur_tab();
    keys("saved text");
    snprintf(pick_result, sizeof pick_result, "%s", at_dir("Documents"));
    for (const char *p = "new.txt"; *p; p++) {
        push(*p, 0, NULL);
    }
    push('\n', 0, NULL);
    key('s', KEYS_CTRL);
    CHECK(qhead == qtail && strcmp(pick_type, "inode/directory") == 0);
    CHECK(strcmp(t->doc->path, at_dir("Documents/new.txt")) == 0 && !doc_modified(t->doc));
    CHECK(strcmp(file_bytes(at_dir("Documents/new.txt")), "saved text\n") == 0); /* Newline at End of File */
    struct stat st;
    CHECK(stat(at_dir("Documents/new.txt"), &st) == 0 && (!is_root || st.st_uid == EDIT_USER));
    draw();
    CHECK(on_screen(" new.txt ") == space_rect(active).y);
    keys("!");
    push('\n', 0, NULL); /* the name suggested: its own */
    push('o', 0, NULL); /* Overwrite */
    key('s', KEYS_CTRL | KEYS_SHIFT);
    CHECK(qhead == qtail && strcmp(file_bytes(at_dir("Documents/new.txt")), "saved text\n!\n") == 0);
    puts("PASS saving: Ctrl+S on Untitled is Save As (a folder, then the name), the final line break, the user's; "
         "Ctrl+Shift+S asks before overwriting");

    keys("x");
    push('s', 0, NULL); /* Save, closing */
    key('w', KEYS_CTRL);
    CHECK(ndocs == 0 && strcmp(file_bytes(at_dir("Documents/new.txt")), "saved text\n!\nx\n") == 0);
    draw();
    CHECK(on_screen("No document is open.") >= 0 && on_screen("[New File]") >= 0);
    key('n', KEYS_CTRL);
    CHECK(ndocs == 1);
    struct doc *untitled = cur_tab()->doc;
    CHECK(open_file(at_dir("Documents/new.txt")) && ndocs == 1 && cur_tab()->doc != untitled);
    puts("PASS closing: Save then close; the last one closed shows the welcome view; an unmodified Untitled is "
         "replaced by a file opened");
}

static void test_split(void)
{
    reset();
    struct tab *t = open_text("split.txt", "one\ntwo\nthree\nfour\n");
    struct doc *d = t->doc;
    key('l', KEYS_CTRL | KEYS_SHIFT);
    CHECK(nspaces == 2 && active == 1 && tab_of(1)->doc == d && tab_of(0)->doc == d);
    key(K_DOWN, 0);
    key(K_DOWN, 0);
    CHECK(tab_of(1)->cursor == 8 && tab_of(0)->cursor == 0);
    keys("X");
    CHECK(text_is(d, "one\ntwo\nXthree\nfour\n") && tab_of(0)->cursor == 0);
    key(K_F1 + 7, 0); /* F8 */
    CHECK(active == 0);
    keys("Y");
    CHECK(tab_of(0)->cursor == 1 && tab_of(1)->cursor == 10); /* the other view's cursor moves with the text */
    draw();
    int x1, w1, x0, w0;
    space_cols(0, &x0, &w0);
    space_cols(1, &x1, &w1);
    int y, h;
    text_rows(0, &y, &h);
    CHECK(x0 == 0 && x1 == w0 + 1 && grid[y][x0 + 4].cp == 'Y' && grid[y][x1 + 4].cp == 'Y');
    int asked = picks;
    key('o', KEYS_CTRL);
    CHECK(picks == asked + 1 && strcmp(pick_type, "") == 0); /* the open dialog: any file (nothing chosen) */
    key('r', KEYS_CTRL | KEYS_SHIFT); /* Close Current View */
    CHECK(nspaces == 1 && ndocs == 1);
    puts("PASS vertical split (Ctrl+Shift+L): the same document, cursors of their own, F8, Ctrl+Shift+R");
}

static void sh(const char *cmd)
{
    CHECK(system(cmd) == 0);
}

static void test_tree(void)
{
    reset();
    char cmd[2048];
    CHECK(mkdir(at_dir("proj"), 0755) == 0 && mkdir(at_dir("proj/src"), 0755) == 0);
    put_file(at_dir("proj/a.c"), "int a;\n");
    put_file(at_dir("proj/src/b.c"), "int b;\n");
    put_file(at_dir("proj/.gitignore"), "*.o\n");
    put_file(at_dir("proj/x.o"), "obj\n");
    snprintf(cmd, sizeof cmd, "cd %s && git init -q && git add a.c src/b.c .gitignore && "
             "git -c user.name=t -c user.email=t@t commit -qm one", at_dir("proj"));
    sh(cmd);
    put_file(at_dir("proj/c.txt"), "new\n");
    if (is_root) {
        snprintf(cmd, sizeof cmd, "chown -R %d:%d %s", EDIT_USER, EDIT_USER, at_dir("proj"));
        sh(cmd);
    }
    CHECK(open_file(at_dir("proj/src/b.c")));
    key('f', KEYS_CTRL | KEYS_ALT | KEYS_SHIFT);
    CHECK(sidebar && tree_focus && project && project->git && strcmp(project->root, at_dir("proj")) == 0);
    int n = tree_rows(project, tree_nodes, tree_depth, 8192);
    const char *want[] = { "src", "b.c", ".gitignore", "a.c", "c.txt" };
    CHECK(n == 5);
    for (int i = 0; i < n; i++) {
        CHECK(strcmp(tree_nodes[i]->name, want[i]) == 0);
    }
    CHECK(tree_sel == 1);
    draw();
    CHECK(on_screen("Projects: proj") >= 1 && on_screen("- src") == on_screen("Projects: proj") + 1);
    key(K_DOWN, 0);
    key(K_DOWN, 0);
    key('\n', 0);
    CHECK(strcmp(cur_tab()->doc->path, at_dir("proj/a.c")) == 0);
    click(5, on_screen("Projects: proj") + 1 + 4, BTN_LEFT, 1, 1); /* c.txt: a click opens it */
    CHECK(strcmp(cur_tab()->doc->path, at_dir("proj/c.txt")) == 0);
    puts("PASS Projects: the nearest .git, git's files (tracked and new, not ignored), folders first, the file "
         "selected; Enter and a click open");

    CHECK(mkdir(at_dir("plain"), 0755) == 0 && mkdir(at_dir("plain/sub"), 0755) == 0);
    put_file(at_dir("plain/z.txt"), "z\n");
    put_file(at_dir("plain/.hidden"), "h\n");
    put_file(at_dir("plain/sub/y.txt"), "y\n");
    CHECK(open_file(at_dir("plain/z.txt")));
    CHECK(project && !project->git && strcmp(project->root, at_dir("plain")) == 0);
    n = tree_rows(project, tree_nodes, tree_depth, 8192);
    CHECK(n == 2 && strcmp(tree_nodes[0]->name, "sub") == 0 && !tree_nodes[0]->expanded &&
          strcmp(tree_nodes[1]->name, "z.txt") == 0);
    tree_sel = 0;
    key(K_RIGHT, 0);
    CHECK(tree_rows(project, tree_nodes, tree_depth, 8192) == 3);
    key(27, 0);
    CHECK(!tree_focus && sidebar);
    key('f', KEYS_CTRL | KEYS_ALT | KEYS_SHIFT);
    CHECK(!sidebar);
    puts("PASS Projects without version control: the file's folder, hidden files left out, folders read when "
         "opened; Esc back to the text, Ctrl+Alt+Shift+F");
}

static void test_disk(void)
{
    reset();
    if (ino < 0) {
        ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    }
    struct tab *t = open_text("ext.txt", "one\n");
    struct doc *d = t->doc;
    put_file(at_dir("ext.txt"), "two\n");
    disk_events();
    CHECK(d->disk == DISK_MODIFIED && ui_of(d)->nnotes == 1);
    draw();
    CHECK(on_screen("was modified on disk.") >= 0 && on_screen("[Reload]") >= 0 && on_screen("[Ignore]") >= 0);
    CHECK(on_screen("[Enable Auto Reload]") >= 0);
    int row = on_screen("[Reload]");
    char line[COLS * 4 + 1];
    row_text(row, line, sizeof line);
    int col = (int)(strstr(line, "[Reload]") - line);
    click(col + 1, row, BTN_LEFT, 1, 1);
    CHECK(text_is(d, "two\n") && d->disk == DISK_NONE && ui_of(d)->nnotes == 0);
    puts("PASS changed on disk: Kate's message and buttons (Enable Auto Reload, Reload, Ignore), Reload gives the "
         "new text");

    keys("x");
    key('s', KEYS_CTRL);
    disk_events();
    CHECK(d->disk == DISK_NONE && ui_of(d)->nnotes == 0);
    put_file(at_dir("ext.txt"), "xtwo\n"); /* the same bytes again: no change */
    disk_events();
    CHECK(d->disk == DISK_NONE && ui_of(d)->nnotes == 0);
    keys("y");
    put_file(at_dir("ext.txt"), "other\n");
    disk_events();
    CHECK(d->disk == DISK_MODIFIED && ui_of(d)->nnotes == 1);
    note_action(d, 0, ACT_IGNORE);
    put_file(at_dir("ext.txt"), "other again\n");
    disk_events();
    CHECK(ui_of(d)->nnotes == 0); /* not asked again for the same */
    puts("PASS its own save, and the same bytes written again, ask nothing; Ignore is not asked again");

    CHECK(unlink(at_dir("ext.txt")) == 0);
    disk_events();
    CHECK(d->disk == DISK_DELETED);
    draw();
    CHECK(on_screen("was deleted or moved on disk.") >= 0 && on_screen("[Close File]") >= 0 &&
          on_screen("[Save As...]") >= 0);
    push('d', 0, NULL); /* modified: Discard */
    note_action(d, 0, ACT_CLOSE_FILE);
    CHECK(ndocs == 0);
    puts("PASS deleted on disk: Kate's message and buttons (Close File, Save As, Ignore); Close File closes it");

    /* In git, unmodified, its old text committed: reloaded without asking. */
    char cmd[1024];
    CHECK(mkdir(at_dir("repo"), 0755) == 0);
    put_file(at_dir("repo/f.txt"), "v1\n");
    snprintf(cmd, sizeof cmd, "cd %s && git init -q && git add f.txt && git -c user.name=t -c user.email=t@t "
             "commit -qm one%s%s", at_dir("repo"), is_root ? " && chown -R 1000:1000 " : "",
             is_root ? at_dir("repo") : "");
    sh(cmd);
    t = open_text("repo/f.txt", "v1\n");
    put_file(at_dir("repo/f.txt"), "v2\n");
    disk_events();
    CHECK(text_is(t->doc, "v2\n") && ui_of(t->doc)->nnotes == 0);
    puts("PASS Auto Reload If State Is In Version Control: git has the old text, so it is reloaded at once");
}

static void test_ime_mouse(void)
{
    reset();
    struct tab *t = open_text("ime.txt", "ab\n");
    key(K_RIGHT, 0);
    key(K_CTRL_SPACE, KEYS_CTRL);
    CHECK(ime_now == IME_MODE_CN);
    keys("ni");
    draw();
    CHECK(on_screen("[CN] ni") == ROWS - 1 && text_is(t->doc, "ab\n"));
    keys(" ");
    CHECK(text_is(t->doc, "a你好b\n") && t->cursor == 7);
    key(K_CTRL_SPACE, KEYS_CTRL);
    key(K_CTRL_SPACE, KEYS_CTRL);
    CHECK(ime_now == IME_MODE_RAW);
    puts("PASS input method: Ctrl+Space cycles it, its candidates shown, the committed text typed at the cursor");

    reset();
    t = open_text("mouse.txt", "hello world foo\nline two\n");
    draw();
    int y0, h;
    text_rows(0, &y0, &h);
    click(4 + 7, y0, BTN_LEFT, 1, 2); /* in "world" */
    CHECK(t->anchor == 6 && t->cursor == 11 && strcmp(clip[1], "world") == 0);
    click(4 + 7, y0, BTN_LEFT, 0, 0);
    click(4 + 7, y0, BTN_LEFT, 1, 3);
    CHECK(t->anchor == 0 && t->cursor == 16 && strcmp(clip[1], "hello world foo\n") == 0);
    click(4 + 7, y0, BTN_LEFT, 0, 0);
    click(4 + 6, y0, BTN_LEFT, 1, 1);
    CHECK(t->cursor == 6 && !has_sel(t));
    ptr_now = (struct pointer_event){ .x = (4 + 14) * 10 + 2, .col = 4 + 14, .row = y0, .y = y0 * 20 + 5,
                                      .buttons = 1 };
    key_event(K_POINTER);
    click(4 + 14, y0, BTN_LEFT, 0, 0);
    CHECK(t->anchor == 6 && t->cursor == 14 && strcmp(clip[1], "world fo") == 0);
    puts("PASS pointer: a double click a word, a triple one the line with its break, a drag; each the primary "
         "selection");

    click(4 + 2, y0 + 1, BTN_MIDDLE, 1, 1);
    click(4 + 2, y0 + 1, BTN_MIDDLE, 0, 0);
    CHECK(text_is(t->doc, "hello world foo\nliworld fone two\n"));
    key('c', KEYS_CTRL); /* no selection: the line */
    CHECK(strcmp(clip[0], "liworld fone two\n") == 0);
    t->cursor = t->anchor = 0;
    int before = clip_sets[1];
    key(K_RIGHT, KEYS_SHIFT);
    key(K_RIGHT, KEYS_SHIFT);
    CHECK(clip_sets[1] == before); /* not yet: while Shift is held */
    key(K_EOL, 0);
    CHECK(clip_sets[1] == before + 1 && strcmp(clip[1], "he") == 0);
    t->cursor = 0;
    t->anchor = 5;
    key('x', KEYS_CTRL);
    CHECK(strcmp(clip[0], "hello") == 0 && text_is(t->doc, " world foo\nliworld fone two\n"));
    key(K_EOL, 0);
    key('v', KEYS_CTRL);
    CHECK(text_is(t->doc, " world foohello\nliworld fone two\n"));
    key('z', KEYS_CTRL);
    CHECK(text_is(t->doc, " world foo\nliworld fone two\n")); /* a paste is a step of its own */
    puts("PASS clipboard: the middle button pastes the selection where clicked, Ctrl+C a line, Shift+arrows "
         "the primary selection after, Ctrl+X and Ctrl+V");

    ptr_now = (struct pointer_event){ .col = 10, .row = y0 + 2, .wheel = -1 };
    key_event(K_POINTER);
    CHECK(t->sub == 0 && t->top == 0); /* nothing below to scroll to */
}

static void test_notes(void)
{
    reset();
    FILE *f = fopen(at_dir("bin2.dat"), "w");
    fwrite("ab\0cd\n", 1, 6, f);
    fclose(f);
    CHECK(!is_root || chown(at_dir("bin2.dat"), EDIT_USER, EDIT_USER) == 0);
    CHECK(open_file(at_dir("bin2.dat")));
    struct tab *t = cur_tab();
    /* As KTextEditor: NUL bytes are characters like others, no message, not
     * read-only, and saved back as they were. */
    CHECK(!t->doc->readonly && ui_of(t->doc)->nnotes == 0 && doc_len(t->doc) == 6);
    key(K_EOL, 0);
    keys("e");
    key('s', KEYS_CTRL);
    FILE *back = fopen(at_dir("bin2.dat"), "rb");
    char bytes[16];
    CHECK(back && fread(bytes, 1, sizeof bytes, back) == 7 && memcmp(bytes, "ab\0cde\n", 7) == 0);
    fclose(back);
    char *line = malloc(30001);
    memset(line, 'q', 30000);
    line[30000] = '\0';
    put_file(at_dir("long.txt"), line);
    free(line);
    CHECK(open_file(at_dir("long.txt")));
    t = cur_tab();
    CHECK(t->doc->readonly && t->doc->buf.lines == 3);
    draw();
    CHECK(on_screen("Line Length") >= 0 && on_screen("[Temporarily raise limit and reload file]") >= 0);
    keys("z");
    CHECK(byte_at(t->doc, 0) == 'q'); /* read-only */
    note_action(t->doc, 0, ACT_RAISE_LIMIT);
    CHECK(!t->doc->readonly && t->doc->buf.lines == 1 && ui_of(t->doc)->limit == 32768);
    if (is_root) {
        put_file(at_dir("secret"), "s\n");
        CHECK(chown(at_dir("secret"), 0, 0) == 0 && chmod(at_dir("secret"), 0600) == 0);
        CHECK(open_file(at_dir("secret")));
        draw();
        CHECK(on_screen("could not be loaded") >= 0 && doc_len(cur_tab()->doc) == 0);
        keys("pwned");
        push('o', 0, NULL); /* the error's OK */
        key('s', KEYS_CTRL);
        CHECK(strcmp(file_bytes(at_dir("secret")), "s\n") == 0);
        puts("PASS a root-only file: Kate's could-not-be-loaded message, and saving over it refused with Kate's "
             "write error");
    }
    puts("PASS notes: NUL bytes nothing special (edited, saved back), the Line Length Limit's wrap, read-only, "
         "raised and reloaded");
}

/* Audit regressions drive the real editor's key, pointer and drawing paths,
 * with only its devices stubbed above. Each case also runs independently so
 * the original failures do not hide the later ones. */
static void test_correctness(const char *which)
{
    reset();
    if (strcmp(which, "horizontal") == 0) {
        char line[401];
        memset(line, 'a', 400);
        line[400] = '\0';
        struct tab *t = open_text("horizontal.txt", line);
        key(K_F1 + 9, 0);
        key(K_EOL, 0);
        draw();
        int y, h;
        text_rows(active, &y, &h);
        CHECK(!t->wrap && t->left > 0);
        CHECK(grid[y][gutter(t)].cp == 'a');
        CHECK(grid[y][gutter(t) + 10].cp == 'a');
    } else if (strcmp(which, "tree-wheel") == 0) {
        CHECK(mkdir(at_dir("wheel"), 0755) == 0);
        if (is_root) CHECK(chown(at_dir("wheel"), EDIT_USER, EDIT_USER) == 0);
        for (int i = 0; i < 80; i++) {
            char rel[80];
            snprintf(rel, sizeof rel, "wheel/f%02d.txt", i);
            put_file(at_dir(rel), "x\n");
        }
        CHECK(open_file(at_dir("wheel/f00.txt")));
        key('f', KEYS_CTRL | KEYS_ALT | KEYS_SHIFT);
        draw();
        ptr_now = (struct pointer_event){ .col = 3, .row = 4, .wheel = -2 };
        key_event(K_POINTER);
        draw();
        CHECK(tree_top == 6 && on_screen("f06.txt") == on_screen("Projects: wheel") + 1);
        key(K_DOWN, 0);
        draw();
        CHECK(tree_sel == 1 && tree_top <= tree_sel);
    } else if (strcmp(which, "tree-refresh") == 0) {
        CHECK(mkdir(at_dir("refresh"), 0755) == 0);
        if (is_root) CHECK(chown(at_dir("refresh"), EDIT_USER, EDIT_USER) == 0);
        open_text("refresh/a.txt", "a\n");
        key('f', KEYS_CTRL | KEYS_ALT | KEYS_SHIFT);
        draw();
        put_file(at_dir("refresh/external.txt"), "new\n");
        step(); /* the real event-loop poll, not a helper-only refresh */
        draw();
        CHECK(on_screen("external.txt") >= 0);
        CHECK(unlink(at_dir("refresh/external.txt")) == 0);
        disk_events();
        draw();
        CHECK(on_screen("external.txt") < 0);
        snprintf(pick_result, sizeof pick_result, "%s", at_dir("refresh"));
        for (const char *p = "saved.txt"; *p; p++) push(*p, 0, NULL);
        push('\n', 0, NULL);
        tree_focus = 0;
        key('s', KEYS_CTRL | KEYS_SHIFT);
        draw();
        CHECK(on_screen("saved.txt") >= 0 && tree_sel >= 0 &&
              strcmp(tree_nodes[tree_sel]->name, "saved.txt") == 0);
    } else if (strcmp(which, "tree-git-refresh") == 0) {
        char cmd[2048];
        CHECK(mkdir(at_dir("git-refresh"), 0755) == 0);
        CHECK(mkdir(at_dir("git-refresh/sub"), 0755) == 0);
        put_file(at_dir("git-refresh/sub/a.txt"), "a\n");
        snprintf(cmd, sizeof cmd, "cd %s && git init -q && git add sub/a.txt%s%s",
                 at_dir("git-refresh"), is_root ? " && chown -R 1000:1000 " : "",
                 is_root ? at_dir("git-refresh") : "");
        sh(cmd);
        CHECK(open_file(at_dir("git-refresh/sub/a.txt")));
        key('f', KEYS_CTRL | KEYS_ALT | KEYS_SHIFT);
        draw();
        CHECK(project->git && on_screen("- sub") >= 0);
        put_file(at_dir("git-refresh/sub/b.txt"), "b\n");
        step();
        draw();
        CHECK(on_screen("b.txt") >= 0 && on_screen("- sub") >= 0);
        CHECK(unlink(at_dir("git-refresh/sub/b.txt")) == 0);
        step();
        draw();
        CHECK(on_screen("b.txt") < 0 && on_screen("- sub") >= 0);
        CHECK(mkdir(at_dir("git-refresh/newdir"), 0755) == 0);
        if (is_root) CHECK(chown(at_dir("git-refresh/newdir"), EDIT_USER, EDIT_USER) == 0);
        step();
        put_file(at_dir("git-refresh/newdir/new.txt"), "new\n");
        step();
        draw();
        CHECK(on_screen("newdir") >= 0);
    } else if (strcmp(which, "tree-empty") == 0) {
        CHECK(mkdir(at_dir("empty-tree"), 0755) == 0);
        CHECK(open_file(at_dir("empty-tree/missing.txt")));
        key('f', KEYS_CTRL | KEYS_ALT | KEYS_SHIFT);
        draw();
        CHECK(tree_sel == -1);
        key(K_RIGHT, 0);
        key(K_LEFT, 0);
        key(K_DOWN, 0);
        draw();
        CHECK(tree_sel == -1 && tree_top == 0);
    } else if (strcmp(which, "selection-scope") == 0) {
        struct tab *t = open_text("selection-scope.txt", "foo outside\nfoo one\nfoo two\nfoo outside\n");
        t->anchor = 12;
        t->cursor = 28;
        key('r', KEYS_CTRL);
        CHECK(bar.selection_only);
        key('a', KEYS_CTRL);
        keys("foo");
        key('\n', 0);
        CHECK(t->anchor == 12 && t->cursor == 15);
        key('\n', 0);
        CHECK(t->anchor == 20 && t->cursor == 23);
        t->cursor = t->anchor = 0; /* scope survives deselection and navigation */
        key('f', KEYS_ALT);
        CHECK(ui_of(t->doc)->nhl == 2);
        key('l', KEYS_ALT);
        keys("X");
        key('a', KEYS_ALT);
        CHECK(text_is(t->doc, "foo outside\nX one\nX two\nfoo outside\n"));
    } else if (strcmp(which, "encoding-controls") == 0) {
        open_text("bad-encoding.txt", "bad \xff\n");
        struct tab *t = cur_tab();
        CHECK(t->doc->readonly && (t->doc->flags & FILE_BROKEN));
        draw();
        CHECK(on_screen("[Reopen with Encoding...]") >= 0);
        CHECK(on_screen("[Enable Read-Write Mode]") >= 0);
        int row = on_screen("[Reopen with Encoding...]");
        char visible[COLS * 4 + 1];
        row_text(row, visible, sizeof visible);
        int col = (int)(strstr(visible, "[Reopen with Encoding...]") - visible);
        push(27, 0, NULL); /* cancelling must not change bytes or protection */
        click(col + 2, row, BTN_LEFT, 1, 1);
        CHECK(t->doc->readonly && (t->doc->flags & FILE_BROKEN));
        for (const char *p = "ISO-8859-1"; *p; p++) push(*p, 0, NULL);
        push('\n', 0, NULL);
        click(col + 2, row, BTN_LEFT, 1, 1);
        CHECK(!t->doc->readonly && !t->doc->flags && t->doc->format.encoding == ENC_LATIN1);
        CHECK(text_is(t->doc, "bad \xc3\xbf\n"));
        key('s', KEYS_CTRL);
        CHECK(strcmp(file_bytes(at_dir("bad-encoding.txt")), "bad \xff\n") == 0);
        keys("X");
        for (const char *p = "UTF-8"; *p; p++) push(*p, 0, NULL);
        push('\n', 0, NULL);
        push(27, 0, NULL); /* unsaved-change confirmation is cancellable */
        reopen_encoding(t->doc);
        CHECK(text_is(t->doc, "Xbad \xc3\xbf\n") && t->doc->format.encoding == ENC_LATIN1);
        CHECK(doc_modified(t->doc));
        key('z', KEYS_CTRL);
        CHECK(text_is(t->doc, "bad \xc3\xbf\n"));
        draw();
        CHECK(on_screen("ISO-8859-1") >= 0);
        CHECK(open_file(at_dir("bad-encoding-again.txt")));
        put_file(at_dir("bad-encoding-again.txt"), "bad \xff\n");
        reload(cur_tab()->doc);
        t = cur_tab();
        draw();
        row = on_screen("[Enable Read-Write Mode]");
        row_text(row, visible, sizeof visible);
        col = (int)(strstr(visible, "[Enable Read-Write Mode]") - visible);
        push(27, 0, NULL);
        click(col + 2, row, BTN_LEFT, 1, 1);
        CHECK(t->doc->readonly);
        draw(); /* restore warning hit targets after the cancelled dialog */
        push('w', 0, NULL); /* explicit Enable Read-Write confirmation */
        click(col + 2, row, BTN_LEFT, 1, 1);
        CHECK(!t->doc->readonly);
        keys("X");
        CHECK(byte_at(t->doc, 0) == 'X');
        CHECK(strcmp(file_bytes(at_dir("bad-encoding-again.txt")), "bad \xff\n") == 0);
    } else if (strcmp(which, "reload-safety") == 0) {
        struct tab *t = open_text("reload-failure.txt", "original\n");
        keys("X");
        CHECK(unlink(at_dir("reload-failure.txt")) == 0);
        CHECK(mkdir(at_dir("reload-failure.txt"), 0755) == 0); /* load must fail, not erase the buffer */
        reload(t->doc);
        CHECK(text_is(t->doc, "Xoriginal\n") && doc_modified(t->doc));
        key('z', KEYS_CTRL);
        CHECK(text_is(t->doc, "original\n"));
    } else if (strcmp(which, "readonly-save-as") == 0) {
        struct tab *t = open_text("readonly-original.txt", "original\n");
        t->doc->readonly = 1;
        snprintf(pick_result, sizeof pick_result, "%s", test_dir);
        for (const char *p = "readonly-copy.txt"; *p; p++) push(*p, 0, NULL);
        push('\n', 0, NULL);
        key('s', KEYS_CTRL | KEYS_SHIFT);
        CHECK(qhead == qtail && strcmp(t->doc->path, at_dir("readonly-copy.txt")) == 0);
        CHECK(strcmp(file_bytes(at_dir("readonly-original.txt")), "original\n") == 0);
        CHECK(strcmp(file_bytes(at_dir("readonly-copy.txt")), "original\n") == 0);
        if (is_root) {
            put_file(at_dir("readonly-denied.txt"), "protected\n");
            CHECK(chown(at_dir("readonly-denied.txt"), 0, 0) == 0);
            CHECK(chmod(at_dir("readonly-denied.txt"), 0600) == 0);
            for (const char *p = "readonly-denied.txt"; *p; p++) push(*p, 0, NULL);
            push('\n', 0, NULL);
            push('o', 0, NULL); /* overwrite question */
            push('o', 0, NULL); /* write error's OK */
            key('s', KEYS_CTRL | KEYS_SHIFT);
            CHECK(qhead == qtail && t->doc->readonly);
            CHECK(strcmp(t->doc->path, at_dir("readonly-copy.txt")) == 0);
            CHECK(strcmp(file_bytes(at_dir("readonly-denied.txt")), "protected\n") == 0);
        }
    } else if (strcmp(which, "save-as-open-modified") == 0) {
        struct tab *a = open_text("save-as-target.txt", "target\n");
        struct doc *target = a->doc;
        keys("X");
        struct doc *source = open_text("save-as-source.txt", "source\n")->doc;
        snprintf(pick_result, sizeof pick_result, "%s", test_dir);
        for (const char *p = "save-as-target.txt"; *p; p++) push(*p, 0, NULL);
        push('\n', 0, NULL);
        push(K_LEFT, 0, NULL); push('\n', 0, NULL); /* Close Other && Save */
        push('c', 0, NULL); /* the other document's own Save/Discard/Cancel: Cancel */
        key('s', KEYS_CTRL | KEYS_SHIFT);
        CHECK(qhead == qtail && doc_for_path(at_dir("save-as-target.txt")) == target);
        CHECK(doc_modified(target) && text_is(target, "Xtarget\n"));
        CHECK(strcmp(source->path, at_dir("save-as-source.txt")) == 0);
        CHECK(strcmp(file_bytes(at_dir("save-as-target.txt")), "target\n") == 0);
        for (const char *p = "save-as-target.txt"; *p; p++) push(*p, 0, NULL);
        push('\n', 0, NULL);
        push(K_LEFT, 0, NULL); push('\n', 0, NULL);
        push('d', 0, NULL); /* Discard the other document's changes; Close Other is the overwrite consent */
        key('s', KEYS_CTRL | KEYS_SHIFT);
        CHECK(qhead == qtail && ndocs == 1 && doc_for_path(at_dir("save-as-target.txt")) == source);
        CHECK(strcmp(file_bytes(at_dir("save-as-target.txt")), "source\n") == 0);
    } else if (strcmp(which, "selection-no-scope") == 0) {
        struct tab *t = open_text("selection-no-scope.txt", "one\ntwo\n");
        key('r', KEYS_CTRL);
        for (int i = 0; i < 4 && bar.mode != SEARCH_REGEX; i++) key('m', KEYS_ALT);
        CHECK(bar.mode == SEARCH_REGEX);
        bar.selection_only = 1; /* toggled with nothing selected: no saved range */
        ui_of(t->doc)->scope_valid = 0;
        key('d', KEYS_ALT); /* the Find field again */
        key('a', KEYS_CTRL);
        keys("^");
        CHECK(strcmp(bar.find.s, "^") == 0);
        key('\n', 0);
        CHECK(bar.match == MATCH_MISMATCH && t->cursor == 0 && t->anchor == 0);
        key('f', KEYS_ALT);
        CHECK(ui_of(t->doc)->nhl == 0 && bar.match == MATCH_MISMATCH);
        key('l', KEYS_ALT);
        keys("X");
        key('a', KEYS_ALT);
        CHECK(text_is(t->doc, "one\ntwo\n") && !doc_modified(t->doc) && bar.match == MATCH_MISMATCH);
    } else {
        CHECK(!"unknown correctness case");
    }
    printf("PASS audit regression: %s\n", which);
}

static void test_quit(void)
{
    reset();
    struct tab *t = open_text("q1.txt", "one\n");
    keys("x");
    open_text("q2.txt", "two\n");
    keys("y");
    push('c', 0, NULL);
    key('q', KEYS_CTRL);
    CHECK(!quitting && qhead == qtail);
    draw();
    push('d', 0, NULL);
    key('q', KEYS_CTRL);
    CHECK(quitting && strcmp(file_bytes(at_dir("q1.txt")), "one\n") == 0);
    quitting = 0;
    reset();
    t = open_text("q3.txt", "three\n");
    keys("z");
    put_file(at_dir("q3.txt"), "THREE\n");
    disk_events();
    push('o', 0, NULL); /* Documents Modified on Disk: Overwrite */
    key('q', KEYS_CTRL);
    CHECK(quitting && strcmp(file_bytes(at_dir("q3.txt")), "zthree\n") == 0 && !doc_modified(t->doc));
    quitting = 0;
    puts("PASS Ctrl+Q: Kate's Save Documents question (Cancel stays, Discard quits), and Documents Modified on "
         "Disk first (Overwrite)");
}

static void test_large(void)
{
    reset();
    const char *path = at_dir("big.txt");
    FILE *f = fopen(path, "w");
    CHECK(f);
    size_t lines = 2000000;
    for (size_t i = 0; i < lines - 1; i++) {
        fprintf(f, "line %08zu the quick brown fox jumps over it\n", i);
    }
    fprintf(f, "the last line has the needle-at-the-end in it\n");
    CHECK(fclose(f) == 0);
    struct stat st;
    CHECK(stat(path, &st) == 0);
    if (is_root) {
        CHECK(chown(path, EDIT_USER, EDIT_USER) == 0);
    }
    long rss0 = status_kib("VmRSS:");
    double t0 = ms();
    CHECK(open_file(path));
    draw();
    double open_ms = ms() - t0;
    struct tab *t = cur_tab();
    CHECK(t->doc->buf.lines == lines + 1);
    t0 = ms();
    key(K_EOL, KEYS_CTRL);
    draw();
    double end_ms = ms() - t0;
    CHECK(line_of(t->doc, t->cursor) == lines);
    key(K_UP, 0);
    t0 = ms();
    key(K_BOL, KEYS_CTRL);
    key('f', KEYS_CTRL);
    key('a', KEYS_CTRL);
    snprintf(clip[0], sizeof clip[0], "needle-at-the-end");
    clip_len[0] = strlen(clip[0]);
    key('v', KEYS_CTRL); /* the pattern at once, one search */
    draw();
    double find_ms = ms() - t0;
    CHECK(bar.match == MATCH_FOUND && line_of(t->doc, t->cursor) == lines - 1);
    key(27, 0);
    key(K_EOL, 0);
    keys("!");
    t0 = ms();
    key('s', KEYS_CTRL);
    double save_ms = ms() - t0;
    CHECK(!doc_modified(t->doc));
    struct stat after;
    CHECK(stat(path, &after) == 0 && after.st_size == st.st_size + 1);
    printf("  %zu lines, %.1f MB: open %.0f ms, Ctrl+End %.0f ms, search to the last line %.0f ms, save %.0f ms\n",
           lines, (double)st.st_size / 1e6, open_ms, end_ms, find_ms, save_ms);
    printf("  memory: VmRSS %ld KiB before, %ld KiB with it open; VmHWM %ld KiB\n", rss0, status_kib("VmRSS:"),
           status_kib("VmHWM:"));
    CHECK(status_kib("VmRSS:") - rss0 < 400 * 1024);
    puts("PASS a large file: opened, scrolled to its end, searched and saved (the numbers above)");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 || argc == 3);
    setlocale(LC_CTYPE, "C.UTF-8");
    is_root = geteuid() == 0;
    CHECK(mkdir(argv[1], 0755) == 0);
    char *real = realpath(argv[1], NULL);
    snprintf(test_dir, sizeof test_dir, "%s", real);
    free(real);
    if (is_root) {
        CHECK(chown(test_dir, EDIT_USER, EDIT_USER) == 0);
    }
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (argc == 3) {
        test_correctness(argv[2]);
        return 0;
    }
    test_cursor();
    test_indent();
    test_brackets_comments();
    test_search_ui();
    test_view();
    test_tabs();
    test_split();
    test_tree();
    test_disk();
    test_ime_mouse();
    test_notes();
    test_quit();
    test_large();
    puts("ALL PASSED");
    return 0;
}
