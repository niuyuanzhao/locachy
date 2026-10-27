/*
 * make-view-samples OUTDIR FONT.ttf: one sample document per format view-
 * render reads, made with the libraries in the build root (MuPDF, libtiff,
 * libwebp, libheif, libjxl, libarchive, iconv) and by hand where the format
 * is simple (MOBI, FB2, XPS, BMP, PostScript, DVI). make-view-samples.sh adds
 * the DjVu ones with djvulibre's tools from the PBMs and texts written here.
 * Known words and places are listed in test-view-render.c.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <gif_lib.h>
#include <iconv.h>
#include <jxl/encode.h>
#include <libheif/heif.h>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <tiffio.h>
#include <webp/encode.h>

static fz_context *ctx;
static const char *out;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("make-view-samples: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static const char *at(const char *name)
{
    static char path[8][4096];
    static int i;
    i = (i + 1) % 8;
    snprintf(path[i], sizeof path[i], "%s/%s", out, name);
    return path[i];
}

static void write_file(const char *name, const void *data, size_t size)
{
    FILE *f = fopen(at(name), "wb");
    if (!f || fwrite(data, 1, size, f) != size || fclose(f) != 0) die("cannot write %s", name);
}

static void write_text(const char *name, const char *text)
{
    write_file(name, text, strlen(text));
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot read %s", path);
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(*size + 1);
    if (!data || fread(data, 1, *size, f) != *size) die("cannot read %s", path);
    fclose(f);
    return data;
}

/* An RGB image: white, a block of color, a black square in its top left. */
static unsigned char *picture(int w, int h, unsigned color)
{
    unsigned char *p = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char *q = p + ((size_t)y * w + x) * 3;
            unsigned c = x < w / 4 && y < h / 4 ? 0x000000 : x > w / 3 && y > h / 3 ? color : 0xFFFFFF;
            q[0] = c >> 16, q[1] = c >> 8, q[2] = c;
        }
    }
    return p;
}

static fz_pixmap *picture_pixmap(int w, int h, unsigned color)
{
    unsigned char *p = picture(w, h, color);
    fz_pixmap *pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), w, h, NULL, 0);
    for (int y = 0; y < h; y++) memcpy(pix->samples + (size_t)y * pix->stride, p + (size_t)y * w * 3, (size_t)w * 3);
    pix->xres = pix->yres = 72;
    free(p);
    return pix;
}

static void png(const char *name, int w, int h, unsigned color)
{
    fz_pixmap *pix = picture_pixmap(w, h, color);
    fz_save_pixmap_as_png(ctx, pix, at(name));
    fz_drop_pixmap(ctx, pix);
}

static void jpeg(const char *name, int w, int h, unsigned color)
{
    fz_pixmap *pix = picture_pixmap(w, h, color);
    fz_save_pixmap_as_jpeg(ctx, pix, at(name), 90);
    fz_drop_pixmap(ctx, pix);
}

static void pnm_psd(const char *name, int w, int h, unsigned color)
{
    fz_pixmap *pix = picture_pixmap(w, h, color);
    if (strstr(name, ".psd")) fz_save_pixmap_as_psd(ctx, pix, at(name));
    else fz_save_pixmap_as_pnm(ctx, pix, at(name));
    fz_drop_pixmap(ctx, pix);
}

static void gif(const char *name, int w, int h, unsigned color)
{
    int err;
    GifFileType *g = EGifOpenFileName(at(name), false, &err);
    GifColorType colors[4] = { { 255, 255, 255 }, { 0, 0, 0 }, { color >> 16, color >> 8 & 0xFF, color & 0xFF }, { 0, 0, 0 } };
    ColorMapObject *map = GifMakeMapObject(4, colors);
    if (!g || !map || EGifPutScreenDesc(g, w, h, 2, 0, map) != GIF_OK || EGifPutImageDesc(g, 0, 0, w, h, false, NULL) != GIF_OK) die("cannot write %s", name);
    GifPixelType *line = malloc((size_t)w);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) line[x] = x < w / 4 && y < h / 4 ? 1 : x > w / 3 && y > h / 3 ? 2 : 0;
        if (EGifPutLine(g, line, w) != GIF_OK) die("cannot write %s", name);
    }
    EGifCloseFile(g, &err);
    GifFreeMapObject(map);
    free(line);
}

/* 24-bit BMP, bottom-up, 72 dpi. */
static void bmp(const char *name, int w, int h, unsigned color)
{
    unsigned char *p = picture(w, h, color);
    int row = (w * 3 + 3) & ~3;
    size_t size = 54 + (size_t)row * h;
    unsigned char *b = calloc(1, size);
    unsigned v[] = { (unsigned)size, 0, 54, 40, (unsigned)w, (unsigned)h };
    b[0] = 'B', b[1] = 'M';
    for (int i = 0; i < 6; i++) for (int k = 0; k < 4; k++) b[2 + i * 4 + k] = (unsigned char)(v[i] >> (8 * k));
    b[26] = 1, b[28] = 24;
    for (int k = 0; k < 4; k++) b[38 + k] = b[42 + k] = (unsigned char)(2835u >> (8 * k));
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const unsigned char *q = p + ((size_t)(h - 1 - y) * w + x) * 3;
            unsigned char *d = b + 54 + (size_t)y * row + x * 3;
            d[0] = q[2], d[1] = q[1], d[2] = q[0];
        }
    }
    write_file(name, b, size);
    free(b);
    free(p);
}

