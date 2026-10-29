/*
 * view [PATH]: documents on the screen, as Okular shows them (PDF, XPS,
 * EPUB, MOBI, FictionBook, Markdown, text, HTML, OpenDocument text, comic
 * books, DjVu, PostScript, DVI, fax and images). The documents are read
 * by view-render (view-proto.h), as the user in a sandbox; this program is
 * the screen, the keys, the pointer and the clipboard. It never waits for a
 * page: one request is out at a time, the pages on the screen first, then
 * those next to them, then the sidebar's thumbnails, and what is no longer
 * wanted is cancelled (SIGUSR1). Without PATH, or on Ctrl+O, a file is chosen as in
 * home's file manager.
 *
 * Okular's keys (its part.cpp, pageview.cpp and KDE's standard shortcuts):
 *   Ctrl+O open   Ctrl+Q quit   Alt+Return properties   Ctrl+F or / find
 *   F3, Shift+F3 the next, previous match   Ctrl+G go to page   Ctrl+C copy
 *   Ctrl++ Ctrl+- zoom in, out   Ctrl+0 zoom to 100%   F7 the sidebar
 *   Space, PgDn / Shift+Space, PgUp, Backspace: a screen down / up
 *   Down, J / Up, K: a step down / up (Shift+J, Shift+K: ten)
 *   Left, H / Right, L: sideways, or the previous / next page
 *   Ctrl+Home, Ctrl+End: the beginning, the end of the document
 *   Alt+Shift+Left / Alt+Shift+Right: back / forward (after links, ...)
 *   Ctrl+1 to Ctrl+6, Tools: Browse, Zoom, Area Selection, Text Selection,
 *   Table Selection, Magnifier (Okular's mouse modes)
 *   Ctrl+Shift+P, View > Presentation: a page at a time on the whole screen
 *   (Right, Space, PgDn, Down and Left, Backspace, PgUp, Up, Home, End, the
 *   buttons and the wheel; B black; Esc leaves)
 *   View > Color Mode, Change Colors: Okular's colour modes and their toggle
 *   View > Overview Columns, Use Right to Left Reading Direction
 *   File > Open Recent, Open Containing Folder
 *   Esc: closes the find bar, clears the selection
 *   Alt+F, E, V, G, T, S: the menus, for what has no key in Okular either
 *   (fit width or page, facing pages, rotation, inverted colours, the
 *   embedded files, export as plain text)
 *   < and > (MuPDF's viewer's keys): a reflowed document's text smaller, larger
 *   A PDF's (view-pdf-ui.inc): F6 the annotation tools, Ctrl+Z, Ctrl+Shift+Z
 *   undo, redo, Ctrl+S, Ctrl+Shift+S save, save as; its forms, signatures
 * The pointer: with Browse, dragging moves the pages and a click follows a
 * link (the right button dragged is Area Selection); with Text Selection,
 * dragging selects text for the primary selection (a double click a word, a
 * triple click a line); Zoom, Area and Table Selection drag a rectangle; the
 * Magnifier shows the page at ten times while a button is held. The wheel scrolls; with
 * Ctrl, or a pinch, it zooms at the pointer, as does the middle button
 * dragged up or down. The right button gives Okular's menus.
 *
 * Every question is typed with the find bar's editor (UTF-8, the input
 * method, selection and the clipboard). A reflowed document's bookmarks keep
 * their place in the text (MuPDF's bookmarks) through new layouts.
 *
 * Each document's place, zoom, rotation and view mode are kept in the
 * user's ~/.local/state/locachy/view, written as the user (fsuid 1000) when
 * they have not changed for a second: process 1 may freeze this program, or
 * end it, without notice.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "base.h"
#include "internal.h"
#include "clipboard.h"
#include "ime.h"
#include "files.h"
#include "view-layout.h"
#include "view-proto.h"

#define FG      0xD0D0D0
#define BG      0x101820
#define DESK    0x2E3238 /* around the pages */
#define PAPER   0xFFFFFF /* a page not drawn yet */
#define ACCENT  0x80C0FF
#define DIM     0x808890
#define ERROR   0xFF9090
#define TOAST   0x183860
#define FIND_HIT   0xFFFF40 /* Okular's find bar: qRgb(255, 255, 64) */
#define SELECTED   0x3DAEE9 /* Breeze's selection */
/* The rendered pages kept: 256 MiB is a dozen pages at fit width on a 1920
 * by 1080 screen (19 MiB each), the pages on the screen and those next to
 * them several times over, and a small part of the memory of the X230 (4 to
 * 16 GiB). A page of more than WHOLE_MAX pixels is drawn from tiles of TILE
 * by TILE (4 MiB each), only those on the screen. */
#define CACHE_LIMIT (256ull << 20)
#define WHOLE_MAX   (8u << 20)
#define TILE        1024
#define HISTORY     100   /* Okular's OKULAR_HISTORY_MAXSTEPS */
#define STATE_LINES 200   /* documents remembered */
#define QUEUE       64
#define EM_DEFAULT  11.0f /* MuPDF's FZ_DEFAULT_LAYOUT_EM, points */
#define EM_MIN      6.0f  /* and its viewer's limits */
#define EM_MAX      36.0f
#define STATE_FILE  ".local/state/locachy/view"

/* Okular's mouse modes (okular.kcfg MouseMode, Tools menu, Ctrl+1 to Ctrl+6;
 * pageview.cpp setupViewerActions and setupActions). */
enum { TOOL_BROWSE, TOOL_TEXT, TOOL_ZOOM, TOOL_AREA, TOOL_TABLE, TOOL_MAGNIFIER };
enum { FOCUS_DOC, FOCUS_SIDEBAR, FOCUS_FIND, FOCUS_PROMPT, FOCUS_LIST };
enum { TAB_CONTENTS, TAB_THUMBNAILS, TAB_BOOKMARKS, TAB_ANNOTATIONS, TAB_SIGNATURES };
enum { J_NONE, J_OPEN, J_AUTH, J_PAGE, J_RENDER, J_THUMB, J_LINKS, J_HITS, J_SELECT, J_OUTLINE,
       J_INFO, J_EXPORT, J_EMBEDDED, J_SAVE, J_LAYOUT, J_RESOLVE, J_TOPTEXT, J_REFIND, J_BBOX,
       J_PDF_STATUS, J_PDF_ANNOTS, J_PDF_FIELDS, J_PDF_SIGS, J_PDF_EDIT, J_PDF_WRITE, J_WORDS, J_MAGNIFY,
       J_MARK, J_FIND_MARK, J_SLIDE };
enum { P_NONE, P_GOTO, P_PASSWORD, P_EXPORT, P_SAVE, P_OVERWRITE, P_BOOKMARK,
       P_PDF_TEXT, P_PDF_SAVE_AS, P_PDF_UNSAVED, P_PDF_CONSENT, P_COLOR, P_IMAGE };
enum { L_NONE, L_INFO, L_EMBEDDED };

/* Okular's Change Colors (okular_core.kcfg Document: ChangeColors,
 * RenderMode, PaperColor; okular.kcfg Dlg Accessibility: RecolorForeground,
 * RecolorBackground, BWThreshold, BWContrast), in its RenderMode order. */
enum { CM_INVERTED, CM_PAPER, CM_RECOLOR, CM_BLACK_WHITE, CM_INVERT_LIGHTNESS, CM_INVERT_LUMA,
       CM_INVERT_LUMA_SYMMETRIC, CM_HUE_POSITIVE, CM_HUE_NEGATIVE, CM_COUNT };

/* The actions: of the keys, the menus and the buttons. */
enum { A_NONE, A_OPEN, A_PROPERTIES, A_EMBEDDED, A_EXPORT, A_QUIT, A_COPY, A_FIND, A_FIND_NEXT,
       A_FIND_PREV, A_ZOOM_IN, A_ZOOM_OUT, A_ZOOM_100, A_FIT_WIDTH, A_FIT_PAGE, A_SINGLE, A_FACING,
       A_ROTATE_RIGHT, A_ROTATE_LEFT, A_ROTATE_NONE, A_INVERT, A_FONT_UP, A_FONT_DOWN, A_PREV_PAGE,
       A_NEXT_PAGE, A_FIRST, A_LAST, A_BACK, A_FORWARD, A_GOTO, A_BROWSE, A_TEXT_SELECT, A_SIDEBAR,
       A_FOLLOW_LINK, A_COPY_LINK, A_COPY_TEXT, A_TRIM_MARGINS, A_TRIM_SELECTION, A_FIT_AUTO, A_FIRST_CENTERED, A_OVERVIEW, A_CONTINUOUS, A_RELOAD, A_BOOKMARK, A_BOOKMARK_PREV, A_BOOKMARK_NEXT, A_BOOKMARK_RENAME, A_CLOSE_TAB, A_REOPEN_TAB, A_NEXT_TAB, A_PREV_TAB,
       A_SAVE, A_SAVE_AS, A_UNDO, A_REDO, A_ANNOTATE, A_SHOW_FORMS, A_ANNOT_OPEN, A_ANNOT_COPY, A_ANNOT_DELETE,
       A_ANNOT_PROPERTIES, A_TAB_SWITCH, A_PRESENTATION, A_COLOR_MENU, A_CHANGE_COLORS, A_COLOR_NORMAL,
       A_COLOR_MODE, A_COLOR_MODE_LAST = A_COLOR_MODE + CM_COUNT - 1, A_COLOR_CONFIG, A_COLUMNS_MENU, A_RTL,
       A_ZOOM_TOOL, A_AREA_TOOL, A_TABLE_TOOL, A_MAGNIFIER, A_RECENT, A_CONTAINING, A_AREA_COPY, A_AREA_COPY_FLAT,
       A_AREA_SEARCH, A_AREA_IMAGE_COPY, A_AREA_IMAGE_SAVE, A_TABLE_COPY, A_BOOKMARK_REMOVE };

/* Places the pointer clicks, as registered with ui_hit. */
#define HIT_MENU   1000    /* + the menu */
#define HIT_GOTO   1100
#define HIT_ZOOM   1101
#define HIT_TAB    1200    /* + the tab */
#define HIT_FIND   1300    /* + 0 next, 1 previous, 2 close */
#define HIT_SIDEBAR 1150
#define HIT_FIND_INPUT 1350
#define HIT_LIST   1400    /* + 0 save, 1 close */
#define HIT_LINE   100000  /* + a line of the list */
#define HIT_TOC    1000000 /* + an entry of the contents */
#define HIT_THUMB  2000000 /* + a page */
#define HIT_BOOKMARK 3000000 /* + a bookmarked page */
#define HIT_DOCUMENT_TAB 5000
#define HIT_CLOSE_TAB 6000
#define TABS_MAX 32
struct tab_keep;
static struct {
    char *path[TABS_MAX], *closed[TABS_MAX];
    struct tab_keep *keep[TABS_MAX]; /* an inactive tab's find, history, selection */
    int n, current, nclosed;
} tabs;

#define RECENT_MAX 10         /* okular.kcfg MaxRecentItems */
static struct {
    char *path[RECENT_MAX];   /* the newest first */
    int n;
} recent;
static int tab_rows(void) { return tabs.n > 1 ? 1 : 0; }

struct job {
    int kind, page, tx, ty, rotate, gen;
    float zoom;
    struct view_request req;
};

struct page {
    float pw, ph;             /* points, unrotated */
    unsigned char known;      /* its size has come (else an estimate) */
    unsigned char asked;      /* its size was asked for */
    unsigned char links;      /* 0: not asked, 1: asked, 2: here */
    unsigned char hits;       /* the same, for the search */
    unsigned char failed;     /* cannot be shown */
    unsigned char deaths;     /* times view-render ended rendering it */
    unsigned char text;       /* VIEW_HAS_TEXT */
    unsigned char bounds;     /* 0 unknown, 1 requested, 2 known */
    struct vl_crop bbox;
    float bbox_failed;        /* the display zoom whose raster could not be scanned (0: none) */
    struct view_link *link;
    int nlink;
    struct view_rect *hit;
    int nhit;
    /* A PDF's: its annotations and form fields (0 not asked, 1 asked, 2 here). */
    unsigned char annots, fields;
    struct view_annot *annot;
    int nannot;
    struct view_field *field;
    int nfield;
    struct view_option *option;
    int noption;
};

/* A PDF's annotations, forms, signatures and saving (view-pdf-ui.inc). */
struct item;
static int pdf_on(void);                 /* view-render edits this PDF */
static int pdf_modified(void);
static int pdf_rows(void);               /* rows of its bars above the pages */
static void pdf_opened(int again);
static void pdf_forget(void);
static void pdf_free_page(struct page *pg);
static int pdf_background(struct job *j);
static void pdf_reply(const struct job *j, const struct view_reply *rep, const void *data, size_t size);
static void pdf_overlays(int page);
static void pdf_draw(void);
static int pdf_draw_bottom(int row);
static int pdf_sidebar_tabs(int col);
static int pdf_draw_sidebar(void);
static int pdf_key_sidebar(int key, int mods);
static int pdf_wheel_sidebar(int wheel);
static int pdf_click(int id, const struct pointer_event *e);
static int pdf_pointer(const struct pointer_event *e, int in_area);
static int pdf_context_menu(int X, int Y, int col, int row);
static int pdf_key_global(int key, int mods);
static int pdf_key_doc(int key, int mods);
static int pdf_key_prompt(int key, int mods);
static int pdf_guard(int action, int arg);
static void pdf_action(int a);
static int pdf_editing(void);
static void pdf_insert(const char *s, size_t len);
static void pdf_menu_items(int m, struct item *it, int *n);

static struct {
    int open;                 /* there are pages to show */
    int opening;              /* OPEN or AUTH is out */
    int gen;                  /* a new one with each document and layout */
    char path[PATH_MAX];
    char format[64], title[256];
    int pages;
    unsigned flags;
    struct page *page;
    float *pw, *ph;
    struct vl_item *item;
    struct vl_crop *crop;
    int full_w, full_h;
    struct view_outline *outline;
    int noutline;
    char password[VIEW_TEXT];
    int with_password;
    float em, lw, lh;         /* a reflowed document's font size and page, points */
    int size_next;            /* the next page whose size to ask for */
    char **bookmark;          /* per-page title, persisted with the document */
    char **bmark;             /* and, of a reflowed document, its MuPDF bookmark (VIEW_MARK) */
    struct bpend { char *title, *mark; int page; } *bpend; /* bookmarks being placed (VIEW_FIND_MARK) */
    int nbpend;
} doc;

/* Kept across documents and runs, as Okular keeps its settings. */
static struct {
    int sidebar, invert, tool, facing; /* invert: Okular's ChangeColors, in render_mode */
    int side_width, trim_margins; /* logical pixels, and the persistent trim mode */
    int paged;
    int render_mode, bw_threshold, bw_contrast;
    uint32_t paper, recolor_fg, recolor_bg;
    int columns, rtl;         /* okular.kcfg ViewColumns (1 to 8), rtlReadingDirection */
} settings;

static void settings_default(void)
{
    settings = (typeof(settings)){ .sidebar = 1, .tool = TOOL_BROWSE, .render_mode = CM_INVERTED, .bw_threshold = 127,
        .bw_contrast = 2, .paper = 0xFFFFFF, .recolor_fg = 0x600000, .recolor_bg = 0xF0F0F0, .columns = 3 };
}

static struct {
    int selection, picking, dragging, page;
    double xa, ya, xb, yb;
    struct vl_crop box; /* unrotated normalized selection, shared by every page */
} trim;

static struct {
    int mode;                 /* VL_FIXED ... (Okular's default: fit width) */
    double zoom;              /* VL_FIXED: Okular's factor, 1 for 100% */
    int rotate, facing;
    int sx, sy;               /* the content's pixel at the area's top left */
    int current;
} v = { VL_FIT_WIDTH, 1, 0, 0, 0, 0, 0 };

/* A place in the document: the area's top left, in fractions of a page's item. */
struct pos {
    int page;
    double nx, ny;
};

static struct {
    struct pos at[HISTORY];
    int n, i;
} hist;

/* view-render and the one request out. */
static int sock = -1;
static pid_t render_pid = -1;
static int restarts;
static double last_death;
static struct {
    int busy, cancelled;
    struct job job;
} fly;
static struct job queue[QUEUE];
static int nqueue;

/* The screen: pixels, cells, the document area and the sidebar. */
static int W, H, cw, ch, top, ncols, nrows;
static int ax, ay, aw, ah, side_cols, side_px, splitter_px;
static void viewport_reflow(void);
static double scale = 1, z100 = 96.0 / 72.0; /* pixels a point at 100%: 96 dpi at the screen's scale */
static int col_margin, row_margin;
static uint32_t *scratch;
static size_t scratch_size;

/* The rendered pictures. */
struct pix {
    uint64_t used;            /* 0: an empty slot */
    int kind, page, rotate, tx, ty; /* kind 1: a thumbnail; tx -1: the whole page */
    float zoom;
    int w, h;
    uint32_t *px;
    size_t size;
};
static struct pix *cache;
static int ncache;
static uint64_t ticks, frame_start;

static struct {
    int active;               /* rects to show */
    int dragging, started;    /* the left button is down; it has moved */
    int mode;
    int pa, pb;               /* the ends: pages and points */
    double xa, ya, xb, yb;
    int press_x, press_y;
    int pending;              /* the range changed: ask again */
    int final;                /* released: the text goes to the primary selection */
    int round, next, waiting; /* a round of requests, the next page of it */
    int from, to;             /* the round's pages */
    struct sel_part { int page; struct view_rect *rect; int nrect; char *text; size_t len; } *part, *staged;
    int npart, nstaged;
} sel;

/* A line being typed: the find bar's and every question's (and a PDF's
 * texts), one editor: UTF-8, an insertion point and a selection, the
 * clipboard and the input method. */
struct line {
    char text[VIEW_TEXT];
    size_t cursor, mark, left; /* UTF-8 byte boundaries; insertion, selection and scroll */
    size_t limit;             /* bytes it holds at most (0: sizeof text - 1) */
    int max;                  /* characters at most (0: any; Okular's QLineEdit::setMaxLength) */
    int replace;              /* typing replaces it (all selected, as Okular's) */
    int multiline, masked;    /* Shift+Enter a new line; shown as '*' (no input method) */
    int col, width;           /* where it was drawn */
};

static struct {
    int bar;                  /* the find bar is shown */
    struct line in;           /* what is typed */
    double typed;             /* when (find as you type, 700 ms later) */
    char active[256];         /* what the hits are of */
    int gen;
    int cur_page, cur_hit;    /* the current match, or -1 */
    int dir;                  /* looking: +1 or -1, else 0 */
    int step_page, after, done;
    int failed;               /* no match */
} find = { .cur_page = -1 };

static struct {
    int kind, masked;
    char question[600];
    struct line in;           /* the answer, typed as in the find bar */
    int then;                 /* P_OVERWRITE: what it was for */
} prompt;
static char target[PATH_MAX]; /* where an export or embedded file goes */
static int save_index = -1;   /* the embedded file */

static struct {
    int kind, n, sel, top;
    char **line;              /* as shown */
    int *index;               /* L_EMBEDDED: each file's index, -1 for a heading */
    char **name;              /* and its name */
} list;

static struct {
    double em;                /* the new size */
    char needle[96];          /* the text at the top before */
    int page, tries, busy;
    float lw, lh;
} font;

static struct {
    int focus, dirty, quit, tab;
    int toc_sel, toc_top, thumb_sel, thumb_top; /* thumb_top: pixels */
    int bm_sel, bm_top;       /* the bookmarks panel's selected row and first shown */
    int follow_thumbs;        /* keep the current page's thumbnail in sight */
    char toast[256];
    double toast_until;
    int toast_error;
    int cursor;
    double changed;           /* when the place or a setting last changed (0: saved) */
} ui = { .cursor = -1 };

static struct {
    int x, y;
    int grab, grab_x, grab_y, moved; /* Browse: dragging the pages */
    int press_x, press_y, press_page, press_link;
    int mid, mid_y;           /* the middle button: zooming */
    int splitter, split_offset, find_drag;
    int right;                /* the right button is down (on the pages) */
} ptr = { .press_page = -1, .press_link = -1 };

/* Okular's other mouse tools (pageview.cpp mousePressEvent, mouseMoveEvent,
 * mouseReleaseEvent, guessTableDividers, getTableContents; magnifierview.cpp):
 * a rectangle dragged for Zoom, Area Selection and Table Selection, in the
 * content's pixels; the Magnifier while a button is held. */
static struct {
    int dragging, button, prev_tool; /* prev_tool: Browse's right drag (-1: none) */
    int x0, y0, x1, y1;
} rsel = { .prev_tool = -1 };

/* The words view-render gives (VIEW_WORDS) of the pages a rectangle covers. */
struct word {
    int page, line;
    float x0, y0, x1, y1;     /* page points */
    char *text;
};
static struct {
    int purpose, pending;     /* 1 Area Selection's menu, 2 Table Selection; replies still to come */
    int x0, y0, x1, y1;       /* the rectangle, content pixels */
    struct word *w;
    int n, size;
    char *text;               /* Area Selection's text */
} words;

/* Table Selection: its parts (a page's piece of the rectangle) and dividers,
 * normalized in the rectangle as Okular's tableSelectionCols and Rows. */
#define TABLE_LINES 64
static struct {
    int on, guessed;
    int x0, y0, x1, y1;       /* content pixels */
    int nparts, part_page[16];
    int part[16][4];          /* content pixels, within the rectangle */
    struct vl_item item[16];  /* its pages as laid out then: laid out again, it goes */
    double cols[TABLE_LINES], rows[TABLE_LINES];
    int ncols, nrows;
} table;

#define MAG_W 351             /* Okular's magnifier: 351 by 201, 10 pixels a page pixel */
#define MAG_H 201
#define MAG_SCALE 10
static struct {
    int on, X, Y, page, asked, seq, done_seq;
    double px, py;
    int w, h;
    uint32_t *px_buf;
} mag;


