/*
 * view-proto.h: between view (the screen, keyboard, pointer and clipboard;
 * root) and view-render (the documents: running as the user, uid 1000, with
 * no_new_privs and an empty network namespace), over a SOCK_SEQPACKET socket
 * pair. Documents are parsed only in view-render: a malformed or hostile file
 * can at most take over a process that has the user's file access and
 * nothing else (no network, no screen, no root).
 *
 * One request is one message (struct view_request); each gets one reply
 * (struct view_reply), with the same serial, and for bulk results a sealed
 * memfd passed with it (SCM_RIGHTS) holding size bytes. One request is
 * handled at a time; SIGUSR1 to view-render aborts the current RENDER or
 * SEARCH (its reply has status ECANCELED).
 *
 * Coordinates are a page's own, in points (1/72 inch), unrotated, with the
 * page's top left at (0, 0) (view-render moves MuPDF's page origin there).
 * Pixels are 0x00RRGGBB, rows of w pixels.
 */
#ifndef LOCACHY_VIEW_PROTO_H
#define LOCACHY_VIEW_PROTO_H

#include <stdint.h>
#include <sys/types.h>

#define VIEW_RENDER_PATH "/usr/local/lib/locachy/view-render"
#define VIEW_RENDER_FD 3        /* the socket in view-render */
#define VIEW_USER 1000          /* the user it runs as */
#define VIEW_USER_CSS ".config/locachy/view.css" /* in the user's home: for reflowed documents */
#define VIEW_TEXT 4096
#define VIEW_MAX_PIXELS (64u << 20) /* one RENDER at most this many pixels: else EFBIG */

