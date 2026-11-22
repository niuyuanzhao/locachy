/* test-libmupdf DIR [--update-smoke]: linked with -lmupdf alone, no
 * cmark-gfm or renderer shim. In both modes the Markdown link must resolve
 * to a heading on a later page, using the id from bundled cmark-gfm's
 * autoheaderid extension. The full test also draws the first page and
 * finds its marker in Markdown, PDF and EPUB. */
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stdio.h>
#include <string.h>

static fz_context *ctx;
static int failed;

static void check(int ok, const char *what, float a, float b)
{
    printf("%s %s (%.1f, %.1f)\n", ok ? "PASS" : "FAIL", what, a, b);
    failed |= !ok;
}

static void zip_text(fz_zip_writer *zip, const char *name, const char *text, int compress)
{
    fz_buffer *b = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)text, strlen(text));
    fz_write_zip_entry(ctx, zip, name, b, compress);
    fz_drop_buffer(ctx, b);
}

static void make_markdown(const char *md)
{
    fz_buffer *b = fz_new_buffer(ctx, 4096);
    fz_append_string(ctx, b, "# First Heading\n\nGo to [the second heading](#second-heading).\n\n");
    /* At 400 x 300, em 12: the second heading in the middle of page 3 (a
     * heading at a page's foot has its box there and its text on the next). */
    for (int i = 1; i <= 16; i++)
        fz_append_printf(ctx, b, "Filler paragraph %d pushes the next heading down.\n\n", i);
    fz_append_string(ctx, b, "## Second Heading\n\nMarkdown marker okapi.\n");
    fz_save_buffer(ctx, b, md);
    fz_drop_buffer(ctx, b);
}

static void make_pdf_epub(const char *pdf, const char *epub)
{
    pdf_document *doc = pdf_create_document(ctx);
    fz_font *helv = fz_new_base14_font(ctx, "Helvetica");
    pdf_obj *res = pdf_new_dict(ctx, doc, 1);
    pdf_obj *fonts = pdf_dict_put_dict(ctx, res, PDF_NAME(Font), 1);
    pdf_obj *font = pdf_add_simple_font(ctx, doc, helv, PDF_SIMPLE_ENCODING_LATIN);
    pdf_dict_puts(ctx, fonts, "F1", font);
    const char *text = "BT /F1 24 Tf 72 700 Td (PDF marker quokka) Tj ET\n";
    fz_buffer *b = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)text, strlen(text));
    pdf_obj *page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 612, 792), 0, res, b);
    pdf_insert_page(ctx, doc, -1, page);
    pdf_write_options opts = pdf_default_write_options;
    pdf_save_document(ctx, doc, pdf, &opts);
    pdf_drop_obj(ctx, page);
    fz_drop_buffer(ctx, b);
    pdf_drop_obj(ctx, font);
    pdf_drop_obj(ctx, res);
    fz_drop_font(ctx, helv);
    pdf_drop_document(ctx, doc);

    fz_zip_writer *zip = fz_new_zip_writer(ctx, epub);
    zip_text(zip, "mimetype", "application/epub+zip", 0);
    zip_text(zip, "META-INF/container.xml",
             "<?xml version=\"1.0\"?>\n<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
             "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n", 1);
    zip_text(zip, "OEBPS/content.opf",
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"id\">"
             "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">libmupdf-test</dc:identifier>"
             "<dc:title>Test</dc:title><dc:language>en</dc:language></metadata>"
             "<manifest><item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
             "<spine><itemref idref=\"ch1\"/></spine></package>\n", 1);
    zip_text(zip, "OEBPS/ch1.xhtml",
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
             "<head><title>One</title></head><body><h1>One</h1><p>EPUB marker zebra</p></body></html>\n", 1);
    fz_close_zip_writer(ctx, zip);
    fz_drop_zip_writer(ctx, zip);
}