static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int clampi(int x, int lo, int hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static void as_user(int on)
{
    setfsgid(on ? VIEW_USER : 0);
    setfsuid(on ? VIEW_USER : 0);
}

__attribute__((format(printf, 2, 3)))
static void toast(int error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui.toast, sizeof ui.toast, fmt, ap);
    va_end(ap);
    for (char *c = ui.toast; *c; c++) {
        if ((unsigned char)*c < ' ') *c = ' '; /* what view-render said: no control characters */
    }
    ui.toast_error = error;
    ui.toast_until = now() + (error ? 5 : 3);
    ui.dirty = 1;
    say("view: %s", ui.toast);
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* The bottom row is the find bar's or a question's. */
static int bottom_used(void)
{
    return find.bar || prompt.kind != P_NONE;
}

static void geometry(void)
{
    screen_pixels(&W, &H);
    screen_cells(&cw, &ch, &top);
    ncols = screen_cols();
    nrows = screen_rows();
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    scale = screen_scale();
    if (!(scale > 0)) scale = 1;
    z100 = 96.0 / 72.0 * scale;
    col_margin = (int)(6 * scale + 0.5); /* Okular's kcolWidthMargin and krowHeightMargin */
    row_margin = (int)(12 * scale + 0.5);
    splitter_px = (int)lround(5 * scale);
    if (splitter_px < 1) splitter_px = 1;
    int logical_w = (int)(W / scale);
    if (!settings.side_width) settings.side_width = clampi(logical_w / 11, 90, 600);
    side_px = settings.sidebar ? (int)lround(clampi(settings.side_width, 90, 600) * scale) : 0;
    /* Preserve the stored width on a temporarily narrow screen. */
    if (side_px > W - splitter_px - cw) side_px = W - splitter_px - cw;
    if (side_px < 0) side_px = 0;
    side_cols = side_px / cw;
    ax = side_px ? side_px + splitter_px : 0;
    ay = top + (2 + tab_rows() + pdf_rows()) * ch;
    int bottom = bottom_used() ? top + (nrows - 1) * ch : H;
    aw = W - ax > 1 ? W - ax : 1;
    ah = bottom - ay > 1 ? bottom - ay : 1;
    size_t need = (size_t)(W > 0 ? W : 1) * (size_t)(H > 0 ? H : 1);
    if (need > scratch_size) {
        uint32_t *s = realloc(scratch, need * sizeof *s);
        if (s) {
            scratch = s;
            scratch_size = need;
        }
    }
}

static struct vl_params params(void)
{
    return (struct vl_params){ aw, ah, v.facing, v.rotate, v.mode, v.zoom * z100, col_margin, row_margin,
        settings.paged, v.current, settings.columns, settings.rtl };
}

static int columns(void)
{
    return v.facing == VL_OVERVIEW ? (doc.pages > 0 && doc.pages < settings.columns ? doc.pages : settings.columns) :
        v.facing == VL_FIRST_CENTERED ? (doc.pages >= 3 ? 2 : 1) :
        v.facing == VL_FACING && doc.pages > 1 ? 2 : 1;
}

static void clamp_scroll(void)
{
    v.sx = clampi(v.sx, 0, doc.full_w - aw > 0 ? doc.full_w - aw : 0);
    v.sy = clampi(v.sy, 0, doc.full_h - ah > 0 ? doc.full_h - ah : 0);
}

static void layout(void)
{
    if (!doc.open) return;
    struct vl_params p = params();
    for (int i = 0; i < doc.pages; i++) {
        struct vl_crop b = { 0, 0, 1, 1 };
        if (settings.trim_margins && doc.page[i].bounds == 2) b = vl_crop_box(doc.page[i].bbox, v.rotate, 1);
        else if (trim.selection && !trim.picking) b = vl_crop_box(trim.box, v.rotate, 0);
        doc.crop[i] = b;
    }
    vl_layout_crop(&p, doc.pages, doc.pw, doc.ph, doc.crop, doc.item, &doc.full_w, &doc.full_h);
    clamp_scroll();
}

/* A screen point and the page's point under it, to keep there. */
struct anchor {
    int page, X, Y;
    double px, py;
};

static struct anchor anchor_at(int X, int Y)
{
    struct anchor a = { -1, X, Y, 0, 0 };
    if (doc.open) {
        struct vl_params p = params();
        a.page = vl_screen_to_page(&p, doc.pages, doc.item, doc.pw, doc.ph, ax, ay, v.sx, v.sy, X, Y, 1,
                                   &a.px, &a.py);
    }
    return a;
}

static void anchor_keep(const struct anchor *a)
{
    if (a->page >= 0 && a->page < doc.pages) {
        struct vl_params p = params();
        double X, Y;
        vl_page_to_screen(&p, &doc.item[a->page], doc.pw[a->page], doc.ph[a->page], ax, ay, v.sx, v.sy,
                          a->px, a->py, &X, &Y);
        v.sx += (int)lround(X - a->X);
        v.sy += (int)lround(Y - a->Y);
    }
    clamp_scroll();
}

/* Lay out again, keeping what is at screen point (X, Y) there. */
static void relayout_at(int X, int Y)
{
    struct anchor a = anchor_at(X, Y);
    layout();
    anchor_keep(&a);
}

/* The page at the middle of the area: Okular's current page; of facing
 * pages, the row's first. */
static int current_page(void)
{
    if (!doc.open) return 0;
    if (settings.paged) return clampi(v.current, 0, doc.pages - 1);
    int i = vl_item_at(doc.pages, doc.item, v.sx + aw / 2, v.sy + ah / 2, 1);
    if (v.facing == VL_FIRST_CENTERED && doc.pages >= 3) {
        if (i > 0 && i % 2 == 0) i--;
    } else if (i > 0) i = i / columns() * columns();
    return i < 0 ? 0 : i;
}

static struct pos pos_now(void)
{
    struct pos p = { 0, 0, 0 };
    if (!doc.open) return p;
    /* The page at the top edge; in the gap above a page, that page. */
    p.page = vl_item_at(doc.pages, doc.item, v.sx + aw / 2, v.sy, 0);
    if (p.page < 0) p.page = vl_item_at(doc.pages, doc.item, v.sx + aw / 2, v.sy + row_margin + 1, 1);
    if (p.page < 0) p.page = 0;
    const struct vl_item *it = &doc.item[p.page];
    p.ny = (v.sy - it->y) / (double)it->h;
    p.nx = (v.sx - it->x) / (double)it->w;
    return p;
}

static void page_prepare(int page)
{
    if (settings.paged && doc.open && (v.current != page || doc.item[page].hidden)) {
        v.current = page;
        layout();
    }
}

static void pos_go(struct pos p)
{
    if (!doc.open) return;
    p.page = clampi(p.page, 0, doc.pages - 1);
    if (settings.paged) { v.current = p.page; layout(); }
    const struct vl_item *it = &doc.item[p.page];
    v.sy = it->y + (int)lround(p.ny * it->h);
    v.sx = it->x + (int)lround(p.nx * it->w);
    clamp_scroll();
}

/* A page's top at the area's top, less Okular's 10 pixels, the page in the middle. */
static struct pos pos_page(int page)
{
    page = clampi(page, 0, doc.pages - 1);
    page_prepare(page);
    const struct vl_item *it = &doc.item[page];
    return (struct pos){ page, (it->w / 2 - aw / 2) / (double)it->w, -10 * scale / it->h };
}

/* A page's point (points) at the area's top left (Okular's TopLeft). */
static struct pos pos_point(int page, double x, double y)
{
    page = clampi(page, 0, doc.pages - 1);
    page_prepare(page);
    if (!isfinite(x) || x < 0) x = 0;
    if (!isfinite(y) || y < 0) return pos_page(page);
    const struct vl_item *it = &doc.item[page];
    double ix, iy;
    vl_point_to_item(v.rotate, doc.pw[page], doc.ph[page], it->zoom, x, y, &ix, &iy);
    return (struct pos){ page, (ix - it->ox) / it->w, (iy - it->oy) / it->h };
}

static void check_stale(void);
static struct {
    int on, page, bar, black, playing, press, press_link;
    double overlay_until, next_at;
} pres;                       /* the presentation */
static void draw_presentation(void);
static int pres_background(struct job *j);
static int pres_wanted(const struct job *j);
static void pres_key(int key, int mods);
static void pres_pointer(const struct pointer_event *e);
static void pres_enter(void);
static void pres_exit(void);
static void pres_go(int page);
static void keep_restore(void);
static void keep_free(struct tab_keep *k);
static void recent_clear(void);
static void got_words(const struct job *j, const struct view_reply *rep, const void *data, size_t size);
static void got_magnify(const struct job *j, const struct view_reply *rep, const void *data, size_t size);
static void draw_tools(void);
static void tool_set(int tool);
static void tools_reset(void);
static void area_action(int a);
static void table_copy(void);
static void image_named(void);
static void save_image(void);
static void table_clear(void);
static int utf8_count(const char *s);

/* The place, the zoom or a setting changed: draw, drop work no longer
 * wanted, and save it in a while. */
static void changed(void)
{
    ui.dirty = 1;
    ui.changed = now();
    check_stale();
}

/* Okular's history (Document::setViewport): the current entry follows the
 * place; going to another page adds an entry after it, dropping those
 * forward of it. */
static void hist_note(void)
{
    if (hist.n == 0) hist.n = 1;
    hist.at[hist.i] = pos_now();
}

static void go(struct pos to)
{
    if (!doc.open) return;
    hist_note();
    if (hist.at[hist.i].page != to.page) {
        hist.n = hist.i + 1;
        if (hist.n == HISTORY) {
            memmove(hist.at, hist.at + 1, sizeof hist.at[0] * (HISTORY - 1));
            hist.n--;
        }
        hist.i = hist.n++;
    }
    hist.at[hist.i] = to;
    pos_go(to);
    ui.follow_thumbs = 1;
    changed();
}

static void hist_move(int step)
{
    if (!doc.open) return;
    hist_note();
    if (hist.i + step < 0 || hist.i + step >= hist.n) return;
    hist.i += step;
    pos_go(hist.at[hist.i]);
    ui.follow_thumbs = 1;
    changed();
}

/* The pictures. */
static int pix_find(int kind, int page, int rotate, float zoom, int tx, int ty)
{
    for (int i = 0; i < ncache; i++) {
        const struct pix *c = &cache[i];
        if (c->used && c->kind == kind && c->page == page && c->rotate == rotate && c->zoom == zoom &&
            c->tx == tx && c->ty == ty) {
            return i;
        }
    }
    return -1;
}

static void pix_drop(int i)
{
    if (cache[i].used && cache[i].px) munmap(cache[i].px, cache[i].size);
    memset(&cache[i], 0, sizeof cache[i]);
}

static void pix_flush(void)
{
    for (int i = 0; i < ncache; i++) pix_drop(i);
}

static uint64_t pix_bytes(int w, int h)
{
    return (uint64_t)w * (uint64_t)h * 4;
}

/* Room for need bytes, dropping the least recently used, never what the
 * screen shows (or anything when keep is 0): 0, or -1 if there is none. */
static int pix_room(uint64_t need, uint64_t keep, int apply)
{
    struct vl_lru *e = calloc((size_t)ncache + 1, sizeof *e);
    int *out = calloc((size_t)ncache + 1, sizeof *out);
    int n = -1;
    if (e && out) {
        for (int i = 0; i < ncache; i++) {
            e[i] = (struct vl_lru){ cache[i].used ? cache[i].size : 0, cache[i].used };
        }
        n = vl_evict(e, ncache, CACHE_LIMIT, need, keep, out);
        for (int k = 0; apply && k < n; k++) pix_drop(out[k]);
    }
    free(e);
    free(out);
    return n < 0 ? -1 : 0;
}

/* Keep a picture (mapped, size bytes); what the screen shows is kept even
 * over the limit, dropping all else. 0, or -1 (then it is unmapped). */
static int pix_add(int kind, int page, int rotate, float zoom, int tx, int ty, int w, int h, uint32_t *px,
                   size_t size, int on_screen)
{
    if (pix_room(size, frame_start, 1) < 0) {
        if (!on_screen) {
            munmap(px, size);
            return -1;
        }
        for (int i = 0; i < ncache; i++) {
            if (cache[i].used && cache[i].used < frame_start) pix_drop(i);
        }
    }
    int slot = -1;
    for (int i = 0; i < ncache && slot < 0; i++) {
        if (!cache[i].used) slot = i;
    }
    if (slot < 0) {
        struct pix *c = realloc(cache, sizeof *c * (size_t)(ncache + 16));
        if (!c) {
            munmap(px, size);
            return -1;
        }
        cache = c;
        memset(cache + ncache, 0, sizeof *c * 16);
        slot = ncache;
        ncache += 16;
    }
    cache[slot] = (struct pix){ ++ticks, kind, page, rotate, tx, ty, zoom, w, h, px, size };
    return 0;
}

/* view-render. */
static void reopen(void);

static void start_renderer(void)
{
    render_pid = view_spawn(&sock);
    if (render_pid < 0) {
        sock = -1;
        /* TRANSLATORS: %s is the system error message (strerror). */
        toast(1, _("Cannot start the document renderer: %s."), strerror(errno));
    }
}

static void stop_renderer(void)
{
    if (sock >= 0) close(sock);
    if (render_pid > 0) {
        kill(render_pid, SIGKILL);
        waitpid(render_pid, NULL, 0);
    }
    sock = -1;
    render_pid = -1;
    fly.busy = 0;
}

static void push_job(const struct job *j, int first)
{
    if (nqueue == QUEUE) return;
    if (first) {
        memmove(queue + 1, queue, sizeof queue[0] * (size_t)nqueue);
        queue[0] = *j;
    } else {
        queue[nqueue] = *j;
    }
    nqueue++;
    /* What is asked for now goes before a picture being drawn. */
    if (fly.busy && !fly.cancelled && (fly.job.kind == J_RENDER || fly.job.kind == J_THUMB || fly.job.kind == J_SLIDE) &&
        render_pid > 0) {
        kill(render_pid, SIGUSR1);
        fly.cancelled = 1;
    }
}

static struct job new_job(int kind, int op)
{
    struct job j;
    memset(&j, 0, sizeof j);
    j.kind = kind;
    j.gen = doc.gen;
    j.page = -1;
    j.req.op = (uint32_t)op;
    return j;
}

/* view-render has gone (ended, or said something it should not): start it
 * again and open the document where it was. Twice on the same page, and
 * that page is not drawn again; three times in a row with no answer at
 * all, and the document is closed. */
static void renderer_gone(void)
{
    int was = fly.busy ? fly.job.kind : J_NONE, page = fly.job.page;
    stop_renderer();
    if (was == J_RENDER && page >= 0 && page < doc.pages && doc.gen == fly.job.gen &&
        ++doc.page[page].deaths >= 2) {
        doc.page[page].failed = 1;
    }
    nqueue = 0;
    sel.pending = sel.pending || sel.round; /* a round cut short is asked again */
    sel.waiting = sel.round = 0;
    find.dir = 0;
    /* Deaths count against it when they come within ten seconds of each other. */
    if (now() - last_death > 10) restarts = 0;
    last_death = now();
    if (++restarts > 3) {
        toast(1, _("The document renderer keeps stopping; the document is closed."));
        doc.open = doc.opening = 0;
        return;
    }
    toast(1, _("The document renderer stopped; it was started again."));
    start_renderer();
    if (sock >= 0 && (doc.open || doc.opening)) reopen();
}

static void send_job(const struct job *j)
{
    struct job copy = *j;
    if (view_send(sock, &copy.req) < 0) {
        renderer_gone();
        return;
    }
    fly.busy = 1;
    fly.cancelled = 0;
    fly.job = copy;
}

/* The sidebar's thumbnails: their width, and each page's picture there. */
static int thumb_width(void)
{
    return side_cols * cw - 3 * cw > 8 ? side_cols * cw - 3 * cw : 8;
}

static void thumb_size(int i, int *w, int *h, double *z)
{
    float rw, rh;
    vl_rotated(v.rotate, doc.pw[i] > 0 ? doc.pw[i] : 1, doc.ph[i] > 0 ? doc.ph[i] : 1, &rw, &rh);
    *z = thumb_width() / (double)rw;
    *w = (int)(rw * *z + 0.5);
    *h = (int)(rh * *z + 0.5);
}

/* A thumbnail's entry: the picture, its number under it, a gap. */
static int thumb_entry(int i)
{
    int w, h;
    double z;
    thumb_size(i, &w, &h, &z);
    return h + ch + ch / 2;
}

static int thumbs_y0(void)
{
    return top + 3 * ch;
}

/* Is page i's thumbnail in sight: 1, and where (in the list's pixels). */
static int thumb_seen(int i, int *y)
{
    int at = 0;
    for (int k = 0; k < i; k++) at += thumb_entry(k);
    *y = at;
    return at + thumb_entry(i) > ui.thumb_top && at < ui.thumb_top + (H - thumbs_y0());
}

static int thumbs_shown(void)
{
    return side_cols > 0 && doc.open && ui.tab == TAB_THUMBNAILS;
}

/* What is wanted, in order: the pages seen, then a row before and after. */
static int prio[64], nprio, nvis;

static void compute_prio(void)
{
    nprio = nvis = 0;
    if (doc.open) nprio = vl_priority(doc.pages, doc.item, columns(), v.sx, v.sy, aw, ah, prio, 64, &nvis);
}

/* The part of item i on the screen, in its pixels: 0 if none. */
static int item_seen(int i, int *x0, int *y0, int *x1, int *y1)
{
    const struct vl_item *it = &doc.item[i];
    *x0 = v.sx - it->x > 0 ? v.sx - it->x : 0;
    *y0 = v.sy - it->y > 0 ? v.sy - it->y : 0;
    *x1 = v.sx + aw - it->x < it->w ? v.sx + aw - it->x : it->w;
    *y1 = v.sy + ah - it->y < it->h ? v.sy + ah - it->y : it->h;
    return *x0 < *x1 && *y0 < *y1;
}

static int whole(int i)
{
    return (uint64_t)doc.item[i].w * (uint64_t)doc.item[i].h <= WHOLE_MAX;
}

static void render_job(struct job *j, int kind, int page, float zoom, int tx, int ty)
{
    *j = new_job(kind, VIEW_RENDER);
    j->page = page;
    j->zoom = zoom;
    j->rotate = v.rotate;
    j->tx = tx;
    j->ty = ty;
    j->req.page = page;
    j->req.zoom = zoom;
    j->req.rotate = v.rotate;
    if (settings.invert && settings.render_mode == CM_PAPER) j->req.paper = VIEW_PAPER | settings.paper;
    if (kind != J_THUMB && kind != J_SLIDE) {
        const struct vl_item *it = &doc.item[page];
        int x = tx >= 0 ? tx * TILE : 0, y = ty >= 0 ? ty * TILE : 0;
        j->req.x = it->ox + x;
        j->req.y = it->oy + y;
        j->req.w = tx >= 0 && it->w - x > TILE ? TILE : it->w - x;
        j->req.h = ty >= 0 && it->h - y > TILE ? TILE : it->h - y;
    }
}

/* A piece of page i not drawn yet: the whole page, or (seen: a large one)
 * the tile nearest the middle of the screen. 1 and *j, or 0. */
static int need_piece(int i, int seen, struct job *j)
{
    const struct page *pg = &doc.page[i];
    if (!pg->known || pg->failed) return 0;
    float z = (float)doc.item[i].zoom;
    if (whole(i)) {
        if (pix_find(0, i, v.rotate, z, -1, -1) >= 0) return 0;
        if (!seen && pix_room(pix_bytes(doc.item[i].w, doc.item[i].h), frame_start, 0) < 0) return 0;
        render_job(j, J_RENDER, i, z, -1, -1);
        return 1;
    }
    int x0, y0, x1, y1, best = -1, bx = 0, by = 0;
    if (!seen || !item_seen(i, &x0, &y0, &x1, &y1)) return 0;
    const struct vl_item *it = &doc.item[i];
    int mx = v.sx + aw / 2 - it->x, my = v.sy + ah / 2 - it->y;
    for (int ty = y0 / TILE; ty <= (y1 - 1) / TILE; ty++) {
        for (int tx = x0 / TILE; tx <= (x1 - 1) / TILE; tx++) {
            if (pix_find(0, i, v.rotate, z, tx, ty) >= 0) continue;
            int d = abs(tx * TILE + TILE / 2 - mx) + abs(ty * TILE + TILE / 2 - my);
            if (best < 0 || d < best) {
                best = d;
                bx = tx;
                by = ty;
            }
        }
    }
    if (best < 0) return 0;
    render_job(j, J_RENDER, i, z, bx, by);
    return 1;
}

static int still_wanted(const struct job *j)
{
    if (j->gen != doc.gen) return 0;
    if (j->kind == J_SLIDE) return pres_wanted(j);
    if (j->kind == J_HITS) return j->tx == find.gen;
    if (j->kind == J_THUMB) {
        int y, w, h;
        double z;
        if (!thumbs_shown() || j->rotate != v.rotate || !thumb_seen(j->page, &y)) return 0;
        thumb_size(j->page, &w, &h, &z);
        return (float)z == j->zoom;
    }
    if (j->kind != J_RENDER) return 1;
    compute_prio();
    int k = 0;
    while (k < nprio && prio[k] != j->page) k++;
    if (k == nprio || j->rotate != v.rotate || (float)doc.item[j->page].zoom != j->zoom) return 0;
    const struct vl_item *it = &doc.item[j->page];
    if (j->req.x != it->ox + (j->tx >= 0 ? j->tx * TILE : 0) ||
        j->req.y != it->oy + (j->ty >= 0 ? j->ty * TILE : 0)) return 0;
    if (j->tx < 0) return j->req.w == it->w && j->req.h == it->h;
    int x0, y0, x1, y1;
    return item_seen(j->page, &x0, &y0, &x1, &y1) && j->tx * TILE < x1 && (j->tx + 1) * TILE > x0 &&
           j->ty * TILE < y1 && (j->ty + 1) * TILE > y0;
}

static void check_stale(void)
{
    if (fly.busy && !fly.cancelled && render_pid > 0 &&
        (fly.job.kind == J_RENDER || fly.job.kind == J_THUMB || fly.job.kind == J_HITS || fly.job.kind == J_SLIDE) &&
        !still_wanted(&fly.job)) {
        kill(render_pid, SIGUSR1);
        fly.cancelled = 1;
    }
}

static struct job page_job(int i)
{
    struct job j = new_job(J_PAGE, VIEW_PAGE);
    j.page = j.req.page = i;
    doc.page[i].asked = 1;
    return j;
}

static struct job hits_job(int i)
{
    struct job j = new_job(J_HITS, VIEW_SEARCH);
    j.page = j.req.page = i;
    j.tx = find.gen;
    snprintf(j.req.text, sizeof j.req.text, "%s", find.active);
    doc.page[i].hits = 1;
    return j;
}

/* The work no one asked for, in its order. */
static int background(struct job *j)
{
    if (pres.on) return pres_background(j);
    compute_prio();
    for (int k = 0; k < nprio; k++) {
        if (!doc.page[prio[k]].known && !doc.page[prio[k]].asked) {
            *j = page_job(prio[k]);
            return 1;
        }
    }
    for (int k = 0; settings.trim_margins && k < nvis; k++) {
        struct page *pg = &doc.page[prio[k]];
        if (pg->known && !pg->bounds && pg->bbox_failed != (float)doc.item[prio[k]].zoom) {
            *j = new_job(J_BBOX, VIEW_BBOX);
            j->page = j->req.page = prio[k];
            /* Okular scans its first full-page display raster: item zoom is
             * pixels per point of the (still uncropped) page as displayed. */
            j->req.zoom = (float)doc.item[prio[k]].zoom;
            pg->bounds = 1;
            return 1;
        }
    }
    for (int k = 0; k < nvis; k++) {
        if (need_piece(prio[k], 1, j)) return 1;
    }
    for (int k = 0; k < nvis; k++) {
        struct page *pg = &doc.page[prio[k]];
        if (pg->known && !pg->links) {
            *j = new_job(J_LINKS, VIEW_LINKS);
            j->page = j->req.page = prio[k];
            pg->links = 1;
            return 1;
        }
    }
    if (pdf_background(j)) return 1;
    for (int k = 0; find.active[0] && k < nvis; k++) {
        if (!doc.page[prio[k]].hits) {
            *j = hits_job(prio[k]);
            return 1;
        }
    }
    for (int k = nvis; k < nprio; k++) {
        if (need_piece(prio[k], 0, j)) return 1;
    }
    for (int i = 0, y = 0, e = 0; thumbs_shown() && i < doc.pages; i++, y += e) {
        e = thumb_entry(i);
        if (y > ui.thumb_top + H) break;
        if (y + e <= ui.thumb_top) continue;
        struct page *pg = &doc.page[i];
        if (!pg->known) {
            if (pg->asked) continue;
            *j = page_job(i);
            return 1;
        }
        int w, h;
        double z;
        thumb_size(i, &w, &h, &z);
        if (pg->failed || pix_find(1, i, v.rotate, (float)z, -1, -1) >= 0 ||
            pix_room(pix_bytes(w, h), frame_start, 0) < 0) {
            continue;
        }
        render_job(j, J_THUMB, i, (float)z, -1, -1);
        return 1;
    }
    for (; doc.size_next < doc.pages; doc.size_next++) {
        if (!doc.page[doc.size_next].known && !doc.page[doc.size_next].asked) {
            *j = page_job(doc.size_next++);
            return 1;
        }
    }
    return 0;
}

/* The selection's next request: a round asks for each of its pages. */
static int sel_job(struct job *j)
{
    if (sel.waiting || !doc.open) return 0;
    if (!sel.round) {
        if (!sel.pending) return 0;
        sel.pending = 0;
        sel.round = 1;
        sel.from = sel.pa < sel.pb ? sel.pa : sel.pb;
        sel.to = sel.pa < sel.pb ? sel.pb : sel.pa;
        sel.next = sel.from;
        for (int k = 0; k < sel.nstaged; k++) {
            free(sel.staged[k].rect);
            free(sel.staged[k].text);
        }
        sel.nstaged = 0;
    }
    int k = sel.next, first = sel.pa <= sel.pb;
    double fx = first ? sel.xa : sel.xb, fy = first ? sel.ya : sel.yb;
    double lx = first ? sel.xb : sel.xa, ly = first ? sel.yb : sel.ya;
    *j = new_job(J_SELECT, VIEW_SELECT);
    j->page = j->req.page = k;
    j->req.mode = sel.mode;
    if (sel.from == sel.to) {
        j->req.ax = (float)sel.xa;
        j->req.ay = (float)sel.ya;
        j->req.bx = (float)sel.xb;
        j->req.by = (float)sel.yb;
    } else {
        j->req.ax = k == sel.from ? (float)fx : 0;
        j->req.ay = k == sel.from ? (float)fy : 0;
        j->req.bx = k == sel.to ? (float)lx : doc.pw[k];
        j->req.by = k == sel.to ? (float)ly : doc.ph[k];
    }
    sel.waiting = 1;
    return 1;
}

/* Looking for the next match: the hits of the page it has reached. */
static int find_job(struct job *j)
{
    if (!find.dir || find.step_page < 0 || find.step_page >= doc.pages) return 0;
    if (doc.page[find.step_page].hits) return 0;
    *j = hits_job(find.step_page);
    return 1;
}

static void schedule(void)
{
    if (sock < 0 || fly.busy) return;
    struct job j;
    if (nqueue > 0) {
        j = queue[0];
        memmove(queue, queue + 1, sizeof queue[0] * (size_t)--nqueue);
        send_job(&j);
        return;
    }
    if (!doc.open || doc.opening) return;
    if (sel_job(&j) || find_job(&j) || background(&j)) send_job(&j);
}

/* The state file: a line a document, the newest first, "page=N y=F mode=N
 * zoom=F rotate=N facing=N em=F" a tab and its path; and the settings'
 * line, with no path. */
static struct saved {
    int found, page, mode, rotate, facing;
    double ny, zoom, em;
} saved;

static int state_path(char *out, size_t size)
{
    struct passwd *pw = getpwuid(VIEW_USER);
    if (!pw || !pw->pw_dir) return -1;
    return snprintf(out, size, "%s/%s", pw->pw_dir, STATE_FILE) < (int)size ? 0 : -1;
}

/* The state file's lines (as the user), or NULL. */
static char *state_read(void)
{
    char path[PATH_MAX];
    if (state_path(path, sizeof path) < 0) return NULL;
    as_user(1);
    FILE *f = fopen(path, "re");
    as_user(0);
    if (!f) return NULL;
    char *data = NULL;
    size_t size = 0;
    FILE *m = open_memstream(&data, &size);
    char buf[4096];
    size_t n;
    while (m && (n = fread(buf, 1, sizeof buf, f)) > 0 && size < (1u << 20)) fwrite(buf, 1, n, m);
    fclose(f);
    if (m) fclose(m);
    return data;
}

static void state_fields(const char *line, struct saved *s, int settings_line)
{
    char copy[512];
    snprintf(copy, sizeof copy, "%.*s", (int)strcspn(line, "\t\n"), line);
    for (char *t = strtok(copy, " "); t; t = strtok(NULL, " ")) {
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq++ = '\0';
        double x = atof(eq);
        if (settings_line) {
            if (!strcmp(t, "sidebar")) settings.sidebar = x != 0;
            else if (!strcmp(t, "invert")) settings.invert = x != 0;
            else if (!strcmp(t, "tool")) settings.tool = clampi((int)x, TOOL_BROWSE, TOOL_MAGNIFIER);
            else if (!strcmp(t, "render_mode")) settings.render_mode = clampi((int)x, 0, CM_COUNT - 1);
            else if (!strcmp(t, "paper")) settings.paper = (uint32_t)strtoul(eq, NULL, 16) & 0xFFFFFF;
            else if (!strcmp(t, "recolor_fg")) settings.recolor_fg = (uint32_t)strtoul(eq, NULL, 16) & 0xFFFFFF;
            else if (!strcmp(t, "recolor_bg")) settings.recolor_bg = (uint32_t)strtoul(eq, NULL, 16) & 0xFFFFFF;
            else if (!strcmp(t, "bw_threshold")) settings.bw_threshold = clampi((int)x, 2, 253);
            else if (!strcmp(t, "bw_contrast")) settings.bw_contrast = clampi((int)x, 2, 6);
            else if (!strcmp(t, "columns")) settings.columns = clampi((int)x, 1, 8);
            else if (!strcmp(t, "rtl")) settings.rtl = x != 0;
            else if (!strcmp(t, "facing")) settings.facing = clampi((int)x, VL_SINGLE, VL_OVERVIEW);
            else if (!strcmp(t, "side_width")) settings.side_width = clampi((int)x, 90, 600);
            else if (!strcmp(t, "trim_margins")) settings.trim_margins = x != 0;
            else if (!strcmp(t, "paged")) settings.paged = x != 0;
            continue;
        }
        if (!strcmp(t, "page")) s->page = (int)x;
        else if (!strcmp(t, "y") && isfinite(x)) s->ny = x;
        else if (!strcmp(t, "mode")) s->mode = clampi((int)x, VL_FIXED, VL_FIT_AUTO);
        else if (!strcmp(t, "zoom") && isfinite(x)) s->zoom = fmin(fmax(x, VL_ZOOM_MIN), VL_ZOOM_MAX);
        else if (!strcmp(t, "rotate")) s->rotate = ((int)x % 360 + 360) % 360 / 90 * 90;
        else if (!strcmp(t, "facing")) s->facing = clampi((int)x, VL_SINGLE, VL_OVERVIEW);
        else if (!strcmp(t, "em") && isfinite(x)) s->em = x;
    }
}

/* The settings' line, and the document path's (in *s), if there are. */
static void state_load(const char *path, struct saved *s)
{
    memset(s, 0, sizeof *s);
    s->zoom = 1;
    s->mode = VL_FIT_WIDTH;
    char *data = state_read();
    for (char *line = data; line && *line;) {
        char *end = strchr(line, '\n'), *tab = strchr(line, '\t');
        if (end) *end = '\0';
        if (tab && tab[1] == '\0') {
            state_fields(line, s, 1);
        } else if (path && strncmp(line, "bookmark=", 9) && strncmp(line, "recent\t", 7) && tab && !s->found &&
                   strcmp(tab + 1, path) == 0) {
            s->found = 1;
            state_fields(line, s, 0);
        }
        line = end ? end + 1 : line + strlen(line);
    }
    free(data);
}

/* Write the settings and this document's place in front of the others. */
static void state_save(void)
{
    char path[PATH_MAX], tmp[PATH_MAX + 8], dir[PATH_MAX];
    ui.changed = 0;
    if (state_path(path, sizeof path) < 0) return;
    char *data = state_read();
    snprintf(tmp, sizeof tmp, "%s.new", path);
    size_t home = strlen(path) - strlen(STATE_FILE);
    as_user(1);
    /* ~/.local, ~/.local/state and ~/.local/state/locachy, the user's own. */
    for (const char *s = strchr(STATE_FILE, '/'); s; s = strchr(s + 1, '/')) {
        snprintf(dir, sizeof dir, "%.*s", (int)(home + (size_t)(s - STATE_FILE)), path);
        mkdir(dir, 0700);
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600); /* what was read: the user's */
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f && fd >= 0) close(fd);
    if (f) {
        fprintf(f, "sidebar=%d invert=%d tool=%d facing=%d side_width=%d paged=%d trim_margins=%d render_mode=%d "
                "paper=%06x recolor_fg=%06x recolor_bg=%06x bw_threshold=%d bw_contrast=%d columns=%d rtl=%d\t\n",
                settings.sidebar, settings.invert, settings.tool, settings.facing,
                settings.side_width, settings.paged, settings.trim_margins, settings.render_mode, settings.paper,
                settings.recolor_fg, settings.recolor_bg, settings.bw_threshold, settings.bw_contrast,
                settings.columns, settings.rtl);
        int lines = 1;
        for (int k = 0; k < recent.n; k++, lines++) fprintf(f, "recent\t%s\n", recent.path[k]);
        if (doc.open && !strchr(doc.path, '\n')) {
            struct pos p = pos_now();
            fprintf(f, "page=%d y=%.5f mode=%d zoom=%.4f rotate=%d facing=%d em=%.1f\t%s\n", p.page, p.ny, v.mode,
                    v.zoom, v.rotate, v.facing, doc.em, doc.path);
            lines++;
            for (int i = 0; i < doc.pages + doc.nbpend; i++) {
                const char *title = i < doc.pages ? doc.bookmark[i] : doc.bpend[i - doc.pages].title;
                const char *mark = i < doc.pages ? doc.bmark[i] : doc.bpend[i - doc.pages].mark;
                if (!title) continue;
                fprintf(f, "bookmark=%d title=", i < doc.pages ? i : doc.bpend[i - doc.pages].page);
                for (const unsigned char *c = (const unsigned char *)title; *c; c++) fprintf(f, "%02x", *c);
                if (mark) fprintf(f, " mark=%s", mark);
                fprintf(f, "\t%s\n", doc.path);
            }
        }
        for (char *line = data; line && *line && lines < STATE_LINES;) {
            char *end = strchr(line, '\n'), *tab = strchr(line, '\t');
            size_t len = end ? (size_t)(end - line) : strlen(line);
            int skip = !tab || tab >= line + len || tab + 1 == line + len || !strncmp(line, "recent\t", 7) ||
                       (doc.open && len - (size_t)(tab + 1 - line) == strlen(doc.path) &&
                        strncmp(tab + 1, doc.path, strlen(doc.path)) == 0);
            if (!skip) {
                fprintf(f, "%.*s\n", (int)len, line);
                lines++;
            }
            line = end ? end + 1 : line + len;
        }
        int bad = ferror(f);
        if (fclose(f) != 0 || bad || rename(tmp, path) < 0) {
            unlink(tmp);
            say("view: cannot write %s", path);
        }
    } else {
        say("view: cannot write %s: %s", tmp, strerror(errno));
    }
    as_user(0);
    free(data);
}

/* The recent documents (Shell's m_recent): added when one opens, taken off
 * when it cannot be opened, kept as "recent" lines of the state file. */
static void recent_clear(void)
{
    for (int k = 0; k < recent.n; k++) free(recent.path[k]);
    recent.n = 0;
}

static void recent_remove(const char *path)
{
    for (int k = 0; k < recent.n; k++) if (!strcmp(recent.path[k], path)) {
        free(recent.path[k]);
        memmove(recent.path + k, recent.path + k + 1, sizeof recent.path[0] * (size_t)(recent.n - k - 1));
        recent.n--;
        return;
    }
}

static void recent_add(const char *path)
{
    if (strchr(path, '\n')) return;
    recent_remove(path);
    char *p = strdup(path);
    if (!p) return;
    if (recent.n == RECENT_MAX) free(recent.path[--recent.n]);
    memmove(recent.path + 1, recent.path, sizeof recent.path[0] * (size_t)recent.n);
    recent.path[0] = p;
    recent.n++;
}

static void recent_load(void)
{
    recent_clear();
    char *data = state_read();
    for (char *line = data; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        if (!strncmp(line, "recent\t", 7) && line[7] && recent.n < RECENT_MAX) recent.path[recent.n++] = strdup(line + 7);
        line = end ? end + 1 : line + strlen(line);
    }
    while (recent.n && !recent.path[recent.n - 1]) recent.n--;
    free(data);
}

/* Bookmark records share the atomic state file but are not document-view
 * records. Titles are hex UTF-8 so tabs/newlines in titles cannot alter it. */
static void bpend_add(char *title, char *mark, int page)
{
    struct bpend *b = realloc(doc.bpend, sizeof *b * (size_t)(doc.nbpend + 1));
    if (!b) {
        free(title);
        free(mark);
        return;
    }
    doc.bpend = b;
    doc.bpend[doc.nbpend++] = (struct bpend){ title, mark, page };
}

static void bpend_free(void)
{
    for (int k = 0; k < doc.nbpend; k++) {
        free(doc.bpend[k].title);
        free(doc.bpend[k].mark);
    }
    free(doc.bpend);
    doc.bpend = NULL;
    doc.nbpend = 0;
}

/* The bookmarks waiting to be placed: a reflowed document's where their
 * MuPDF bookmark is in this layout (asked for), the others on their page. */
static void bookmarks_place(void)
{
    int reflowed = (doc.flags & VIEW_REFLOWABLE) != 0;
    for (int k = 0; k < doc.nbpend; k++) {
        struct bpend *b = &doc.bpend[k];
        if (!b->title) continue;
        if (reflowed && b->mark) {
            struct job j = new_job(J_FIND_MARK, VIEW_FIND_MARK);
            snprintf(j.req.text, sizeof j.req.text, "%s", b->mark);
            j.tx = k;
            push_job(&j, 0);
        } else if (b->page >= 0 && b->page < doc.pages && !doc.bookmark[b->page]) {
            doc.bookmark[b->page] = b->title;
            doc.bmark[b->page] = b->mark;
            b->title = b->mark = NULL;
        }
    }
}

static void got_mark(const struct job *j, const struct view_reply *rep)
{
    if (j->gen != doc.gen) return;
    if (j->kind == J_MARK) {
        int p = j->page;
        if (rep->status == 0 && p >= 0 && p < doc.pages && doc.bookmark[p] && !doc.bmark[p]) {
            doc.bmark[p] = strdup(rep->text);
            changed();
        }
        return;
    }
    if (j->tx < 0 || j->tx >= doc.nbpend) return;
    struct bpend *b = &doc.bpend[j->tx];
    int p = rep->status == 0 ? rep->page : b->page;
    if (b->title && p >= 0 && p < doc.pages && !doc.bookmark[p]) {
        doc.bookmark[p] = b->title;
        doc.bmark[p] = b->mark;
        b->title = b->mark = NULL;
        ui.dirty = 1;
    }
}

/* A page's bookmark of a reflowed document gets its place in the flow. */
static void bookmark_mark(int page)
{
    if (!(doc.flags & VIEW_REFLOWABLE)) return;
    struct job j = new_job(J_MARK, VIEW_MARK);
    j.page = j.req.page = page;
    push_job(&j, 0);
}

/* Before a new layout: the bookmarks to be placed again. */
static void bookmarks_lift(void)
{
    for (int i = 0; doc.bookmark && i < doc.pages; i++) {
        if (doc.bookmark[i]) bpend_add(doc.bookmark[i], doc.bmark[i], i);
        doc.bookmark[i] = doc.bmark[i] = NULL;
    }
}

/* Bookmark records share the atomic state file but are not document-view
 * records. Titles are hex UTF-8 so tabs/newlines in titles cannot alter it;
 * a reflowed document's carry their MuPDF bookmark (mark=). */
static void bookmarks_load(void)
{
    char *data = state_read();
    bpend_free();
    for (char *line = data; line && *line;) {
        char *end = strchr(line, '\n'), *tab = strchr(line, '\t');
        if (end) *end = 0;
        if (tab && !strncmp(line, "bookmark=", 9) && !strcmp(tab + 1, doc.path)) {
            int p = atoi(line + 9);
            char *hex = strstr(line, " title="), *mark = strstr(line, " mark=");
            if (hex && hex < tab) {
                hex += 7;
                char *stop = mark && mark < tab && mark > hex ? mark : tab;
                size_t len = (size_t)(stop - hex) / 2;
                char *title = calloc(len + 1, 1), *m = NULL;
                if (mark && mark < tab) m = strndup(mark + 6, (size_t)(tab - mark - 6));
                if (title) {
                    size_t k = 0;
                    for (; k < len; k++) { unsigned c; if (sscanf(hex + 2 * k, "%2x", &c) != 1 || !c) break; title[k] = (char)c; }
                    title[k] = 0;
                    if (p >= 0 && p < doc.pages && doc.bookmark[p]) { free(doc.bookmark[p]); free(doc.bmark[p]); doc.bookmark[p] = doc.bmark[p] = NULL; }
                    bpend_add(title, m, p);
                } else {
                    free(m);
                }
            }
        }
        line = end ? end + 1 : line + strlen(line);
    }
    free(data);
    bookmarks_place();
}

/* The document's pages, all of the size not known yet (an estimate). */
static void free_pages(void)
{
    for (int i = 0; doc.bookmark && i < doc.pages; i++) free(doc.bookmark[i]);
    for (int i = 0; doc.bmark && i < doc.pages; i++) free(doc.bmark[i]);
    free(doc.bookmark);
    free(doc.bmark);
    doc.bookmark = doc.bmark = NULL;
    for (int i = 0; doc.page && i < doc.pages; i++) {
        free(doc.page[i].link);
        free(doc.page[i].hit);
        pdf_free_page(&doc.page[i]);
    }
    free(doc.page);
    free(doc.pw);
    free(doc.ph);
    free(doc.item);
    free(doc.crop);
    doc.crop = NULL;
    doc.page = NULL;
    doc.pw = doc.ph = NULL;
    doc.item = NULL;
    doc.pages = 0;
}

static int alloc_pages(int pages)
{
    free_pages();
    if (pages < 1) return -1;
    doc.page = calloc((size_t)pages, sizeof *doc.page);
    doc.pw = calloc((size_t)pages, sizeof *doc.pw);
    doc.ph = calloc((size_t)pages, sizeof *doc.ph);
    doc.item = calloc((size_t)pages, sizeof *doc.item);
    doc.crop = calloc((size_t)pages, sizeof *doc.crop);
    doc.bookmark = calloc((size_t)pages, sizeof *doc.bookmark);
    doc.bmark = calloc((size_t)pages, sizeof *doc.bmark);
    if (!doc.page || !doc.pw || !doc.ph || !doc.item || !doc.crop || !doc.bookmark || !doc.bmark) {
        free_pages();
        return -1;
    }
    doc.pages = pages;
    doc.size_next = 0;
    int reflowed = doc.flags & VIEW_REFLOWABLE;
    for (int i = 0; i < pages; i++) {
        doc.pw[i] = reflowed ? doc.lw : 612; /* US Letter until the first page's size comes */
        doc.ph[i] = reflowed ? doc.lh : 792;
    }
    return 0;
}

static void sel_free(struct sel_part *p, int n)
{
    for (int k = 0; k < n; k++) {
        free(p[k].rect);
        free(p[k].text);
    }
}

static void sel_clear(void)
{
    sel_free(sel.part, sel.npart);
    sel_free(sel.staged, sel.nstaged);
    free(sel.part);
    free(sel.staged);
    int waiting = sel.waiting;
    memset(&sel, 0, sizeof sel);
    sel.waiting = waiting; /* its answer is still to come */
    ui.dirty = 1;
}