static void put16(unsigned char *p, unsigned v) { p[0] = v >> 8, p[1] = v; }
static void put32(unsigned char *p, unsigned v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
static void le32(unsigned char *p, unsigned v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

/* An icon of one s x s image, 32-bit BGRA (the BMP form inside ICO). */
static void ico(const char *name, int s, unsigned color)
{
    unsigned char *p = picture(s, s, color);
    size_t mask_row = (size_t)(s + 31) / 32 * 4, dib = 40 + (size_t)s * s * 4 + mask_row * s, size = 22 + dib;
    unsigned char *b = calloc(1, size), *h = b + 22;
    b[2] = 1, b[4] = 1;                          /* an icon, of one image */
    b[6] = (unsigned char)s, b[7] = (unsigned char)s, b[10] = 1, b[12] = 32;
    le32(b + 14, (unsigned)dib);
    le32(b + 18, 22);
    le32(h, 40);
    le32(h + 4, (unsigned)s);
    le32(h + 8, 2 * (unsigned)s);                /* the image and its mask */
    h[12] = 1, h[14] = 32;
    le32(h + 20, (unsigned)(s * s * 4));
    for (int y = 0; y < s; y++) {
        for (int x = 0; x < s; x++) {
            const unsigned char *q = p + ((size_t)(s - 1 - y) * s + x) * 3;
            unsigned char *d = h + 40 + ((size_t)y * s + x) * 4;
            d[0] = q[2], d[1] = q[1], d[2] = q[0], d[3] = 255;
        }
    }
    write_file(name, b, size);
    free(b);
    free(p);
}

/* A GIMP XCF of one s x s RGBA layer (s <= 64: one tile), uncompressed. */
static void xcf(const char *name, int s, unsigned color)
{
    static unsigned char b[1024 + 3 * 64 * 64 * 4];
    unsigned char *p = picture(s, s, color);
    size_t n = 0;
#define U32(v) (put32(b + n, (unsigned)(v)), n += 4)
    memcpy(b, "gimp xcf file", 14), n = 14;
    U32(s), U32(s), U32(0);                      /* RGB */
    U32(17), U32(1), b[n++] = 0;                 /* PROP_COMPRESSION: none */
    U32(0), U32(0);                              /* PROP_END */
    size_t layer_at = n;
    U32(0), U32(0);                              /* the layer's offset, then no more */
    U32(0);                                      /* no channels */
    put32(b + layer_at, (unsigned)n);
    U32(s), U32(s), U32(1);                      /* RGBA */
    U32(6), memcpy(b + n, "Layer", 6), n += 6;
    U32(6), U32(4), U32(255);                    /* PROP_OPACITY */
    U32(8), U32(4), U32(1);                      /* PROP_VISIBLE */
    U32(15), U32(8), U32(0), U32(0);             /* PROP_OFFSETS */
    U32(0), U32(0);
    U32(n + 8), U32(0);                          /* its hierarchy, no mask */
    U32(s), U32(s), U32(4);
    U32(n + 8), U32(0);                          /* one level */
    U32(s), U32(s);
    U32(n + 8), U32(0);                          /* one tile */
    for (int i = 0; i < s * s; i++) {
        memcpy(b + n, p + (size_t)i * 3, 3), b[n + 3] = 255;
        n += 4;
    }
#undef U32
    /* GraphicsMagick reads a last tile as up to 1.5 times its size. */
    n += (size_t)s * s * 4;
    write_file(name, b, n);
    free(p);
}

static void webp(const char *name, int w, int h, unsigned color)
{
    unsigned char *p = picture(w, h, color), *data;
    size_t n = WebPEncodeLosslessRGB(p, w, h, w * 3, &data);
    if (!n) die("WebP encoding failed");
    write_file(name, data, n);
    WebPFree(data);
    free(p);
}

static void heif(const char *name, int w, int h, unsigned color, enum heif_compression_format codec)
{
    struct heif_context *hc = heif_context_alloc();
    struct heif_encoder *enc;
    struct heif_image *img;
    struct heif_error e = heif_context_get_encoder_for_format(hc, codec, &enc);
    if (e.code) die("%s: no encoder: %s", name, e.message);
    heif_encoder_set_lossy_quality(enc, 90);
    if ((e = heif_image_create(w, h, heif_colorspace_RGB, heif_chroma_interleaved_RGB, &img)).code ||
        (e = heif_image_add_plane(img, heif_channel_interleaved, w, h, 8)).code) {
        die("%s: %s", name, e.message);
    }
    size_t stride;
    uint8_t *dst = heif_image_get_plane2(img, heif_channel_interleaved, &stride);
    unsigned char *p = picture(w, h, color);
    for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * stride, p + (size_t)y * w * 3, (size_t)w * 3);
    if ((e = heif_context_encode_image(hc, img, enc, NULL, NULL)).code || (e = heif_context_write_to_file(hc, at(name))).code) {
        die("%s: %s", name, e.message);
    }
    free(p);
    heif_image_release(img);
    heif_encoder_release(enc);
    heif_context_free(hc);
}

static void jxl(const char *name, int w, int h, unsigned color)
{
    JxlEncoder *e = JxlEncoderCreate(NULL);
    JxlBasicInfo bi;
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = (uint32_t)w;
    bi.ysize = (uint32_t)h;
    bi.bits_per_sample = 8;
    bi.num_color_channels = 3;
    bi.uses_original_profile = JXL_TRUE;
    JxlColorEncoding ce;
    JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
    JxlPixelFormat pf = { 3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
    unsigned char *p = picture(w, h, color);
    JxlEncoderFrameSettings *fs;
    if (JxlEncoderSetBasicInfo(e, &bi) != JXL_ENC_SUCCESS || JxlEncoderSetColorEncoding(e, &ce) != JXL_ENC_SUCCESS ||
        !(fs = JxlEncoderFrameSettingsCreate(e, NULL)) || JxlEncoderSetFrameLossless(fs, JXL_TRUE) != JXL_ENC_SUCCESS ||
        JxlEncoderAddImageFrame(fs, &pf, p, (size_t)w * h * 3) != JXL_ENC_SUCCESS) {
        die("JPEG XL encoding failed");
    }
    JxlEncoderCloseInput(e);
    size_t cap = 65536, used = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        uint8_t *next = buf + used;
        size_t avail = cap - used;
        JxlEncoderStatus s = JxlEncoderProcessOutput(e, &next, &avail);
        used = (size_t)(next - buf);
        if (s == JXL_ENC_SUCCESS) break;
        if (s != JXL_ENC_NEED_MORE_OUTPUT) die("JPEG XL encoding failed");
        buf = realloc(buf, cap *= 2);
    }
    write_file(name, buf, used);
    free(buf);
    free(p);
    JxlEncoderDestroy(e);
}

/* Two pages of 200x100 and 100x200. */
static void tiff(const char *name)
{
    TIFF *t = TIFFOpen(at(name), "w");
    if (!t) die("cannot write %s", name);
    for (int pg = 0; pg < 2; pg++) {
        int w = pg ? 100 : 200, h = pg ? 200 : 100;
        unsigned char *p = picture(w, h, pg ? 0x00A000 : 0x0000C0);
        TIFFSetField(t, TIFFTAG_IMAGEWIDTH, w);
        TIFFSetField(t, TIFFTAG_IMAGELENGTH, h);
        TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 3);
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
        TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, h);
        TIFFSetField(t, TIFFTAG_XRESOLUTION, 72.0);
        TIFFSetField(t, TIFFTAG_YRESOLUTION, 72.0);
        TIFFSetField(t, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
        TIFFSetField(t, TIFFTAG_SUBFILETYPE, FILETYPE_PAGE);
        TIFFSetField(t, TIFFTAG_PAGENUMBER, pg, 2);
        for (int y = 0; y < h; y++) TIFFWriteScanline(t, p + (size_t)y * w * 3, (uint32_t)y, 0);
        TIFFWriteDirectory(t);
        free(p);
    }
    TIFFClose(t);
}

