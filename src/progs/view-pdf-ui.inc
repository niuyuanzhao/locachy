/*
 * view-pdf-ui.inc, a part of view.c: a PDF's annotations, form fields,
 * signatures and saving, as Okular has them, by view-render's VIEW_PDF_*
 * (view-pdf.c on MuPDF's journal: the document is edited there, and only
 * what view-render answers is shown here).
 *
 * From Okular's handbook (Annotations, Forms, Signatures, the File and Edit
 * menus) and its sources (part/annotationactionhandler.cpp's tools and keys,
 * part/data/tools.xml's defaults, part/pageviewmouseannotation.cpp's
 * selection, core/annotations.cpp's canBeMoved and canBeResized,
 * part/formwidgets.cpp, part/annotationmodel.cpp, gui/signaturemodel.cpp
 * and gui/signatureguiutils.cpp's texts):
 *   F6 (Tools > Annotations): the annotation toolbar. Its tools, and Alt+1
 *   to Alt+8 and Alt+0: Highlighter, Underline, Squiggle, Strike Out,
 *   Typewriter, Inline Note, Popup Note, Freehand Line, Rectangle; with the
 *   toolbar shown, 1 to 9 its first nine. Esc stops annotating; Keep Active
 *   (Okular's default) keeps the tool after use. Shift: squares and circles,
 *   lines at 15 degree steps.
 *   Browse: a click on an annotation selects it; dragged it moves (not the
 *   text markups), by one of its 8 handles it is resized (Inline Note,
 *   Typewriter, Stamp, Rectangle, Ellipse); Del deletes it, Esc unselects
 *   it, a double click edits its text; the right button: Open Pop-up Note,
 *   Copy Text to Clipboard, Delete, Properties.
 *   Ctrl+Z, Ctrl+Shift+Z: undo, redo. Ctrl+S, Ctrl+Shift+S: Save, Save As.
 *   Closing, reloading or quitting with unsaved changes asks to save or
 *   discard them (Okular's queryClose).
 *   View > Show Forms: Tab and Shift+Tab go from field to field; Space
 *   toggles a check box, chooses a radio button, opens a choice's options;
 *   Up and Down choose in a combo box; a text field takes what is typed
 *   (Enter edits its text), up to its MaxLen; a value the document's
 *   validation rejects is shown, and the field keeps its old one.
 *   The sidebar's Annotations panel (by page) and, when there are some,
 *   its Signatures panel.
 */

#define HIT_PDF_TOOL  7000    /* + a tool */
#define HIT_PDF_SET   7040    /* + a toolbar setting */
#define HIT_PDF_BAR   7060    /* + 0 forms shown, 1 its bar closed, 2 signatures panel, 3 its bar closed */
#define HIT_PDF_ANNOT 4000000 /* + a row of the annotations panel */
#define HIT_PDF_SIG   4500000 /* + a row of the signatures panel */

/* The PDF's annotation flags (ISO 32000 12.5.3) */
#define AF_READ_ONLY       64
#define AF_LOCKED          128
#define AF_LOCKED_CONTENTS 512
#define TX_MULTILINE       4096 /* a text field's /Ff */

enum { T_NONE, T_HIGHLIGHT, T_UNDERLINE, T_SQUIGGLE, T_STRIKE, T_TYPEWRITER, T_INLINE, T_POPUP, T_INK,
       T_LINE, T_RECT, T_ELLIPSE, T_POLYGON, T_STAMP, T_COUNT };

/* Okular's default tools (tools.xml): their colours and widths. Its Arrow
 * (Alt+9) needs line endings, which view-pdf.c does not set. The Inline
 * Note's yellow fill needs an interior colour, which it does not set
 * either: the note is black text in a frame. */
static const struct tool {
    const char *name;
    int type;
    uint32_t rgb;
    float width;
    char alt;
} tools[T_COUNT] = {
    [T_HIGHLIGHT] = { N_("Highlighter"), VIEW_ANNOT_HIGHLIGHT, 0xFFFF00, 1, '1' },
    [T_UNDERLINE] = { N_("Underline"), VIEW_ANNOT_UNDERLINE, 0xFF0000, 1, '2' },
    [T_SQUIGGLE] = { N_("Squiggle"), VIEW_ANNOT_SQUIGGLY, 0xFF0000, 1, '3' },
    [T_STRIKE] = { N_("Strike Out"), VIEW_ANNOT_STRIKE_OUT, 0xFF0000, 1, '4' },
    [T_TYPEWRITER] = { N_("Typewriter"), VIEW_ANNOT_FREE_TEXT, 0x000000, 0, '5' },
    [T_INLINE] = { N_("Inline Note"), VIEW_ANNOT_FREE_TEXT, 0x000000, 1, '6' },
    [T_POPUP] = { N_("Popup Note"), VIEW_ANNOT_TEXT, 0xFFFF00, 1, '7' },
    [T_INK] = { N_("Freehand Line"), VIEW_ANNOT_INK, 0x00FF00, 2, '8' },
    [T_LINE] = { N_("Straight Line"), VIEW_ANNOT_LINE, 0x000000, 2, 0 },
    [T_RECT] = { N_("Rectangle"), VIEW_ANNOT_SQUARE, 0xFF0000, 3, '0' },
    [T_ELLIPSE] = { N_("Ellipse"), VIEW_ANNOT_CIRCLE, 0x00FFFF, 3, 0 },
    [T_POLYGON] = { N_("Polygon"), VIEW_ANNOT_POLYGON, 0x007EEE, 2, 0 },
    [T_STAMP] = { N_("Stamp"), VIEW_ANNOT_STAMP, 0xFF0000, 1, 0 },
};

/* Okular's colour choices (annotationactionhandler.cpp). */
static const struct { const char *name; uint32_t rgb; } colors[] = {
    { N_("Red"), 0xFF0000 }, { N_("Orange"), 0xFF5500 }, { N_("Yellow"), 0xFFFF00 }, { N_("Green"), 0x00FF00 },
    { N_("Cyan"), 0x00FFFF }, { N_("Blue"), 0x0000FF }, { N_("Magenta"), 0xFF00FF }, { N_("White"), 0xFFFFFF },
    { N_("Gray"), 0xA0A0A4 }, { N_("Black"), 0x000000 },
};
#define NCOLORS (int)(sizeof colors / sizeof colors[0])

/* The standard stamps and note icons (ISO 32000 tables 175 and 181). */
static const char *const stamps[] = {
    "Approved", "AsIs", "Confidential", "Departmental", "Draft", "Experimental", "Expired", "Final",
    "ForComment", "ForPublicRelease", "NotApproved", "NotForPublicRelease", "Sold", "TopSecret",
};
static const char *const note_icons[] = { "Comment", "Key", "Note", "Help", "NewParagraph", "Paragraph", "Insert" };

enum { D_NONE, D_SELECT, D_DRAW, D_LINE, D_POLYGON };
enum { E_NONE, E_CONTENTS, E_AUTHOR, E_FIELD, E_NEW };
enum { H_L = 1, H_R = 2, H_T = 4, H_B = 8 };
static const int handles[8] = { H_L | H_T, H_T, H_R | H_T, H_R, H_R | H_B, H_B, H_L | H_B, H_L };

static struct {
    int attached;                 /* view-render edits this PDF */
    unsigned flags;               /* VIEW_PDF_* of its last answer */
    uint64_t revision;
    int nsig_fields;
    struct view_signature *sig;
    int nsig, sigs;               /* sigs: 0 not asked, 1 asked, 2 here */
    /* the annotation toolbar */
    int toolbar, tool, keep, set_init;
    char stamp[32];
    struct { uint32_t rgb; float width, opacity; } set[T_COUNT];
    /* a press on the pages: what it is doing */
    int drag, dpage, handle, moved, press_x, press_y;
    double ax, ay, bx, by;        /* page points: where it began, where it is */
    struct view_rect orig;        /* the selected one's rectangle at the press */
    int npts;
    float pts[2 * VIEW_PDF_POINTS];
    /* the selected annotation, and its rectangle moved, not answered yet */
    int spage, sobject, sgen, pending;
    struct view_rect pending_rect;
    /* forms, and the bars about them */
    int forms, forms_closed, sigbar_closed;
    int fpage, fobject, fgen;     /* the focused field (fobject 0: none) */
    /* what is typed: an annotation's contents or author, a field, a new note */
    struct {
        int what, page, object, gen; /* the text itself: prompt.in */
        struct job create;
    } ed;
    /* unsaved changes: the action waiting for them, a save going */
    int resume, resume_arg, saving, discard, save_as;
    char save_path[PATH_MAX];
    /* the sidebar panels */
    int asel, atop, ssel, stop;
} pdf;

static int pdf_on(void)
{
    return doc.open && pdf.attached;
}

static int pdf_modified(void)
{
    return pdf_on() && (pdf.flags & VIEW_PDF_DIRTY);
}

static int pdf_editing(void)
{
    return prompt.kind == P_PDF_TEXT;
}

static void tool_settings(void)
{
    if (pdf.set_init) return;
    for (int t = 1; t < T_COUNT; t++) {
        pdf.set[t].rgb = tools[t].rgb;
        pdf.set[t].width = tools[t].width;
        pdf.set[t].opacity = 1;
    }
    pdf.set_init = 1;
}

/* The bytes of the valid UTF-8 character at s (left bytes there): 0 if none. */
static int utf8_char(const char *s, size_t left)
{
    unsigned char b = (unsigned char)s[0];
    size_t n = b < 0x80 ? 1 : b >= 0xC2 && b < 0xE0 ? 2 : b >= 0xE0 && b < 0xF0 ? 3 : b >= 0xF0 && b <= 0xF4 ? 4 : 0;
    if (!n || n > left) return 0;
    for (size_t k = 1; k < n; k++) if (((unsigned char)s[k] & 0xC0) != 0x80) return 0;
    if (n == 3 && ((b == 0xE0 && (unsigned char)s[1] < 0xA0) || (b == 0xED && (unsigned char)s[1] >= 0xA0))) return 0;
    if (n == 4 && ((b == 0xF0 && (unsigned char)s[1] < 0x90) || (b == 0xF4 && (unsigned char)s[1] >= 0x90))) return 0;
    return (int)n;
}

static int utf8_valid(const char *s)
{
    for (size_t n = strlen(s), i = 0; i < n;) {
        int k = utf8_char(s + i, n - i);
        if (!k) return 0;
        i += (size_t)k;
    }
    return 1;
}

static int utf8_count(const char *s)
{
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

/* src into dst of size bytes, cut between UTF-8 characters. */
static void str_copy(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src), k = n < size - 1 ? n : size - 1;
    if (k < n) while (k > 0 && ((unsigned char)src[k] & 0xC0) == 0x80) k--;
    memcpy(dst, src, k);
    dst[k] = '\0';
}

/* The author of new annotations: the user's full name (Okular's identity). */
static const char *user_name(void)
{
    static char name[256];
    struct passwd *pw = getpwuid(VIEW_USER);
    name[0] = '\0';
    if (pw && pw->pw_gecos && *pw->pw_gecos) {
        snprintf(name, sizeof name, "%.*s", (int)strcspn(pw->pw_gecos, ","), pw->pw_gecos);
        if (!utf8_valid(name)) name[0] = '\0';
    }
    if (!name[0] && pw && pw->pw_name && utf8_valid(pw->pw_name)) str_copy(name, sizeof name, pw->pw_name);
    return name;
}

static struct job pdf_job(int kind, int op, int page)
{
    struct job j = new_job(kind, op);
    j.page = j.req.page = page;
    return j;
}

static void pdf_free_page(struct page *pg)
{
    free(pg->annot);
    free(pg->field);
    free(pg->option);
    pg->annot = NULL;
    pg->field = NULL;
    pg->option = NULL;
    pg->nannot = pg->nfield = pg->noption = 0;
    pg->annots = pg->fields = 0;
}

static int forms_bar(void)
{
    return (pdf.flags & (VIEW_PDF_HAS_FORMS | VIEW_PDF_HAS_XFA)) && !pdf.forms_closed;
}

static int sig_bar(void)
{
    return pdf.nsig_fields > 0 && !pdf.sigbar_closed;
}

/* The toolbar and the bars over the pages. */
static const char *color_name(uint32_t rgb)
{
    static char hex[16];
    for (int k = 0; k < NCOLORS; k++) if (colors[k].rgb == rgb) return _(colors[k].name);
    snprintf(hex, sizeof hex, "#%06X", rgb & 0xFFFFFF);
    return hex;
}