static void find_reset(void)
{
    find.gen++;
    find.active[0] = '\0';
    find.cur_page = find.cur_hit = -1;
    find.dir = find.failed = 0;
    for (int i = 0; i < doc.pages; i++) {
        free(doc.page[i].hit);
        doc.page[i].hit = NULL;
        doc.page[i].nhit = 0;
        doc.page[i].hits = 0;
    }
    check_stale();
    ui.dirty = 1;
}

static void list_close(void)
{
    for (int i = 0; i < list.n; i++) {
        free(list.line[i]);
        if (list.name) free(list.name[i]);
    }
    free(list.line);
    free(list.index);
    free(list.name);
    memset(&list, 0, sizeof list);
    if (ui.focus == FOCUS_LIST) ui.focus = FOCUS_DOC;
    ui.dirty = 1;
}

static void close_document(void)
{
    pdf_forget();
    free_pages();
    bpend_free();
    free(doc.outline);
    doc.outline = NULL;
    doc.noutline = 0;
    doc.open = doc.opening = 0;
    doc.gen++;
    sel_clear();
    sel.waiting = 0;
    font.busy = 0;
    find_reset();
    list_close();
    pix_flush();
    hist.n = hist.i = 0;
    v.sx = v.sy = v.current = 0;
    tools_reset();
    ui.toc_sel = ui.toc_top = ui.thumb_sel = ui.thumb_top = 0;
    if (ui.focus != FOCUS_FIND) ui.focus = FOCUS_DOC;
}

static struct job open_job(int again)
{
    struct job j = new_job(J_OPEN, VIEW_OPEN);
    j.tx = again;
    snprintf(j.req.text, sizeof j.req.text, "%s", doc.path);
    j.req.ax = doc.lw;
    j.req.ay = doc.lh;
    j.req.bx = doc.em;
    return j;
}

static void open_document(const char *path)
{
    char real[PATH_MAX];
    if (doc.open) state_save();
    close_document();
    nqueue = 0;
    if (!tabs.n) { tabs.path[0] = strdup(path); tabs.n = tabs.path[0] ? 1 : 0; tabs.current = 0; }
    geometry();
    memset(&trim, 0, sizeof trim);
    as_user(1);
    int ok = realpath(path, real) != NULL;
    as_user(0);
    if (!ok) {
        if (path[0] == '/') {
            snprintf(real, sizeof real, "%s", path);
        } else {
            char cwd[PATH_MAX / 2];
            snprintf(real, sizeof real, "%s/%s", getcwd(cwd, sizeof cwd) ? cwd : "", path);
        }
    }
    snprintf(doc.path, sizeof doc.path, "%s", real);
    doc.format[0] = doc.title[0] = '\0';
    doc.with_password = 0;
    memset(doc.password, 0, sizeof doc.password);
    state_load(doc.path, &saved);
    doc.em = saved.found && saved.em >= EM_MIN && saved.em <= EM_MAX ? (float)saved.em : EM_DEFAULT;
    doc.lw = (float)((aw - col_margin) / z100);
    doc.lh = (float)((ah - row_margin) / z100);
    struct job j = open_job(0);
    if (sock < 0) start_renderer();
    if (sock >= 0) {
        doc.opening = 1;
        push_job(&j, 1);
    }
    ui.dirty = 1;
}

/* view-render again, the same document: where it was. */
static void reopen(void)
{
    doc.opening = 1;
    struct job j = open_job(1);
    push_job(&j, 1); /* a password given is given again (opened) */
}

static void prompt_open(int kind, const char *question, const char *initial, int masked);

static void opened(const struct job *j, const struct view_reply *rep)
{
    doc.opening = 0;
    if (rep->status == EACCES && (rep->flags & VIEW_NEEDS_PASSWORD)) {
        if (j->kind == J_OPEN && doc.with_password) {
            struct job a = new_job(J_AUTH, VIEW_AUTH);
            a.tx = j->tx;
            snprintf(a.req.text, sizeof a.req.text, "%s", doc.password);
            doc.opening = 1;
            push_job(&a, 1);
            return;
        }
        char q[600];
        /* TRANSLATORS: %.300s is the file name of the document. */
        snprintf(q, sizeof q, j->kind == J_AUTH ? _("Incorrect password. Try again: %.300s") :
                 _("Please enter the password to read the document: %.300s"), base_name(doc.path));
        doc.with_password = 0;
        doc.open = 0;
        prompt_open(P_PASSWORD, q, "", 1);
        return;
    }
    if (rep->status != 0 || rep->pages < 1) {
        /* TRANSLATORS: the first %.200s is the file name; the second is the reason from the renderer. */
        toast(1, _("Could not open %.200s. Reason: %.200s"), base_name(doc.path),
              rep->status ? rep->text : _("it has no pages"));
        doc.open = 0;
        recent_remove(doc.path);
        return;
    }
    if (j->tx && doc.page && rep->pages == doc.pages) {
        /* Started again: what was asked and not answered is asked again. */
        for (int i = 0; i < doc.pages; i++) {
            struct page *pg = &doc.page[i];
            if (!pg->known) pg->asked = 0;
            if (pg->links == 1) pg->links = 0;
            if (pg->hits == 1) pg->hits = 0;
            if (pg->bounds == 1) pg->bounds = 0;
        }
        doc.size_next = 0;
        doc.open = 1;
        ui.dirty = 1;
        pdf_opened(1);
        return;
    }
    struct pos keep = pos_now();
    int again = j->tx && doc.open;
    const char *tab = strchr(rep->text, '\t');
    snprintf(doc.format, sizeof doc.format, "%.*s", tab ? (int)(tab - rep->text) : (int)strlen(rep->text), rep->text);
    snprintf(doc.title, sizeof doc.title, "%s", tab ? tab + 1 : "");
    doc.flags = rep->flags;
    if (alloc_pages(rep->pages) < 0) {
        /* TRANSLATORS: %.200s is the file name; %d is the page count. */
        toast(1, P_("Could not open %.200s. Reason: no memory for %d page",
                    "Could not open %.200s. Reason: no memory for %d pages", (unsigned long)rep->pages),
              base_name(doc.path), rep->pages);
        doc.open = 0;
        return;
    }
    doc.gen++;
    doc.open = 1;
    bookmarks_load();
    if (!again) recent_add(doc.path);
    say("view: opened %s: %s, %d pages", doc.path, doc.format, rep->pages);
    if (!again) {
        v.mode = saved.found ? saved.mode : VL_FIT_WIDTH;
        v.zoom = saved.found ? saved.zoom : 1;
        v.rotate = saved.found ? saved.rotate : 0;
        v.facing = saved.found ? saved.facing : settings.facing;
        ui.tab = doc.flags & VIEW_HAS_OUTLINE ? TAB_CONTENTS : TAB_THUMBNAILS;
    }
    if (doc.flags & VIEW_HAS_OUTLINE) {
        struct job o = new_job(J_OUTLINE, VIEW_OUTLINE);
        push_job(&o, 0);
    }
    geometry();
    layout();
    if (again) {
        pos_go(keep);
    } else if (saved.found) {
        struct pos p = pos_page(saved.page);
        p.ny = saved.ny;
        pos_go(p);
    } else {
        v.sx = v.sy = 0;
    }
    hist.n = 1;
    hist.i = 0;
    hist.at[0] = pos_now();
    if (!again) keep_restore();
    ui.follow_thumbs = 1;
    ui.changed = 0;
    ui.dirty = 1;
    pdf_opened(again);
}

/* Text after a reply's count rects. */
static const char *reply_text(const struct view_reply *rep, const void *data, size_t size, size_t *len)
{
    size_t rects = (size_t)rep->count * sizeof(struct view_rect);
    *len = 0;
    if (!data || rep->count < 0 || rep->w < 0 || rects > size || (size_t)rep->w > size - rects) return NULL;
    *len = (size_t)rep->w;
    return (const char *)data + rects;
}

static int reply_count(const struct view_reply *rep, size_t item, size_t size, const void *data)
{
    if (rep->count <= 0 || !data) return 0;
    size_t n = size / item;
    return (size_t)rep->count < n ? rep->count : (int)n;
}

/* The selection's text: its pages', one line apart. */
static char *sel_text(size_t *len)
{
    size_t total = 1;
    for (int k = 0; k < sel.npart; k++) total += sel.part[k].len + 1;
    char *t = malloc(total);
    *len = 0;
    if (!t) return NULL;
    for (int k = 0; k < sel.npart; k++) {
        if (*len > 0 && t[*len - 1] != '\n' && sel.part[k].len) t[(*len)++] = '\n';
        memcpy(t + *len, sel.part[k].text ? sel.part[k].text : "", sel.part[k].len);
        *len += sel.part[k].len;
    }
    t[*len] = '\0';
    return t;
}

static void copy_selection(int primary)
{
    size_t len;
    char *t = sel_text(&len);
    if (t && len) {
        int status = clipboard_set(primary, t, len);
        if (status != CLIPBOARD_OK) toast(1, "%s", clipboard_error(status));
    }
    free(t);
}

static int hit_count(const struct page *pg)
{
    int n = 0;
    for (int k = 0; k < pg->nhit; k++) {
        if (pg->hit[k].hit + 1 > n) n = pg->hit[k].hit + 1;
    }
    return n;
}

/* The current match on the screen: Okular centres it when it is not all
 * in sight with a margin (5% of the page) around it. */
static void show_hit(void)
{
    const struct page *pg = &doc.page[find.cur_page];
    int k = 0;
    while (k < pg->nhit && pg->hit[k].hit != find.cur_hit) k++;
    if (k == pg->nhit) return;
    const struct view_rect *r = &pg->hit[k];
    const struct vl_item *it = &doc.item[find.cur_page];
    double x0, y0, x1, y1, mx = 0.05 * it->w, my = 0.05 * it->h;
    float pw = doc.pw[find.cur_page], ph = doc.ph[find.cur_page];
    vl_point_to_item(v.rotate, pw, ph, it->zoom, r->x0, r->y0, &x0, &y0);
    vl_point_to_item(v.rotate, pw, ph, it->zoom, r->x1, r->y1, &x1, &y1);
    double lx = fmin(x0, x1), rx = fmax(x0, x1), ty = fmin(y0, y1), by = fmax(y0, y1);
    if (it->x + lx - mx >= v.sx && it->x + rx + mx <= v.sx + aw && it->y + ty - my >= v.sy &&
        it->y + by + my <= v.sy + ah) {
        ui.dirty = 1;
        return;
    }
    struct pos p = { find.cur_page, ((lx + rx) / 2 - aw / 2.0) / it->w, ((ty + by) / 2 - ah / 2.0) / it->h };
    go(p);
}

static void find_continue(void)
{
    while (find.dir && doc.open) {
        struct page *pg = &doc.page[find.step_page];
        if (pg->hits != 2) return; /* asked for: on its answer, again */
        int n = hit_count(pg), k = find.dir > 0 ? find.after + 1 : find.after < 0 ? n - 1 : find.after - 1;
        if (k >= 0 && k < n) {
            find.cur_page = find.step_page;
            find.cur_hit = k;
            find.dir = 0;
            show_hit();
            ui.dirty = 1;
            return;
        }
        if (++find.done > doc.pages) {
            find.dir = 0;
            find.failed = 1;
            ui.dirty = 1;
            return;
        }
        find.step_page += find.dir;
        find.after = -1;
        if (find.step_page >= doc.pages) {
            find.step_page = 0;
            toast(0, _("Continuing search from beginning"));
        } else if (find.step_page < 0) {
            find.step_page = doc.pages - 1;
            toast(0, _("Continuing search from bottom"));
        }
    }
}

/* F3, Shift+F3, Enter in the find bar: the next match from the current
 * one, or (a new text) from the current page (Okular's "From current page"). */
static void find_go(int dir)
{
    if (!doc.open || !find.in.text[0]) return;
    find.typed = 0;
    if (strcmp(find.active, find.in.text) != 0) {
        find_reset();
        snprintf(find.active, sizeof find.active, "%.255s", find.in.text);
        find.step_page = current_page();
        find.after = -1;
    } else if (find.cur_page >= 0) {
        find.step_page = find.cur_page;
        find.after = find.cur_hit;
    } else {
        find.step_page = current_page();
        find.after = -1;
    }
    find.dir = dir;
    find.done = 0;
    find.failed = 0;
    find_continue();
}

/* A reflowed document's text size: its text at the top before (J_TOPTEXT),
 * laid out again (J_LAYOUT), and that text found again (J_REFIND). */
static void font_size(double em)
{
    if (!doc.open || !(doc.flags & VIEW_REFLOWABLE)) {
        toast(0, _("Only a reflowed document's text size can change."));
        return;
    }
    em = em < EM_MIN ? EM_MIN : em > EM_MAX ? EM_MAX : em;
    if (em == doc.em && fabs((aw / columns() - col_margin) / z100 - doc.lw) < .5 &&
        fabs((ah - row_margin) / z100 - doc.lh) < .5) return;
    if (font.busy) return;
    font.busy = 1;
    font.lw = doc.lw;
    font.lh = doc.lh;
    font.em = em;
    font.needle[0] = '\0';
    double px, py;
    struct vl_params p = params();
    int page = vl_screen_to_page(&p, doc.pages, doc.item, doc.pw, doc.ph, ax, ay, v.sx, v.sy, ax, ay, 1, &px, &py);
    font.page = page < 0 ? 0 : page;
    struct job j = new_job(J_TOPTEXT, VIEW_SELECT);
    j.page = j.req.page = font.page;
    j.req.mode = VIEW_SELECT_CHARS;
    j.req.ax = 0;
    j.req.ay = (float)py;
    j.req.bx = doc.pw[font.page];
    j.req.by = doc.ph[font.page];
    push_job(&j, 0);
}

static void font_layout(void)
{
    struct job j = new_job(J_LAYOUT, VIEW_LAYOUT);
    j.page = j.req.page = font.page;
    j.req.ax = font.lw;
    j.req.ay = font.lh;
    j.req.bx = (float)font.em;
    push_job(&j, 0);
}

/* Reflow to the actual document viewport, in points, not a scaled picture
 * of the old layout. Serialise requests; a later resize is retried on reply. */
static void viewport_reflow(void)
{
    if (!doc.open || !(doc.flags & VIEW_REFLOWABLE) || font.busy) return;
    float w = (float)fmax(1, (aw / columns() - col_margin) / z100);
    float h = (float)fmax(1, (ah - row_margin) / z100);
    if (fabs(w - doc.lw) < .5 && fabs(h - doc.lh) < .5) return;
    font_size(doc.em); /* capture the text at the old top */
    font.lw = w;
    font.lh = h;
}

static void refind(int page)
{
    struct job j = new_job(J_REFIND, VIEW_SEARCH);
    j.page = j.req.page = page;
    snprintf(j.req.text, sizeof j.req.text, "%s", font.needle);
    push_job(&j, 0);
}

static void list_add(const char *line, int index, const char *name)
{
    if (list.n >= 10000) return;
    char **l = realloc(list.line, sizeof *l * (size_t)(list.n + 1));
    if (l) list.line = l;
    int *x = realloc(list.index, sizeof *x * (size_t)(list.n + 1));
    if (x) list.index = x;
    char **nm = realloc(list.name, sizeof *nm * (size_t)(list.n + 1));
    if (nm) list.name = nm;
    if (!l || !x || !nm) return;
    list.line[list.n] = strdup(line);
    list.index[list.n] = index;
    list.name[list.n] = name ? strdup(name) : NULL;
    if (list.line[list.n]) list.n++;
}

/* Properties ("KEY\tVALUE" lines, the fonts last) or the embedded files
 * ("INDEX\tNAME\tBYTES"), as Okular's dialogs list them. */
static const char *info_key_label(const char *key)
{
    static const struct { const char *key, *label; } keys[] = {
        { "Format", N_("Format") }, { "Title", N_("Title") }, { "Author", N_("Author") },
        { "Subject", N_("Subject") }, { "Keywords", N_("Keywords") }, { "Creator", N_("Creator") },
        { "Producer", N_("Producer") }, { "Created", N_("Created") }, { "Modified", N_("Modified") },
        { "Encryption", N_("Encryption") }, { "Version", N_("Version") }, { "Pages", N_("Pages") },
        { "Page size", N_("Page size") }, { "Resolution", N_("Resolution") },
        { "Document type", N_("Document type") }, { "Language level", N_("Language level") },
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (!strcmp(key, keys[i].key)) return _(keys[i].label);
    }
    return key;
}

static void list_open(int kind, const char *data, size_t size)
{
    list_close();
    for (int pass = 0; pass < (kind == L_INFO ? 2 : 1); pass++) {
        int fonts = 0;
        for (size_t at = 0; data && at < size;) {
            const char *end = memchr(data + at, '\n', size - at);
            size_t len = end ? (size_t)(end - (data + at)) : size - at;
            char line[1024], shown[1200];
            snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), data + at);
            at += len + 1;
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab++ = '\0';
            if (kind == L_EMBEDDED) {
                char *bytes = strchr(tab, '\t');
                if (bytes) *bytes++ = '\0';
                /* TRANSLATORS: the first %s is the embedded file's name; the second is its size in bytes. */
                snprintf(shown, sizeof shown, _("%s   (%s bytes)"), tab, bytes ? bytes : "?");
                list_add(shown, atoi(line), tab);
            } else if ((strcmp(line, "Font") == 0) == pass) {
                if (pass && !fonts++) {
                    list_add("", -1, NULL);
                    list_add(_("Fonts"), -1, NULL);
                }
                if (pass) snprintf(shown, sizeof shown, "  %s", tab);
                else snprintf(shown, sizeof shown, "%s: %s", info_key_label(line), tab);
                list_add(shown, -1, NULL);
            }
        }
    }
    list.kind = kind;
    ui.focus = FOCUS_LIST;
    ui.dirty = 1;
}

/* The text (pages apart by \f) as Okular exports it: one after the other. */
static void write_export(const char *data, size_t size)
{
    as_user(1);
    int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    int ok = fd >= 0;
    char buf[65536];
    size_t n = 0;
    for (size_t i = 0; ok && i < size; i++) {
        if (data[i] == '\f') {
            if (i == 0 || data[i - 1] == '\n') continue;
            buf[n++] = '\n';
        } else {
            buf[n++] = data[i];
        }
        if (n == sizeof buf) {
            ok = write(fd, buf, n) == (ssize_t)n;
            n = 0;
        }
    }
    if (ok && n) ok = write(fd, buf, n) == (ssize_t)n;
    if (fd >= 0 && close(fd) < 0) ok = 0;
    as_user(0);
    /* TRANSLATORS: %.200s is the file name. */
    if (ok) toast(0, _("Exported to %.200s."), base_name(target));
    /* TRANSLATORS: %.200s is the file path; %s is the system error message. */
    else toast(1, _("Cannot write %.200s: %s."), target, strerror(errno));
}

static void page_size(const struct job *j, const struct view_reply *rep)
{
    struct page *pg = &doc.page[j->page];
    struct anchor a = anchor_at(ax, ay);
    int first = 1;
    for (int i = 0; i < doc.pages && first; i++) first = !doc.page[i].known;
    pg->known = 1;
    if (rep->status != 0 || !(rep->pw > 0) || !(rep->ph > 0) || !isfinite(rep->pw) || !isfinite(rep->ph)) {
        pg->failed = 1;
        ui.dirty = 1;
        return;
    }
    pg->text = (rep->flags & VIEW_HAS_TEXT) != 0;
    for (int i = first ? 0 : j->page; i < (first ? doc.pages : j->page + 1); i++) {
        if (i == j->page || !doc.page[i].known) {
            doc.pw[i] = rep->pw; /* the others' estimate: this one's size */
            doc.ph[i] = rep->ph;
        }
    }
    layout();
    anchor_keep(&a);
    compute_prio();
    for (int k = 0; k < nprio; k++) {
        if (prio[k] == j->page) ui.dirty = 1;
    }
    if (first || thumbs_shown()) ui.dirty = 1;
}

static void got_select(const struct job *j, const struct view_reply *rep, const void *data, size_t size)
{
    sel.waiting = 0;
    if (j->gen != doc.gen || !sel.round) return;
    size_t len;
    const char *text = rep->status == 0 ? reply_text(rep, data, size, &len) : NULL;
    struct sel_part *p = realloc(sel.staged, sizeof *p * (size_t)(sel.nstaged + 1));
    if (p) {
        sel.staged = p;
        struct sel_part *part = &p[sel.nstaged++];
        memset(part, 0, sizeof *part);
        part->page = j->page;
        if (text) {
            part->nrect = rep->count;
            part->rect = malloc(sizeof *part->rect * (size_t)(rep->count ? rep->count : 1));
            part->text = malloc(len + 1);
            if (part->rect && part->text) {
                memcpy(part->rect, data, sizeof *part->rect * (size_t)rep->count);
                memcpy(part->text, text, len);
                part->text[len] = '\0';
                part->len = len;
            } else {
                part->nrect = 0;
            }
        }
    }
    if (++sel.next <= sel.to) return;
    sel_free(sel.part, sel.npart);
    free(sel.part);
    sel.part = sel.staged;
    sel.npart = sel.nstaged;
    sel.staged = NULL;
    sel.nstaged = 0;
    sel.round = 0;
    sel.active = 1;
    ui.dirty = 1;
    if (!sel.pending && sel.final) {
        sel.final = 0;
        copy_selection(1);
    }
}

/* A reflowed document laid out again: all its pages are new. */
static void got_layout(const struct view_reply *rep)
{
    font.busy = 0;
    if (rep->status != 0 || rep->pages < 1) {
        /* TRANSLATORS: %.200s is the reason from the renderer. */
        toast(1, _("Cannot lay the document out again: %.200s"), rep->text);
        return;
    }
    doc.em = (float)font.em;
    doc.lw = font.lw;
    doc.lh = font.lh;
    free(doc.outline);
    doc.outline = NULL;
    doc.noutline = 0;
    sel_clear();
    int resume_find = find.bar && find.in.text[0] && (find.active[0] || find.typed);
    char text[sizeof find.in.text];
    snprintf(text, sizeof text, "%s", find.in.text);
    find_reset();
    snprintf(find.in.text, sizeof find.in.text, "%s", text);
    if (resume_find) find.typed = now();
    pix_flush();
    bookmarks_lift(); /* placed again by their marks */
    if (alloc_pages(rep->pages) < 0) {
        toast(1, P_("No memory for %d page.", "No memory for %d pages.", (unsigned long)rep->pages), rep->pages);
        close_document();
        return;
    }
    doc.gen++;
    if (doc.flags & VIEW_HAS_OUTLINE) {
        struct job o = new_job(J_OUTLINE, VIEW_OUTLINE);
        push_job(&o, 0);
    }
    bookmarks_place();
    layout();
    pos_go(pos_page(clampi(rep->page, 0, doc.pages - 1)));
    hist.n = 1;
    hist.i = 0;
    hist.at[0] = pos_now();
    font.page = clampi(rep->page, 0, doc.pages - 1);
    font.tries = 0;
    if (font.needle[0]) refind(font.page);
    viewport_reflow();
    changed();
}