enum view_op {
    VIEW_OPEN = 1,  /* text: the path; ax, ay, bx: the layout (w, h, em: points) for a reflowable
                     * document. Reply: pages, flags, text "FORMAT\tTITLE" (FORMAT as Okular
                     * names it: PDF, XPS, EPUB, Mobipocket, FictionBook, Markdown, Plain text,
                     * Comic book, DjVu, PostScript, DVI, Fax, Image, TIFF); status EACCES and
                     * flags VIEW_NEEDS_PASSWORD when a password is asked for. */
    VIEW_AUTH,      /* text: the password. Reply as OPEN's; EACCES: wrong. */
    VIEW_PAGE,      /* page. Reply: pw, ph (its size in points), flags VIEW_HAS_TEXT if it has. */
    VIEW_RENDER,    /* page at zoom pixels a point, rotate (0, 90, 180, 270 clockwise); x, y, w,
                     * h: the rectangle of the rendered page in its pixels to draw (w 0: the
                     * whole page). Reply: w, h; the pixels in the memfd. */
    VIEW_SEARCH,    /* page, text: the needle (case-insensitive, as MuPDF's). Reply: count; the
                     * memfd holds count struct view_rect (each hit's boxes, in order; a hit
                     * over two lines is two rects with the same hit). */
    VIEW_SELECT,    /* page, (ax, ay) to (bx, by), mode VIEW_SELECT_CHARS, _WORDS (a double
                     * click) or _LINES (a triple click). Reply: count rects in the memfd,
                     * then the selected text (UTF-8) after them: size covers both; text
                     * length in the reply's w. */
    VIEW_LINKS,     /* page. Reply: count struct view_link in the memfd. */
    VIEW_OUTLINE,   /* Reply: count struct view_outline in the memfd, in document order. */
    VIEW_INFO,      /* Reply: memfd: "KEY\tVALUE\n" lines (Format, Title, Author, Subject,
                     * Keywords, Creator, Producer, Created, Modified, Pages, Page size,
                     * Encryption ...), then for PDF "Font\tNAME (TYPE, embedded or not)". */
    VIEW_EXPORT,    /* Reply: memfd: the text of every page (UTF-8), pages separated by \f. */
    VIEW_EMBEDDED,  /* Reply: memfd: "INDEX\tNAME\tBYTES\n" lines for the files in the document. */
    VIEW_SAVE,      /* page: the embedded file's index; text: where (written as the user). */
    VIEW_LAYOUT,    /* ax, ay, bx: w, h, em (points) for a reflowable document; page: the one
                     * at the top now. Reply: pages, page (where that position is now, by
                     * MuPDF's bookmarks). */
    VIEW_RESOLVE,   /* text: an internal link's URI. Reply: page, x, y (-1: none). */
    VIEW_BBOX,      /* page, zoom: initial full-page raster scale (pixels/point).
                     * Reply memfd: one normalized view_rect of nonwhite page content;
                     * count 0 for a blank page. Coordinates are unrotated. */
    /* PDF interaction (view-pdf.c), for a PDF (format "PDF") only: else ENOTSUP.
     * Each reply has the session's state: flags VIEW_PDF_* and revision, which
     * changes with every edit, undo and redo (then the pages are drawn anew).
     * An annotation or field is (page, object, generation). */
    VIEW_PDF_STATUS,   /* Reply: flags, revision; count: signature fields. */
    VIEW_PDF_ANNOTS,   /* page. Reply: count struct view_annot in the memfd. */
    VIEW_PDF_CREATE,   /* page, mode: VIEW_ANNOT_*; contents in text, author, color,
                        * opacity, width, icon; ax, ay, bx, by: its rectangle (the
                        * text markups: the two ends of the text, as SELECT's);
                        * points: a line's two ends, a polygon's corners, the ink
                        * strokes' points (stroke: each stroke's count). Reply: w
                        * object, h generation of the new one. */
    VIEW_PDF_UPDATE,   /* the annotation; mask VIEW_SET_*: what of contents (text),
                        * author, color, opacity, width, icon to set. */
    VIEW_PDF_MOVE,     /* the annotation to rectangle ax, ay, bx, by (moved or resized). */
    VIEW_PDF_DELETE,   /* the annotation. */
    VIEW_PDF_UNDO,
    VIEW_PDF_REDO,
    VIEW_PDF_FIELDS,   /* page. Reply: count struct view_field, then w struct
                        * view_option (each field's from its first_option). */
    VIEW_PDF_SET,      /* the field; count values (export values) in text, each
                        * ending in a NUL. Rejected (MaxLen, the document's
                        * validation): EINVAL, nothing changed, text says why. */
    VIEW_PDF_SIGNATURES, /* Reply: count struct view_signature in the memfd. */
    VIEW_PDF_WRITE,    /* text: the path to save to, as the user; mode
                        * VIEW_PDF_INVALIDATE_SIGNATURES: consented. EPERM: a signed
                        * document changed, its signatures would break. */
    VIEW_WORDS,        /* page, (ax, ay) to (bx, by): the words with a point in that
                        * rectangle (Okular's Area and Table Selection read words).
                        * Reply: count struct view_rect (a word's box; hit: its line,
                        * counted on the page), then each word's UTF-8 text ending in
                        * a NUL, in the same (reading) order; size covers both. */
    VIEW_MARK,         /* page of a reflowable document. Reply: text, MuPDF's bookmark
                        * of the page's start as a decimal number: it survives a new
                        * layout and a reopening. */
    VIEW_FIND_MARK,    /* text: a VIEW_MARK. Reply: page, where it is in this layout. */
};

/* VIEW_PDF_*'s reply flags */
#define VIEW_PDF_DIRTY          1u /* unsaved changes */
#define VIEW_PDF_CAN_UNDO       2u
#define VIEW_PDF_CAN_REDO       4u
#define VIEW_PDF_CAN_ANNOTATE   8u /* the document's permissions */
#define VIEW_PDF_CAN_FILL      16u
#define VIEW_PDF_HAS_FORMS     32u
#define VIEW_PDF_HAS_XFA       64u
#define VIEW_PDF_HAS_SIGNED   128u /* signed signature fields: saving changes needs consent */
#define VIEW_PDF_MORE         256u /* ANNOTS, FIELDS: more than VIEW_PDF_ITEMS on the page */
#define VIEW_PDF_INVALIDATE_SIGNATURES 1 /* WRITE's mode */
#define VIEW_PDF_POINTS  512   /* points in a CREATE */
#define VIEW_PDF_STROKES 64    /* ink strokes in a CREATE */
#define VIEW_PDF_ITEMS   1024  /* annotations or fields of a page listed */
#define VIEW_PDF_OPTIONS 4096  /* choice options of a page listed */