/* The toolbar's settings: of the tool in use (else of none). */
static void setting_label(int k, char *out, size_t size)
{
    int t = pdf.tool;
    tool_settings();
    switch (k) {
    case 0: snprintf(out, size, _("Color: %s"), t ? color_name(pdf.set[t].rgb) : "-"); break;
    case 1: if (t) snprintf(out, size, _("Width: %g"), pdf.set[t].width); else snprintf(out, size, "%s", _("Width: -")); break;
    case 2: if (t) snprintf(out, size, _("Opacity: %d%%"), (int)lround(pdf.set[t].opacity * 100)); else snprintf(out, size, "%s", _("Opacity: -")); break;
    case 3: snprintf(out, size, _("Stamp: %s"), pdf.stamp); break;
    case 4: snprintf(out, size, "%s", _("Keep Active")); break;
    default: snprintf(out, size, "%s", _("Hide")); break;
    }
}

static int tool_button(int col, int row, const char *label, int id, int on, int draw)
{
    int n = ui_cells(label) + 2;
    if (draw && col + n <= ncols) {
        uint32_t bg = on ? ACCENT : 0x304860, fg = on ? BG : FG;
        screen_fill(col, row, n, 1, bg);
        screen_text(col, row, "[", fg, bg);
        screen_text(col + 1, row, label, fg, bg);
        screen_text(col + n - 1, row, "]", fg, bg);
        ui_hit(col, row, n, 1, id);
    }
    return n + 1;
}

/* The toolbar from row (drawn or not): the rows it takes. */
static int toolbar(int row, int draw)
{
    int start = (ax + cw - 1) / cw, col = start, rows = 1;
    char label[64];
    for (int k = 1; k < T_COUNT + 6; k++) {
        int t = k < T_COUNT ? k : 0, s = k - T_COUNT;
        if (t) snprintf(label, sizeof label, "%s", _(tools[t].name));
        else setting_label(s, label, sizeof label);
        int n = ui_cells(label) + 3;
        if (col + n > ncols && col > start) {
            col = start;
            row++;
            rows++;
        }
        if (t) col += tool_button(col, row, label, HIT_PDF_TOOL + t, pdf.tool == t, draw);
        else col += tool_button(col, row, label, HIT_PDF_SET + s, s == 4 && pdf.keep, draw);
    }
    return rows;
}

static int pdf_rows(void)
{
    if (!pdf_on()) return 0;
    return (pdf.toolbar ? toolbar(0, 0) : 0) + forms_bar() + sig_bar();
}

/* The bars' rows changed: the pages laid out again, where they were. */
static void pdf_relayout(void)
{
    struct anchor a = anchor_at(ax, ay);
    geometry();
    layout();
    anchor_keep(&a);
    viewport_reflow();
    ui.dirty = 1;
}

static const struct view_field *focused_field(int *page, int *index);

static void field_text(const struct page *pg, const struct view_field *f, char *out, size_t size)
{
    static const char *const kinds[] = {
        N_("text"), N_("check box"), N_("radio button"), N_("combo box"), N_("list box"),
        N_("push button"), N_("signature"), N_("field"),
    };
    char value[600] = "", more[64] = "";
    int type = f->type >= 0 && f->type <= VIEW_FIELD_OTHER ? f->type : VIEW_FIELD_OTHER;
    if (type == VIEW_FIELD_CHECKBOX) snprintf(value, sizeof value, "%s", strcmp(f->value, f->on_value) ? _("not checked") : _("checked"));
    else if (type == VIEW_FIELD_RADIO) snprintf(value, sizeof value, "%s", strcmp(f->value, f->on_value) ? _("not selected") : _("selected"));
    else if (type == VIEW_FIELD_COMBO || type == VIEW_FIELD_LIST) {
        size_t n = 0;
        for (int k = 0; k < f->options; k++) {
            const struct view_option *o = &pg->option[f->first_option + k];
            if (o->selected && n + 2 < sizeof value) {
                n += (size_t)snprintf(value + n, sizeof value - n, "%s%.200s", n ? ", " : "", o->label[0] ? o->label : o->value);
                if (n >= sizeof value) n = sizeof value - 1;
            }
        }
        if (!n) snprintf(value, sizeof value, "%.500s", f->value);
    } else snprintf(value, sizeof value, "%.500s", f->value);
    if (f->max_length) {
        snprintf(more, sizeof more, P_(", at most %d character", ", at most %d characters", (unsigned long)f->max_length), f->max_length);
    }
    snprintf(out, size, "%.200s (%s%s%s%s): %.300s", f->name, _(kinds[type]), f->required ? _(", required") : "",
             f->readonly ? _(", read-only") : "", more, value);
}

static void bar_text(int col, int row, const char *text)
{
    char shown[700];
    if (col >= ncols - 1) return;
    fit(shown, sizeof shown, text, ncols - col - 1);
    screen_text(col, row, shown, FG, BG);
}

static void pdf_draw(void)
{
    int rows = pdf_rows();
    if (!rows) return;
    int start = (ax + cw - 1) / cw, row = 2 + tab_rows();
    if (ax > side_px) screen_fill_px(side_px, top + row * ch, ax - side_px, rows * ch, BG);
    screen_fill(start, row, ncols - start, rows, BG);
    if (pdf.toolbar) row += toolbar(row, 1);
    if (forms_bar()) {
        char text[700];
        /* Okular's View > Show Forms, a toggle: shown pressed when on. */
        int col = start + tool_button(start, row, _("Show Forms"), HIT_PDF_BAR + 0, pdf.forms, 1);
        col = ui_button(col, row, "x", HIT_PDF_BAR + 1);
        int page, index;
        const struct view_field *f = focused_field(&page, &index);
        if (pdf.flags & VIEW_PDF_HAS_XFA) snprintf(text, sizeof text, "%s", _("This document has XFA forms, which are currently unsupported."));
        else if (pdf.forms && f) field_text(&doc.page[page], f, text, sizeof text);
        else snprintf(text, sizeof text, "%s", _("This document has forms. Click on the button to interact with them, or use View -> Show Forms."));
        bar_text(col, row++, text);
    }
    if (sig_bar()) {
        int col = ui_button(start, row, _("Show Signatures Panel"), HIT_PDF_BAR + 2);
        col = ui_button(col, row, "x", HIT_PDF_BAR + 3);
        /* Okular's text when not every signature is valid and trusted: none
         * is, with no verifier here. */
        bar_text(col, row, pdf.flags & VIEW_PDF_HAS_SIGNED ?
                 _("This document is digitally signed. Some of the signatures could not be validated properly.") :
                 _("This document has signature placeholder fields."));
    }
}

/* The annotations and fields of the pages, as view-render listed them. */
static int is_markup(int type)
{
    return type == VIEW_ANNOT_HIGHLIGHT || type == VIEW_ANNOT_UNDERLINE || type == VIEW_ANNOT_SQUIGGLY ||
           type == VIEW_ANNOT_STRIKE_OUT;
}

static int annot_editable(const struct view_annot *a)
{
    return a->editable && a->type != VIEW_ANNOT_OTHER && !(a->flags & (AF_READ_ONLY | AF_LOCKED)) &&
           (pdf.flags & VIEW_PDF_CAN_ANNOTATE);
}

/* Okular's canBeMoved: not the highlights, bound to their text. */
static int annot_movable(const struct view_annot *a)
{
    return annot_editable(a) && !is_markup(a->type);
}

/* Okular's canBeResized: inline notes and typewriter text, stamps, shapes. */
static int annot_resizable(const struct view_annot *a)
{
    return annot_editable(a) && (a->type == VIEW_ANNOT_FREE_TEXT || a->type == VIEW_ANNOT_STAMP ||
                                 a->type == VIEW_ANNOT_SQUARE || a->type == VIEW_ANNOT_CIRCLE);
}

static int has_width(int type)
{
    return type == VIEW_ANNOT_LINE || type == VIEW_ANNOT_SQUARE || type == VIEW_ANNOT_CIRCLE || type == VIEW_ANNOT_POLYGON ||
           type == VIEW_ANNOT_POLY_LINE || type == VIEW_ANNOT_INK || type == VIEW_ANNOT_FREE_TEXT;
}

/* Okular's caption of an annotation (gui/guiutils.cpp). */
static const char *annot_caption(const struct view_annot *a)
{
    switch (a->type) {
    case VIEW_ANNOT_TEXT: return _("Pop-up Note");
    case VIEW_ANNOT_FREE_TEXT: return a->width > 0 ? _("Inline Note") : _("Typewriter");
    case VIEW_ANNOT_LINE: return _("Straight Line");
    case VIEW_ANNOT_POLYGON:
    case VIEW_ANNOT_POLY_LINE: return _("Polygon");
    case VIEW_ANNOT_SQUARE: return _("Rectangle");
    case VIEW_ANNOT_CIRCLE: return _("Ellipse");
    case VIEW_ANNOT_HIGHLIGHT: return _("Highlight");
    case VIEW_ANNOT_UNDERLINE: return _("Underline");
    case VIEW_ANNOT_SQUIGGLY: return _("Squiggle");
    case VIEW_ANNOT_STRIKE_OUT: return _("Strike Out");
    case VIEW_ANNOT_STAMP: return _("Stamp");
    case VIEW_ANNOT_INK: return _("Freehand Line");
    }
    static const char *const others[][2] = {
        { "Caret", N_("Caret") }, { "FileAttachment", N_("File Attachment") },
        { "Sound", N_("Sound") }, { "Movie", N_("Movie") },
        { "Screen", N_("Screen") }, { "RichMedia", N_("Rich Media") },
    };
    for (size_t k = 0; k < sizeof others / sizeof others[0]; k++) if (!strcmp(a->subtype, others[k][0])) return _(others[k][1]);
    return _("Annotation");
}

static const struct view_annot *annot_find(int page, int object, int gen, int *index)
{
    if (page < 0 || page >= doc.pages || !object) return NULL;
    const struct page *pg = &doc.page[page];
    for (int k = 0; k < pg->nannot; k++) {
        if (pg->annot[k].object == object && pg->annot[k].generation == gen) {
            if (index) *index = k;
            return &pg->annot[k];
        }
    }
    return NULL;
}

static const struct view_annot *selected(void)
{
    return annot_find(pdf.spage, pdf.sobject, pdf.sgen, NULL);
}

static void unselect(void)
{
    pdf.sobject = 0;
    pdf.pending = 0;
    ui.dirty = 1;
}

static void select_annot(int page, const struct view_annot *a)
{
    pdf.spage = page;
    pdf.sobject = a->object;
    pdf.sgen = a->generation;
    pdf.pending = 0;
    ui.dirty = 1;
}

/* The point's tolerance: 3 pixels at the page's zoom, in points. */
static double slack(int page)
{
    return 3 / (doc.item[page].zoom > 0 ? doc.item[page].zoom : 1);
}

/* The annotation at page's point: the topmost (the last drawn), or -1. */
static int annot_at(int page, double px, double py)
{
    const struct page *pg = &doc.page[page];
    double e = slack(page);
    for (int k = pg->nannot - 1; k >= 0; k--) {
        const struct view_annot *a = &pg->annot[k];
        if (px >= fmin(a->x0, a->x1) - e && px <= fmax(a->x0, a->x1) + e && py >= fmin(a->y0, a->y1) - e &&
            py <= fmax(a->y0, a->y1) + e) return k;
    }
    return -1;
}

static int field_at(int page, double px, double py)
{
    const struct page *pg = &doc.page[page];
    for (int k = pg->nfield - 1; k >= 0; k--) {
        const struct view_field *f = &pg->field[k];
        if (px >= f->x0 && px <= f->x1 && py >= f->y0 && py <= f->y1) return k;
    }
    return -1;
}

static const struct view_field *focused_field(int *page, int *index)
{
    if (!pdf.fobject || pdf.fpage < 0 || pdf.fpage >= doc.pages) return NULL;
    const struct page *pg = &doc.page[pdf.fpage];
    for (int k = 0; k < pg->nfield; k++) {
        if (pg->field[k].object == pdf.fobject && pg->field[k].generation == pdf.fgen) {
            *page = pdf.fpage;
            *index = k;
            return &pg->field[k];
        }
    }
    return NULL;
}

static void to_screen(int page, double x, double y, double *X, double *Y)
{
    struct vl_params p = params();
    vl_page_to_screen(&p, &doc.item[page], doc.pw[page], doc.ph[page], ax, ay, v.sx, v.sy, x, y, X, Y);
}

static void screen_box(int page, double x0, double y0, double x1, double y1, int *X0, int *Y0, int *X1, int *Y1)
{
    double a, b, c, d;
    to_screen(page, x0, y0, &a, &b);
    to_screen(page, x1, y1, &c, &d);
    *X0 = (int)floor(fmin(a, c));
    *Y0 = (int)floor(fmin(b, d));
    *X1 = (int)ceil(fmax(a, c));
    *Y1 = (int)ceil(fmax(b, d));
}