/* The first words of what was at the top before. */
static void got_top_text(const struct view_reply *rep, const void *data, size_t size)
{
    size_t len;
    const char *t = rep->status == 0 ? reply_text(rep, data, size, &len) : NULL;
    size_t n = 0;
    int words = 0, space = 1;
    for (size_t i = 0; t && i < len && n + 1 < sizeof font.needle && words < 6; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c <= ' ') {
            if (!space && n) {
                words++;
                if (words < 6) font.needle[n++] = ' ';
            }
            space = 1;
        } else {
            font.needle[n++] = (char)c;
            space = 0;
        }
    }
    while (n > 0 && font.needle[n - 1] == ' ') n--;
    /* Not cut in the middle of a UTF-8 character. */
    while (n > 0 && ((unsigned char)font.needle[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0 && (unsigned char)font.needle[n - 1] >= 0xC0) n--;
    font.needle[n] = '\0';
    font_layout();
}

static void got_refind(const struct job *j, const struct view_reply *rep, const void *data, size_t size)
{
    int n = rep->status == 0 ? reply_count(rep, sizeof(struct view_rect), size, data) : 0;
    if (n > 0) {
        const struct view_rect *r = data;
        struct pos p = pos_point(j->page, 0, fmin(r->y0, r->y1));
        p.nx = pos_now().nx;
        pos_go(p);
        hist.at[hist.i] = pos_now();
        changed();
    } else if (++font.tries < 3 && j->page + 1 < doc.pages) {
        refind(j->page + 1);
    }
}

/* An answer: 1 if it keeps the mapping (a picture). */
static int reply(const struct job *j, const struct view_reply *rep, void *data, size_t size)
{
    int mine = j->gen == doc.gen && doc.pages > 0 && j->page < doc.pages;
    switch (j->kind) {
    case J_OPEN:
    case J_AUTH:
        opened(j, rep);
        break;
    case J_PAGE:
        if (mine && j->page >= 0) page_size(j, rep);
        break;
    case J_RENDER:
    case J_THUMB:
    case J_SLIDE:
        if (!mine || rep->status == ECANCELED) break;
        if (rep->status != 0 || !data || rep->w < 1 || rep->h < 1 || pix_bytes(rep->w, rep->h) > size) {
            /* TRANSLATORS: %d is the page number; %.200s is the reason from the renderer. */
            if (!doc.page[j->page].failed) toast(1, _("Page %d cannot be shown: %.200s"), j->page + 1, rep->text);
            doc.page[j->page].failed = 1;
            ui.dirty = 1;
            break;
        }
        if (j->rotate != v.rotate || (j->kind == J_RENDER && !still_wanted(j))) break;
        ui.dirty = 1;
        /* Kept, or unmapped by pix_add when there is no room. */
        if (j->kind == J_SLIDE && !still_wanted(j)) break;
        pix_add(j->kind == J_THUMB ? 1 : j->kind == J_SLIDE ? 2 : 0, j->page, j->rotate, j->zoom, j->tx, j->ty, rep->w, rep->h,
                data, size, (j->kind == J_RENDER && still_wanted(j)) || (j->kind == J_SLIDE && j->page == pres.page));
        return 1;
    case J_BBOX:
        if (!mine) break;
        {
            struct anchor a = anchor_at(ax, ay);
            struct page *pg = &doc.page[j->page];
            if (rep->status == ECANCELED) {
                pg->bounds = 0; /* asked again */
                break;
            }
            if (rep->status) {
                /* Shown, not silently treated as an untrimmed page; asked
                 * again from a later display zoom (as Okular takes the bbox
                 * of a later whole-page pixmap). */
                pg->bounds = 0;
                pg->bbox_failed = j->req.zoom;
                /* TRANSLATORS: %d is the page number; %.200s is the reason from the renderer. */
                toast(1, _("Cannot find the margins of page %d: %.200s"), j->page + 1, rep->text);
                break;
            }
            pg->bounds = 2;
            pg->bbox = (struct vl_crop){ 0, 0, 1, 1 };
            if (reply_count(rep, sizeof(struct view_rect), size, data) == 1) {
                const struct view_rect *b = data;
                pg->bbox = (struct vl_crop){ b->x0, b->y0, b->x1, b->y1 };
            }
            pix_flush();
            layout();
            anchor_keep(&a);
            changed();
        }
        break;
    case J_LINKS:
        if (!mine) break;
        {
            struct page *pg = &doc.page[j->page];
            int n = rep->status == 0 ? reply_count(rep, sizeof(struct view_link), size, data) : 0;
            free(pg->link);
            pg->link = n ? malloc(sizeof *pg->link * (size_t)n) : NULL;
            pg->nlink = pg->link ? n : 0;
            if (pg->link) memcpy(pg->link, data, sizeof *pg->link * (size_t)n);
            for (int k = 0; k < pg->nlink; k++) pg->link[k].uri[sizeof pg->link[k].uri - 1] = '\0';
            pg->links = 2;
        }
        break;
    case J_HITS:
        if (!mine || j->tx != find.gen) break;
        if (rep->status == ECANCELED) {
            doc.page[j->page].hits = 0;
            break;
        }
        {
            struct page *pg = &doc.page[j->page];
            int n = rep->status == 0 ? reply_count(rep, sizeof(struct view_rect), size, data) : 0;
            free(pg->hit);
            pg->hit = n ? malloc(sizeof *pg->hit * (size_t)n) : NULL;
            pg->nhit = pg->hit ? n : 0;
            if (pg->hit) memcpy(pg->hit, data, sizeof *pg->hit * (size_t)n);
            pg->hits = 2;
            ui.dirty = 1;
            if (find.dir && j->page == find.step_page) find_continue();
        }
        break;
    case J_SELECT:
        got_select(j, rep, data, size);
        break;
    case J_OUTLINE:
        if (j->gen != doc.gen) break;
        {
            int n = rep->status == 0 ? reply_count(rep, sizeof(struct view_outline), size, data) : 0;
            free(doc.outline);
            doc.outline = n ? malloc(sizeof *doc.outline * (size_t)n) : NULL;
            doc.noutline = doc.outline ? n : 0;
            if (doc.outline) memcpy(doc.outline, data, sizeof *doc.outline * (size_t)n);
            for (int k = 0; k < doc.noutline; k++) doc.outline[k].title[sizeof doc.outline[k].title - 1] = '\0';
            if (!doc.noutline && ui.tab == TAB_CONTENTS) ui.tab = TAB_THUMBNAILS;
            ui.dirty = 1;
        }
        break;
    case J_INFO:
        if (rep->status != 0) toast(1, _("No properties: %.200s"), rep->text);
        else list_open(L_INFO, data, data ? size : 0);
        break;
    case J_EMBEDDED:
        if (rep->status != 0) toast(1, _("No embedded files: %.200s"), rep->text);
        else if (!data || !size) toast(0, _("This document has no embedded files."));
        else list_open(L_EMBEDDED, data, size);
        break;
    case J_EXPORT:
        if (rep->status != 0) toast(1, _("Cannot export: %.200s"), rep->text);
        else write_export(data ? data : "", data ? size : 0);
        break;
    case J_SAVE:
        /* TRANSLATORS: the first %.200s is the file name; the second is the reason from the renderer. */
        if (rep->status != 0) toast(1, _("Cannot save %.200s: %.200s"), base_name(target), rep->text);
        /* TRANSLATORS: %.200s is the file name. */
        else toast(0, _("Saved %.200s."), base_name(target));
        break;
    case J_LAYOUT:
        if (j->gen == doc.gen) got_layout(rep);
        break;
    case J_RESOLVE:
        if (j->gen != doc.gen) break;
        if (rep->status == 0 && rep->page >= 0 && rep->page < doc.pages) go(pos_point(rep->page, rep->x, rep->y));
        /* TRANSLATORS: %.200s is the link URI. */
        else toast(1, _("Cannot follow the link %.200s."), j->req.text);
        break;
    case J_TOPTEXT:
        if (j->gen == doc.gen) got_top_text(rep, data, size);
        break;
    case J_REFIND:
        if (j->gen == doc.gen) got_refind(j, rep, data, size);
        break;
    case J_WORDS:
        got_words(j, rep, data, size);
        break;
    case J_MARK:
    case J_FIND_MARK:
        got_mark(j, rep);
        break;
    case J_MAGNIFY:
        got_magnify(j, rep, data, size);
        break;
    case J_PDF_STATUS:
    case J_PDF_ANNOTS:
    case J_PDF_FIELDS:
    case J_PDF_SIGS:
    case J_PDF_EDIT:
    case J_PDF_WRITE:
        pdf_reply(j, rep, data, size);
        break;
    }
    return 0;
}

static void receive(void)
{
    struct view_reply rep;
    int fd = -1;
    if (view_recv(sock, &rep, &fd) < 0) {
        renderer_gone();
        return;
    }
    if (!fly.busy || rep.serial != fly.job.req.serial || rep.op != fly.job.req.op) {
        if (fd >= 0) close(fd);
        return;
    }
    struct job j = fly.job;
    fly.busy = 0;
    void *data = NULL;
    size_t size = 0;
    if (fd >= 0) {
        data = view_map(fd, rep.size);
        close(fd);
        if (!data) {
            renderer_gone(); /* a memfd it could still change */
            return;
        }
        size = (size_t)rep.size;
    }
    if (!reply(&j, &rep, data, size) && data) munmap(data, size);
}

/* Drawing. Highlights over a page: the search's (the current match more
 * strongly, in Okular's find colour) and the selection. */
struct overlay {
    int x0, y0, x1, y1, alpha;
    uint32_t rgb;
};
static struct overlay *ovl;
static int novl, ovl_size;

static void overlay_add(int page, const struct view_rect *r, uint32_t rgb, int alpha)
{
    struct vl_params p = params();
    double X0, Y0, X1, Y1;
    vl_page_to_screen(&p, &doc.item[page], doc.pw[page], doc.ph[page], ax, ay, v.sx, v.sy, r->x0, r->y0, &X0, &Y0);
    vl_page_to_screen(&p, &doc.item[page], doc.pw[page], doc.ph[page], ax, ay, v.sx, v.sy, r->x1, r->y1, &X1, &Y1);
    if (novl == ovl_size) {
        struct overlay *o = realloc(ovl, sizeof *o * (size_t)(ovl_size + 64));
        if (!o) return;
        ovl = o;
        ovl_size += 64;
    }
    ovl[novl++] = (struct overlay){ (int)floor(fmin(X0, X1)), (int)floor(fmin(Y0, Y1)), (int)ceil(fmax(X0, X1)),
                                    (int)ceil(fmax(Y0, Y1)), alpha, rgb };
}

static void overlays_for(int page)
{
    novl = 0;
    const struct page *pg = &doc.page[page];
    for (int k = 0; find.active[0] && pg->hits == 2 && k < pg->nhit; k++) {
        int current = page == find.cur_page && pg->hit[k].hit == find.cur_hit;
        overlay_add(page, &pg->hit[k], FIND_HIT, current ? 170 : 90);
    }
    if (trim.dragging && page == trim.page) {
        struct view_rect r = { (float)trim.xa, (float)trim.ya, (float)trim.xb, (float)trim.yb, 0 };
        overlay_add(page, &r, SELECTED, 90);
    }
    for (int k = 0; sel.active && k < sel.npart; k++) {
        for (int r = 0; sel.part[k].page == page && r < sel.part[k].nrect; r++) {
            overlay_add(page, &sel.part[k].rect[r], SELECTED, 110);
        }
    }
    pdf_overlays(page);
}

static uint32_t blend(uint32_t p, uint32_t c, int a)
{
    uint32_t out = 0;
    for (int s = 0; s < 24; s += 8) {
        uint32_t x = ((p >> s & 0xFF) * (uint32_t)(256 - a) + (c >> s & 0xFF) * (uint32_t)a) >> 8;
        out |= x << s;
    }
    return out;
}

/* Okular's Change Colors (gui/pagepainter.cpp: recolor, blackWhite,
 * invertLightness, invertLuma, invertLumaPixel, hueShiftPositive and
 * Negative), applied to the drawn page before its highlights. */
static int qgray(int r, int g, int b)
{
    return (r * 11 + g * 16 + b * 5) / 32; /* Qt's qGray */
}

static uint32_t invert_luma(int R, int G, int B, float Y_R, float Y_G, float Y_B)
{
    if (R == G && G == B) return (uint32_t)(255 - R) << 16 | (uint32_t)(255 - G) << 8 | (uint32_t)(255 - B);
    float Y = R * Y_R + G * Y_G + B * Y_B, Y_inv = 255 - Y;
    int m = R < G ? (R < B ? R : B) : (G < B ? G : B);
    R -= m;
    G -= m;
    B -= m;
    float Y_full_C;
    if (R >= B && B >= G) Y_full_C = 255 * Y_R + 255 * Y_B * B / R;
    else if (R >= G && G >= B) Y_full_C = 255 * Y_R + 255 * Y_G * G / R;
    else if (G >= R && R >= B) Y_full_C = 255 * Y_G + 255 * Y_R * R / G;
    else if (G >= B && B >= R) Y_full_C = 255 * Y_G + 255 * Y_B * B / G;
    else if (B >= G && G >= R) Y_full_C = 255 * Y_B + 255 * Y_G * G / B;
    else Y_full_C = 255 * Y_B + 255 * Y_R * R / B;
    float C_max = Y >= Y_full_C ? Y_inv / (255 - Y_full_C) : Y / Y_full_C;
    float C_inv_max = Y_inv >= Y_full_C ? Y / (255 - Y_full_C) : Y_inv / Y_full_C;
    float C_scale = C_inv_max / C_max, R_ = R * C_scale, G_ = G * C_scale, B_ = B * C_scale;
    float m_inv = Y_inv - (Y_R * R_ + Y_G * G_ + Y_B * B_);
    int r = clampi((int)(R_ + m_inv + 0.5f), 0, 255), g = clampi((int)(G_ + m_inv + 0.5f), 0, 255);
    int b = clampi((int)(B_ + m_inv + 0.5f), 0, 255);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static uint32_t recolor(uint32_t p)
{
    int r = p >> 16 & 0xFF, g = p >> 8 & 0xFF, b = p & 0xFF;
    switch (settings.render_mode) {
    case CM_INVERTED: return ~p & 0xFFFFFF;
    case CM_RECOLOR: {
        uint32_t fg = settings.recolor_fg, bg = settings.recolor_bg;
        int l = qgray(r, g, b), out = 0;
        for (int sh = 16; sh >= 0; sh -= 8) {
            float scale = (float)((bg >> sh & 0xFF) - (fg >> sh & 0xFF)) / 255;
            int v = (int)(scale * l + (float)(fg >> sh & 0xFF));
            out |= clampi(v, 0, 255) << sh;
        }
        return (uint32_t)out;
    }
    case CM_BLACK_WHITE: {
        int val = qgray(r, g, b), con = settings.bw_contrast, thr = 255 - settings.bw_threshold;
        if (val > thr) val = 128 + (127 * (val - thr)) / (255 - thr);
        else if (val < thr) val = (128 * val) / thr;
        if (con > 2) val = clampi(thr + (val - thr) * con / 2, 0, 255);
        return (uint32_t)val * 0x010101;
    }
    case CM_INVERT_LIGHTNESS: {
        int m = r < g ? (r < b ? r : b) : (g < b ? g : b);
        r -= m;
        g -= m;
        b -= m;
        int c = r > g ? (r > b ? r : b) : (g > b ? g : b), m_ = 255 - c - m;
        return (uint32_t)(r + m_) << 16 | (uint32_t)(g + m_) << 8 | (uint32_t)(b + m_);
    }
    case CM_INVERT_LUMA: return invert_luma(r, g, b, 0.2126f, 0.7152f, 0.0722f);
    case CM_INVERT_LUMA_SYMMETRIC: return invert_luma(r, g, b, 0.3333f, 0.3334f, 0.3333f);
    case CM_HUE_POSITIVE: return (uint32_t)b << 16 | (uint32_t)r << 8 | (uint32_t)g;
    case CM_HUE_NEGATIVE: return (uint32_t)g << 16 | (uint32_t)b << 8 | (uint32_t)r;
    }
    return p; /* CM_PAPER: drawn on the paper by view-render */
}

/* The colours are changed (but the magnifier's, PagePainter flags 0). */
static int finish_plain;
static int colors_changed(void)
{
    return settings.invert && settings.render_mode != CM_PAPER;
}

/* The paper a page is drawn on: Change Paper Color's, else white. */
static uint32_t paper_rgb(void)
{
    return settings.invert && settings.render_mode == CM_PAPER ? settings.paper : PAPER;
}

/* The pictures being kept of what is drawn (Area Selection's image). */
static struct {
    int on, x0, y0, w, h;
    uint32_t *px;
} capture;

/* The scratch picture (w by h, for screen pixel x0, y0): colours changed
 * when asked, the highlights over it, onto the screen. */
static void finish(int x0, int y0, int w, int h)
{
    size_t n = (size_t)w * (size_t)h;
    if (colors_changed() && !finish_plain) {
        for (size_t k = 0; k < n; k++) scratch[k] = recolor(scratch[k] & 0xFFFFFF);
    }
    for (int k = 0; k < novl; k++) {
        const struct overlay *o = &ovl[k];
        int ox0 = o->x0 > x0 ? o->x0 : x0, oy0 = o->y0 > y0 ? o->y0 : y0;
        int ox1 = o->x1 < x0 + w ? o->x1 : x0 + w, oy1 = o->y1 < y0 + h ? o->y1 : y0 + h;
        for (int y = oy0; y < oy1; y++) {
            uint32_t *row = scratch + (size_t)(y - y0) * (size_t)w;
            for (int x = ox0; x < ox1; x++) row[x - x0] = blend(row[x - x0], o->rgb, o->alpha);
        }
    }
    if (capture.on) {
        for (int y = y0 > capture.y0 ? y0 : capture.y0; y < y0 + h && y < capture.y0 + capture.h; y++)
            for (int x = x0 > capture.x0 ? x0 : capture.x0; x < x0 + w && x < capture.x0 + capture.w; x++)
                capture.px[(size_t)(y - capture.y0) * (size_t)capture.w + (size_t)(x - capture.x0)] =
                    scratch[(size_t)(y - y0) * (size_t)w + (size_t)(x - x0)];
    }
    screen_blit(x0, y0, w, h, scratch, w);
}

/* Clip (cx0, cy0)-(cx1, cy1) to the document area: 0 if nothing is left. */
static int clip_area(int *cx0, int *cy0, int *cx1, int *cy1)
{
    if (*cx0 < ax) *cx0 = ax;
    if (*cy0 < ay) *cy0 = ay;
    if (*cx1 > ax + aw) *cx1 = ax + aw;
    if (*cy1 > ay + ah) *cy1 = ay + ah;
    return *cx0 < *cx1 && *cy0 < *cy1 && scratch;
}

/* Pixels src (sw by sh) with their top left at screen (X, Y), what of them
 * is inside the clip. */
static void put_pixels(const uint32_t *src, int sw, int sh, int X, int Y, int cx0, int cy0, int cx1, int cy1)
{
    int x0 = X > cx0 ? X : cx0, y0 = Y > cy0 ? Y : cy0;
    int x1 = X + sw < cx1 ? X + sw : cx1, y1 = Y + sh < cy1 ? Y + sh : cy1;
    if (x0 >= x1 || y0 >= y1) return;
    int w = x1 - x0, h = y1 - y0;
    for (int r = 0; r < h; r++) {
        memcpy(scratch + (size_t)r * (size_t)w, src + (size_t)(y0 - Y + r) * (size_t)sw + (size_t)(x0 - X),
               sizeof *scratch * (size_t)w);
    }
    finish(x0, y0, w, h);
}

static void put_paper(int cx0, int cy0, int cx1, int cy1)
{
    int w = cx1 - cx0, h = cy1 - cy0;
    if (w <= 0 || h <= 0) return;
    uint32_t paper = paper_rgb();
    for (size_t k = 0; k < (size_t)w * (size_t)h; k++) scratch[k] = paper;
    finish(cx0, cy0, w, h);
}

/* Until it is drawn: any picture there is of page i (another zoom, or its
 * thumbnail) scaled to its iw by ih at (X, Y). 0 if there is none. */
static int put_scaled(int i, int X, int Y, int iw, int ih, int cx0, int cy0, int cx1, int cy1)
{
    int best = -1; /* the largest whole page, else the thumbnail */
    for (int k = 0; k < ncache; k++) {
        const struct pix *c = &cache[k];
        if (!c->used || c->page != i || c->rotate != v.rotate || c->tx >= 0) continue;
        if (best < 0 || c->kind < cache[best].kind || (c->kind == cache[best].kind && c->w > cache[best].w)) best = k;
    }
    if (best < 0 || cx0 >= cx1 || cy0 >= cy1) return 0;
    /* The sidebar always shows an uncropped whole page. Never stretch a
     * cropped document-cache entry to pretend it is an uncropped thumbnail. */
    if (X < ax && cache[best].kind == 0 && (doc.crop[i].x0 != 0 || doc.crop[i].y0 != 0 ||
        doc.crop[i].x1 != 1 || doc.crop[i].y1 != 1)) return 0;
    const struct pix *c = &cache[best];
    struct vl_crop crop = { 0, 0, 1, 1 };
    if (X >= ax && c->kind == 1) crop = doc.crop[i];
    int w = cx1 - cx0, h = cy1 - cy0;
    for (int r = 0; r < h; r++) {
        int sy = (int)((crop.y0 + (cy0 + r - Y) / (double)(ih > 0 ? ih : 1) * (crop.y1 - crop.y0)) * c->h);
        sy = clampi(sy, 0, c->h - 1);
        const uint32_t *src = c->px + (size_t)sy * (size_t)c->w;
        uint32_t *dst = scratch + (size_t)r * (size_t)w;
        for (int x = 0; x < w; x++) {
            int sx = clampi((int)((crop.x0 + (cx0 + x - X) / (double)(iw > 0 ? iw : 1) * (crop.x1 - crop.x0)) * c->w), 0, c->w - 1);
            dst[x] = src[sx];
        }
    }
    finish(cx0, cy0, w, h);
    return 1;
}

static void draw_page(int i)
{
    const struct vl_item *it = &doc.item[i];
    const struct page *pg = &doc.page[i];
    int X = ax - v.sx + it->x, Y = ay - v.sy + it->y;
    int cx0 = X, cy0 = Y, cx1 = X + it->w, cy1 = Y + it->h;
    if (!clip_area(&cx0, &cy0, &cx1, &cy1)) return;
    overlays_for(i);
    float z = (float)it->zoom;
    if (pg->failed) {
        novl = 0;
        put_paper(cx0, cy0, cx1, cy1);
        int col = (cx0 + cw - 1) / cw, row = (cy0 - top + ch - 1) / ch;
        if (row >= 2 && row < nrows && col < ncols) screen_text(col, row, _("This page cannot be shown."), ERROR, PAPER);
        return;
    }
    if (whole(i)) {
        int c = pix_find(0, i, v.rotate, z, -1, -1);
        if (c >= 0) {
            cache[c].used = ++ticks;
            put_pixels(cache[c].px, cache[c].w, cache[c].h, X, Y, cx0, cy0, cx1, cy1);
        } else if (!put_scaled(i, X, Y, it->w, it->h, cx0, cy0, cx1, cy1)) {
            put_paper(cx0, cy0, cx1, cy1);
        }
        return;
    }
    for (int ty = (cy0 - Y) / TILE; ty <= (cy1 - 1 - Y) / TILE; ty++) {
        for (int tx = (cx0 - X) / TILE; tx <= (cx1 - 1 - X) / TILE; tx++) {
            int tx0 = X + tx * TILE, ty0 = Y + ty * TILE;
            int bx0 = tx0 > cx0 ? tx0 : cx0, by0 = ty0 > cy0 ? ty0 : cy0;
            int bx1 = tx0 + TILE < cx1 ? tx0 + TILE : cx1, by1 = ty0 + TILE < cy1 ? ty0 + TILE : cy1;
            int c = pix_find(0, i, v.rotate, z, tx, ty);
            if (c >= 0) {
                cache[c].used = ++ticks;
                put_pixels(cache[c].px, cache[c].w, cache[c].h, tx0, ty0, bx0, by0, bx1, by1);
            } else if (!put_scaled(i, X, Y, it->w, it->h, bx0, by0, bx1, by1)) {
                put_paper(bx0, by0, bx1, by1);
            }
        }
    }
}

/* s cut to cells cells (wide characters two). */
static void fit(char *out, size_t size, const char *s, int cells)
{
    size_t n = 0;
    int used = 0;
    while (s[n] && n + 4 < size) {
        unsigned char b = (unsigned char)s[n];
        int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
        uint32_t cp = len == 1 ? b : b & (0x3Fu >> (len - 1));
        for (int k = 1; k < len; k++) {
            if (((unsigned char)s[n + k] & 0xC0) != 0x80) {
                len = k;
                break;
            }
            cp = cp << 6 | ((unsigned char)s[n + k] & 0x3F);
        }
        int wide = screen_char_cells(cp);
        if (used + wide > cells) break;
        used += wide;
        n += (size_t)len;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    for (size_t k = 0; k < n; k++) {
        if ((unsigned char)out[k] < ' ') out[k] = ' '; /* titles and names from documents */
    }
}

static int side_last(void)
{
    return bottom_used() ? nrows - 2 : nrows - 1;
}

/* The contents' entry for the current page: the last one at or before it. */
static int toc_current(void)
{
    int cur = current_page(), at = -1;
    for (int k = 0; k < doc.noutline; k++) {
        if (doc.outline[k].page >= 0 && doc.outline[k].page <= cur) at = k;
    }
    return at;
}

static void draw_sidebar(void)
{
    char line[600], text[512], label[64];
    int focus = ui.focus == FOCUS_SIDEBAR;
    screen_fill_px(0, top + 2 * ch, side_px, H - (top + 2 * ch), BG);
    if (tabs.n > 1) screen_fill_px(side_px, top + 2 * ch, splitter_px, ch, DIM);
    screen_fill(0, 2, side_cols, nrows - 2, BG);
    if (!doc.open) return;
    int col = 1;
    for (int t = doc.noutline ? TAB_CONTENTS : TAB_THUMBNAILS; t <= TAB_BOOKMARKS; t++) {
        const char *name = t == TAB_BOOKMARKS ? (side_cols >= 38 ? _("Bookmarks") : "B") :
            side_cols >= 38 ? (t == TAB_CONTENTS ? _("Contents") : _("Thumbnails")) :
            side_cols >= 20 ? (t == TAB_CONTENTS ? _("Contents") : _("Thumbs")) : (t == TAB_CONTENTS ? "C" : "T");
        snprintf(label, sizeof label, " %s ", name);
        int w = ui_cells(label), on = ui.tab == t;
        screen_text(col, 2, label, on ? BG : FG, on ? ACCENT : BG);
        ui_hit(col, 2, w, 1, HIT_TAB + t);
        col += w + 1;
    }
    pdf_sidebar_tabs(col);
    if (pdf_draw_sidebar()) return;
    int last = side_last();
    if (ui.tab == TAB_BOOKMARKS) {
        int n = 0, rows = last - 3 + 1;
        for (int i = 0; i < doc.pages; i++) n += doc.bookmark[i] != NULL;
        ui.bm_sel = clampi(ui.bm_sel, 0, n > 0 ? n - 1 : 0);
        if (ui.bm_sel < ui.bm_top) ui.bm_top = ui.bm_sel;
        if (ui.bm_sel >= ui.bm_top + rows) ui.bm_top = ui.bm_sel - rows + 1;
        int row = 3, k = 0;
        for (int i = 0; i < doc.pages && row <= last; i++) if (doc.bookmark[i]) {
            if (k++ < ui.bm_top) continue;
            int on = focus && k - 1 == ui.bm_sel;
            uint32_t bg = on ? ACCENT : BG, fg = on ? BG : i == current_page() ? ACCENT : FG;
            snprintf(line, sizeof line, "%d %s", i + 1, doc.bookmark[i]);
            fit(text, sizeof text, line, side_cols - 1);
            screen_fill(0, row, side_cols - 1, 1, bg);
            screen_text(0, row, text, fg, bg);
            ui_hit(0, row++, side_cols, 1, HIT_BOOKMARK + i);
        }
        if (!n) screen_text(0, 3, _("No bookmarks"), DIM, BG);
        return;
    }
    if (ui.tab == TAB_CONTENTS) {
        int cur = toc_current(), rows = last - 3 + 1;
        if (ui.toc_sel < ui.toc_top) ui.toc_top = ui.toc_sel;
        if (ui.toc_sel >= ui.toc_top + rows) ui.toc_top = ui.toc_sel - rows + 1;
        for (int r = 3; r <= last; r++) {
            int k = ui.toc_top + r - 3;
            if (k >= doc.noutline) break;
            const struct view_outline *o = &doc.outline[k];
            char num[16] = "";
            if (o->page >= 0) snprintf(num, sizeof num, "%d", o->page + 1);
            int indent = 1 + 2 * clampi(o->level, 0, 6), room = side_cols - indent - (int)strlen(num) - 2;
            fit(text, sizeof text, o->title, room > 1 ? room : 1);
            int on = focus && k == ui.toc_sel;
            uint32_t bg = on ? ACCENT : BG, fg = on ? BG : k == cur ? ACCENT : FG;
            screen_fill(0, r, side_cols - 1, 1, bg);
            snprintf(line, sizeof line, "%*s%s", indent, "", text);
            screen_text(0, r, line, fg, bg);
            screen_text(side_cols - 1 - (int)strlen(num), r, num, on ? BG : DIM, bg);
            ui_hit(0, r, side_cols - 1, 1, HIT_TOC + k);
        }
        return;
    }
    int y0 = thumbs_y0(), y1 = last >= 0 ? top + (last + 1) * ch : H, cur = current_page();
    if (ui.follow_thumbs) {
        int at;
        if (!thumb_seen(cur, &at) || at < ui.thumb_top || at + thumb_entry(cur) > ui.thumb_top + (y1 - y0)) {
            ui.thumb_top = at - (y1 - y0) / 3 > 0 ? at - (y1 - y0) / 3 : 0;
        }
        ui.follow_thumbs = 0;
        ui.thumb_sel = cur;
    }
    novl = 0;
    for (int i = 0, y = y0 - ui.thumb_top; i < doc.pages && y < y1; i++) {
        int e = thumb_entry(i);
        if (y + e <= y0) {
            y += e;
            continue;
        }
        int w, h;
        double z;
        thumb_size(i, &w, &h, &z);
        int X = (side_px - w) / 2, cx0 = X, cy0 = y > y0 ? y : y0, cx1 = X + w, cy1 = y + h < y1 ? y + h : y1;
        if (i == cur || (focus && i == ui.thumb_sel)) {
            uint32_t frame = focus && i == ui.thumb_sel ? FG : ACCENT;
            int fy0 = y - 2 > y0 ? y - 2 : y0, fy1 = y + h + 2 < y1 ? y + h + 2 : y1;
            if (fy1 > fy0) screen_fill_px(X - 2, fy0, w + 4, fy1 - fy0, frame);
        }
        if (cy1 > cy0) {
            int c = pix_find(1, i, v.rotate, (float)z, -1, -1);
            if (doc.page[i].failed) {
                put_paper(cx0, cy0, cx1, cy1);
            } else if (c >= 0) {
                cache[c].used = ++ticks;
                put_pixels(cache[c].px, cache[c].w, cache[c].h, X, y, cx0, cy0, cx1, cy1);
            } else if (!put_scaled(i, X, y, w, h, cx0, cy0, cx1, cy1)) {
                put_paper(cx0, cy0, cx1, cy1);
            }
        }
        int row = (y + h - top + ch - 1) / ch;
        snprintf(line, sizeof line, "%d", i + 1);
        if (row >= 3 && row <= last) {
            screen_text((side_cols - (int)strlen(line)) / 2, row, line, i == cur ? ACCENT : FG, BG);
        }
        int r0 = clampi((y - top) / ch, 3, last), r1 = clampi((y + e - 1 - top) / ch, 3, last);
        if (y + e > y0) ui_hit(0, r0, side_cols, r1 - r0 + 1, HIT_THUMB + i);
        y += e;
    }
}

static const char *menu_name(int m)
{
    switch (m) {
    case 0: return _("File");
    case 1: return _("Edit");
    case 2: return C_("menu", "View");
    case 3: return C_("menu", "Go");
    case 4: return _("Tools");
    default: return _("Settings");
    }
}
#define MENUS 6

static int zoom_percent(void)
{
    return doc.open ? (int)lround(doc.item[current_page()].zoom / z100 * 100) : (int)lround(v.zoom * 100);
}

static void draw_menubar(void)
{
    char page[64], zoom[16], text[300];
    screen_fill(0, 1, ncols, 1, BG);
    int col = ui_button(0, 1, settings.sidebar ? _("Hide Sidebar") : _("Show Sidebar"), HIT_SIDEBAR);
    for (int m = 0; m < MENUS; m++) {
        const char *name = menu_name(m);
        int len = ui_cells(name);
        screen_text(col, 1, name, FG, BG);
        ui_hit(col - 1, 1, len + 2, 1, HIT_MENU + m);
        col += len + 2;
    }
    if (!doc.open) return;
    char title[300];
    /* KDE's caption of a modified document. */
    if (pdf_modified()) {
        /* TRANSLATORS: %.255s is the document title or file name. */
        snprintf(title, sizeof title, _("%.255s [modified]"), doc.title[0] ? doc.title : base_name(doc.path));
    } else {
        snprintf(title, sizeof title, "%.255s", doc.title[0] ? doc.title : base_name(doc.path));
    }
    /* TRANSLATORS: the first %d is the current page number; the second is the total page count. */
    snprintf(page, sizeof page, _("Page %d of %d"), current_page() + 1, doc.pages);
    snprintf(zoom, sizeof zoom, "%d%%", zoom_percent());
    int right = ncols - 1 - ui_cells(doc.format);
    screen_text(right, 1, doc.format, DIM, BG);
    right -= 3 + (int)strlen(zoom);
    screen_text(right, 1, zoom, FG, BG);
    ui_hit(right, 1, (int)strlen(zoom), 1, HIT_ZOOM);
    int page_w = ui_cells(page);
    right -= 3 + page_w;
    screen_text(right, 1, page, FG, BG);
    ui_hit(right, 1, page_w, 1, HIT_GOTO);
    int room = right - col - 3;
    if (room > 4) {
        fit(text, sizeof text, title, room);
        screen_text(col + 1, 1, text, DIM, BG);
    }
}

static size_t utf_prev(const char *s, size_t p);
static size_t utf_next(const char *s, size_t p);

static int utf_cells(const char *s, size_t from, size_t to)
{
    int cells = 0;
    for (size_t i = from; i < to;) {
        size_t next = utf_next(s, i);
        unsigned char b = (unsigned char)s[i];
        uint32_t cp = next - i == 1 ? b : b & (0x3Fu >> (next - i - 1));
        for (size_t k = i + 1; k < next; k++) cp = cp << 6 | ((unsigned char)s[k] & 63);
        cells += screen_char_cells(cp);
        i = next;
    }
    return cells;
}

#define HIT_PROMPT_INPUT 1351
static void line_draw(struct line *l, int col, int row, int width, int focused, uint32_t fg, uint32_t bg);
static void line_set(struct line *l, const char *text, int all);
static void line_insert(struct line *l, const char *s, size_t len);
static int line_key(struct line *l, int key, int mods);
static size_t line_at_cell(const struct line *l, int col);
static void line_paste(struct line *l, int primary);
static int prompt_typed(void);

static void draw_bottom(void)
{
    int row = nrows - 1;
    screen_fill_px(0, top + row * ch, W, H - (top + row * ch), BG);
    screen_fill(0, row, ncols, 1, BG);
    if (pdf_draw_bottom(row)) return;
    if (prompt.kind != P_NONE) {
        char question[700];
        fit(question, sizeof question, prompt.question, prompt_typed() ? (ncols * 2 / 3 > 8 ? ncols * 2 / 3 : 8) : ncols - 2);
        screen_text(1, row, question, ACCENT, BG);
        if (!prompt_typed()) return;
        int col = 2 + utf_cells(question, 0, strlen(question)), width = ncols - col - 1 > 4 ? ncols - col - 1 : 4;
        prompt.in.masked = prompt.masked;
        line_draw(&prompt.in, col, row, width, ui.focus == FOCUS_PROMPT, FG, 0x202838);
        ui_hit(col, row, width, 1, HIT_PROMPT_INPUT);
        return;
    }
    int col = ui_button(1, row, "x", HIT_FIND + 2);
    const char *find_label = _("Find:");
    screen_text(col, row, find_label, FG, BG);
    col += ui_cells(find_label) + 1;
    int width = clampi(ncols / 3, 1, ncols - col - 1 > 0 ? ncols - col - 1 : 1);
    line_draw(&find.in, col, row, width, ui.focus == FOCUS_FIND, find.failed ? ERROR : FG, find.failed ? 0x402020 : 0x202838);
    ui_hit(col, row, width, 1, HIT_FIND_INPUT);
    col += width + 1;
    col = ui_button(col, row, _("Next"), HIT_FIND + 0);
    col = ui_button(col, row, _("Previous"), HIT_FIND + 1);
    if (find.dir) screen_text(col + 1, row, _("Searching..."), DIM, BG);
    else if (find.failed) screen_text(col + 1, row, _("No matches found"), ERROR, BG);
}

static void draw_list(void)
{
    char text[1200];
    int c0 = side_cols + 2, c1 = ncols - 2, r0 = 3, r1 = side_last() - 1;
    if (c1 - c0 < 10 || r1 - r0 < 4) return;
    screen_fill(c0, r0, c1 - c0, r1 - r0 + 1, BG);
    screen_text(c0 + 1, r0, list.kind == L_INFO ? _("Properties") : _("Embedded Files"), ACCENT, BG);
    int rows = r1 - 1 - (r0 + 2);
    if (list.sel < list.top) list.top = list.sel;
    if (list.sel >= list.top + rows) list.top = list.sel - rows + 1;
    for (int r = 0; r < rows && list.top + r < list.n; r++) {
        int k = list.top + r, on = k == list.sel;
        fit(text, sizeof text, list.line[k], c1 - c0 - 3);
        screen_fill(c0 + 1, r0 + 2 + r, c1 - c0 - 2, 1, on ? ACCENT : BG);
        screen_text(c0 + 2, r0 + 2 + r, text, on ? BG : FG, on ? ACCENT : BG);
        ui_hit(c0 + 1, r0 + 2 + r, c1 - c0 - 2, 1, HIT_LINE + k);
    }
    int col = c0 + 1;
    if (list.kind == L_EMBEDDED) col = ui_button(col, r1, _("Save..."), HIT_LIST + 0);
    ui_button(col, r1, _("Close"), HIT_LIST + 1);
}

static void draw_document_tabs(void)
{
    if (!tab_rows()) return;
    int start = (ax + cw - 1) / cw;
    screen_fill(start, 2, ncols - start, 1, BG);
    int width = (ncols - start) / tabs.n;
    if (width < 4) width = 4;
    for (int i = 0; i < tabs.n && start + i * width < ncols; i++) {
        int col = start + i * width, room = width < ncols - col ? width : ncols - col;
        char label[PATH_MAX];
        fit(label, sizeof label, base_name(tabs.path[i]), room > 3 ? room - 3 : 1);
        uint32_t bg = i == tabs.current ? ACCENT : BG, fg = i == tabs.current ? BG : FG;
        screen_fill(col, 2, room, 1, bg);
        screen_text(col, 2, label, fg, bg);
        ui_hit(col, 2, room, 1, HIT_DOCUMENT_TAB + i);
        if (room >= 3) {
            screen_text(col + room - 2, 2, "x", fg, bg);
            ui_hit(col + room - 2, 2, 2, 1, HIT_CLOSE_TAB + i);
        }
    }
}

/* The input method's: the find bar, or a question's answer but a password
 * (a Password line edit takes no input method). */
static int typing_line(void)
{
    return ui.focus == FOCUS_FIND || (ui.focus == FOCUS_PROMPT && prompt_typed() && !prompt.masked);
}

static void draw(void)
{
    ui.dirty = 0;
    frame_start = ++ticks;
    ui_hits_clear();
    if (pres.on && doc.open) {
        draw_presentation();
        if (prompt.kind) draw_bottom();
        if (ui.toast[0] && now() < ui.toast_until) screen_text(1, nrows - 2, ui.toast, FG, TOAST);
        screen_flush();
        return;
    }
    screen_fill_px(ax, ay, aw, ah, DESK);
    if (doc.open) {
        compute_prio();
        for (int k = 0; k < nvis; k++) draw_page(prio[k]);
    } else {
        char text[400];
        /* TRANSLATORS: %.300s is the file name. */
        if (doc.opening) snprintf(text, sizeof text, _("Opening %.300s ..."), base_name(doc.path));
        /* TRANSLATORS: Ctrl+O and Ctrl+Q are key shortcuts; keep them. */
        else snprintf(text, sizeof text, _("No document.   Ctrl+O: open a file   Ctrl+Q: quit"));
        screen_text(side_cols + 2, 3, text, FG, DESK);
    }
    if (doc.open) draw_tools();
    if (side_cols) draw_sidebar();
    if (side_px) screen_fill_px(side_px, ay, splitter_px, ah, ptr.splitter ? ACCENT : DIM);
    draw_menubar();
    draw_document_tabs();
    pdf_draw();
    if (bottom_used()) draw_bottom();
    if (list.kind) draw_list();
    if (ui.toast[0] && now() < ui.toast_until) {
        char text[300];
        snprintf(text, sizeof text, " %s ", ui.toast);
        screen_text(side_cols + 1, 2, text, ui.toast_error ? 0xFFFFFF : FG, ui.toast_error ? 0x702828 : TOAST);
    } else {
        ui.toast[0] = '\0';
    }
    char preedit[512], candidates[512];
    int typing = typing_line();
    if (typing && ime_get_overlay(preedit, candidates, sizeof preedit)) {
        screen_text(0, nrows - 3, preedit, FG, BG);
        screen_text(0, nrows - 2, candidates, FG, BG);
    }
    screen_bar("view", typing ? ime_mode_label() : NULL);
    screen_flush();
}

/* After a menu or the file chooser, which took the keys: the screen may
 * have been given back meanwhile (K_SHOWN went to them), with a new scale. */
static void refresh(void)
{
    struct anchor a = anchor_at(ax, ay);
    geometry();
    layout();
    anchor_keep(&a);
    ui.cursor = -1;
    viewport_reflow();
    ui.dirty = 1;
}

/* Zooming: Okular's factor of the current page, the fit factors, and a new
 * zoom keeping what is at screen point (X, Y) there. */
static double zoom_now(void)
{
    return doc.open ? doc.item[current_page()].zoom / z100 : v.zoom;
}

static double fit_factor(int mode)
{
    int i = current_page(), avail_w = aw / columns() - col_margin, avail_h = ah - row_margin;
    float w, h;
    vl_rotated(v.rotate, doc.pw[i] > 0 ? doc.pw[i] : 1, doc.ph[i] > 0 ? doc.ph[i] : 1, &w, &h);
    w *= doc.crop[i].x1 - doc.crop[i].x0;
    h *= doc.crop[i].y1 - doc.crop[i].y0;
    double zw = (avail_w < 1 ? 1 : avail_w) / (double)w, zh = (avail_h < 1 ? 1 : avail_h) / (double)h;
    return (mode == VL_FIT_WIDTH ? zw : zw < zh ? zw : zh) / z100;
}

static void zoom_to(int mode, double factor, int X, int Y)
{
    if (!doc.open) return;
    struct anchor a = anchor_at(X, Y);
    v.mode = mode;
    if (mode == VL_FIXED) v.zoom = factor < VL_ZOOM_MIN ? VL_ZOOM_MIN : factor > VL_ZOOM_MAX ? VL_ZOOM_MAX : factor;
    layout();
    anchor_keep(&a);
    changed();
}

static void zoom_step(int in)
{
    if (!doc.open) return;
    int mode;
    double f = vl_zoom_step(zoom_now(), fit_factor(VL_FIT_WIDTH), fit_factor(VL_FIT_PAGE), in, &mode);
    if (mode >= 0) zoom_to(mode, f, ax + aw / 2, ay + ah / 2);
}

/* The rotation or the view mode, keeping the middle of the screen. */
static void relayout_middle(void)
{
    relayout_at(ax + aw / 2, ay + ah / 2);
    ui.follow_thumbs = 1;
    changed();
}

static void scroll_by(int dx, int dy)
{
    if (!doc.open) return;
    v.sx += dx;
    v.sy += dy;
    clamp_scroll();
    ui.follow_thumbs = 1;
    changed();
}

/* Okular's arrow keys: 100 pixels a step; Left and Right scroll sideways
 * (20 a step) when the pages are wider than the screen, else they go to
 * the previous or next page (a row of pages). */
static void step(int steps)
{
    scroll_by(0, (int)lround(100 * scale * steps));
}

static void sideways(int dir, int steps)
{
    if (!doc.open) return;
    if (doc.full_w > aw) scroll_by((int)lround(20 * scale * steps) * dir, 0);
    else go(pos_page(current_page() + dir * columns()));
}

/* A screen less Okular's ScrollOverlap (10%). */
static void screen_step(int dir)
{
    int limit = doc.full_h - ah > 0 ? doc.full_h - ah : 0;
    if (settings.paged && ((dir > 0 && v.sy >= limit) || (dir < 0 && v.sy <= 0))) {
        int next = current_page() + dir * columns();
        if (v.facing == VL_FIRST_CENTERED && doc.pages >= 3) {
            if (dir > 0 && current_page() == 0) next = 1;
            else if (dir < 0 && current_page() <= 2) next = 0;
        }
        go(pos_page(next));
    } else scroll_by(0, dir * (int)(ah * 0.9));
}

static void find_bar(int on)
{
    struct anchor a = anchor_at(ax, ay);
    find.bar = on;
    if (on) {
        find.in.limit = sizeof find.active - 1;
        find.in.replace = 1;
        find.in.mark = 0;
        find.in.cursor = strlen(find.in.text);
        ui.focus = FOCUS_FIND;
    } else {
        find.typed = 0;
        find_reset();
        if (ui.focus == FOCUS_FIND) ui.focus = FOCUS_DOC;
    }
    geometry();
    layout();
    anchor_keep(&a);
    viewport_reflow();
    changed();
}

static void sidebar(int on)
{
    struct anchor a = anchor_at(ax + aw / 2, ay + ah / 2);
    settings.sidebar = on;
    if (!on && ui.focus == FOCUS_SIDEBAR) ui.focus = FOCUS_DOC;
    geometry();
    layout();
    anchor_keep(&a);
    ui.follow_thumbs = 1;
    viewport_reflow();
    changed();
}

static void prompt_open(int kind, const char *question, const char *initial, int masked)
{
    struct anchor a = anchor_at(ax, ay);
    prompt.kind = kind;
    prompt.masked = masked;
    snprintf(prompt.question, sizeof prompt.question, "%s", question);
    memset(&prompt.in, 0, sizeof prompt.in);
    prompt.in.masked = masked;
    line_set(&prompt.in, initial, 1); /* selected, as Okular's dialogs give it */
    ui.focus = FOCUS_PROMPT;
    geometry();
    layout();
    anchor_keep(&a);
    ui.dirty = 1;
}

static void prompt_close(void)
{
    struct anchor a = anchor_at(ax, ay);
    prompt.kind = P_NONE;
    if (prompt.masked) memset(prompt.in.text, 0, sizeof prompt.in.text); /* a password */
    ui.focus = find.bar ? FOCUS_FIND : list.kind ? FOCUS_LIST : FOCUS_DOC;
    geometry();
    layout();
    anchor_keep(&a);
    ui.dirty = 1;
}

/* A folder for a file to go into, chosen as in home's file manager: the
 * document's own selected in its parent. */
static char chosen_dir[PATH_MAX];

static int choose_folder(const char *what)
{
    char parent[PATH_MAX], keep[NAME_MAX + 1];
    snprintf(parent, sizeof parent, "%s", doc.path);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = '\0'; /* the document's folder */
    slash = strrchr(parent, '/');
    snprintf(keep, sizeof keep, "%s", slash ? slash + 1 : "");
    if (slash && slash != parent) *slash = '\0';
    else if (slash) parent[1] = '\0';
    int ok = files_pick(parent, keep, "inode/directory", what, chosen_dir, sizeof chosen_dir);
    refresh();
    return ok;
}

static void start_export(void)
{
    struct job j = new_job(J_EXPORT, VIEW_EXPORT);
    push_job(&j, 0);
    /* TRANSLATORS: %.200s is the file name. */
    toast(0, _("Exporting to %.200s ..."), base_name(target));
}

static void start_save(void)
{
    struct job j = new_job(J_SAVE, VIEW_SAVE);
    j.req.page = save_index;
    snprintf(j.req.text, sizeof j.req.text, "%s", target);
    push_job(&j, 0);
}

/* The name typed for an export or an embedded file: into the chosen
 * folder, asking before writing over a file. */
static void named(int kind)
{
    const char *name = prompt.in.text;
    if (!name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") ||
        snprintf(target, sizeof target, "%s/%s", chosen_dir, name) >= (int)sizeof target) {
        prompt_close();
        /* TRANSLATORS: %.200s is the invalid file name typed by the user. */
        toast(1, _("%.200s cannot be a file's name here."), name);
        return;
    }
    struct stat st;
    as_user(1);
    int exists = lstat(target, &st) == 0;
    as_user(0);
    prompt_close();
    if (exists) {
        char q[600];
        /* TRANSLATORS: %.300s is the file name; y and N are keys; keep y/N. */
        snprintf(q, sizeof q, _("A file named %.300s already exists. Overwrite it? y/N"), name);
        prompt_open(P_OVERWRITE, q, "", 0);
        prompt.then = kind;
        return;
    }
    if (kind == P_EXPORT) start_export();
    else start_save();
}

static void color_typed(int which, const char *text);

static void prompt_done(int ok)
{
    int kind = prompt.kind;
    if (!ok) {
        prompt_close();
        /* TRANSLATORS: %.200s is the file name. */
        if (kind == P_PASSWORD) toast(0, _("%.200s was not opened."), base_name(doc.path));
        return;
    }
    if (kind == P_BOOKMARK) {
        int p = clampi(prompt.then, 0, doc.pages - 1);
        char *name = strdup(prompt.in.text);
        if (name) { free(doc.bookmark[p]); doc.bookmark[p] = name; }
        prompt_close(); changed();
    } else if (kind == P_GOTO) {
        int n = atoi(prompt.in.text);
        prompt_close();
        if (n >= 1 && n <= doc.pages && pres.on) pres_go(n - 1);
        else if (n >= 1 && n <= doc.pages) go(pos_page(n - 1));
        /* TRANSLATORS: %.20s is the page number typed; %d is the total page count. */
        else if (prompt.in.text[0]) toast(1, _("There is no page %.20s (1 to %d)."), prompt.in.text, doc.pages);
    } else if (kind == P_PASSWORD) {
        snprintf(doc.password, sizeof doc.password, "%s", prompt.in.text);
        memset(prompt.in.text, 0, sizeof prompt.in.text);
        doc.with_password = 1;
        prompt_close();
        struct job j = new_job(J_AUTH, VIEW_AUTH);
        snprintf(j.req.text, sizeof j.req.text, "%s", doc.password);
        doc.opening = 1;
        push_job(&j, 1);
    } else if (kind == P_EXPORT || kind == P_SAVE) {
        named(kind);
    } else if (kind == P_IMAGE) {
        image_named();
    } else if (kind == P_COLOR) {
        char text[64];
        snprintf(text, sizeof text, "%.63s", prompt.in.text);
        int which = prompt.then;
        prompt_close();
        color_typed(which, text);
    }
}

static void export_text(void)
{
    if (!doc.open) return;
    /* TRANSLATORS: o is the key in the file manager; keep it. */
    if (!choose_folder(_("Choose the folder to export the text into (o on it)."))) return;
    char name[NAME_MAX + 1];
    snprintf(name, sizeof name, "%.200s", base_name(doc.path));
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = '\0';
    if (strlen(name) + 4 < sizeof name) strcat(name, ".txt");
    prompt_open(P_EXPORT, _("Export as plain text, the file's name:"), name, 0);
}

static void save_embedded(void)
{
    if (list.kind != L_EMBEDDED || list.sel < 0 || list.sel >= list.n || list.index[list.sel] < 0) return;
    save_index = list.index[list.sel];
    char name[NAME_MAX + 1];
    snprintf(name, sizeof name, "%s", list.name[list.sel] ? base_name(list.name[list.sel]) : "file");
    /* TRANSLATORS: o is the key in the file manager; keep it. */
    if (!choose_folder(_("Choose the folder to save the file into (o on it)."))) return;
    prompt_open(P_SAVE, _("Save the embedded file as:"), name, 0);
}

static struct { int page, link; } ctx = { -1, -1 };

static int link_at(int X, int Y, int *page)
{
    double px, py;
    struct vl_params p = params();
    int i = doc.open ? vl_screen_to_page(&p, doc.pages, doc.item, doc.pw, doc.ph, ax, ay, v.sx, v.sy, X, Y, 0,
                                         &px, &py) : -1;
    *page = i;
    if (i < 0 || doc.page[i].links != 2) return -1;
    for (int k = 0; k < doc.page[i].nlink; k++) {
        const struct view_link *l = &doc.page[i].link[k];
        if (px >= fmin(l->x0, l->x1) && px <= fmax(l->x0, l->x1) && py >= fmin(l->y0, l->y1) &&
            py <= fmax(l->y0, l->y1)) {
            return k;
        }
    }
    return -1;
}

/* A URI with a scheme (RFC 3986): opened by home; else the document's own. */
static int external(const char *uri)
{
    int n = 0;
    while (uri[n] && (((uri[n] | 0x20) >= 'a' && (uri[n] | 0x20) <= 'z') ||
                      (n > 0 && ((uri[n] >= '0' && uri[n] <= '9') || strchr("+-.", uri[n]))))) {
        n++;
    }
    return n > 1 && uri[n] == ':';
}

static void follow_link(int page, int k)
{
    if (page < 0 || page >= doc.pages || k < 0 || k >= doc.page[page].nlink) return;
    const struct view_link *l = &doc.page[page].link[k];
    if (l->page >= 0) {
        go(pos_point(l->page, l->x, l->y));
    } else if (external(l->uri)) {
        /* TRANSLATORS: %.200s is the external URL or target. */
        if (open_in_home(NULL, l->uri) < 0) toast(1, _("Cannot open %.200s."), l->uri);
    } else if (l->uri[0]) {
        struct job j = new_job(J_RESOLVE, VIEW_RESOLVE);
        snprintf(j.req.text, sizeof j.req.text, "%s", l->uri);
        push_job(&j, 0);
    }
}

static void do_action(int a);

/* The menus: Okular's, with what view has of them. */
struct item {
    const char *label, *keys;
    int action, checked;
};

static int menu_items(int m, struct item *it)
{
    int n = 0, reflowed = doc.open && (doc.flags & VIEW_REFLOWABLE);
#define ITEM(label, keys, action, checked) (it[n++] = (struct item){ label, keys, action, checked })
    if (m == 0) {
        ITEM(_("Open..."), "Ctrl+O", A_OPEN, 0);
        ITEM(_("Open Recent..."), "", A_RECENT, 0);
        ITEM(_("Properties"), "Alt+Return", A_PROPERTIES, 0);
        ITEM(_("Embedded Files..."), "", A_EMBEDDED, 0);
        ITEM(_("Export As Plain Text..."), "", A_EXPORT, 0);
        ITEM(_("Quit"), "Ctrl+Q", A_QUIT, 0);
        ITEM(_("Reload"), "F5", A_RELOAD, 0);
        ITEM(_("Open Containing Folder"), "", A_CONTAINING, 0);
        ITEM(_("Close Tab"), "Ctrl+W", A_CLOSE_TAB, 0);
        ITEM(_("Undo Close Tab"), "Ctrl+Shift+T", A_REOPEN_TAB, 0);
    } else if (m == 1) {
        if (pdf_on()) { /* Okular's Edit menu begins with them */
            ITEM(_("Undo"), "Ctrl+Z", A_UNDO, 0);
            ITEM(_("Redo"), "Ctrl+Shift+Z", A_REDO, 0);
        }
        ITEM(_("Copy"), "Ctrl+C", A_COPY, 0);
        ITEM(_("Find..."), "Ctrl+F", A_FIND, 0);
        ITEM(_("Find Next"), "F3", A_FIND_NEXT, 0);
        ITEM(_("Find Previous"), "Shift+F3", A_FIND_PREV, 0);
    } else if (m == 2) {
        ITEM(_("Zoom In"), "Ctrl++", A_ZOOM_IN, 0);
        ITEM(_("Zoom Out"), "Ctrl+-", A_ZOOM_OUT, 0);
        ITEM(_("Zoom to 100%"), "Ctrl+0", A_ZOOM_100, 0);
        ITEM(_("Fit Width"), "", A_FIT_WIDTH, v.mode == VL_FIT_WIDTH);
        ITEM(_("Fit Page"), "", A_FIT_PAGE, v.mode == VL_FIT_PAGE);
        ITEM(_("Single Page"), "", A_SINGLE, v.facing == VL_SINGLE);
        ITEM(_("Facing Pages"), "", A_FACING, v.facing == VL_FACING);
        ITEM(_("Rotate Right"), "", A_ROTATE_RIGHT, 0);
        ITEM(_("Rotate Left"), "", A_ROTATE_LEFT, 0);
        ITEM(_("Original Orientation"), "", A_ROTATE_NONE, v.rotate == 0);
        ITEM(_("Invert Colors"), "", A_INVERT, settings.invert);
        if (reflowed) {
            ITEM(_("Larger Text"), ">", A_FONT_UP, 0);
            ITEM(_("Smaller Text"), "<", A_FONT_DOWN, 0);
        }
        ITEM(_("Trim Margins"), "", A_TRIM_MARGINS, settings.trim_margins);
        ITEM(_("Trim To Selection"), "", A_TRIM_SELECTION, trim.selection);
        ITEM(_("Auto Fit"), "", A_FIT_AUTO, v.mode == VL_FIT_AUTO);
        ITEM(_("Facing Pages (Center First Page)"), "", A_FIRST_CENTERED, v.facing == VL_FIRST_CENTERED);
        ITEM(_("Overview"), "", A_OVERVIEW, v.facing == VL_OVERVIEW);
        ITEM(_("Continuous"), "", A_CONTINUOUS, !settings.paged);
    } else if (m == 3) {
        ITEM(_("Previous Page"), "", A_PREV_PAGE, 0);
        ITEM(_("Next Page"), "", A_NEXT_PAGE, 0);
        ITEM(_("Beginning of the document"), "Ctrl+Home", A_FIRST, 0);
        ITEM(_("End of the document"), "Ctrl+End", A_LAST, 0);
        ITEM(_("Back"), "Alt+Shift+Left", A_BACK, 0);
        ITEM(_("Forward"), "Alt+Shift+Right", A_FORWARD, 0);
        ITEM(_("Go to Page..."), "Ctrl+G", A_GOTO, 0);
        ITEM(_("Add/Remove Bookmark"), "Ctrl+B", A_BOOKMARK, doc.open && doc.bookmark[current_page()] != NULL);
        ITEM(_("Previous Bookmark"), "", A_BOOKMARK_PREV, 0);
        ITEM(_("Next Bookmark"), "", A_BOOKMARK_NEXT, 0);
        ITEM(_("Rename Bookmark..."), "", A_BOOKMARK_RENAME, 0);
    } else if (m == 4) {
        ITEM(_("Browse"), "Ctrl+1", A_BROWSE, settings.tool == TOOL_BROWSE);
        ITEM(_("Zoom"), "Ctrl+2", A_ZOOM_TOOL, settings.tool == TOOL_ZOOM);
        ITEM(_("Area Selection"), "Ctrl+3", A_AREA_TOOL, settings.tool == TOOL_AREA);
        ITEM(_("Text Selection"), "Ctrl+4", A_TEXT_SELECT, settings.tool == TOOL_TEXT);
        ITEM(_("Table Selection"), "Ctrl+5", A_TABLE_TOOL, settings.tool == TOOL_TABLE);
        ITEM(_("Magnifier"), "Ctrl+6", A_MAGNIFIER, settings.tool == TOOL_MAGNIFIER);
    } else if (m == 5) {
        ITEM(_("Show Sidebar"), "F7", A_SIDEBAR, settings.sidebar);
    }
    pdf_menu_items(m, it, &n);
    if (m == 2) {
        ITEM(_("Presentation"), "Ctrl+Shift+P", A_PRESENTATION, 0);
        ITEM(_("Color Mode..."), "", A_COLOR_MENU, settings.invert);
        ITEM(_("Change Colors"), "", A_CHANGE_COLORS, settings.invert);
        ITEM(_("Overview Columns..."), "", A_COLUMNS_MENU, 0);
        ITEM(_("Use Right to Left Reading Direction"), "", A_RTL, settings.rtl);
    }
#undef ITEM
    return n;
}

/* A menu of items at (col, row), chosen with the keys or the pointer. */
#define MENU_ITEMS 40
static int run_menu(int col, int row, const struct item *it, int n)
{
    char lines[MENU_ITEMS][160], pad[128];
    const char *ptrs[MENU_ITEMS + 1];
    int width = 0;
    for (int k = 0; k < n; k++) {
        int w = ui_cells(it[k].label);
        if (w > width) width = w;
    }
    for (int k = 0; k < n && k < MENU_ITEMS; k++) {
        snprintf(lines[k], sizeof lines[k], "%c %s  %s", it[k].checked ? '*' : ' ',
                 ui_pad(pad, sizeof pad, it[k].label, width), it[k].keys);
        ptrs[k] = lines[k];
    }
    ptrs[n < MENU_ITEMS ? n : MENU_ITEMS] = NULL;
    if (ui.dirty) draw();
    int k = ui_menu(col, row, ptrs);
    refresh();
    return k >= 0 && k < n ? it[k].action : A_NONE;
}

static int menu_col(int m)
{
    int col = ui_cells(settings.sidebar ? _("Hide Sidebar") : _("Show Sidebar")) + 3;
    for (int k = 0; k < m; k++) col += ui_cells(menu_name(k)) + 2;
    return col;
}

static void open_menu(int m)
{
    struct item it[MENU_ITEMS];
    do_action(run_menu(menu_col(m), 2, it, menu_items(m, it)));
}

/* Okular's zoom box: the fit modes and its factors. */
static void zoom_menu(int col)
{
    char lines[18][32];
    const char *ptrs[19];
    snprintf(lines[0], sizeof lines[0], "%s", _("Fit Width"));
    snprintf(lines[1], sizeof lines[1], "%s", _("Fit Page"));
    for (int k = 0; k < 16; k++) snprintf(lines[k + 2], sizeof lines[k + 2], "%g%%", vl_zoom_values[k] * 100);
    for (int k = 0; k < 18; k++) ptrs[k] = lines[k];
    ptrs[18] = NULL;
    if (ui.dirty) draw();
    int k = ui_menu(col, 2, ptrs);
    refresh();
    if (k == 0 || k == 1) zoom_to(k == 0 ? VL_FIT_WIDTH : VL_FIT_PAGE, 1, ax + aw / 2, ay + ah / 2);
    else if (k >= 2) zoom_to(VL_FIXED, vl_zoom_values[k - 2], ax + aw / 2, ay + ah / 2);
}

/* The right button on a page: Okular's link menu, else its page menu
 * (Browse: Fit Width) or the selection's (Text Selection: Copy Text). */
static void context_menu(int X, int Y, int col, int row)
{
    struct item it[4];
    int n = 0, page;
    if (pdf_context_menu(X, Y, col, row)) return;
    ctx.link = link_at(X, Y, &ctx.page);
    if (ctx.page < 0) return;
    if (ctx.link >= 0) {
        it[n++] = (struct item){ _("Follow This Link"), "", A_FOLLOW_LINK, 0 };
        if (doc.page[ctx.page].link[ctx.link].page < 0) {
            it[n++] = (struct item){ _("Copy Link Address"), "", A_COPY_LINK, 0 };
        }
    }
    page = ctx.page;
    if (settings.tool == TOOL_TEXT) {
        for (int k = 0; k < sel.npart; k++) {
            if (sel.active && sel.part[k].page == page && sel.part[k].len) {
                it[n++] = (struct item){ _("Copy Text"), "", A_COPY_TEXT, 0 };
                break;
            }
        }
    } else if (ctx.link < 0) {
        it[n++] = (struct item){ _("Fit Width"), "", A_FIT_WIDTH, 0 };
    }
    if (n) do_action(run_menu(col, row, it, n));
}

/* Each tab's own find bar, history and text selection (each Okular tab is
 * a part of its own): kept while another tab is shown. */
struct tab_keep {
    struct line find_in;
    char find_active[256];
    int find_bar, cur_page, cur_hit;
    struct pos at[HISTORY];
    int hist_n, hist_i;
    struct sel_part *part;
    int npart;
};

static void keep_free(struct tab_keep *k)
{
    if (!k) return;
    sel_free(k->part, k->npart);
    free(k->part);
    free(k);
}

/* The shown tab's, before another is shown. */
static void keep_save(void)
{
    if (!tabs.n || !doc.open) return;
    struct tab_keep *k = calloc(1, sizeof *k);
    if (!k) return;
    k->find_in = find.in;
    snprintf(k->find_active, sizeof k->find_active, "%s", find.active);
    k->find_bar = find.bar;
    k->cur_page = find.cur_page;
    k->cur_hit = find.cur_hit;
    hist_note();
    memcpy(k->at, hist.at, sizeof k->at);
    k->hist_n = hist.n;
    k->hist_i = hist.i;
    if (sel.active && !sel.round) { /* its parts move to the tab */
        k->part = sel.part;
        k->npart = sel.npart;
        sel.part = NULL;
        sel.npart = 0;
    }
    keep_free(tabs.keep[tabs.current]);
    tabs.keep[tabs.current] = k;
}

/* Another tab's find bar: its own, or none. */
static void keep_find(void)
{
    struct tab_keep *k = tabs.n ? tabs.keep[tabs.current] : NULL;
    find_reset();
    find.typed = 0;
    if (k) {
        find.in = k->find_in;
        find.bar = k->find_bar;
    } else {
        memset(&find.in, 0, sizeof find.in);
        find.bar = 0;
    }
    if (ui.focus == FOCUS_FIND && !find.bar) ui.focus = FOCUS_DOC;
}

/* The tab's document has opened: its history and selection back. */
static void keep_restore(void)
{
    struct tab_keep *k = tabs.n ? tabs.keep[tabs.current] : NULL;
    if (!k) return;
    snprintf(find.active, sizeof find.active, "%s", k->find_active);
    find.cur_page = k->find_active[0] && k->cur_page < doc.pages ? k->cur_page : -1;
    find.cur_hit = find.cur_page >= 0 ? k->cur_hit : -1;
    if (k->hist_n > 0) {
        memcpy(hist.at, k->at, sizeof hist.at);
        hist.n = k->hist_n;
        hist.i = k->hist_i;
    }
    int ok = 1;
    for (int i = 0; i < k->npart; i++) ok &= k->part[i].page < doc.pages;
    if (ok && k->npart) {
        sel.part = k->part;
        sel.npart = k->npart;
        sel.active = 1;
        k->part = NULL;
        k->npart = 0;
    }
    keep_free(k);
    tabs.keep[tabs.current] = NULL;
}

/* A document opened from the file chooser or Open Recent: its tab if it has
 * one (Okular's SwitchToTabIfOpen), else a new tab. */
static void open_in_tab(const char *path)
{
    for (int i = 0; i < tabs.n; i++) if (!strcmp(tabs.path[i], path)) {
        if (i != tabs.current) keep_save();
        tabs.current = i;
        keep_find();
        stop_renderer(); open_document(path); return;
    }
    if (tabs.n == TABS_MAX) { toast(1, _("Close a document tab before opening another.")); return; }
    char *name = strdup(path);
    if (!name) return;
    keep_save();
    tabs.current = tabs.n;
    tabs.keep[tabs.n] = NULL;
    tabs.path[tabs.n++] = name;
    keep_find();
    stop_renderer(); open_document(path);
}

static void pick_document(void)
{
    char dir[PATH_MAX], keep[NAME_MAX + 1] = "", path[PATH_MAX];
    if (doc.path[0]) {
        snprintf(dir, sizeof dir, "%s", doc.path);
        char *slash = strrchr(dir, '/');
        snprintf(keep, sizeof keep, "%s", slash ? slash + 1 : "");
        if (slash && slash != dir) *slash = '\0';
    } else {
        struct passwd *pw = getpwuid(VIEW_USER);
        snprintf(dir, sizeof dir, "%s", pw && pw->pw_dir ? pw->pw_dir : "/");
    }
    int ok = files_pick(dir, keep, "", _("Choose a document to open."), path, sizeof path);
    refresh();
    if (ok) open_in_tab(path);
}

/* File > Open Recent (KRecentFilesAction, MaxRecentItems 10): the newest
 * first, then Clear List. */
static void recent_menu(void)
{
    char label[RECENT_MAX][200];
    struct item it[RECENT_MAX + 1];
    int n = 0;
    for (int k = 0; k < recent.n; k++) {
        char dir[PATH_MAX], name[80];
        snprintf(dir, sizeof dir, "%s", recent.path[k]);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
        fit(name, sizeof name, base_name(recent.path[k]), 40);
        snprintf(label[k], sizeof label[k], "%s  [%.60s]", name, dir[0] ? dir : "/");
        it[n++] = (struct item){ label[k], "", k + 1, 0 };
    }
    it[n++] = (struct item){ _("Clear List"), "", 100, 0 };
    int k = run_menu(menu_col(0) + 4, 3, it, n);
    if (k == 100) {
        recent_clear();
        changed();
    } else if (k >= 1 && k <= recent.n) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s", recent.path[k - 1]);
        open_in_tab(path);
    }
}

/* File > Open Containing Folder (Part::slotOpenContainingFolder): the
 * document's folder in home's file manager, as locachy-bar asks for a
 * container's folder (FILES_REQUEST), home brought to the front. */
static void open_containing_folder(void)
{
    char dir[PATH_MAX], tmp[64];
    snprintf(dir, sizeof dir, "%s", doc.path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) *slash = '\0';
    else if (slash) dir[1] = '\0';
    snprintf(tmp, sizeof tmp, "%s.%d", FILES_REQUEST, (int)getpid());
    FILE *out = strchr(dir, '\n') ? NULL : fopen(tmp, "we");
    int ok = out && fprintf(out, "%s\n", dir) > 0;
    if (out && fclose(out) != 0) ok = 0;
    if (!ok || rename(tmp, FILES_REQUEST) != 0) {
        unlink(tmp);
        /* TRANSLATORS: %.200s is the folder path; %s is the system error message. */
        toast(1, _("Cannot open the folder %.200s: %s."), dir, strerror(errno));
        return;
    }
    static const char *const home_[] = { "home", NULL };
    if (control_fd() >= 0) control_send_fields(control_fd(), home_);
    /* TRANSLATORS: %.200s is the folder path. */
    toast(0, _("%.200s shown in the file manager."), dir);
}

static void tab_switch(int index)
{
    if (index < 0 || index >= tabs.n || index == tabs.current) return;
    if (pdf_guard(A_TAB_SWITCH, index)) return;
    keep_save();
    tabs.current = index;
    keep_find();
    stop_renderer();
    open_document(tabs.path[index]);
}

static void tab_close(void)
{
    if (!tabs.n) return;
    int index = tabs.current;
    if (doc.open) state_save();
    if (tabs.nclosed == TABS_MAX) {
        free(tabs.closed[0]);
        memmove(tabs.closed, tabs.closed + 1, sizeof tabs.closed[0] * (TABS_MAX - 1));
        tabs.nclosed--;
    }
    tabs.closed[tabs.nclosed++] = tabs.path[index];
    keep_free(tabs.keep[index]);
    memmove(tabs.path + index, tabs.path + index + 1, sizeof tabs.path[0] * (size_t)(tabs.n - index - 1));
    memmove(tabs.keep + index, tabs.keep + index + 1, sizeof tabs.keep[0] * (size_t)(tabs.n - index - 1));
    tabs.n--;
    tabs.keep[tabs.n] = NULL;
    stop_renderer();
    close_document();
    if (!tabs.n) { doc.path[0] = 0; geometry(); ui.dirty = 1; return; }
    tabs.current = index < tabs.n ? index : tabs.n - 1;
    open_document(tabs.path[tabs.current]);
}

/* Presentation (Okular's View > Presentation, Ctrl+Shift+P; part/
 * presentationwidget.cpp): a page at a time on the whole screen, fit to it,
 * on black (Dlg Presentation SlidesBackgroundColor); keys and the pointer
 * as keyPressEvent, wheelEvent, mousePressEvent and mouseReleaseEvent; the
 * page's number in the top right circle for 2.5 seconds after it changes
 * (SlidesShowProgress, generateOverlay); the top bar when the pointer reaches
 * the top of the screen; B the black screen. SlidesTransition's default,
 * Replace, shows the next page at once. */
#define HIT_PRES 1500         /* + 0 previous, 1 the page, 2 next, 3 play, 4 exit */
#define PRES_ADVANCE 5        /* okular_core.kcfg SlidesAdvanceTime */

static int pres_side(void)
{
    return (int)(W / 16.0);
}

/* Page i on the screen: its zoom, size and top left. */
static double pres_zoom(int i, int *w, int *h, int *X, int *Y)
{
    float rw, rh;
    vl_rotated(v.rotate, doc.pw[i] > 0 ? doc.pw[i] : 1, doc.ph[i] > 0 ? doc.ph[i] : 1, &rw, &rh);
    double z = fmin(W / (double)rw, H / (double)rh);
    *w = (int)(rw * (float)z + .5f);
    *h = (int)(rh * (float)z + .5f);
    *X = (W - *w) / 2;
    *Y = (H - *h) / 2;
    return (float)z;
}

static void pres_go(int page)
{
    page = clampi(page, 0, doc.pages - 1);
    if (page != pres.page) {
        pres.page = page;
        if (pres.playing) pres.next_at = now() + PRES_ADVANCE;
        check_stale();
    }
    pres.overlay_until = now() + 2.5; /* the overlay again, also at either end */
    ui.dirty = 1;
}

static void pres_enter(void)
{
    if (!doc.open) return;
    memset(&pres, 0, sizeof pres);
    pres.on = 1;
    pres.page = current_page();
    pres.overlay_until = now() + 2.5;
    pres.press_link = -1;
    ui.dirty = 1;
    check_stale();
}

/* Leaving: the document where the presentation was (its page). */
static void pres_exit(void)
{
    pres.on = 0;
    if (prompt.kind) prompt_close();
    screen_fill_px(0, 0, W, H, BG);
    screen_fill(0, 0, ncols, nrows, BG); /* its top bar's row too */
    if (doc.open) go(pos_page(pres.page));
    refresh();
}

static int pres_background(struct job *j)
{
    for (int k = 0; k < 3; k++) {
        int i = pres.page + (k == 2 ? -1 : k);
        if (i < 0 || i >= doc.pages) continue;
        struct page *pg = &doc.page[i];
        if (!pg->known) {
            if (pg->asked) continue;
            *j = page_job(i);
            return 1;
        }
        int w, h, X, Y;
        float z = (float)pres_zoom(i, &w, &h, &X, &Y);
        if (!pg->failed && pix_find(2, i, v.rotate, z, -1, -1) < 0 && (k == 0 || pix_room(pix_bytes(w, h), frame_start, 0) == 0)) {
            render_job(j, J_SLIDE, i, z, -1, -1);
            return 1;
        }
        if (k == 0 && !pg->links) {
            *j = new_job(J_LINKS, VIEW_LINKS);
            j->page = j->req.page = i;
            pg->links = 1;
            return 1;
        }
    }
    return 0;
}

static int pres_wanted(const struct job *j)
{
    int w, h, X, Y;
    return pres.on && abs(j->page - pres.page) <= 1 && (float)pres_zoom(j->page, &w, &h, &X, &Y) == j->zoom;
}

/* The link under (X, Y) on the page shown: its index, or -1. */
static int pres_link_at(int X, int Y)
{
    int w, h, x0, y0, i = pres.page;
    double z = pres_zoom(i, &w, &h, &x0, &y0), px, py;
    if (X < x0 || Y < y0 || X >= x0 + w || Y >= y0 + h || doc.page[i].links != 2) return -1;
    vl_item_to_point(v.rotate, doc.pw[i], doc.ph[i], z, X - x0, Y - y0, &px, &py);
    for (int k = 0; k < doc.page[i].nlink; k++) {
        const struct view_link *l = &doc.page[i].link[k];
        if (px >= fmin(l->x0, l->x1) && px <= fmax(l->x0, l->x1) && py >= fmin(l->y0, l->y1) && py <= fmax(l->y0, l->y1)) return k;
    }
    return -1;
}

static int pres_in_overlay(int X, int Y)
{
    int side = pres_side();
    return now() < pres.overlay_until && X >= W - side - 4 && X < W - 4 && Y >= 4 && Y < 4 + side;
}

/* overlayClick: the page at the angle of the click round the circle. */
static void pres_overlay_click(int X, int Y)
{
    int side = pres_side(), x = X - (W - side - 4) - side / 2, y = side / 2 - (Y - 4);
    if (!x && !y) return;
    double angle = 0.5 + 0.5 * atan2(-x, -y) / M_PI;
    pres_go((int)(angle * (doc.pages - 1) + 0.5));
}

/* generateOverlay: a ring of the pages, those up to this one lit, and its number. */
static void pres_draw_overlay(void)
{
    if (now() >= pres.overlay_until) return;
    int side = pres_side(), x0 = W - side - 4, y0 = 4;
    if (side < 8) return;
    uint32_t *px = scratch;
    double r = side / 2.0, inner = r / 2;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            double dx = x + .5 - r, dy = y + .5 - r, d = sqrt(dx * dx + dy * dy);
            uint32_t c = 0;
            if (d <= r - 1 && d > inner) {
                double a = atan2(dx, -dy) / (2 * M_PI); /* clockwise from the top */
                if (a < 0) a += 1;
                c = a < (pres.page + 1) / (double)doc.pages ? SELECTED : 0x404850;
            } else if (d > r - 1) {
                c = 0;
            }
            px[(size_t)y * (size_t)side + (size_t)x] = c;
        }
    }
    screen_blit(x0, y0, side, side, px, side);
    char num[16];
    snprintf(num, sizeof num, "%d", pres.page + 1);
    int col = (x0 + side / 2) / cw - (int)strlen(num) / 2, row = (y0 + side / 2 - top) / ch;
    if (row >= 0 && row < nrows) screen_text(col, row, num, FG, 0);
}

