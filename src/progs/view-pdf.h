/* MuPDF PDF editing backend, independent of the renderer and protocol.
 * All fallible calls throw MuPDF exceptions. Arrays/strings are owned by the
 * caller and released by the matching drop call (using the same fz_context).
 * Page numbers are zero-based; geometry is MuPDF unscaled page/document space,
 * not PDF bottom-up coordinates and not screen pixels. No GLib/JSON dependency.
 *
 * Attach only after authentication. The session keeps the document, borrows ctx,
 * enables the PDF journal and standard MuPDF JavaScript form validation, and
 * must be its sole editor. Serialize calls with rendering on that context.
 * Drop/reload cached renderer pages after edits/undo/redo (revision changes).
 * The caller remains responsible for opening files as uid 1000 and for sandboxing
 * document actions; no application/document event callback is installed here.
 */
#ifndef LOCACHY_VIEW_PDF_H
#define LOCACHY_VIEW_PDF_H

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stdint.h>

typedef struct view_pdf view_pdf;
typedef struct { int page, object, generation; } view_pdf_id;

typedef struct {
    view_pdf_id id;
    enum pdf_annot_type type;
    fz_rect rect;
    char *name, *author, *contents, *icon;
    int flags, color_n;
    float color[4], opacity, width;
    int quad_count;
    fz_quad *quads;
    int vertex_count;
    fz_point *vertices;
    int stroke_count, point_count;
    int *stroke_sizes;
    fz_point *points;
    fz_point line[2];
    int editable; /* Known creation/editing type; other annotations still listed. */
} view_pdf_annotation;

typedef struct {
    view_pdf_annotation *items;
    int count;
} view_pdf_annotations;

/* Only fields selected in mask are used on update. Creation always uses rect,
 * contents/author, color/opacity/width/flags and type-specific geometry. Use
 * view_pdf_default_annotation() for defaults. FreeText = Okular typewriter;
 * Text = popup note, Square/Circle = rectangle/ellipse, Squiggly = squiggle.
 * Stamps use standard PDF icon names or a supplied fz_image. */
enum {
    VIEW_PDF_CONTENTS = 1u << 0, VIEW_PDF_AUTHOR = 1u << 1,
    VIEW_PDF_COLOR = 1u << 2, VIEW_PDF_OPACITY = 1u << 3,
    VIEW_PDF_WIDTH = 1u << 4, VIEW_PDF_FLAGS = 1u << 5,
    VIEW_PDF_GEOMETRY = 1u << 6, VIEW_PDF_ICON = 1u << 7,
    VIEW_PDF_TEXT_STYLE = 1u << 8, VIEW_PDF_STAMP_IMAGE = 1u << 9
};
#define VIEW_PDF_ALL ((1u << 10) - 1)
typedef struct {
    enum pdf_annot_type type;
    fz_rect rect;
    const char *contents, *author, *icon;
    int flags, color_n;
    float color[4], opacity, width;
    int quad_count;
    const fz_quad *quads;
    int vertex_count;
    const fz_point *vertices;
    int stroke_count, point_count;
    const int *stroke_sizes;
    const fz_point *points;
    fz_point line[2];
    const char *font; /* Standard MuPDF PDF font name, default Helv. */
    float font_size;
    fz_image *stamp_image; /* Borrowed; NULL retains a standard stamp. */
} view_pdf_annotation_spec;

typedef struct { char *label, *value; } view_pdf_option;
typedef struct {
    view_pdf_id id;
    enum pdf_widget_type type;
    char *name, *value, *on_value;
    fz_rect rect;
    int readonly, required, flags, max_length, multiselect;
    int option_count, value_count;
    view_pdf_option *options;
    char **values; /* All selected export values, including multi-select. */
} view_pdf_widget;
typedef struct { view_pdf_widget *items; int count; } view_pdf_widgets;

/* The standard libmupdf package has the verifier interface but no verifier
 * factory/PKCS7 implementation. NULL verifier reports UNAVAILABLE, never OKAY.
 * A caller with a mature MuPDF-compatible backend may pass its borrowed verifier;
 * then pdf_check_* and pdf_signature_get_signatory perform verification.
 * Name/reason/location/date are PDF metadata, NOT authenticated signer identity.
 * A digest result describes the opened file's signed revision, not unsaved edits.
 */
