/*
 * Drive view (progs/view.c) by its keys and pointer against the real
 * view-render: the screen is a picture in memory, the keys and pointer a
 * script, the clipboard, home's opening of links and the file chooser are
 * test doubles. The documents are view-render's samples.
 *   test-view SAMPLES WORK [PDFS]   (as root, in a root with a user of uid 1000;
 *   PDFS: test-view-pdf's fixtures, for annotations, forms, signatures, saving)
 * Build with -DVIEW_RENDER_TEST_PATH, progs/view-layout.c,
 * progs/view-client.c and base/ui.c. PASS lines; an assert ends it otherwise.
 */
#define main view_main
#include "../progs/view.c"
#include "i18n-stub.h"
#undef main
#include <assert.h>

/* The screen: 1024 by 768 pixels, cells of 8 by 16 from 8 pixels down. */
#define SW 1024
#define SH 768
#define CW 8
#define CH 16
#define TOP 8
#define COLS (SW / CW)
#define ROWS ((SH - TOP) / CH)
static uint32_t pic[SH][SW];
static char grid[ROWS][COLS + 1];
static int shape = -1, flushes;
static int screen_w = SW, screen_h = SH;

int screen_open(const char *font) { (void)font; return 1; }
int screen_cols(void) { return COLS; }
int screen_rows(void) { return ROWS; }
void screen_pixels(int *w, int *h) { *w = screen_w; *h = screen_h; }
void screen_cells(int *w, int *h, int *t) { *w = CW; *h = CH; *t = TOP; }
double screen_scale(void) { return 1; }
int screen_char_cells(uint32_t cp) { (void)cp; return 1; }
void screen_cursor(int s) { shape = s; }
void screen_flush(void) { flushes++; }
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
int keys_open(void) { return 1; }
#ifdef CONFIG_IME
int rc_read(void (*fn)(const char *, const char *, void *), void *arg) { (void)fn; (void)arg; return 0; }
#endif
void say(const char *fmt, ...) { (void)fmt; }

/* Text under pixels drawn over it is gone: the cells all inside them. */
static void cover(int x, int y, int w, int h)
{
    for (int row = 0; row < ROWS; row++) {
        int cy = TOP + row * CH;
        if (cy < y || cy + CH > y + h) continue;
        for (int col = 0; col < COLS; col++) {
            if (col * CW >= x && col * CW + CW <= x + w) grid[row][col] = ' ';
        }
    }
}

void screen_fill_px(int x, int y, int w, int h, uint32_t rgb)
{
    for (int r = y < 0 ? 0 : y; r < y + h && r < SH; r++)
        for (int c = x < 0 ? 0 : x; c < x + w && c < SW; c++) pic[r][c] = rgb & 0xFFFFFF;
    cover(x, y, w, h);
}

void screen_blit(int x, int y, int w, int h, const uint32_t *px, int stride)
{
    cover(x, y, w, h);
    for (int r = 0; r < h; r++) {
        if (y + r < 0 || y + r >= SH) continue;
        for (int c = 0; c < w; c++) {
            if (x + c >= 0 && x + c < SW) pic[y + r][x + c] = px[(size_t)r * (size_t)stride + (size_t)c] & 0xFFFFFF;
        }
    }
}

void screen_fill(int col, int row, int cols, int rows, uint32_t rgb)
{
    screen_fill_px(col * CW, TOP + row * CH, cols * CW, rows * CH, rgb);
    for (int r = row; r < row + rows && r < ROWS; r++)
        for (int c = col; r >= 0 && c < col + cols && c < COLS; c++) if (c >= 0) grid[r][c] = ' ';
}

void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    (void)fg;
    (void)bg;
    if (row < 0 || row >= ROWS) return;
    for (; *s && col < COLS; s++) {
        if (((unsigned char)*s & 0xC0) == 0x80) continue;
        if (col >= 0) grid[row][col] = (unsigned char)*s < 0x80 ? *s : '?';
        col++;
    }
}

static int on_screen(const char *s)
{
    for (int r = 0; r < ROWS; r++) {
        grid[r][COLS] = '\0';
        for (int c = 0; c < COLS; c++) if (!grid[r][c]) grid[r][c] = ' ';
        if (strstr(grid[r], s)) return 1;
    }
    return 0;
}

/* The doubles of the clipboard, home and the file chooser. */
static char clip[2][4096], opened_target[1024], pick_out[PATH_MAX];
static int pick_ok, picks;

int clipboard_set(int primary, const void *text, size_t size)
{
    assert(size < sizeof clip[0]);
    memcpy(clip[primary != 0], text, size);
    clip[primary != 0][size] = '\0';
    return CLIPBOARD_OK;
}

int clipboard_get(int primary, int *fd, size_t *size)
{
    *fd = memfd_create("test-view-clipboard", MFD_CLOEXEC);
    *size = strlen(clip[primary != 0]);
    assert(*fd >= 0 && write(*fd, clip[primary != 0], *size) == (ssize_t)*size && lseek(*fd, 0, SEEK_SET) == 0);
    return CLIPBOARD_OK;
}

const char *clipboard_error(int status) { (void)status; return "clipboard error"; }

int open_in_home(const char *with, const char *target_)
{
    assert(with == NULL);
    snprintf(opened_target, sizeof opened_target, "%s", target_);
    return 0;
}

/* Process 1's control socket: home asked to the front. */
static int home_asked;
int control_fd(void) { return 5; }
int control_send_fields(int sock, const char *const *fields)
{
    assert(sock == 5 && fields[0] && !fields[1]);
    home_asked += !strcmp(fields[0], "home");
    return 0;
}

int files_pick(const char *dir, const char *keep, const char *type, const char *what, char *out, size_t size)
{
    (void)dir;
    (void)keep;
    (void)type;
    (void)what;
    picks++;
    snprintf(out, size, "%s", pick_out);
    return pick_ok;
}

/* The script. */
enum { S_KEY, S_POINTER, S_PAGE_POINTER, S_IDLE, S_CHECK, S_KILL, S_RENDERING, S_TIMED_KEY, S_UNTIL };
struct step {
    int kind, key, mods;
    struct pointer_event e;
    int page;                 /* S_PAGE_POINTER: at this page's point */
    double px, py;
    void (*check)(void);
    int (*until)(void);
};
static void spot(int i, double x, double y, int *X, int *Y);
static struct step script[512];
static int nsteps, at_step, mods_now;
static struct pointer_event last_event;
static double t_key, t_handled = -1, t_reply = -1;

int keys_modifiers(void) { return mods_now; }
const struct pointer_event *pointer_last(void) { return &last_event; }

static void add(struct step s)
{
    assert(nsteps < 512);
    script[nsteps++] = s;
}

static void key(int k, int mods) { add((struct step){ .kind = S_KEY, .key = k, .mods = mods }); }
static void idle(void) { add((struct step){ .kind = S_IDLE }); }
static void check(void (*fn)(void)) { add((struct step){ .kind = S_CHECK, .check = fn }); }
static void type(const char *s)
{
    for (; *s; s++) key(*s, (*s >= 'A' && *s <= 'Z') ? KEYS_SHIFT : 0);
}

/* A pointer event at screen pixel (x, y). */
static void pointer_at(int x, int y, int button, int pressed, int clicks, int wheel, int mods)
{
    struct pointer_event e = { .x = x, .y = y, .col = x / CW, .row = y < TOP ? -1 : (y - TOP) / CH,
                               .button = button, .pressed = pressed, .clicks = clicks, .wheel = wheel,
                               .buttons = button && pressed ? (button == BTN_LEFT ? 1 : button == BTN_RIGHT ? 2 : 4) : 0 };
    add((struct step){ .kind = S_POINTER, .e = e, .mods = mods });
}

static void deliver(const struct pollfd *p, struct pollfd *extra, size_t count)
{
    for (size_t i = 0; i < count; i++) extra[i].revents = p[i].revents;
}

/* Ready descriptors are reported with every step, as the real one does. */
static void poll_extra(struct pollfd *extra, size_t count, int ms)
{
    struct pollfd p[4];
    for (size_t i = 0; i < count && i < 4; i++) p[i] = extra[i];
    if (poll(p, (nfds_t)count, ms) < 0) memset(p, 0, sizeof p);
    deliver(p, extra, count);
}

static int was_rendering;

int key_poll_many(struct pollfd *extra, size_t count, int timeout_ms)
{
    static double deadline;
    int rendering = fly.busy && fly.job.kind == J_RENDER;
    if (was_rendering && !rendering && t_reply < 0 && t_handled >= 0) t_reply = now();
    was_rendering = rendering;
    if (t_key > 0 && t_handled < 0) t_handled = now();
    if (at_step >= nsteps) {
        /* A question left open (unsaved changes) would ask for ever. */
        static int quits;
        assert(++quits < 1000);
        poll_extra(extra, count, 10); /* a save asked for at Ctrl+Q is answered */
        mods_now = KEYS_CTRL;
        return 17; /* Ctrl+Q */
    }
    struct step *s = &script[at_step];
    if (s->kind == S_IDLE || s->kind == S_RENDERING || s->kind == S_UNTIL) {
        int done = s->kind == S_IDLE ? !fly.busy && nqueue == 0 && !find.typed && !sel.pending && !sel.round
                 : s->kind == S_RENDERING ? rendering : s->until();
        if (done) {
            at_step++;
            deadline = 0;
            return 0;
        }
        if (!deadline) deadline = now() + 30;
        assert(now() < deadline); /* stuck */
        int ms = timeout_ms < 0 || timeout_ms > 50 ? 50 : timeout_ms;
        poll_extra(extra, count, ms);
        return 0;
    }
    at_step++;
    poll_extra(extra, count, 0);
    mods_now = s->mods;
    switch (s->kind) {
    case S_KEY:
        return s->key;
    case S_TIMED_KEY:
        t_key = now();
        return s->key;
    case S_POINTER:
        last_event = s->e;
        return K_POINTER;
    case S_PAGE_POINTER:
        last_event = s->e;
        spot(s->page, s->px, s->py, &last_event.x, &last_event.y);
        last_event.col = last_event.x / CW;
        last_event.row = (last_event.y - TOP) / CH;
        return K_POINTER;
    case S_CHECK:
        s->check();
        return 0;
    case S_KILL:
        assert(render_pid > 0 && kill(render_pid, SIGKILL) == 0);
        return 0;
    }
    return 0;
}

int key_poll_timeout(int fd, int timeout_ms)
{
    struct pollfd p = { fd, POLLIN, 0 };
    return key_poll_many(&p, fd >= 0 ? 1 : 0, timeout_ms);
}

static const char *samples;
static char home[PATH_MAX - 128], out_dir[PATH_MAX - 64];

static const char *sample(const char *name)
{
    static char path[4][PATH_MAX];
    static int i;
    i = (i + 1) % 4;
    snprintf(path[i], sizeof path[i], "%s/%s", samples, name);
    return path[i];
}

/* view with PATH and the script so far, to its end (then Ctrl+Q). */
static void run(const char *what, const char *path)
{
    printf("== %s\n", what);
    fflush(stdout);
    char *argv[] = { "view", (char *)path, NULL };
    at_step = 0;
    assert(view_main(path ? 2 : 1, argv) == 0);
    assert(at_step == nsteps);
    nsteps = 0;
}

/* Where page i's point is on the screen. */
static void spot(int i, double x, double y, int *X, int *Y)
{
    struct vl_params p = params();
    double fx, fy;
    vl_page_to_screen(&p, &doc.item[i], doc.pw[i], doc.ph[i], ax, ay, v.sx, v.sy, x, y, &fx, &fy);
    *X = (int)floor(fx);
    *Y = (int)floor(fy);
}

static int red(uint32_t c)
{
    return (c >> 16 & 0xFF) > 0xC0 && (c >> 8 & 0xFF) < 0x50 && (c & 0xFF) < 0x50;
}

/* The red square of sample.pdf's first page (400..500 by 92..192 points):
 * its width on the screen along its middle row. */
static int red_width(void)
{
    int X, Y, n = 0;
    spot(0, 450, 142, &X, &Y);
    assert(X >= 0 && X < SW && Y >= 0 && Y < SH);
    for (int x = 0; x < SW; x++) n += red(pic[Y][x]);
    return n;
}

/* Pixels not white within page i's rectangle on the screen. */
static int inked(int i)
{
    int n = 0, X = ax - v.sx + doc.item[i].x, Y = ay - v.sy + doc.item[i].y;
    for (int y = Y > ay ? Y : ay; y < Y + doc.item[i].h && y < ay + ah; y++)
        for (int x = X > ax ? X : ax; x < X + doc.item[i].w && x < ax + aw; x++) n += pic[y][x] != PAPER;
    return n;
}

static uint32_t at_point(int i, double x, double y)
{
    int X, Y;
    spot(i, x, y, &X, &Y);
    assert(X >= ax && X < ax + aw && Y >= ay && Y < ay + ah);
    return pic[Y][X];
}

/* The pointer at page i's point (where it is when the step comes): a
 * button pressed (1), released (0), or a motion with it held (-1). */
static void page_pointer(int i, double x, double y, int button, int pressed, int clicks)
{
    struct pointer_event e = { .button = pressed < 0 ? 0 : button, .pressed = pressed > 0, .clicks = clicks,
                               .buttons = !button || !pressed ? 0 : button == BTN_LEFT ? 1 : button == BTN_RIGHT ? 2 : 4 };
    add((struct step){ .kind = S_PAGE_POINTER, .e = e, .page = i, .px = x, .py = y });
}

static void click_point(int i, double x, double y, int button, int clicks)
{
    page_pointer(i, x, y, button, 1, clicks);
    page_pointer(i, x, y, button, 0, 0);
}

static void until(int (*fn)(void)) { add((struct step){ .kind = S_UNTIL, .until = fn }); }

/* Checks, run inside the script. */
static int red_before, sy_before, zoom_pct;
static struct pos place_before;

static void opened_pdf(void)
{
    assert(doc.open && doc.pages == 2 && strcmp(doc.format, "PDF") == 0);
    assert(doc.page[0].known && doc.pw[0] == 612 && doc.ph[0] == 792);
    /* Fit width (Okular's default): the page is the area's width less 6. */
    assert(v.mode == VL_FIT_WIDTH && doc.item[0].w == aw - 6);
    int X = ax - v.sx + doc.item[0].x, Y = ay - v.sy + doc.item[0].y;
    assert(X == ax + 3 && pic[Y + 5][X + 1] == PAPER && pic[Y + 5][X - 1] == DESK);
    assert(pic[Y + 5][X + doc.item[0].w - 2] == PAPER && pic[Y + 5][X + doc.item[0].w + 1] == DESK);
    assert(inked(0) > 2000); /* the text */
    red_before = red_width();
    assert(abs(red_before - (int)lround(100 * doc.item[0].zoom)) <= 2);
    assert(on_screen("Page 1 of 2") && on_screen("PDF"));
    /* The sidebar: the contents, as Okular shows when there are some. */
    assert(ui.tab == TAB_CONTENTS && on_screen("Chapter One") && on_screen("Section 1.1") && on_screen("Chapter Two"));
    printf("PASS open: page pixels within the page's rectangle (red square %d px at zoom %.3f), fit width exact, "
           "contents listed\n", red_before, doc.item[0].zoom);
}

static void zoomed_in(void)
{
    int w = red_width();
    assert(v.mode == VL_FIXED && w > red_before && abs(w - (int)lround(100 * doc.item[0].zoom)) <= 2);
    zoom_pct = zoom_percent();
    printf("PASS zoom in (Ctrl++): red square %d -> %d px, %d%%\n", red_before, w, zoom_pct);
    red_before = w;
}

static void zoomed_out(void)
{
    int w = red_width();
    assert(w < red_before && zoom_percent() < zoom_pct && abs(w - (int)lround(100 * doc.item[0].zoom)) <= 2);
    printf("PASS zoom out (Ctrl+-): red square %d -> %d px, %d%%\n", red_before, w, zoom_percent());
}

static void before_scroll(void)
{
    int X;
    spot(0, 400, 92, &X, &sy_before);
}

static void scrolled(void)
{
    int X, Y;
    spot(0, 400, 92, &X, &Y);
    assert(Y == sy_before - 100 && red(pic[Y + 3][X + 3]));
    printf("PASS scroll (Down): the picture moved up 100 pixels (Okular's step)\n");
    sy_before = Y;
}

static void wheeled(void)
{
    int X, Y;
    spot(0, 400, 92, &X, &Y);
    assert(Y == sy_before + 3 * CH && red(pic[Y + 3][X + 3]));
    printf("PASS scroll (wheel up a step): the picture moved down 3 rows\n");
}

static void at_page_two(void)
{
    assert(current_page() == 1 && v.sy == doc.item[1].y - 10);
    assert(on_screen("Page 2 of 2") && inked(1) > 1000);
    printf("PASS go to page (Ctrl+G 2 Enter): page 2's top 10 pixels below the area's (Okular's)\n");
}

static void rotated(void)
{
    assert(v.rotate == 90 && doc.item[0].w > doc.item[0].h);
    /* The square's middle still red where the turned page has it. */
    assert(red(at_point(0, 450, 142)) && red(at_point(0, 405, 97)) && red(at_point(0, 495, 187)));
    assert(!red(at_point(0, 380, 142)) && !red(at_point(0, 450, 210)));
    printf("PASS rotate right (View menu): page %dx%d, the square where a clockwise turn puts it\n", doc.item[0].w,
           doc.item[0].h);
}

static void inverted(void)
{
    uint32_t c = at_point(0, 450, 142), paper = at_point(0, 300, 400);
    assert(settings.invert && (c >> 16 & 0xFF) < 0x40 && (c >> 8 & 0xFF) > 0xB0 && (c & 0xFF) > 0xB0);
    assert(paper == 0);
    printf("PASS invert colors (View menu): red shows as %06X, paper as %06X\n", c, paper);
}

static void facing(void)
{
    assert(v.facing && columns() == 2 && doc.item[0].y == doc.item[1].y && doc.item[1].x > doc.item[0].x + doc.item[0].w);
    assert(inked(0) > 1000 && inked(1) > 1000);
    printf("PASS facing pages (View menu): pages 1 and 2 side by side, both drawn\n");
}

/* A hit's highlight: the blue of its box's background (red and green
 * full, as on paper), the most common one; 0x123456 if there is none. */
static uint32_t hit_colour(int page, int hit)
{
    const struct page *pg = &doc.page[page];
    for (int k = 0; k < pg->nhit; k++) {
        if (pg->hit[k].hit != hit) continue;
        int x0, y0, x1, y1, count[256] = { 0 }, best = -1;
        spot(page, pg->hit[k].x0, pg->hit[k].y0, &x0, &y0);
        spot(page, pg->hit[k].x1, pg->hit[k].y1, &x1, &y1);
        for (int y = y0 + 1; y < y1 - 1; y++) {
            for (int x = x0 + 1; x < x1 - 1; x++) {
                uint32_t c = pic[y][x];
                if ((c >> 16 & 0xFF) > 0xF0 && (c >> 8 & 0xFF) > 0xF0) count[c & 0xFF]++;
            }
        }
        for (int b = 0; b < 256; b++) if (count[b] && (best < 0 || count[b] > count[best])) best = b;
        return best < 0 ? 0x123456 : 0xFFFF00u | (uint32_t)best;
    }
    return 0x123456;
}