static void pres_draw_bar(void)
{
    char page[32], of[32];
    screen_fill_px(0, 0, W, top + ch, BG);
    screen_fill(0, 0, ncols, 1, BG);
    int col = ui_button(0, 0, _("Previous Page"), HIT_PRES + 0);
    snprintf(page, sizeof page, " %d ", pres.page + 1);
    screen_text(col, 0, page, BG, ACCENT);
    ui_hit(col, 0, (int)strlen(page), 1, HIT_PRES + 1);
    col += (int)strlen(page);
    /* TRANSLATORS: follows the current page number in the presentation bar; %d is the total page count. */
    snprintf(of, sizeof of, " %s", _("of %d"));
    char total[48];
    snprintf(total, sizeof total, of, doc.pages);
    screen_text(col, 0, total, FG, BG);
    col += ui_cells(total) + 1;
    col = ui_button(col, 0, _("Next Page"), HIT_PRES + 2);
    col = ui_button(col + 1, 0, pres.playing ? _("Pause") : _("Play"), HIT_PRES + 3);
    const char *exit_ = _("Exit Presentation Mode");
    ui_button(ncols - ui_cells(exit_) - 3, 0, exit_, HIT_PRES + 4);
}

static void draw_presentation(void)
{
    screen_fill_px(0, 0, W, H, 0); /* SlidesBackgroundColor: black */
    if (pres.black) return;
    int w, h, X, Y, i = pres.page;
    float z = (float)pres_zoom(i, &w, &h, &X, &Y);
    novl = 0;
    if (doc.page[i].failed) put_paper(X, Y, X + w, Y + h);
    else {
        int c = pix_find(2, i, v.rotate, z, -1, -1);
        if (c >= 0) {
            cache[c].used = ++ticks;
            put_pixels(cache[c].px, cache[c].w, cache[c].h, X, Y, 0, 0, W, H);
        } else if (!put_scaled(i, X, Y, w, h, X > 0 ? X : 0, Y > 0 ? Y : 0, X + w < W ? X + w : W, Y + h < H ? Y + h : H)) {
            put_paper(X > 0 ? X : 0, Y > 0 ? Y : 0, X + w < W ? X + w : W, Y + h < H ? Y + h : H);
        }
    }
    pres_draw_overlay();
    if (pres.bar) pres_draw_bar();
}