/* A page's point under screen (X, Y), held to page's rectangle (a drag). */
static void page_clamped(int page, int X, int Y, double *px, double *py)
{
    struct vl_params p = params();
    const struct vl_item *it = &doc.item[page];
    double lx = fmax(0, fmin(it->w, X - ax + v.sx - it->x)), ly = fmax(0, fmin(it->h, Y - ay + v.sy - it->y));
    vl_item_to_point(p.rotate, doc.pw[page], doc.ph[page], it->zoom, lx + it->ox, ly + it->oy, px, py);
}

static void handle_point(const struct view_rect *r, int h, double *x, double *y)
{
    *x = h & H_L ? r->x0 : h & H_R ? r->x1 : (r->x0 + r->x1) / 2.0;
    *y = h & H_T ? r->y0 : h & H_B ? r->y1 : (r->y0 + r->y1) / 2.0;
}

static struct view_rect annot_rect(const struct view_annot *a)
{
    return (struct view_rect){ fminf(a->x0, a->x1), fminf(a->y0, a->y1), fmaxf(a->x0, a->x1), fmaxf(a->y0, a->y1), 0 };
}

/* The selected one's resize handle under screen (X, Y): its H_*, or 0.
 * Okular's are 10 pixels square. */
static int handle_at(int X, int Y)
{
    const struct view_annot *a = selected();
    if (!a || !annot_resizable(a)) return 0;
    struct view_rect r = annot_rect(a);
    for (int k = 0; k < 8; k++) {
        double x, y, sx, sy;
        handle_point(&r, handles[k], &x, &y);
        to_screen(pdf.spage, x, y, &sx, &sy);
        if (fabs(X - sx) <= 5 * scale + 1 && fabs(Y - sy) <= 5 * scale + 1) return handles[k];
    }
    return 0;
}

/* The selected one's rectangle as the drag has it. */
static struct view_rect drag_rect(void)
{
    struct view_rect r = pdf.orig;
    double dx = pdf.bx - pdf.ax, dy = pdf.by - pdf.ay;
    double pw = doc.pw[pdf.dpage], ph = doc.ph[pdf.dpage];
    if (!pdf.handle) {
        /* Moved, it stays on its page, as Okular keeps it. */
        if (r.x1 + dx > pw) dx = pw - r.x1;
        if (r.x0 + dx < 0) dx = -r.x0;
        if (r.y1 + dy > ph) dy = ph - r.y1;
        if (r.y0 + dy < 0) dy = -r.y0;
        r.x0 += (float)dx;
        r.x1 += (float)dx;
        r.y0 += (float)dy;
        r.y1 += (float)dy;
        return r;
    }
    if (pdf.handle & H_L) r.x0 += (float)dx;
    if (pdf.handle & H_R) r.x1 += (float)dx;
    if (pdf.handle & H_T) r.y0 += (float)dy;
    if (pdf.handle & H_B) r.y1 += (float)dy;
    struct view_rect n = { fminf(r.x0, r.x1), fminf(r.y0, r.y1), fmaxf(r.x0, r.x1), fmaxf(r.y0, r.y1), 0 };
    if (n.x1 - n.x0 < 1) n.x1 = n.x0 + 1; /* a rectangle with an area */
    if (n.y1 - n.y0 < 1) n.y1 = n.y0 + 1;
    return n;
}

/* Overlays (view.c's): boxes in screen pixels. */
static void ovl_box(int x0, int y0, int x1, int y1, uint32_t rgb, int alpha)
{
    if (x1 <= x0 || y1 <= y0) return;
    if (novl == ovl_size) {
        struct overlay *o = realloc(ovl, sizeof *o * (size_t)(ovl_size + 64));
        if (!o) return;
        ovl = o;
        ovl_size += 64;
    }
    ovl[novl++] = (struct overlay){ x0, y0, x1, y1, alpha, rgb };
}

static void ovl_frame(int x0, int y0, int x1, int y1, int t, uint32_t rgb, int alpha)
{
    ovl_box(x0 - t, y0 - t, x1 + t, y0, rgb, alpha);
    ovl_box(x0 - t, y1, x1 + t, y1 + t, rgb, alpha);
    ovl_box(x0 - t, y0, x0, y1, rgb, alpha);
    ovl_box(x1, y0, x1 + t, y1, rgb, alpha);
}

static void ovl_segment(int page, double xa, double ya, double xb, double yb, uint32_t rgb)
{
    double X0, Y0, X1, Y1;
    to_screen(page, xa, ya, &X0, &Y0);
    to_screen(page, xb, yb, &X1, &Y1);
    int steps = (int)(fmax(fabs(X1 - X0), fabs(Y1 - Y0)) / 2) + 1;
    for (int k = 0; k <= steps && k < 4000; k++) {
        int x = (int)lround(X0 + (X1 - X0) * k / steps), y = (int)lround(Y0 + (Y1 - Y0) * k / steps);
        ovl_box(x - 1, y - 1, x + 1, y + 1, rgb, 256);
    }
}

/* A drag's end: with Shift, Okular's square shapes and 15 degree lines. */
static void constrained(double *bx, double *by)
{
    double dx = *bx - pdf.ax, dy = *by - pdf.ay;
    if (!(keys_modifiers() & KEYS_SHIFT)) return;
    if (pdf.tool == T_RECT || pdf.tool == T_ELLIPSE) {
        double side = fmax(fabs(dx), fabs(dy));
        *bx = pdf.ax + (dx < 0 ? -side : side);
        *by = pdf.ay + (dy < 0 ? -side : side);
    } else if (pdf.tool == T_LINE || pdf.tool == T_POLYGON) {
        double len = hypot(dx, dy), step = M_PI / 12, angle = round(atan2(dy, dx) / step) * step;
        *bx = pdf.ax + len * cos(angle);
        *by = pdf.ay + len * sin(angle);
    }
}

static void pdf_overlays(int page)
{
    if (!pdf_on()) return;
    const struct page *pg = &doc.page[page];
    int X0, Y0, X1, Y1, fpage = -1, findex = -1;
    if (pdf.forms) {
        focused_field(&fpage, &findex);
        for (int k = 0; k < pg->nfield; k++) {
            const struct view_field *f = &pg->field[k];
            screen_box(page, f->x0, f->y0, f->x1, f->y1, &X0, &Y0, &X1, &Y1);
            ovl_box(X0, Y0, X1, Y1, f->readonly ? 0x808080 : 0x80B0FF, 50);
            if (f->required) ovl_frame(X0, Y0, X1, Y1, 1, 0xE04040, 256);
            if (page == fpage && k == findex) ovl_frame(X0, Y0, X1, Y1, 2, ACCENT, 256);
        }
    }
    const struct view_annot *a = selected();
    if (a && pdf.spage == page) {
        struct view_rect r = pdf.pending ? pdf.pending_rect : pdf.drag == D_SELECT ? drag_rect() : annot_rect(a);
        screen_box(page, r.x0, r.y0, r.x1, r.y1, &X0, &Y0, &X1, &Y1);
        ovl_frame(X0, Y0, X1, Y1, 1, SELECTED, 256);
        for (int k = 0; annot_resizable(a) && k < 8; k++) {
            double x, y, sx, sy;
            handle_point(&r, handles[k], &x, &y);
            to_screen(page, x, y, &sx, &sy);
            int h = (int)lround(5 * scale);
            ovl_box((int)sx - h, (int)sy - h, (int)sx + h, (int)sy + h, SELECTED, 256);
        }
    }
    if (pdf.drag == D_NONE || pdf.drag == D_SELECT || pdf.dpage != page) return;
    uint32_t rgb = pdf.tool ? pdf.set[pdf.tool].rgb : ACCENT;
    if (pdf.tool == T_INK) {
        for (int k = 0; k + 1 < pdf.npts; k++)
            ovl_segment(page, pdf.pts[2 * k], pdf.pts[2 * k + 1], pdf.pts[2 * k + 2], pdf.pts[2 * k + 3], rgb);
    } else if (pdf.tool == T_LINE || pdf.tool == T_POLYGON) {
        double bx = pdf.bx, by = pdf.by;
        for (int k = 0; k + 1 < pdf.npts; k++)
            ovl_segment(page, pdf.pts[2 * k], pdf.pts[2 * k + 1], pdf.pts[2 * k + 2], pdf.pts[2 * k + 3], rgb);
        constrained(&bx, &by); /* from ax, ay: the line's start, the polygon's last corner */
        ovl_segment(page, pdf.ax, pdf.ay, bx, by, rgb);
    } else {
        double bx = pdf.bx, by = pdf.by;
        constrained(&bx, &by);
        screen_box(page, pdf.ax, pdf.ay, bx, by, &X0, &Y0, &X1, &Y1);
        ovl_frame(X0, Y0, X1, Y1, 1, is_markup(tools[pdf.tool].type) ? SELECTED : rgb, 256);
    }
}

/* The document: opened, closed, and view-render's answers. */
static void pdf_forget(void)
{
    free(pdf.sig);
    pdf.sig = NULL;
    pdf.nsig = pdf.sigs = pdf.nsig_fields = 0;
    pdf.attached = 0;
    pdf.flags = 0;
    pdf.revision = 0;
    pdf.drag = pdf.npts = 0;
    pdf.sobject = pdf.pending = pdf.fobject = 0;
    pdf.forms = pdf.forms_closed = pdf.sigbar_closed = 0;
    pdf.saving = pdf.discard = 0;
    pdf.asel = pdf.atop = pdf.ssel = pdf.stop = 0;
    /* The toolbar and its tool stay, as Okular's, for the next document. */
}

static void pdf_opened(int again)
{
    if (strcmp(doc.format, "PDF") != 0) return;
    if (again) {
        if (pdf_modified())
            /* TRANSLATORS: %.200s is the file name. */
            toast(1, _("The document renderer stopped: the unsaved changes to %.200s were lost."), base_name(doc.path));
        int rows = pdf_rows();
        for (int i = 0; i < doc.pages; i++) doc.page[i].annots = doc.page[i].fields = 0;
        pdf.attached = 0;
        pdf.flags = 0;
        pdf.sigs = 0;
        pdf.drag = pdf.pending = pdf.saving = pdf.resume = 0;
        if (pdf_rows() != rows) pdf_relayout();
    }
    struct job j = new_job(J_PDF_STATUS, VIEW_PDF_STATUS);
    push_job(&j, 0);
}

/* A reply's state of the session; with a new revision, the pages anew. */
static void pdf_state(const struct view_reply *rep)
{
    int rows = pdf_rows();
    pdf.attached = 1;
    pdf.flags = rep->flags & ~VIEW_PDF_MORE;
    if (rep->revision != pdf.revision) {
        pdf.revision = rep->revision;
        for (int i = 0; i < doc.pages; i++) doc.page[i].annots = doc.page[i].fields = 0;
        if (pdf.sigs == 2) pdf.sigs = 0;
        pix_flush();
    }
    if (pdf_rows() != rows) pdf_relayout();
    ui.dirty = 1;
}

static int pdf_background(struct job *j)
{
    if (!pdf_on()) return 0;
    if (pdf.nsig_fields && !pdf.sigs) {
        *j = new_job(J_PDF_SIGS, VIEW_PDF_SIGNATURES);
        pdf.sigs = 1;
        return 1;
    }
    for (int k = 0; k < nvis; k++) {
        struct page *pg = &doc.page[prio[k]];
        if (!pg->annots) {
            *j = pdf_job(J_PDF_ANNOTS, VIEW_PDF_ANNOTS, prio[k]);
            pg->annots = 1;
            return 1;
        }
        if (!pg->fields && (pdf.flags & VIEW_PDF_HAS_FORMS)) {
            *j = pdf_job(J_PDF_FIELDS, VIEW_PDF_FIELDS, prio[k]);
            pg->fields = 1;
            return 1;
        }
    }
    /* All the pages': for Tab through the fields, and the annotations panel. */
    for (int i = 0; i < doc.pages; i++) {
        struct page *pg = &doc.page[i];
        if (pdf.forms && !pg->fields) {
            *j = pdf_job(J_PDF_FIELDS, VIEW_PDF_FIELDS, i);
            pg->fields = 1;
            return 1;
        }
        if (side_cols && ui.tab == TAB_ANNOTATIONS && !pg->annots) {
            *j = pdf_job(J_PDF_ANNOTS, VIEW_PDF_ANNOTS, i);
            pg->annots = 1;
            return 1;
        }
    }
    return 0;
}

/* n records of size bytes from a reply, copied (NULL: none, or garbled). */
static void *records(const void *data, size_t size, int n, size_t item)
{
    if (n <= 0 || !data || (size_t)n > size / item) return NULL;
    void *p = malloc((size_t)n * item);
    if (p) memcpy(p, data, (size_t)n * item);
    return p;
}