/* A raw G3 (one-dimensional) or G4 fax of 1728 x 100: libtiff encodes it,
 * the strip is the file. */
static unsigned char *fax(int g4, size_t *size)
{
    const char *tmp = at("fax.tif");
    TIFF *t = TIFFOpen(tmp, "w");
    const int w = 1728, h = 100;
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, w);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, h);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 1);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE);
    TIFFSetField(t, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
    TIFFSetField(t, TIFFTAG_COMPRESSION, g4 ? COMPRESSION_CCITTFAX4 : COMPRESSION_CCITTFAX3);
    if (!g4) TIFFSetField(t, TIFFTAG_GROUP3OPTIONS, 0);
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, h);
    unsigned char row[1728 / 8];
    for (int y = 0; y < h; y++) {
        memset(row, 0, sizeof row);
        /* MINISWHITE: 1 is black. A bar across rows 20-59, columns 200-999. */
        for (int x = 0; x < w; x++) if (y >= 20 && y < 60 && x >= 200 && x < 1000) row[x / 8] |= 0x80 >> (x % 8);
        TIFFWriteScanline(t, row, (uint32_t)y, 0);
    }
    TIFFClose(t);
    t = TIFFOpen(tmp, "r");
    tmsize_t n = TIFFRawStripSize(t, 0);
    unsigned char *data = malloc((size_t)n);
    if (n <= 0 || TIFFReadRawStrip(t, 0, data, n) != n) die("cannot read the fax strip");
    TIFFClose(t);
    remove(tmp);
    *size = (size_t)n;
    return data;
}

struct entry {
    const char *name;
    const void *data;
    size_t size;
};