static void pres_play(void)
{
    pres.playing = !pres.playing;
    pres.next_at = pres.playing ? now() + PRES_ADVANCE : 0;
    ui.dirty = 1;
}

static void pres_key(int key, int mods)
{
    if (key == 17 && (mods & KEYS_CTRL)) {
        do_action(A_QUIT);
        return;
    }
    /* The actions it has (setupActions): back and forward, and Ctrl+Home
     * and Ctrl+End (first_page, last_page) as Home and End. */
    if ((key == K_LEFT || key == K_RIGHT) && mods == (KEYS_ALT | KEYS_SHIFT)) {
        hist_move(key == K_LEFT ? -1 : 1);
        pres_go(current_page());
        return;
    }
    if ((mods & (KEYS_CTRL | KEYS_ALT)) && !(mods == KEYS_CTRL && (key == K_BOL || key == K_EOL))) return;
    switch (key) {
    case K_LEFT: case '\b': case K_PGUP: case K_UP: pres_go(pres.page - 1); break;
    case K_RIGHT: case ' ': case K_PGDN: case K_DOWN: pres_go(pres.page + 1); break;
    case K_BOL: pres_go(0); break;
    case K_EOL: pres_go(doc.pages - 1); break;
    case 'b': case 'B': pres.black = !pres.black; ui.dirty = 1; break; /* switch_blackscreen_mode */
    case 27:
        if (pres.bar) { pres.bar = 0; ui.dirty = 1; }
        else pres_exit();
        break;
    }
}

static void pres_pointer(const struct pointer_event *e)
{
    int bar_h = top + ch;
    if (e->wheel) { /* a step a notch, at most 3 */
        int n = clampi(abs(e->wheel), 0, 3);
        for (int k = 0; k < n; k++) pres_go(pres.page + (e->wheel > 0 ? -1 : 1));
        return;
    }
    if (!e->button) {
        if (pres.bar && e->y > bar_h + 1) { pres.bar = 0; ui.dirty = 1; }
        else if (!pres.bar && e->y <= 1) { pres.bar = 1; ui.dirty = 1; }
        else if ((e->buttons & 1) && pres_in_overlay(e->x, e->y)) pres_overlay_click(e->x, e->y);
        return;
    }
    if (e->pressed) {
        int id = pres.bar ? ui_hit_at(e->col, e->row) : -1;
        pres.press = 0;
        pres.press_link = -1;
        if (id >= HIT_PRES && id <= HIT_PRES + 4 && e->button == BTN_LEFT) {
            if (id == HIT_PRES + 0) pres_go(pres.page - 1);
            else if (id == HIT_PRES + 1) {
                char q[64];
                /* TRANSLATORS: %d is the total page count. */
                snprintf(q, sizeof q, _("Go to page (1 to %d):"), doc.pages);
                prompt_open(P_GOTO, q, "", 0);
            } else if (id == HIT_PRES + 2) pres_go(pres.page + 1);
            else if (id == HIT_PRES + 3) pres_play();
            else pres_exit();
            return;
        }
        if (e->button == BTN_LEFT) {
            if ((pres.press_link = pres_link_at(e->x, e->y)) >= 0) return;
            if (pres_in_overlay(e->x, e->y)) { pres_overlay_click(e->x, e->y); return; }
            pres.press = 1;
        } else if (e->button == BTN_RIGHT) {
            pres.press = -1;
        }
        return;
    }
    if (pres.press_link >= 0 && e->button == BTN_LEFT) {
        int k = pres.press_link, page = pres.page;
        pres.press_link = -1;
        if (pres_link_at(e->x, e->y) == k) {
            const struct view_link *l = &doc.page[page].link[k];
            int to = l->page;
            follow_link(page, k);
            if (to >= 0) pres_go(to);
        }
    }
    if (pres.press) {
        pres_go(pres.page + pres.press);
        pres.press = 0;
    }
}

/* A menu of the View menu's at its place (for its submenus). */
static int submenu(const struct item *it, int n)
{
    return run_menu(menu_col(2) + 4, 3, it, n);
}

/* Okular's ColorModeMenu::slotColorModeActionTriggered: Normal Colors turns
 * Change Colors off; the checked mode again toggles it; another mode is
 * chosen and turns it on. A new paper colour draws the pages again. */
static void color_mode(int mode)
{
    uint32_t paper = settings.invert && settings.render_mode == CM_PAPER ? settings.paper | VIEW_PAPER : 0;
    if (mode < 0) settings.invert = 0;
    else if (settings.render_mode == mode) settings.invert = !settings.invert;
    else {
        settings.render_mode = mode;
        settings.invert = 1;
    }
    if (paper != (settings.invert && settings.render_mode == CM_PAPER ? settings.paper | VIEW_PAPER : 0)) pix_flush();
    changed();
}

static const char *const color_modes[CM_COUNT] = {
    N_("Invert Colors"), N_("Change Paper Color"), N_("Change Dark & Light Colors"),
    N_("Convert to Black & White"), N_("Invert Lightness"), N_("Invert Luma (sRGB Linear)"),
    N_("Invert Luma (Symmetric)"), N_("Shift Hue Positive"), N_("Shift Hue Negative"),
};

/* Configure Color Modes (Okular's Accessibility page): its colours and the
 * black and white threshold and contrast, typed. */
static void color_config(void)
{
    char label[5][64];
    struct item it[5];
    snprintf(label[0], sizeof label[0], _("Paper Color: #%06X"), settings.paper);
    snprintf(label[1], sizeof label[1], _("Dark Color: #%06X"), settings.recolor_fg);
    snprintf(label[2], sizeof label[2], _("Light Color: #%06X"), settings.recolor_bg);
    snprintf(label[3], sizeof label[3], _("Threshold: %d"), settings.bw_threshold);
    snprintf(label[4], sizeof label[4], _("Contrast: %d"), settings.bw_contrast);
    for (int k = 0; k < 5; k++) it[k] = (struct item){ label[k], "", k + 1, 0 };
    int k = submenu(it, 5);
    if (k < 1) return;
    char value[16];
    static const char *const q[] = {
        N_("Paper color (#RRGGBB):"), N_("Dark color (#RRGGBB):"), N_("Light color (#RRGGBB):"),
        N_("Threshold (2 to 253):"), N_("Contrast (2 to 6):"),
    };
    if (k <= 3) snprintf(value, sizeof value, "#%06X", k == 1 ? settings.paper : k == 2 ? settings.recolor_fg : settings.recolor_bg);
    else snprintf(value, sizeof value, "%d", k == 4 ? settings.bw_threshold : settings.bw_contrast);
    prompt_open(P_COLOR, _(q[k - 1]), value, 0);
    prompt.then = k;
}

static void color_typed(int which, const char *text)
{
    char *end;
    const char *t = text[0] == '#' ? text + 1 : text;
    long x = strtol(t, &end, which <= 3 ? 16 : 10);
    int ok = end != t && !*end && (which <= 3 ? x >= 0 && x <= 0xFFFFFF && strlen(t) == 6 :
                                   which == 4 ? x >= 2 && x <= 253 : x >= 2 && x <= 6);
    if (!ok) {
        /* TRANSLATORS: %.100s is the invalid value typed by the user. */
        toast(1, which <= 3 ? _("%.100s is not a colour (#RRGGBB).") : _("%.100s is not a value in the range."), text);
        return;
    }
    if (which == 1) {
        settings.paper = (uint32_t)x;
        if (settings.invert && settings.render_mode == CM_PAPER) pix_flush();
    } else if (which == 2) settings.recolor_fg = (uint32_t)x;
    else if (which == 3) settings.recolor_bg = (uint32_t)x;
    else if (which == 4) settings.bw_threshold = (int)x;
    else settings.bw_contrast = (int)x;
    changed();
}

static void color_menu(void)
{
    struct item it[CM_COUNT + 2];
    int n = 0;
    it[n++] = (struct item){ _("Normal Colors"), "", 100, !settings.invert };
    for (int k = 0; k < CM_COUNT; k++) it[n++] = (struct item){ _(color_modes[k]), "", 101 + k, settings.invert && settings.render_mode == k };
    it[n++] = (struct item){ _("Configure Color Modes..."), "", 200, 0 };
    int k = submenu(it, n);
    if (k == 100) color_mode(-1);
    else if (k > 100 && k <= 100 + CM_COUNT) color_mode(k - 101);
    else if (k == 200) color_config();
}

/* Okular's ViewColumns (1 to 8) and reading direction: laid out again. */
static void columns_menu(void)
{
    static const char *const names[8] = { "1", "2", "3", "4", "5", "6", "7", "8" };
    struct item it[8];
    for (int k = 0; k < 8; k++) it[k] = (struct item){ names[k], "", k + 1, settings.columns == k + 1 };
    int k = submenu(it, 8);
    if (k < 1) return;
    settings.columns = k;
    v.current = current_page();
    relayout_middle();
    viewport_reflow();
}

static void do_action(int a)
{
    int needs_doc = a != A_NONE && a != A_OPEN && a != A_QUIT && a != A_INVERT && a != A_SIDEBAR &&
                    a != A_COLOR_MENU && a != A_CHANGE_COLORS && a != A_COLOR_NORMAL && a != A_COLOR_CONFIG &&
                    a != A_RTL && a != A_COLUMNS_MENU && a != A_RECENT && a != A_ZOOM_TOOL && a != A_AREA_TOOL &&
                    a != A_TABLE_TOOL && a != A_MAGNIFIER &&
                    a != A_BROWSE && a != A_TEXT_SELECT && a != A_FIND && a != A_REOPEN_TAB &&
                    a != A_CLOSE_TAB && a != A_NEXT_TAB && a != A_PREV_TAB;
    if (needs_doc && !doc.open) return;
    /* What closes the document asks first about its unsaved changes (Okular's queryClose). */
    if ((a == A_QUIT || a == A_RELOAD || a == A_OPEN || a == A_CLOSE_TAB || a == A_REOPEN_TAB) && pdf_guard(a, -1)) return;
    switch (a) {
    case A_OPEN: pick_document(); break;
    case A_RECENT: recent_menu(); break;
    case A_CONTAINING: open_containing_folder(); break;
    case A_CLOSE_TAB: tab_close(); break;
    case A_NEXT_TAB: if (tabs.n) tab_switch((tabs.current + 1) % tabs.n); break;
    case A_PREV_TAB: if (tabs.n) tab_switch((tabs.current + tabs.n - 1) % tabs.n); break;
    case A_REOPEN_TAB:
        if (tabs.nclosed && tabs.n < TABS_MAX) {
            tabs.keep[tabs.n] = NULL;
            tabs.path[tabs.n++] = tabs.closed[--tabs.nclosed];
            int next = tabs.n - 1;
            if (tabs.n == 1) { tabs.current = next; stop_renderer(); open_document(tabs.path[next]); }
            else tab_switch(next);
        }
        break;
    case A_RELOAD: {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s", doc.path);
        stop_renderer();
        open_document(path);
        break;
    }
    case A_BOOKMARK: {
        int p = current_page();
        if (doc.bookmark[p]) {
            free(doc.bookmark[p]);
            free(doc.bmark[p]);
            doc.bookmark[p] = doc.bmark[p] = NULL;
        } else {
            char title[64];
            /* TRANSLATORS: %d is the page number (default bookmark title). */
            snprintf(title, sizeof title, _("Page %d"), p + 1);
            doc.bookmark[p] = strdup(title);
            bookmark_mark(p);
        }
        changed();
        break;
    }
    case A_BOOKMARK_RENAME:
        if (doc.bookmark[current_page()]) {
            prompt_open(P_BOOKMARK, _("Bookmark name:"), doc.bookmark[current_page()], 0);
            prompt.then = current_page();
        }
        break;
    case A_BOOKMARK_PREV:
    case A_BOOKMARK_NEXT: {
        int dir = a == A_BOOKMARK_PREV ? -1 : 1;
        for (int p = current_page() + dir; p >= 0 && p < doc.pages; p += dir)
            if (doc.bookmark[p]) { go(pos_page(p)); break; }
        break;
    }
    case A_PROPERTIES: {
        struct job j = new_job(J_INFO, VIEW_INFO);
        push_job(&j, 0);
        break;
    }
    case A_EMBEDDED:
        if (!(doc.flags & VIEW_HAS_EMBEDDED)) {
            toast(0, _("This document has no embedded files."));
        } else {
            struct job j = new_job(J_EMBEDDED, VIEW_EMBEDDED);
            push_job(&j, 0);
        }
        break;
    case A_EXPORT: export_text(); break;
    case A_QUIT: ui.quit = 1; break;
    case A_COPY:
    case A_COPY_TEXT: copy_selection(0); break;
    case A_FIND: find_bar(1); break;
    case A_FIND_NEXT:
    case A_FIND_PREV:
        if (!find.bar || !find.in.text[0]) find_bar(1); /* Okular: the find bar, if it is not shown */
        else find_go(a == A_FIND_NEXT ? 1 : -1);
        break;
    case A_ZOOM_IN: zoom_step(1); break;
    case A_ZOOM_OUT: zoom_step(0); break;
    case A_ZOOM_100: zoom_to(VL_FIXED, 1, ax + aw / 2, ay + ah / 2); break;
    case A_FIT_WIDTH: zoom_to(VL_FIT_WIDTH, 1, ax + aw / 2, ay + ah / 2); break;
    case A_FIT_PAGE: zoom_to(VL_FIT_PAGE, 1, ax + aw / 2, ay + ah / 2); break;
    case A_FIT_AUTO: zoom_to(VL_FIT_AUTO, 1, ax + aw / 2, ay + ah / 2); break;
    case A_CONTINUOUS:
        v.current = current_page();
        settings.paged = !settings.paged;
        relayout_middle();
        break;
    case A_SINGLE:
    case A_FACING:
    case A_FIRST_CENTERED:
    case A_OVERVIEW:
        v.current = current_page();
        v.facing = settings.facing = a == A_FACING ? VL_FACING : a == A_FIRST_CENTERED ? VL_FIRST_CENTERED :
            a == A_OVERVIEW ? VL_OVERVIEW : VL_SINGLE;
        relayout_middle();
        viewport_reflow();
        break;
    case A_ROTATE_RIGHT: v.rotate = (v.rotate + 90) % 360; relayout_middle(); break;
    case A_ROTATE_LEFT: v.rotate = (v.rotate + 270) % 360; relayout_middle(); break;
    case A_ROTATE_NONE: v.rotate = 0; relayout_middle(); break;
    case A_PRESENTATION: pres_enter(); break;
    case A_INVERT: color_mode(CM_INVERTED); break; /* Okular's color_mode_inverted */
    case A_COLOR_MENU: color_menu(); break;
    case A_CHANGE_COLORS: /* Okular's color_mode_change_colors */
        if (settings.invert && settings.render_mode == CM_PAPER) pix_flush();
        settings.invert = !settings.invert;
        if (settings.invert && settings.render_mode == CM_PAPER) pix_flush();
        changed();
        break;
    case A_COLOR_NORMAL: color_mode(-1); break;
    case A_COLOR_CONFIG: color_config(); break;
    case A_COLUMNS_MENU: columns_menu(); break;
    case A_RTL:
        settings.rtl = !settings.rtl;
        relayout_middle();
        break;
    case A_FONT_UP: font_size(doc.em + 1); break;
    case A_FONT_DOWN: font_size(doc.em - 1); break;
    case A_PREV_PAGE: if (current_page() > 0) go(pos_page(current_page() - 1)); break;
    case A_NEXT_PAGE: if (current_page() < doc.pages - 1) go(pos_page(current_page() + 1)); break;
    case A_FIRST: go(pos_page(0)); break;
    case A_LAST: {
        struct pos p = pos_page(doc.pages - 1);
        p.ny = 1; /* Okular: the last page's bottom at the top, as far as it goes */
        go(p);
        break;
    }
    case A_BACK: hist_move(-1); break;
    case A_FORWARD: hist_move(1); break;
    case A_GOTO: {
        char q[64];
        /* TRANSLATORS: %d is the total page count. */
        snprintf(q, sizeof q, _("Go to page (1 to %d):"), doc.pages);
        prompt_open(P_GOTO, q, "", 0);
        break;
    }
    case A_BROWSE: tool_set(TOOL_BROWSE); break;
    case A_TEXT_SELECT: tool_set(TOOL_TEXT); break;
    case A_ZOOM_TOOL: tool_set(TOOL_ZOOM); break;
    case A_AREA_TOOL: tool_set(TOOL_AREA); break;
    case A_TABLE_TOOL: tool_set(TOOL_TABLE); break;
    case A_MAGNIFIER: tool_set(TOOL_MAGNIFIER); break;
    case A_AREA_COPY:
    case A_AREA_COPY_FLAT:
    case A_AREA_SEARCH:
    case A_AREA_IMAGE_COPY:
    case A_AREA_IMAGE_SAVE: area_action(a); break;
    case A_TABLE_COPY: table_copy(); break;
    case A_TRIM_MARGINS:
    case A_TRIM_SELECTION: {
        struct anchor at = anchor_at(ax + aw / 2, ay + ah / 2);
        if (a == A_TRIM_MARGINS) {
            settings.trim_margins = !settings.trim_margins;
            memset(&trim, 0, sizeof trim);
        } else {
            int on = !trim.selection;
            memset(&trim, 0, sizeof trim);
            trim.selection = trim.picking = on;
            settings.trim_margins = 0;
            if (on) toast(0, _("Draw a rectangle on the page to trim to selection. Escape cancels."));
        }
        sel_clear();
        pix_flush();
        layout();
        anchor_keep(&at);
        changed();
        break;
    }
    case A_SIDEBAR: sidebar(!settings.sidebar); break;
    case A_FOLLOW_LINK: follow_link(ctx.page, ctx.link); break;
    case A_COPY_LINK:
        if (ctx.page >= 0 && ctx.link >= 0 && ctx.link < doc.page[ctx.page].nlink) {
            const char *uri = doc.page[ctx.page].link[ctx.link].uri;
            clipboard_set(0, uri, strlen(uri));
            clipboard_set(1, uri, strlen(uri));
        }
        break;
    default:
        pdf_action(a);
        break;
    }
}

/* The keys every focus has (Okular's window shortcuts): 1 if key was one. */
static int key_global(int key, int mods)
{
    int ctrl = mods & KEYS_CTRL, a = A_NONE;
    if (pdf_key_global(key, mods)) return 1;
    if (key == 17 && ctrl) a = A_QUIT;
    else if (key == 15 && ctrl) a = A_OPEN;
    else if (key == 6 && ctrl) a = A_FIND;
    else if (key == 7 && ctrl) a = A_GOTO;
    else if (key == 2 && ctrl) a = A_BOOKMARK;
    else if (key == K_F1 + 4) a = A_RELOAD;
    else if (key == 23 && mods == KEYS_CTRL) a = A_CLOSE_TAB;
    else if (key == 20 && mods == (KEYS_CTRL | KEYS_SHIFT)) a = A_REOPEN_TAB;
    else if (key == '\t' && mods == KEYS_CTRL) a = A_NEXT_TAB;
    else if (key == '\t' && mods == (KEYS_CTRL | KEYS_SHIFT)) a = A_PREV_TAB;
    else if (key == 3 && mods == KEYS_CTRL) a = A_COPY;
    else if (key == K_F1 + 2) a = mods & KEYS_SHIFT ? A_FIND_PREV : A_FIND_NEXT;
    else if (key == K_F1 + 6) a = A_SIDEBAR;
    else if (key == '\n' && mods == KEYS_ALT) a = A_PROPERTIES;
    else if ((key == '+' || key == '=') && ctrl) a = A_ZOOM_IN;
    else if (key == '-' && ctrl) a = A_ZOOM_OUT;
    else if (key == K_CTRL_0) a = A_ZOOM_100;
    else if (key == K_CTRL_0 + 1) a = A_BROWSE;
    else if (key == K_CTRL_0 + 2) a = A_ZOOM_TOOL;
    else if (key == K_CTRL_0 + 3) a = A_AREA_TOOL;
    else if (key == K_CTRL_0 + 4) a = A_TEXT_SELECT;
    else if (key == K_CTRL_0 + 5) a = A_TABLE_TOOL;
    else if (key == K_CTRL_0 + 6) a = A_MAGNIFIER;
    else if (key == 16 && mods == (KEYS_CTRL | KEYS_SHIFT)) a = A_PRESENTATION;
    else if (key == K_LEFT && mods == (KEYS_ALT | KEYS_SHIFT)) a = A_BACK;
    else if (key == K_RIGHT && mods == (KEYS_ALT | KEYS_SHIFT)) a = A_FORWARD;
    else if (key == K_BOL && mods == KEYS_CTRL) a = A_FIRST;
    else if (key == K_EOL && mods == KEYS_CTRL) a = A_LAST;
    if (a != A_NONE) {
        do_action(a);
        return 1;
    }
    if (mods == KEYS_ALT && key > 0 && key < 0x80) {
        const char *keys = "fevgts", *at = strchr(keys, key);
        if (at && *at) {
            open_menu((int)(at - keys));
            return 1;
        }
    }
    return 0;
}

static void key_doc(int key, int mods)
{
    int shift = mods & KEYS_SHIFT;
    if (pdf_key_doc(key, mods)) return;
    if (mods & (KEYS_CTRL | KEYS_ALT)) return;
    switch (key) {
    case ' ': screen_step(shift ? -1 : 1); break;
    case K_PGDN: screen_step(1); break;
    case K_PGUP:
    case '\b': screen_step(-1); break;
    case K_DOWN: if (!shift) step(1); break; /* Shift+Down: Okular's auto scroll, not here */
    case K_UP: if (!shift) step(-1); break;
    case 'j': case 'J': step(key == 'J' ? 10 : 1); break;
    case 'k': case 'K': step(key == 'K' ? -10 : -1); break;
    case K_LEFT: case 'h': case 'H': sideways(-1, key == 'H' ? 10 : 1); break;
    case K_RIGHT: case 'l': case 'L': sideways(1, key == 'L' ? 10 : 1); break;
    case '<': do_action(A_FONT_DOWN); break;
    case '>': do_action(A_FONT_UP); break;
    case '/': do_action(A_FIND); break;
    case '\t':
        if (side_cols && doc.open) {
            ui.focus = FOCUS_SIDEBAR;
            ui.dirty = 1;
        }
        break;
    case 27:
        if (trim.picking) do_action(A_TRIM_SELECTION);
        else if (find.bar) find_bar(0);
        else if (table.on && table.guessed) { /* Okular: the guessed dividers first, then the table */
            table.ncols = table.nrows = table.guessed = 0;
            ui.dirty = 1;
        } else if (table.on) table_clear();
        else if (sel.active || sel.dragging) sel_clear();
        break;
    }
}

static void thumb_into_sight(void)
{
    int at, y0 = thumbs_y0(), y1 = top + (side_last() + 1) * ch;
    thumb_seen(ui.thumb_sel, &at);
    if (at < ui.thumb_top) ui.thumb_top = at;
    else if (at + thumb_entry(ui.thumb_sel) > ui.thumb_top + (y1 - y0)) {
        ui.thumb_top = at + thumb_entry(ui.thumb_sel) - (y1 - y0);
    }
}

static void side_go(void)
{
    if (ui.tab == TAB_CONTENTS && ui.toc_sel < doc.noutline) {
        const struct view_outline *o = &doc.outline[ui.toc_sel];
        if (o->page >= 0 && o->page < doc.pages) go(pos_point(o->page, 0, o->y));
    } else if (ui.tab == TAB_THUMBNAILS) {
        go(pos_page(ui.thumb_sel));
    }
}

/* The page of the bookmarks panel's row k, or -1. */
static int bookmark_row_page(int k)
{
    for (int i = 0; i < doc.pages; i++) if (doc.bookmark[i] && k-- == 0) return i;
    return -1;
}

static void bookmark_remove(int page)
{
    if (page < 0 || page >= doc.pages || !doc.bookmark[page]) return;
    free(doc.bookmark[page]);
    free(doc.bmark[page]);
    doc.bookmark[page] = doc.bmark[page] = NULL;
    changed();
}

/* Okular's bookmark list (part/bookmarklist.cpp): Up and Down through it,
 * Enter goes to one (itemActivated); Delete removes it, as its menu's
 * Remove Bookmark does. */
static void key_bookmarks(int key)
{
    int n = 0, rows = side_last() - 3;
    for (int i = 0; i < doc.pages; i++) n += doc.bookmark[i] != NULL;
    if (key == K_UP) ui.bm_sel--;
    else if (key == K_DOWN) ui.bm_sel++;
    else if (key == K_PGUP) ui.bm_sel -= rows;
    else if (key == K_PGDN) ui.bm_sel += rows;
    else if (key == K_BOL) ui.bm_sel = 0;
    else if (key == K_EOL) ui.bm_sel = n - 1;
    else if (key == '\n') {
        int p = bookmark_row_page(ui.bm_sel);
        if (p >= 0) go(pos_page(p));
    } else if (key == K_DEL) {
        bookmark_remove(bookmark_row_page(ui.bm_sel));
        n--;
    } else if (key == '\t' || key == 27) ui.focus = FOCUS_DOC;
    ui.bm_sel = clampi(ui.bm_sel, 0, n > 0 ? n - 1 : 0);
    ui.dirty = 1;
}

/* Its menu (bookmarklist.cpp contextMenuForBookmarkItem). */
static void bookmark_menu(int page, int col, int row)
{
    struct item it[2] = { { _("Rename Bookmark"), "", 1, 0 }, { _("Remove Bookmark"), "", 2, 0 } };
    int k = run_menu(col, row, it, 2);
    if (k == 1 && page >= 0 && page < doc.pages && doc.bookmark[page]) {
        prompt_open(P_BOOKMARK, _("Bookmark name:"), doc.bookmark[page], 0);
        prompt.then = page;
    } else if (k == 2) {
        bookmark_remove(page);
    }
}

static void key_sidebar(int key, int mods)
{
    if (mods & (KEYS_CTRL | KEYS_ALT)) return;
    if (pdf_key_sidebar(key, mods)) return;
    if (ui.tab == TAB_BOOKMARKS) {
        key_bookmarks(key);
        return;
    }
    int contents = ui.tab == TAB_CONTENTS;
    int n = contents ? doc.noutline : doc.pages, *sel_at = contents ? &ui.toc_sel : &ui.thumb_sel;
    int page = side_last() - 3;
    if (key == K_UP) *sel_at -= 1;
    else if (key == K_DOWN) *sel_at += 1;
    else if (key == K_PGUP) *sel_at -= ui.tab == TAB_CONTENTS ? page : 3;
    else if (key == K_PGDN) *sel_at += ui.tab == TAB_CONTENTS ? page : 3;
    else if (key == K_BOL) *sel_at = 0;
    else if (key == K_EOL) *sel_at = n - 1;
    else if (key == '\n') side_go();
    else if ((key == K_LEFT || key == K_RIGHT) && doc.noutline) ui.tab = key == K_LEFT ? TAB_CONTENTS : TAB_THUMBNAILS;
    else if (key == '\t' || key == 27) ui.focus = FOCUS_DOC;
    *sel_at = clampi(*sel_at, 0, n > 0 ? n - 1 : 0);
    if (ui.tab == TAB_THUMBNAILS && doc.pages) thumb_into_sight();
    ui.dirty = 1;
}

static size_t utf_prev(const char *s, size_t p)
{
    if (p) do { p--; } while (p && ((unsigned char)s[p] & 0xC0) == 0x80);
    return p;
}

static size_t utf_next(const char *s, size_t p)
{
    if (s[p]) do { p++; } while (s[p] && ((unsigned char)s[p] & 0xC0) == 0x80);
    return p;
}

/* The line editor: what is typed or pasted goes in whole UTF-8 characters,
 * no control characters (but new lines where they may be), within its
 * bytes and at most its characters (what goes over is not taken). */
static void line_insert(struct line *l, const char *s, size_t len)
{
    size_t n = strlen(l->text), lo = l->cursor < l->mark ? l->cursor : l->mark;
    size_t hi = l->cursor > l->mark ? l->cursor : l->mark;
    size_t room = l->limit && l->limit < sizeof l->text ? l->limit : sizeof l->text - 1;
    if (l->replace) { lo = 0; hi = n; }
    int count = 0;
    for (size_t i = 0; i < n; i++) count += (i < lo || i >= hi) && ((unsigned char)l->text[i] & 0xC0) != 0x80;
    char clean[sizeof l->text];
    size_t k = 0;
    for (size_t i = 0; i < len;) {
        unsigned char b = (unsigned char)s[i];
        size_t bytes = b < 128 ? 1 : b >= 0xC2 && b < 0xE0 ? 2 : b < 0xF0 && b >= 0xE0 ? 3 : b >= 0xF0 && b <= 0xF4 ? 4 : 0;
        if (!bytes || i + bytes > len) break;
        int valid = 1;
        for (size_t j = 1; j < bytes; j++) valid &= ((unsigned char)s[i + j] & 0xC0) == 0x80;
        if (bytes == 3 && ((b == 0xE0 && (unsigned char)s[i + 1] < 0xA0) ||
            (b == 0xED && (unsigned char)s[i + 1] >= 0xA0))) valid = 0;
        if (bytes == 4 && ((b == 0xF0 && (unsigned char)s[i + 1] < 0x90) ||
            (b == 0xF4 && (unsigned char)s[i + 1] >= 0x90))) valid = 0;
        if (!valid) break;
        if ((b >= 32 && b != 127) || (b == '\n' && l->multiline)) {
            if ((l->max && count >= l->max) || n - (hi - lo) + k + bytes > room) break;
            memcpy(clean + k, s + i, bytes);
            k += bytes;
            count++;
        }
        i += bytes;
    }
    memmove(l->text + lo + k, l->text + hi, n - hi + 1);
    memcpy(l->text + lo, clean, k);
    l->cursor = l->mark = lo + k;
    l->replace = 0;
    ui.dirty = 1;
}