static void got_annots(const struct job *j, const struct view_reply *rep, const void *data, size_t size)
{
    struct page *pg = &doc.page[j->page];
    if (!rep->status) pdf_state(rep);
    free(pg->annot);
    pg->annot = rep->status ? NULL : records(data, size, rep->count, sizeof *pg->annot);
    pg->nannot = pg->annot ? rep->count : 0;
    for (int k = 0; k < pg->nannot; k++) {
        struct view_annot *a = &pg->annot[k];
        a->subtype[sizeof a->subtype - 1] = a->icon[sizeof a->icon - 1] = a->author[sizeof a->author - 1] = '\0';
        a->contents[sizeof a->contents - 1] = '\0';
        if (!isfinite(a->x0) || !isfinite(a->y0) || !isfinite(a->x1) || !isfinite(a->y1)) a->x0 = a->y0 = a->x1 = a->y1 = 0;
    }
    pg->annots = 2;
    if (pdf.sobject && pdf.spage == j->page && !selected()) unselect();
    ui.dirty = 1;
}

static void got_fields(const struct job *j, const struct view_reply *rep, const void *data, size_t size)
{
    struct page *pg = &doc.page[j->page];
    if (!rep->status) pdf_state(rep);
    free(pg->field);
    free(pg->option);
    pg->field = NULL;
    pg->option = NULL;
    pg->nfield = pg->noption = 0;
    size_t fields = rep->count > 0 ? (size_t)rep->count * sizeof *pg->field : 0;
    if (!rep->status && data && rep->count > 0 && rep->w >= 0 && fields <= size &&
        (size_t)rep->w <= (size - fields) / sizeof *pg->option) {
        pg->field = records(data, size, rep->count, sizeof *pg->field);
        pg->option = rep->w ? records((const char *)data + fields, size - fields, rep->w, sizeof *pg->option) : NULL;
        pg->nfield = pg->field ? rep->count : 0;
        pg->noption = pg->option ? rep->w : 0;
    }
    for (int k = 0; k < pg->nfield; k++) {
        struct view_field *f = &pg->field[k];
        f->name[sizeof f->name - 1] = f->on_value[sizeof f->on_value - 1] = f->value[sizeof f->value - 1] = '\0';
        if (f->first_option < 0 || f->options < 0 || f->first_option > pg->noption || f->options > pg->noption - f->first_option)
            f->first_option = f->options = 0;
        if (!isfinite(f->x0) || !isfinite(f->y0) || !isfinite(f->x1) || !isfinite(f->y1)) f->x0 = f->y0 = f->x1 = f->y1 = 0;
    }
    for (int k = 0; k < pg->noption; k++) {
        pg->option[k].label[sizeof pg->option[k].label - 1] = pg->option[k].value[sizeof pg->option[k].value - 1] = '\0';
    }
    pg->fields = 2;
    ui.dirty = 1;
}

static void got_signatures(const struct view_reply *rep, const void *data, size_t size)
{
    free(pdf.sig);
    pdf.sig = rep->status ? NULL : records(data, size, rep->count, sizeof *pdf.sig);
    pdf.nsig = pdf.sig ? rep->count : 0;
    for (int k = 0; k < pdf.nsig; k++) {
        struct view_signature *s = &pdf.sig[k];
        s->field[sizeof s->field - 1] = s->name[sizeof s->name - 1] = s->reason[sizeof s->reason - 1] = '\0';
        s->location[sizeof s->location - 1] = s->date[sizeof s->date - 1] = '\0';
        s->signatory[sizeof s->signatory - 1] = s->error[sizeof s->error - 1] = '\0';
    }
    pdf.sigs = 2;
    ui.dirty = 1;
}

static void got_edit(const struct job *j, const struct view_reply *rep)
{
    int op = (int)j->req.op;
    if (op == VIEW_PDF_MOVE) pdf.pending = 0;
    if (rep->status) {
        if (op == VIEW_PDF_SET)
            /* TRANSLATORS: %.80s is the rejected value; %.200s is the reason from the renderer. */
            toast(1, _("\"%.80s\" was not accepted, the field keeps its value: %.200s"), j->req.text, rep->text);
        else if (op == VIEW_PDF_CREATE) toast(1, _("Cannot add the annotation: %.200s"), rep->text);
        else if (op == VIEW_PDF_UNDO || op == VIEW_PDF_REDO) toast(0, "%.200s.", rep->text);
        else toast(1, _("Cannot change the annotation: %.200s"), rep->text);
        ui.dirty = 1;
        return;
    }
    pdf_state(rep);
    if (op == VIEW_PDF_DELETE && j->req.object == pdf.sobject && j->req.generation == pdf.sgen) unselect();
    if (op == VIEW_PDF_CREATE && !pdf.keep) pdf.tool = T_NONE;
}

static void pdf_resume(void);

static void got_write(const struct job *j, const struct view_reply *rep)
{
    pdf.saving = 0;
    if (rep->status == EPERM) {
        char q[600];
        str_copy(pdf.save_path, sizeof pdf.save_path, j->req.text);
        pdf.save_as = j->tx;
        /* TRANSLATORS: %.300s is the file name; y and N are keys; keep y/N. */
        snprintf(q, sizeof q, _("Saving the changes will invalidate the digital signatures of %.300s. Save anyway? y/N"),
                 base_name(doc.path));
        prompt_open(P_PDF_CONSENT, q, "", 0);
        return;
    }
    if (rep->status) {
        /* TRANSLATORS: the first %.200s is the file path; the second is the reason. */
        toast(1, _("File could not be saved in '%.200s': %.200s. Try to save it to another location."), j->req.text, rep->text);
        pdf.resume = 0;
        return;
    }
    pdf_state(rep);
    if (j->tx) {
        /* Save As: the document is that file now, as in Okular. */
        str_copy(doc.path, sizeof doc.path, j->req.text);
        if (tabs.n && tabs.current < tabs.n) {
            char *p = strdup(doc.path);
            if (p) {
                free(tabs.path[tabs.current]);
                tabs.path[tabs.current] = p;
            }
        }
        ui.changed = now();
    }
    toast(0, _("Saved %.200s."), base_name(j->req.text));
    if (pdf.resume) pdf_resume();
}

static void pdf_reply(const struct job *j, const struct view_reply *rep, const void *data, size_t size)
{
    if (j->gen != doc.gen || !doc.open) return;
    int page_ok = j->page >= 0 && j->page < doc.pages;
    switch (j->kind) {
    case J_PDF_STATUS:
        if (rep->status) {
            pdf.attached = 0; /* not a PDF view-render edits */
            break;
        }
        pdf.nsig_fields = rep->count > 0 ? rep->count : 0;
        pdf_state(rep);
        break;
    case J_PDF_ANNOTS:
        if (page_ok) got_annots(j, rep, data, size);
        break;
    case J_PDF_FIELDS:
        if (page_ok) got_fields(j, rep, data, size);
        break;
    case J_PDF_SIGS:
        got_signatures(rep, data, size);
        break;
    case J_PDF_EDIT:
        got_edit(j, rep);
        break;
    case J_PDF_WRITE:
        got_write(j, rep);
        break;
    }
}

/* Saving: to path, as the document's new file (as) or not. */
static void pdf_write(const char *path, int consent, int as)
{
    if (strlen(path) >= VIEW_TEXT) {
        /* TRANSLATORS: %.200s is the file path. */
        toast(1, _("The path %.200s is too long."), path);
        pdf.resume = 0;
        return;
    }
    struct job j = new_job(J_PDF_WRITE, VIEW_PDF_WRITE);
    str_copy(j.req.text, sizeof j.req.text, path);
    j.req.mode = consent ? VIEW_PDF_INVALIDATE_SIGNATURES : 0;
    j.tx = as;
    push_job(&j, 0);
    pdf.saving = 1;
}

/* An action closing the document with unsaved changes: held, and asked
 * about (Okular's "Close Document" question). 1: held. */
static int pdf_guard(int action, int arg)
{
    if (!pdf_modified() || pdf.discard) return 0;
    if (prompt.kind == P_PDF_UNSAVED || pdf.saving) return 1;
    char q[600];
    pdf.resume = action;
    pdf.resume_arg = arg;
    /* TRANSLATORS: %.300s is the file name; s, d and Esc are keys; keep them. */
    snprintf(q, sizeof q, _("Do you want to save your changes to \"%.300s\" or discard them? s: Save, d: Discard, Esc: Cancel"),
             base_name(doc.path));
    prompt_open(P_PDF_UNSAVED, q, "", 0);
    return 1;
}

/* The action held, now the changes are saved or discarded. */
static void pdf_resume(void)
{
    int a = pdf.resume, arg = pdf.resume_arg;
    pdf.resume = 0;
    pdf.discard = 1;
    if (a == A_TAB_SWITCH) tab_switch(arg);
    else if (a == A_CLOSE_TAB) {
        if (arg >= 0 && arg != tabs.current) tab_switch(arg);
        tab_close();
    } else do_action(a);
    pdf.discard = 0;
}

/* What is typed: the bottom row, view's line editor (view.c's struct line:
 * UTF-8, selection, the clipboard and the input method). */
static void edit_open(int what, const char *question, const char *initial, size_t limit, int max, int multiline)
{
    pdf.ed.what = what;
    prompt_open(P_PDF_TEXT, question, "", 0);
    prompt.in.limit = limit < sizeof prompt.in.text ? limit : sizeof prompt.in.text - 1;
    /* What is there already is kept whole (else it is not edited here):
     * no MaxLen while it is set, then the cursor at its end. */
    char kept[VIEW_TEXT];
    str_copy(kept, prompt.in.limit + 1, initial ? initial : "");
    prompt.in.multiline = 1;
    line_set(&prompt.in, kept, 0);
    prompt.in.max = max;
    prompt.in.multiline = multiline;
}

/* Typed or pasted, at most MaxLen characters (Okular's
 * QLineEdit::setMaxLength: what goes over is not taken). */
static void pdf_insert(const char *s, size_t len)
{
    if (pdf_editing()) line_insert(&prompt.in, s, len);
}

static void edit_commit(void)
{
    char text[VIEW_TEXT];
    int what = pdf.ed.what;
    str_copy(text, sizeof text, prompt.in.text);
    pdf.ed.what = E_NONE;
    prompt_close();
    struct job j;
    if (what == E_CONTENTS || what == E_AUTHOR) {
        j = pdf_job(J_PDF_EDIT, VIEW_PDF_UPDATE, pdf.ed.page);
        j.req.object = pdf.ed.object;
        j.req.generation = pdf.ed.gen;
        j.req.mask = what == E_CONTENTS ? VIEW_SET_CONTENTS : VIEW_SET_AUTHOR;
        if (what == E_CONTENTS) str_copy(j.req.text, sizeof j.req.text, text);
        else str_copy(j.req.author, sizeof j.req.author, text);
    } else if (what == E_FIELD) {
        j = pdf_job(J_PDF_EDIT, VIEW_PDF_SET, pdf.ed.page);
        j.req.object = pdf.ed.object;
        j.req.generation = pdf.ed.gen;
        j.req.count = 1;
        str_copy(j.req.text, sizeof j.req.text, text);
    } else if (what == E_NEW) {
        j = pdf.ed.create;
        str_copy(j.req.text, sizeof j.req.text, text);
    } else {
        return;
    }
    push_job(&j, 0);
}

static int pdf_draw_bottom(int row)
{
    if (prompt.kind != P_PDF_TEXT) return 0;
    char question[200], counter[32] = "";
    fit(question, sizeof question, prompt.question, ncols / 3 > 8 ? ncols / 3 : 8);
    screen_text(1, row, question, ACCENT, BG);
    int col = 2 + utf_cells(question, 0, strlen(question));
    if (prompt.in.max) snprintf(counter, sizeof counter, "%d/%d", utf8_count(prompt.in.text), prompt.in.max);
    int width = ncols - col - (int)strlen(counter) - 2;
    if (width < 4) width = 4;
    line_draw(&prompt.in, col, row, width, ui.focus == FOCUS_PROMPT, FG, 0x202838);
    ui_hit(col, row, width, 1, HIT_PROMPT_INPUT);
    if (counter[0]) screen_text(col + width + 1, row, counter, DIM, BG);
    return 1;
}

static void edit_key(int key, int mods)
{
    if (key == '\n') {
        if ((mods & KEYS_SHIFT) && prompt.in.multiline) pdf_insert("\n", 1);
        else edit_commit();
    } else if (key == 27) {
        pdf.ed.what = E_NONE;
        prompt_close();
    } else {
        line_key(&prompt.in, key, mods);
    }
    ui.dirty = 1;
}

/* Form fields. */
static int can_fill(void)
{
    if (pdf.flags & VIEW_PDF_CAN_FILL) return 1;
    toast(1, _("This document does not allow filling in its forms."));
    return 0;
}