static int yellowish(uint32_t c)
{
    return (c >> 16 & 0xFF) > 0xC0 && (c >> 8 & 0xFF) > 0xC0 && (c & 0xFF) < 0xD0;
}

static void found_first(void)
{
    assert(find.cur_page == 0 && find.cur_hit == 0 && doc.page[0].hits == 2 && doc.page[1].hits == 2);
    uint32_t a = hit_colour(0, 0), b = hit_colour(1, 0);
    assert(yellowish(a) && yellowish(b) && (a & 0xFF) < (b & 0xFF)); /* the current one stronger */
    printf("PASS search (Ctrl+F quokka Enter): both visible pages' hits highlighted (%06X current, %06X other)\n", a, b);
}

static void found_next(void)
{
    assert(find.cur_page == 1 && find.cur_hit == 0);
    uint32_t a = hit_colour(0, 0), b = hit_colour(1, 0);
    assert(yellowish(a) && yellowish(b) && (b & 0xFF) < (a & 0xFF));
    printf("PASS find next (F3): the match on page 2 is now the current one\n");
}

static void selected_hello(void)
{
    if (strcmp(clip[1], "Hello") != 0) printf("primary '%s' active %d parts %d shape %d\n", clip[1], sel.active, sel.npart, shape);
    assert(strcmp(clip[1], "Hello") == 0 && sel.active && shape == CURSOR_TEXT);
    printf("PASS drag-select (Ctrl+4, drag over a word): primary is \"%s\"\n", clip[1]);
}

static void copied_hello(void)
{
    assert(strcmp(clip[0], "Hello") == 0);
    printf("PASS copy (Ctrl+C): the clipboard is \"%s\"\n", clip[0]);
}

static void word_and_line(void)
{
    assert(strcmp(clip[1], "Search target: quokka") == 0);
    printf("PASS double click selects a word, triple click a line: primary \"%s\"\n", clip[1]);
}

static void word_only(void)
{
    assert(strcmp(clip[1], "quokka") == 0);
}

static void links_here(void)
{
    assert(doc.page[0].links == 2 && doc.page[0].nlink == 2);
    place_before = pos_now();
}

static void followed(void)
{
    assert(current_page() == 1 || pos_now().page == 1);
    int X, Y;
    spot(1, 72, 100, &X, &Y);
    assert(Y == ay && hist.n == 2 && hist.i == 1);
    printf("PASS internal link (a click in Browse): page 2 with its point (72, 100) at the top\n");
}

static void came_back(void)
{
    struct pos p = pos_now();
    assert(p.page == place_before.page && fabs(p.ny - place_before.ny) < 1e-6 && hist.i == 0);
    printf("PASS back (Alt+Shift+Left): where the link was clicked\n");
}

static void went_out(void)
{
    assert(strcmp(opened_target, "https://example.org/") == 0 && current_page() == 0);
    printf("PASS external link: open_in_home(NULL, \"%s\")\n", opened_target);
}

static void toc_click(void)
{
    int col, row;
    for (row = 0; row < ROWS; row++)
        for (col = 0; col < COLS; col++)
            if (ui_hit_at(col, row) == HIT_TOC + 2) goto found;
    assert(0);
found:
    assert(doc.noutline == 3 && strcmp(doc.outline[2].title, "Chapter Two") == 0 && doc.outline[1].level == 1);
    last_event = (struct pointer_event){ .x = col * CW + 2, .y = TOP + row * CH + 2, .col = col, .row = row,
                                         .button = BTN_LEFT, .pressed = 1, .clicks = 1 };
    pointer(&last_event);
}

static void toc_went(void)
{
    assert(current_page() == 1 && pos_now().page == 1);
    printf("PASS contents: Chapter One, Section 1.1, Chapter Two listed; clicking Chapter Two goes to page 2\n");
}