/* Its text, all of it selected (as Okular's line edits are when they open). */
static void line_set(struct line *l, const char *text, int all)
{
    l->text[0] = '\0';
    l->cursor = l->mark = l->left = 0;
    l->replace = 0;
    line_insert(l, text ? text : "", text ? strlen(text) : 0);
    l->replace = all && l->text[0];
    l->mark = all ? 0 : l->cursor;
}

static void line_selection(const struct line *l, size_t *lo, size_t *hi)
{
    *lo = l->cursor < l->mark ? l->cursor : l->mark;
    *hi = l->cursor > l->mark ? l->cursor : l->mark;
    if (l->replace) { *lo = 0; *hi = strlen(l->text); }
}

static void line_paste(struct line *l, int primary)
{
    int fd = -1;
    size_t size = 0;
    if (clipboard_get(primary, &fd, &size) != CLIPBOARD_OK) return;
    char text[sizeof l->text];
    ssize_t n = read(fd, text, size < sizeof text ? size : sizeof text);
    close(fd);
    if (n > 0) line_insert(l, text, (size_t)n);
}

/* The editing keys: 2 if the text changed, 1 if the key was one of them, 0 if not. */
static int line_key(struct line *l, int key, int mods)
{
    size_t n = strlen(l->text), lo, hi;
    line_selection(l, &lo, &hi);
    if (key == 1 && mods == KEYS_CTRL) { l->mark = 0; l->cursor = n; l->replace = 1; return 1; }
    if ((key == 3 || key == 24) && mods == KEYS_CTRL) {
        /* A password is not copied (QLineEdit's Password echo mode). */
        if (l->masked || hi <= lo) return 1;
        int status = clipboard_set(0, l->text + lo, hi - lo);
        if (status != CLIPBOARD_OK) toast(1, "%s", clipboard_error(status));
        if (key == 3) return 1;
        line_insert(l, "", 0);
        return 2;
    }
    if (key == 22 && mods == KEYS_CTRL) { line_paste(l, 0); return 2; }
    const char *ime_notice;
    if (key == K_CTRL_SPACE || key == K_ACT_IME_CN || key == K_ACT_IME_RAW || key == K_ACT_IME_WIDTH) {
        if (!l->masked && ime_action(key, &ime_notice) && key == K_ACT_IME_WIDTH && ime_notice) {
            toast(0, "%s", ime_notice);
        }
        return 1;
    }
    const char *typed = keys_typed(key);
    if (typed && !(mods & (KEYS_CTRL | KEYS_ALT))) {
        line_insert(l, typed, strlen(typed));
        return 2;
    }
    if (key == '\b' || key == K_DEL) {
        if (!l->replace && l->cursor == l->mark) {
            if (key == '\b') l->mark = utf_prev(l->text, l->cursor);
            else l->mark = utf_next(l->text, l->cursor);
        }
        line_insert(l, "", 0);
        return 2;
    }
    if (key == K_LEFT || key == K_RIGHT || key == K_BOL || key == K_EOL) {
        size_t to = key == K_BOL ? 0 : key == K_EOL ? n : key == K_LEFT ? utf_prev(l->text, l->cursor) : utf_next(l->text, l->cursor);
        if (!(mods & KEYS_SHIFT)) l->mark = to;
        l->cursor = to;
        l->replace = 0;
        return 1;
    }
    return 0;
}

static size_t line_at_cell(const struct line *l, int col)
{
    int cell = clampi(col - l->col, 0, l->width);
    size_t at = l->left;
    while (l->text[at] && utf_cells(l->text, l->left, utf_next(l->text, at)) <= cell) at = utf_next(l->text, at);
    return at;
}

/* Drawn at (col, row), width cells: the text (masked: a '*' a character),
 * scrolled to the insertion point; with the focus that and the selection. */
static void line_draw(struct line *l, int col, int row, int width, int focused, uint32_t fg, uint32_t bg)
{
    char shown[sizeof l->text + 8], masked[sizeof l->text];
    const char *t = l->text;
    if (l->masked) {
        size_t k = 0;
        for (const char *c = l->text; *c && k + 1 < sizeof masked; c++) if (((unsigned char)*c & 0xC0) != 0x80) masked[k++] = '*';
        masked[k] = '\0';
        t = masked;
    }
    /* Byte offsets of the masked text: one a character. */
    size_t cur = l->cursor, left = l->left, lo, hi;
    line_selection(l, &lo, &hi);
    if (l->masked) {
        size_t c[4] = { cur, left, lo, hi };
        for (int k = 0; k < 4; k++) {
            size_t m = 0;
            for (size_t i = 0; i < c[k]; i++) m += ((unsigned char)l->text[i] & 0xC0) != 0x80;
            c[k] = m;
        }
        cur = c[0]; left = c[1]; lo = c[2]; hi = c[3];
    }
    l->col = col;
    l->width = width;
    if (left > cur) left = cur;
    while (utf_cells(t, left, cur) >= width - 1 && left < cur) left = utf_next(t, left);
    if (l->masked) {
        size_t b = 0;
        for (size_t m = 0; m < left && l->text[b]; m++) b = utf_next(l->text, b);
        l->left = b;
    } else {
        l->left = left;
    }
    fit(shown, sizeof shown, t + left, width);
    screen_fill(col, row, width, 1, bg);
    screen_text(col, row, shown, fg, bg);
    if (!focused) return;
    screen_fill_px((col + utf_cells(t, left, cur)) * cw, top + row * ch, 1, ch, ACCENT);
    if (hi > left && hi > lo) {
        if (lo < left) lo = left;
        char selected[sizeof l->text];
        snprintf(selected, sizeof selected, "%.*s", (int)(hi - lo), t + lo);
        fit(shown, sizeof shown, selected, width - utf_cells(t, left, lo));
        screen_text(col + utf_cells(t, left, lo), row, shown, BG, ACCENT);
    }
}

static void find_insert(const char *s, size_t len)
{
    line_insert(&find.in, s, len);
    find.failed = 0;
    find.typed = now();
}

static void key_find(int key, int mods)
{
    int done = line_key(&find.in, key, mods);
    if (done == 2) {
        find.failed = 0;
        find.typed = now();
    } else if (done) {
        /* moved, selected or copied */
    } else if (key == '\n') {
        find.in.replace = 0;
        find_go(mods & KEYS_SHIFT ? -1 : 1);
    } else if (key == 27) find_bar(0);
    else if (key == K_PGUP || key == K_PGDN) screen_step(key == K_PGDN ? 1 : -1);
    else if (key == '\t') ui.focus = FOCUS_DOC;
    ui.dirty = 1;
}

/* A question with a typed answer (the go to page, password, file name and
 * bookmark questions, and a PDF's texts): the same editor as the find bar. */
static int prompt_typed(void)
{
    return prompt.kind != P_NONE && prompt.kind != P_OVERWRITE && prompt.kind != P_PDF_UNSAVED &&
           prompt.kind != P_PDF_CONSENT;
}

static void key_prompt(int key, int mods)
{
    if (pdf_key_prompt(key, mods)) {
        ui.dirty = 1;
        return;
    }
    if (prompt.kind == P_OVERWRITE) {
        int then = prompt.then;
        prompt_close();
        if (key == 'y' || key == 'Y') {
            if (then == P_EXPORT) start_export();
            else if (then == P_IMAGE) save_image();
            else start_save();
        } else {
            toast(0, _("Not saved."));
        }
    } else if (key == '\n') {
        prompt_done(1);
    } else if (key == 27) {
        prompt_done(0);
    } else {
        line_key(&prompt.in, key, mods);
    }
    ui.dirty = 1;
}

static void key_list(int key, int mods)
{
    (void)mods;
    int rows = side_last() - 1 - 5;
    if (key == K_UP) list.sel--;
    else if (key == K_DOWN) list.sel++;
    else if (key == K_PGUP) list.sel -= rows;
    else if (key == K_PGDN) list.sel += rows;
    else if (key == K_BOL) list.sel = 0;
    else if (key == K_EOL) list.sel = list.n - 1;
    else if (key == '\n' && list.kind == L_EMBEDDED) save_embedded();
    else if (key == '\n' || key == 27 || key == 'q') list_close();
    list.sel = clampi(list.sel, 0, list.n > 0 ? list.n - 1 : 0);
    ui.dirty = 1;
}

/* The page and point under screen (X, Y): -1 if on none (nearest: the nearest). */
static int page_point(int X, int Y, int nearest, double *px, double *py)
{
    struct vl_params p = params();
    return doc.open ? vl_screen_to_page(&p, doc.pages, doc.item, doc.pw, doc.ph, ax, ay, v.sx, v.sy, X, Y, nearest,
                                        px, py) : -1;
}

static void set_cursor(int X, int Y)
{
    double px, py;
    int in_area = X >= ax && X < ax + aw && Y >= ay && Y < ay + ah;
    int shape = settings.tool == TOOL_TEXT && in_area && list.kind == L_NONE && page_point(X, Y, 0, &px, &py) >= 0 ?
                CURSOR_TEXT : CURSOR_ARROW;
    if (shape != ui.cursor) {
        ui.cursor = shape;
        screen_cursor(shape);
    }
}

static void words_clear(void)
{
    for (int k = 0; k < words.n; k++) free(words.w[k].text);
    free(words.w);
    free(words.text);
    memset(&words, 0, sizeof words);
}

static void table_clear(void)
{
    if (table.on) ui.dirty = 1;
    memset(&table, 0, sizeof table);
}

/* A word's box in the content's pixels. */
static void word_box(const struct word *w, int *x0, int *y0, int *x1, int *y1)
{
    const struct vl_item *it = &doc.item[w->page];
    double ax_, ay_, bx_, by_;
    vl_point_to_item(v.rotate, doc.pw[w->page], doc.ph[w->page], it->zoom, w->x0, w->y0, &ax_, &ay_);
    vl_point_to_item(v.rotate, doc.pw[w->page], doc.ph[w->page], it->zoom, w->x1, w->y1, &bx_, &by_);
    *x0 = it->x - it->ox + (int)floor(fmin(ax_, bx_));
    *x1 = it->x - it->ox + (int)ceil(fmax(ax_, bx_));
    *y0 = it->y - it->oy + (int)floor(fmin(ay_, by_));
    *y1 = it->y - it->oy + (int)ceil(fmax(ay_, by_));
}

/* Ask for the words of every visible page the rectangle meets: how many asked. */
static int words_ask(int purpose, int x0, int y0, int x1, int y1)
{
    words_clear();
    words.purpose = purpose;
    words.x0 = x0;
    words.y0 = y0;
    words.x1 = x1;
    words.y1 = y1;
    for (int i = 0; i < doc.pages; i++) {
        const struct vl_item *it = &doc.item[i];
        if (it->hidden || !doc.page[i].text || it->x >= x1 || it->x + it->w <= x0 || it->y >= y1 || it->y + it->h <= y0) continue;
        double ax_, ay_, bx_, by_;
        int lx0 = (x0 > it->x ? x0 : it->x) - it->x + it->ox, ly0 = (y0 > it->y ? y0 : it->y) - it->y + it->oy;
        int lx1 = (x1 < it->x + it->w ? x1 : it->x + it->w) - it->x + it->ox, ly1 = (y1 < it->y + it->h ? y1 : it->y + it->h) - it->y + it->oy;
        vl_item_to_point(v.rotate, doc.pw[i], doc.ph[i], it->zoom, lx0, ly0, &ax_, &ay_);
        vl_item_to_point(v.rotate, doc.pw[i], doc.ph[i], it->zoom, lx1, ly1, &bx_, &by_);
        struct job j = new_job(J_WORDS, VIEW_WORDS);
        j.page = j.req.page = i;
        j.req.ax = (float)fmin(ax_, bx_);
        j.req.ay = (float)fmin(ay_, by_);
        j.req.bx = (float)fmax(ax_, bx_);
        j.req.by = (float)fmax(ay_, by_);
        push_job(&j, 0);
        words.pending++;
    }
    return words.pending;
}

static void area_menu(void);
static void table_ready(void);

static void got_words(const struct job *j, const struct view_reply *rep, const void *data, size_t size)
{
    if (j->gen != doc.gen || !words.pending) return;
    size_t len = 0;
    const char *t = rep->status == 0 ? reply_text(rep, data, size, &len) : NULL;
    const struct view_rect *r = data;
    for (int k = 0; t && k < rep->count; k++) {
        size_t n = strnlen(t, len);
        if (n == len) break;
        if (words.n == words.size) {
            struct word *w = realloc(words.w, sizeof *w * (size_t)(words.size + 64));
            if (!w) break;
            words.w = w;
            words.size += 64;
        }
        words.w[words.n++] = (struct word){ j->page, r[k].hit, r[k].x0, r[k].y0, r[k].x1, r[k].y1, strndup(t, n) };
        t += n + 1;
        len -= n + 1;
    }
    if (--words.pending) return;
    if (words.purpose == 1) area_menu();
    else table_ready();
}

/* The text of the words in a rectangle (content pixels): central, Okular's
 * CentralPixelTextAreaInclusionBehaviour (Table), else any of their pixels. */
static char *words_text(int x0, int y0, int x1, int y1, int central, int lines)
{
    size_t total = 1;
    for (int k = 0; k < words.n; k++) total += strlen(words.w[k].text ? words.w[k].text : "") + 1;
    char *out = malloc(total);
    if (!out) return NULL;
    size_t n = 0;
    const struct word *last = NULL;
    for (int k = 0; k < words.n; k++) {
        const struct word *w = &words.w[k];
        int bx0, by0, bx1, by1;
        word_box(w, &bx0, &by0, &bx1, &by1);
        int in = central ? (bx0 + bx1) / 2 >= x0 && (bx0 + bx1) / 2 < x1 && (by0 + by1) / 2 >= y0 && (by0 + by1) / 2 < y1
                         : bx0 < x1 && bx1 > x0 && by0 < y1 && by1 > y0;
        if (!in || !w->text) continue;
        if (last) out[n++] = lines && (last->page != w->page || last->line != w->line) ? '\n' : ' ';
        memcpy(out + n, w->text, strlen(w->text));
        n += strlen(w->text);
        last = w;
    }
    out[n] = '\0';
    return out;
}

/* Okular::removeLineBreaks: a hyphen at a line's end joins the word, a
 * single line break is a space, runs of spaces one; trimmed. */
static void remove_line_breaks(char *s)
{
    size_t n = 0;
    for (size_t i = 0; s[i];) {
        if (s[i] == '-' && s[i + 1] == '\n') {
            i += 2;
            while (s[i] == ' ' || s[i] == '\t') i++;
            continue;
        }
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\n') && s[i + 1] != '\n') {
            s[n++] = ' ';
            i++;
            while (s[i] == ' ' || s[i] == '\t') i++;
            continue;
        }
        if ((s[i] == ' ' || s[i] == '\t') && n && s[n - 1] == ' ') {
            i++;
            continue;
        }
        s[n++] = s[i++];
    }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n')) n--;
    s[n] = '\0';
    size_t lead = strspn(s, " \n");
    memmove(s, s + lead, n - lead + 1);
}

static void copy_both(const char *text)
{
    int status = clipboard_set(0, text, strlen(text));
    if (status == CLIPBOARD_OK) status = clipboard_set(1, text, strlen(text));
    if (status != CLIPBOARD_OK) toast(1, "%s", clipboard_error(status));
}

/* The rectangle's picture as drawn (the document only, Okular's
 * drawDocumentOnPainter): drawn again, what the screen gets kept. */
static int capture_area(int x0, int y0, int x1, int y1)
{
    int X0 = ax - v.sx + x0, Y0 = ay - v.sy + y0, X1 = ax - v.sx + x1, Y1 = ay - v.sy + y1;
    if (!clip_area(&X0, &Y0, &X1, &Y1)) return -1;
    free(capture.px);
    capture.px = malloc(sizeof *capture.px * (size_t)(X1 - X0) * (size_t)(Y1 - Y0));
    if (!capture.px) return -1;
    capture.x0 = X0;
    capture.y0 = Y0;
    capture.w = X1 - X0;
    capture.h = Y1 - Y0;
    for (size_t k = 0; k < (size_t)capture.w * (size_t)capture.h; k++) capture.px[k] = DESK;
    capture.on = 1;
    draw();
    capture.on = 0;
    return 0;
}