static void field_set(int page, const struct view_field *f, int count, const char *const *values)
{
    struct job j = pdf_job(J_PDF_EDIT, VIEW_PDF_SET, page);
    size_t at = 0;
    j.req.object = f->object;
    j.req.generation = f->generation;
    j.req.count = count;
    for (int i = 0; i < count; i++) {
        size_t len = strlen(values[i]);
        if (at + len + 1 > sizeof j.req.text - 1) {
            /* TRANSLATORS: %.200s is the form field's name. */
            toast(1, _("The values of %.200s are too long to set."), f->name);
            return;
        }
        memcpy(j.req.text + at, values[i], len + 1);
        at += len + 1;
    }
    push_job(&j, 0);
}

static void focus_field(int page, const struct view_field *f)
{
    pdf.fpage = page;
    pdf.fobject = f->object;
    pdf.fgen = f->generation;
    int X0, Y0, X1, Y1;
    screen_box(page, f->x0, f->y0, f->x1, f->y1, &X0, &Y0, &X1, &Y1);
    if (X0 < ax || Y0 < ay || X1 > ax + aw || Y1 > ay + ah) go(pos_point(page, 0, fmax(0, f->y0 - 24)));
    ui.dirty = 1;
}

/* A choice's options in a menu; chosen: set (a multiple selection's
 * option toggled). Okular's combo box and list box. */
static void choice_menu(int page, const struct view_field *f, int col, int row)
{
    const struct page *pg = &doc.page[page];
    if (f->truncated) {
        /* TRANSLATORS: %.200s is the form field's name. */
        toast(1, _("The options of %.200s are too long to choose here."), f->name);
        return;
    }
    struct item it[24];
    char labels[24][96];
    int n = 0;
    for (int k = 0; k < f->options && n < 23; k++) {
        const struct view_option *o = &pg->option[f->first_option + k];
        snprintf(labels[n], sizeof labels[n], "%.90s", o->label[0] ? o->label : o->value);
        it[n] = (struct item){ labels[n], "", k + 1, o->selected };
        n++;
    }
    if (f->options > 23) toast(0, _("Only the first 23 of the %d options are listed."), f->options);
    if (f->editable_choice && n < 24) it[n++] = (struct item){ _("Other value..."), "", 1000, 0 };
    if (!n) return;
    int a = run_menu(col, row, it, n);
    if (a == 1000) {
        char q[300];
        snprintf(q, sizeof q, "%.200s:", f->name);
        pdf.ed.page = page;
        pdf.ed.object = f->object;
        pdf.ed.gen = f->generation;
        edit_open(E_FIELD, q, f->value, VIEW_TEXT - 1, 0, 0);
        return;
    }
    if (a < 1 || a > f->options || pg->option[f->first_option + a - 1].truncated) return;
    const struct view_option *chosen = &pg->option[f->first_option + a - 1];
    if (!f->multiselect) {
        const char *v[] = { chosen->value };
        field_set(page, f, 1, v);
        return;
    }
    const char *v[64];
    int count = 0;
    for (int k = 0; k < f->options && count < 64; k++) {
        const struct view_option *o = &pg->option[f->first_option + k];
        if ((o == chosen) != (o->selected != 0)) v[count++] = o->value;
    }
    field_set(page, f, count, v);
}

/* The field used: Okular's widget's click, or Space on it. */
static void field_activate(int page, int index, int col, int row)
{
    struct page *pg = &doc.page[page];
    const struct view_field *f = &pg->field[index];
    char q[300];
    if (f->readonly) {
        /* TRANSLATORS: %.200s is the form field's name. */
        toast(0, _("%.200s is read-only."), f->name);
        return;
    }
    focus_field(page, f);
    switch (f->type) {
    case VIEW_FIELD_TEXT:
        if (!can_fill()) return;
        if (f->truncated) {
            /* TRANSLATORS: %.200s is the form field's name. */
            toast(1, _("The text of %.200s is too long to edit here."), f->name);
            return;
        }
        snprintf(q, sizeof q, "%.200s:", f->name);
        pdf.ed.page = page;
        pdf.ed.object = f->object;
        pdf.ed.gen = f->generation;
        edit_open(E_FIELD, q, f->value, VIEW_TEXT - 1, f->max_length, (f->flags & TX_MULTILINE) != 0);
        break;
    case VIEW_FIELD_CHECKBOX:
        if (can_fill()) {
            const char *v[] = { strcmp(f->value, f->on_value) ? f->on_value : "Off" };
            field_set(page, f, 1, v);
        }
        break;
    case VIEW_FIELD_RADIO:
        /* A radio button chosen again stays chosen, as Qt's. */
        if (can_fill() && strcmp(f->value, f->on_value) != 0) {
            const char *v[] = { f->on_value };
            field_set(page, f, 1, v);
        }
        break;
    case VIEW_FIELD_COMBO:
    case VIEW_FIELD_LIST:
        if (can_fill()) choice_menu(page, f, col, row);
        break;
    case VIEW_FIELD_SIGNATURE:
        if (!settings.sidebar) sidebar(1);
        ui.tab = TAB_SIGNATURES;
        break;
    default:
        toast(0, _("Push buttons and their actions are not run here."));
        break;
    }
}

/* The screen cell under a field, for its menu. */
static void field_cell(int page, const struct view_field *f, int *col, int *row)
{
    int X0, Y0, X1, Y1;
    screen_box(page, f->x0, f->y0, f->x1, f->y1, &X0, &Y0, &X1, &Y1);
    *col = clampi(X0 / cw, 0, ncols - 1);
    *row = clampi((Y1 - top) / ch, 0, nrows - 1);
}

/* Tab and Shift+Tab: the next or previous field, round (Qt's focus chain). */
static void focus_next(int dir)
{
    int count = 0, at = -1, fpage = -1, findex = -1;
    focused_field(&fpage, &findex);
    for (int i = 0; i < doc.pages; i++) {
        for (int k = 0; k < doc.page[i].nfield; k++) {
            const struct view_field *f = &doc.page[i].field[k];
            if (f->readonly || f->type == VIEW_FIELD_OTHER) continue;
            if (i == fpage && k == findex) at = count;
            count++;
        }
    }
    if (!count) {
        toast(0, _("No fields to fill in yet."));
        return;
    }
    int want = at < 0 ? (dir > 0 ? 0 : count - 1) : ((at + dir) % count + count) % count;
    for (int i = 0, n = 0; i < doc.pages; i++) {
        for (int k = 0; k < doc.page[i].nfield; k++) {
            const struct view_field *f = &doc.page[i].field[k];
            if (f->readonly || f->type == VIEW_FIELD_OTHER) continue;
            if (n++ == want) {
                focus_field(i, f);
                return;
            }
        }
    }
}

static int field_key(int key, int mods)
{
    int page, index;
    const struct view_field *f = focused_field(&page, &index);
    if (!f) return 0;
    int col, row, plain = !(mods & (KEYS_CTRL | KEYS_ALT));
    field_cell(page, f, &col, &row);
    if (f->type == VIEW_FIELD_TEXT) {
        if (key == '\n' || key == K_F1 + 1) {
            field_activate(page, index, col, row);
            return 1;
        }
        const char *typed = keys_typed(key);
        if (plain && (typed || key == '\b' || key == K_DEL)) {
            /* Tabbed into, a line edit has its text selected: typing replaces it. */
            field_activate(page, index, col, row);
            if (pdf_editing()) {
                prompt.in.text[0] = '\0';
                prompt.in.cursor = prompt.in.mark = 0;
                if (typed) {
                    pdf_insert(typed, strlen(typed));
                }
            }
            return 1;
        }
        return 0;
    }
    if (key == ' ' || (key == '\n' && f->type != VIEW_FIELD_CHECKBOX && f->type != VIEW_FIELD_RADIO)) {
        field_activate(page, index, col, row);
        return 1;
    }
    if ((key == K_UP || key == K_DOWN) && (f->type == VIEW_FIELD_COMBO || (f->type == VIEW_FIELD_LIST && !f->multiselect))) {
        /* Qt's combo box: the previous or next option. */
        const struct page *pg = &doc.page[page];
        int cur = -1;
        for (int k = 0; k < f->options; k++) if (pg->option[f->first_option + k].selected) cur = k;
        int next = clampi(cur + (key == K_DOWN ? 1 : -1), 0, f->options - 1);
        if (f->options && next != cur && can_fill() && !f->truncated) {
            const char *v[] = { pg->option[f->first_option + next].value };
            field_set(page, f, 1, v);
        }
        return 1;
    }
    return 0;
}

/* Annotations: made with the tools, and changed. */
static struct job create_job(int tool, int page)
{
    struct job j = pdf_job(J_PDF_EDIT, VIEW_PDF_CREATE, page);
    uint32_t rgb = pdf.set[tool].rgb;
    j.req.mode = tools[tool].type;
    j.req.color_n = 3;
    j.req.color[0] = (float)(rgb >> 16 & 0xFF) / 255;
    j.req.color[1] = (float)(rgb >> 8 & 0xFF) / 255;
    j.req.color[2] = (float)(rgb & 0xFF) / 255;
    j.req.opacity = pdf.set[tool].opacity;
    j.req.width = pdf.set[tool].width;
    str_copy(j.req.author, sizeof j.req.author, user_name());
    if (tool == T_STAMP) str_copy(j.req.icon, sizeof j.req.icon, pdf.stamp);
    if (tool == T_POPUP) str_copy(j.req.icon, sizeof j.req.icon, "Note");
    return j;
}

static void job_rect(struct job *j, double x0, double y0, double x1, double y1)
{
    j->req.ax = (float)fmin(x0, x1);
    j->req.ay = (float)fmin(y0, y1);
    j->req.bx = (float)fmax(x0, x1);
    j->req.by = (float)fmax(y0, y1);
}

static void add_point(double x, double y)
{
    if (pdf.npts == VIEW_PDF_POINTS) {
        /* Full: every other point kept, the stroke's shape with half of them. */
        for (int k = 1; k < VIEW_PDF_POINTS / 2; k++) {
            pdf.pts[2 * k] = pdf.pts[4 * k];
            pdf.pts[2 * k + 1] = pdf.pts[4 * k + 1];
        }
        pdf.npts = VIEW_PDF_POINTS / 2;
    }
    pdf.pts[2 * pdf.npts] = (float)x;
    pdf.pts[2 * pdf.npts + 1] = (float)y;
    pdf.npts++;
}

static void create_points(int tool)
{
    struct job j = create_job(tool, pdf.dpage);
    j.req.count = pdf.npts;
    if (tool == T_INK) {
        j.req.strokes = 1;
        j.req.stroke[0] = pdf.npts;
    }
    memcpy(j.req.points, pdf.pts, sizeof pdf.pts[0] * 2 * (size_t)pdf.npts);
    push_job(&j, 0);
}

/* A drawing tool's drag let go (or a click). */
static void tool_done(void)
{
    int tool = pdf.tool, moved = pdf.moved >= 3 * scale;
    double bx = pdf.bx, by = pdf.by;
    constrained(&bx, &by);
    struct job j = create_job(tool, pdf.dpage);
    if (is_markup(tools[tool].type)) {
        if (!moved) return;
        j.req.ax = (float)pdf.ax;
        j.req.ay = (float)pdf.ay;
        j.req.bx = (float)bx;
        j.req.by = (float)by;
        push_job(&j, 0);
    } else if (tool == T_POPUP || tool == T_TYPEWRITER || tool == T_INLINE) {
        if (tool == T_POPUP) job_rect(&j, pdf.ax - 10, pdf.ay - 10, pdf.ax + 10, pdf.ay + 10);
        else if (!moved) {
            toast(0, _("Drag to place the text's corners."));
            return;
        } else job_rect(&j, pdf.ax, pdf.ay, bx, by);
        pdf.ed.create = j;
        edit_open(E_NEW, tool == T_POPUP ? _("Pop-up Note text:") : tool == T_TYPEWRITER ? _("Typewriter text:") : _("Inline Note text:"),
                  "", VIEW_TEXT - 1, 0, 1);
    } else if (tool == T_RECT || tool == T_ELLIPSE) {
        if (!moved) return;
        job_rect(&j, pdf.ax, pdf.ay, bx, by);
        push_job(&j, 0);
    } else if (tool == T_STAMP) {
        /* A click: Okular's square stamp of 64. */
        if (moved) job_rect(&j, pdf.ax, pdf.ay, bx, by);
        else job_rect(&j, pdf.ax - 32, pdf.ay - 32, pdf.ax + 32, pdf.ay + 32);
        push_job(&j, 0);
    } else if (tool == T_INK) {
        if (pdf.npts >= 2) create_points(T_INK);
    } else if (tool == T_LINE) {
        if (!moved) {
            pdf.drag = D_LINE; /* its end placed by the next click */
            return;
        }
        pdf.npts = 0;
        add_point(pdf.ax, pdf.ay);
        add_point(bx, by);
        create_points(T_LINE);
    }
    pdf.npts = 0;
}