/* With a marker word, also draw the first page and find its marker. */
static void test(const char *path, const char *word, int markdown)
{
    fz_document *doc = NULL;
    fz_page *page = NULL, *target = NULL;
    fz_pixmap *pix = NULL;
    fz_link *links = NULL;
    fz_var(doc);
    fz_var(page);
    fz_var(target);
    fz_var(pix);
    fz_var(links);
    printf("== %s\n", path);
    fz_try(ctx) {
        doc = fz_open_document(ctx, path);
        fz_layout_document(ctx, doc, 400, 300, 12);
        if (word) {
            int pages = fz_count_pages(ctx, doc);
            pix = fz_new_pixmap_from_page_number(ctx, doc, 0, fz_identity, fz_device_rgb(ctx), 0);
            page = fz_load_page(ctx, doc, 0);
            fz_quad q;
            int mark, hits = fz_search_page(ctx, page, word, &mark, &q, 1);
            check(pages >= 1 && fz_pixmap_width(ctx, pix) > 0 && hits == 1, "opens, draws page 1 and finds its marker",
                  (float)pages, (float)hits);
        }
        if (markdown) {
            if (!page)
                page = fz_load_page(ctx, doc, 0);
            fz_link *link = links = fz_load_links(ctx, page);
            while (link && strcmp(link->uri, "#second-heading"))
                link = link->next;
            check(link != NULL, "page 1 links to #second-heading", 0, 0);
            float x = 0, y = 0;
            fz_location loc = fz_resolve_link(ctx, doc, link ? link->uri : "#second-heading", &x, &y);
            int number = fz_page_number_from_location(ctx, doc, loc);
            check(number > 0, "the link resolves to a later page (page index, y)", (float)number, y);
            if (number > 0) {
                target = fz_load_page(ctx, doc, number);
                fz_quad q;
                int mark, hits = fz_search_page(ctx, target, "Second Heading", &mark, &q, 1);
                /* The anchor is the heading's box, above the heading's text. */
                check(hits == 1 && y < q.ul.y, "it resolves to the heading's page, above its text (anchor y, text y)",
                      y, hits == 1 ? q.ul.y : -1);
            }
        }
    } fz_always(ctx) {
        fz_drop_link(ctx, links);
        fz_drop_page(ctx, target);
        fz_drop_page(ctx, page);
        fz_drop_pixmap(ctx, pix);
        fz_drop_document(ctx, doc);
    } fz_catch(ctx) {
        check(0, fz_caught_message(ctx), 0, 0);
    }
}

int main(int argc, char **argv)
{
    int smoke = argc == 3 && strcmp(argv[2], "--update-smoke") == 0;
    if (argc != 2 && !smoke) {
        fprintf(stderr, "usage: test-libmupdf DIR [--update-smoke]\n");
        return 2;
    }
    char md[4096], pdf[4096], epub[4096];
    snprintf(md, sizeof md, "%s/anchors.md", argv[1]);
    snprintf(pdf, sizeof pdf, "%s/sample.pdf", argv[1]);
    snprintf(epub, sizeof epub, "%s/sample.epub", argv[1]);
    ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx)
        return 1;
    fz_register_document_handlers(ctx);
    fz_try(ctx) {
        make_markdown(md);
        if (!smoke)
            make_pdf_epub(pdf, epub);
    } fz_catch(ctx) {
        fprintf(stderr, "test-libmupdf: cannot write samples: %s\n", fz_caught_message(ctx));
        fz_drop_context(ctx);
        return 1;
    }
    test(md, smoke ? NULL : "First Heading", 1);
    if (!smoke) {
        test(pdf, "quokka", 0);
        test(epub, "zebra", 0);
    }
    fz_drop_context(ctx);
    if (failed)
        puts("FAIL");
    else if (smoke)
        puts("PASS libmupdf opens Markdown with heading anchors");
    else
        puts("PASS libmupdf opens Markdown with heading anchors, PDF and EPUB");
    return failed;
}