static void archive(const char *name, int format, const struct entry *e, int n)
{
    struct archive *a = archive_write_new();
    if (archive_write_set_format(a, format) != ARCHIVE_OK || archive_write_open_filename(a, at(name)) != ARCHIVE_OK) {
        die("%s: %s", name, archive_error_string(a));
    }
    for (int i = 0; i < n; i++) {
        struct archive_entry *ae = archive_entry_new();
        archive_entry_set_pathname(ae, e[i].name);
        archive_entry_set_size(ae, (la_int64_t)e[i].size);
        archive_entry_set_filetype(ae, AE_IFREG);
        archive_entry_set_perm(ae, 0644);
        if (archive_write_header(a, ae) != ARCHIVE_OK || archive_write_data(a, e[i].data, e[i].size) != (la_ssize_t)e[i].size) {
            die("%s: %s", name, archive_error_string(a));
        }
        archive_entry_free(ae);
    }
    if (archive_write_close(a) != ARCHIVE_OK) die("%s: %s", name, archive_error_string(a));
    archive_write_free(a);
}

/* gzip or bzip2 of a file already written. */
static void packed(const char *name, const char *from, int filter)
{
    size_t size;
    unsigned char *data = read_file(at(from), &size);
    struct archive *a = archive_write_new();
    archive_write_add_filter(a, filter);
    archive_write_set_format_raw(a);
    if (archive_write_open_filename(a, at(name)) != ARCHIVE_OK) die("%s: %s", name, archive_error_string(a));
    struct archive_entry *ae = archive_entry_new();
    archive_entry_set_pathname(ae, from);
    archive_entry_set_size(ae, (la_int64_t)size);
    archive_entry_set_filetype(ae, AE_IFREG);
    if (archive_write_header(a, ae) != ARCHIVE_OK || archive_write_data(a, data, size) != (la_ssize_t)size) {
        die("%s: %s", name, archive_error_string(a));
    }
    archive_entry_free(ae);
    archive_write_close(a);
    archive_write_free(a);
    free(data);
}

/* sample.pdf: two Letter pages in Helvetica (not embedded); page 1 has a
 * link to page 2 (72,100 there) and one to https://example.org/, and a red
 * square; an outline (Chapter One > Section 1.1, Chapter Two); notes.txt
 * attached. locked.pdf: the same, AES-256, user password "secret". */
static void pdf(void)
{
    pdf_document *doc = pdf_create_document(ctx);
    fz_font *helv = fz_new_base14_font(ctx, "Helvetica");
    pdf_obj *font = pdf_add_simple_font(ctx, doc, helv, PDF_SIMPLE_ENCODING_LATIN);
    pdf_obj *res = pdf_new_dict(ctx, doc, 1);
    pdf_obj *fonts = pdf_dict_put_dict(ctx, res, PDF_NAME(Font), 1);
    pdf_dict_puts(ctx, fonts, "F1", font);
    const char *pages[2] = {
        "BT /F1 24 Tf 72 720 Td (Hello LOCachy viewer) Tj ET\n"
        "BT /F1 18 Tf 72 650 Td (Search target: quokka) Tj ET\n"
        "BT /F1 14 Tf 72 500 Td (Go to page two) Tj ET\n"
        "BT /F1 14 Tf 72 450 Td (Visit example.org) Tj ET\n"
        "1 0 0 rg 400 600 100 100 re f\n",
        "BT /F1 24 Tf 72 720 Td (Second page marker) Tj ET\n"
        "BT /F1 18 Tf 72 600 Td (quokka again) Tj ET\n",
    };
    for (int i = 0; i < 2; i++) {
        fz_buffer *c = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)pages[i], strlen(pages[i]));
        pdf_obj *p = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 612, 792), 0, res, c);
        pdf_insert_page(ctx, doc, -1, p);
        pdf_drop_obj(ctx, p);
        fz_drop_buffer(ctx, c);
    }
    fz_document *d = &doc->super;
    fz_page *pg = fz_load_page(ctx, d, 0);
    char *uri = fz_format_link_uri(ctx, d, fz_make_link_dest_xyz(0, 1, 72, 100, 0));
    fz_drop_link(ctx, fz_create_link(ctx, pg, fz_make_rect(72, 278, 250, 296), uri));
    fz_drop_link(ctx, fz_create_link(ctx, pg, fz_make_rect(72, 328, 250, 346), "https://example.org/"));
    fz_free(ctx, uri);
    fz_drop_page(ctx, pg);

    char *u1 = fz_format_link_uri(ctx, d, fz_make_link_dest_xyz(0, 0, 0, 0, 0));
    char *u11 = fz_format_link_uri(ctx, d, fz_make_link_dest_xyz(0, 0, 72, 130, 0));
    char *u2 = fz_format_link_uri(ctx, d, fz_make_link_dest_xyz(0, 1, 0, 0, 0));
    fz_outline_iterator *it = fz_new_outline_iterator(ctx, d);
    fz_outline_item one = { .title = "Chapter One", .uri = u1, .is_open = 1 };
    fz_outline_item sub = { .title = "Section 1.1", .uri = u11 }, two = { .title = "Chapter Two", .uri = u2 };
    fz_outline_iterator_insert(ctx, it, &one);
    fz_outline_iterator_insert(ctx, it, &two);
    fz_outline_iterator_prev(ctx, it);
    fz_outline_iterator_prev(ctx, it);
    fz_outline_iterator_down(ctx, it);
    fz_outline_iterator_insert(ctx, it, &sub);
    fz_drop_outline_iterator(ctx, it);
    fz_free(ctx, u1);
    fz_free(ctx, u11);
    fz_free(ctx, u2);

    fz_set_metadata(ctx, d, FZ_META_INFO_TITLE, "LOCachy Sample");
    fz_set_metadata(ctx, d, FZ_META_INFO_AUTHOR, "View Tester");
    fz_buffer *notes = fz_new_buffer_from_copied_data(ctx, (const unsigned char *)"attached notes\n", 15);
    pdf_obj *fs = pdf_add_embedded_file(ctx, doc, "notes.txt", "text/plain", notes, -1, -1, 0);
    pdf_obj *root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
    pdf_obj *names = pdf_dict_put_dict(ctx, root, PDF_NAME(Names), 1);
    pdf_obj *ef = pdf_dict_put_dict(ctx, names, PDF_NAME(EmbeddedFiles), 1);
    pdf_obj *arr = pdf_dict_put_array(ctx, ef, PDF_NAME(Names), 2);
    pdf_array_push_text_string(ctx, arr, "notes.txt");
    pdf_array_push(ctx, arr, fs);
    pdf_drop_obj(ctx, fs);
    fz_drop_buffer(ctx, notes);

    pdf_write_options opts = pdf_default_write_options;
    pdf_save_document(ctx, doc, at("sample.pdf"), &opts);
    opts.do_encrypt = PDF_ENCRYPT_AES_256;
    strcpy(opts.upwd_utf8, "secret");
    strcpy(opts.opwd_utf8, "owner");
    pdf_save_document(ctx, doc, at("locked.pdf"), &opts);
    pdf_drop_obj(ctx, res);
    fz_drop_font(ctx, helv);
    pdf_drop_document(ctx, doc);
}