static void tool_press(const struct pointer_event *e, int page, double px, double py)
{
    if (!(pdf.flags & VIEW_PDF_CAN_ANNOTATE)) {
        toast(1, _("This document does not allow annotations."));
        return;
    }
    if (page < 0) return;
    tool_settings();
    pdf.dpage = page;
    pdf.ax = pdf.bx = px;
    pdf.ay = pdf.by = py;
    pdf.press_x = e->x;
    pdf.press_y = e->y;
    pdf.moved = 0;
    pdf.npts = 0;
    if (pdf.tool == T_POLYGON) {
        pdf.drag = D_POLYGON;
        add_point(px, py);
        return;
    }
    pdf.drag = D_DRAW;
    if (pdf.tool == T_INK) add_point(px, py);
}

/* A click while a line's end or a polygon's corners are placed. */
static void tool_click(const struct pointer_event *e)
{
    double bx = pdf.bx, by = pdf.by;
    constrained(&bx, &by);
    if (pdf.drag == D_LINE) {
        pdf.drag = D_NONE;
        pdf.npts = 0;
        add_point(pdf.ax, pdf.ay);
        add_point(bx, by);
        create_points(T_LINE);
        pdf.npts = 0;
        return;
    }
    /* Okular's polygon: closed by a click on its first corner. */
    double fx, fy;
    to_screen(pdf.dpage, pdf.pts[0], pdf.pts[1], &fx, &fy);
    if (pdf.npts >= 3 && fabs(e->x - fx) <= 8 * scale && fabs(e->y - fy) <= 8 * scale) {
        pdf.drag = D_NONE;
        create_points(T_POLYGON);
        pdf.npts = 0;
        return;
    }
    if (pdf.npts < VIEW_PDF_POINTS) add_point(bx, by);
    pdf.ax = bx;
    pdf.ay = by;
}

/* Events while a press is held, or a line or polygon is placed. */
static int drag_event(const struct pointer_event *e)
{
    if (e->wheel || e->hwheel || e->gesture) return 0;
    page_clamped(pdf.dpage, e->x, e->y, &pdf.bx, &pdf.by);
    int m = abs(e->x - pdf.press_x) > abs(e->y - pdf.press_y) ? abs(e->x - pdf.press_x) : abs(e->y - pdf.press_y);
    if (m > pdf.moved) pdf.moved = m;
    if (!e->button) {
        if (pdf.drag == D_DRAW && pdf.tool == T_INK && pdf.npts) {
            double lx, ly, X, Y;
            to_screen(pdf.dpage, pdf.pts[2 * pdf.npts - 2], pdf.pts[2 * pdf.npts - 1], &lx, &ly);
            to_screen(pdf.dpage, pdf.bx, pdf.by, &X, &Y);
            if (fabs(X - lx) + fabs(Y - ly) >= 2) add_point(pdf.bx, pdf.by);
        }
        ui.dirty = 1;
        return 1;
    }
    if (e->button != BTN_LEFT) return 1;
    if (e->pressed) {
        if (pdf.drag == D_LINE || pdf.drag == D_POLYGON) tool_click(e);
        return 1;
    }
    if (pdf.drag == D_SELECT) {
        pdf.drag = D_NONE;
        const struct view_annot *a = selected();
        if (!a || pdf.moved < 3 * scale) return 1;
        struct view_rect r = drag_rect();
        struct job j = pdf_job(J_PDF_EDIT, VIEW_PDF_MOVE, pdf.dpage);
        j.req.object = a->object;
        j.req.generation = a->generation;
        job_rect(&j, r.x0, r.y0, r.x1, r.y1);
        push_job(&j, 0);
        pdf.pending = 1;
        pdf.pending_rect = r;
    } else if (pdf.drag == D_DRAW) {
        pdf.drag = D_NONE;
        if (pdf.tool) tool_done();
    }
    return 1;
}

static void delete_selected(void)
{
    const struct view_annot *a = selected();
    if (!a) return;
    if (!annot_editable(a)) {
        toast(1, _("This annotation cannot be deleted."));
        return;
    }
    struct job j = pdf_job(J_PDF_EDIT, VIEW_PDF_DELETE, pdf.spage);
    j.req.object = a->object;
    j.req.generation = a->generation;
    push_job(&j, 0);
}

static void update_selected(unsigned mask, uint32_t rgb, float opacity, float width, const char *icon)
{
    const struct view_annot *a = selected();
    if (!a) return;
    struct job j = pdf_job(J_PDF_EDIT, VIEW_PDF_UPDATE, pdf.spage);
    j.req.object = a->object;
    j.req.generation = a->generation;
    j.req.mask = mask;
    j.req.color_n = 3;
    j.req.color[0] = (float)(rgb >> 16 & 0xFF) / 255;
    j.req.color[1] = (float)(rgb >> 8 & 0xFF) / 255;
    j.req.color[2] = (float)(rgb & 0xFF) / 255;
    j.req.opacity = opacity;
    j.req.width = width;
    if (icon) str_copy(j.req.icon, sizeof j.req.icon, icon);
    push_job(&j, 0);
}

/* Okular's Open Pop-up Note and its properties' text: the contents edited. */
static void edit_selected(int what)
{
    const struct view_annot *a = selected();
    char q[400];
    if (!a) return;
    if (!annot_editable(a) || (what == E_CONTENTS && (a->flags & AF_LOCKED_CONTENTS))) {
        toast(1, _("The text of this annotation cannot be changed."));
        return;
    }
    if (a->truncated) {
        toast(1, _("This annotation's text is too long to edit here."));
        return;
    }
    pdf.ed.page = pdf.spage;
    pdf.ed.object = a->object;
    pdf.ed.gen = a->generation;
    if (what == E_CONTENTS) {
        snprintf(q, sizeof q, "%s (%.100s):", annot_caption(a), a->author[0] ? a->author : _("Unknown"));
        edit_open(E_CONTENTS, q, a->contents, VIEW_TEXT - 1, 0, 1);
    } else {
        edit_open(E_AUTHOR, _("Author:"), a->author, sizeof a->author - 1, 0, 0);
    }
}

static uint32_t annot_rgb(const struct view_annot *a)
{
    if (a->color_n != 3) return 0;
    return (uint32_t)lround(fmin(1, fmax(0, a->color[0])) * 255) << 16 |
           (uint32_t)lround(fmin(1, fmax(0, a->color[1])) * 255) << 8 | (uint32_t)lround(fmin(1, fmax(0, a->color[2])) * 255);
}

/* Okular's Properties dialog, as menus: what view-pdf.c can set. */
static void annot_properties(int col, int row)
{
    const struct view_annot *a = selected();
    if (!a) return;
    if (!annot_editable(a)) {
        toast(1, _("This annotation cannot be changed here."));
        return;
    }
    struct item it[8];
    char label[6][160];
    int n = 0, type = a->type;
    uint32_t rgb = annot_rgb(a);
    snprintf(label[0], sizeof label[0], _("Contents: %.60s"), a->contents);
    snprintf(label[1], sizeof label[1], _("Author: %.60s"), a->author);
    snprintf(label[2], sizeof label[2], _("Color: %s"), a->color_n == 3 ? color_name(rgb) : "-");
    snprintf(label[3], sizeof label[3], _("Opacity: %d%%"), (int)lround(a->opacity * 100));
    snprintf(label[4], sizeof label[4], _("Width: %g"), a->width);
    snprintf(label[5], sizeof label[5], type == VIEW_ANNOT_STAMP ? _("Stamp symbol: %.60s") : _("Icon: %.60s"), a->icon);
    for (char *c = label[0]; *c; c++) if ((unsigned char)*c < ' ') *c = ' ';
    it[n++] = (struct item){ label[0], "", 1, 0 };
    it[n++] = (struct item){ label[1], "", 2, 0 };
    /* A typewriter's colour is its text's, kept in its font setting. */
    if (type != VIEW_ANNOT_FREE_TEXT) it[n++] = (struct item){ label[2], "", 3, 0 };
    it[n++] = (struct item){ label[3], "", 4, 0 };
    if (has_width(type)) it[n++] = (struct item){ label[4], "", 5, 0 };
    if (type == VIEW_ANNOT_TEXT || type == VIEW_ANNOT_STAMP) it[n++] = (struct item){ label[5], "", 6, 0 };
    int choice = run_menu(col, row, it, n);
    struct item sub[24];
    char names[24][40];
    int m = 0, picked;
    switch (choice) {
    case 1: edit_selected(E_CONTENTS); break;
    case 2: edit_selected(E_AUTHOR); break;
    case 3:
        for (int k = 0; k < NCOLORS; k++) sub[m++] = (struct item){ _(colors[k].name), "", k + 1, colors[k].rgb == rgb };
        picked = run_menu(col, row, sub, m);
        if (picked > 0) update_selected(VIEW_SET_COLOR, colors[picked - 1].rgb, 1, 0, NULL);
        break;
    case 4:
        for (int k = 1; k <= 10; k++, m++) {
            snprintf(names[m], sizeof names[m], "%d%%", k * 10);
            sub[m] = (struct item){ names[m], "", k, lround(a->opacity * 10) == k };
        }
        picked = run_menu(col, row, sub, m);
        if (picked > 0) update_selected(VIEW_SET_OPACITY, 0, picked / 10.0f, 0, NULL);
        break;
    case 5:
        for (int k = 2; k <= 10; k++, m++) {
            snprintf(names[m], sizeof names[m], _("Width %g"), k / 2.0);
            sub[m] = (struct item){ names[m], "", k, fabs(a->width - k / 2.0) < 0.01 };
        }
        picked = run_menu(col, row, sub, m);
        if (picked > 0) update_selected(VIEW_SET_WIDTH, 0, 1, picked / 2.0f, NULL);
        break;
    case 6: {
        const char *const *names_ = type == VIEW_ANNOT_STAMP ? stamps : note_icons;
        int count = type == VIEW_ANNOT_STAMP ? (int)(sizeof stamps / sizeof *stamps) : (int)(sizeof note_icons / sizeof *note_icons);
        for (int k = 0; k < count; k++) sub[m++] = (struct item){ names_[k], "", k + 1, !strcmp(names_[k], a->icon) };
        picked = run_menu(col, row, sub, m);
        if (picked > 0) update_selected(VIEW_SET_ICON, 0, 1, 0, names_[picked - 1]);
        break;
    }
    }
}

/* The pointer on the pages. 1: it was this file's. */
static int pdf_pointer(const struct pointer_event *e, int in_area)
{
    if (pdf.drag) return drag_event(e);
    if (!pdf_on() || !in_area || list.kind || prompt.kind || !e->pressed || e->button != BTN_LEFT ||
        ui_hit_at(e->col, e->row) >= 0) return 0;
    double px, py;
    int i = page_point(e->x, e->y, 0, &px, &py);
    /* The fields first: Okular's widgets are over the page. */
    if (pdf.forms && i >= 0) {
        int k = field_at(i, px, py);
        if (k >= 0) {
            ui.focus = FOCUS_DOC;
            int col, row;
            field_cell(i, &doc.page[i].field[k], &col, &row);
            field_activate(i, k, col, row);
            return 1;
        }
    }
    if (pdf.tool) {
        ui.focus = FOCUS_DOC;
        unselect();
        tool_press(e, i, px, py);
        return 1;
    }
    if (settings.tool != TOOL_BROWSE) return 0;
    int h = pdf.sobject ? handle_at(e->x, e->y) : 0, page = h ? pdf.spage : i, k = -1;
    if (h) annot_find(pdf.spage, pdf.sobject, pdf.sgen, &k);
    else if (i >= 0) k = annot_at(i, px, py);
    if (k < 0) {
        /* Okular: a click outside cancels the selection; the press is Browse's. */
        if (pdf.sobject) unselect();
        return 0;
    }
    const struct view_annot *a = &doc.page[page].annot[k];
    ui.focus = FOCUS_DOC;
    select_annot(page, a);
    if (e->clicks >= 2) {
        edit_selected(E_CONTENTS);
        return 1;
    }
    if (h ? annot_resizable(a) : annot_movable(a)) {
        pdf.drag = D_SELECT;
        pdf.dpage = page;
        pdf.handle = h;
        pdf.orig = annot_rect(a);
        page_clamped(page, e->x, e->y, &pdf.ax, &pdf.ay);
        pdf.bx = pdf.ax;
        pdf.by = pdf.ay;
        pdf.press_x = e->x;
        pdf.press_y = e->y;
        pdf.moved = 0;
    }
    return 1;
}