/* The text from page's point y down, by a renderer of the test's own. */
static void text_at(int page, double y, float lw, float lh, float em, char *out, size_t size)
{
    int s;
    pid_t pid = view_spawn(&s);
    assert(pid > 0);
    struct view_request q;
    struct view_reply r;
    int fd;
    memset(&q, 0, sizeof q);
    q.op = VIEW_OPEN;
    snprintf(q.text, sizeof q.text, "%s", doc.path);
    q.ax = lw;
    q.ay = lh;
    q.bx = em;
    assert(view_call(s, &q, &r, &fd) == 0 && r.status == 0);
    memset(&q, 0, sizeof q);
    q.op = VIEW_SELECT;
    q.page = page;
    q.mode = VIEW_SELECT_CHARS;
    q.ax = 0;
    q.ay = (float)y;
    q.bx = 10000;
    q.by = 10000;
    assert(view_call(s, &q, &r, &fd) == 0 && r.status == 0 && fd >= 0);
    void *m = view_map(fd, r.size);
    close(fd);
    size_t len;
    const char *t = reply_text(&r, m, (size_t)r.size, &len);
    assert(t);
    /* Its first words, one space apart. */
    size_t n = 0;
    int words = 0, space = 1;
    for (size_t i = 0; i < len && n + 2 < size && words < 4; i++) {
        if ((unsigned char)t[i] <= ' ') {
            if (!space) words++;
            if (!space && words < 4) out[n++] = ' ';
            space = 1;
        } else {
            out[n++] = t[i];
            space = 0;
        }
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = '\0';
    munmap(m, r.size);
    close(s);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static char top_before[256];
static float em_before;

static void reflow_before(void)
{
    assert(doc.open && (doc.flags & VIEW_REFLOWABLE) && doc.pages >= 2);
    struct pos p = pos_now();
    assert(p.page >= 1 && p.ny > 0.05); /* the middle of a page, not its start */
    double px, py;
    page_point(ax + aw / 2, ay, 1, &px, &py);
    em_before = doc.em;
    text_at(p.page, py, doc.lw, doc.lh, doc.em, top_before, sizeof top_before);
    assert(top_before[0]);
}

static void reflow_after(void)
{
    char after[256];
    double px, py;
    assert(doc.em == em_before + 1);
    int page = page_point(ax + aw / 2, ay + 1, 1, &px, &py);
    text_at(page, py, doc.lw, doc.lh, doc.em, after, sizeof after);
    assert(strncmp(after, top_before, strlen(top_before) < 20 ? strlen(top_before) : 20) == 0);
    printf("PASS reflow (EPUB, >): text %.0f -> %.0f points, the top still \"%s\" (page %d of %d)\n", em_before,
           doc.em, after, page + 1, doc.pages);
}

static void asked_password(void)
{
    assert(!doc.open && prompt.kind == P_PASSWORD && strstr(prompt.question, "Please enter the password"));
}

static void asked_again(void)
{
    assert(!doc.open && prompt.kind == P_PASSWORD && strstr(prompt.question, "Incorrect password"));
}

static void unlocked(void)
{
    assert(doc.open && doc.pages == 2 && inked(0) > 1000);
    printf("PASS password: asked, a wrong one asked again, the right one opens locked.pdf\n");
}

static char state_file[PATH_MAX + 64];

static void place_and_zoom(void)
{
    assert(current_page() == 1 && v.mode == VL_FIXED && v.zoom == 1);
}

static void restored(void)
{
    assert(current_page() == 1 && v.mode == VL_FIXED && fabs(v.zoom - 1) < 1e-6 && zoom_percent() == 100);
    printf("PASS reopened: page 2 at 100%% again, from the state file\n");
}

static void exported(void)
{
    char path[PATH_MAX + 128], buf[4096];
    snprintf(path, sizeof path, "%s/sample.txt", out_dir);
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_uid == VIEW_USER);
    FILE *f = fopen(path, "r");
    assert(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    assert(strstr(buf, "Hello LOCachy viewer") && strstr(buf, "Second page marker") && !strchr(buf, '\f'));
    printf("PASS export as plain text: %s, uid %d, both pages' text\n", path, (int)st.st_uid);
}

static void listed_embedded(void)
{
    assert(list.kind == L_EMBEDDED && list.n == 1 && strcmp(list.name[0], "notes.txt") == 0 && on_screen("notes.txt"));
}

static void saved_embedded(void)
{
    char path[PATH_MAX + 128], buf[64];
    snprintf(path, sizeof path, "%s/notes.txt", out_dir);
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_uid == VIEW_USER);
    FILE *f = fopen(path, "r");
    assert(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    assert(strcmp(buf, "attached notes\n") == 0);
    printf("PASS save an embedded file: %s, uid %d, its bytes\n", path, (int)st.st_uid);
}

static void properties(void)
{
    assert(list.kind == L_INFO && on_screen("Properties") && on_screen("Title: LOCachy Sample") &&
           on_screen("Author: View Tester") && on_screen("Fonts") && on_screen("Helvetica"));
    printf("PASS properties (Alt+Return): title, author and the fonts listed\n");
}

static pid_t old_pid;

static void before_kill(void)
{
    old_pid = render_pid;
}

static void survived(void)
{
    assert(render_pid > 0 && render_pid != old_pid && doc.open && strstr(ui.toast, "renderer stopped"));
    assert(inked(0) > 2000 && pix_find(0, 0, v.rotate, (float)doc.item[0].zoom, -1, -1) >= 0);
    printf("PASS the renderer killed: started again, the document where it was, pages drawn at a new zoom\n");
}

static int cancels;

static void rendering_heavy(void)
{
    assert(fly.busy && fly.job.kind == J_RENDER);
}

static void responsive(void)
{
    /* The key was handled (the place moved, the screen drawn) with the
     * render of the heavy page still out. */
    assert(t_handled > 0 && fly.busy && fly.job.kind == J_RENDER && v.sy > 0);
}

static void responsive_measured(void)
{
    assert(t_reply > t_handled && t_handled >= t_key);
    printf("PASS responsive: a key handled %.1f ms after it came, the page's render answered %.1f ms later\n",
           (t_handled - t_key) * 1000, (t_reply - t_handled) * 1000);
}

static void cancel_sent(void)
{
    assert(fly.cancelled);
    cancels++;
}

static void cancelled(void)
{
    assert(cancels == 1 && inked(0) > 1000);
    printf("PASS stale work cancelled: a zoom during a render sends SIGUSR1, the new zoom drawn\n");
}

/* The cell where id was registered when last drawn. */
static int hit_cell(int id, int *col, int *row)
{
    for (*row = 0; *row < ROWS; (*row)++)
        for (*col = 0; *col < COLS; (*col)++)
            if (ui_hit_at(*col, *row) == id) return 1;
    return 0;
}

static void press_id(int id)
{
    int col, row;
    assert(hit_cell(id, &col, &row));
    struct pointer_event e = { .x = col * CW + 1, .y = TOP + row * CH + 1, .col = col, .row = row,
                               .button = BTN_LEFT, .pressed = 1, .clicks = 1, .buttons = 1 };
    last_event = e;
    pointer(&e);
}

static void context_fit_width(void)
{
    assert(v.mode == VL_FIT_WIDTH);
    printf("PASS right click on a page (Browse): Okular's page menu, Fit Width chosen\n");
}

static void sidebar_hidden(void)
{
    assert(!settings.sidebar && side_cols == 0 && ax == 0 && doc.item[0].w == SW - 6 && !on_screen("Contents"));
}

static void thumbnails_tab(void)
{
    assert(settings.sidebar && side_cols > 0 && doc.item[0].w == aw - 6);
    press_id(HIT_TAB + TAB_THUMBNAILS);
    assert(ui.tab == TAB_THUMBNAILS);
}

static void thumbnails_drawn(void)
{
    int n = 0;
    for (int y = ay; y < SH; y++)
        for (int x = 0; x < side_cols * CW; x++) n += red(pic[y][x]);
    int w, h;
    double z;
    thumb_size(0, &w, &h, &z);
    assert(n > 0 && abs(n - (int)lround(100 * z) * (int)lround(100 * z)) < 30 * (int)lround(100 * z));
    assert(pix_find(1, 0, v.rotate, (float)z, -1, -1) >= 0 && (on_screen("Thumbnails") || on_screen("Thumbs")));
    press_id(HIT_THUMB + 1);
}

static void thumbnail_went(void)
{
    assert(current_page() == 1 && pos_now().page == 1);
    printf("PASS sidebar: F7 hides and shows it; the thumbnails drawn (red square %dx%d), a click goes to page 2\n",
           (int)lround(100 * thumb_width() / 612.0), (int)lround(100 * thumb_width() / 612.0));
}

/* A zoom at the pointer: the page's point under it stays under it. */
static double zoom_was;
static int fixed_x, fixed_y;

static void zoom_at_pointer(struct pointer_event e, int mods)
{
    spot(0, 450, 142, &fixed_x, &fixed_y);
    e.x = fixed_x;
    e.y = fixed_y;
    e.col = e.x / CW;
    e.row = (e.y - TOP) / CH;
    zoom_was = zoom_now();
    mods_now = mods;
    last_event = e;
    pointer(&e);
    mods_now = 0;
}

static void still_under(double factor)
{
    int X, Y;
    spot(0, 450, 142, &X, &Y);
    assert(fabs(zoom_now() / zoom_was - factor) < 1e-6 && abs(X - fixed_x) <= 1 && abs(Y - fixed_y) <= 1);
}

static void ctrl_wheel(void)
{
    zoom_at_pointer((struct pointer_event){ .wheel = 1 }, KEYS_CTRL);
    still_under(1 + 120 / 500.0);
    printf("PASS Ctrl+wheel: zoom times 1.24 (Okular's 1 + 120/500), the point under the pointer kept\n");
}

static void red_matches(void)
{
    assert(abs(red_width() - (int)lround(100 * doc.item[0].zoom)) <= 2);
}

static void pinch(void)
{
    zoom_at_pointer((struct pointer_event){ .gesture = GESTURE_PINCH, .scale = 1.5 }, 0);
    still_under(1.5);
    printf("PASS pinch: zoom times its scale, at the pinch\n");
}

static void middle_drag(void)
{
    double before = zoom_now();
    int X = ax + aw / 2, Y = ay + ah / 2;
    struct pointer_event e = { .x = X, .y = Y, .button = BTN_MIDDLE, .pressed = 1, .clicks = 1, .buttons = 4 };
    pointer(&e);
    e = (struct pointer_event){ .x = X, .y = Y - 50, .buttons = 4 };
    pointer(&e);
    e = (struct pointer_event){ .x = X, .y = Y - 50, .button = BTN_MIDDLE, .pressed = 0 };
    pointer(&e);
    assert(fabs(zoom_now() / before - 1.1) < 1e-6 && !ptr.mid);
    printf("PASS middle button dragged up 50 pixels: zoom times 1.1 (Okular's continuous zoom)\n");
}

static void zoom_box(void)
{
    press_id(HIT_ZOOM); /* its menu takes the next keys */
    assert(v.mode == VL_FIXED && v.zoom == 4.0 && !whole(0));
}

static void square_in_middle(void)
{
    double ix, iy;
    vl_point_to_item(v.rotate, 612, 792, doc.item[0].zoom, 450, 142, &ix, &iy);
    v.sx = doc.item[0].x + (int)ix - aw / 2;
    v.sy = doc.item[0].y + (int)iy - ah / 2;
    clamp_scroll();
    changed();
}

static void tiles(void)
{
    double ix, iy;
    vl_point_to_item(v.rotate, 612, 792, doc.item[0].zoom, 450, 142, &ix, &iy);
    float z = (float)doc.item[0].zoom;
    int c = pix_find(0, 0, v.rotate, z, (int)ix / TILE, (int)iy / TILE), n = 0;
    for (int k = 0; k < ncache; k++) n += cache[k].used && cache[k].zoom == z && cache[k].page == 0 && cache[k].tx >= 0;
    assert(c >= 0 && cache[c].w <= TILE && cache[c].h <= TILE && pix_find(0, 0, v.rotate, z, -1, -1) < 0);
    assert(red(at_point(0, 450, 142)) && !red(at_point(0, 390, 142)));
    printf("PASS 400%% (the zoom box): page %dx%d drawn from %d tiles of %d, the square where it should be\n",
           doc.item[0].w, doc.item[0].h, n, TILE);
}

static int restarted(void)
{
    return render_pid > 0 && render_pid != old_pid && !doc.opening;
}

static void state_written(void)
{
    struct stat st;
    char buf[8192];
    assert(stat(state_file, &st) == 0 && st.st_uid == VIEW_USER && (st.st_mode & 0777) == 0600);
    FILE *f = fopen(state_file, "r");
    assert(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char line[PATH_MAX + 64];
    snprintf(line, sizeof line, "mode=0 zoom=1.0000 rotate=0 facing=0 em=11.0\t%s\n", sample("sample.pdf"));
    if (!strstr(buf, line) || !strstr(buf, "page=1 ")) printf("state file:\n%s(wanted: %s)\n", buf, line);
    assert(strstr(buf, "page=1 ") && strstr(buf, line) && strstr(buf, "sidebar=1 invert=0 tool=0 facing=0 side_width=") && strstr(buf, "trim_margins=0 render_mode=0 paper=ffffff recolor_fg=600000 recolor_bg=f0f0f0 bw_threshold=127 bw_contrast=2 columns=3 rtl=0\t\n"));
    printf("PASS state file: %s written as uid %d (mode %o), the place and the zoom\n", state_file, (int)st.st_uid,
           st.st_mode & 0777);
}

/* Regression of the real pointer dispatch, not a parallel geometry model. */
static void pointer_focus_splitter(void)
{
    find_bar(1);
    draw();
    struct pointer_event e = { .x = ax + aw / 2, .y = ay + 80,
        .col = (ax + aw / 2) / CW, .row = (ay + 80 - TOP) / CH,
        .button = BTN_LEFT, .pressed = 1, .clicks = 1 };
    pointer(&e);
    assert(ui.focus == FOCUS_DOC);
    e.pressed = 0;
    pointer(&e);
    find_bar(0);
    draw();
    int before = ax;
    e = (struct pointer_event){ .x = ax - 2, .y = ay + 100,
        .col = (ax - 2) / CW, .row = (ay + 100 - TOP) / CH,
        .button = BTN_LEFT, .pressed = 1, .clicks = 1 };
    pointer(&e);
    e.x += 80;
    e.col = e.x / CW;
    e.button = 0;
    pointer(&e);
    assert(ax >= before + 79); /* Opaque/live resize, before release. */
    e.button = BTN_LEFT;
    e.pressed = 0;
    pointer(&e);
    assert(ax >= before + 79);
    e = (struct pointer_event){ .x = ax - 2, .y = ay + 100, .button = BTN_LEFT, .pressed = 1 };
    pointer(&e); e.x = -100; e.button = 0; pointer(&e);
    assert(settings.side_width == 90);
    e.x = SW + 100; pointer(&e); assert(settings.side_width == 600);
    e.button = BTN_LEFT; e.pressed = 0; pointer(&e);
    struct saved persisted;
    settings.sidebar = 0; settings.side_width = 90;
    state_load(NULL, &persisted);
    assert(settings.sidebar && settings.side_width == 600);
    geometry(); layout();
    printf("PASS pointer document focus and live sidebar splitter dispatch, logical 90..600 bounds\n");
}

static int bbox_requests;

/* Every job sent is in flight when the loop next polls: the VIEW_BBOX
 * request carries the page's display raster scale. */
static int bbox_in_flight(void)
{
    if (!fly.busy || fly.job.kind != J_BBOX) return 0;
    assert(fly.job.req.zoom == (float)doc.item[fly.job.page].zoom && fly.job.req.zoom > 0);
    bbox_requests++;
    return 1;
}

static void parity_bbox_huge_start(void)
{
    v.rotate = 0; v.facing = 0;
    go(pos_page(0));
    zoom_to(VL_FIXED, 100, ax + aw / 2, ay + ah / 2);
    do_action(A_TRIM_MARGINS);
    assert(settings.trim_margins);
}

static int bbox_failed_shown(void)
{
    for (int i = 0; i < doc.pages; i++)
        if (doc.page[i].bbox_failed > 0 && ui.toast_error && strstr(ui.toast, "Cannot find the margins")) return 1;
    return 0;
}

static void parity_bbox_huge(void)
{
    int failed = -1;
    for (int i = 0; i < doc.pages; i++) if (doc.page[i].bbox_failed > 0) failed = i;
    assert(failed >= 0 && doc.page[failed].bounds == 0);
    assert(doc.crop[failed].x0 == 0 && doc.crop[failed].y1 == 1 && doc.item[failed].ox == 0);
    do_action(A_TRIM_MARGINS);
    assert(!settings.trim_margins);
    printf("PASS Trim Margins: a display raster over the renderer's pixel limit shows an error, the page stays uncropped, retried at another zoom\n");
}

static void parity_trim_start(void)
{
    v.rotate = 0; v.facing = 0; v.mode = VL_FIT_PAGE;
    layout(); pos_go(pos_page(0));
    do_action(A_TRIM_MARGINS);
}

static void parity_margins(void)
{
    assert(settings.trim_margins && !trim.selection && doc.page[0].bounds == 2);
    struct vl_crop b = doc.crop[0];
    assert(b.x0 > 0 && b.y0 > 0 && b.x1 < 1 && b.y1 < 1);
    assert(b.x1 - b.x0 >= .5 && b.y1 - b.y0 >= .5);
    pos_go(pos_page(0)); draw();
    assert(red(at_point(0, 450, 142)));
    struct vl_params p = params();
    double X, Y, px, py;
    vl_page_to_screen(&p, &doc.item[0], 612, 792, ax, ay, v.sx, v.sy, 450, 142, &X, &Y);
    assert(page_point((int)X, (int)Y, 0, &px, &py) == 0 && fabs(px - 450) < 2 && fabs(py - 142) < 2);
    zoom_to(VL_FIXED, zoom_now() * 1.2, (int)X, (int)Y);
    assert(page_point((int)X, (int)Y, 0, &px, &py) == 0 && fabs(px - 450) < 2 && fabs(py - 142) < 2);
    v.rotate = 90; relayout_middle();
    assert(fabs(doc.crop[0].x0 - (1 - b.y1)) < 1e-6 && fabs(doc.crop[0].y0 - b.x0) < 1e-6);
    v.rotate = 0;
    do_action(A_TRIM_SELECTION);
    assert(!settings.trim_margins && trim.picking && trim.selection && doc.item[0].ox == 0);
    v.mode = VL_FIT_PAGE; layout(); pos_go(pos_page(0)); draw();
    printf("PASS Trim Margins: rendered-content bounds, 4%% expansion/minimum, rotation, actual pixels and zoom coordinate anchors\n");
}

static void parity_selection(void)
{
    assert(trim.selection && !trim.picking && !trim.dragging && !settings.trim_margins);
    for (int i = 0; i < doc.pages; i++) {
        assert(fabs(doc.crop[i].x0 - trim.box.x0) < 1e-6 && fabs(doc.crop[i].y0 - trim.box.y0) < 1e-6);
        assert(doc.item[i].ox > 0 && doc.item[i].oy > 0);
    }
    pos_go(pos_page(0)); draw();
    assert(red(at_point(0, 450, 142)));
    find_bar(1);
    snprintf(clip[0], sizeof clip[0], "quokka");
    key_find(22, KEYS_CTRL);
    key_find('\n', 0);
}

static void parity_selection_found(void)
{
    assert(find.cur_page >= 0 && find.cur_hit >= 0);
    find_bar(0);
    do_action(A_TRIM_SELECTION);
    assert(!trim.selection && doc.item[0].ox == 0 && doc.item[0].oy == 0);
    do_action(A_TRIM_SELECTION);
    key_doc(27, 0);
    assert(!trim.selection && !trim.picking);
    do_action(A_TRIM_MARGINS);
    state_save();
    char *stored = state_read();
    assert(stored && !strstr(stored, "trim_selection") && !strstr(stored, "trim_box"));
    free(stored);
    struct saved test;
    settings.side_width = 377;
    sidebar(0);
    state_save();
    settings.trim_margins = 0; settings.sidebar = 1; settings.side_width = 90;
    state_load(NULL, &test);
    assert(settings.trim_margins && !settings.sidebar && settings.side_width == 377);
    do_action(A_TRIM_MARGINS);
    sidebar(1);
    printf("PASS Trim To Selection: actual rectangle pointer dispatch, normalized all-page crop, search after crop, toggle/Escape and margins/sidebar persistence\n");
}

static void parity_crop_interaction_start(void)
{
    v.mode = VL_FIT_PAGE; v.rotate = 0; v.facing = 0;
    trim.selection = 1; trim.picking = 0;
    trim.box = (struct vl_crop){ .05, .05, .95, .85 };
    settings.trim_margins = 0;
    pix_flush(); layout(); go(pos_page(0));
    settings.tool = TOOL_TEXT;
}

static void parity_crop_text(void)
{
    assert(!strcmp(clip[1], "Hello"));
    settings.tool = TOOL_BROWSE;
    printf("PASS actual text selection after crop maps to original document text\n");
}

static void parity_crop_link(void)
{
    assert(current_page() == 1);
    printf("PASS actual internal link hit testing and target navigation after crop\n");
    do_action(A_TRIM_SELECTION);
}

static void parity_find_utf8(void)
{
    find_bar(1);
    find_insert("A中文B", strlen("A中文B"));
    key_find(K_LEFT, 0);
    key_find('\b', 0);
    assert(!strcmp(find.in.text, "A中B"));
    key_find(K_BOL, 0); key_find(K_RIGHT, 0);
    find_insert("文", strlen("文"));
    assert(!strcmp(find.in.text, "A文中B"));
    key_find(1, KEYS_CTRL); key_find(3, KEYS_CTRL);
    assert(!strcmp(clip[0], "A文中B"));
    key_find(24, KEYS_CTRL); assert(!find.in.text[0]);
    key_find(22, KEYS_CTRL); assert(!strcmp(find.in.text, "A文中B"));
    key_find(K_BOL, 0); key_find(K_DEL, 0); assert(!strcmp(find.in.text, "文中B"));
    key_find(K_RIGHT, KEYS_SHIFT); key_find(3, KEYS_CTRL); assert(!strcmp(clip[0], "文"));
    draw();
    struct pointer_event e = { .x = find.in.col * CW + 1, .y = TOP + (ROWS - 1) * CH + 1,
        .col = find.in.col, .row = ROWS - 1, .button = BTN_LEFT, .pressed = 1, .clicks = 1 };
    pointer(&e);
    e.col += 2; e.x += 2 * CW; e.button = 0; pointer(&e);
    e.button = BTN_LEFT; e.pressed = 0; pointer(&e);
    assert(ui.focus == FOCUS_FIND && find.in.cursor > find.in.mark);
    key_find(3, KEYS_CTRL); assert(!strcmp(clip[0], "文中"));
    find_bar(0);
    printf("PASS find UTF-8 insertion cursor, codepoint backspace/delete, selection, clipboard cut/copy/paste\n");
}

static void parity_blank_page(void)
{
    do_action(A_TRIM_MARGINS);
}
static void parity_blank_bounds(void)
{
    assert(doc.page[0].bounds == 2);
    assert(doc.crop[0].x0 == 0 && doc.crop[0].y0 == 0 && doc.crop[0].x1 == 1 && doc.crop[0].y1 == 1);
    assert(doc.item[0].ox == 0 && doc.item[0].oy == 0);
    printf("PASS real blank white PDF page remains uncropped under Trim Margins\n");
    do_action(A_TRIM_MARGINS);
}

static double parity_old_lw;
static void parity_reflow_resize(void)
{
    parity_old_lw = doc.lw;
    settings.side_width = settings.side_width >= 550 ? 200 : 600;
    sidebar(1);
    settings.side_width = settings.side_width >= 550 ? 180 : 580;
    sidebar(1); /* A second resize while the old top-text/layout job is pending. */
}

static void parity_reflow_resized(void)
{
    assert(!font.busy && fabs(doc.lw - parity_old_lw) > 50);
    assert(fabs(doc.lw - (aw - col_margin) / z100) < .5);
    parity_old_lw = doc.lw;
    sidebar(0);
}

static void parity_reflow_hidden(void)
{
    assert(!font.busy && doc.lw > parity_old_lw + 100);
    assert(fabs(doc.lw - (aw - col_margin) / z100) < .5);
    parity_old_lw = doc.lw;
    screen_w = SW - 160; screen_h = SH - 80;
    handle(K_SHOWN);
}

static void parity_reflow_screen(void)
{
    assert(!font.busy && doc.lw < parity_old_lw - 80);
    assert(fabs(doc.lw - (aw - col_margin) / z100) < .5 && fabs(doc.lh - (ah - row_margin) / z100) < .5);
    screen_w = SW; screen_h = SH;
    handle(K_SHOWN);
    printf("PASS reflowable document renderer layout tracks viewport after sidebar resize/hide and actual K_SHOWN screen geometry change\n");
}

static void parity_bookmarks(void)
{
    do_action(A_SINGLE); do_action(A_FIT_PAGE); go(pos_page(0));
    do_action(A_BOOKMARK); assert(doc.bookmark[0]);
    go(pos_page(1)); do_action(A_BOOKMARK); assert(doc.bookmark[1]);
    do_action(A_BOOKMARK_RENAME);
    snprintf(prompt.in.text, sizeof prompt.in.text, "Second chapter"); prompt_done(1);
    assert(!strcmp(doc.bookmark[1], "Second chapter"));
    do_action(A_BOOKMARK_PREV); assert(current_page() == 0);
    do_action(A_BOOKMARK_NEXT); assert(current_page() == 1);
    sidebar(1); ui.tab = TAB_BOOKMARKS; draw(); press_id(HIT_BOOKMARK); assert(current_page() == 0);
    state_save();
    free(doc.bookmark[0]); doc.bookmark[0] = NULL;
    free(doc.bookmark[1]); doc.bookmark[1] = NULL;
    bookmarks_load();
    assert(doc.bookmark[0] && !strcmp(doc.bookmark[1], "Second chapter"));
    do_action(A_CONTINUOUS); assert(settings.paged);
    go(pos_page(1)); draw();
    assert(doc.item[0].hidden && !doc.item[1].hidden && current_page() == 1);
    screen_step(-1); assert(current_page() == 0);
    do_action(A_CONTINUOUS); assert(!settings.paged);
    do_action(A_FIT_AUTO); assert(v.mode == VL_FIT_AUTO);
    do_action(A_RELOAD);
}

static void parity_reloaded(void)
{
    assert(doc.open && doc.bookmark[0] && doc.bookmark[1]);
    assert(v.mode == VL_FIT_AUTO && current_page() == 0);
    printf("PASS bookmark add/remove/rename, previous/next, clickable sidebar and persistence; independent paged navigation, Auto Fit and actual F5 reload\n");
}

#ifdef CONFIG_IME
static void parity_ime_start(void)
{
    find_bar(1);
    ime_set_mode(IME_MODE_CN);
    mods_now = 0;
    handle('n'); handle('i');
    char a[512], b[512];
    assert(ime_get_overlay(a, b, sizeof a));
    handle(' ');
    assert(find.in.text[0] && (unsigned char)find.in.text[0] >= 128);
    draw();
    key_find('\n', 0);
}

static void parity_ime_found(void)
{
    if (find.failed || find.cur_page < 0) printf("IME query: %s active: %s pages %d flags %u failed %d\n", find.in.text, find.active, doc.pages, doc.flags, find.failed);
    assert(!find.failed && find.cur_page >= 0);
    printf("PASS real Rime Chinese preedit, committed UTF-8 query and real renderer search result\n");
    find_bar(0);
    ime_set_mode(IME_MODE_RAW);
}
#endif

#ifdef CONFIG_IME
/* Every prompt has the find bar's input method: a bookmark renamed in Chinese. */
static void ime_prompt(void)
{
    go(pos_page(0));
    if (!doc.bookmark[0]) do_action(A_BOOKMARK);
    do_action(A_BOOKMARK_RENAME);
    assert(prompt.kind == P_BOOKMARK && typing_line());
    ime_set_mode(IME_MODE_CN);
    mods_now = 0;
    handle('n');
    handle('i');
    char a[512], b[512];
    assert(ime_get_overlay(a, b, sizeof a));
    handle(' ');
    assert((unsigned char)prompt.in.text[0] >= 128 && !strstr(prompt.in.text, "Page"));
    handle('\n');
    assert(prompt.kind == P_NONE && (unsigned char)doc.bookmark[0][0] >= 128);
    ime_set_mode(IME_MODE_RAW);
    printf("PASS real Rime: a bookmark renamed in Chinese through the prompt's input method (\"%s\")\n", doc.bookmark[0]);
    bookmark_remove(0);
}
#endif

static void parity_tabs_open(void)
{
    go(pos_page(1)); zoom_to(VL_FIXED, 1.25, ax + aw / 2, ay + ah / 2);
    snprintf(pick_out, sizeof pick_out, "%s", sample("sample.md")); pick_ok = 1;
    do_action(A_OPEN);
}

static void parity_tabs_back(void)
{
    assert(tabs.n == 2 && tabs.current == 1 && !strcmp(doc.format, "Markdown"));
    draw(); press_id(HIT_DOCUMENT_TAB);
}

static void parity_tabs_close(void)
{
    assert(tabs.current == 0 && !strcmp(doc.format, "PDF") && current_page() == 1 && v.zoom == 1.25);
    draw(); press_id(HIT_CLOSE_TAB);
}

static void parity_tabs_reopen(void)
{
    assert(tabs.n == 1 && tabs.nclosed == 1 && !strcmp(doc.format, "Markdown"));
    do_action(A_REOPEN_TAB);
}

static void parity_tabs_restored(void)
{
    assert(tabs.n == 2 && tabs.current == 1 && !strcmp(doc.format, "PDF") && current_page() == 1 && v.zoom == 1.25);
    printf("PASS document tabs: Open creates tab, pointer switches/closes, Undo Close restores document position and zoom\n");
}

/* PDF interaction: test-view-pdf's fixtures, copied for the user to save. */
static const char *pdf_dir;
static gid_t user_gid;

static const char *user_copy(const char *from_dir, const char *name, const char *as)
{
    static char path[4][PATH_MAX];
    static int i;
    i = (i + 1) % 4;
    snprintf(path[i], sizeof path[i], "%s/%s", out_dir, as);
    char cmd[3 * PATH_MAX];
    snprintf(cmd, sizeof cmd, "cp '%s/%s' '%s' && chown %d:%d '%s'", from_dir, name, path[i], VIEW_USER, (int)user_gid, path[i]);
    assert(system(cmd) == 0);
    return path[i];
}

static const char *out_file(const char *name)
{
    static char path[2][PATH_MAX];
    static int i;
    i = (i + 1) % 2;
    snprintf(path[i], sizeof path[i], "%s/%s", out_dir, name);
    return path[i];
}

static void erase(int n)
{
    for (int k = 0; k < n; k++) key('\b', 0);
}

static void downs(int n)
{
    for (int k = 0; k < n; k++) key(K_DOWN, 0);
}

/* The nth widget of the field called name (a radio group's widgets share it). */
static const struct view_field *field_named(const char *name, int nth, int *page)
{
    for (int i = 0; i < doc.pages; i++)
        for (int k = 0; k < doc.page[i].nfield; k++)
            if (!strcmp(doc.page[i].field[k].name, name) && nth-- == 0) {
                if (page) *page = i;
                return &doc.page[i].field[k];
            }
    return NULL;
}

static const char *value_of(const char *name)
{
    const struct view_field *f = field_named(name, 0, NULL);
    assert(f);
    return f->value;
}

/* A choice's option of that export value: selected 1, not 0, none -1. */
static int chosen(const char *name, const char *value)
{
    int page;
    const struct view_field *f = field_named(name, 0, &page);
    assert(f);
    for (int k = 0; k < f->options; k++) {
        const struct view_option *o = &doc.page[page].option[f->first_option + k];
        if (!strcmp(o->value, value)) return o->selected;
    }
    return -1;
}

static void forms_opened(void)
{
    assert(doc.open && pdf_on() && (pdf.flags & VIEW_PDF_HAS_FORMS) && doc.page[0].fields == 2 && !pdf.forms);
    const struct view_field *f = field_named("text", 0, NULL);
    assert(f && f->required && f->max_length == 16 && !strcmp(f->value, "before") && field_named("readonly", 0, NULL)->readonly);
    assert(on_screen("This document has forms.") && on_screen("This document has signature placeholder fields."));
    assert(!pdf_modified() && !on_screen("[modified]"));
    snprintf(pick_out, sizeof pick_out, "%s", out_dir); /* where Save As's folder is chosen */
    pick_ok = 1;
    press_id(HIT_PDF_BAR + 0); /* the forms bar's Show Forms */
    assert(pdf.forms);
    printf("PASS forms: Okular's forms and signature placeholder bars; Show Forms by its button\n");
}

static void forms_text_focused(void)
{
    draw();
    assert(on_screen("text (text, required, at most 16 characters): before") && on_screen("[Show Forms]") && pdf.forms);
}

static void forms_typed(void)
{
    assert(!strcmp(value_of("text"), "after") && pdf_modified());
    draw();
    assert(on_screen("[modified]") && on_screen("): after"));
    printf("PASS forms: Tab focuses the required text field; typing replaces it, Enter sets it; [modified] shown\n");
}

static void forms_maxlen_typing(void)
{
    assert(pdf_editing() && !strcmp(prompt.in.text, "after0123456789A") && utf8_count(prompt.in.text) == 16);
    draw();
    assert(on_screen("16/16"));
}

static void forms_maxlen(void)
{
    assert(!strcmp(value_of("text"), "after0123456789A"));
    printf("PASS forms: MaxLen 16 stops typing at 16 characters (16/16 shown), as Okular's line edit\n");
}

static void forms_rejected(void)
{
    assert(!strcmp(value_of("text"), "after0123456789A") && ui.toast_error && strstr(ui.toast, "\"bad\" was not accepted"));
}

static void forms_key_rejected(void)
{
    assert(!strcmp(value_of("text"), "after0123456789A") && ui.toast_error && strstr(ui.toast, "\"keybad\" was not accepted"));
    printf("PASS forms: the document's validation and keystroke scripts reject \"bad\" and \"keybad\": shown, the value kept\n");
}

static void forms_checked(void)
{
    assert(!strcmp(value_of("check"), "Yes"));
}

static void forms_unchecked(void)
{
    assert(!strcmp(value_of("check"), "Off"));
    printf("PASS forms: check box toggled by Space and by a click\n");
}

static void forms_radio_one(void)
{
    assert(!strcmp(field_named("radio", 0, NULL)->value, "One") && !strcmp(field_named("radio", 1, NULL)->value, "One"));
}

static void forms_radio_two(void)
{
    assert(!strcmp(field_named("radio", 0, NULL)->value, "Two"));
}

static void forms_radio_clicked(void)
{
    assert(!strcmp(field_named("radio", 1, NULL)->value, "One"));
    printf("PASS forms: radio buttons chosen by Space and by a click, one of the group at a time\n");
}

static void forms_combo_down(void)
{
    assert(!strcmp(value_of("combo"), "b") && chosen("combo", "b") == 1);
}

static void forms_combo_rejected(void)
{
    assert(!strcmp(value_of("combo"), "b") && strstr(ui.toast, "\"c\" was not accepted"));
    printf("PASS forms: combo box: Down chooses the next option; Gamma from its menu rejected by validation, Beta kept\n");
}

static void forms_list(void)
{
    assert(chosen("list", "a") == 1 && chosen("list", "c") == 1 && chosen("list", "b") == 0);
}

static void forms_list_rejected(void)
{
    assert(chosen("validated-list", "a") == 1 && chosen("validated-list", "b") == 0 && ui.toast_error &&
           strstr(ui.toast, "was not accepted"));
    printf("PASS forms: list box: Gamma added to Alpha; a validated multiple selection is refused and shown\n");
}

static void forms_readonly(void)
{
    assert(strstr(ui.toast, "readonly is read-only") && !strcmp(value_of("readonly"), "locked"));
}

static void forms_undone(void)
{
    assert(chosen("list", "c") == 0 && chosen("list", "a") == 1 && (pdf.flags & VIEW_PDF_CAN_REDO));
}

static void forms_redone(void)
{
    assert(chosen("list", "c") == 1);
    printf("PASS forms: read-only field refused on a click; Ctrl+Z, Ctrl+Shift+Z undo and redo the last fill\n");
}

static void forms_saved_as(void)
{
    struct stat st;
    assert(!strcmp(doc.path, out_file("forms-saved.pdf")) && !pdf_modified() && strstr(ui.toast, "Saved forms-saved.pdf"));
    assert(stat(out_file("forms-saved.pdf"), &st) == 0 && st.st_uid == VIEW_USER && tabs.n == 1 && !strcmp(tabs.path[0], doc.path));
    draw();
    assert(!on_screen("[modified]"));
    printf("PASS forms: Save As (Ctrl+Shift+S) to a new file as uid %d; the document is that file now\n", (int)st.st_uid);
}

static void forms_reopened(void)
{
    assert(doc.page[0].fields == 2 && !strcmp(value_of("text"), "after0123456789A") && !strcmp(value_of("check"), "Off"));
    assert(!strcmp(field_named("radio", 0, NULL)->value, "One") && !strcmp(value_of("combo"), "b"));
    assert(chosen("list", "a") == 1 && chosen("list", "c") == 1 && !pdf_modified());
    printf("PASS forms: reopened, the saved text, check box, radio, combo and list values are there\n");
}

/* Unsaved changes: asked about; a failed save keeps them; discard, save. */
static void guard_typed(void)
{
    assert(pdf.forms && !strcmp(value_of("text"), "first") && pdf_modified());
}

static void guard_asked(void)
{
    assert(prompt.kind == P_PDF_UNSAVED && on_screen("Do you want to save your changes to \"forms-guard.pdf\" or discard them?"));
}

static void guard_cancelled(void)
{
    assert(prompt.kind == P_NONE && doc.open && pdf_modified() && !ui.quit);
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s' && chmod 700 '%s'", out_file("root-only"), out_file("root-only"));
    assert(system(cmd) == 0);
    snprintf(pick_out, sizeof pick_out, "%s", out_file("root-only"));
}

static void guard_save_failed(void)
{
    assert(ui.toast_error && strstr(ui.toast, "File could not be saved in") && pdf_modified() && doc.open);
    assert(!strcmp(value_of("text"), "first") && !strcmp(doc.path, out_file("forms-guard.pdf")));
    snprintf(pick_out, sizeof pick_out, "%s", out_dir);
    printf("PASS save error: a folder uid 1000 cannot write to: the error shown, the document and its changes kept\n");
}

static void guard_discarded(void)
{
    assert(doc.open && !pdf_modified() && doc.page[0].fields == 2 && !strcmp(value_of("text"), "before"));
    printf("PASS unsaved changes: Ctrl+Q asks (Okular's save or discard), Esc keeps them; F5, Discard reloads the file\n");
}

static void guard_reopened(void)
{
    assert(!strcmp(value_of("text"), "second") && !pdf_modified());
    printf("PASS unsaved changes: Ctrl+Q, Save: saved over the file, then quit; reopened with the value\n");
}

/* Annotations on a page of text: each tool, then changed, saved, reopened. */
static int count_type(int type)
{
    int n = 0;
    for (int k = 0; k < doc.page[0].nannot; k++) n += doc.page[0].annot[k].type == type;
    return n;
}

static const struct view_annot *annot_of(int type, int nth)
{
    for (int k = 0; k < doc.page[0].nannot; k++)
        if (doc.page[0].annot[k].type == type && nth-- == 0) return &doc.page[0].annot[k];
    return NULL;
}

static void annots_start(void)
{
    assert(pdf_on() && pdf.toolbar && on_screen("[Highlighter]") && on_screen("[Polygon]") && doc.page[0].nannot == 0);
    assert(v.mode == VL_FIT_PAGE);
}

static void annots_pick(int tool)
{
    press_id(HIT_PDF_TOOL + tool);
    assert(pdf.tool == tool);
}

static void pick_ellipse(void) { annots_pick(T_ELLIPSE); }
static void pick_polygon(void) { annots_pick(T_POLYGON); }
static void pick_stamp(void) { annots_pick(T_STAMP); }

/* A red pixel within 4 of page i's point, along its row (a stroke's antialiased edges). */
static int red_near(int i, double x, double y)
{
    int X, Y;
    spot(i, x, y, &X, &Y);
    assert(X >= ax + 4 && X < ax + aw - 4 && Y >= ay && Y < ay + ah);
    for (int dx = -4; dx <= 4; dx++) if (red(pic[Y][X + dx])) return 1;
    return 0;
}

static void annots_made(void)
{
    static const int types[] = { VIEW_ANNOT_HIGHLIGHT, VIEW_ANNOT_UNDERLINE, VIEW_ANNOT_SQUIGGLY, VIEW_ANNOT_STRIKE_OUT,
        VIEW_ANNOT_TEXT, VIEW_ANNOT_INK, VIEW_ANNOT_LINE, VIEW_ANNOT_SQUARE, VIEW_ANNOT_CIRCLE, VIEW_ANNOT_POLYGON, VIEW_ANNOT_STAMP };
    if (doc.page[0].nannot != 13) {
        for (int k = 0; k < doc.page[0].nannot; k++) printf("annotation %d: type %d %s\n", k, doc.page[0].annot[k].type, doc.page[0].annot[k].contents);
    }
    assert(doc.page[0].nannot == 13 && count_type(VIEW_ANNOT_FREE_TEXT) == 2);
    for (size_t k = 0; k < sizeof types / sizeof *types; k++) assert(count_type(types[k]) == 1);
    assert(!strcmp(annot_of(VIEW_ANNOT_FREE_TEXT, 0)->contents, "Typed") && !strcmp(annot_of(VIEW_ANNOT_FREE_TEXT, 1)->contents, "Inline"));
    assert(!strcmp(annot_of(VIEW_ANNOT_TEXT, 0)->contents, "Popup") && !strcmp(annot_of(VIEW_ANNOT_SQUARE, 0)->author, user_name()));
    const struct view_annot *r = annot_of(VIEW_ANNOT_SQUARE, 0);
    /* Within the pointer's precision: a pixel is about 1.2 points at Fit Page. */
    assert(fabsf(r->x0 - 250) < 3 && fabsf(r->y0 - 500) < 3 && fabsf(r->x1 - 350) < 3 && fabsf(r->y1 - 560) < 3);
    assert(red_near(0, 250.5, 530) && pdf_modified());
    assert(on_screen("[modified]"));
    printf("PASS annotations: the pointer made all 13 the backend makes (Highlighter, Underline, Squiggle, Strike Out, "
           "Typewriter, Inline Note, Popup Note, Freehand Line, Straight Line, Rectangle, Ellipse, Polygon, Stamp); "
           "the rectangle drawn red\n");
}

static void annots_moved(void)
{
    const struct view_annot *r = annot_of(VIEW_ANNOT_SQUARE, 0);
    assert(r && fabsf(r->x0 - 270) < 3 && fabsf(r->y0 - 530) < 3 && fabsf(r->x1 - 370) < 3 && fabsf(r->y1 - 590) < 3);
    assert(pdf.sobject == r->object && red_near(0, 270.5, 542));
    printf("PASS annotations: a rectangle dragged in Browse moved by (20, 30) points, redrawn there\n");
}

static void annots_resized(void)
{
    const struct view_annot *e = annot_of(VIEW_ANNOT_CIRCLE, 0);
    assert(e && fabsf(e->x0 - 400) < 3 && fabsf(e->y0 - 500) < 3 && fabsf(e->x1 - 520) < 3 && fabsf(e->y1 - 600) < 3);
    printf("PASS annotations: an ellipse resized by its bottom right handle to 120 x 100 points\n");
}

static void annots_deleted(void)
{
    assert(doc.page[0].nannot == 12 && !count_type(VIEW_ANNOT_STAMP) && !pdf.sobject);
}

static void annots_undone(void)
{
    assert(doc.page[0].nannot == 13 && count_type(VIEW_ANNOT_STAMP) == 1);
}

static void annots_redone(void)
{
    assert(doc.page[0].nannot == 12 && !count_type(VIEW_ANNOT_STAMP));
    printf("PASS annotations: Del deletes the selected stamp; Ctrl+Z brings it back, Ctrl+Shift+Z deletes it again\n");
}

static void annots_recolored(void)
{
    const struct view_annot *r = annot_of(VIEW_ANNOT_SQUARE, 0);
    assert(r->color_n == 3 && r->color[0] < .01 && r->color[1] < .01 && r->color[2] > .99);
    printf("PASS annotations: the right button's Properties, Color: Blue\n");
}

static void annots_properties(void)
{
    const struct view_annot *r = annot_of(VIEW_ANNOT_SQUARE, 0);
    assert(fabsf(r->opacity - .5f) < .01 && fabsf(r->width - 4) < .01 && !strcmp(r->author, "Reviewer"));
    printf("PASS annotations: Properties: opacity 50%%, width 4, author edited\n");
}

static void annots_icon(void)
{
    assert(!strcmp(annot_of(VIEW_ANNOT_TEXT, 0)->icon, "Help"));
    printf("PASS annotations: a pop-up note's Properties: its icon set to Help\n");
}

static void annots_edited(void)
{
    assert(!strcmp(annot_of(VIEW_ANNOT_TEXT, 0)->contents, "Popup edited"));
    printf("PASS annotations: a double click opens the pop-up note's text; edited\n");
}

static void annots_panel(void)
{
    settings.side_width = 480;
    sidebar(1);
    draw();
    press_id(HIT_TAB + TAB_ANNOTATIONS);
    assert(ui.tab == TAB_ANNOTATIONS);
}

static void annots_panel_listed(void)
{
    draw();
    assert(on_screen("Annotations") && on_screen("Page 1") && on_screen("  Typed") && on_screen("  Popup edited") &&
           on_screen("  Highlight") && on_screen("  Freehand Line") && on_screen("  Polygon"));
    int loading, n = annot_rows(&loading);
    assert(n == 13 && !loading);
    for (int k = 0; k < n; k++)
        if (arows[k].index >= 0 && !strcmp(doc.page[0].annot[arows[k].index].contents, "Inline")) press_id(HIT_PDF_ANNOT + k);
    const struct view_annot *s = selected();
    assert(s && !strcmp(s->contents, "Inline") && ui.focus == FOCUS_SIDEBAR);
    printf("PASS annotations panel: Page 1 and its 12 by their text or caption; a click selects one\n");
}

static void annots_saved(void)
{
    assert(!pdf_modified() && strstr(ui.toast, "Saved annots.pdf"));
}

static void annots_reopened(void)
{
    assert(doc.page[0].annots == 2 && doc.page[0].nannot == 12 && !count_type(VIEW_ANNOT_STAMP));
    const struct view_annot *r = annot_of(VIEW_ANNOT_SQUARE, 0), *e = annot_of(VIEW_ANNOT_CIRCLE, 0);
    assert(fabsf(r->x0 - 270) < 3 && r->color[2] > .99 && fabsf(r->opacity - .5f) < .01 && !strcmp(r->author, "Reviewer") &&
           fabsf(e->x1 - 520) < 3);
    assert(!strcmp(annot_of(VIEW_ANNOT_TEXT, 0)->contents, "Popup edited") && count_type(VIEW_ANNOT_INK) == 1 &&
           count_type(VIEW_ANNOT_HIGHLIGHT) == 1 && count_type(VIEW_ANNOT_POLYGON) == 1);
    printf("PASS annotations: saved (Ctrl+S) over the file and reopened: 12, moved, resized, recoloured, edited\n");
}

/* An encrypted PDF: filled, saved, still asking for its password. */
static void enc_filled(void)
{
    assert(!strcmp(value_of("text"), "encrypted") && pdf_modified());
}

static void enc_reopened(void)
{
    assert(doc.open && !strcmp(value_of("text"), "encrypted"));
    printf("PASS encryption: filled, saved as a new file; reopened, it asks for the password, the value kept\n");
}

/* A signed PDF: its signature listed as the backend reports it, never valid. */
static void signed_opened(void)
{
    assert(pdf_on() && pdf.nsig_fields == 1 && (pdf.flags & VIEW_PDF_HAS_SIGNED) && pdf.sigs == 2 && pdf.nsig == 1);
    sidebar(0);
    draw();
    assert(on_screen("This document is digitally signed. Some of the signatures could not be validated properly."));
    settings.side_width = 480;
    sidebar(1);
    draw();
    press_id(HIT_PDF_BAR + 2); /* Show Signatures Panel */
    assert(ui.tab == TAB_SIGNATURES);
    draw();
    assert(on_screen("Rev. 1: Signed By Generated test signer") && on_screen("(the name the document gives, not verified)"));
    assert(on_screen("Validity: unavailable") && on_screen("Certificate has not yet been verified."));
    assert(on_screen("Changes: unchanged") && on_screen("Reason: Backend tests") && on_screen("Field: signed-fixture on page 1"));
    assert(!on_screen("cryptographically valid") && !on_screen("Trusted"));
    printf("PASS signatures: the signatures bar and panel: signer name, reason, field; validity unavailable, never valid; unchanged\n");
}

static void signed_changed(void)
{
    assert(pdf_modified() && pdf.sigs == 2 && pdf.sig[0].modified);
    draw();
    assert(on_screen("The document has unsaved changes."));
}

static void signed_consent(void)
{
    assert(prompt.kind == P_PDF_CONSENT && on_screen("invalidate the digital signatures"));
}

static void signed_refused(void)
{
    assert(prompt.kind == P_NONE && pdf_modified() && strstr(ui.toast, "Not saved"));
}

static void signed_saved(void)
{
    assert(!pdf_modified() && strstr(ui.toast, "Saved signed-ui.pdf"));
    /* The signed bytes kept: an incremental save after them. */
    char fixture[PATH_MAX];
    snprintf(fixture, sizeof fixture, "%s/signed.pdf", pdf_dir);
    FILE *a = fopen(fixture, "rb"), *b = fopen(out_file("signed-ui.pdf"), "rb");
    assert(a && b);
    int same = 1;
    long n = 0;
    for (int c; (c = fgetc(a)) != EOF; n++) same &= c == fgetc(b);
    fseek(b, 0, SEEK_END);
    assert(same && ftell(b) > n);
    fclose(a);
    fclose(b);
    printf("PASS signatures: saving a changed signed PDF asks consent; no keeps it unsaved, yes saves after the signed bytes\n");
}

static void parity_press_settings(void) { draw(); press_id(HIT_MENU + 5); }
static void parity_sidebar_menu_hidden(void) { assert(!settings.sidebar && ax == 0); draw(); press_id(HIT_SIDEBAR); assert(settings.sidebar && side_px > 0); }

/* ---- Okular's other mouse tools, presentation, colour modes, overview
 * columns, file lifecycle, prompts and reflow-stable bookmarks. ---- */

/* A PDF of n Letter pages, page i's content stream content(i) (Helvetica F1). */
static void write_pdf(const char *path, int n, const char *(*content)(int))
{
    FILE *f = fopen(path, "w");
    assert(f && n <= 20);
    long off[3 + 2 * 20];
    int objs = 3 + 2 * n;
    fputs("%PDF-1.4\n", f);
    off[1] = ftell(f);
    fputs("1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n", f);
    off[2] = ftell(f);
    fputs("2 0 obj << /Type /Pages /Kids [", f);
    for (int i = 0; i < n; i++) fprintf(f, "%d 0 R ", 4 + 2 * i);
    fprintf(f, "] /Count %d >> endobj\n", n);
    off[3] = ftell(f);
    fputs("3 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n", f);
    for (int i = 0; i < n; i++) {
        const char *c = content(i);
        off[4 + 2 * i] = ftell(f);
        fprintf(f, "%d 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> "
                   "/Contents %d 0 R >> endobj\n", 4 + 2 * i, 5 + 2 * i);
        off[5 + 2 * i] = ftell(f);
        fprintf(f, "%d 0 obj << /Length %zu >> stream\n%s\nendstream endobj\n", 5 + 2 * i, strlen(c), c);
    }
    long xref = ftell(f);
    fprintf(f, "xref\n0 %d\n0000000000 65535 f \n", objs + 1);
    for (int i = 1; i <= objs; i++) fprintf(f, "%010ld 00000 n \n", off[i]);
    fprintf(f, "trailer << /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", objs + 1, xref);
    fclose(f);
    struct passwd *pw = getpwuid(VIEW_USER);
    assert(pw && chown(path, VIEW_USER, pw->pw_gid) == 0);
}

/* table.pdf: three columns of three rows (baselines 92, 112, 132 from the top). */
static const char *table_page(int i)
{
    (void)i;
    return "BT /F1 12 Tf 72 700 Td (Name) Tj ET BT /F1 12 Tf 200 700 Td (Count) Tj ET BT /F1 12 Tf 330 700 Td (Colour) Tj ET\n"
           "BT /F1 12 Tf 72 680 Td (Alpha) Tj ET BT /F1 12 Tf 200 680 Td (1) Tj ET BT /F1 12 Tf 330 680 Td (red) Tj ET\n"
           "BT /F1 12 Tf 72 660 Td (Beta) Tj ET BT /F1 12 Tf 200 660 Td (22) Tj ET BT /F1 12 Tf 330 660 Td (green) Tj ET";
}

/* pages.pdf: ten pages, each its number in large type. */
static const char *numbered_page(int i)
{
    static char c[10][96];
    snprintf(c[i], sizeof c[i], "BT /F1 96 Tf 200 400 Td (%d) Tj ET 0 0 1 rg 50 50 40 40 re f", i + 1);
    return c[i];
}

/* A drag with the left (or right) button between two of page i's points. */
static void page_drag(int i, double x0, double y0, double x1, double y1, int button)
{
    page_pointer(i, x0, y0, button, 1, 1);
    page_pointer(i, (x0 + x1) / 2, (y0 + y1) / 2, button, -1, 0);
    page_pointer(i, x1, y1, button, -1, 0);
    page_pointer(i, x1, y1, button, 0, 0);
}

static void tools_menu(void)
{
    /* Okular's Tools menu (part.rc): Browse, Zoom, Area Selection, Text
     * Selection, Table Selection, Magnifier, each with its Ctrl+N. */
    struct item it[MENU_ITEMS];
    int n = menu_items(4, it);
    static const char *const want[] = { "Browse", "Zoom", "Area Selection", "Text Selection", "Table Selection", "Magnifier" };
    static const char *const keys_[] = { "Ctrl+1", "Ctrl+2", "Ctrl+3", "Ctrl+4", "Ctrl+5", "Ctrl+6" };
    assert(n >= 6);
    for (int k = 0; k < 6; k++) assert(!strcmp(it[k].label, want[k]) && !strcmp(it[k].keys, keys_[k]));
}

static double zoom_before_tool, zoom_nx, zoom_ny;

static void zoom_tool_on(void)
{
    assert(settings.tool == TOOL_ZOOM && strstr(ui.toast, "Select zooming area"));
    zoom_before_tool = zoom_now();
    /* Where the rectangle's middle is in the content, normalized (Okular's nX, nY). */
    int X0, Y0, X1, Y1;
    spot(0, 400, 92, &X0, &Y0);
    spot(0, 500, 192, &X1, &Y1);
    zoom_nx = (X0 + X1 - 2 * ax + 2 * v.sx) / (2.0 * doc.full_w);
    zoom_ny = (Y0 + Y1 - 2 * ay + 2 * v.sy) / (2.0 * doc.full_h);
}

static void zoomed_rect(void)
{
    /* The square (100 points) dragged round: Okular's zoom into it, its
     * middle in the middle of the area; the area is wider than high. */
    int X, Y;
    spot(0, 450, 142, &X, &Y);
    assert(v.mode == VL_FIXED && zoom_now() > zoom_before_tool * 2);
    double side = fmin(aw, ah); /* the rectangle's longer way fills the area */
    if (fabs(100 * doc.item[0].zoom - side) >= 0.05 * side) printf("zoomed: item zoom %.3f aw %d ah %d\n", doc.item[0].zoom, aw, ah);
    assert(fabs(100 * doc.item[0].zoom - side) < 0.05 * side);
    int sx = clampi((int)(zoom_nx * doc.full_w) - aw / 2, 0, doc.full_w - aw > 0 ? doc.full_w - aw : 0);
    int sy = clampi((int)(zoom_ny * doc.full_h) - ah / 2, 0, doc.full_h - ah > 0 ? doc.full_h - ah : 0);
    int n = 0;
    for (int y = ay; y < ay + ah; y += 4) for (int x = ax; x < ax + aw; x += 4) n += red(pic[y][x]);
    if (abs(v.sx - sx) > 1 || abs(v.sy - sy) > 1) printf("zoom centre: sx %d want %d, sy %d want %d (X %d Y %d)\n", v.sx, sx, v.sy, sy, X, Y);
    assert(abs(v.sx - sx) <= 1 && abs(v.sy - sy) <= 1 && n > (aw / 4) * (ah / 4) / 3);
    zoom_before_tool = zoom_now();
    printf("PASS Zoom tool (Ctrl+2): a dragged rectangle zoomed into (%.0f%%), its normalized middle at the area's middle "
           "(Okular's nX, nY); the square fills a third of the area\n", zoom_now() * 100);
}

static void zoom_tool_out(void)
{
    assert(zoom_now() < zoom_before_tool);
    printf("PASS Zoom tool: the right button zooms out a step (pageview.cpp ZoomOut)\n");
}

static void zoom_tiny(void)
{
    assert(fabs(zoom_now() - zoom_before_tool) < 1e-9);
}

static void area_tool_on(void)
{
    assert(settings.tool == TOOL_AREA && strstr(ui.toast, "Draw a rectangle around the text/graphics"));
}

static void area_copied(void)
{
    assert(!strcmp(clip[0], "Hello LOCachy viewer\nSearch target: quokka") && !strcmp(clip[1], clip[0]));
    printf("PASS Area Selection (Ctrl+3): its menu's Copy to Clipboard: \"%s\" (clipboard and primary)\n", clip[0]);
}

static void area_flat(void)
{
    assert(!strcmp(clip[0], "Hello LOCachy viewer Search target: quokka"));
    printf("PASS Area Selection: Copy Text (Without line breaks): \"%s\"\n", clip[0]);
}

static void area_searched(void)
{
    assert(find.bar && !strcmp(find.active, "Search target: quokka") && find.cur_page == 0);
    find_bar(0);
    printf("PASS Area Selection: Search for 'Search target: quokka' in this document: found on page 1\n");
}

static void area_image_refused(void)
{
    assert(ui.toast_error && strstr(ui.toast, "The clipboard takes text only"));
    printf("PASS Area Selection: Copy to Clipboard of the image: refused and shown (the clipboard carries text only)\n");
}

/* The saved PNG: its size and, decoded (stored blocks), a red middle. */
static void area_image_saved(void)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/selection.png", out_dir);
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_uid == VIEW_USER && strstr(ui.toast, "saved to PNG file"));
    FILE *f = fopen(path, "rb");
    assert(f);
    unsigned char *png = malloc((size_t)st.st_size);
    assert(png && fread(png, 1, (size_t)st.st_size, f) == (size_t)st.st_size);
    fclose(f);
    assert(!memcmp(png, "\x89PNG\r\n\x1a\n", 8) && !memcmp(png + 12, "IHDR", 4));
    int w = png[16] << 24 | png[17] << 16 | png[18] << 8 | png[19], h = png[20] << 24 | png[21] << 16 | png[22] << 8 | png[23];
    assert(w == capture.w && h == capture.h && w > 50 && h > 50);
    size_t idat = 33;
    assert(!memcmp(png + idat + 4, "IDAT", 4));
    size_t len = (size_t)png[idat] << 24 | (size_t)png[idat + 1] << 16 | (size_t)png[idat + 2] << 8 | png[idat + 3];
    const unsigned char *z = png + idat + 8 + 2;
    size_t row = 1 + 3 * (size_t)w, raw = row * (size_t)h, got = 0;
    unsigned char *pix = malloc(raw);
    assert(pix);
    while (got < raw) {
        size_t n = z[1] | (size_t)z[2] << 8;
        assert((z[0] & 6) == 0 && (unsigned)(z[3] | z[4] << 8) == (~n & 0xFFFF));
        memcpy(pix + got, z + 5, n);
        got += n;
        z += 5 + n;
    }
    assert(z + 4 == png + idat + 8 + len);
    const unsigned char *mid = pix + (size_t)(h / 2) * row + 1 + 3 * (size_t)(w / 2), *corner = pix + 1;
    assert(mid[0] > 0xC0 && mid[1] < 0x50 && mid[2] < 0x50 && corner[0] == 0xFF && corner[1] == 0xFF);
    free(pix);
    free(png);
    printf("PASS Area Selection: Save to File: %s, %dx%d PNG as uid %d, the square red in it\n", path, w, h, (int)st.st_uid);
}

static void browse_right_area(void)
{
    assert(!strcmp(clip[0], "Search target: quokka") && settings.tool == TOOL_BROWSE);
    printf("PASS Browse: the right button dragged is Area Selection (its menu), then Browse again\n");
}

static const char *table_want = "Name\tCount\tColour\nAlpha\t1\tred\nBeta\t22\tgreen\n";

static void table_guessed(void)
{
    if (strcmp(clip[1], table_want)) printf("table primary: [%s] cols %d rows %d words %d\n", clip[1], table.ncols, table.nrows, words.n);
    assert(settings.tool == TOOL_TABLE && table.on && table.guessed && table.ncols == 2 && table.nrows == 2);
    assert(!strcmp(clip[1], table_want));
    printf("PASS Table Selection (Ctrl+5): dividers guessed between the words (2 columns, 2 rows); the table to the primary selection\n");
}

static void table_divided(void)
{
    assert(table.ncols == 3 && !table.guessed);
}

static void table_undivided(void)
{
    assert(table.ncols == 2);
    printf("PASS Table Selection: a click near the top edge adds a column divider, a click on it takes it away\n");
}

static void table_copied(void)
{
    assert(!strcmp(clip[0], table_want));
    printf("PASS Table Selection: the right button's Copy Table Contents to Clipboard: tab-separated cells\n");
}

static void table_escaped(void)
{
    assert(table.on && !table.ncols && !table.nrows);
}

static void table_gone(void)
{
    assert(!table.on);
    printf("PASS Table Selection: Esc clears the guessed dividers, Esc again the table\n");
}

/* The magnifier's pixel dx, dy from its middle. */
static uint32_t mag_at(int dx, int dy)
{
    int x, y;
    mag_place(&x, &y);
    return pic[y + mag.h / 2 + dy][x + mag.w / 2 + dx];
}

static void magnified_in(void)
{
    assert(settings.tool == TOOL_MAGNIFIER && mag.on && mag.done_seq == mag.seq && mag.w == MAG_W && mag.h == MAG_H);
    assert(red(mag_at(30, 30)) && red(mag_at(-150, -80)) && mag_at(0, 30) == 0 && mag_at(30, 0) == 0);
}

static void magnified_edge(void)
{
    /* At the square's left edge (2 points in): at 10 times 100% a point is
     * 13.3 pixels; the left of the magnifier is off the square. */
    assert(mag.on && mag.done_seq == mag.seq);
    assert(!red(mag_at(-60, 30)) && red(mag_at(10, 30)));
    printf("PASS Magnifier (Ctrl+6): while pressed, the page at 10 times (Okular's SCALE) round the pointer, its cross and ticks; it follows the pointer\n");
}

static void magnifier_gone(void)
{
    assert(!mag.on);
}

/* The presentation's page: page i's point on the screen. */
static void pres_spot(double x, double y, int *X, int *Y)
{
    int w, h, x0, y0;
    double z = pres_zoom(pres.page, &w, &h, &x0, &y0), ix, iy;
    vl_point_to_item(v.rotate, doc.pw[pres.page], doc.ph[pres.page], z, x, y, &ix, &iy);
    *X = x0 + (int)ix;
    *Y = y0 + (int)iy;
}

static int slide_drawn(void)
{
    int w, h, x0, y0;
    float z = (float)pres_zoom(pres.page, &w, &h, &x0, &y0);
    return pres.on && pix_find(2, pres.page, v.rotate, z, -1, -1) >= 0 && !fly.busy && !nqueue;
}

static void pres_first(void)
{
    int X, Y, w, h, x0, y0;
    pres_zoom(0, &w, &h, &x0, &y0);
    pres_spot(450, 142, &X, &Y);
    assert(pres.on && pres.page == 0 && red(pic[Y][X]) && pic[2][2] == 0 && pic[y0 + 5][x0 + 5] == PAPER);
    assert(h == SH && w == (int)(612 * (SH / 792.0) + .5) && x0 == (SW - w) / 2); /* fit to the screen */
    int side = SW / 16, ox = SW - side - 4, oy = 4;
    assert(pic[oy + side / 2][ox + side / 2] == 0 && pic[oy + side / 8][ox + side / 2] == SELECTED && on_screen("1"));
    printf("PASS presentation (Ctrl+Shift+P): page 1 fit to the whole screen on black, the page indicator lit in the top right\n");
}

static void pres_second(void)
{
    int X, Y;
    pres_spot(450, 142, &X, &Y);
    assert(pres.page == 1 && !red(pic[Y][X]));
}

static void pres_keys(void)
{
    assert(pres.page == 1);
    printf("PASS presentation: Right, Left, Space, Backspace, PgDn, PgUp, Down, Up, Home, End move as presentationwidget.cpp's keyPressEvent\n");
}

static void pres_page0(void) { assert(pres.page == 0); }
static void pres_page1(void) { assert(pres.page == 1); }

static void pres_pointer_done(void)
{
    assert(pres.page == 0);
    printf("PASS presentation: the left button the next page, the right the previous; the wheel a page a notch\n");
}

static void pres_black(void)
{
    int X, Y;
    pres_spot(300, 400, &X, &Y);
    assert(pres.black && pic[Y][X] == 0);
}

static void pres_unblack(void)
{
    int X, Y;
    pres_spot(300, 400, &X, &Y);
    assert(!pres.black && pic[Y][X] == PAPER);
    printf("PASS presentation: B the black screen (Switch Blackscreen Mode) and back\n");
}

static void pres_bar(void)
{
    assert(pres.bar && on_screen("Exit Presentation Mode") && on_screen("of 2"));
    press_id(HIT_PRES + 2);
}

static void pres_bar_next(void)
{
    assert(pres.page == 1 && pres.bar);
}

static void pres_left(void)
{
    assert(!pres.on && current_page() == 1 && !on_screen("Exit Presentation Mode"));
    printf("PASS presentation: the top bar at the screen's top (its Next Page), Esc hides it, Esc again leaves at the page shown\n");
}

static void pres_link_press(void)
{
    assert(pres.on && pres.page == 0 && doc.page[0].links == 2);
    int X, Y;
    pres_spot(100, 287, &X, &Y);
    struct pointer_event e = { .x = X, .y = Y, .col = X / CW, .row = (Y - TOP) / CH, .button = BTN_LEFT, .pressed = 1, .clicks = 1 };
    pres_pointer(&e);
    e.pressed = 0;
    pres_pointer(&e);
}

static void pres_link_followed(void)
{
    assert(pres.page == 1 && pres.on);
    printf("PASS presentation: a click on an internal link goes to its page\n");
}

/* Colours: the square's middle and the paper at page 1's (300, 400). */
static void color_keys(int k)
{
    key('v', KEYS_ALT);
    downs(18); /* Color Mode... */
    key('\n', 0);
    downs(k);
    key('\n', 0);
}

static int near_rgb(uint32_t c, int r, int g, int b)
{
    return abs((int)(c >> 16 & 0xFF) - r) <= 2 && abs((int)(c >> 8 & 0xFF) - g) <= 2 && abs((int)(c & 0xFF) - b) <= 2;
}

static uint32_t sq(void) { return at_point(0, 450, 142); }
static uint32_t paper_px(void) { return at_point(0, 300, 400); }

static void colors_normal(void)
{
    assert(!settings.invert && near_rgb(sq(), 255, 0, 0) && paper_px() == PAPER);
}

static void colors_inverted(void)
{
    assert(settings.invert && settings.render_mode == CM_INVERTED && near_rgb(sq(), 0, 255, 255) && paper_px() == 0);
}

static void colors_off_again(void)
{
    assert(!settings.invert && settings.render_mode == CM_INVERTED && near_rgb(sq(), 255, 0, 0));
}

static void colors_recolor(void)
{
    /* recolor(): qGray(255, 0, 0) is 87; dark 0x600000, light 0xF0F0F0. */
    assert(settings.render_mode == CM_RECOLOR && near_rgb(sq(), 145, 81, 81) && near_rgb(paper_px(), 0xF0, 0xF0, 0xF0));
}

static void colors_bw(void)
{
    assert(settings.render_mode == CM_BLACK_WHITE && near_rgb(sq(), 87, 87, 87) && paper_px() == 0xFFFFFF);
}

static void colors_lightness(void)
{
    assert(settings.render_mode == CM_INVERT_LIGHTNESS && near_rgb(sq(), 255, 0, 0) && paper_px() == 0);
}

static void colors_luma(void)
{
    assert(settings.render_mode == CM_INVERT_LUMA && near_rgb(sq(), 255, 186, 186) && paper_px() == 0);
}

static void colors_luma_sym(void)
{
    assert(settings.render_mode == CM_INVERT_LUMA_SYMMETRIC && near_rgb(sq(), 255, 128, 128) && paper_px() == 0);
}

static void colors_hue_pos(void)
{
    assert(settings.render_mode == CM_HUE_POSITIVE && near_rgb(sq(), 0, 255, 0) && paper_px() == 0xFFFFFF);
}

static void colors_hue_neg(void)
{
    assert(settings.render_mode == CM_HUE_NEGATIVE && near_rgb(sq(), 0, 0, 255));
    printf("PASS Color Mode: Invert Colors, Change Dark & Light Colors, Convert to Black & White, Invert Lightness, "
           "Invert Luma (sRGB Linear and Symmetric), Shift Hue Positive and Negative: the page's pixels as pagepainter.cpp makes them\n");
}

static void colors_paper_set(void)
{
    assert(settings.paper == 0xF0E0C0 && settings.render_mode == CM_HUE_NEGATIVE);
}

static int paper_drawn(void)
{
    return !fly.busy && !nqueue && pix_find(0, 0, v.rotate, (float)doc.item[0].zoom, -1, -1) >= 0;
}

static void colors_paper(void)
{
    assert(settings.render_mode == CM_PAPER && settings.invert && paper_px() == 0xF0E0C0 && near_rgb(sq(), 255, 0, 0));
    printf("PASS Color Mode: Configure Color Modes' paper colour typed (#F0E0C0); Change Paper Color draws the page on it (view-render)\n");
}

static void colors_toggle_off(void)
{
    assert(!settings.invert && paper_px() == PAPER);
}

static void colors_toggle_on(void)
{
    assert(settings.invert && settings.render_mode == CM_PAPER && paper_px() == 0xF0E0C0);
    printf("PASS Change Colors: the toggle turns the chosen mode off and on; Normal Colors turns it off\n");
}

static void colors_saved(void)
{
    state_save();
    char *stored = state_read();
    assert(stored && strstr(stored, "invert=1 ") && strstr(stored, "render_mode=1 ") && strstr(stored, "paper=f0e0c0 "));
    free(stored);
    struct saved s;
    settings.render_mode = 0;
    settings.paper = 0;
    state_load(NULL, &s);
    assert(settings.render_mode == CM_PAPER && settings.paper == 0xF0E0C0);
    do_action(A_COLOR_NORMAL);
}

/* Overview: columns 1 to 8 (here 4, of pages.pdf's ten), right to left. */
static void overview_four(void)
{
    assert(v.facing == VL_OVERVIEW && settings.columns == 4 && columns() == 4);
    for (int i = 1; i < 4; i++) assert(doc.item[i].y == doc.item[0].y && doc.item[i].x > doc.item[i - 1].x);
    assert(doc.item[4].y > doc.item[0].y && doc.item[4].x == doc.item[0].x && inked(0) > 100 && inked(3) > 100);
}

static void overview_rtl(void)
{
    assert(settings.rtl && doc.item[1].x < doc.item[0].x && doc.item[3].x < doc.item[2].x && doc.item[4].x == doc.item[0].x);
    assert(doc.item[0].x > doc.item[3].x && inked(0) > 100);
    printf("PASS Overview: View > Overview Columns 4 (okular.kcfg ViewColumns), four pages a row; Use Right to Left Reading Direction mirrors them\n");
}

static void overview_saved(void)
{
    state_save();
    char *stored = state_read();
    assert(stored && strstr(stored, "columns=4 ") && strstr(stored, "rtl=1\t"));
    free(stored);
    settings.rtl = 0;
    do_action(A_SINGLE);
}

/* File lifecycle: Open Recent (MaxRecentItems 10), Open Containing Folder,
 * each tab's own find, history and selection, no tabs restored at start. */
static char recent_file[12][PATH_MAX];

static void recent_started(void)
{
    assert(tabs.n == 1 && recent.n >= 1 && !strcmp(recent.path[0], doc.path));
}

static void recent_ten(void)
{
    assert(tabs.n == 12 && recent.n == RECENT_MAX && !strcmp(recent.path[0], recent_file[10]) && !strcmp(recent.path[9], recent_file[1]));
    for (int k = 0; k < recent.n; k++) assert(strcmp(recent.path[k], recent_file[0]));
    draw();
}

static void recent_opened(void)
{
    assert(!strcmp(doc.path, recent_file[1]) && !strcmp(recent.path[0], recent_file[1]) && tabs.n == 12);
    assert(!strcmp(recent.path[9], recent_file[2]) && unlink(recent_file[2]) == 0);
    printf("PASS File > Open Recent: the newest first, at most 10 (MaxRecentItems); choosing one opens it (its tab)\n");
}

static void recent_failed(void)
{
    assert(!doc.open && recent.n == RECENT_MAX - 1);
    for (int k = 0; k < recent.n; k++) assert(strcmp(recent.path[k], recent_file[2]));
}

static void recent_cleared(void)
{
    assert(recent.n == 0);
    state_save();
    char *stored = state_read();
    assert(stored && !strstr(stored, "recent\t"));
    free(stored);
    printf("PASS File > Open Recent: a file that cannot be opened leaves the list; Clear List empties it (and the state file)\n");
}

static void recent_kept(void)
{
    char *stored = state_read();
    char line[PATH_MAX + 16];
    snprintf(line, sizeof line, "recent\t%s\n", doc.path);
    assert(stored && strstr(stored, line));
    free(stored);
}

static void folder_asked(void)
{
    char want[PATH_MAX], got[PATH_MAX] = "";
    snprintf(want, sizeof want, "%s", doc.path);
    *strrchr(want, '/') = '\0';
    FILE *f = fopen(FILES_REQUEST, "r");
    assert(f && fgets(got, sizeof got, f));
    fclose(f);
    unlink(FILES_REQUEST);
    got[strcspn(got, "\n")] = '\0';
    assert(!strcmp(got, want) && home_asked == 1);
    printf("PASS File > Open Containing Folder: %s for home's file manager (FILES_REQUEST), home asked to the front\n", got);
}

static void tab_a_ready(void)
{
    assert(find.bar && !strcmp(find.active, "quokka") && find.cur_page == 0 && sel.active && !strcmp(clip[1], "Hello"));
    assert(hist.n == 2 && current_page() == 1);
}

static void tab_b_own(void)
{
    assert(tabs.n == 2 && tabs.current == 1 && !strcmp(doc.format, "Markdown"));
    assert(!find.bar && !find.in.text[0] && !find.active[0] && !sel.active && hist.n == 1);
    draw();
    press_id(HIT_DOCUMENT_TAB);
}

static void tab_a_back(void)
{
    size_t len;
    char *t = sel_text(&len);
    assert(tabs.current == 0 && find.bar && !strcmp(find.in.text, "quokka") && !strcmp(find.active, "quokka"));
    assert(find.cur_page == 0 && sel.active && t && !strcmp(t, "Hello") && hist.n == 2 && hist.i == 1 && current_page() == 1);
    free(t);
}

static void tab_a_history(void)
{
    assert(current_page() == 0 && doc.page[0].hits == 2);
    printf("PASS tabs: a tab's find bar and match, text selection and history are its own, back when it is shown again\n");
}

/* Prompts: the find bar's editor (UTF-8, selection, clipboard, pointer). */
static void prompt_rename(void)
{
    assert(prompt.kind == P_BOOKMARK && !strcmp(prompt.in.text, "Page 1") && prompt.in.replace && ui.focus == FOCUS_PROMPT);
}

static void prompt_typed_utf8(void)
{
    assert(!strcmp(prompt.in.text, "Chap第二") && !strcmp(clip[0], "Ch"));
}

static void prompt_renamed(void)
{
    assert(prompt.kind == P_NONE && !strcmp(doc.bookmark[0], "Chap第二"));
    printf("PASS prompts: Rename Bookmark types as the find bar does: its text selected, UTF-8 pasted, code-point backspace, Shift selection, Ctrl+C\n");
}

static void prompt_clicked(void)
{
    assert(prompt.kind == P_BOOKMARK);
    draw();
    int col, row;
    assert(hit_cell(HIT_PROMPT_INPUT, &col, &row));
    struct pointer_event e = { .x = (col + 2) * CW + 1, .y = TOP + row * CH + 1, .col = col + 2, .row = row,
                               .button = BTN_LEFT, .pressed = 1, .clicks = 1, .buttons = 1 };
    pointer(&e);
    e.pressed = 0;
    e.buttons = 0;
    pointer(&e);
    assert(prompt.in.cursor == 2 && !prompt.in.replace);
}

static void prompt_click_typed(void)
{
    assert(!strcmp(prompt.in.text, "ChXap第二"));
    printf("PASS prompts: a click in the answer puts the insertion point there\n");
}

static void password_masked(void)
{
    draw();
    assert(prompt.kind == P_PASSWORD && !strcmp(prompt.in.text, "sec") && on_screen("***") && !on_screen("sec_"));
    assert(!typing_line() && !strcmp(clip[0], "unchanged"));
}

static void password_done(void)
{
    assert(doc.open && !prompt.in.text[0]);
    printf("PASS prompts: the password is masked, never copied, without the input method, and wiped\n");
}

/* Bookmarks of a reflowed document: by MuPDF's bookmark, not the page. */
static char bm_needle[256];
static int bm_page;
static float bm_em;

static int page_has(int page, const char *needle)
{
    int s;
    pid_t pid = view_spawn(&s);
    assert(pid > 0);
    struct view_request q;
    struct view_reply r;
    int fd = -1;
    memset(&q, 0, sizeof q);
    q.op = VIEW_OPEN;
    snprintf(q.text, sizeof q.text, "%s", doc.path);
    q.ax = doc.lw;
    q.ay = doc.lh;
    q.bx = doc.em;
    assert(view_call(s, &q, &r, &fd) == 0 && r.status == 0 && r.pages == doc.pages);
    memset(&q, 0, sizeof q);
    q.op = VIEW_SEARCH;
    q.page = page;
    snprintf(q.text, sizeof q.text, "%s", needle);
    assert(view_call(s, &q, &r, &fd) == 0 && r.status == 0);
    if (fd >= 0) close(fd);
    close(s);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return r.count > 0;
}

static int bookmarked_page(void)
{
    int p = -1;
    for (int i = 0; i < doc.pages; i++) if (doc.bookmark[i]) { assert(p < 0); p = i; }
    return p;
}

static void reflow_bookmarked(void)
{
    assert(doc.flags & VIEW_REFLOWABLE);
    assert(current_page() == 3 && doc.bookmark[3] && doc.bmark[3] && bookmarked_page() == 3);
    text_at(3, 0, doc.lw, doc.lh, doc.em, bm_needle, sizeof bm_needle);
    char *space = strchr(bm_needle, ' ');
    if (space && (space = strchr(space + 1, ' '))) *space = '\0'; /* its first two words */
    assert(bm_needle[0]);
    bm_page = doc.pages;
    bm_em = doc.em;
}

static void reflow_bookmark_moved(void)
{
    int p = bookmarked_page();
    if (p <= 3) printf("reflow bookmark: em %.0f -> %.0f, pages %d -> %d, page 4 -> %d\n", bm_em, doc.em, bm_page, doc.pages, p + 1);
    /* Where the page number alone would put it, the text is not. */
    assert(doc.em == bm_em + 6 && doc.pages > bm_page && p > 3 && page_has(p, bm_needle) && !page_has(3, bm_needle));
    state_save();
    char *stored = state_read();
    assert(stored && strstr(stored, " mark="));
    free(stored);
    printf("PASS bookmarks of a reflowed document (EPUB): at MuPDF's bookmark, the text size %.0f -> %.0f moves it from page 4 to %d, with \"%s\"\n",
           bm_em, doc.em, p + 1, bm_needle);
}

static void reflow_bookmark_back(void)
{
    assert(doc.em == bm_em && bookmarked_page() == 3);
}

static void reflow_bookmark_reopened(void)
{
    assert(bookmarked_page() == 3 && doc.bmark[3]);
    printf("PASS bookmarks of a reflowed document: back on page 4 at the old size, and so when reopened (the state file's mark)\n");
    bookmark_remove(3); /* not left for the other runs */
}

/* The bookmarks panel's keys and menu. */
static void panel_start(void)
{
    for (int i = 0; i < 2; i++) if (!doc.bookmark[i]) { go(pos_page(i)); do_action(A_BOOKMARK); }
    sidebar(1);
    ui.tab = TAB_BOOKMARKS;
    draw();
    press_id(HIT_TAB + TAB_BOOKMARKS);
    assert(ui.focus == FOCUS_SIDEBAR && doc.bookmark[0] && doc.bookmark[1]);
    go(pos_page(0));
}

static void panel_down(void) { assert(ui.bm_sel == 1 && current_page() == 1); }
static void panel_up(void) { assert(ui.bm_sel == 0 && current_page() == 0); }

static void panel_deleted(void)
{
    assert(!doc.bookmark[0] && doc.bookmark[1] && ui.bm_sel == 0);
    draw();
    int col, row;
    assert(hit_cell(HIT_BOOKMARK + 1, &col, &row));
    struct pointer_event e = { .x = col * CW + 1, .y = TOP + row * CH + 1, .col = col, .row = row,
                               .button = BTN_RIGHT, .pressed = 1, .clicks = 1, .buttons = 2 };
    last_event = e;
    pointer(&e); /* its menu takes the next keys */
}

static void panel_removed(void)
{
    assert(!doc.bookmark[0] && !doc.bookmark[1]);
    draw();
    assert(on_screen("No bookmarks"));
    printf("PASS bookmarks panel: Down, Up and Enter go to a bookmark, Delete removes it; its right button's Remove Bookmark\n");
}

static void pick_next(void)
{
    static int k;
    snprintf(pick_out, sizeof pick_out, "%s", recent_file[k++ % 11]);
    pick_ok = 1;
}

static void pick_markdown(void)
{
    snprintf(pick_out, sizeof pick_out, "%s", sample("sample.md"));
    pick_ok = 1;
}

static void drop_bookmarks(void)
{
    for (int i = 0; i < doc.pages; i++) bookmark_remove(i);
}

static void clip_chinese(void)
{
    assert(!strcmp(prompt.in.text, "Chap"));
    snprintf(clip[0], sizeof clip[0], "第二章");
}

static void clip_unchanged(void)
{
    snprintf(clip[0], sizeof clip[0], "unchanged");
}

static void pick_out_dir(void)
{
    snprintf(pick_out, sizeof pick_out, "%s", out_dir);
    pick_ok = 1;
}


/* Gwenview's Browse mode (make-view-samples.c gallery: a.jpg B.jpg c.png
 * d.dng e.png, the folder sub and the archive pack.cbz). */
static int red_px(uint32_t c)
{
    return (c >> 16 & 0xFF) > 0x90 && (c >> 8 & 0xFF) < 0x50 && (c & 0xFF) < 0x50;
}

static int blue_px(uint32_t c)
{
    return (c >> 16 & 0xFF) < 0x50 && (c >> 8 & 0xFF) < 0x50 && (c & 0xFF) > 0x90;
}

/* The screen's pixel at (fx, fy) of item k's thumbnail. */
static uint32_t gal_pixel(int k, double fx, double fy)
{
    int X, Y, cell_w, cell_h, bw, bh, w, h;
    gal_at(k, &X, &Y);
    gal_cell(&cell_w, &cell_h, &bw, &bh);
    gal_zoom(k - gal.nitem, &w, &h);
    int tx = X + (cell_w - bw) / 2 + (bw - w) / 2, ty = Y + col_margin + bh - h;
    return pic[ty + (int)(fy * h)][tx + (int)(fx * w)];
}

static const char *gal_selected(void)
{
    return gal.sel < gal.nitem ? gal.item[gal.sel].name : gal.name[gal.sel - gal.nitem];
}

static void browse_opened(void)
{
    assert(gal.on && gal.folder && doc.open && doc.pages == 5 && gal.nitem == 2 && side_px == 0);
    assert(!strcmp(gal.item[0].name, "pack.cbz") && gal.item[0].archive && !strcmp(gal.item[1].name, "sub") && !gal.item[1].archive);
    assert(!strcmp(gal.name[0], "a.jpg") && !strcmp(gal.name[1], "B.jpg") && !strcmp(gal.name[3], "d.dng"));
    assert(gal.sel == 2 && settings.thumbnail_size == 128);
    assert(on_screen("a.jpg") && on_screen("pack.cbz") && on_screen("sub") && on_screen("5 images") && on_screen("128 x 128"));
    assert(on_screen(" Browse ") && on_screen(" View "));
    assert(red_px(gal_pixel(2, 0.8, 0.8)) && blue_px(gal_pixel(5, 0.75, 0.5)) && red_px(gal_pixel(5, 0.25, 0.5)));
    printf("PASS Browse mode (a folder): the grid of its images by name, the folder and the archive first, the first "
           "image selected, names, \"5 images\", \"128 x 128\", the JPEG's and the DNG's thumbnails\n");
}

static void browse_right(void)
{
    assert(gal.on && gal.sel == 3);
    printf("PASS Browse: Right selects the next image\n");
}

static void viewed_b(void)
{
    assert(!gal.on && current_page() == 1 && side_px > 0 && v.mode == VL_FIT_PAGE);
    printf("PASS Enter: View mode at B.jpg, the page view (fit, Gwenview's Autofit), the sidebar back\n");
}

static void browsed_back(void)
{
    assert(gal.on && gal.sel == 3 && side_px == 0);
    printf("PASS Esc: back to Browse, B.jpg selected\n");
}

static void double_clicked(void)
{
    assert(!gal.on && current_page() == 2);
    printf("PASS a double click on c.png: View mode at it\n");
}

static void size_wheel(void)
{
    assert(gal.on && settings.thumbnail_size == 132 && on_screen("132 x 132"));
    printf("PASS Ctrl+wheel: thumbnails 4 pixels larger (132)\n");
}

static void size_larger(void)
{
    assert(settings.thumbnail_size == 142);
    printf("PASS the slider's +: 10 larger (142)\n");
}

static void size_smaller(void)
{
    assert(settings.thumbnail_size == 132);
    printf("PASS the slider's -: 10 smaller (132)\n");
}

static void sorted_by_date(void)
{
    assert(gal.on && settings.sort == VIEW_SORT_DATE && doc.pages == 5);
    assert(!strcmp(gal.name[0], "B.jpg") && !strcmp(gal.name[1], "c.png") && !strcmp(gal.name[2], "a.jpg") &&
           !strcmp(gal.name[3], "d.dng") && !strcmp(gal.name[4], "e.png"));
    assert(!strcmp(gal_selected(), "c.png"));
    printf("PASS View > Sort By > Date: B.jpg (EXIF 2019), c.png (its file, 2020), a.jpg, d.dng, e.png; c.png still selected\n");
}

static void raw_viewed(void)
{
    int X, Y;
    assert(!gal.on && current_page() == 3);
    spot(3, doc.pw[3] / 4, doc.ph[3] / 2, &X, &Y);
    assert(red_px(pic[Y][X]));
    spot(3, doc.pw[3] * 3 / 4, doc.ph[3] / 2, &X, &Y);
    assert(blue_px(pic[Y][X]));
    printf("PASS the DNG in View mode: LibRaw's picture, red and blue\n");
}

static void info_shown(void)
{
    assert(settings.sidebar && ui.tab == TAB_INFO && gal.info && gal.info_page == 3);
    assert(strstr(gal.info, "Name\td.dng\n") && strstr(gal.info, "Exif.Image.Make\tView\n") && on_screen("Image Size"));
    assert(!on_screen("Manufactur"));
    printf("PASS F4: the Information panel, Gwenview's preferred keys (Name, Image Size)\n");
}

static void info_more(void)
{
    assert(gal.info_more && on_screen("Manufactur"));
    printf("PASS Show more details: every key (Manufacturer ...)\n");
}

static void up_to_browse(void)
{
    assert(gal.on && !strcmp(gal_selected(), "d.dng"));
    printf("PASS Alt+Up in View mode: Browse, d.dng selected\n");
}

static void up_to_parent(void)
{
    char real[PATH_MAX];
    assert(realpath(samples, real) && gal.on && gal.folder && !strcmp(gal.dir, real));
    assert(gal.sel < gal.nitem && !strcmp(gal_selected(), "gallery"));
    printf("PASS Alt+Up in Browse: the parent folder, gallery selected\n");
}

static void back_in(void)
{
    assert(gal.on && gal.folder && strstr(gal.dir, "/gallery") && doc.pages == 5 && gal.sel == 2);
    printf("PASS Enter on a folder: it is browsed, its first image selected\n");
}

static void archive_browsed(void)
{
    assert(gal.on && !gal.folder && !strcmp(doc.format, "Comic book") && doc.pages == 2 && !strcmp(gal.name[1], "p2.dng"));
    printf("PASS Enter on an archive: its pages browsed (a PNG and a DNG)\n");
}

static void archive_up(void)
{
    assert(gal.on && gal.folder && !strcmp(gal_selected(), "pack.cbz"));
    printf("PASS Alt+Up from the archive: its folder, the archive selected\n");
}

static void sort_name_back(void)
{
    assert(settings.sort == VIEW_SORT_NAME && !strcmp(gal.name[0], "a.jpg"));
    printf("PASS Sort By > Name again\n");
}

static void years_browsed(void)
{
    assert(gal.on && gal.folder && gal.empty && !doc.open && gal.nitem == 1 && !strcmp(gal.item[0].name, "2024"));
    assert(on_screen("0 images") && on_screen("2024"));
    printf("PASS a folder of folders only: Browse lists them, \"0 images\"\n");
}

static void year_in(void)
{
    assert(gal.on && gal.folder && doc.open && doc.pages == 1 && !strcmp(gal.name[0], "x.png") && gal.sel == 0);
    printf("PASS Enter on its folder: browsed, its image selected\n");
}

static void year_up(void)
{
    assert(gal.on && gal.empty && gal.sel == 0 && !strcmp(gal_selected(), "2024"));
    printf("PASS Alt+Up: back to the folder of folders, 2024 selected\n");
}

static void image_escaped(void)
{
    char real[PATH_MAX];
    assert(realpath(samples, real) && gal.on && gal.folder && !strcmp(gal.dir, real) && doc.open);
    assert(gal.sel >= gal.nitem && !strcmp(gal_selected(), "sample.png"));
    printf("PASS an image by itself: Esc browses its folder, it selected\n");
}

int main(int argc, char **argv)
{
    assert((argc == 3 || argc == 4) && geteuid() == 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    samples = argv[1];
    pdf_dir = argc == 4 ? argv[3] : NULL;
    struct passwd *pw = getpwuid(VIEW_USER);
    assert(pw && pw->pw_dir);
    user_gid = pw->pw_gid;
    snprintf(home, sizeof home, "%.3900s", pw->pw_dir);
    snprintf(out_dir, sizeof out_dir, "%s/view-test-out", home);
    snprintf(state_file, sizeof state_file, "%s/%s", home, STATE_FILE);
    unlink(state_file);
    char cmd[PATH_MAX * 2 + 128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", out_dir);
    assert(system(cmd) == 0);
    assert(mkdir(out_dir, 0755) == 0 && chown(out_dir, VIEW_USER, pw->pw_gid) == 0);
    (void)argv[2];
    snprintf(pick_out, sizeof pick_out, "%s", out_dir);
    pick_ok = 1;

    idle();
    check(pointer_focus_splitter);
    run("sample.pdf: pointer focus and splitter regression", sample("sample.pdf"));
    FILE *legacy_state = fopen(state_file, "w");
    assert(legacy_state);
    fputs("sidebar=1 side_width=195\t\n", legacy_state);
    fclose(legacy_state);
    assert(chown(state_file, VIEW_USER, pw->pw_gid) == 0);

    /* Existing workflows also run with a saved, wider sidebar. */
    /* sample.pdf: open, zoom, scroll, go to page, rotate, invert. */
    idle();
    check(opened_pdf);
    key('=', KEYS_CTRL);
    idle();
    check(zoomed_in);
    key('-', KEYS_CTRL);
    key('-', KEYS_CTRL);
    idle();
    check(zoomed_out);
    key(K_CTRL_0, KEYS_CTRL);
    idle();
    check(before_scroll);
    key(K_DOWN, 0);
    idle();
    check(scrolled);
    pointer_at(600, 400, 0, 0, 0, 1, 0);
    idle();
    check(wheeled);
    key(7, KEYS_CTRL);
    type("2\n");
    idle();
    check(at_page_two);
    key(K_BOL, KEYS_CTRL);
    key('v', KEYS_ALT);
    for (int i = 0; i < 3; i++) key(K_DOWN, 0); /* Fit Width */
    key('\n', 0);
    key('v', KEYS_ALT);
    for (int i = 0; i < 7; i++) key(K_DOWN, 0); /* Rotate Right */
    key('\n', 0);
    idle();
    check(rotated);
    key('v', KEYS_ALT);
    for (int i = 0; i < 10; i++) key(K_DOWN, 0); /* Invert Colors */
    key('\n', 0);
    idle();
    check(inverted);
    key('v', KEYS_ALT);
    for (int i = 0; i < 9; i++) key(K_DOWN, 0); /* Original Orientation */
    key('\n', 0);
    key('v', KEYS_ALT);
    for (int i = 0; i < 10; i++) key(K_DOWN, 0); /* Invert Colors off */
    key('\n', 0);
    key(K_CTRL_0, KEYS_CTRL);
    key(7, KEYS_CTRL);
    type("2\n");
    idle();
    check(place_and_zoom);
    run("sample.pdf: open, zoom, scroll, go to page, rotate, invert", sample("sample.pdf"));
    state_written();

    /* Again: where it was; facing pages, search, properties, embedded
     * files, export, and the renderer killed. */
    idle();
    check(restored);
    key('v', KEYS_ALT);
    for (int i = 0; i < 4; i++) key(K_DOWN, 0); /* Fit Page */
    key('\n', 0);
    key('v', KEYS_ALT);
    for (int i = 0; i < 6; i++) key(K_DOWN, 0); /* Facing Pages */
    key('\n', 0);
    idle();
    check(facing);
    key(6, KEYS_CTRL);
    type("quokka\n");
    idle();
    check(found_first);
    key(K_F1 + 2, 0);
    idle();
    check(found_next);
    key(27, 0);
    key('\n', KEYS_ALT);
    idle();
    check(properties);
    key(27, 0);
    key('f', KEYS_ALT);
    for (int i = 0; i < 3; i++) key(K_DOWN, 0); /* Embedded Files... */
    key('\n', 0);
    idle();
    check(listed_embedded);
    key('\n', 0);                               /* save it: the folder, then the name */
    key('\n', 0);
    idle();
    check(saved_embedded);
    key(27, 0);
    key('f', KEYS_ALT);
    for (int i = 0; i < 4; i++) key(K_DOWN, 0); /* Export As Plain Text... */
    key('\n', 0);
    key('\n', 0);
    idle();
    check(exported);
    check(before_kill);
    add((struct step){ .kind = S_KILL });
    until(restarted);
    idle();
    key('=', KEYS_CTRL);
    idle();
    check(survived);
    run("sample.pdf: reopened; facing, search, properties, embedded file, export, renderer killed",
        sample("sample.pdf"));

    /* Single page at 100%: select text, links, back, the contents. */
    idle();
    key('v', KEYS_ALT);
    for (int i = 0; i < 5; i++) key(K_DOWN, 0); /* Single Page */
    key('\n', 0);
    key(K_CTRL_0, KEYS_CTRL);
    key(K_BOL, KEYS_CTRL);
    key(K_CTRL_0 + 4, KEYS_CTRL);               /* Text Selection */
    idle();
    page_pointer(0, 70, 65, 0, -1, 0);          /* over the page: the text cursor */
    page_pointer(0, 70, 65, BTN_LEFT, 1, 1);
    page_pointer(0, 100, 65, BTN_LEFT, -1, 0);
    page_pointer(0, 127, 65, BTN_LEFT, -1, 0);
    page_pointer(0, 127, 65, BTN_LEFT, 0, 0);
    idle();
    check(selected_hello);
    key(3, KEYS_CTRL);
    check(copied_hello);
    click_point(0, 219, 135, BTN_LEFT, 1);
    click_point(0, 219, 135, BTN_LEFT, 2);
    idle();
    check(word_only);
    click_point(0, 219, 135, BTN_LEFT, 3);
    idle();
    check(word_and_line);
    key(K_CTRL_0 + 1, KEYS_CTRL);               /* Browse */
    idle();
    check(links_here);
    click_point(0, 100, 287, BTN_LEFT, 1);
    idle();
    check(followed);
    key(K_LEFT, KEYS_ALT | KEYS_SHIFT);
    idle();
    check(came_back);
    click_point(0, 100, 337, BTN_LEFT, 1);
    idle();
    check(went_out);
    check(toc_click);
    idle();
    check(toc_went);
    run("sample.pdf: text selection, links, back, contents", sample("sample.pdf"));

    /* The sidebar, the pointer's zooms, tiles, the right button. */
    idle();
    key(K_BOL, KEYS_CTRL);
    idle();
    page_pointer(0, 300, 400, BTN_RIGHT, 1, 1);
    page_pointer(0, 300, 400, BTN_RIGHT, 0, 0);
    key('\n', 0);                               /* Fit Width */
    idle();
    check(context_fit_width);
    key(K_F1 + 6, 0);
    idle();
    check(sidebar_hidden);
    key(K_F1 + 6, 0);
    idle();
    check(thumbnails_tab);
    idle();
    check(thumbnails_drawn);
    idle();
    check(thumbnail_went);
    key(K_BOL, KEYS_CTRL);
    idle();
    check(ctrl_wheel);
    idle();
    check(red_matches);
    check(pinch);
    idle();
    check(red_matches);
    check(middle_drag);
    idle();
    check(red_matches);
    check(zoom_box);
    for (int i = 0; i < 12; i++) key(K_DOWN, 0); /* 400% */
    key('\n', 0);
    idle();
    check(square_in_middle);
    idle();
    check(tiles);
    run("sample.pdf: sidebar, thumbnails, pointer zooms, tiles, the right button", sample("sample.pdf"));

    /* locked.pdf: the password. */
    idle();
    check(asked_password);
    type("wrong\n");
    idle();
    check(asked_again);
    type("secret\n");
    idle();
    check(unlocked);
    run("locked.pdf: the password", sample("locked.pdf"));

    /* sample.epub: reflowed, the text larger, the same text at the top. */
    idle();
    key(7, KEYS_CTRL);
    type("2\n");
    key(K_DOWN, 0);
    key(K_DOWN, 0);
    idle();
    check(reflow_before);
    key('>', KEYS_SHIFT);
    idle();
    check(reflow_after);
    run("sample.epub: larger text", sample("sample.epub"));

    idle(); check(parity_bbox_huge_start); until(bbox_failed_shown); idle(); check(parity_bbox_huge);
    idle(); check(parity_trim_start); until(bbox_in_flight); idle(); check(parity_margins); idle();
    page_pointer(0, 50, 60, BTN_LEFT, 1, 1);
    page_pointer(0, 560, 600, BTN_LEFT, -1, 1);
    page_pointer(0, 560, 600, BTN_LEFT, 0, 1);
    idle(); check(parity_selection); idle(); check(parity_selection_found);
    check(parity_find_utf8);
    check(parity_crop_interaction_start); idle();
    page_pointer(0, 70, 65, BTN_LEFT, 1, 1);
    page_pointer(0, 127, 65, BTN_LEFT, -1, 1);
    page_pointer(0, 127, 65, BTN_LEFT, 0, 0);
    idle(); check(parity_crop_text);
    click_point(0, 100, 287, BTN_LEFT, 1); idle(); check(parity_crop_link);
    run("sample.pdf: exact trim and UTF-8 search parity", sample("sample.pdf"));
    idle(); check(parity_reflow_resize); idle(); check(parity_reflow_resized); idle(); check(parity_reflow_hidden); idle(); check(parity_reflow_screen); idle();
    run("sample.epub: viewport reflow", sample("sample.epub"));
    idle(); check(parity_reflow_resize); idle(); check(parity_reflow_resized); idle(); check(parity_reflow_hidden); idle(); check(parity_reflow_screen); idle();
    run("sample.md: viewport reflow", sample("sample.md"));

    idle(); check(parity_bookmarks); idle(); check(parity_reloaded);
    run("sample.pdf: bookmarks and reload", sample("sample.pdf"));
    idle(); check(parity_press_settings); key('\n', 0); idle(); check(parity_sidebar_menu_hidden);
    run("sample.pdf: actual menu sidebar hide and clickable restore", sample("sample.pdf"));

    idle(); check(parity_tabs_open); idle(); check(parity_tabs_back); idle(); check(parity_tabs_close);
    idle(); check(parity_tabs_reopen); idle(); check(parity_tabs_restored);
    run("sample.pdf: document tabs and undo close", sample("sample.pdf"));

    char blank_path[PATH_MAX];
    snprintf(blank_path, sizeof blank_path, "%s/blank.pdf", out_dir);
    FILE *blank = fopen(blank_path, "w"); assert(blank);
    long offset[5];
    fputs("%PDF-1.4\n", blank);
    offset[1] = ftell(blank); fputs("1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n", blank);
    offset[2] = ftell(blank); fputs("2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n", blank);
    offset[3] = ftell(blank); fputs("3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << >> /Contents 4 0 R >> endobj\n", blank);
    offset[4] = ftell(blank); fputs("4 0 obj << /Length 0 >> stream\n\nendstream endobj\n", blank);
    long xref = ftell(blank);
    fputs("xref\n0 5\n0000000000 65535 f \n", blank);
    for (int i = 1; i <= 4; i++) fprintf(blank, "%010ld 00000 n \n", offset[i]);
    fprintf(blank, "trailer << /Size 5 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref);
    fclose(blank); assert(chown(blank_path, VIEW_USER, pw->pw_gid) == 0);
    idle(); check(parity_blank_page); idle(); check(parity_blank_bounds);
    run("blank PDF margins", blank_path);

    /* heavy.pdf: a key while a page is drawn; a zoom cancels the drawing. */
    add((struct step){ .kind = S_RENDERING });
    add((struct step){ .kind = S_TIMED_KEY, .key = K_DOWN });
    check(responsive);
    idle();
    check(responsive_measured);
    key('=', KEYS_CTRL);
    add((struct step){ .kind = S_RENDERING });
    check(rendering_heavy);
    key('=', KEYS_CTRL);
    check(cancel_sent);
    idle();
    check(cancelled);
    run("heavy.pdf: responsive, cancelled", sample("heavy.pdf"));

    if (pdf_dir) {
        /* Forms: Show Forms, Tab through the fields, keys and the pointer;
         * MaxLen, the document's validation; undo, redo; Save As. */
        idle(); check(forms_opened);
        key('\t', 0); idle(); check(forms_text_focused);
        type("after\n"); idle(); check(forms_typed);
        key('\n', 0); type("0123456789ABCDEFGH"); check(forms_maxlen_typing); key('\n', 0); idle(); check(forms_maxlen);
        key('\n', 0); erase(16); type("bad\n"); idle(); check(forms_rejected);
        key('\n', 0); erase(16); type("keybad\n"); idle(); check(forms_key_rejected);
        key('\t', 0); key(' ', 0); idle(); check(forms_checked);
        click_point(0, 140, 141, BTN_LEFT, 1); idle(); check(forms_unchecked);
        key('\t', 0); key(' ', 0); idle(); check(forms_radio_one);
        key('\t', 0); key(' ', 0); idle(); check(forms_radio_two);
        click_point(0, 140, 171, BTN_LEFT, 1); idle(); check(forms_radio_clicked);
        key('\t', 0); key('\t', 0); key(K_DOWN, 0); idle(); check(forms_combo_down);
        key(' ', 0); downs(2); key('\n', 0); idle(); check(forms_combo_rejected);
        key('\t', 0); key(' ', 0); downs(2); key('\n', 0); idle(); check(forms_list);
        key('\t', 0); key(' ', 0); downs(1); key('\n', 0); idle(); check(forms_list_rejected);
        click_point(0, 140, 111, BTN_LEFT, 1); idle(); check(forms_readonly);
        key(26, KEYS_CTRL); idle(); check(forms_undone);
        key(26, KEYS_CTRL | KEYS_SHIFT); idle(); check(forms_redone);
        key(19, KEYS_CTRL | KEYS_SHIFT); erase((int)strlen("forms-ui.pdf")); type("forms-saved.pdf\n"); idle(); check(forms_saved_as);
        run("forms.pdf: Show Forms, Tab, text, MaxLen, validation, check box, radio, combo, list, undo, Save As",
            user_copy(pdf_dir, "forms.pdf", "forms-ui.pdf"));
        idle(); check(forms_reopened);
        run("forms-saved.pdf: reopened", out_file("forms-saved.pdf"));

        /* Unsaved changes: the View menu's Show Forms; asked on Ctrl+Q and
         * F5; a save that fails keeps them; Save from the question. */
        idle(); key('v', KEYS_ALT); downs(17); key('\n', 0);
        key('\t', 0); type("first\n"); idle(); check(guard_typed);
        key(17, KEYS_CTRL); check(guard_asked); key(27, 0); check(guard_cancelled);
        key(19, KEYS_CTRL | KEYS_SHIFT); key('\n', 0); idle(); check(guard_save_failed);
        key(K_F1 + 4, 0); check(guard_asked); key('d', 0); idle(); check(guard_discarded);
        key('v', KEYS_ALT); downs(17); key('\n', 0);
        key('\t', 0); type("second\n"); idle();
        key(17, KEYS_CTRL); check(guard_asked); key('s', 0);
        run("forms-guard.pdf: unsaved changes asked about, a failed save, discard, save", user_copy(pdf_dir, "forms.pdf", "forms-guard.pdf"));
        idle(); check(guard_reopened);
        run("forms-guard.pdf: reopened", out_file("forms-guard.pdf"));

        /* Annotations: F6, each tool by the pointer, then Browse: move,
         * resize, delete, undo, redo, properties, a double click; the
         * panel; Ctrl+S; reopened. */
        idle(); key('v', KEYS_ALT); downs(4); key('\n', 0); /* Fit Page */
        key(K_F1 + 5, 0); idle(); check(annots_start);
        key('1', KEYS_ALT);
        page_pointer(0, 70, 65, BTN_LEFT, 1, 1); page_pointer(0, 100, 65, BTN_LEFT, -1, 0); page_pointer(0, 127, 65, BTN_LEFT, 0, 0);
        key('2', KEYS_ALT);
        page_pointer(0, 72, 135, BTN_LEFT, 1, 1); page_pointer(0, 240, 135, BTN_LEFT, -1, 0); page_pointer(0, 240, 135, BTN_LEFT, 0, 0);
        key('3', KEYS_ALT);
        page_pointer(0, 130, 65, BTN_LEFT, 1, 1); page_pointer(0, 200, 65, BTN_LEFT, -1, 0); page_pointer(0, 200, 65, BTN_LEFT, 0, 0);
        key('4', KEYS_ALT);
        page_pointer(0, 72, 135, BTN_LEFT, 1, 1); page_pointer(0, 130, 135, BTN_LEFT, -1, 0); page_pointer(0, 130, 135, BTN_LEFT, 0, 0);
        idle();
        key('5', KEYS_ALT);
        page_pointer(0, 100, 400, BTN_LEFT, 1, 1); page_pointer(0, 250, 430, BTN_LEFT, -1, 0); page_pointer(0, 250, 430, BTN_LEFT, 0, 0);
        type("Typed\n");
        key('6', KEYS_ALT);
        page_pointer(0, 300, 400, BTN_LEFT, 1, 1); page_pointer(0, 450, 440, BTN_LEFT, -1, 0); page_pointer(0, 450, 440, BTN_LEFT, 0, 0);
        type("Inline\n");
        key('7', KEYS_ALT);
        click_point(0, 500, 300, BTN_LEFT, 1);
        type("Popup\n");
        idle();
        key('8', KEYS_ALT);
        page_pointer(0, 100, 500, BTN_LEFT, 1, 1); page_pointer(0, 120, 520, BTN_LEFT, -1, 0);
        page_pointer(0, 140, 510, BTN_LEFT, -1, 0); page_pointer(0, 160, 530, BTN_LEFT, -1, 0); page_pointer(0, 160, 530, BTN_LEFT, 0, 0);
        key('9', 0); /* the toolbar's ninth: Straight Line */
        page_pointer(0, 100, 600, BTN_LEFT, 1, 1); page_pointer(0, 200, 650, BTN_LEFT, -1, 0); page_pointer(0, 200, 650, BTN_LEFT, 0, 0);
        key('0', KEYS_ALT);
        page_pointer(0, 250, 500, BTN_LEFT, 1, 1); page_pointer(0, 350, 560, BTN_LEFT, -1, 0); page_pointer(0, 350, 560, BTN_LEFT, 0, 0);
        idle();
        check(pick_ellipse);
        page_pointer(0, 400, 500, BTN_LEFT, 1, 1); page_pointer(0, 500, 560, BTN_LEFT, -1, 0); page_pointer(0, 500, 560, BTN_LEFT, 0, 0);
        idle();
        check(pick_polygon);
        click_point(0, 100, 700, BTN_LEFT, 1); click_point(0, 150, 680, BTN_LEFT, 1);
        click_point(0, 200, 720, BTN_LEFT, 1); click_point(0, 100, 700, BTN_LEFT, 1);
        idle();
        check(pick_stamp);
        click_point(0, 450, 650, BTN_LEFT, 1);
        idle(); check(annots_made);
        key(27, 0); /* annotating stopped: Browse */
        page_pointer(0, 300, 530, BTN_LEFT, 1, 1); page_pointer(0, 310, 545, BTN_LEFT, -1, 0);
        page_pointer(0, 320, 560, BTN_LEFT, -1, 0); page_pointer(0, 320, 560, BTN_LEFT, 0, 0);
        idle(); check(annots_moved);
        click_point(0, 450, 530, BTN_LEFT, 1);
        page_pointer(0, 500, 560, BTN_LEFT, 1, 1); page_pointer(0, 510, 580, BTN_LEFT, -1, 0);
        page_pointer(0, 520, 600, BTN_LEFT, -1, 0); page_pointer(0, 520, 600, BTN_LEFT, 0, 0);
        idle(); check(annots_resized);
        click_point(0, 450, 650, BTN_LEFT, 1); key(K_DEL, 0); idle(); check(annots_deleted);
        key(26, KEYS_CTRL); idle(); check(annots_undone);
        key(26, KEYS_CTRL | KEYS_SHIFT); idle(); check(annots_redone);
        page_pointer(0, 320, 560, BTN_RIGHT, 1, 1); page_pointer(0, 320, 560, BTN_RIGHT, 0, 0);
        downs(2); key('\n', 0); /* Properties */
        downs(2); key('\n', 0); /* Color */
        downs(5); key('\n', 0); /* Blue */
        idle(); check(annots_recolored);
        page_pointer(0, 320, 560, BTN_RIGHT, 1, 1); page_pointer(0, 320, 560, BTN_RIGHT, 0, 0);
        downs(2); key('\n', 0); downs(3); key('\n', 0); downs(4); key('\n', 0); /* Properties, Opacity, 50% */
        idle();
        page_pointer(0, 320, 560, BTN_RIGHT, 1, 1); page_pointer(0, 320, 560, BTN_RIGHT, 0, 0);
        downs(2); key('\n', 0); downs(4); key('\n', 0); downs(6); key('\n', 0); /* Properties, Width, 4 */
        idle();
        page_pointer(0, 320, 560, BTN_RIGHT, 1, 1); page_pointer(0, 320, 560, BTN_RIGHT, 0, 0);
        downs(2); key('\n', 0); downs(1); key('\n', 0); /* Properties, Author */
        erase(64); type("Reviewer\n"); idle(); check(annots_properties);
        page_pointer(0, 500, 300, BTN_RIGHT, 1, 1); page_pointer(0, 500, 300, BTN_RIGHT, 0, 0);
        downs(3); key('\n', 0); downs(4); key('\n', 0); downs(3); key('\n', 0); /* Properties, Icon, Help */
        idle(); check(annots_icon);
        click_point(0, 500, 300, BTN_LEFT, 1); click_point(0, 500, 300, BTN_LEFT, 2);
        type(" edited\n"); idle(); check(annots_edited);
        check(annots_panel); idle(); check(annots_panel_listed);
        key(19, KEYS_CTRL); idle(); check(annots_saved);
        run("annots.pdf: every tool, move, resize, delete, undo, redo, properties, panel, save",
            user_copy(samples, "sample.pdf", "annots.pdf"));
        idle(); check(annots_reopened);
        run("annots.pdf: reopened", out_file("annots.pdf"));

        /* Encrypted: the password, a field filled, Save As, reopened. */
        idle(); type("user-secret\n"); idle();
        key('v', KEYS_ALT); downs(17); key('\n', 0);
        key('\t', 0); type("encrypted\n"); idle(); check(enc_filled);
        key(19, KEYS_CTRL | KEYS_SHIFT); erase((int)strlen("enc-ui.pdf")); type("enc-saved.pdf\n"); idle();
        run("encrypted.pdf: filled and saved as", user_copy(pdf_dir, "encrypted.pdf", "enc-ui.pdf"));
        idle(); check(asked_password); type("user-secret\n"); idle(); check(enc_reopened);
        run("enc-saved.pdf: reopened", out_file("enc-saved.pdf"));

        char signed_path[PATH_MAX];
        snprintf(signed_path, sizeof signed_path, "%s/signed.pdf", pdf_dir);
        if (access(signed_path, R_OK) == 0) {
            idle(); check(signed_opened);
            key('7', KEYS_ALT); click_point(0, 300, 300, BTN_LEFT, 1); type("after signing\n"); key(27, 0);
            idle(); check(signed_changed);
            key(19, KEYS_CTRL); idle(); check(signed_consent); key('n', 0); check(signed_refused);
            key(19, KEYS_CTRL); idle(); key('y', 0); idle(); check(signed_saved);
            run("signed.pdf: signatures panel, a change, consent to save", user_copy(pdf_dir, "signed.pdf", "signed-ui.pdf"));
        } else {
            printf("SKIP signed PDF: no signed fixture\n");
        }
    } else {
        printf("SKIP PDF interaction: no PDF fixtures given\n");
    }


    /* Okular's other mouse tools. */
    char table_path[PATH_MAX], pages_path[PATH_MAX];
    snprintf(table_path, sizeof table_path, "%s/table.pdf", out_dir);
    snprintf(pages_path, sizeof pages_path, "%s/pages.pdf", out_dir);
    write_pdf(table_path, 1, table_page);
    write_pdf(pages_path, 10, numbered_page);

    idle(); check(tools_menu);
    key(K_BOL, KEYS_CTRL); key('v', KEYS_ALT); downs(4); key('\n', 0); idle(); /* Fit Page */
    key(K_CTRL_0 + 2, KEYS_CTRL); check(zoom_tool_on);
    page_drag(0, 400, 92, 500, 192, BTN_LEFT); idle(); check(zoomed_rect);
    idle(); page_drag(0, 450, 142, 450.5, 142.5, BTN_LEFT); check(zoom_tiny);
    click_point(0, 450, 142, BTN_RIGHT, 1); idle(); check(zoom_tool_out);
    key(K_CTRL_0 + 1, KEYS_CTRL); key('v', KEYS_ALT); downs(3); key('\n', 0); /* Browse, Fit Width */
    key(K_BOL, KEYS_CTRL); idle();
    key(K_CTRL_0 + 3, KEYS_CTRL); check(area_tool_on);
    page_drag(0, 60, 40, 380, 150, BTN_LEFT); idle(); downs(1); key('\n', 0); check(area_copied);
    page_drag(0, 60, 40, 380, 150, BTN_LEFT); idle(); downs(2); key('\n', 0); check(area_flat);
    page_drag(0, 60, 120, 380, 150, BTN_LEFT); idle(); downs(3); key('\n', 0); idle(); check(area_searched);
    /* No text in it: the menu is the image's only. */
    page_drag(0, 390, 82, 510, 202, BTN_LEFT); idle(); downs(1); key('\n', 0); check(area_image_refused);
    check(pick_out_dir);
    page_drag(0, 390, 82, 510, 202, BTN_LEFT); idle(); downs(2); key('\n', 0); key('\n', 0); idle(); check(area_image_saved);
    key(K_CTRL_0 + 1, KEYS_CTRL); idle();
    page_drag(0, 60, 120, 380, 150, BTN_RIGHT); idle(); downs(1); key('\n', 0); check(browse_right_area);
    key(K_CTRL_0 + 6, KEYS_CTRL); idle();
    page_pointer(0, 450, 142, BTN_LEFT, 1, 1); idle(); check(magnified_in);
    page_pointer(0, 402, 142, BTN_LEFT, -1, 0); idle(); check(magnified_edge);
    page_pointer(0, 402, 142, BTN_LEFT, 0, 0); check(magnifier_gone);
    key(K_CTRL_0 + 1, KEYS_CTRL);
    run("sample.pdf: Zoom, Area Selection, Magnifier", sample("sample.pdf"));

    idle(); key('t', KEYS_ALT); downs(4); key('\n', 0); /* Tools > Table Selection */
    page_drag(0, 60, 75, 420, 140, BTN_LEFT); idle(); check(table_guessed);
    key(27, 0); check(table_escaped); key(27, 0); check(table_gone);
    page_drag(0, 60, 75, 420, 140, BTN_LEFT); idle();
    click_point(0, 90, 77, BTN_LEFT, 1); check(table_divided);
    click_point(0, 90, 77, BTN_LEFT, 1); check(table_undivided);
    page_pointer(0, 200, 100, BTN_RIGHT, 1, 1); key('\n', 0); page_pointer(0, 200, 100, BTN_RIGHT, 0, 0); check(table_copied);
    key(K_CTRL_0 + 1, KEYS_CTRL);
    run("table.pdf: Table Selection", table_path);

    /* Presentation. */
    idle(); key(K_BOL, KEYS_CTRL); key(16, KEYS_CTRL | KEYS_SHIFT); until(slide_drawn); check(pres_first);
    key(K_RIGHT, 0); until(slide_drawn); check(pres_second);
    key(K_LEFT, 0); check(pres_page0); key(' ', 0); check(pres_page1); key('\b', 0); check(pres_page0);
    key(K_PGDN, 0); check(pres_page1); key(K_PGUP, 0); check(pres_page0); key(K_DOWN, 0); check(pres_page1);
    key(K_UP, 0); check(pres_page0); key(K_EOL, 0); check(pres_page1); key(K_BOL, 0); check(pres_page0);
    key(K_EOL, 0); check(pres_keys);
    pointer_at(500, 400, 0, 0, 0, 1, 0); check(pres_page0);                 /* wheel up */
    pointer_at(500, 400, 0, 0, 0, -1, 0); check(pres_page1);                /* wheel down */
    pointer_at(500, 400, BTN_RIGHT, 1, 1, 0, 0); pointer_at(500, 400, BTN_RIGHT, 0, 0, 0, 0); check(pres_page0);
    pointer_at(500, 400, BTN_LEFT, 1, 1, 0, 0); pointer_at(500, 400, BTN_LEFT, 0, 0, 0, 0); check(pres_page1);
    pointer_at(500, 400, BTN_RIGHT, 1, 1, 0, 0); pointer_at(500, 400, BTN_RIGHT, 0, 0, 0, 0); until(slide_drawn); check(pres_pointer_done);
    key('b', 0); idle(); check(pres_black); key('b', 0); idle(); check(pres_unblack);
    pointer_at(500, 0, 0, 0, 0, 0, 0); idle(); check(pres_bar); until(slide_drawn); check(pres_bar_next);
    key(27, 0); key(27, 0); idle(); check(pres_left);
    key(K_BOL, KEYS_CTRL); key(16, KEYS_CTRL | KEYS_SHIFT); until(slide_drawn); idle(); check(pres_link_press); until(slide_drawn); check(pres_link_followed);
    key(27, 0);
    run("sample.pdf: presentation", sample("sample.pdf"));

    /* Colour modes, through View > Color Mode and its Configure Color Modes. */
    idle(); key(K_BOL, KEYS_CTRL); key('v', KEYS_ALT); downs(4); key('\n', 0); idle(); check(colors_normal);
    color_keys(1); idle(); check(colors_inverted);
    color_keys(1); idle(); check(colors_off_again);
    color_keys(3); idle(); check(colors_recolor);
    color_keys(4); idle(); check(colors_bw);
    color_keys(5); idle(); check(colors_lightness);
    color_keys(6); idle(); check(colors_luma);
    color_keys(7); idle(); check(colors_luma_sym);
    color_keys(8); idle(); check(colors_hue_pos);
    color_keys(9); idle(); check(colors_hue_neg);
    color_keys(10); key('\n', 0); type("#F0E0C0\n"); check(colors_paper_set);
    color_keys(2); until(paper_drawn); idle(); check(colors_paper);
    key('v', KEYS_ALT); downs(19); key('\n', 0); until(paper_drawn); idle(); check(colors_toggle_off);
    key('v', KEYS_ALT); downs(19); key('\n', 0); until(paper_drawn); idle(); check(colors_toggle_on);
    check(colors_saved); idle(); check(colors_normal);
    run("sample.pdf: colour modes", sample("sample.pdf"));

    /* Overview columns and the reading direction. */
    idle(); key('v', KEYS_ALT); downs(15); key('\n', 0);                    /* Overview */
    key('v', KEYS_ALT); downs(20); key('\n', 0); downs(3); key('\n', 0);   /* Overview Columns: 4 */
    key(K_BOL, KEYS_CTRL); idle(); check(overview_four);
    key('v', KEYS_ALT); downs(21); key('\n', 0); idle(); check(overview_rtl);  /* Use Right to Left Reading Direction */
    check(overview_saved);
    run("pages.pdf: overview columns, right to left", pages_path);

    /* File lifecycle. */
    mkdir("/run/locachy", 0755);
    for (int k = 0; k < 11; k++) {
        snprintf(recent_file[k], sizeof recent_file[k], "%s/recent-%02d.txt", out_dir, k);
        FILE *f = fopen(recent_file[k], "w");
        assert(f);
        fprintf(f, "recent file %d\n", k);
        fclose(f);
        assert(chown(recent_file[k], VIEW_USER, pw->pw_gid) == 0);
    }
    idle(); check(recent_started); check(recent_kept);
    key('f', KEYS_ALT); downs(7); key('\n', 0); check(folder_asked);           /* Open Containing Folder */
    for (int k = 0; k < 11; k++) { check(pick_next); key(15, KEYS_CTRL); idle(); }
    check(recent_ten);
    key('f', KEYS_ALT); downs(1); key('\n', 0); downs(9); key('\n', 0); idle(); check(recent_opened);
    key('f', KEYS_ALT); downs(1); key('\n', 0); downs(9); key('\n', 0); idle(); check(recent_failed);
    key('f', KEYS_ALT); downs(1); key('\n', 0); downs(9); key('\n', 0); check(recent_cleared);
    run("sample.pdf: Open Recent, Open Containing Folder", sample("sample.pdf"));

    idle(); key(K_BOL, KEYS_CTRL); key('v', KEYS_ALT); downs(4); key('\n', 0); idle();
    key(6, KEYS_CTRL); type("quokka\n"); idle();
    key(K_CTRL_0 + 4, KEYS_CTRL);
    page_pointer(0, 70, 65, BTN_LEFT, 1, 1); page_pointer(0, 100, 65, BTN_LEFT, -1, 0);
    page_pointer(0, 127, 65, BTN_LEFT, -1, 0); page_pointer(0, 127, 65, BTN_LEFT, 0, 0); idle();
    key(7, KEYS_CTRL); type("2\n"); idle(); check(tab_a_ready);
    check(pick_markdown); key(15, KEYS_CTRL); idle(); check(tab_b_own);
    idle(); check(tab_a_back);
    key(K_LEFT, KEYS_ALT | KEYS_SHIFT); idle(); check(tab_a_history);
    key(K_CTRL_0 + 1, KEYS_CTRL);
    run("sample.pdf: each tab's find, history and selection", sample("sample.pdf"));

    /* Prompts. */
    idle(); key(K_BOL, KEYS_CTRL); check(drop_bookmarks); key(2, KEYS_CTRL);
    key('g', KEYS_ALT); downs(10); key('\n', 0); check(prompt_rename);   /* Rename Bookmark... */
    type("Chap"); check(clip_chinese); key(22, KEYS_CTRL); key('\b', 0);
    key(K_BOL, 0); key(K_RIGHT, KEYS_SHIFT); key(K_RIGHT, KEYS_SHIFT); key(3, KEYS_CTRL); key(K_EOL, 0);
    check(prompt_typed_utf8); key('\n', 0); check(prompt_renamed);
    key('g', KEYS_ALT); downs(10); key('\n', 0); check(prompt_clicked); key('X', KEYS_SHIFT); check(prompt_click_typed);
    key(27, 0);
    run("sample.pdf: prompts", sample("sample.pdf"));
    idle(); check(clip_unchanged); type("sec"); key(1, KEYS_CTRL); key(3, KEYS_CTRL); check(password_masked);
    key(K_EOL, 0); type("ret\n"); idle(); check(password_done);
    run("locked.pdf: the masked password prompt", sample("locked.pdf"));

    /* Bookmarks: reflow-stable, and the panel's keys. */
    idle(); key(7, KEYS_CTRL); type("4\n"); idle(); key(2, KEYS_CTRL); idle(); check(reflow_bookmarked);
    for (int k = 0; k < 6; k++) { key('>', KEYS_SHIFT); idle(); }
    check(reflow_bookmark_moved);
    for (int k = 0; k < 6; k++) { key('<', 0); idle(); }
    check(reflow_bookmark_back);
    run("sample.epub: a bookmark through text size changes", sample("sample.epub"));
    idle(); check(reflow_bookmark_reopened);
    run("sample.epub: the bookmark reopened", sample("sample.epub"));
    idle(); check(panel_start); key(K_DOWN, 0); key('\n', 0); check(panel_down); key(K_UP, 0); key('\n', 0); check(panel_up);
    key(K_DEL, 0); check(panel_deleted); downs(1); key('\n', 0); check(panel_removed);
    run("sample.pdf: the bookmarks panel", sample("sample.pdf"));

    /* Gwenview's Browse and View modes. The grid at 128: cells of 144 by
     * 128 pixels from (0, 40), seven a row; the status bar's - and + at
     * columns 111 and 125 of its row 46 for "132 x 132". */
    idle(); check(browse_opened);
    key(K_RIGHT, 0); check(browse_right);
    key('\n', 0); idle(); check(viewed_b);
    key(27, 0); check(browsed_back);
    pointer_at(4 * 144 + 72, 104, BTN_LEFT, 1, 2, 0, 0); pointer_at(4 * 144 + 72, 104, BTN_LEFT, 0, 0, 0, 0); idle();
    check(double_clicked);
    key(27, 0);
    pointer_at(500, 200, 0, 0, 0, 1, KEYS_CTRL); idle(); check(size_wheel);
    pointer_at(125 * 8 + 4, TOP + 46 * 16 + 8, BTN_LEFT, 1, 1, 0, 0); check(size_larger);
    pointer_at(111 * 8 + 4, TOP + 46 * 16 + 8, BTN_LEFT, 1, 1, 0, 0); check(size_smaller);
    key('v', KEYS_ALT); downs(19); key('\n', 0); downs(1); key('\n', 0); idle(); check(sorted_by_date);
    key(K_RIGHT, 0); key(K_RIGHT, 0); key('\n', 0); idle(); check(raw_viewed);
    key(K_F1 + 3, 0); idle(); check(info_shown);
    key('\t', 0); key('\n', 0); check(info_more); key(27, 0);
    key(K_UP, KEYS_ALT); check(up_to_browse);
    key(K_UP, KEYS_ALT); idle(); check(up_to_parent);
    key('\n', 0); idle(); check(back_in);
    key(K_BOL, 0); key('\n', 0); idle(); check(archive_browsed);
    key(K_UP, KEYS_ALT); idle(); check(archive_up);
    key('v', KEYS_ALT); downs(19); key('\n', 0); key('\n', 0); idle(); check(sort_name_back);
    run("gallery: Browse and View modes, Sort By Date, a RAW page, the Information panel, folders and an archive",
        sample("gallery"));
    idle(); key(27, 0); idle(); check(image_escaped);
    run("sample.png: Esc browses its folder", sample("sample.png"));
    idle(); check(years_browsed); key('\n', 0); idle(); check(year_in); key(K_UP, KEYS_ALT); idle(); check(year_up);
    run("years: a folder of folders only", sample("years"));

#ifdef CONFIG_IME
    char ime_doc[PATH_MAX];
    snprintf(ime_doc, sizeof ime_doc, "%s/ime-search.md", out_dir);
    FILE *im = fopen(ime_doc, "w");
    assert(im); fputs("# Chinese find\n\n你，泥，妮。\n", im); fclose(im);
    assert(chown(ime_doc, VIEW_USER, pw->pw_gid) == 0);
    idle(); check(parity_ime_start); idle(); check(parity_ime_found); check(ime_prompt);
    run("real Rime search commit", ime_doc);
#endif
    puts("ALL PASSED");
    return 0;
}