/* heavy.pdf: one page of 400000 small squares, slow enough to draw that a
 * cancel arrives while it is drawn. */
static void heavy_pdf(void)
{
    pdf_document *doc = pdf_create_document(ctx);
    fz_buffer *c = fz_new_buffer(ctx, 8 << 20);
    for (int i = 0; i < 400000; i++) fz_append_printf(ctx, c, "%d %d 1 1 re f\n", i % 600, (i / 600) % 780);
    pdf_obj *res = pdf_new_dict(ctx, doc, 1);
    pdf_obj *p = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 612, 792), 0, res, c);
    pdf_insert_page(ctx, doc, -1, p);
    pdf_write_options opts = pdf_default_write_options;
    opts.do_compress = 1;
    pdf_save_document(ctx, doc, at("heavy.pdf"), &opts);
    pdf_drop_obj(ctx, p);
    pdf_drop_obj(ctx, res);
    fz_drop_buffer(ctx, c);
    pdf_drop_document(ctx, doc);
}

/* A page of sample.pdf at 150 dpi as a PBM, for cjb2. */
static void pbm(const char *name, int page)
{
    fz_document *d = fz_open_document(ctx, at("sample.pdf"));
    fz_pixmap *pix = fz_new_pixmap_from_page_number(ctx, d, page, fz_scale(150 / 72.0f, 150 / 72.0f), fz_device_gray(ctx), 0);
    FILE *f = fopen(at(name), "wb");
    fprintf(f, "P4\n%d %d\n", pix->w, pix->h);
    for (int y = 0; y < pix->h; y++) {
        for (int x = 0; x < pix->w; x += 8) {
            unsigned char b = 0;
            for (int k = 0; k < 8 && x + k < pix->w; k++) if (pix->samples[(size_t)y * pix->stride + x + k] < 128) b |= 0x80 >> k;
            fputc(b, f);
        }
    }
    fclose(f);
    fz_drop_pixmap(ctx, pix);
    fz_drop_document(ctx, d);
}

static const char *filler(void)
{
    static char text[16384];
    size_t n = 0;
    for (int i = 1; i <= 40; i++) {
        n += (size_t)snprintf(text + n, sizeof text - n,
                              "<p>Paragraph %d of the first chapter, with enough words in it to fill a line or two of a page.</p>\n", i);
    }
    return text;
}

/* sample.epub: two chapters (EPUB 3 nav and EPUB 2 NCX); chapter one has
 * "zebra", Chinese text, a link to chapter two and forty paragraphs; chapter
 * two starts with "kiwi". */