/* Annotation types (MuPDF's names in view-render): those a CREATE makes,
 * and OTHER for the rest (listed, not edited). */
enum view_annot_type {
    VIEW_ANNOT_TEXT,        /* Okular's Pop-up Note */
    VIEW_ANNOT_FREE_TEXT,   /* Typewriter, Inline Note */
    VIEW_ANNOT_LINE, VIEW_ANNOT_SQUARE, VIEW_ANNOT_CIRCLE, VIEW_ANNOT_POLYGON, VIEW_ANNOT_POLY_LINE,
    VIEW_ANNOT_HIGHLIGHT, VIEW_ANNOT_UNDERLINE, VIEW_ANNOT_SQUIGGLY, VIEW_ANNOT_STRIKE_OUT,
    VIEW_ANNOT_STAMP, VIEW_ANNOT_INK, VIEW_ANNOT_OTHER
};
/* UPDATE's mask */
#define VIEW_SET_CONTENTS 1u
#define VIEW_SET_AUTHOR   2u
#define VIEW_SET_COLOR    4u
#define VIEW_SET_OPACITY  8u
#define VIEW_SET_WIDTH   16u
#define VIEW_SET_ICON    32u
/* Field types */
enum view_field_type {
    VIEW_FIELD_TEXT, VIEW_FIELD_CHECKBOX, VIEW_FIELD_RADIO, VIEW_FIELD_COMBO, VIEW_FIELD_LIST,
    VIEW_FIELD_BUTTON, VIEW_FIELD_SIGNATURE, VIEW_FIELD_OTHER
};
/* A signature's digest and certificate results: VIEW_SIG_UNAVAILABLE when no
 * verifier checked it (never shown as valid), else what the verifier said. */
enum view_sig_status {
    VIEW_SIG_UNAVAILABLE, VIEW_SIG_FAILED, VIEW_SIG_NOT_SIGNED, VIEW_SIG_OKAY, VIEW_SIG_NO_SIGNATURES,
    VIEW_SIG_NO_CERTIFICATE, VIEW_SIG_DIGEST_FAILURE, VIEW_SIG_SELF_SIGNED, VIEW_SIG_SELF_SIGNED_IN_CHAIN,
    VIEW_SIG_NOT_TRUSTED, VIEW_SIG_NOT_SUPPORTED, VIEW_SIG_UNKNOWN
};

enum { VIEW_SELECT_CHARS, VIEW_SELECT_WORDS, VIEW_SELECT_LINES };

/* OPEN's and PAGE's flags */
#define VIEW_REFLOWABLE      1u /* laid out to the screen: LAYOUT applies */
#define VIEW_HAS_TEXT        2u /* text can be searched and selected */
#define VIEW_NEEDS_PASSWORD  4u
#define VIEW_HAS_OUTLINE     8u
#define VIEW_HAS_EMBEDDED   16u

