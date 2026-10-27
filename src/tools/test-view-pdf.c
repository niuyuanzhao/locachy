/* Generated PDFs only; no private input files. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../progs/view-pdf.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define REJECT(code) do { int rejected_ = 0; fz_var(rejected_); fz_try(ctx) { code; } fz_catch(ctx) { (void)fz_convert_error(ctx, NULL); rejected_ = 1; } CHECK(rejected_); } while (0)
static fz_context *ctx;
static char pathbuf[4096];
static const char *path(const char *dir, const char *name) { snprintf(pathbuf, sizeof(pathbuf), "%s/%s", dir, name); return pathbuf; }
static pdf_obj *field(pdf_document *doc, pdf_obj *page, pdf_obj *annots, pdf_obj *fields, const char *name, pdf_obj *ft, int flags, int y)
{
    pdf_obj *o = pdf_new_dict(ctx, doc, 12);
    pdf_dict_put(ctx, o, PDF_NAME(Type), PDF_NAME(Annot)); pdf_dict_put(ctx, o, PDF_NAME(Subtype), PDF_NAME(Widget));
    pdf_dict_put(ctx, o, PDF_NAME(FT), ft); pdf_dict_put_text_string(ctx, o, PDF_NAME(T), name);
    pdf_dict_put_int(ctx, o, PDF_NAME(Ff), flags); pdf_dict_put_int(ctx, o, PDF_NAME(F), PDF_ANNOT_IS_PRINT);
    pdf_dict_put_rect(ctx, o, PDF_NAME(Rect), fz_make_rect(40, y, 240, y+22));
    pdf_dict_put(ctx, o, PDF_NAME(P), page);
    pdf_dict_put_text_string(ctx, o, PDF_NAME(DA), "/Helv 12 Tf 0 g");
    pdf_obj *ref = pdf_add_object_drop(ctx, doc, o);
    pdf_array_push(ctx, annots, ref); pdf_array_push(ctx, fields, ref); pdf_drop_obj(ctx, ref);
    return pdf_array_get(ctx, fields, pdf_array_len(ctx, fields)-1);
}
static void js_action(pdf_document *doc, pdf_obj *o, pdf_obj *event, const char *script)
{
    pdf_obj *aa = pdf_dict_get(ctx, o, PDF_NAME(AA));
    if (!aa) aa = pdf_dict_put_dict(ctx, o, PDF_NAME(AA), 2);
    pdf_obj *a = pdf_dict_put_dict(ctx, aa, event, 2);
    pdf_dict_put(ctx, a, PDF_NAME(S), PDF_NAME(JavaScript)); pdf_dict_put_text_string(ctx, a, PDF_NAME(JS), script);
    (void)doc;
}
static void button_appearance(pdf_document *doc, pdf_obj *o, const char *name)
{
    pdf_obj *ap = pdf_dict_put_dict(ctx, o, PDF_NAME(AP), 1);
    pdf_obj *n = pdf_dict_put_dict(ctx, ap, PDF_NAME(N), 2);
    const char *names[] = {"Off", name};
    for (int i = 0; i < 2; ++i) {
        pdf_obj *d = pdf_new_dict(ctx, doc, 3);
        pdf_dict_put(ctx, d, PDF_NAME(Type), PDF_NAME(XObject)); pdf_dict_put(ctx, d, PDF_NAME(Subtype), PDF_NAME(Form));
        pdf_dict_put_rect(ctx, d, PDF_NAME(BBox), fz_make_rect(0, 0, 200, 22));
        fz_buffer *b = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)"q Q", 3);
        pdf_obj *s = pdf_add_stream(ctx, doc, b, d, 0);
        pdf_dict_puts(ctx, n, names[i], s); pdf_drop_obj(ctx, s); fz_drop_buffer(ctx, b); pdf_drop_obj(ctx, d);
    }
    pdf_dict_put(ctx, o, PDF_NAME(AS), PDF_NAME(Off)); pdf_dict_put(ctx, o, PDF_NAME(V), PDF_NAME(Off));
}
static void options(pdf_document *doc, pdf_obj *o)
{
    pdf_obj *arr = pdf_dict_put_array(ctx, o, PDF_NAME(Opt), 3);
    const char *values[] = {"a", "b", "c"}; const char *labels[] = {"Alpha", "Beta", "Gamma"};
    for (int i = 0; i < 3; ++i) {
        pdf_obj *pair = pdf_new_array(ctx, doc, 2);
        pdf_array_push_text_string(ctx, pair, values[i]); pdf_array_push_text_string(ctx, pair, labels[i]);
        pdf_array_push_drop(ctx, arr, pair);
    }
    pdf_dict_put_text_string(ctx, o, PDF_NAME(V), "a");
}
static void fixture(const char *dir)
{
    pdf_document *d = pdf_create_document(ctx);
    fz_buffer *b = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)"q Q", 3);
    pdf_obj *pg = pdf_add_page(ctx, d, fz_make_rect(0, 0, 612, 792), 0, NULL, b);
    pdf_insert_page(ctx, d, -1, pg); fz_drop_buffer(ctx, b);
    pdf_obj *root = pdf_dict_get(ctx, pdf_trailer(ctx, d), PDF_NAME(Root));
    pdf_obj *acro = pdf_dict_put_dict(ctx, root, PDF_NAME(AcroForm), 4);
    pdf_obj *fields = pdf_dict_put_array(ctx, acro, PDF_NAME(Fields), 10);
    pdf_dict_put_text_string(ctx, acro, PDF_NAME(DA), "/Helv 12 Tf 0 g");
    pdf_obj *dr = pdf_dict_put_dict(ctx, acro, PDF_NAME(DR), 1);
    pdf_obj *fonts = pdf_dict_put_dict(ctx, dr, PDF_NAME(Font), 1);
    fz_font *font = fz_new_base14_font(ctx, "Helvetica");
    pdf_obj *fontref = pdf_add_simple_font(ctx, d, font, 0); pdf_dict_puts(ctx, fonts, "Helv", fontref);
    fz_drop_font(ctx, font); pdf_drop_obj(ctx, fontref);
    pdf_obj *annots = pdf_dict_put_array(ctx, pg, PDF_NAME(Annots), 10);
    pdf_obj *o = field(d, pg, annots, fields, "text", PDF_NAME(Tx), PDF_FIELD_IS_REQUIRED, 700);
    pdf_dict_put_text_string(ctx, o, PDF_NAME(V), "before"); pdf_dict_put_int(ctx, o, PDF_NAME(MaxLen), 16);
    js_action(d, o, PDF_NAME(K), "if (event.change == 'keybad') event.rc = false;");
    js_action(d, o, PDF_NAME(V), "if (event.value == 'bad') event.rc = false;");
    o = field(d, pg, annots, fields, "readonly", PDF_NAME(Tx), PDF_FIELD_IS_READ_ONLY, 670);
    pdf_dict_put_text_string(ctx, o, PDF_NAME(V), "locked");
    o = field(d, pg, annots, fields, "check", PDF_NAME(Btn), 0, 640); button_appearance(d, o, "Yes");
    pdf_obj *radio = pdf_new_dict(ctx, d, 5);
    pdf_dict_put(ctx, radio, PDF_NAME(FT), PDF_NAME(Btn)); pdf_dict_put_text_string(ctx, radio, PDF_NAME(T), "radio");
    pdf_dict_put_int(ctx, radio, PDF_NAME(Ff), PDF_BTN_FIELD_IS_RADIO | PDF_BTN_FIELD_IS_NO_TOGGLE_TO_OFF);
    pdf_dict_put(ctx, radio, PDF_NAME(V), PDF_NAME(Off));
    pdf_obj *radio_ref = pdf_add_object_drop(ctx, d, radio); pdf_array_push(ctx, fields, radio_ref);
    pdf_obj *kids = pdf_dict_put_array(ctx, radio_ref, PDF_NAME(Kids), 2);
    for (int i = 0; i < 2; ++i) {
        o = field(d, pg, annots, kids, i ? "second" : "first", PDF_NAME(Btn), PDF_BTN_FIELD_IS_RADIO | PDF_BTN_FIELD_IS_NO_TOGGLE_TO_OFF, 610-30*i);
        pdf_dict_put(ctx, o, PDF_NAME(Parent), radio_ref); button_appearance(d, o, i ? "Two" : "One");
        pdf_dict_del(ctx, o, PDF_NAME(V)); pdf_dict_del(ctx, o, PDF_NAME(T));
        pdf_dict_del(ctx, o, PDF_NAME(FT)); pdf_dict_del(ctx, o, PDF_NAME(Ff));
    }
    pdf_drop_obj(ctx, radio_ref);
    o = field(d, pg, annots, fields, "combo", PDF_NAME(Ch), PDF_CH_FIELD_IS_COMBO, 550); options(d, o);
    js_action(d, o, PDF_NAME(V), "if (event.value == 'c') event.rc = false;");
    o = field(d, pg, annots, fields, "list", PDF_NAME(Ch), PDF_CH_FIELD_IS_MULTI_SELECT, 520); options(d, o);
    o = field(d, pg, annots, fields, "validated-list", PDF_NAME(Ch), PDF_CH_FIELD_IS_MULTI_SELECT, 490); options(d, o);
    js_action(d, o, PDF_NAME(V), "event.rc = false;");
    o = field(d, pg, annots, fields, "unsigned", PDF_NAME(Sig), 0, 460);
    pdf_write_options opts = pdf_default_write_options;
    pdf_save_document(ctx, d, path(dir, "forms.pdf"), &opts);
    opts.do_encrypt = PDF_ENCRYPT_AES_256; snprintf(opts.upwd_utf8, sizeof(opts.upwd_utf8), "user-secret");
    snprintf(opts.opwd_utf8, sizeof(opts.opwd_utf8), "owner-secret"); opts.permissions = ~0;
    pdf_save_document(ctx, d, path(dir, "encrypted.pdf"), &opts);
    pdf_drop_obj(ctx, pg); pdf_drop_document(ctx, d);
}
static fz_document *open_pdf(const char *dir, const char *name, const char *password)
{
    fz_document *doc = fz_open_document(ctx, path(dir, name));
    if (fz_needs_password(ctx, doc)) CHECK(fz_authenticate_password(ctx, doc, password));
    return doc;
}
static view_pdf_widget *widget(view_pdf_widgets *ws, const char *name)
{
    int radio_index = !strcmp(name, "radio.first") ? 0 : !strcmp(name, "radio.second") ? 1 : -1;
    for (int i = 0; i < ws->count; ++i) {
        if (radio_index >= 0 && !strcmp(ws->items[i].name, "radio")) { if (!radio_index--) return &ws->items[i]; }
        else if (!strcmp(ws->items[i].name, name)) return &ws->items[i];
    }
    fprintf(stderr, "missing widget %s\n", name); exit(1);
}
static void check_value(view_pdf *p, const char *name, const char *value)
{
    view_pdf_widgets ws = view_pdf_list_widgets(p, 0);
    if (strcmp(widget(&ws, name)->value, value)) fprintf(stderr, "widget %s: actual [%s], expected [%s]\n", name, widget(&ws, name)->value, value);
    CHECK(!strcmp(widget(&ws, name)->value, value)); view_pdf_drop_widgets(ctx, &ws);
}
static void forms_test(const char *dir)
{
    fz_document *doc = open_pdf(dir, "forms.pdf", NULL); view_pdf *p = view_pdf_attach(ctx, doc);
    CHECK(p && !view_pdf_get_status(p).dirty && view_pdf_get_status(p).javascript);
    view_pdf_widgets ws = view_pdf_list_widgets(p, 0); CHECK(ws.count == 9);
    view_pdf_widget *text = widget(&ws, "text"), *list = widget(&ws, "list");
    CHECK(text->required && text->max_length == 16 && widget(&ws, "readonly")->readonly);
    CHECK(list->multiselect && list->option_count == 3 && !strcmp(list->options[1].label, "Beta") && !strcmp(list->options[1].value, "b"));
    const char *value[] = {"after"}; view_pdf_set_widget(p, text->id, 1, value); check_value(p, "text", "after");
    CHECK(view_pdf_get_status(p).dirty); view_pdf_undo(p); check_value(p, "text", "before");
    CHECK(!view_pdf_get_status(p).dirty); view_pdf_redo(p); check_value(p, "text", "after");
    value[0] = "bad"; REJECT(view_pdf_set_widget(p, text->id, 1, value)); check_value(p, "text", "after");
    value[0] = "keybad"; REJECT(view_pdf_set_widget(p, text->id, 1, value)); check_value(p, "text", "after");
    value[0] = "much-longer-than-sixteen"; REJECT(view_pdf_set_widget(p, text->id, 1, value));
    value[0] = "write"; REJECT(view_pdf_set_widget(p, widget(&ws, "readonly")->id, 1, value));
    value[0] = "Yes"; view_pdf_set_widget(p, widget(&ws, "check")->id, 1, value); check_value(p, "check", "Yes");
    value[0] = "Off"; view_pdf_set_widget(p, widget(&ws, "check")->id, 1, value); check_value(p, "check", "Off");
    value[0] = "One"; view_pdf_set_widget(p, widget(&ws, "radio.first")->id, 1, value);
    value[0] = "Two"; view_pdf_set_widget(p, widget(&ws, "radio.second")->id, 1, value);
    check_value(p, "radio.first", "Two"); check_value(p, "radio.second", "Two");
    value[0] = "Off"; REJECT(view_pdf_set_widget(p, widget(&ws, "radio.second")->id, 1, value));
    value[0] = "b"; view_pdf_set_widget(p, widget(&ws, "combo")->id, 1, value); check_value(p, "combo", "b");
    value[0] = "c"; REJECT(view_pdf_set_widget(p, widget(&ws, "combo")->id, 1, value)); check_value(p, "combo", "b");
    value[0] = "Beta"; REJECT(view_pdf_set_widget(p, widget(&ws, "combo")->id, 1, value));
    const char *many[] = {"a", "c"}; view_pdf_set_widget(p, list->id, 2, many);
    REJECT(view_pdf_set_widget(p, widget(&ws, "validated-list")->id, 2, many));
    view_pdf_widgets current = view_pdf_list_widgets(p, 0);
    CHECK(widget(&current, "list")->value_count == 2 && !strcmp(widget(&current, "list")->values[1], "c"));
    view_pdf_drop_widgets(ctx, &current);
    view_pdf_save(p, path(dir, "forms-saved.pdf"), 0); CHECK(!view_pdf_get_status(p).dirty);
    view_pdf_undo(p); CHECK(view_pdf_get_status(p).dirty); view_pdf_redo(p); CHECK(!view_pdf_get_status(p).dirty);
    view_pdf_signatures sigs = view_pdf_list_signatures(p, NULL); CHECK(sigs.count == 1 && !sigs.items[0].signed_value);
    CHECK(sigs.items[0].digest_status == PDF_SIGNATURE_ERROR_NOT_SIGNED); view_pdf_drop_signatures(ctx, &sigs);
    view_pdf_drop_widgets(ctx, &ws); view_pdf_drop(p); fz_drop_document(ctx, doc);
    doc = open_pdf(dir, "forms-saved.pdf", NULL); p = view_pdf_attach(ctx, doc);
    check_value(p, "text", "after"); check_value(p, "combo", "b"); check_value(p, "radio.first", "Two");
    current = view_pdf_list_widgets(p, 0); CHECK(widget(&current, "list")->value_count == 2); view_pdf_drop_widgets(ctx, &current);
    view_pdf_drop(p); fz_drop_document(ctx, doc);
    puts("PASS forms: text, JS keystroke/validation rejection, readonly, check/radio, combo, multi-select, save/reopen/undo");
}
static view_pdf_annotation *annotation(view_pdf_annotations *list, view_pdf_id id)
{
    for (int i = 0; i < list->count; ++i) if (list->items[i].id.object == id.object) return &list->items[i];
    CHECK(0); return NULL;
}
static void annotations_test(const char *dir)
{
    fz_document *doc = open_pdf(dir, "forms.pdf", NULL); view_pdf *p = view_pdf_attach(ctx, doc);
    const enum pdf_annot_type types[] = {PDF_ANNOT_TEXT, PDF_ANNOT_HIGHLIGHT, PDF_ANNOT_UNDERLINE, PDF_ANNOT_STRIKE_OUT,
        PDF_ANNOT_SQUIGGLY, PDF_ANNOT_INK, PDF_ANNOT_LINE, PDF_ANNOT_SQUARE, PDF_ANNOT_CIRCLE, PDF_ANNOT_POLYGON,
        PDF_ANNOT_POLY_LINE, PDF_ANNOT_STAMP, PDF_ANNOT_FREE_TEXT};
    view_pdf_id ids[sizeof(types)/sizeof(types[0])];
    fz_rect r = fz_make_rect(50, 50, 150, 90);
    fz_quad quad = fz_quad_from_rect(r); fz_point points[] = {{50,50},{150,60},{120,90}}; int sizes[] = {3};
    for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); ++i) {
        view_pdf_annotation_spec s = view_pdf_default_annotation(types[i], r);
        s.contents = "Annotation sample"; s.author = "Tester"; s.opacity = 0.75;
        s.quad_count = 1; s.quads = &quad; s.vertex_count = 3; s.vertices = points;
        s.stroke_count = 1; s.stroke_sizes = sizes; s.point_count = 3; s.points = points;
        ids[i] = view_pdf_create_annotation(p, 0, &s);
    }
    view_pdf_annotations list = view_pdf_list_annotations(p, 0); CHECK(list.count == 13);
    for (int i = 0; i < list.count; ++i) {
        CHECK(list.items[i].editable && !strcmp(list.items[i].contents, "Annotation sample") && !strcmp(list.items[i].author, "Tester"));
        CHECK(fabsf(list.items[i].opacity-0.75f) < .001 && list.items[i].name[0]);
    }
    CHECK(annotation(&list, ids[1])->quad_count == 1 && annotation(&list, ids[5])->point_count == 3);
    fz_rect before = annotation(&list, ids[1])->rect;
    view_pdf_drop_annotations(ctx, &list);
    view_pdf_annotation_spec update = view_pdf_default_annotation(PDF_ANNOT_TEXT, r); update.contents = "Updated note";
    view_pdf_update_annotation(p, ids[0], VIEW_PDF_CONTENTS, &update);
    list = view_pdf_list_annotations(p, 0); CHECK(!strcmp(annotation(&list, ids[0])->contents, "Updated note")); view_pdf_drop_annotations(ctx, &list);
    for (int i = 0; i < 13; ++i) view_pdf_transform_annotation(p, ids[i], fz_make_rect(200, 100, 400, 180));
    list = view_pdf_list_annotations(p, 0); CHECK(annotation(&list, ids[1])->quads[0].ul.x > before.x0 + 100); view_pdf_drop_annotations(ctx, &list);
    view_pdf_delete_annotation(p, ids[0]); list = view_pdf_list_annotations(p, 0); CHECK(list.count == 12); view_pdf_drop_annotations(ctx, &list);
    view_pdf_undo(p); list = view_pdf_list_annotations(p, 0); CHECK(list.count == 13); view_pdf_drop_annotations(ctx, &list);
    view_pdf_redo(p); REJECT(view_pdf_delete_annotation(p, ids[0]));
    update.type = PDF_ANNOT_MOVIE; REJECT(view_pdf_create_annotation(p, 0, &update));
    int pos = view_pdf_get_status(p).position;
    update.opacity = NAN; REJECT(view_pdf_update_annotation(p, ids[1], VIEW_PDF_OPACITY, &update));
    REJECT(view_pdf_update_annotation(p, ids[1], VIEW_PDF_ICON, &update));
    CHECK(view_pdf_get_status(p).position == pos);
    view_pdf_save(p, path(dir, "annotations.pdf"), 0); CHECK(!view_pdf_get_status(p).dirty);
    view_pdf_undo(p); CHECK(view_pdf_get_status(p).dirty);
    update.contents = "Branch edit"; view_pdf_update_annotation(p, ids[0], VIEW_PDF_CONTENTS, &update); CHECK(!view_pdf_get_status(p).can_redo);
    view_pdf_undo(p); CHECK(view_pdf_get_status(p).dirty);
    view_pdf_drop(p); fz_drop_document(ctx, doc);
    doc = open_pdf(dir, "annotations.pdf", NULL); p = view_pdf_attach(ctx, doc);
    list = view_pdf_list_annotations(p, 0); CHECK(list.count == 12); view_pdf_drop_annotations(ctx, &list);
    /* A complete snapshot can be opened independently and is visibly renderable. */
    fz_page *page = fz_load_page(ctx, doc, 0); fz_pixmap *pix = fz_new_pixmap_from_page(ctx, page, fz_identity, fz_device_rgb(ctx), 0);
    CHECK(fz_pixmap_width(ctx, pix) == 612); fz_drop_pixmap(ctx, pix); fz_drop_page(ctx, page);
    view_pdf_drop(p); fz_drop_document(ctx, doc);
    puts("PASS annotations: 13 types, metadata, geometry move/resize, update/delete, undo/redo, branching, rollback, render/reopen");
}
static void encryption_test(const char *dir)
{
    fz_document *doc = open_pdf(dir, "encrypted.pdf", "owner-secret"); view_pdf *p = view_pdf_attach(ctx, doc);
    view_pdf_widgets ws = view_pdf_list_widgets(p, 0); const char *v[] = {"encrypted edit"};
    view_pdf_set_widget(p, widget(&ws, "text")->id, 1, v); view_pdf_drop_widgets(ctx, &ws);
    view_pdf_save(p, path(dir, "encrypted-saved.pdf"), 0); view_pdf_drop(p); fz_drop_document(ctx, doc);
    doc = fz_open_document(ctx, path(dir, "encrypted-saved.pdf")); CHECK(fz_needs_password(ctx, doc));
    CHECK(!fz_authenticate_password(ctx, doc, "wrong")); CHECK(fz_authenticate_password(ctx, doc, "user-secret"));
    p = view_pdf_attach(ctx, doc); check_value(p, "text", "encrypted edit"); view_pdf_drop(p); fz_drop_document(ctx, doc);
    puts("PASS encryption: AES-256 and existing passwords retained by snapshot save");
}
static void bytes(const char *filename, char *out, size_t n)
{
    FILE *f = fopen(filename, "rb"); CHECK(f); size_t got = fread(out, 1, n-1, f); out[got] = 0; fclose(f);
}
static void permissions_test(const char *dir)
{
    fz_document *doc = open_pdf(dir, "forms.pdf", NULL); view_pdf *p = view_pdf_attach(ctx, doc);
    const char *target = path(dir, "untouched.pdf"); FILE *f = fopen(target, "wb"); CHECK(f); CHECK(fwrite("ORIGINAL", 1, 8, f) == 8); fclose(f);
    CHECK(!chmod(target, 0444)); REJECT(view_pdf_save(p, target, 0));
    char content[20]; bytes(target, content, sizeof(content)); CHECK(!strcmp(content, "ORIGINAL")); CHECK(!chmod(target, 0640));
    view_pdf_save(p, target, 0); struct stat st; CHECK(!stat(target, &st) && st.st_uid == 1000 && (st.st_mode & 0777) == 0640);
    const char *newfile = path(dir, "new-private.pdf"); view_pdf_save(p, newfile, 0); CHECK(!stat(newfile, &st) && (st.st_mode & 0777) == 0600);
    char targetcopy[4096]; snprintf(targetcopy, sizeof(targetcopy), "%s", newfile);
    const char *linkpath = path(dir, "link.pdf"); CHECK(!symlink(targetcopy, linkpath)); view_pdf_save(p, linkpath, 0); CHECK(!lstat(linkpath, &st) && S_ISLNK(st.st_mode));
    REJECT(view_pdf_save(p, path(dir, "root-only/original.pdf"), 0));
    REJECT(view_pdf_save(p, "/dev/full", 0));
    CHECK(!mkdir(path(dir, "no-write"), 0500)); REJECT(view_pdf_save(p, path(dir, "no-write/file.pdf"), 0));
    /* A write failure after temp creation must never overwrite the target. */
    target = path(dir, "limited.pdf"); f = fopen(target, "wb"); CHECK(f); CHECK(fwrite("ORIGINAL", 1, 8, f) == 8); fclose(f);
    struct rlimit oldlim, lim; CHECK(!getrlimit(RLIMIT_FSIZE, &oldlim)); lim = oldlim; lim.rlim_cur = 32;
    signal(SIGXFSZ, SIG_IGN); CHECK(!setrlimit(RLIMIT_FSIZE, &lim)); REJECT(view_pdf_save(p, target, 0)); CHECK(!setrlimit(RLIMIT_FSIZE, &oldlim));
    bytes(target, content, sizeof(content)); CHECK(!strcmp(content, "ORIGINAL"));
    view_pdf_drop(p); fz_drop_document(ctx, doc);
    puts("PASS save permissions: uid1000/modes, root-owned/readonly refusal, symlink, no-write directory, mid-write failure preserves original");
}
static void signature_fixture(const char *dir)
{
    pdf_document *d = pdf_create_document(ctx); pdf_obj *pg = pdf_add_page(ctx, d, fz_make_rect(0,0,612,792), 0, NULL, NULL); pdf_insert_page(ctx, d, -1, pg);
    pdf_page *page = pdf_load_page(ctx, d, 0); pdf_annot *sig = pdf_create_signature_widget(ctx, page, "signed-fixture");
    pdf_set_annot_rect(ctx, sig, fz_make_rect(40,40,300,90));
    pdf_obj *value = pdf_new_dict(ctx, d, 8); pdf_dict_put(ctx, value, PDF_NAME(Type), PDF_NAME(Sig));
    pdf_dict_put_name(ctx, value, PDF_NAME(Filter), "Adobe.PPKLite"); pdf_dict_put_name(ctx, value, PDF_NAME(SubFilter), "adbe.pkcs7.detached");
    pdf_dict_put_text_string(ctx, value, PDF_NAME(Name), "Generated test signer"); pdf_dict_put_text_string(ctx, value, PDF_NAME(Reason), "Backend tests");
    pdf_obj *br = pdf_dict_put_array(ctx, value, PDF_NAME(ByteRange), 4);
    for (int i = 1; i <= 4; ++i) pdf_array_push_int(ctx, br, (int64_t)i * 1111111111);
    unsigned char *zero = calloc(1, 8192); CHECK(zero); pdf_dict_put_string(ctx, value, PDF_NAME(Contents), (char *)zero, 8192); free(zero);
    pdf_dict_put_drop(ctx, pdf_annot_obj(ctx, sig), PDF_NAME(V), pdf_add_object_drop(ctx, d, value));
    pdf_save_document(ctx, d, path(dir, "signature-placeholder.pdf"), NULL);
    pdf_drop_annot(ctx, sig); fz_drop_page(ctx, (fz_page *)page); pdf_drop_obj(ctx, pg); pdf_drop_document(ctx, d);
}
static void new_document_test(const char *dir)
{
    pdf_document *pdf = pdf_create_document(ctx);
    pdf_obj *pg = pdf_add_page(ctx, pdf, fz_make_rect(0,0,612,792), 90, NULL, NULL);
    pdf_insert_page(ctx, pdf, -1, pg); pdf_drop_obj(ctx, pg);
    view_pdf *p = view_pdf_attach(ctx, (fz_document *)pdf);
    view_pdf_annotation_spec s = view_pdf_default_annotation(PDF_ANNOT_SQUARE, fz_make_rect(50,50,150,100));
    view_pdf_id id = view_pdf_create_annotation(p, 0, &s);
    view_pdf_save(p, path(dir, "new-document.pdf"), 0);
    CHECK(!view_pdf_get_status(p).dirty && view_pdf_get_status(p).can_undo);
    view_pdf_undo(p);
    view_pdf_annotations list = view_pdf_list_annotations(p, 0); CHECK(list.count == 0); view_pdf_drop_annotations(ctx, &list);
    view_pdf_redo(p); CHECK(!view_pdf_get_status(p).dirty);
    s.contents = "After initial save"; view_pdf_update_annotation(p, id, VIEW_PDF_CONTENTS, &s);
    view_pdf_save(p, path(dir, "new-document.pdf"), 0);
    view_pdf_drop(p); pdf_drop_document(ctx, pdf);
    fz_document *doc = open_pdf(dir, "new-document.pdf", NULL); p = view_pdf_attach(ctx, doc);
    list = view_pdf_list_annotations(p, 0); CHECK(list.count == 1 && !strcmp(list.items[0].contents, "After initial save"));
    CHECK(fabsf(list.items[0].rect.x0-50) < .01 && fabsf(list.items[0].rect.y1-100) < .01);
    view_pdf_drop_annotations(ctx, &list); view_pdf_drop(p); fz_drop_document(ctx, doc);
    fz_buffer *buf = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)"<html><body>Not PDF</body></html>", 33);
    fz_stream *stream = fz_open_buffer(ctx, buf); doc = fz_open_document_with_stream(ctx, "text/html", stream);
    CHECK(!view_pdf_attach(ctx, doc)); fz_drop_document(ctx, doc); fz_drop_stream(ctx, stream); fz_drop_buffer(ctx, buf);
    puts("PASS new/rotated PDF: save twice, undo after save, page-space rect; non-PDF attach rejected");
}
static void signatures_test(const char *dir)
{
    fz_document *doc = open_pdf(dir, "signed.pdf", NULL); view_pdf *p = view_pdf_attach(ctx, doc);
    view_pdf_signatures sigs = view_pdf_list_signatures(p, NULL); CHECK(sigs.count == 1);
    CHECK(sigs.items[0].signed_value && sigs.items[0].id.page == 0 && !sigs.items[0].document_modified);
    CHECK(sigs.items[0].digest_status == VIEW_PDF_SIGNATURE_UNAVAILABLE && sigs.items[0].certificate_status == VIEW_PDF_SIGNATURE_UNAVAILABLE);
    CHECK(!strcmp(sigs.items[0].name, "Generated test signer") && !sigs.items[0].signatory);
    CHECK(sigs.items[0].change_status == 0); view_pdf_drop_signatures(ctx, &sigs);
    view_pdf_save(p, path(dir, "signed-copy.pdf"), 0);
    view_pdf_annotation_spec s = view_pdf_default_annotation(PDF_ANNOT_TEXT, fz_make_rect(50,50,80,80));
    s.contents = "post-signature change"; view_pdf_create_annotation(p, 0, &s);
    sigs = view_pdf_list_signatures(p, NULL); CHECK(sigs.items[0].document_modified && sigs.items[0].digest_status == VIEW_PDF_SIGNATURE_UNAVAILABLE); view_pdf_drop_signatures(ctx, &sigs);
    REJECT(view_pdf_save(p, path(dir, "must-not-exist.pdf"), 0)); CHECK(access(path(dir, "must-not-exist.pdf"), F_OK) < 0);
    view_pdf_save(p, path(dir, "signed-consented.pdf"), VIEW_PDF_ALLOW_SIGNATURE_INVALIDATION);
    view_pdf_drop(p); fz_drop_document(ctx, doc);
    puts("PASS signatures: generated CMS signature enumerated, unavailable != valid, unsaved change state, explicit save consent");
}
int main(int argc, char **argv)
{
    CHECK(argc == 3); ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED); CHECK(ctx); fz_register_document_handlers(ctx);
    int result = 0; fz_var(result);
    fz_try(ctx) {
        if (!strcmp(argv[1], "fixture")) { fixture(argv[2]); signature_fixture(argv[2]); }
        else if (!strcmp(argv[1], "root")) {
            CHECK(getuid() == 0); fz_document *doc = open_pdf(argv[2], "forms.pdf", NULL); view_pdf *p = view_pdf_attach(ctx, doc);
            REJECT(view_pdf_save(p, path(argv[2], "root-output.pdf"), 0)); view_pdf_drop(p); fz_drop_document(ctx, doc); puts("PASS root save refused");
        } else { CHECK(getuid() == 1000 && geteuid() == 1000); forms_test(argv[2]); annotations_test(argv[2]); encryption_test(argv[2]); permissions_test(argv[2]); new_document_test(argv[2]); signatures_test(argv[2]); }
    }
    fz_catch(ctx) { fprintf(stderr, "FAIL MuPDF: %s\n", fz_caught_message(ctx)); result = 1; }
    fz_drop_context(ctx); return result;
}
