/* Standard MuPDF 1.28 PDF annotation/form/journal backend. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "view-pdf.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct view_pdf {
    fz_context *ctx;
    pdf_document *doc;
    uint64_t revision;
    int clean_position, initial_dirty, signature_changes;
};

static char *copy(fz_context *ctx, const char *s) { return fz_strdup(ctx, s ? s : ""); }
static void argument(fz_context *ctx, const char *s) { fz_throw(ctx, FZ_ERROR_ARGUMENT, "%s", s); }
static void system_error(fz_context *ctx, const char *s) { fz_throw(ctx, FZ_ERROR_SYSTEM, "%s: %s", s, strerror(errno)); }
static view_pdf_id object_id(fz_context *ctx, int page, pdf_obj *o)
{
    return (view_pdf_id){page, pdf_to_num(ctx, o), pdf_to_gen(ctx, o)};
}
static int same_id(fz_context *ctx, pdf_obj *o, view_pdf_id id)
{
    return pdf_to_num(ctx, o) == id.object && pdf_to_gen(ctx, o) == id.generation;
}
static int valid_rect(fz_rect r)
{
    return isfinite(r.x0) && isfinite(r.y0) && isfinite(r.x1) && isfinite(r.y1) && r.x1 > r.x0 && r.y1 > r.y0;
}
static int position(view_pdf *p) { int steps; return pdf_undoredo_state(p->ctx, p->doc, &steps); }
static void changed(view_pdf *p, int before)
{
    if (position(p) != before) {
        p->signature_changes = 1;
        if (before < p->clean_position) p->clean_position = -1;
    }
    ++p->revision;
}

view_pdf *view_pdf_attach(fz_context *ctx, fz_document *document)
{
    pdf_document *doc = pdf_specifics(ctx, document);
    view_pdf *p = NULL;
    if (!doc) return NULL;
    fz_var(p);
    fz_try(ctx) {
        p = fz_malloc_struct(ctx, view_pdf);
        p->ctx = ctx;
        p->doc = pdf_keep_document(ctx, doc);
        p->initial_dirty = p->signature_changes = pdf_has_unsaved_changes(ctx, doc);
        pdf_enable_js(ctx, doc);
        pdf_enable_journal(ctx, doc);
        p->clean_position = position(p);
    }
    fz_catch(ctx) { view_pdf_drop(p); fz_rethrow(ctx); }
    return p;
}
void view_pdf_drop(view_pdf *p)
{
    if (p) { fz_context *ctx = p->ctx; pdf_drop_document(ctx, p->doc); fz_free(ctx, p); }
}
view_pdf_status view_pdf_get_status(view_pdf *p)
{
    view_pdf_status s = {0};
    fz_context *ctx = p->ctx;
    s.position = pdf_undoredo_state(ctx, p->doc, &s.steps);
    s.dirty = p->initial_dirty || s.position != p->clean_position;
    s.can_undo = pdf_can_undo(ctx, p->doc); s.can_redo = pdf_can_redo(ctx, p->doc);
    s.javascript = pdf_js_supported(ctx, p->doc);
    s.can_annotate = pdf_has_permission(ctx, p->doc, FZ_PERMISSION_ANNOTATE);
    s.can_fill_forms = pdf_has_permission(ctx, p->doc, FZ_PERMISSION_FORM);
    s.has_xfa = !!pdf_dict_getp(ctx, pdf_trailer(ctx, p->doc), "Root/AcroForm/XFA");
    s.revision = p->revision;
    return s;
}
int view_pdf_annotation_supported(enum pdf_annot_type type)
{
    switch (type) {
    case PDF_ANNOT_TEXT: case PDF_ANNOT_HIGHLIGHT: case PDF_ANNOT_UNDERLINE:
    case PDF_ANNOT_STRIKE_OUT: case PDF_ANNOT_SQUIGGLY: case PDF_ANNOT_INK:
    case PDF_ANNOT_LINE: case PDF_ANNOT_SQUARE: case PDF_ANNOT_CIRCLE:
    case PDF_ANNOT_POLYGON: case PDF_ANNOT_POLY_LINE: case PDF_ANNOT_STAMP:
    case PDF_ANNOT_FREE_TEXT: return 1;
    default: return 0;
    }
}
view_pdf_annotation_spec view_pdf_default_annotation(enum pdf_annot_type type, fz_rect rect)
{
    view_pdf_annotation_spec s = {0};
    s.type = type; s.rect = rect; s.color_n = 3;
    s.color[0] = s.color[1] = 1; s.opacity = 1;
    s.width = type == PDF_ANNOT_FREE_TEXT ? 0 : 1;
    s.flags = PDF_ANNOT_IS_PRINT; s.font = "Helv"; s.font_size = 12;
    s.icon = type == PDF_ANNOT_STAMP ? "Draft" : "Note";
    s.line[0] = fz_make_point(rect.x0, rect.y0); s.line[1] = fz_make_point(rect.x1, rect.y1);
    return s;
}
static void read_annotation(view_pdf *p, pdf_annot *a, int page, view_pdf_annotation *out)
{
    fz_context *ctx = p->ctx;
    out->id = object_id(ctx, page, pdf_annot_obj(ctx, a));
    out->type = pdf_annot_type(ctx, a);
    out->editable = view_pdf_annotation_supported(out->type);
    out->rect = pdf_annot_has_rect(ctx, a) ? pdf_annot_rect(ctx, a) : pdf_bound_annot(ctx, a);
    out->name = copy(ctx, pdf_annot_name(ctx, a));
    out->contents = copy(ctx, pdf_annot_contents(ctx, a));
    out->author = copy(ctx, pdf_annot_has_author(ctx, a) ? pdf_annot_author(ctx, a) : "");
    out->icon = copy(ctx, pdf_annot_has_icon_name(ctx, a) ? pdf_annot_icon_name(ctx, a) : "");
    out->flags = pdf_annot_flags(ctx, a);
    out->opacity = pdf_annot_opacity(ctx, a);
    pdf_annot_color(ctx, a, &out->color_n, out->color);
    if (pdf_annot_has_border(ctx, a)) out->width = pdf_annot_border_width(ctx, a);
    if (pdf_annot_has_quad_points(ctx, a)) {
        out->quad_count = pdf_annot_quad_point_count(ctx, a);
        out->quads = fz_calloc(ctx, out->quad_count, sizeof(*out->quads));
        for (int i = 0; i < out->quad_count; ++i) out->quads[i] = pdf_annot_quad_point(ctx, a, i);
    }
    if (pdf_annot_has_vertices(ctx, a)) {
        out->vertex_count = pdf_annot_vertex_count(ctx, a);
        out->vertices = fz_calloc(ctx, out->vertex_count, sizeof(*out->vertices));
        for (int i = 0; i < out->vertex_count; ++i) out->vertices[i] = pdf_annot_vertex(ctx, a, i);
    }
    if (pdf_annot_has_line(ctx, a)) pdf_annot_line(ctx, a, &out->line[0], &out->line[1]);
    if (pdf_annot_has_ink_list(ctx, a)) {
        out->stroke_count = pdf_annot_ink_list_count(ctx, a);
        out->stroke_sizes = fz_calloc(ctx, out->stroke_count, sizeof(int));
        for (int i = 0; i < out->stroke_count; ++i) {
            int n = pdf_annot_ink_list_stroke_count(ctx, a, i);
            if (n > INT_MAX - out->point_count) fz_throw(ctx, FZ_ERROR_LIMIT, "too many ink points");
            out->stroke_sizes[i] = n; out->point_count += n;
        }
        out->points = fz_calloc(ctx, out->point_count, sizeof(*out->points));
        for (int i = 0, k = 0; i < out->stroke_count; ++i)
            for (int j = 0; j < out->stroke_sizes[i]; ++j)
                out->points[k++] = pdf_annot_ink_list_stroke_vertex(ctx, a, i, j);
    }
}
void view_pdf_drop_annotations(fz_context *ctx, view_pdf_annotations *list)
{
    for (int i = 0; i < list->count; ++i) {
        view_pdf_annotation *a = &list->items[i];
        fz_free(ctx, a->name); fz_free(ctx, a->author); fz_free(ctx, a->contents); fz_free(ctx, a->icon);
        fz_free(ctx, a->quads); fz_free(ctx, a->vertices); fz_free(ctx, a->stroke_sizes); fz_free(ctx, a->points);
    }
    fz_free(ctx, list->items); *list = (view_pdf_annotations){0};
}
view_pdf_annotations view_pdf_list_annotations(view_pdf *p, int page_number)
{
    fz_context *ctx = p->ctx;
    pdf_page *page = NULL;
    view_pdf_annotations list = {0};
    fz_var(page); fz_var(list);
    fz_try(ctx) {
        page = pdf_load_page(ctx, p->doc, page_number);
        int n = 0;
        for (pdf_annot *a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a)) ++n;
        list.items = fz_calloc(ctx, n, sizeof(*list.items));
        for (pdf_annot *a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a))
            read_annotation(p, a, page_number, &list.items[list.count++]);
    }
    fz_always(ctx) { fz_drop_page(ctx, (fz_page *)page); }
    fz_catch(ctx) { view_pdf_drop_annotations(ctx, &list); fz_rethrow(ctx); }
    return list;
}
static pdf_annot *find_annot(view_pdf *p, pdf_page *page, view_pdf_id id, int widget)
{
    fz_context *ctx = p->ctx;
    for (pdf_annot *a = widget ? pdf_first_widget(ctx, page) : pdf_first_annot(ctx, page); a;
         a = widget ? pdf_next_widget(ctx, a) : pdf_next_annot(ctx, a))
        if (same_id(ctx, pdf_annot_obj(ctx, a), id)) return a;
    argument(ctx, "annotation/widget no longer exists on this page"); return NULL;
}
static void check_point(fz_context *ctx, fz_point pt)
{
    if (!isfinite(pt.x) || !isfinite(pt.y)) argument(ctx, "non-finite annotation point");
}
static void validate_spec(fz_context *ctx, unsigned mask, const view_pdf_annotation_spec *s)
{
    if (!s || mask & ~VIEW_PDF_ALL) argument(ctx, "invalid annotation update");
    if (mask & (VIEW_PDF_COLOR | VIEW_PDF_TEXT_STYLE)) {
        if (s->color_n != 0 && s->color_n != 1 && s->color_n != 3 && s->color_n != 4) argument(ctx, "invalid color components");
        for (int i = 0; i < s->color_n; ++i)
            if (!isfinite(s->color[i]) || s->color[i] < 0 || s->color[i] > 1) argument(ctx, "invalid annotation color");
    }
    if ((mask & VIEW_PDF_OPACITY) && (!isfinite(s->opacity) || s->opacity < 0 || s->opacity > 1)) argument(ctx, "invalid opacity");
    if ((mask & VIEW_PDF_WIDTH) && (!isfinite(s->width) || s->width < 0)) argument(ctx, "invalid border width");
    if ((mask & VIEW_PDF_TEXT_STYLE) && (!isfinite(s->font_size) || s->font_size <= 0)) argument(ctx, "invalid font size");
    if (!(mask & VIEW_PDF_GEOMETRY)) return;
    if (!valid_rect(s->rect)) argument(ctx, "invalid annotation rectangle");
    if (s->quad_count < 0 || (s->quad_count && !s->quads) || s->vertex_count < 0 || (s->vertex_count && !s->vertices) ||
        s->stroke_count < 0 || s->point_count < 0 || (s->stroke_count && !s->stroke_sizes) || (s->point_count && !s->points))
        argument(ctx, "invalid annotation geometry arrays");
    for (int i = 0; i < s->quad_count; ++i) {
        check_point(ctx, s->quads[i].ul); check_point(ctx, s->quads[i].ur);
        check_point(ctx, s->quads[i].ll); check_point(ctx, s->quads[i].lr);
    }
    for (int i = 0; i < s->vertex_count; ++i) check_point(ctx, s->vertices[i]);
    int total = 0;
    for (int i = 0; i < s->stroke_count; ++i) {
        if (s->stroke_sizes[i] < 1 || s->stroke_sizes[i] > INT_MAX - total) argument(ctx, "invalid ink stroke");
        total += s->stroke_sizes[i];
    }
    if (total != s->point_count) argument(ctx, "ink point count mismatch");
    for (int i = 0; i < s->point_count; ++i) check_point(ctx, s->points[i]);
    check_point(ctx, s->line[0]); check_point(ctx, s->line[1]);
}
static void apply_spec(view_pdf *p, pdf_annot *a, unsigned mask, const view_pdf_annotation_spec *s)
{
    fz_context *ctx = p->ctx;
    if (mask & VIEW_PDF_CONTENTS) pdf_set_annot_contents(ctx, a, s->contents ? s->contents : "");
    if (mask & VIEW_PDF_AUTHOR) {
        if (!pdf_annot_has_author(ctx, a)) argument(ctx, "annotation has no author property");
        pdf_set_annot_author(ctx, a, s->author ? s->author : "");
    }
    if (mask & VIEW_PDF_COLOR) pdf_set_annot_color(ctx, a, s->color_n, s->color);
    if (mask & VIEW_PDF_OPACITY) pdf_set_annot_opacity(ctx, a, s->opacity);
    if (mask & VIEW_PDF_FLAGS) pdf_set_annot_flags(ctx, a, s->flags);
    if (mask & VIEW_PDF_WIDTH) {
        if (!pdf_annot_has_border(ctx, a)) argument(ctx, "annotation has no border property");
        pdf_set_annot_border_width(ctx, a, s->width);
    }
    if (mask & VIEW_PDF_ICON) {
        if (!pdf_annot_has_icon_name(ctx, a)) argument(ctx, "annotation has no icon property");
        pdf_set_annot_icon_name(ctx, a, s->icon ? s->icon : "");
    }
    if (mask & VIEW_PDF_TEXT_STYLE) {
        if (!pdf_annot_has_default_appearance(ctx, a)) argument(ctx, "annotation has no text style");
        pdf_set_annot_default_appearance(ctx, a, s->font ? s->font : "Helv", s->font_size, s->color_n, s->color);
    }
    if (mask & VIEW_PDF_STAMP_IMAGE) {
        if (pdf_annot_type(ctx, a) != PDF_ANNOT_STAMP || !s->stamp_image) argument(ctx, "stamp image required");
        pdf_set_annot_stamp_image(ctx, a, s->stamp_image);
    }
    if (mask & VIEW_PDF_GEOMETRY) {
        if (pdf_annot_has_quad_points(ctx, a)) {
            if (s->quad_count < 1) argument(ctx, "markup requires text quads");
            pdf_set_annot_quad_points(ctx, a, s->quad_count, s->quads);
        } else if (pdf_annot_has_ink_list(ctx, a)) {
            if (s->stroke_count < 1) argument(ctx, "ink requires strokes");
            pdf_set_annot_ink_list(ctx, a, s->stroke_count, s->stroke_sizes, s->points);
        } else if (pdf_annot_has_vertices(ctx, a)) {
            int min = pdf_annot_type(ctx, a) == PDF_ANNOT_POLYGON ? 3 : 2;
            if (s->vertex_count < min) argument(ctx, "polygon/polyline requires vertices");
            pdf_set_annot_vertices(ctx, a, s->vertex_count, s->vertices);
        } else if (pdf_annot_has_line(ctx, a)) pdf_set_annot_line(ctx, a, s->line[0], s->line[1]);
        else if (pdf_annot_has_rect(ctx, a)) pdf_set_annot_rect(ctx, a, s->rect);
        else argument(ctx, "annotation geometry is not editable");
    }
}
view_pdf_id view_pdf_create_annotation(view_pdf *p, int number, const view_pdf_annotation_spec *s)
{
    fz_context *ctx = p->ctx;
    pdf_page *page = NULL; pdf_annot *a = NULL;
    view_pdf_id id = {0}; int begun = 0, before = position(p);
    fz_var(page); fz_var(a); fz_var(begun); fz_var(id);
    validate_spec(ctx, VIEW_PDF_ALL, s);
    if (!view_pdf_annotation_supported(s->type)) argument(ctx, "annotation creation type unsupported");
    if (!pdf_has_permission(ctx, p->doc, FZ_PERMISSION_ANNOTATE)) argument(ctx, "PDF forbids annotation editing");
    fz_try(ctx) {
        page = pdf_load_page(ctx, p->doc, number);
        pdf_begin_operation(ctx, p->doc, "Create annotation"); begun = 1;
        a = pdf_create_annot(ctx, page, s->type);
        unsigned mask = VIEW_PDF_CONTENTS | VIEW_PDF_COLOR | VIEW_PDF_OPACITY | VIEW_PDF_FLAGS | VIEW_PDF_GEOMETRY;
        if (pdf_annot_has_author(ctx, a)) mask |= VIEW_PDF_AUTHOR;
        if (pdf_annot_has_border(ctx, a)) mask |= VIEW_PDF_WIDTH;
        if (pdf_annot_has_icon_name(ctx, a)) mask |= VIEW_PDF_ICON;
        if (pdf_annot_has_default_appearance(ctx, a)) mask |= VIEW_PDF_TEXT_STYLE;
        if (s->stamp_image) mask |= VIEW_PDF_STAMP_IMAGE;
        apply_spec(p, a, mask, s);
        id = object_id(ctx, number, pdf_annot_obj(ctx, a));
        char name[64]; snprintf(name, sizeof(name), "locachy-%d-%llu", id.object, (unsigned long long)p->revision);
        pdf_set_annot_name(ctx, a, name);
        pdf_update_page(ctx, page);
        pdf_end_operation(ctx, p->doc); begun = 0; changed(p, before);
    }
    fz_always(ctx) { if (begun) pdf_abandon_operation(ctx, p->doc); pdf_drop_annot(ctx, a); fz_drop_page(ctx, (fz_page *)page); }
    fz_catch(ctx) { fz_rethrow(ctx); }
    return id;
}
typedef void (*edit_fn)(view_pdf *, pdf_annot *, void *);
static void edit_annotation(view_pdf *p, view_pdf_id id, const char *title, edit_fn fn, void *arg)
{
    fz_context *ctx = p->ctx; pdf_page *page = NULL;
    int begun = 0, before = position(p);
    fz_var(page); fz_var(begun);
    if (!pdf_has_permission(ctx, p->doc, FZ_PERMISSION_ANNOTATE)) argument(ctx, "PDF forbids annotation editing");
    fz_try(ctx) {
        page = pdf_load_page(ctx, p->doc, id.page);
        pdf_annot *a = find_annot(p, page, id, 0);
        int flags = pdf_annot_flags(ctx, a);
        if (flags & (PDF_ANNOT_IS_READ_ONLY | PDF_ANNOT_IS_LOCKED)) argument(ctx, "annotation is locked/read-only");
        if (!view_pdf_annotation_supported(pdf_annot_type(ctx, a))) argument(ctx, "annotation editing type unsupported");
        pdf_begin_operation(ctx, p->doc, title); begun = 1;
        fn(p, a, arg); pdf_update_page(ctx, page);
        pdf_end_operation(ctx, p->doc); begun = 0; changed(p, before);
    }
    fz_always(ctx) { if (begun) pdf_abandon_operation(ctx, p->doc); fz_drop_page(ctx, (fz_page *)page); }
    fz_catch(ctx) { fz_rethrow(ctx); }
}
struct update { unsigned mask; const view_pdf_annotation_spec *s; };
static void update_annotation(view_pdf *p, pdf_annot *a, void *arg)
{
    struct update *u = arg;
    if ((u->mask & VIEW_PDF_CONTENTS) && (pdf_annot_flags(p->ctx, a) & PDF_ANNOT_IS_LOCKED_CONTENTS))
        argument(p->ctx, "annotation contents are locked");
    apply_spec(p, a, u->mask, u->s);
}
void view_pdf_update_annotation(view_pdf *p, view_pdf_id id, unsigned mask, const view_pdf_annotation_spec *s)
{
    validate_spec(p->ctx, mask, s); struct update u = {mask, s};
    edit_annotation(p, id, "Edit annotation", update_annotation, &u);
}
static void transform_annotation(view_pdf *p, pdf_annot *a, void *arg)
{
    fz_context *ctx = p->ctx;
    view_pdf_annotations list = {0};
    fz_rect target = *(fz_rect *)arg;
    fz_var(list);
    fz_try(ctx) {
        list.items = fz_calloc(ctx, 1, sizeof(*list.items)); list.count = 1;
        view_pdf_annotation *r = list.items;
        read_annotation(p, a, 0, r);
        if (!valid_rect(r->rect)) argument(ctx, "cannot transform degenerate annotation");
        float sx = (target.x1-target.x0)/(r->rect.x1-r->rect.x0), sy = (target.y1-target.y0)/(r->rect.y1-r->rect.y0);
        fz_matrix m = {sx, 0, 0, sy, target.x0-r->rect.x0*sx, target.y0-r->rect.y0*sy};
        view_pdf_annotation_spec s = view_pdf_default_annotation(r->type, target);
        for (int i = 0; i < r->quad_count; ++i) r->quads[i] = fz_transform_quad(r->quads[i], m);
        for (int i = 0; i < r->vertex_count; ++i) r->vertices[i] = fz_transform_point(r->vertices[i], m);
        for (int i = 0; i < r->point_count; ++i) r->points[i] = fz_transform_point(r->points[i], m);
        s.quad_count = r->quad_count; s.quads = r->quads;
        s.vertex_count = r->vertex_count; s.vertices = r->vertices;
        s.stroke_count = r->stroke_count; s.stroke_sizes = r->stroke_sizes; s.point_count = r->point_count; s.points = r->points;
        s.line[0] = fz_transform_point(r->line[0], m); s.line[1] = fz_transform_point(r->line[1], m);
        apply_spec(p, a, VIEW_PDF_GEOMETRY, &s);
    }
    fz_always(ctx) { view_pdf_drop_annotations(ctx, &list); }
    fz_catch(ctx) { fz_rethrow(ctx); }
}
void view_pdf_transform_annotation(view_pdf *p, view_pdf_id id, fz_rect target)
{
    if (!valid_rect(target)) argument(p->ctx, "invalid target rectangle");
    edit_annotation(p, id, "Move/resize annotation", transform_annotation, &target);
}
static void delete_annotation(view_pdf *p, pdf_annot *a, void *arg)
{
    (void)arg; pdf_delete_annot(p->ctx, pdf_annot_page(p->ctx, a), a);
}
void view_pdf_delete_annotation(view_pdf *p, view_pdf_id id) { edit_annotation(p, id, "Delete annotation", delete_annotation, NULL); }
void view_pdf_undo(view_pdf *p) { pdf_undo(p->ctx, p->doc); p->signature_changes = 1; ++p->revision; }
void view_pdf_redo(view_pdf *p) { pdf_redo(p->ctx, p->doc); p->signature_changes = 1; ++p->revision; }

void view_pdf_drop_widgets(fz_context *ctx, view_pdf_widgets *list)
{
    for (int i = 0; i < list->count; ++i) {
        view_pdf_widget *w = &list->items[i];
        fz_free(ctx, w->name); fz_free(ctx, w->value); fz_free(ctx, w->on_value);
        for (int j = 0; j < w->option_count; ++j) { fz_free(ctx, w->options[j].label); fz_free(ctx, w->options[j].value); }
        for (int j = 0; j < w->value_count; ++j) fz_free(ctx, w->values[j]);
        fz_free(ctx, w->options); fz_free(ctx, w->values);
    }
    fz_free(ctx, list->items); *list = (view_pdf_widgets){0};
}
view_pdf_widgets view_pdf_list_widgets(view_pdf *p, int number)
{
    fz_context *ctx = p->ctx; pdf_page *page = NULL;
    view_pdf_widgets list = {0};
    fz_var(page); fz_var(list);
    fz_try(ctx) {
        page = pdf_load_page(ctx, p->doc, number);
        int n = 0;
        for (pdf_annot *a = pdf_first_widget(ctx, page); a; a = pdf_next_widget(ctx, a)) ++n;
        list.items = fz_calloc(ctx, n, sizeof(*list.items));
        for (pdf_annot *a = pdf_first_widget(ctx, page); a; a = pdf_next_widget(ctx, a)) {
            view_pdf_widget *w = &list.items[list.count++];
            pdf_obj *o = pdf_annot_obj(ctx, a);
            w->id = object_id(ctx, number, o); w->type = pdf_widget_type(ctx, a);
            w->rect = pdf_bound_widget(ctx, a); w->name = pdf_load_field_name(ctx, o);
            w->value = copy(ctx, pdf_annot_field_value(ctx, a));
            w->flags = pdf_field_flags(ctx, o); w->readonly = !!pdf_widget_is_readonly(ctx, a);
            w->required = !!(w->flags & PDF_FIELD_IS_REQUIRED);
            if (w->type == PDF_WIDGET_TYPE_TEXT) w->max_length = pdf_text_widget_max_len(ctx, a);
            if (w->type == PDF_WIDGET_TYPE_CHECKBOX || w->type == PDF_WIDGET_TYPE_RADIOBUTTON)
                w->on_value = copy(ctx, pdf_to_name(ctx, pdf_button_field_on_state(ctx, o)));
            if (w->type == PDF_WIDGET_TYPE_COMBOBOX || w->type == PDF_WIDGET_TYPE_LISTBOX) {
                w->multiselect = pdf_choice_widget_is_multiselect(ctx, a);
                n = pdf_choice_field_option_count(ctx, o);
                w->options = fz_calloc(ctx, n, sizeof(*w->options)); w->option_count = n;
                for (int i = 0; i < n; ++i) {
                    w->options[i].label = copy(ctx, pdf_choice_field_option(ctx, o, 0, i));
                    w->options[i].value = copy(ctx, pdf_choice_field_option(ctx, o, 1, i));
                }
                pdf_obj *v = pdf_dict_get_inheritable(ctx, o, PDF_NAME(V));
                n = pdf_is_array(ctx, v) ? pdf_array_len(ctx, v) : (pdf_is_string(ctx, v) ? 1 : 0);
                w->values = fz_calloc(ctx, n, sizeof(*w->values)); w->value_count = n;
                for (int i = 0; i < n; ++i) w->values[i] = copy(ctx, pdf_to_text_string(ctx, pdf_is_array(ctx, v) ? pdf_array_get(ctx, v, i) : v));
            }
        }
    }
    fz_always(ctx) { fz_drop_page(ctx, (fz_page *)page); }
    fz_catch(ctx) { view_pdf_drop_widgets(ctx, &list); fz_rethrow(ctx); }
    return list;
}
static int has_choice_value(fz_context *ctx, pdf_obj *o, const char *value)
{
    int n = pdf_choice_field_option_count(ctx, o);
    for (int i = 0; i < n; ++i) if (!strcmp(value, pdf_choice_field_option(ctx, o, 1, i))) return 1;
    return 0;
}
void view_pdf_set_widget(view_pdf *p, view_pdf_id id, int count, const char *const *values)
{
    fz_context *ctx = p->ctx; pdf_page *page = NULL;
    int begun = 0, before = position(p);
    fz_var(page); fz_var(begun);
    if (count < 0 || (count && !values)) argument(ctx, "invalid widget values");
    for (int i = 0; i < count; ++i) if (!values[i]) argument(ctx, "NULL widget value");
    if (!pdf_has_permission(ctx, p->doc, FZ_PERMISSION_FORM)) argument(ctx, "PDF forbids filling forms");
    fz_try(ctx) {
        page = pdf_load_page(ctx, p->doc, id.page);
        pdf_annot *a = find_annot(p, page, id, 1);
        pdf_obj *o = pdf_annot_obj(ctx, a);
        enum pdf_widget_type type = pdf_widget_type(ctx, a);
        int flags = pdf_field_flags(ctx, o);
        if (pdf_widget_is_readonly(ctx, a)) argument(ctx, "widget is read-only");
        if (type != PDF_WIDGET_TYPE_TEXT && type != PDF_WIDGET_TYPE_COMBOBOX && type != PDF_WIDGET_TYPE_LISTBOX &&
            type != PDF_WIDGET_TYPE_CHECKBOX && type != PDF_WIDGET_TYPE_RADIOBUTTON) argument(ctx, "widget type is not value-editable");
        int multi = pdf_choice_widget_is_multiselect(ctx, a);
        if (!multi && count != 1) argument(ctx, "widget requires one value");
        if (type == PDF_WIDGET_TYPE_COMBOBOX || type == PDF_WIDGET_TYPE_LISTBOX) {
            for (int i = 0; i < count; ++i) {
                if (!(type == PDF_WIDGET_TYPE_COMBOBOX && (flags & PDF_CH_FIELD_IS_EDIT)) && !has_choice_value(ctx, o, values[i]))
                    argument(ctx, "choice export value is not an option");
                for (int j = 0; j < i; ++j) if (!strcmp(values[j], values[i])) argument(ctx, "duplicate selected option");
            }
            if (multi && count != 1) {
                pdf_obj *aa = pdf_dict_get_inheritable(ctx, o, PDF_NAME(AA));
                if (pdf_dict_get(ctx, aa, PDF_NAME(K)) || pdf_dict_get(ctx, aa, PDF_NAME(V)))
                    argument(ctx, "MuPDF cannot validate array-valued choice events");
            }
        }
        if (type == PDF_WIDGET_TYPE_TEXT) {
            int max = pdf_text_widget_max_len(ctx, a);
            if (max && fz_utflen(values[0]) > max) argument(ctx, "text exceeds widget MaxLen");
        }
        if (type == PDF_WIDGET_TYPE_CHECKBOX || type == PDF_WIDGET_TYPE_RADIOBUTTON) {
            const char *on = pdf_to_name(ctx, pdf_button_field_on_state(ctx, o));
            if (strcmp(values[0], "Off") && strcmp(values[0], on)) argument(ctx, "invalid button export value");
            if (type == PDF_WIDGET_TYPE_RADIOBUTTON && !strcmp(values[0], "Off") && (flags & PDF_BTN_FIELD_IS_NO_TOGGLE_TO_OFF))
                argument(ctx, "radio disallows toggle to Off");
        }
        pdf_begin_operation(ctx, p->doc, "Fill form field"); begun = 1;
        pdf_annot_event_focus(ctx, a);
        int accepted = 1;
        if (type == PDF_WIDGET_TYPE_TEXT) accepted = pdf_set_text_field_value(ctx, a, values[0]);
        else if (type == PDF_WIDGET_TYPE_COMBOBOX || type == PDF_WIDGET_TYPE_LISTBOX) {
            if (count == 1) accepted = pdf_set_choice_field_value(ctx, a, values[0]);
            else pdf_choice_widget_set_value(ctx, a, count, (const char **)values);
        } else {
            const char *as = pdf_dict_get_name(ctx, o, PDF_NAME(AS));
            if (strcmp(as, values[0])) {
                pdf_annot_event_down(ctx, a);
                accepted = pdf_toggle_widget(ctx, a);
                pdf_annot_event_up(ctx, a);
            }
        }
        if (!accepted) argument(ctx, "widget event validation rejected the value");
        pdf_annot_event_blur(ctx, a);
        pdf_calculate_form(ctx, p->doc); pdf_update_page(ctx, page); pdf_update_open_pages(ctx, p->doc);
        pdf_end_operation(ctx, p->doc); begun = 0; changed(p, before);
    }
    fz_always(ctx) { if (begun) pdf_abandon_operation(ctx, p->doc); fz_drop_page(ctx, (fz_page *)page); }
    fz_catch(ctx) { fz_rethrow(ctx); }
}

void view_pdf_drop_signatures(fz_context *ctx, view_pdf_signatures *list)
{
    for (int i = 0; i < list->count; ++i) {
        view_pdf_signature *s = &list->items[i];
        fz_free(ctx, s->field); fz_free(ctx, s->name); fz_free(ctx, s->reason); fz_free(ctx, s->location);
        fz_free(ctx, s->date); fz_free(ctx, s->signatory); fz_free(ctx, s->error);
    }
    fz_free(ctx, list->items); *list = (view_pdf_signatures){0};
}
struct sig_walk { view_pdf *p; view_pdf_signatures *list; pdf_pkcs7_verifier *verifier; };
static void signature_field(fz_context *ctx, pdf_obj *o, void *arg, pdf_obj **inherited)
{
    (void)inherited;
    struct sig_walk *w = arg;
    if (pdf_field_type(ctx, o) != PDF_WIDGET_TYPE_SIGNATURE || pdf_array_len(ctx, pdf_dict_get(ctx, o, PDF_NAME(Kids)))) return;
    int n = w->list->count;
    w->list->items = fz_realloc_array(ctx, w->list->items, n+1, view_pdf_signature);
    view_pdf_signature *s = &w->list->items[n]; memset(s, 0, sizeof(*s)); ++w->list->count;
    s->id = object_id(ctx, -1, o); s->field = pdf_load_field_name(ctx, o);
    s->signed_value = pdf_signature_is_signed(ctx, w->p->doc, o);
    s->document_modified = view_pdf_get_status(w->p).dirty;
    pdf_obj *v = pdf_dict_get_inheritable(ctx, o, PDF_NAME(V));
    s->name = copy(ctx, pdf_dict_get_text_string(ctx, v, PDF_NAME(Name)));
    s->reason = copy(ctx, pdf_dict_get_text_string(ctx, v, PDF_NAME(Reason)));
    s->location = copy(ctx, pdf_dict_get_text_string(ctx, v, PDF_NAME(Location)));
    s->date = copy(ctx, pdf_dict_get_text_string(ctx, v, PDF_NAME(M)));
    s->digest_status = s->certificate_status = s->signed_value ? VIEW_PDF_SIGNATURE_UNAVAILABLE : PDF_SIGNATURE_ERROR_NOT_SIGNED;
    s->change_status = VIEW_PDF_SIGNATURE_FAILED;
    if (s->signed_value) {
        fz_try(ctx) { s->change_status = pdf_validate_signature(ctx, w->p->doc, o); }
        fz_catch(ctx) { s->error = copy(ctx, fz_convert_error(ctx, NULL)); }
    }
    if (!s->signed_value || !w->verifier) return;
    pdf_pkcs7_distinguished_name *dn = NULL;
    fz_var(dn);
    fz_try(ctx) {
        s->digest_status = pdf_check_digest(ctx, w->verifier, w->p->doc, o);
        s->certificate_status = pdf_check_certificate(ctx, w->verifier, w->p->doc, o);
        dn = pdf_signature_get_signatory(ctx, w->verifier, w->p->doc, o);
        if (dn) s->signatory = pdf_signature_format_distinguished_name(ctx, dn);
    }
    fz_always(ctx) { pdf_signature_drop_distinguished_name(ctx, dn); }
    fz_catch(ctx) {
        fz_free(ctx, s->error); s->error = NULL;
        s->error = copy(ctx, fz_convert_error(ctx, NULL));
        if (s->digest_status == VIEW_PDF_SIGNATURE_UNAVAILABLE) s->digest_status = VIEW_PDF_SIGNATURE_FAILED;
        if (s->certificate_status == VIEW_PDF_SIGNATURE_UNAVAILABLE) s->certificate_status = VIEW_PDF_SIGNATURE_FAILED;
    }
}
view_pdf_signatures view_pdf_list_signatures(view_pdf *p, pdf_pkcs7_verifier *verifier)
{
    fz_context *ctx = p->ctx; view_pdf_signatures list = {0};
    struct sig_walk w = {p, &list, verifier};
    pdf_page *page = NULL;
    fz_var(list); fz_var(page);
    fz_try(ctx) {
        pdf_walk_tree(ctx, pdf_dict_getp(ctx, pdf_trailer(ctx, p->doc), "Root/AcroForm/Fields"), PDF_NAME(Kids), signature_field, NULL, &w, NULL, NULL);
        int pages = pdf_count_pages(ctx, p->doc);
        for (int i = 0; i < pages; ++i) {
            page = pdf_load_page(ctx, p->doc, i);
            for (pdf_annot *a = pdf_first_widget(ctx, page); a; a = pdf_next_widget(ctx, a))
                for (int j = 0; j < list.count; ++j)
                    if (same_id(ctx, pdf_annot_obj(ctx, a), list.items[j].id)) list.items[j].id.page = i;
            fz_drop_page(ctx, (fz_page *)page); page = NULL;
        }
    }
    fz_always(ctx) { fz_drop_page(ctx, (fz_page *)page); }
    fz_catch(ctx) { view_pdf_drop_signatures(ctx, &list); fz_rethrow(ctx); }
    return list;
}

static int stat_equal(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
struct signed_count { pdf_document *doc; int count; };
static void count_signed_field(fz_context *ctx, pdf_obj *o, void *arg, pdf_obj **inherited)
{
    (void)inherited;
    struct signed_count *n = arg;
    if (!pdf_array_len(ctx, pdf_dict_get(ctx, o, PDF_NAME(Kids))) && pdf_signature_is_signed(ctx, n->doc, o)) ++n->count;
}
static int signed_fields(view_pdf *p)
{
    struct signed_count n = {p->doc, 0};
    pdf_walk_tree(p->ctx, pdf_dict_getp(p->ctx, pdf_trailer(p->ctx, p->doc), "Root/AcroForm/Fields"),
                  PDF_NAME(Kids), count_signed_field, NULL, &n, NULL, NULL);
    return n.count;
}
void view_pdf_save(view_pdf *p, const char *path, unsigned flags)
{
    fz_context *ctx = p->ctx;
    fz_output *out = NULL;
    FILE *file = NULL;
    char *resolved = NULL, *parent = NULL, *base = NULL;
    int dirfd = -1, fd = -1, temp_exists = 0, existed = 0;
    char temp[80] = {0}; struct stat old = {0};
    fz_var(out); fz_var(file); fz_var(resolved); fz_var(parent); fz_var(base);
    fz_var(dirfd); fz_var(fd); fz_var(temp_exists); fz_var(existed);
    if (getuid() != 1000 || geteuid() != 1000 || getgid() != 1000 || getegid() != 1000)
        argument(ctx, "PDF save requires unprivileged uid/gid 1000; root refused");
    if (!path || !*path || flags & ~VIEW_PDF_ALLOW_SIGNATURE_INVALIDATION) argument(ctx, "invalid save request");
    if (signed_fields(p) && p->signature_changes && !(flags & VIEW_PDF_ALLOW_SIGNATURE_INVALIDATION))
        argument(ctx, "saving signatures requires explicit invalidation consent");
    if (pdf_has_unsaved_sigs(ctx, p->doc)) argument(ctx, "pending signature creation is not supported by snapshot saving");
    if (!pdf_can_be_saved_incrementally(ctx, p->doc)) argument(ctx, "repaired/redacted PDF cannot be saved with undo-preserving snapshots");
    fz_try(ctx) {
        /* Resolve an existing symlink as a user-visible save target. The
         * directory descriptor pins the destination through rename. */
        resolved = realpath(path, NULL);
        if (!resolved) {
            if (errno != ENOENT) system_error(ctx, "resolve save target");
            struct stat link_st;
            if (!lstat(path, &link_st) && S_ISLNK(link_st.st_mode)) argument(ctx, "dangling save symlink");
            resolved = strdup(path); if (!resolved) system_error(ctx, "allocate save path");
        }
        char *slash = strrchr(resolved, '/');
        base = strdup(slash ? slash+1 : resolved);
        if (!base) system_error(ctx, "allocate save filename");
        if (!*base || !strcmp(base, ".") || !strcmp(base, "..")) argument(ctx, "invalid save filename");
        if (slash) { *slash = 0; parent = realpath(*resolved ? resolved : "/", NULL); }
        else parent = realpath(".", NULL);
        if (!parent) system_error(ctx, "resolve save directory");
        dirfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (dirfd < 0) system_error(ctx, "open save directory");
        int original = openat(dirfd, base, O_WRONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (original >= 0) {
            existed = 1;
            int r = fstat(original, &old); int e = errno; close(original); errno = e;
            if (r < 0) system_error(ctx, "stat save target");
            if (!S_ISREG(old.st_mode)) argument(ctx, "save target is not a regular file");
        } else if (errno != ENOENT) system_error(ctx, "open writable save target");
        /* mkstemp cannot use dirfd. A /proc/self/fd path pins this directory
         * and never follows a substituted parent between creation and rename. */
        char template[128]; snprintf(template, sizeof(template), "/proc/self/fd/%d/.locachy-pdf-XXXXXX", dirfd);
        fd = mkstemp(template); if (fd < 0) system_error(ctx, "create PDF save temporary");
        snprintf(temp, sizeof(temp), "%s", strrchr(template, '/')+1); temp_exists = 1;
        int stream_fd = dup(fd); if (stream_fd < 0) system_error(ctx, "duplicate PDF save stream");
        file = fdopen(stream_fd, "w+b");
        if (!file) { int e = errno; close(stream_fd); errno = e; system_error(ctx, "open PDF save stream"); }
        FILE *owned = file; file = NULL; /* MuPDF owns/closes FILE, even on constructor failure. */
        out = fz_new_output_with_file_ptr(ctx, owned);
        if (p->doc->file) pdf_write_snapshot(ctx, p->doc, out);
        else {
            pdf_write_options opts = pdf_default_write_options;
            opts.do_encrypt = PDF_ENCRYPT_KEEP; opts.dont_regenerate_id = 1;
            pdf_write_document(ctx, p->doc, out, &opts);
        }
        fz_close_output(ctx, out); fz_drop_output(ctx, out); out = NULL;
        if (fchmod(fd, existed ? old.st_mode & 0777 : 0600) < 0) system_error(ctx, "set PDF save permissions");
        if (fsync(fd)) system_error(ctx, "sync PDF save temporary");
        if (close(fd)) { fd = -1; system_error(ctx, "close PDF save temporary"); } fd = -1;
        struct stat now;
        if (existed) {
            if (fstatat(dirfd, base, &now, AT_SYMLINK_NOFOLLOW) < 0) system_error(ctx, "recheck PDF save target");
            if (!stat_equal(&old, &now)) argument(ctx, "save target changed during save");
        } else if (fstatat(dirfd, base, &now, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
            argument(ctx, "save target appeared during save");
        if (renameat(dirfd, temp, dirfd, base) < 0) system_error(ctx, "commit PDF save");
        temp_exists = 0;
        /* Rename is the commit point. A directory-fsync failure cannot be
         * rolled back safely and must not be presented as an uncommitted save. */
        if (fsync(dirfd) < 0) fz_warn(ctx, "PDF saved, but directory sync failed: %s", strerror(errno));
        p->initial_dirty = 0; p->signature_changes = 0; p->clean_position = position(p);
    }
    fz_always(ctx) {
        fz_drop_output(ctx, out); if (file) fclose(file); if (fd >= 0) close(fd);
        if (temp_exists && dirfd >= 0) unlinkat(dirfd, temp, 0);
        if (dirfd >= 0) close(dirfd);
        free(resolved); free(parent); free(base);
    }
    fz_catch(ctx) { fz_rethrow(ctx); }
}