static uint32_t crc32_png(uint32_t c, const unsigned char *p, size_t n)
{
    c = ~c;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static int png_chunk(FILE *f, const char *type, const unsigned char *data, size_t n)
{
    unsigned char len[4] = { (unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n };
    uint32_t c = crc32_png(crc32_png(0, (const unsigned char *)type, 4), data, n);
    unsigned char crc[4] = { (unsigned char)(c >> 24), (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
    return fwrite(len, 1, 4, f) == 4 && fwrite(type, 1, 4, f) == 4 && (!n || fwrite(data, 1, n, f) == n) &&
           fwrite(crc, 1, 4, f) == 4;
}

/* The captured picture as a PNG (Okular's default type): RGB rows in
 * stored (uncompressed) deflate blocks. */
static int write_png(FILE *f)
{
    size_t row = 1 + 3 * (size_t)capture.w, raw = row * (size_t)capture.h;
    size_t size = 2 + raw + 5 * (raw / 65535 + 1) + 4;
    unsigned char *z = malloc(size), *line = malloc(row);
    if (!z || !line) {
        free(z);
        free(line);
        return 0;
    }
    size_t at = 0, done = 0;
    uint32_t a = 1, b = 0;
    z[at++] = 0x78;
    z[at++] = 0x01;
    for (int y = 0; y < capture.h; y++) {
        line[0] = 0;
        for (int x = 0; x < capture.w; x++) {
            uint32_t p = capture.px[(size_t)y * (size_t)capture.w + (size_t)x];
            line[1 + 3 * x] = (unsigned char)(p >> 16);
            line[2 + 3 * x] = (unsigned char)(p >> 8);
            line[3 + 3 * x] = (unsigned char)p;
        }
        for (size_t i = 0; i < row; i++) {
            if (done % 65535 == 0) {
                size_t left = raw - done < 65535 ? raw - done : 65535;
                z[at++] = left == raw - done ? 1 : 0;
                z[at++] = (unsigned char)left;
                z[at++] = (unsigned char)(left >> 8);
                z[at++] = (unsigned char)~left;
                z[at++] = (unsigned char)(~left >> 8);
            }
            z[at++] = line[i];
            a = (a + line[i]) % 65521;
            b = (b + a) % 65521;
            done++;
        }
    }
    uint32_t adler = b << 16 | a;
    z[at++] = (unsigned char)(adler >> 24);
    z[at++] = (unsigned char)(adler >> 16);
    z[at++] = (unsigned char)(adler >> 8);
    z[at++] = (unsigned char)adler;
    unsigned char ihdr[13] = { (unsigned char)(capture.w >> 24), (unsigned char)(capture.w >> 16),
        (unsigned char)(capture.w >> 8), (unsigned char)capture.w, (unsigned char)(capture.h >> 24),
        (unsigned char)(capture.h >> 16), (unsigned char)(capture.h >> 8), (unsigned char)capture.h, 8, 2, 0, 0, 0 };
    int ok = fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && png_chunk(f, "IHDR", ihdr, 13) &&
             png_chunk(f, "IDAT", z, at) && png_chunk(f, "IEND", NULL, 0);
    free(z);
    free(line);
    return ok;
}

static void save_image(void)
{
    as_user(1);
    int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f && fd >= 0) close(fd);
    int ok = f && write_png(f);
    if (f && fclose(f) != 0) ok = 0;
    as_user(0);
    /* TRANSLATORS: the two %d values are the image width and height in pixels. */
    if (ok) toast(0, _("Image [%d x %d] saved to PNG file."), capture.w, capture.h);
    /* TRANSLATORS: %.200s is the file path; %s is the system error message. */
    else toast(1, _("File not saved: %.200s: %s."), target, strerror(errno));
}

/* The name typed for the picture: into the chosen folder, asking before
 * writing over a file (as Okular's file dialog does). */
static void image_named(void)
{
    const char *name = prompt.in.text;
    if (!name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") ||
        snprintf(target, sizeof target, "%s/%s", chosen_dir, name) >= (int)sizeof target) {
        prompt_close();
        /* TRANSLATORS: %.200s is the invalid file name typed by the user. */
        toast(1, _("File not saved: %.200s cannot be a file's name here."), name);
        return;
    }
    struct stat st;
    as_user(1);
    int exists = lstat(target, &st) == 0;
    as_user(0);
    prompt_close();
    if (exists) {
        char q[600];
        /* TRANSLATORS: %.300s is the file name; y and N are keys; keep y/N. */
        snprintf(q, sizeof q, _("A file named %.300s already exists. Overwrite it? y/N"), name);
        prompt_open(P_OVERWRITE, q, "", 0);
        prompt.then = P_IMAGE;
        return;
    }
    save_image();
}

/* Okular's popup after an Area Selection: the text (if the document has
 * some) to the clipboard, with or without line breaks, or searched for; the
 * picture to the clipboard or a file. */
static void area_menu(void)
{
    char title_text[64], title_image[64], search[160];
    struct item it[8];
    int n = 0, w = words.x1 - words.x0, h = words.y1 - words.y0;
    free(words.text);
    words.text = words_text(words.x0, words.y0, words.x1, words.y1, 0, 1);
    int chars = words.text ? utf8_count(words.text) : 0;
    if (chars) {
        snprintf(title_text, sizeof title_text, P_("Text (1 character)", "Text (%d characters)", (unsigned long)chars), chars);
        it[n++] = (struct item){ title_text, "", A_NONE, 0 };
        it[n++] = (struct item){ _("Copy to Clipboard"), "", A_AREA_COPY, 0 };
        it[n++] = (struct item){ _("Copy Text (Without line breaks)"), "", A_AREA_COPY_FLAT, 0 };
        char shown[96];
        fit(shown, sizeof shown, words.text, 21); /* searchTextPreviewLength */
        /* TRANSLATORS: %s is the selected text preview. */
        snprintf(search, sizeof search, chars > 21 ? _("Search for '%s...' in this document") :
                 _("Search for '%s' in this document"), shown);
        it[n++] = (struct item){ search, "", A_AREA_SEARCH, 0 };
    }
    /* TRANSLATORS: the two %d values are the image width and height in pixels. */
    snprintf(title_image, sizeof title_image, _("Image (%d x %d pixels)"), w, h);
    it[n++] = (struct item){ title_image, "", A_NONE, 0 };
    it[n++] = (struct item){ _("Copy to Clipboard"), "", A_AREA_IMAGE_COPY, 0 };
    it[n++] = (struct item){ _("Save to File..."), "", A_AREA_IMAGE_SAVE, 0 };
    rsel.dragging = 0;
    int a = run_menu((ptr.x + cw - 1) / cw, (ptr.y - top) / ch + 1, it, n);
    if (rsel.prev_tool >= 0) { /* came from Browse's right button: back to it */
        settings.tool = rsel.prev_tool;
        rsel.prev_tool = -1;
    }
    do_action(a);
    ui.dirty = 1;
}

static void area_action(int a)
{
    if (a == A_AREA_COPY || a == A_AREA_COPY_FLAT) {
        if (!words.text) return;
        if (a == A_AREA_COPY_FLAT) remove_line_breaks(words.text);
        copy_both(words.text);
    } else if (a == A_AREA_SEARCH) {
        if (!words.text) return;
        for (char *c = words.text; *c; c++) if (*c == '\n') *c = ' ';
        find_bar(1);
        line_set(&find.in, words.text, 0);
        find_go(1);
    } else if (a == A_AREA_IMAGE_COPY) {
        /* LOCachy's clipboard (base/clipboard.c) carries text only. */
        /* TRANSLATORS: the two %d values are the image width and height in pixels. */
        toast(1, _("The clipboard takes text only: the image [%d x %d] was not copied."), words.x1 - words.x0, words.y1 - words.y0);
    } else if (a == A_AREA_IMAGE_SAVE) {
        if (capture_area(words.x0, words.y0, words.x1, words.y1) < 0) return;
        /* TRANSLATORS: o is the key in the file manager; keep it. */
        if (!choose_folder(_("Choose the folder to save the image into (o on it)."))) {
            toast(1, _("File not saved."));
            return;
        }
        prompt_open(P_IMAGE, _("Save the image as:"), "selection.png", 0);
    }
}

/* Okular's guessTableDividers: a divider in each gap between the words
 * (and between the rectangle's parts), across and down. */
struct tick { double at; int d; };

static int by_tick(const void *a, const void *b)
{
    const struct tick *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at ? 1 : x->d - y->d;
}

static void guess_dividers(void)
{
    int n = 2 * words.n + 4 * table.nparts + 4;
    struct tick *c = calloc((size_t)n, sizeof *c), *r = calloc((size_t)n, sizeof *r);
    struct tick *cs = calloc((size_t)n, sizeof *cs), *rs = calloc((size_t)n, sizeof *rs);
    double W_ = table.x1 - table.x0, H_ = table.y1 - table.y0;
    int nc = 0, nr = 0, ncs = 0, nrs = 0;
    table.ncols = table.nrows = 0;
    if (!c || !r || !cs || !rs) goto done;
    for (int k = 0; k < table.nparts; k++) {
        cs[ncs++] = (struct tick){ (table.part[k][0] - table.x0) / W_, +1 };
        cs[ncs++] = (struct tick){ (table.part[k][2] - table.x0) / W_, -1 };
        rs[nrs++] = (struct tick){ (table.part[k][1] - table.y0) / H_, +1 };
        rs[nrs++] = (struct tick){ (table.part[k][3] - table.y0) / H_, -1 };
    }
    for (int k = 0; k < words.n; k++) {
        int x0, y0, x1, y1;
        word_box(&words.w[k], &x0, &y0, &x1, &y1);
        int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        if (cx < table.x0 || cx >= table.x1 || cy < table.y0 || cy >= table.y1 || !words.w[k].text || !words.w[k].text[0]) continue;
        c[nc++] = (struct tick){ (x0 - table.x0) / W_, +1 };
        c[nc++] = (struct tick){ (x1 - table.x0) / W_, -1 };
        r[nr++] = (struct tick){ (y0 - table.y0) / H_, +1 };
        r[nr++] = (struct tick){ (y1 - table.y0) / H_, -1 };
    }
    qsort(cs, (size_t)ncs, sizeof *cs, by_tick);
    qsort(rs, (size_t)nrs, sizeof *rs, by_tick);
    for (int pass = 0; pass < 2; pass++) {
        struct tick *s = pass ? rs : cs, *t = pass ? r : c;
        int ns = pass ? nrs : ncs, *nt = pass ? &nr : &nc;
        for (int i = 0, tally = 0; i < ns; i++) {
            tally += s[i].d;
            if (tally == 0 && i + 1 < ns && s[i + 1].at != s[i].at) {
                t[(*nt)++] = (struct tick){ s[i].at, +1 };
                t[(*nt)++] = (struct tick){ s[i + 1].at, -1 };
            }
        }
    }
    qsort(c, (size_t)nc, sizeof *c, by_tick);
    qsort(r, (size_t)nr, sizeof *r, by_tick);
    for (int pass = 0; pass < 2; pass++) {
        struct tick *t = pass ? r : c;
        int nt = pass ? nr : nc, *count = pass ? &table.nrows : &table.ncols;
        double *lines = pass ? table.rows : table.cols;
        for (int i = 0, tally = 0; i < nt; i++) {
            tally += t[i].d;
            if (tally == 0 && i + 1 < nt && t[i + 1].at != t[i].at && *count < TABLE_LINES) {
                lines[(*count)++] = (t[i].at + t[i + 1].at) / 2;
                table.guessed = 1;
            }
        }
    }
done:
    free(c);
    free(r);
    free(cs);
    free(rs);
}

/* Okular's getTableContents (its text): cells a tab apart, rows a line. */
static char *table_text(void)
{
    double xs[TABLE_LINES + 2], ys[TABLE_LINES + 2];
    int nx = 0, ny = 0;
    xs[nx++] = 0;
    for (int k = 0; k < table.ncols; k++) xs[nx++] = table.cols[k];
    xs[nx++] = 1;
    ys[ny++] = 0;
    for (int k = 0; k < table.nrows; k++) ys[ny++] = table.rows[k];
    ys[ny++] = 1;
    char *out = NULL;
    size_t size = 0;
    FILE *m = open_memstream(&out, &size);
    if (!m) return NULL;
    double W_ = table.x1 - table.x0, H_ = table.y1 - table.y0;
    for (int r = 0; r + 1 < ny; r++) {
        for (int c = 0; c + 1 < nx; c++) {
            if (c) fputc('\t', m);
            int x0 = table.x0 + (int)lround(xs[c] * W_), x1 = table.x0 + (int)lround(xs[c + 1] * W_);
            int y0 = table.y0 + (int)lround(ys[r] * H_), y1 = table.y0 + (int)lround(ys[r + 1] * H_);
            char *t = words_text(x0, y0, x1, y1, 1, 0);
            if (t) fputs(t, m);
            free(t);
        }
        fputc('\n', m);
    }
    fclose(m);
    return out;
}

static void table_ready(void)
{
    guess_dividers();
    ui.dirty = 1;
    char *t = table_text();
    if (t) {
        /* To the primary selection on release (Okular's QClipboard::Selection). */
        int status = clipboard_set(1, t, strlen(t));
        if (status != CLIPBOARD_OK) toast(1, "%s", clipboard_error(status));
    }
    free(t);
}

static void table_start(int x0, int y0, int x1, int y1)
{
    table_clear();
    table.on = 1;
    table.x0 = x0;
    table.y0 = y0;
    table.x1 = x1;
    table.y1 = y1;
    for (int i = 0; i < doc.pages && table.nparts < 16; i++) {
        const struct vl_item *it = &doc.item[i];
        if (it->hidden || it->x >= x1 || it->x + it->w <= x0 || it->y >= y1 || it->y + it->h <= y0) continue;
        int *p = table.part[table.nparts];
        table.item[table.nparts] = *it;
        table.part_page[table.nparts++] = i;
        p[0] = x0 > it->x ? x0 : it->x;
        p[1] = y0 > it->y ? y0 : it->y;
        p[2] = x1 < it->x + it->w ? x1 : it->x + it->w;
        p[3] = y1 < it->y + it->h ? y1 : it->y + it->h;
    }
    if (!words_ask(2, x0, y0, x1, y1)) table_ready();
}

/* A click on a table's part (pageview.cpp mousePressEvent): nearer its top
 * or bottom edge a column divider, nearer its left or right one a row
 * divider; on a divider (3 pixels) it goes. */
static void table_click(int cx, int cy)
{
    for (int k = 0; k < table.nparts; k++) {
        const int *p = table.part[k];
        if (cx < p[0] || cx >= p[2] || cy < p[1] || cy >= p[3]) continue;
        table.guessed = 0;
        int from_left = abs(p[0] - cx), from_right = abs(p[2] - cx), from_top = abs(p[1] - cy), from_bottom = abs(p[3] - cy);
        int col = (from_top < from_bottom ? from_top : from_bottom) < (from_left < from_right ? from_left : from_right);
        double size = col ? table.x1 - table.x0 : table.y1 - table.y0, *lines = col ? table.cols : table.rows;
        int *n = col ? &table.ncols : &table.nrows, origin = col ? table.x0 : table.y0, at = col ? cx : cy, deleted = 0;
        for (int i = 0; i < *n; i++) {
            if (abs(origin + (int)(lines[i] * size + .5) - at) <= 3) {
                memmove(lines + i, lines + i + 1, sizeof *lines * (size_t)(*n - i - 1));
                (*n)--;
                deleted = 1;
                break;
            }
        }
        if (!deleted && *n < TABLE_LINES) {
            double x = (at - origin) / size;
            int i = *n;
            while (i > 0 && lines[i - 1] > x) {
                lines[i] = lines[i - 1];
                i--;
            }
            lines[i] = x;
            (*n)++;
        }
    }
    ui.dirty = 1;
}

static void table_copy(void)
{
    char *t = table_text();
    if (t) {
        int status = clipboard_set(0, t, strlen(t));
        if (status != CLIPBOARD_OK) toast(1, "%s", clipboard_error(status));
    }
    free(t);
}

/* Zoom: Okular's zoom into the rectangle, its middle in the middle. */
static void zoom_into(int x0, int y0, int x1, int y1)
{
    double zoom = fmin(aw / (double)(x1 - x0 > 0 ? x1 - x0 : 1), ah / (double)(y1 - y0 > 0 ? y1 - y0 : 1));
    double nx = (x0 + x1) / (2.0 * (doc.full_w > 0 ? doc.full_w : 1)), ny = (y0 + y1) / (2.0 * (doc.full_h > 0 ? doc.full_h : 1));
    double f = zoom_now();
    if (f <= VL_ZOOM_MAX || zoom <= 1) f *= zoom;
    v.mode = VL_FIXED;
    v.zoom = fmin(fmax(f, VL_ZOOM_MIN), VL_ZOOM_MAX);
    layout();
    v.sx = (int)(nx * doc.full_w) - aw / 2;
    v.sy = (int)(ny * doc.full_h) - ah / 2;
    clamp_scroll();
    ui.follow_thumbs = 1;
    changed();
}

/* The rectangle let go (pageview.cpp mouseReleaseEvent): within 8 by 8
 * pixels it is nothing. */
static void rsel_done(void)
{
    rsel.dragging = 0;
    ui.dirty = 1;
    int x0 = rsel.x0 < rsel.x1 ? rsel.x0 : rsel.x1, x1 = rsel.x0 < rsel.x1 ? rsel.x1 : rsel.x0;
    int y0 = rsel.y0 < rsel.y1 ? rsel.y0 : rsel.y1, y1 = rsel.y0 < rsel.y1 ? rsel.y1 : rsel.y0;
    int tool = settings.tool;
    if (x1 - x0 <= 8 && y1 - y0 <= 8) {
        if (rsel.prev_tool >= 0) settings.tool = rsel.prev_tool;
        rsel.prev_tool = -1;
        return;
    }
    if (tool == TOOL_ZOOM) zoom_into(x0, y0, x1, y1);
    else if (tool == TOOL_TABLE) table_start(x0, y0, x1, y1);
    else if (tool == TOOL_AREA && !words_ask(1, x0, y0, x1, y1)) area_menu();
}

static void rsel_start(int X, int Y, int button)
{
    rsel.dragging = 1;
    rsel.button = button;
    rsel.x0 = rsel.x1 = X - ax + v.sx;
    rsel.y0 = rsel.y1 = Y - ay + v.sy;
    ui.dirty = 1;
}

/* The magnifier: around the pointer, the page under it at ten times. */
static void mag_ask(void)
{
    if (!mag.on || mag.asked || mag.page < 0) return;
    double z = MAG_SCALE * z100, ix, iy;
    vl_point_to_item(v.rotate, doc.pw[mag.page], doc.ph[mag.page], z, mag.px, mag.py, &ix, &iy);
    float rw, rh;
    vl_rotated(v.rotate, doc.pw[mag.page], doc.ph[mag.page], &rw, &rh);
    int W_ = (int)(rw * z + .5), H_ = (int)(rh * z + .5);
    int x0 = (int)lround(ix) - mag.w / 2, y0 = (int)lround(iy) - mag.h / 2;
    int cx0 = clampi(x0, 0, W_ - 1), cy0 = clampi(y0, 0, H_ - 1);
    int cx1 = clampi(x0 + mag.w, 1, W_), cy1 = clampi(y0 + mag.h, 1, H_);
    if (cx1 <= cx0 || cy1 <= cy0) return;
    struct job j = new_job(J_MAGNIFY, VIEW_RENDER);
    j.page = j.req.page = mag.page;
    j.req.zoom = (float)z;
    j.req.rotate = v.rotate;
    j.req.x = cx0;
    j.req.y = cy0;
    j.req.w = cx1 - cx0;
    j.req.h = cy1 - cy0;
    if (settings.invert && settings.render_mode == CM_PAPER) j.req.paper = VIEW_PAPER | settings.paper;
    j.tx = cx0 - x0; /* where in the magnifier */
    j.ty = cy0 - y0;
    j.rotate = mag.seq;
    push_job(&j, 1);
    mag.asked = 1;
}

/* moveMagnifier and updateMagnifier: centred at the pointer, within the
 * area, the pages scrolled when it reaches an edge they can scroll past. */
static void mag_move(int X, int Y)
{
    double px, py;
    int page = page_point(X, Y, 0, &px, &py);
    int sx = 0, sy = 0, w = mag.w / 2, h = mag.h / 2;
    if (X - w < ax && v.sx > 0) sx = X - w - ax - w;
    if (Y - h < ay && v.sy > 0) sy = Y - h - ay - h;
    if (X + w > ax + aw && v.sx < doc.full_w - aw) sx = X + 2 * w - (ax + aw);
    if (Y + h > ay + ah && v.sy < doc.full_h - ah) sy = Y + 2 * h - (ay + ah);
    if (sx || sy) {
        int cx = X + sx - ax + v.sx, cy = Y + sy - ay + v.sy;
        if (cx < v.sx) v.sx = cx;
        else if (cx >= v.sx + aw) v.sx = cx - aw + 1;
        if (cy < v.sy) v.sy = cy;
        else if (cy >= v.sy + ah) v.sy = cy - ah + 1;
        clamp_scroll();
        changed();
    }
    mag.X = X;
    mag.Y = Y;
    if (page >= 0) {
        mag.page = page;
        mag.px = px;
        mag.py = py;
        mag.seq++;
        mag_ask();
    }
    ui.dirty = 1;
}

static void mag_start(int X, int Y)
{
    mag.on = 1;
    mag.w = (int)lround(MAG_W * scale);
    mag.h = (int)lround(MAG_H * scale);
    free(mag.px_buf);
    mag.px_buf = malloc(sizeof *mag.px_buf * (size_t)mag.w * (size_t)mag.h);
    mag.done_seq = -1;
    mag.page = -1;
    if (!mag.px_buf) {
        mag.on = 0;
        return;
    }
    mag_move(X, Y);
}

static void got_magnify(const struct job *j, const struct view_reply *rep, const void *data, size_t size)
{
    mag.asked = 0;
    if (!mag.on || j->gen != doc.gen) return;
    if (rep->status == 0 && data && pix_bytes(rep->w, rep->h) <= size) {
        /* Okular's PagePainter fills the rest with the paper (white). */
        for (size_t k = 0; k < (size_t)mag.w * (size_t)mag.h; k++) mag.px_buf[k] = paper_rgb();
        const uint32_t *px = data;
        for (int y = 0; y < rep->h && j->ty + y < mag.h; y++)
            for (int x = 0; x < rep->w && j->tx + x < mag.w; x++)
                mag.px_buf[(size_t)(j->ty + y) * (size_t)mag.w + (size_t)(j->tx + x)] = px[(size_t)y * (size_t)rep->w + (size_t)x] & 0xFFFFFF;
        mag.done_seq = j->rotate;
        ui.dirty = 1;
    }
    if (j->rotate != mag.seq) mag_ask();
}

/* Where the magnifier is drawn: its top left, within the area. */
static void mag_place(int *x, int *y)
{
    *x = clampi(mag.X - mag.w / 2, ax, ax + aw - mag.w - 1 > ax ? ax + aw - mag.w - 1 : ax);
    *y = clampi(mag.Y - mag.h / 2, ay, ay + ah - mag.h - 1 > ay ? ay + ah - mag.h - 1 : ay);
}

/* drawTicks: the cross, the border and a tick every 5 page pixels. */
static void draw_magnifier(void)
{
    if (!mag.on || mag.done_seq < 0) return;
    int x, y;
    mag_place(&x, &y);
    screen_blit(x, y, mag.w, mag.h, mag.px_buf, mag.w);
    int t = (int)lround(10 * scale), ps = MAG_SCALE * 5;
    screen_fill_px(x + mag.w / 2, y, 1, mag.h, 0);
    screen_fill_px(x, y + mag.h / 2, mag.w, 1, 0);
    screen_fill_px(x, y, mag.w, 1, 0);
    screen_fill_px(x, y + mag.h - 1, mag.w, 1, 0);
    screen_fill_px(x, y, 1, mag.h, 0);
    screen_fill_px(x + mag.w - 1, y, 1, mag.h, 0);
    for (int k = 0; k < mag.w; k += ps) {
        screen_fill_px(x + k, y + 1, 1, t, 0);
        screen_fill_px(x + k, y + mag.h - 1 - t, 1, t, 0);
        if (k < mag.h) {
            screen_fill_px(x + 1, y + k, t, 1, 0);
            screen_fill_px(x + mag.w - 1 - t, y + k, t, 1, 0);
        }
    }
}

static void frame_px(int x0, int y0, int x1, int y1)
{
    screen_fill_px(x0, y0, x1 - x0, 1, SELECTED);
    screen_fill_px(x0, y1 - 1, x1 - x0, 1, SELECTED);
    screen_fill_px(x0, y0, 1, y1 - y0, SELECTED);
    screen_fill_px(x1 - 1, y0, 1, y1 - y0, SELECTED);
}

/* The rectangle being dragged and the table, over the pages. */
static void draw_tools(void)
{
    if (rsel.dragging) {
        int x0 = ax - v.sx + (rsel.x0 < rsel.x1 ? rsel.x0 : rsel.x1), x1 = ax - v.sx + (rsel.x0 < rsel.x1 ? rsel.x1 : rsel.x0);
        int y0 = ay - v.sy + (rsel.y0 < rsel.y1 ? rsel.y0 : rsel.y1), y1 = ay - v.sy + (rsel.y0 < rsel.y1 ? rsel.y1 : rsel.y0);
        if (clip_area(&x0, &y0, &x1, &y1)) frame_px(x0, y0, x1, y1);
    }
    for (int k = 0; table.on && k < table.nparts; k++) {
        const struct vl_item *was = &table.item[k], *now_ = &doc.item[table.part_page[k]];
        if (was->x != now_->x || was->y != now_->y || was->w != now_->w || was->h != now_->h) {
            table_clear();
            break;
        }
    }
    for (int k = 0; table.on && k < table.nparts; k++) {
        const int *p = table.part[k];
        int x0 = ax - v.sx + p[0], y0 = ay - v.sy + p[1], x1 = ax - v.sx + p[2], y1 = ay - v.sy + p[3];
        if (!clip_area(&x0, &y0, &x1, &y1)) continue;
        frame_px(x0, y0, x1, y1);
        for (int c = 0; c < table.ncols; c++) {
            int X = ax - v.sx + table.x0 + (int)(table.cols[c] * (table.x1 - table.x0) + .5);
            if (X >= x0 && X < x1) screen_fill_px(X, y0, 1, y1 - y0, SELECTED);
        }
        for (int r = 0; r < table.nrows; r++) {
            int Y = ay - v.sy + table.y0 + (int)(table.rows[r] * (table.y1 - table.y0) + .5);
            if (Y >= y0 && Y < y1) screen_fill_px(x0, Y, x1 - x0, 1, SELECTED);
        }
    }
    draw_magnifier();
}

/* A press on the pages with Zoom, Area or Table Selection or the
 * Magnifier: 1 if it was theirs. */
static int mouse_tool_press(const struct pointer_event *e)
{
    int t = settings.tool;
    if (t == TOOL_ZOOM && (e->button == BTN_LEFT || e->button == BTN_RIGHT)) {
        if (e->button == BTN_LEFT) rsel_start(e->x, e->y, BTN_LEFT);
        else zoom_step(0); /* Okular: the right button zooms out */
        return 1;
    }
    if (t == TOOL_AREA && e->button == BTN_LEFT) {
        rsel_start(e->x, e->y, BTN_LEFT);
        return 1;
    }
    if (t == TOOL_TABLE && e->button == BTN_LEFT) {
        if (!table.on) rsel_start(e->x, e->y, BTN_LEFT);
        else table_click(e->x - ax + v.sx, e->y - ay + v.sy);
        return 1;
    }
    if (t == TOOL_TABLE && e->button == BTN_RIGHT && table.on) {
        struct item it[1] = { { _("Copy Table Contents to Clipboard"), "", A_TABLE_COPY, 0 } };
        do_action(run_menu(e->col, e->row + 1, it, 1));
        return 1;
    }
    if (t == TOOL_MAGNIFIER && e->button != BTN_MIDDLE) {
        mag_start(e->x, e->y);
        return 1;
    }
    return 0;
}

/* Pointer events while the rectangle or the magnifier has the pointer: 1 if so. */
static int mouse_tool_pointer(const struct pointer_event *e)
{
    if (mag.on) {
        if (!e->button && !e->wheel) mag_move(e->x, e->y);
        else if (e->button && !e->pressed) {
            mag.on = 0;
            ui.dirty = 1;
        }
        return 1;
    }
    if (!rsel.dragging) return 0;
    if (e->wheel || (e->button && e->button != rsel.button)) return 1;
    rsel.x1 = clampi(e->x, ax, ax + aw) - ax + v.sx;
    rsel.y1 = clampi(e->y, ay, ay + ah) - ay + v.sy;
    ui.dirty = 1;
    if (e->button == rsel.button && !e->pressed) rsel_done();
    return 1;
}

/* A tool chosen: what the others were doing ends (pageview.cpp slotSetMouse...). */
static const char *const tool_messages[] = {
    [TOOL_TEXT] = N_("Select text"),
    [TOOL_ZOOM] = N_("Select zooming area. Right-click to zoom out."),
    [TOOL_AREA] = N_("Draw a rectangle around the text/graphics to copy."),
    /* TRANSLATORS: Esc is the Escape key; keep it. */
    [TOOL_TABLE] = N_("Draw a rectangle around the table, then click near edges to divide up; press Esc to clear."),
    [TOOL_MAGNIFIER] = N_("Click to see the magnified view."),
};

static void tools_reset(void)
{
    rsel.dragging = 0;
    rsel.prev_tool = -1;
    words_clear();
    table_clear();
    mag.on = 0;
}

static void tool_set(int tool)
{
    tools_reset();
    settings.tool = tool;
    sel_clear();
    if (tool_messages[tool]) toast(0, "%s", _(tool_messages[tool]));
    changed();
}

/* A click on what was drawn to be clicked. */
static void click(int id, const struct pointer_event *e)
{
    if (pdf_click(id, e)) {
        ui.dirty = 1;
        return;
    }
    if (id >= HIT_BOOKMARK) {
        ui.focus = FOCUS_SIDEBAR;
        for (int i = 0, k = 0; i < doc.pages; i++) if (doc.bookmark[i]) {
            if (i == id - HIT_BOOKMARK) ui.bm_sel = k;
            k++;
        }
        go(pos_page(id - HIT_BOOKMARK));
    } else if (id >= HIT_THUMB) {
        ui.focus = FOCUS_SIDEBAR;
        ui.thumb_sel = id - HIT_THUMB;
        go(pos_page(ui.thumb_sel));
    } else if (id >= HIT_TOC) {
        ui.focus = FOCUS_SIDEBAR;
        ui.toc_sel = id - HIT_TOC;
        side_go();
    } else if (id >= HIT_LINE) {
        list.sel = id - HIT_LINE;
        if (e->clicks == 2 && list.kind == L_EMBEDDED) save_embedded();
    } else if (id >= HIT_CLOSE_TAB && id < HIT_CLOSE_TAB + TABS_MAX) {
        int index = id - HIT_CLOSE_TAB;
        if (pdf_guard(A_CLOSE_TAB, index)) return;
        if (index != tabs.current) tab_switch(index);
        tab_close();
    } else if (id >= HIT_DOCUMENT_TAB && id < HIT_DOCUMENT_TAB + TABS_MAX) {
        tab_switch(id - HIT_DOCUMENT_TAB);
    } else if (id >= HIT_LIST) {
        if (id == HIT_LIST + 0) save_embedded();
        else list_close();
    } else if (id == HIT_FIND_INPUT || id == HIT_PROMPT_INPUT) {
        struct line *l = id == HIT_FIND_INPUT ? &find.in : &prompt.in;
        ui.focus = id == HIT_FIND_INPUT ? FOCUS_FIND : FOCUS_PROMPT;
        size_t at = line_at_cell(l, e->col);
        l->cursor = at;
        if (!(keys_modifiers() & KEYS_SHIFT)) l->mark = at;
        l->replace = 0;
        ptr.find_drag = e->button == BTN_LEFT ? (id == HIT_FIND_INPUT ? 1 : 2) : 0;
        if (e->clicks >= 2) { l->mark = 0; l->cursor = strlen(l->text); l->replace = 1; }
    } else if (id >= HIT_FIND) {
        if (id == HIT_FIND + 2) find_bar(0);
        else find_go(id == HIT_FIND + 0 ? 1 : -1);
    } else if (id >= HIT_TAB) {
        ui.focus = FOCUS_SIDEBAR;
        ui.tab = id - HIT_TAB;
    } else if (id == HIT_SIDEBAR) {
        do_action(A_SIDEBAR);
    } else if (id == HIT_ZOOM) {
        zoom_menu(e->col);
    } else if (id == HIT_GOTO) {
        do_action(A_GOTO);
    } else if (id >= HIT_MENU) {
        open_menu(id - HIT_MENU);
    }
    ui.dirty = 1;
}

static void pointer(const struct pointer_event *e)
{
    if (pres.on && doc.open && !prompt.kind) {
        pres_pointer(e);
        return;
    }
    int mods = keys_modifiers(), page;
    double px, py;
    int in_area = e->x >= ax && e->x < ax + aw && e->y >= ay && e->y < ay + ah;
    ptr.x = e->x;
    ptr.y = e->y;
    /* What a held button is doing has the pointer until it is let go. */
    if (ptr.find_drag) {
        if (!e->button || (e->button == BTN_LEFT && !e->pressed)) {
            struct line *l = ptr.find_drag == 1 ? &find.in : &prompt.in;
            l->cursor = line_at_cell(l, e->col);
            l->replace = 0;
            ui.dirty = 1;
        }
        if (e->button == BTN_LEFT && !e->pressed) ptr.find_drag = 0;
        return;
    }
    if (ptr.splitter) {
        if (!e->button || (e->button == BTN_LEFT && !e->pressed)) {
            struct anchor a = anchor_at(ax + aw / 2, ay + ah / 2);
            settings.side_width = clampi((int)lround((e->x - ptr.split_offset) / scale), 90, 600);
            geometry();
            layout();
            anchor_keep(&a);
            viewport_reflow();
            changed();
        }
        if (e->button == BTN_LEFT && !e->pressed) { ptr.splitter = 0; state_save(); }
        return;
    }
    if (trim.dragging) {
        if (!e->button || (e->button == BTN_LEFT && !e->pressed)) {
            struct vl_params p = params();
            const struct vl_item *it = &doc.item[trim.page];
            double lx = e->x - ax + v.sx - it->x, ly = e->y - ay + v.sy - it->y;
            lx = fmax(0, fmin(it->w, lx)); ly = fmax(0, fmin(it->h, ly));
            vl_item_to_point(p.rotate, doc.pw[trim.page], doc.ph[trim.page], it->zoom,
                lx + it->ox, ly + it->oy, &px, &py);
            trim.xb = px; trim.yb = py;
            ui.dirty = 1;
        }
        if (e->button == BTN_LEFT && !e->pressed) {
            trim.dragging = 0;
            if (page_point(e->x, e->y, 0, &px, &py) != trim.page ||
                fabs(trim.xa - trim.xb) < 1 || fabs(trim.ya - trim.yb) < 1) return;
            trim.box = (struct vl_crop){ fmin(trim.xa, trim.xb) / doc.pw[trim.page],
                fmin(trim.ya, trim.yb) / doc.ph[trim.page], fmax(trim.xa, trim.xb) / doc.pw[trim.page],
                fmax(trim.ya, trim.yb) / doc.ph[trim.page] };
            struct anchor a = anchor_at(ax + aw / 2, ay + ah / 2);
            trim.picking = 0;
            pix_flush(); layout(); anchor_keep(&a); changed();
            ui.toast[0] = 0;
        }
        return;
    }
    if (ptr.mid) {
        if (e->button == BTN_MIDDLE && !e->pressed) {
            ptr.mid = 0;
        } else if (!e->button && e->y != ptr.mid_y) {
            /* Okular's continuous zoom: up zooms in. */
            zoom_to(VL_FIXED, zoom_now() * (1 + (ptr.mid_y - e->y) / (500.0 * scale)), ax + aw / 2, ay + ah / 2);
            ptr.mid_y = e->y;
        }
        return;
    }
    if (ptr.grab) {
        if (e->button == BTN_LEFT && !e->pressed) {
            ptr.grab = 0;
            int k = link_at(e->x, e->y, &page);
            if (ptr.moved < 10 * scale && ptr.press_link >= 0 && k == ptr.press_link && page == ptr.press_page) {
                follow_link(page, k);
            }
        } else if (!e->button && !e->wheel) {
            int dx = e->x - ptr.grab_x, dy = e->y - ptr.grab_y;
            ptr.moved += abs(dx) + abs(dy);
            ptr.grab_x = e->x;
            ptr.grab_y = e->y;
            scroll_by(-dx, -dy);
        }
        return;
    }
    if (sel.dragging) {
        if (!e->button && !e->wheel) {
            if (!sel.started && abs(e->x - sel.press_x) + abs(e->y - sel.press_y) > 5 * scale) sel.started = 1;
            int i = sel.started ? page_point(e->x, e->y, 1, &px, &py) : -1;
            if (i >= 0) {
                sel.pb = i;
                sel.xb = px;
                sel.yb = py;
                sel.mode = VIEW_SELECT_CHARS;
                sel.pending = 1;
            }
        } else if (e->button == BTN_LEFT && !e->pressed) {
            sel.dragging = 0;
            if (sel.started) {
                sel.final = 1;
                if (!sel.pending && !sel.round && !sel.waiting) {
                    sel.final = 0;
                    copy_selection(1);
                }
            } else {
                int k = link_at(e->x, e->y, &page);
                if (k >= 0 && k == ptr.press_link && page == ptr.press_page) follow_link(page, k);
            }
        }
        return;
    }
    if (mouse_tool_pointer(e)) return;
    if (pdf_pointer(e, in_area)) {
        ui.dirty = 1;
        return;
    }
    if (e->gesture == GESTURE_PINCH) {
        if (in_area && e->scale > 0) zoom_to(VL_FIXED, zoom_now() * e->scale, e->x, e->y);
        return;
    }
    if (e->wheel || e->hwheel) {
        if (list.kind) {
            list.sel = clampi(list.sel - 3 * e->wheel, 0, list.n > 0 ? list.n - 1 : 0);
            ui.dirty = 1;
        } else if (side_cols && e->x < side_cols * cw && e->y >= ay) {
            int last = doc.noutline > 0 ? doc.noutline - 1 : 0;
            if (pdf_wheel_sidebar(e->wheel)) {
                /* its panels' */
            } else if (ui.tab == TAB_CONTENTS) {
                ui.toc_top = clampi(ui.toc_top - 3 * e->wheel, 0, last);
                ui.toc_sel = clampi(ui.toc_sel, ui.toc_top, last);
            } else {
                ui.thumb_top = ui.thumb_top - 3 * ch * e->wheel > 0 ? ui.thumb_top - 3 * ch * e->wheel : 0;
            }
            ui.dirty = 1;
        } else if (in_area && (mods & KEYS_CTRL) && e->wheel) {
            /* Okular: a step (120) times 1 + 120 / 500, at the pointer. */
            double f = zoom_now();
            for (int k = 0; k < abs(e->wheel); k++) f *= e->wheel > 0 ? 1 + 120 / 500.0 : 1 - 120 / 500.0;
            zoom_to(VL_FIXED, f, e->x, e->y);
        } else if (in_area) {
            int times = mods & KEYS_SHIFT ? 10 : 1; /* Okular: Shift, ten times as far */
            scroll_by(3 * ch * e->hwheel * times, -3 * ch * e->wheel * times);
        }
        return;
    }
    if (!e->button) {
        /* Browse: the right button moved 5 pixels from its press is Area
         * Selection until let go (pageview.cpp mouseMoveEvent). */
        if (ptr.right && settings.tool == TOOL_BROWSE && doc.open && (e->buttons & 2) &&
            (abs(e->x - ptr.press_x) > 5 * scale || abs(e->y - ptr.press_y) > 5 * scale)) {
            ptr.right = 0;
            rsel.prev_tool = TOOL_BROWSE;
            settings.tool = TOOL_AREA;
            rsel_start(ptr.press_x, ptr.press_y, BTN_RIGHT);
            mouse_tool_pointer(e);
            return;
        }
        set_cursor(e->x, e->y);
        return;
    }
    if (e->button == BTN_LEFT && e->pressed && side_px && e->x >= side_px && e->x < ax &&
        e->y >= ay && e->y < ay + ah && !list.kind && !prompt.kind) {
        ptr.splitter = 1;
        ptr.split_offset = e->x - side_px;
        ui.focus = FOCUS_SIDEBAR;
        return;
    }
    if (e->pressed && e->y >= ay && !list.kind && !prompt.kind) {
        if (in_area) ui.focus = FOCUS_DOC;
        else if (side_px && e->x < side_px && (!find.bar || e->row < nrows - 1)) ui.focus = FOCUS_SIDEBAR;
        ui.dirty = 1;
    }
    int id = ui_hit_at(e->col, e->row);
    if (id == HIT_FIND_INPUT && e->button == BTN_MIDDLE && e->pressed) {
        click(id, e); line_paste(&find.in, 1); find.failed = 0; find.typed = now(); return;
    }
    if (id == HIT_PROMPT_INPUT && e->button == BTN_MIDDLE && e->pressed) {
        click(id, e); line_paste(&prompt.in, 1); return;
    }
    if (e->pressed && e->button == BTN_RIGHT && id >= HIT_BOOKMARK && id < HIT_BOOKMARK + 1000000) {
        ui.focus = FOCUS_SIDEBAR;
        bookmark_menu(id - HIT_BOOKMARK, e->col, e->row + 1);
        ui.dirty = 1;
        return;
    }
    if (e->pressed && e->button == BTN_LEFT && id >= 0) {
        click(id, e);
        return;
    }
    if (list.kind || !in_area || !doc.open) return;
    if (trim.picking && e->button == BTN_LEFT && e->pressed) {
        int i = page_point(e->x, e->y, 0, &px, &py);
        if (i >= 0) {
            trim.dragging = 1; trim.page = i;
            trim.xa = trim.xb = px; trim.ya = trim.yb = py;
        }
        return;
    }
    if (e->pressed && mouse_tool_press(e)) return;
    if (e->button == BTN_MIDDLE && e->pressed) {
        ptr.mid = 1;
        ptr.mid_y = e->y;
    } else if (e->button == BTN_RIGHT && e->pressed) {
        ptr.press_x = e->x;
        ptr.press_y = e->y;
        ptr.right = 1;
    } else if (e->button == BTN_RIGHT && !e->pressed && ptr.right && abs(e->x - ptr.press_x) + abs(e->y - ptr.press_y) < 10 * scale) {
        ptr.right = 0;
        context_menu(e->x, e->y, e->col, e->row);
    } else if (e->button == BTN_LEFT && e->pressed) {
        ptr.press_link = link_at(e->x, e->y, &ptr.press_page);
        if (settings.tool == TOOL_BROWSE) {
            ptr.grab = 1;
            ptr.grab_x = e->x;
            ptr.grab_y = e->y;
            ptr.moved = 0;
            return;
        }
        int i = page_point(e->x, e->y, 0, &px, &py);
        if (e->clicks >= 2 && i >= 0) {
            /* A double click selects a word, a triple one a line (Okular's
             * Text Selection), straight onto the primary selection. */
            sel.pa = sel.pb = i;
            sel.xa = sel.xb = px;
            sel.ya = sel.yb = py;
            sel.mode = e->clicks == 2 ? VIEW_SELECT_WORDS : VIEW_SELECT_LINES;
            sel.pending = sel.final = 1;
            return;
        }
        sel_clear();
        if (i < 0) return;
        sel.dragging = 1;
        sel.started = 0;
        sel.pa = sel.pb = i;
        sel.xa = sel.xb = px;
        sel.ya = sel.yb = py;
        sel.press_x = e->x;
        sel.press_y = e->y;
    }
}

static void handle(int key)
{
    if (key == K_SHOWN) {
        refresh();
        return;
    }
    if (key == K_POINTER) {
        pointer(pointer_last());
        return;
    }
    if (pres.on && doc.open && ui.focus != FOCUS_PROMPT) {
        pres_key(key, keys_modifiers());
        return;
    }
    int mods = keys_modifiers();
    int typing = typing_line();
    ime_set_focus(typing);
    if (typing && ime_mode() == IME_MODE_CN && ime_width_key(key, mods)) {
        const char *notice = ime_toggle_width();
        if (notice) toast(0, "%s", notice);
        ui.dirty = 1; return;
    }
    if (typing && key != K_CTRL_SPACE && key != K_ACT_IME_CN && key != K_ACT_IME_RAW && key != K_ACT_IME_WIDTH) {
        int taken = ime_process_key(key);
        const char *commit = ime_get_commit();
        if (commit && *commit) {
            if (ui.focus == FOCUS_FIND) find_insert(commit, strlen(commit));
            else line_insert(&prompt.in, commit, strlen(commit));
        }
        if (taken) { ui.dirty = 1; return; }
    }
    if (trim.picking && key == 27) {
        do_action(A_TRIM_SELECTION);
        return;
    }
    if (ui.focus == FOCUS_FIND && mods == KEYS_CTRL && (key == 3 || key == 24 || key == 22 || key == 1)) {
        key_find(key, mods);
        return;
    }
    if (ui.focus == FOCUS_PROMPT) {
        key_prompt(key, mods);
    } else if (ui.focus == FOCUS_LIST) {
        if (key == 17 && (mods & KEYS_CTRL)) do_action(A_QUIT);
        else key_list(key, mods);
    } else if (!key_global(key, mods)) {
        if (ui.focus == FOCUS_FIND) key_find(key, mods);
        else if (ui.focus == FOCUS_SIDEBAR) key_sidebar(key, mods);
        else key_doc(key, mods);
    }
}

#include "view-pdf-ui.inc"

/* Milliseconds until something is due (-1: nothing). */
static int next_timeout(void)
{
    double t = 0, at = now();
    if (ui.toast[0]) t = ui.toast_until;
    if (find.typed && (!t || find.typed + 0.7 < t)) t = find.typed + 0.7;
    if (ui.changed && (!t || ui.changed + 1 < t)) t = ui.changed + 1;
    if (pres.on && pres.overlay_until > at && (!t || pres.overlay_until < t)) t = pres.overlay_until;
    if (pres.on && pres.playing && (!t || pres.next_at < t)) t = pres.next_at;
    if (!t) return -1;
    return t <= at ? 0 : (int)((t - at) * 1000) + 1;
}

static void timers(void)
{
    double at = now();
    if (ui.toast[0] && at >= ui.toast_until) ui.dirty = 1;
    if (find.typed && at >= find.typed + 0.7) {
        find.typed = 0;
        if (find.in.text[0]) find_go(1);
        else find_reset();
    }
    if (ui.changed && at >= ui.changed + 1) state_save();
    if (pres.on && pres.overlay_until && at >= pres.overlay_until) {
        pres.overlay_until = 0;
        ui.dirty = 1;
    }
    if (pres.on && pres.playing && at >= pres.next_at) {
        if (pres.page + 1 < doc.pages) pres_go(pres.page + 1);
        else pres.playing = 0; /* at the end the timer is not started again */
        pres.next_at = at + PRES_ADVANCE;
    }
}

/* Everything back to how it starts (for the tests, which run it again). */
static void forget(void)
{
    stop_renderer();
    close_document();
    free(cache);
    free(scratch);
    free(ovl);
    cache = NULL;
    scratch = NULL;
    ovl = NULL;
    ncache = novl = ovl_size = 0;
    scratch_size = 0;
    nqueue = 0;
    memset(&fly, 0, sizeof fly);
    memset(&doc, 0, sizeof doc);
    memset(&find, 0, sizeof find);
    find.cur_page = find.cur_hit = -1;
    memset(&prompt, 0, sizeof prompt);
    memset(&ui, 0, sizeof ui);
    ui.cursor = -1;
    memset(&ptr, 0, sizeof ptr);
    ptr.press_page = ptr.press_link = -1;
    memset(&hist, 0, sizeof hist);
    v = (typeof(v)){ VL_FIT_WIDTH, 1, 0, 0, 0, 0, 0 };
    settings_default();
    memset(&trim, 0, sizeof trim);
    memset(&font, 0, sizeof font);
    tools_reset();
    memset(&pres, 0, sizeof pres);
    free(mag.px_buf);
    free(capture.px);
    memset(&mag, 0, sizeof mag);
    memset(&capture, 0, sizeof capture);
    for (int i = 0; i < tabs.n; i++) {
        free(tabs.path[i]);
        keep_free(tabs.keep[i]);
    }
    for (int i = 0; i < tabs.nclosed; i++) free(tabs.closed[i]);
    recent_clear();
    memset(&tabs, 0, sizeof tabs);
    restarts = 0;
    last_death = 0;
    pdf_reset();
}

int main(int argc, char **argv)
{
    forget();
    screen_open(BASE_FONT);
    keys_open();
    ime_init(NULL, NULL);
    state_load(NULL, &saved);
    recent_load();
    geometry();
    start_renderer();
    if (argc > 1) {
        open_document(argv[1]);
    } else {
        pick_document();
        if (!doc.path[0]) {
            ime_done();
            forget();
            return 0; /* nothing chosen: back */
        }
    }
    while (!ui.quit) {
        schedule();
        if (ui.dirty) draw();
        struct pollfd p = { sock, POLLIN, 0 };
        int key = key_poll_many(&p, sock >= 0 ? 1 : 0, next_timeout());
        if (sock >= 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) receive();
        if (key) handle(key);
        timers();
    }
    state_save();
    ime_done();
    forget();
    return 0;
}