/* The right button on an annotation (Browse): Okular's annotation menu. */
static int pdf_context_menu(int X, int Y, int col, int row)
{
    if (!pdf_on() || settings.tool != TOOL_BROWSE || pdf.tool) return 0;
    double px, py;
    int i = page_point(X, Y, 0, &px, &py), k = i >= 0 ? annot_at(i, px, py) : -1;
    if (k < 0) return 0;
    const struct view_annot *a = &doc.page[i].annot[k];
    select_annot(i, a);
    struct item it[4];
    int n = 0;
    it[n++] = (struct item){ _("Open Pop-up Note"), "", A_ANNOT_OPEN, 0 };
    if (a->contents[0]) it[n++] = (struct item){ _("Copy Text to Clipboard"), "", A_ANNOT_COPY, 0 };
    it[n++] = (struct item){ _("Delete"), "Del", A_ANNOT_DELETE, 0 };
    it[n++] = (struct item){ _("Properties"), "", A_ANNOT_PROPERTIES, 0 };
    do_action(run_menu(col, row, it, n));
    return 1;
}

static void choose_tool(int t)
{
    if (!pdf_on()) {
        toast(0, _("Annotations can be added to PDF documents."));
        return;
    }
    if (!(pdf.flags & VIEW_PDF_CAN_ANNOTATE)) {
        toast(1, _("This document does not allow annotations."));
        return;
    }
    tool_settings();
    pdf.drag = D_NONE;
    pdf.npts = 0;
    /* Okular: the tool's button again stops annotating. */
    pdf.tool = pdf.tool == t ? T_NONE : t;
    if (pdf.tool) {
        unselect();
        if (!pdf.toolbar) {
            pdf.toolbar = 1;
            pdf_relayout();
        }
        if (settings.tool != TOOL_BROWSE) do_action(A_BROWSE);
    }
    ui.dirty = 1;
}

/* The toolbar's settings, for its tool. */
static void tool_setting(int k, int col, int row)
{
    struct item it[24];
    char names[24][40];
    int n = 0, t = pdf.tool, picked;
    tool_settings();
    if (k == 4) {
        pdf.keep = !pdf.keep;
        return;
    }
    if (k == 5) {
        do_action(A_ANNOTATE);
        return;
    }
    if (k == 3) {
        for (int s = 0; s < (int)(sizeof stamps / sizeof *stamps); s++) it[n++] = (struct item){ stamps[s], "", s + 1, !strcmp(stamps[s], pdf.stamp) };
        picked = run_menu(col, row, it, n);
        if (picked > 0) {
            str_copy(pdf.stamp, sizeof pdf.stamp, stamps[picked - 1]);
            if (pdf.tool != T_STAMP) choose_tool(T_STAMP);
        }
        return;
    }
    if (!t) {
        toast(0, _("Choose an annotation tool first."));
        return;
    }
    if (k == 0) {
        for (int c = 0; c < NCOLORS; c++) it[n++] = (struct item){ _(colors[c].name), "", c + 1, colors[c].rgb == pdf.set[t].rgb };
        picked = run_menu(col, row, it, n);
        if (picked > 0) pdf.set[t].rgb = colors[picked - 1].rgb;
    } else if (k == 1) {
        for (int w = 2; w <= 10; w++, n++) {
            snprintf(names[n], sizeof names[n], _("Width %g"), w / 2.0);
            it[n] = (struct item){ names[n], "", w, fabs(pdf.set[t].width - w / 2.0) < 0.01 };
        }
        picked = run_menu(col, row, it, n);
        if (picked > 0) pdf.set[t].width = picked / 2.0f;
    } else if (k == 2) {
        for (int o = 1; o <= 10; o++, n++) {
            snprintf(names[n], sizeof names[n], "%d%%", o * 10);
            it[n] = (struct item){ names[n], "", o, lround(pdf.set[t].opacity * 10) == o };
        }
        picked = run_menu(col, row, it, n);
        if (picked > 0) pdf.set[t].opacity = picked / 10.0f;
    }
    pdf_relayout(); /* its labels' widths may take another row */
}

static void pdf_action(int a)
{
    const struct view_annot *s = selected();
    switch (a) {
    case A_SAVE:
        if (!pdf_on()) toast(0, _("Only a PDF's changes can be saved."));
        else if (pdf_modified() && !pdf.saving) pdf_write(doc.path, 0, 0);
        break;
    case A_SAVE_AS:
        if (!pdf_on()) {
            toast(0, _("Only a PDF's changes can be saved."));
            break;
        }
        /* TRANSLATORS: o is the key in the file manager; keep it. */
        if (!choose_folder(_("Choose the folder to save the document into (o on it)."))) break;
        prompt_open(P_PDF_SAVE_AS, _("Save As, the file's name:"), base_name(doc.path), 0);
        break;
    case A_UNDO:
    case A_REDO:
        if (!pdf_on()) break;
        if (!(pdf.flags & (a == A_UNDO ? VIEW_PDF_CAN_UNDO : VIEW_PDF_CAN_REDO))) {
            toast(0, a == A_UNDO ? _("There is nothing to undo.") : _("There is nothing to redo."));
        } else {
            struct job j = new_job(J_PDF_EDIT, a == A_UNDO ? VIEW_PDF_UNDO : VIEW_PDF_REDO);
            push_job(&j, 0);
        }
        break;
    case A_ANNOTATE:
        if (!pdf_on()) {
            toast(0, _("Annotations can be added to PDF documents."));
            break;
        }
        pdf.toolbar = !pdf.toolbar;
        pdf.drag = D_NONE;
        if (!pdf.toolbar) pdf.tool = T_NONE;
        else if (settings.tool != TOOL_BROWSE) do_action(A_BROWSE); /* Okular: annotating is in Browse */
        pdf_relayout();
        break;
    case A_SHOW_FORMS:
        if (!(pdf.flags & VIEW_PDF_HAS_FORMS)) {
            toast(0, _("This document has no forms."));
            break;
        }
        pdf.forms = !pdf.forms;
        if (!pdf.forms) pdf.fobject = 0;
        if (pdf.forms_closed) {
            pdf.forms_closed = 0;
            pdf_relayout();
        }
        ui.dirty = 1;
        break;
    case A_ANNOT_OPEN: edit_selected(E_CONTENTS); break;
    case A_ANNOT_COPY:
        if (s && s->contents[0]) {
            int status = clipboard_set(0, s->contents, strlen(s->contents));
            if (status != CLIPBOARD_OK) toast(1, "%s", clipboard_error(status));
        }
        break;
    case A_ANNOT_DELETE: delete_selected(); break;
    case A_ANNOT_PROPERTIES: annot_properties(ptr.x / cw, (ptr.y - top) / ch); break;
    }
}

/* Keys every focus has. */
static int pdf_key_global(int key, int mods)
{
    if (!doc.open) return 0;
    int ctrl = mods & KEYS_CTRL, shift = mods & KEYS_SHIFT;
    if (key == 19 && ctrl && pdf_on()) {
        do_action(shift ? A_SAVE_AS : A_SAVE);
        return 1;
    }
    if (key == 26 && ctrl && pdf_on() && ui.focus != FOCUS_FIND) {
        do_action(shift ? A_REDO : A_UNDO);
        return 1;
    }
    if (key == K_F1 + 5 && !mods) {
        do_action(A_ANNOTATE);
        return 1;
    }
    if (mods == KEYS_ALT && key >= '0' && key <= '9') {
        for (int t = 1; t < T_COUNT; t++) {
            if (tools[t].alt == key) {
                choose_tool(t);
                return 1;
            }
        }
        if (key == '9') toast(0, _("The Arrow tool needs line endings, which cannot be set yet."));
        return key == '9';
    }
    return 0;
}

static int pdf_key_doc(int key, int mods)
{
    if (!pdf_on()) return 0;
    if (key == 27 && !mods) {
        if (pdf.drag == D_LINE || pdf.drag == D_POLYGON || pdf.drag == D_DRAW) pdf.drag = D_NONE;
        else if (pdf.tool) pdf.tool = T_NONE; /* Okular: Esc stops annotating */
        else if (pdf.sobject) unselect();
        else if (pdf.fobject) pdf.fobject = 0;
        else return 0;
        pdf.npts = 0;
        ui.dirty = 1;
        return 1;
    }
    if (key == K_DEL && !mods && pdf.sobject) {
        delete_selected();
        return 1;
    }
    if (pdf.toolbar && !mods && key >= '1' && key <= '9') {
        choose_tool(key - '0');
        return 1;
    }
    if (pdf.forms && key == '\t' && !(mods & ~KEYS_SHIFT)) {
        focus_next(mods & KEYS_SHIFT ? -1 : 1);
        return 1;
    }
    if (pdf.forms && pdf.fobject) return field_key(key, mods);
    return 0;
}

static void save_as_named(void)
{
    const char *name = prompt.in.text;
    char path[PATH_MAX];
    if (!name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") ||
        snprintf(path, sizeof path, "%s/%s", chosen_dir, name) >= (int)sizeof path) {
        prompt_close();
        toast(1, _("%.200s cannot be a file's name here."), name);
        return;
    }
    struct stat st;
    as_user(1);
    int exists = lstat(path, &st) == 0;
    as_user(0);
    str_copy(pdf.save_path, sizeof pdf.save_path, path);
    prompt_close();
    if (exists) {
        char q[600];
        snprintf(q, sizeof q, _("A file named %.300s already exists. Overwrite it? y/N"), name);
        prompt_open(P_OVERWRITE, q, "", 0);
        prompt.then = P_PDF_SAVE_AS;
        return;
    }
    pdf_write(pdf.save_path, 0, 1);
}

static int pdf_key_prompt(int key, int mods)
{
    int yes = key == 'y' || key == 'Y';
    switch (prompt.kind) {
    case P_PDF_TEXT:
        edit_key(key, mods);
        return 1;
    case P_PDF_SAVE_AS:
        if (key == '\n') save_as_named();
        else if (key == 27) {
            prompt_close();
            toast(0, _("Not saved."));
        } else return 0;
        return 1;
    case P_OVERWRITE:
        if (prompt.then != P_PDF_SAVE_AS) return 0;
        prompt_close();
        if (yes) pdf_write(pdf.save_path, 0, 1);
        else toast(0, _("Not saved."));
        return 1;
    case P_PDF_CONSENT:
        prompt_close();
        if (yes) pdf_write(pdf.save_path, 1, pdf.save_as);
        else {
            toast(0, _("Not saved."));
            pdf.resume = 0;
        }
        return 1;
    case P_PDF_UNSAVED:
        if (key == 's' || key == 'S') {
            prompt_close();
            pdf_write(doc.path, 0, 0); /* the action goes on when it is saved */
        } else if (key == 'd' || key == 'D') {
            prompt_close();
            pdf_resume();
        } else if (key == 27 || key == 'c' || key == 'C') {
            prompt_close();
            pdf.resume = 0;
        }
        return 1;
    }
    return 0;
}

/* The sidebar's panels: Okular's Annotations (by page: an annotation's
 * text, else its caption) and Signatures (signaturemodel.cpp's rows). */
struct arow { int page, index; }; /* index -1: the page's heading */
static struct arow *arows;
static int narows, arows_size;

static int annot_rows(int *loading)
{
    narows = 0;
    *loading = 0;
    for (int i = 0; i < doc.pages; i++) {
        const struct page *pg = &doc.page[i];
        if (pg->annots != 2 && !pg->annot) *loading = 1;
        for (int k = -1; pg->nannot && k < pg->nannot; k++) {
            if (narows == arows_size) {
                struct arow *r = realloc(arows, sizeof *r * (size_t)(arows_size + 64));
                if (!r) return narows;
                arows = r;
                arows_size += 64;
            }
            arows[narows++] = (struct arow){ i, k };
        }
    }
    return narows;
}

static void annot_label(const struct view_annot *a, char *out, size_t size)
{
    size_t n = 0;
    int space = 0;
    /* Its contents, simplified (QString::simplified). */
    for (const char *c = a->contents; *c && n + 2 < size; c++) {
        if ((unsigned char)*c <= ' ') space = n > 0;
        else {
            if (space) out[n++] = ' ';
            out[n++] = *c;
            space = 0;
        }
    }
    out[n] = '\0';
    if (!n) snprintf(out, size, "%s", annot_caption(a));
}

#define SIG_LINES 4096
struct sline { int sig; char text[300]; };
static struct sline *slines;
static int nslines;

__attribute__((format(printf, 2, 3)))
static void sig_line(int sig, const char *fmt, ...)
{
    if (nslines == SIG_LINES) return;
    if (!slines && !(slines = malloc(sizeof *slines * SIG_LINES))) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(slines[nslines].text, sizeof slines[nslines].text, fmt, ap);
    va_end(ap);
    slines[nslines++].sig = sig;
}