struct view_request {
    uint32_t op, serial;   /* serial: copied into the reply */
    int32_t page;
    float zoom;
    int32_t rotate;
    int32_t x, y, w, h;
    float ax, ay, bx, by;
    int32_t mode;
    char text[VIEW_TEXT];
    /* VIEW_PDF_*: the annotation or field (on page), and what to give it. */
    int32_t object, generation;
    uint32_t mask;
    int32_t count;                      /* SET: values; CREATE: points */
    int32_t color_n;                    /* 0 (none), 1, 3 or 4 components, 0 to 1 */
    float color[4], opacity, width;
    char icon[32];                      /* a PDF name: a stamp's or a note's icon */
    char author[256];
    int32_t strokes;
    int32_t stroke[VIEW_PDF_STROKES];
    float points[2 * VIEW_PDF_POINTS];  /* x, y pairs, page coordinates */
    uint32_t paper;                     /* RENDER: VIEW_PAPER | 0xRRGGBB, the paper drawn on
                                         * (Okular's Change Paper Color); 0: white */
};
#define VIEW_PAPER 0x1000000u

struct view_reply {
    uint32_t op, serial;
    int32_t status;        /* 0, or an errno value; text then says why */
    int32_t pages, page, count;
    int32_t w, h;
    float pw, ph, x, y;
    uint32_t flags;
    uint64_t size;         /* bytes in the memfd passed with it (0: none) */
    char text[VIEW_TEXT];
    uint64_t revision;     /* VIEW_PDF_*: the edits' counter */
};

/* VIEW_PDF_ANNOTS: one annotation (strings end in NUL; truncated: contents or
 * author were longer than here, and are not edited from these). */
struct view_annot {
    int32_t object, generation, type, flags; /* type VIEW_ANNOT_*; flags the PDF's /F */
    float x0, y0, x1, y1;
    int32_t color_n;
    float color[4], opacity, width;
    int32_t editable, truncated;
    char subtype[32], icon[32], author[256];
    char contents[VIEW_TEXT];
};

/* VIEW_PDF_FIELDS: one widget of a field, and the options of choices. */
struct view_field {
    int32_t object, generation, type, flags; /* VIEW_FIELD_*; the field's /Ff */
    float x0, y0, x1, y1;
    int32_t readonly, required, max_length, multiselect, editable_choice;
    int32_t options, first_option;
    int32_t truncated;
    char name[256], on_value[128];
    char value[VIEW_TEXT];
};
struct view_option {
    int32_t selected, truncated;
    char label[256], value[256];
};

/* VIEW_PDF_SIGNATURES: a signature field. The name, reason, location and
 * date are what the PDF says, not who signed it; signatory is from a
 * verifier only. changes: 0 no disallowed change found (MuPDF's check of
 * the revisions, not cryptographic), -1 an unsaved change breaks it, N > 0
 * broken by the revision N saves later, -2 unknown. */
struct view_signature {
    int32_t page, object, generation;   /* page -1: no widget */
    int32_t is_signed, digest, certificate, changes, modified;
    char field[256], name[256], reason[256], location[256], date[64], signatory[256], error[256];
};

struct view_rect {
    float x0, y0, x1, y1;
    int32_t hit;           /* SEARCH: which hit; SELECT: 0 */
};

struct view_link {
    float x0, y0, x1, y1;
    int32_t page;          /* the target page, or -1: an external URI (uri) */
    float x, y;            /* where on it */
    char uri[1024];
};

struct view_outline {
    int32_t level;         /* 0: top */
    int32_t page;          /* -1: none (an external link) */
    float y;
    char title[256];
};

/* view-client.c, used by view (and the tests): start view-render as the
 * user in its sandbox, its end of the socket in *sock: its pid, or -1. */
pid_t view_spawn(int *sock);
/* Send r (its serial set from a counter): 0, or -1. */
int view_send(int sock, struct view_request *r);
/* Receive one reply, *fd its memfd or -1 (the caller closes it): 0, or -1
 * (view-render has gone: start it again). */
int view_recv(int sock, struct view_reply *r, int *fd);
/* Both, waiting for this request's reply (others are dropped). */
int view_call(int sock, struct view_request *r, struct view_reply *rep, int *fd);
/* A memfd's bytes mapped read-only (NULL if size is 0 or it fails); unmap
 * with munmap(p, size). */
void *view_map(int fd, uint64_t size);

#endif