static void epub(void)
{
    static char ch1[20000];
    snprintf(ch1, sizeof ch1,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Chapter One</title></head><body>\n"
             "<h1>Chapter One</h1>\n<p>EPUB marker zebra</p>\n<p>\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95 \xE7\xAB\xA0\xE8\x8A\x82</p>\n"
             "<p><a href=\"ch2.xhtml#two\">Go to chapter two</a></p>\n%s</body></html>\n", filler());
    const char *ch2 =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Chapter Two</title></head><body>\n"
        "<h1 id=\"two\">Chapter Two</h1>\n<p>kiwi starts the second chapter.</p>\n</body></html>\n";
    const char *container =
        "<?xml version=\"1.0\"?>\n<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n";
    const char *opf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">\n"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">locachy-sample</dc:identifier>"
        "<dc:title>Sample EPUB</dc:title><dc:creator>View Tester</dc:creator><dc:language>en</dc:language></metadata>\n"
        "<manifest><item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
        "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
        "<item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"ch2\" href=\"ch2.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>\n"
        "<spine toc=\"ncx\"><itemref idref=\"ch1\"/><itemref idref=\"ch2\"/></spine></package>\n";
    const char *nav =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>Contents</title></head>"
        "<body><nav epub:type=\"toc\"><ol><li><a href=\"ch1.xhtml\">Chapter One</a></li><li><a href=\"ch2.xhtml#two\">Chapter Two</a></li></ol></nav></body></html>\n";
    const char *ncx =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><head/><docTitle><text>Sample EPUB</text></docTitle><navMap>"
        "<navPoint id=\"n1\" playOrder=\"1\"><navLabel><text>Chapter One</text></navLabel><content src=\"ch1.xhtml\"/></navPoint>"
        "<navPoint id=\"n2\" playOrder=\"2\"><navLabel><text>Chapter Two</text></navLabel><content src=\"ch2.xhtml#two\"/></navPoint>"
        "</navMap></ncx>\n";
    struct entry e[] = {
        { "mimetype", "application/epub+zip", 20 },
        { "META-INF/container.xml", container, strlen(container) },
        { "OEBPS/content.opf", opf, strlen(opf) },
        { "OEBPS/nav.xhtml", nav, strlen(nav) },
        { "OEBPS/toc.ncx", ncx, strlen(ncx) },
        { "OEBPS/ch1.xhtml", ch1, strlen(ch1) },
        { "OEBPS/ch2.xhtml", ch2, strlen(ch2) },
    };
    archive("sample.epub", ARCHIVE_FORMAT_ZIP, e, (int)(sizeof e / sizeof *e));
}

/* sample.xps: one page of 816 x 1056 (1/96 inch: Letter), a blue box and
 * "XPS marker penguin" in the given TrueType font. */
static void xps(const char *font, const char *name, int open_xps)
{
    size_t fsize;
    unsigned char *fdata = read_file(font, &fsize);
    const char *types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"fdseq\" ContentType=\"application/vnd.ms-package.xps-fixeddocumentsequence+xml\"/>"
        "<Default Extension=\"fdoc\" ContentType=\"application/vnd.ms-package.xps-fixeddocument+xml\"/>"
        "<Default Extension=\"fpage\" ContentType=\"application/vnd.ms-package.xps-fixedpage+xml\"/>"
        "<Default Extension=\"ttf\" ContentType=\"application/vnd.ms-opentype\"/></Types>\n";
    /* XPS's namespace, or OpenXPS's (ECMA-388). */
    const char *ns = open_xps ? "http://schemas.openxps.org/oxps/v1.0" : "http://schemas.microsoft.com/xps/2005/06";
    char rels[512], seq[256], fdoc[256], page[768];
    snprintf(rels, sizeof rels,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
             "<Relationship Type=\"%s/fixedrepresentation\" Target=\"/FixedDocSeq.fdseq\" Id=\"R0\"/></Relationships>\n", ns);
    snprintf(seq, sizeof seq, "<FixedDocumentSequence xmlns=\"%s\"><DocumentReference Source=\"/Documents/1/FixedDoc.fdoc\"/></FixedDocumentSequence>\n", ns);
    snprintf(fdoc, sizeof fdoc, "<FixedDocument xmlns=\"%s\"><PageContent Source=\"/Documents/1/Pages/1.fpage\"/></FixedDocument>\n", ns);
    snprintf(page, sizeof page,
             "<FixedPage xmlns=\"%s\" Width=\"816\" Height=\"1056\" xml:lang=\"en\">\n"
             "<Path Data=\"M 96,96 L 400,96 L 400,200 L 96,200 Z\" Fill=\"#FF0000FF\"/>\n"
             "<Glyphs FontUri=\"/Resources/font.ttf\" FontRenderingEmSize=\"24\" OriginX=\"96\" OriginY=\"300\" UnicodeString=\"XPS marker penguin\" Fill=\"#FF000000\"/>\n"
             "</FixedPage>\n", ns);
    struct entry e[] = {
        { "[Content_Types].xml", types, strlen(types) },
        { "_rels/.rels", rels, strlen(rels) },
        { "FixedDocSeq.fdseq", seq, strlen(seq) },
        { "Documents/1/FixedDoc.fdoc", fdoc, strlen(fdoc) },
        { "Documents/1/Pages/1.fpage", page, strlen(page) },
        { "Resources/font.ttf", fdata, fsize },
    };
    archive(name, ARCHIVE_FORMAT_ZIP, e, (int)(sizeof e / sizeof *e));
    free(fdata);
}


/* sample.mobi: a PalmDB "BOOKMOBI" with record 0 the PalmDOC and MOBI
 * headers (no compression, UTF-8) and record 1 the HTML. */