/* A verifier's result in Okular's words; unavailable without one. */
static const char *digest_text(const struct view_signature *s)
{
    switch (s->digest) {
    case VIEW_SIG_UNAVAILABLE: return _("Validity: unavailable (no signature verifier here: not checked)");
    case VIEW_SIG_FAILED: return _("Validity: the check failed");
    case VIEW_SIG_OKAY: return _("The signature is cryptographically valid.");
    case VIEW_SIG_DIGEST_FAILURE: return _("Digest Mismatch occurred.");
    case VIEW_SIG_NO_SIGNATURES: return _("The requested signature is not present in the document.");
    default: return _("The signature could not be verified.");
    }
}

static const char *certificate_text(const struct view_signature *s)
{
    switch (s->certificate) {
    case VIEW_SIG_UNAVAILABLE: return _("Certificate has not yet been verified.");
    case VIEW_SIG_OKAY: return _("Certificate is Trusted.");
    case VIEW_SIG_NOT_TRUSTED: return _("Certificate issuer isn't Trusted.");
    case VIEW_SIG_SELF_SIGNED:
    case VIEW_SIG_SELF_SIGNED_IN_CHAIN: return _("Certificate issuer is unknown.");
    default: return _("Unknown issue with Certificate or corrupted data.");
    }
}

static int sig_build(void)
{
    nslines = 0;
    for (int k = 0, rev = 0, placeholder = 0; k < pdf.nsig; k++) {
        const struct view_signature *s = &pdf.sig[k];
        char page[64];
        /* TRANSLATORS: %d is the page number where the signature field is located. */
        if (s->page >= 0) snprintf(page, sizeof page, _("on page %d"), s->page + 1);
        else snprintf(page, sizeof page, "%s", _("not on a page"));
        if (!s->is_signed) {
            char line[280];
            /* TRANSLATORS: %d is the signature placeholder number (1, 2, ...). */
            sig_line(k, _("Signature placeholder %d"), ++placeholder);
            /* TRANSLATORS: %.200s is the signature field name; %s is "on page N" or "not on a page". */
            snprintf(line, sizeof line, _("Field: %.200s %s"), s->field, page);
            sig_line(k, "  %s", line);
            continue;
        }
        /* The name the PDF gives: a verifier's signatory only when there is one. */
        /* TRANSLATORS: %d is the signature revision number; %.200s is the signer's name. */
        sig_line(k, _("Rev. %d: Signed By %.200s"), ++rev, s->signatory[0] ? s->signatory : s->name[0] ? s->name : _("Unknown"));
        if (!s->signatory[0]) sig_line(k, "  %s", _("(the name the document gives, not verified)"));
        sig_line(k, "  %s", digest_text(s));
        sig_line(k, "  %s", certificate_text(s));
        if (s->changes == 0) sig_line(k, "  %s", _("Changes: unchanged (no disallowed changes since signing; not a cryptographic check)"));
        /* TRANSLATORS: %d is the number of saved revisions after the signature. */
        else if (s->changes > 0) {
            char chg[280];
            snprintf(chg, sizeof chg, _("Changes: changed (by the revision saved %d later)"), s->changes);
            sig_line(k, "  %s", chg);
        }
        else if (s->changes == -1) sig_line(k, "  %s", _("Changes: changed (by the unsaved changes)"));
        else sig_line(k, "  %s", _("Changes: unavailable"));
        if (s->modified) sig_line(k, "  %s", _("The document has unsaved changes."));
        if (s->date[0]) {
            char line[280];
            snprintf(line, sizeof line, _("Signing Time: %.60s"), s->date);
            sig_line(k, "  %s", line);
        }
        if (s->reason[0]) {
            char line[280];
            snprintf(line, sizeof line, _("Reason: %.200s"), s->reason);
            sig_line(k, "  %s", line);
        }
        if (s->location[0]) {
            char line[280];
            snprintf(line, sizeof line, _("Location: %.200s"), s->location);
            sig_line(k, "  %s", line);
        }
        {
            char line[280];
            snprintf(line, sizeof line, _("Field: %.200s %s"), s->field, page);
            sig_line(k, "  %s", line);
        }
        if (s->error[0]) {
            char line[280];
            snprintf(line, sizeof line, _("Error: %.200s"), s->error);
            sig_line(k, "  %s", line);
        }
    }
    return nslines;
}

static int pdf_sidebar_tabs(int col)
{
    if (!pdf_on()) return col;
    char full[64], brief[16];
    for (int t = TAB_ANNOTATIONS; t <= TAB_SIGNATURES; t++) {
        if (t == TAB_SIGNATURES && !pdf.nsig_fields) continue;
        snprintf(full, sizeof full, " %s ", t == TAB_ANNOTATIONS ? _("Annotations") : _("Signatures"));
        snprintf(brief, sizeof brief, " %s ", t == TAB_ANNOTATIONS ? "A" : "S");
        int fw = ui_cells(full), bw = ui_cells(brief);
        const char *name = col + fw <= side_cols ? full : col + bw <= side_cols ? brief : NULL;
        int w = name == full ? fw : bw;
        if (!name) continue; /* also the forms bar's and Show Signatures Panel's */
        int on = ui.tab == t;
        screen_text(col, 2, name, on ? BG : FG, on ? ACCENT : BG);
        ui_hit(col, 2, w, 1, HIT_TAB + t);
        col += w + 1;
    }
    return col;
}

/* An annotation row chosen: its page, the annotation selected. */
static void annot_row_go(int k)
{
    int loading;
    if (k < 0 || k >= annot_rows(&loading)) return;
    const struct arow *r = &arows[k];
    if (r->index < 0) {
        go(pos_page(r->page));
        return;
    }
    const struct view_annot *a = &doc.page[r->page].annot[r->index];
    go(pos_point(r->page, 0, fmax(0, fmin(a->y0, a->y1) - 24)));
    select_annot(r->page, a);
}

static void sig_row_go(int k)
{
    if (k < 0 || k >= sig_build()) return;
    const struct view_signature *s = &pdf.sig[slines[k].sig];
    if (s->page >= 0 && s->page < doc.pages) go(pos_page(s->page));
}

static int pdf_draw_sidebar(void)
{
    if (ui.tab != TAB_ANNOTATIONS && ui.tab != TAB_SIGNATURES) return 0;
    char text[600], line[700];
    int last = side_last(), rows = last - 3 + 1, focus = ui.focus == FOCUS_SIDEBAR;
    if (!pdf_on()) {
        screen_text(0, 3, _("Not a PDF"), DIM, BG);
        return 1;
    }
    int loading = 0, n = ui.tab == TAB_ANNOTATIONS ? annot_rows(&loading) : sig_build();
    int *sel = ui.tab == TAB_ANNOTATIONS ? &pdf.asel : &pdf.ssel, *first = ui.tab == TAB_ANNOTATIONS ? &pdf.atop : &pdf.stop;
    if (!n) {
        screen_text(0, 3, ui.tab == TAB_SIGNATURES ? (pdf.sigs == 2 ? _("No signatures") : _("Reading the signatures...")) :
                    loading ? _("Looking for annotations...") : _("No annotations"), DIM, BG);
        return 1;
    }
    *sel = clampi(*sel, 0, n - 1);
    if (*sel < *first) *first = *sel;
    if (*sel >= *first + rows) *first = *sel - rows + 1;
    *first = clampi(*first, 0, n - 1);
    for (int r = 3; r <= last && *first + r - 3 < n; r++) {
        int k = *first + r - 3, mark = 0;
        if (ui.tab == TAB_ANNOTATIONS) {
            const struct arow *a = &arows[k];
            if (a->index < 0) snprintf(line, sizeof line, _("Page %d"), a->page + 1);
            else {
                const struct view_annot *an = &doc.page[a->page].annot[a->index];
                annot_label(an, text, sizeof text);
                snprintf(line, sizeof line, "  %s", text);
                mark = pdf.sobject == an->object && pdf.sgen == an->generation && pdf.spage == a->page;
            }
        } else {
            snprintf(line, sizeof line, "%s", slines[k].text);
        }
        int on = focus && k == *sel;
        uint32_t bg = on ? ACCENT : BG, fg = on ? BG : mark ? ACCENT : FG;
        fit(text, sizeof text, line, side_cols - 1);
        screen_fill(0, r, side_cols - 1, 1, bg);
        screen_text(0, r, text, fg, bg);
        ui_hit(0, r, side_cols - 1, 1, (ui.tab == TAB_ANNOTATIONS ? HIT_PDF_ANNOT : HIT_PDF_SIG) + k);
    }
    return 1;
}

static int pdf_key_sidebar(int key, int mods)
{
    (void)mods;
    if (ui.tab != TAB_ANNOTATIONS && ui.tab != TAB_SIGNATURES) return 0;
    int loading, n = ui.tab == TAB_ANNOTATIONS ? annot_rows(&loading) : sig_build(), page = side_last() - 3;
    int *sel = ui.tab == TAB_ANNOTATIONS ? &pdf.asel : &pdf.ssel;
    if (key == K_UP) *sel -= 1;
    else if (key == K_DOWN) *sel += 1;
    else if (key == K_PGUP) *sel -= page;
    else if (key == K_PGDN) *sel += page;
    else if (key == K_BOL) *sel = 0;
    else if (key == K_EOL) *sel = n - 1;
    else if (key == '\n') {
        if (ui.tab == TAB_ANNOTATIONS) annot_row_go(*sel);
        else sig_row_go(*sel);
    } else if (key == '\t' || key == 27) ui.focus = FOCUS_DOC;
    *sel = clampi(*sel, 0, n > 0 ? n - 1 : 0);
    ui.dirty = 1;
    return 1;
}

static int pdf_wheel_sidebar(int wheel)
{
    if (ui.tab != TAB_ANNOTATIONS && ui.tab != TAB_SIGNATURES) return 0;
    int *first = ui.tab == TAB_ANNOTATIONS ? &pdf.atop : &pdf.stop, *sel = ui.tab == TAB_ANNOTATIONS ? &pdf.asel : &pdf.ssel;
    *first = *first - 3 * wheel > 0 ? *first - 3 * wheel : 0;
    if (*sel < *first) *sel = *first;
    ui.dirty = 1;
    return 1;
}

static int pdf_click(int id, const struct pointer_event *e)
{
    if (id >= HIT_PDF_SIG && id < HIT_PDF_SIG + 500000) {
        ui.focus = FOCUS_SIDEBAR;
        pdf.ssel = id - HIT_PDF_SIG;
        sig_row_go(pdf.ssel);
        return 1;
    }
    if (id >= HIT_PDF_ANNOT && id < HIT_PDF_SIG) {
        ui.focus = FOCUS_SIDEBAR;
        pdf.asel = id - HIT_PDF_ANNOT;
        annot_row_go(pdf.asel);
        return 1;
    }
    if (id >= HIT_PDF_TOOL && id < HIT_PDF_TOOL + T_COUNT) {
        choose_tool(id - HIT_PDF_TOOL);
        return 1;
    }
    if (id >= HIT_PDF_SET && id < HIT_PDF_SET + 6) {
        tool_setting(id - HIT_PDF_SET, e->col, e->row + 1);
        return 1;
    }
    if (id >= HIT_PDF_BAR && id < HIT_PDF_BAR + 4) {
        switch (id - HIT_PDF_BAR) {
        case 0: do_action(A_SHOW_FORMS); break;
        case 1: pdf.forms_closed = 1; pdf_relayout(); break;
        case 2:
            if (!settings.sidebar) sidebar(1);
            ui.tab = TAB_SIGNATURES;
            ui.focus = FOCUS_SIDEBAR;
            break;
        case 3: pdf.sigbar_closed = 1; pdf_relayout(); break;
        }
        return 1;
    }
    return 0;
}

static void pdf_menu_items(int m, struct item *it, int *n)
{
    if (!pdf_on() || *n > MENU_ITEMS - 2) return;
    if (m == 0) {
        it[(*n)++] = (struct item){ _("Save"), "Ctrl+S", A_SAVE, 0 };
        it[(*n)++] = (struct item){ _("Save As..."), "Ctrl+Shift+S", A_SAVE_AS, 0 };
    } else if (m == 2 && (pdf.flags & VIEW_PDF_HAS_FORMS)) {
        it[(*n)++] = (struct item){ _("Show Forms"), "", A_SHOW_FORMS, pdf.forms };
    } else if (m == 4) {
        it[(*n)++] = (struct item){ _("Annotations"), "F6", A_ANNOTATE, pdf.toolbar };
    }
}

/* Everything back to how it starts (view.c's forget). */
static void pdf_reset(void)
{
    free(pdf.sig);
    free(arows);
    free(slines);
    arows = NULL;
    slines = NULL;
    narows = arows_size = nslines = 0;
    memset(&pdf, 0, sizeof pdf);
    pdf.keep = 1; /* Okular's AnnotationContinuousMode */
    snprintf(pdf.stamp, sizeof pdf.stamp, "Draft");
}