#define VIEW_PDF_SIGNATURE_UNAVAILABLE (-1)
#define VIEW_PDF_SIGNATURE_FAILED (-2)
typedef struct {
    view_pdf_id id; /* page == -1 for non-visible signature fields. */
    char *field, *name, *reason, *location, *date, *signatory, *error;
    int signed_value, digest_status, certificate_status;
    int document_modified; /* Session has unsaved edits. */
    /* MuPDF pdf_validate_signature result, NOT cryptographic verification:
     * 0 = no disallowed PDF changes found, -1 = disallowed unsaved change,
     * >0 = invalidated this many saved revisions ago, -2 = unavailable. */
    int change_status;
} view_pdf_signature;
typedef struct { view_pdf_signature *items; int count; } view_pdf_signatures;

typedef struct {
    int dirty, can_undo, can_redo, position, steps;
    int javascript, can_annotate, can_fill_forms, has_xfa;
    uint64_t revision;
} view_pdf_status;

/* Returns NULL for non-PDF, not an error. */
view_pdf *view_pdf_attach(fz_context *ctx, fz_document *document);
void view_pdf_drop(view_pdf *pdf);
view_pdf_status view_pdf_get_status(view_pdf *pdf);
int view_pdf_annotation_supported(enum pdf_annot_type type);
view_pdf_annotation_spec view_pdf_default_annotation(enum pdf_annot_type type, fz_rect rect);
view_pdf_annotations view_pdf_list_annotations(view_pdf *pdf, int page);
void view_pdf_drop_annotations(fz_context *ctx, view_pdf_annotations *list);
view_pdf_id view_pdf_create_annotation(view_pdf *pdf, int page, const view_pdf_annotation_spec *spec);
void view_pdf_update_annotation(view_pdf *pdf, view_pdf_id id, unsigned mask, const view_pdf_annotation_spec *spec);
/* Affine move/resize also transforms quads, ink strokes, vertices and line ends.
 * A zero-area source/target rectangle is rejected, not silently approximated. */
void view_pdf_transform_annotation(view_pdf *pdf, view_pdf_id id, fz_rect target);
void view_pdf_delete_annotation(view_pdf *pdf, view_pdf_id id);
void view_pdf_undo(view_pdf *pdf);
void view_pdf_redo(view_pdf *pdf);
view_pdf_widgets view_pdf_list_widgets(view_pdf *pdf, int page);
void view_pdf_drop_widgets(fz_context *ctx, view_pdf_widgets *list);
/* Export values, not option labels. Text/check/radio/combo require one value;
 * multi-select list accepts zero or more. Off turns off a checkbox/radio unless
 * the radio has NoToggleToOff. Rejected event validation throws and rolls back.
 * Multi-selection with validation/keystroke JS is explicitly unsupported by
 * MuPDF's array setter and rejected here rather than bypassing those events. */
void view_pdf_set_widget(view_pdf *pdf, view_pdf_id id, int count, const char *const *values);
view_pdf_signatures view_pdf_list_signatures(view_pdf *pdf, pdf_pkcs7_verifier *verifier);
void view_pdf_drop_signatures(fz_context *ctx, view_pdf_signatures *list);

enum { VIEW_PDF_ALLOW_SIGNATURE_INVALIDATION = 1u << 0 };
/* Save and Save As share this API. Must run as real/effective uid 1000 (root
 * refused); no privilege changes. Existing targets must be writable regular
 * files; symlink targets are resolved. A sibling mkstemp, fsync, compare-before-
 * rename and atomic rename keep the original intact on pre-commit failures.
 * Existing mode is retained, new files are 0600, directory is fsynced where
 * supported. No post-rename error is reported as an uncommitted failure.
 * Encryption is KEEP, never silently removed. Standard snapshots retain undo
 * history; repaired/redacted PDFs cannot be snapshotted and fail explicitly.
 * Modified signed input requires explicit invalidation consent even for
 * incremental saves: certification/allowed-change policy cannot be promised.
 * An untouched signed document can be copied without consent.
 * No signing, crypto, printing, flattening, or UI is implemented here. */
void view_pdf_save(view_pdf *pdf, const char *path, unsigned flags);
#endif