static void mobi(void)
{
    const char *html = "<html><head><title>MOBI Sample</title></head><body><h1>MOBI Sample</h1>"
                       "<p>Mobipocket marker otter</p><p>\xE4\xB8\xAD\xE6\x96\x87</p></body></html>";
    size_t hl = strlen(html);
    unsigned char db[4096] = { 0 };
    const unsigned list = 78, rec0 = list + 2 * 8 + 2, rec1 = rec0 + 32;
    memcpy(db, "MOBI Sample", 11);
    memcpy(db + 60, "BOOKMOBI", 8);
    put16(db + 76, 2);
    put32(db + list, rec0);
    put32(db + list + 4, 0);
    put32(db + list + 8, rec1);
    put32(db + list + 12, 1);
    unsigned char *r0 = db + rec0;
    put16(r0, 1);                     /* no compression */
    put32(r0 + 4, (unsigned)hl);      /* text length */
    put16(r0 + 8, 1);                 /* text records */
    put16(r0 + 10, 4096);
    memcpy(r0 + 16, "MOBI", 4);
    put32(r0 + 20, 16);               /* header length */
    put32(r0 + 24, 2);                /* a book */
    put32(r0 + 28, 65001);            /* UTF-8 */
    memcpy(db + rec1, html, hl);
    write_file("sample.mobi", db, rec1 + hl);
}

/* A DVI file of one empty page: what TeX writes, the least of it. */
static void dvi(void)
{
    unsigned char d[100];
    size_t n = 0;
    d[n++] = 247, d[n++] = 2;
    put32(d + n, 25400000), n += 4;
    put32(d + n, 473628672), n += 4;
    put32(d + n, 1000), n += 4;
    d[n++] = 0;
    size_t bop = n;
    d[n++] = 139;
    for (int i = 0; i < 10; i++) put32(d + n, i == 0), n += 4;
    put32(d + n, 0xFFFFFFFFu), n += 4;
    d[n++] = 140;
    size_t post = n;
    d[n++] = 248;
    put32(d + n, (unsigned)bop), n += 4;
    put32(d + n, 25400000), n += 4;
    put32(d + n, 473628672), n += 4;
    put32(d + n, 1000), n += 4;
    put32(d + n, 0), n += 4;
    put32(d + n, 0), n += 4;
    put16(d + n, 1), n += 2;
    put16(d + n, 1), n += 2;
    d[n++] = 249;
    put32(d + n, (unsigned)post), n += 4;
    d[n++] = 2;
    for (int k = 0; k < 4 || n % 4; k++) d[n++] = 223;
    write_file("sample.dvi", d, n);
}

static void encode(const char *name, const char *encoding, const char *bom, size_t bom_len, const char *utf8)
{
    iconv_t cd = iconv_open(encoding, "UTF-8");
    char buf[4096], *src = (char *)utf8, *dst = buf + bom_len;
    size_t left = strlen(utf8), room = sizeof buf - bom_len;
    memcpy(buf, bom, bom_len);
    if (cd == (iconv_t)-1 || iconv(cd, &src, &left, &dst, &room) == (size_t)-1) die("cannot encode %s", name);
    iconv_close(cd);
    write_file(name, buf, (size_t)(dst - buf));
}

/* The images of the comic books: natural order p1, p2, p10 (widths 100,
 * 110, 120 points), p2 a WebP; a resource fork and a text file that are not
 * pages. The folder has also sub/p11.png (130) and a hidden image. */
static void comics(void)
{
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/comic-dir", out);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/comic-dir/sub", out);
    mkdir(dir, 0755);
    png("comic-dir/p1.png", 100, 150, 0xC00000);
    webp("comic-dir/p2.webp", 110, 150, 0x00C000);
    jpeg("comic-dir/p10.jpg", 120, 150, 0x0000C0);
    png("comic-dir/sub/p11.png", 130, 150, 0xC0C000);
    png("comic-dir/.hidden.png", 140, 150, 0x00C0C0);
    write_text("comic-dir/notes.txt", "not a page\n");
    size_t s1, s2, s10;
    unsigned char *p1 = read_file(at("comic-dir/p1.png"), &s1);
    unsigned char *p2 = read_file(at("comic-dir/p2.webp"), &s2);
    unsigned char *p10 = read_file(at("comic-dir/p10.jpg"), &s10);
    struct entry e[] = {
        { "p10.jpg", p10, s10 },
        { "__MACOSX/._p1.png", "\0\5\26\7junk", 8 },
        { "p2.webp", p2, s2 },
        { "readme.txt", "not a page\n", 11 },
        { "p1.png", p1, s1 },
    };
    int n = (int)(sizeof e / sizeof *e);
    archive("sample.cbz", ARCHIVE_FORMAT_ZIP, e, n);
    archive("sample.cbt", ARCHIVE_FORMAT_TAR_USTAR, e, n);
    archive("sample.cb7", ARCHIVE_FORMAT_7ZIP, e, n);
    free(p1);
    free(p2);
    free(p10);
}

int main(int argc, char **argv)
{
    if (argc != 3) die("usage: make-view-samples OUTDIR FONT.ttf");
    out = argv[1];
    mkdir(out, 0755);
    ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);

    pdf();
    heavy_pdf();
    pbm("djvu-p1.pbm", 0);
    pbm("djvu-p2.pbm", 1);
    epub();
    xps(argv[2], "sample.xps", 0);
    xps(argv[2], "sample.oxps", 1);
    mobi();
    dvi();
    packed("sample.pdf.gz", "sample.pdf", ARCHIVE_FILTER_GZIP);
    packed("sample.dvi.gz", "sample.dvi", ARCHIVE_FILTER_GZIP);

    const char *fb2 =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\"><description><title-info>"
        "<book-title>FB2 Sample</book-title><author><first-name>View</first-name><last-name>Tester</last-name></author>"
        "<lang>en</lang></title-info></description>\n"
        "<body><section><title><p>Chapter</p></title><p>FictionBook marker badger</p></section></body></FictionBook>\n";
    write_text("sample.fb2", fb2);
    struct entry fb2_zip[] = { { "book.fb2", fb2, strlen(fb2) } };
    archive("sample.fb2.zip", ARCHIVE_FORMAT_ZIP, fb2_zip, 1);
    write_text("sample.md",
               "# Markdown Sample\n\nMarkdown marker walrus and *emphasis*.\n\n"
               "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95 \xE6\xAE\xB5\xE8\x90\xBD\n\n"
               "Jump to [Section Two](#section-two).\n\n"
               "## Section Two\n\nSee [example](https://example.org/).\n");
    write_text("sample.html",
               "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>HTML Sample</title></head><body>"
               "<h1>HTML</h1><p>HTML marker heron</p><p>\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95</p></body></html>\n");
    write_text("sample.svg",
               "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\">"
               "<rect x=\"20\" y=\"20\" width=\"100\" height=\"50\" fill=\"red\"/><text x=\"10\" y=\"90\" font-size=\"12\">SVG text</text></svg>\n");

    /* Plain text: "narwhal" and Chinese, in each encoding Okular's txt reads. */
    const char *text = "Plain text marker narwhal\n\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95 \xE6\x96\x87\xE6\x9C\xAC\n";
    write_text("sample-utf8.txt", text);
    write_text("README", text);
    encode("sample-utf16le.txt", "UTF-16LE", "\xFF\xFE", 2, text);
    encode("sample-utf16be.txt", "UTF-16BE", "\xFE\xFF", 2, text);
    encode("sample-gb18030.txt", "GB18030", "", 0, text);
    write_text("sample-latin1.txt", "Caf\xE9 au lait marker ocelot\n");
    /* Text whose first line MuPDF would take for a Markdown heading. */
    write_text("sample.conf", "# a comment\nkey = value marker yak\n");

    write_text("sample.ps",
               "%!PS-Adobe-3.0\n%%Title: PostScript Sample\n%%Creator: make-view-samples\n%%Pages: 2\n"
               "%%BoundingBox: 0 0 612 792\n%%EndComments\n"
               "%%Page: 1 1\n1 0 0 setrgbcolor 400 600 100 100 rectfill\n"
               "0 setgray /Helvetica findfont 24 scalefont setfont 72 720 moveto (PostScript page one) show\nshowpage\n"
               "%%Page: 2 2\n0 0 1 setrgbcolor 72 72 100 100 rectfill\nshowpage\n%%EOF\n");
    write_text("sample.eps",
               "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 200 100\n%%Title: EPS Sample\n%%EndComments\n"
               "0 0.6 0 setrgbcolor 20 20 100 50 rectfill\n%%EOF\n");
    packed("sample.ps.bz2", "sample.ps", ARCHIVE_FILTER_BZIP2);

    png("sample.png", 200, 100, 0xC00000);
    jpeg("sample.jpg", 200, 100, 0x00C000);
    gif("sample.gif", 200, 100, 0xC06000);
    bmp("sample.bmp", 200, 100, 0x6000C0);
    pnm_psd("sample.ppm", 200, 100, 0x00C060);
    pnm_psd("sample.psd", 200, 100, 0xC00060);
    tiff("sample.tiff");
    webp("sample.webp", 200, 100, 0x0000C0);
    heif("sample.avif", 200, 100, 0xC000C0, heif_compression_AV1);
    heif("sample.heic", 200, 100, 0x00C0C0, heif_compression_HEVC);
    jxl("sample.jxl", 200, 100, 0xC0C000);
    /* GraphicsMagick's: ICO and XCF by hand (it only reads them); the
     * sources make-view-samples.sh converts with gm into MNG (two frames
     * a second apart),
     * PCX, SGI, TGA, XBM (dark: black) and XPM. */
    ico("sample.ico", 64, 0xE08000);
    xcf("sample.xcf", 64, 0x8000E0);
    png("gm-src.png", 200, 100, 0x40A0E0);
    png("gm-src2.png", 200, 100, 0xE04040);
    png("gm-dark.png", 200, 100, 0x202020);
    comics();

    /* Faxes: raw G3 (.g3), raw G4 (.fax), and G3 with a PC Research header
     * saying normal resolution. */
    size_t n3, n4;
    unsigned char *g3 = fax(0, &n3), *g4 = fax(1, &n4);
    write_file("sample.g3", g3, n3);
    write_file("sample.fax", g4, n4);
    unsigned char *pcr = calloc(1, 64 + n3);
    memcpy(pcr, "\000PC Research, Inc\000\000\000\000\000\000", 24);
    pcr[29] = 0;
    memcpy(pcr + 64, g3, n3);
    write_file("sample-pcr.g3", pcr, 64 + n3);
    free(g3);
    free(g4);
    free(pcr);

    fz_drop_context(ctx);
    return 0;
}
