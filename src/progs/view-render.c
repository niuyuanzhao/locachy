/*
 * view-render: the documents of view (view-proto.h), each request on fd 3
 * answered in turn until view closes its end. It runs as the user, with
 * no_new_privs and no network, and refuses to run otherwise.
 *
 * The formats are those of Okular's generators. MuPDF reads most of them;
 * DjVu is djvulibre's and PostScript libspectre's (Ghostscript), as in
 * Okular. What MuPDF cannot read is first made into something it can:
 * plain text into UTF-8 (its encoding detected), WebP, HEIF/AVIF, JPEG XL
 * and the images ImageMagick reads (ICO, MNG, PCX, SGI, TGA, XBM, XCF, XPM,
 * DDS, OpenEXR, Radiance HDR: view-image.c) into PAM, a G3/G4 fax into PNG,
 * DVI into PDF (dvipdfmx), gzip and bzip2 into what they hold. Comic books
 * (zip, tar, RAR, 7z or a folder) are a document of their images in
 * Okular's natural order, read by MuPDF's archive and image readers, or by
 * libarchive and the libraries above.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <jxl/decode.h>
#include <libdjvu/ddjvuapi.h>
#include <libdjvu/miniexp.h>
#include <libheif/heif.h>
#include <libspectre/spectre.h>
#include <limits.h>
#include <math.h>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <net/if.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <webp/decode.h>
#include <webp/demux.h>

#include "view-image.h"
#include "view-proto.h"

#define DVIPDFMX "/usr/bin/dvipdfmx"
#define MAX_UNPACKED (1ull << 30) /* gzip/bzip2 contents at most this big */
#define MAX_CSS (1 << 20)
#define MAX_QUADS 4096
#define FAX_COLUMNS 1728          /* as Okular's fax generator assumes */
#define FAX_MAX_ROWS 65536

/* MuPDF's document handlers, to tell which one a file is for. */
extern fz_document_handler pdf_document_handler, xps_document_handler, epub_document_handler,
    mobi_document_handler, fb2_document_handler, md_document_handler, txt_document_handler,
    html_document_handler, xhtml_document_handler, cbz_document_handler, img_document_handler,
    svg_document_handler;

static fz_context *ctx;
static fz_cookie cookie;           /* SIGUSR1 sets its abort */
static enum { NONE, MUPDF, DJVU, PS } kind;
static fz_document *doc;
static int locked, npages, has_text;
static char format[64], title[256];
static int data_fd = -1;           /* unpacked contents the document is read from */
static int sys_errno;              /* the errno of the last FZ_ERROR_SYSTEM thrown here */

/* MuPDF: the page of the last requests, and its text. */
static fz_page *page;
static int page_no = -1;
static fz_rect page_box;
static fz_stext_page *page_text;

/* Bulk results: a buffer copied into a sealed memfd, or a memfd written in place. */
struct out {
    fz_buffer *buf;
    int fd;
    void *map;
    size_t size;
};

__attribute__((format(printf, 1, 2)))
static void say(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    /* One write, so lines of other processes on view's stderr do not cut it. */
    dprintf(2, "view-render: %s\n", n < 0 ? fmt : line);
}

__attribute__((format(printf, 3, 4)))
static void fail(struct view_reply *r, int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->text, sizeof r->text, fmt, ap);
    va_end(ap);
    r->status = status;
}

/* Throw with an errno kept for the reply. */
__attribute__((noreturn))
static void throw_errno(int err, const char *what)
{
    sys_errno = err;
    fz_throw(ctx, FZ_ERROR_SYSTEM, "%s: %s", what, strerror(err));
}

static void on_usr1(int sig)
{
    (void)sig;
    cookie.abort = 1;
}

/* Fail closed: never parse documents as root, with privileges to gain, or
 * with a network to reach. */
static int sandboxed(void)
{
    if (getuid() == 0 || geteuid() == 0) {
        say("refusing to run as root");
        return 0;
    }
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) {
        say("refusing to run without no_new_privs");
        return 0;
    }
    struct if_nameindex *ifs = if_nameindex();
    if (!ifs) {
        say("refusing to run: cannot list network interfaces: %s", strerror(errno));
        return 0;
    }
    int ok = 1;
    for (struct if_nameindex *i = ifs; i->if_index; i++) {
        if (strcmp(i->if_name, "lo") != 0) {
            say("refusing to run with network interface %s", i->if_name);
            ok = 0;
        }
    }
    if_freenameindex(ifs);
    return ok;
}

static int seal(int fd)
{
    return fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
}

/* A sealed memfd holding size bytes of data: its fd, or -1. */
static int sealed(const void *data, size_t size)
{
    int fd = memfd_create("view-render", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    for (size_t done = 0; done < size;) {
        ssize_t n = write(fd, (const char *)data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    if (seal(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* A memfd of size bytes mapped writable, for pixels drawn in place. */
static void *new_pixels(struct out *o, size_t size)
{
    o->fd = memfd_create("view-render", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (o->fd < 0) throw_errno(errno, "memfd");
    if (ftruncate(o->fd, (off_t)size) < 0) throw_errno(errno, "memfd");
    o->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, o->fd, 0);
    if (o->map == MAP_FAILED) {
        o->map = NULL;
        throw_errno(errno, "mmap");
    }
    o->size = size;
    return o->map;
}

static void put(fz_buffer *b, const void *data, size_t size)
{
    fz_append_data(ctx, b, data, size);
}

/* "KEY\tVALUE\n", tabs and newlines in the value made spaces. */
static void put_info(fz_buffer *b, const char *key, const char *value)
{
    if (!value || !*value) return;
    fz_append_printf(ctx, b, "%s\t", key);
    for (const char *s = value; *s; s++) fz_append_byte(ctx, b, *s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s);
    fz_append_byte(ctx, b, '\n');
}

static void copy_text(char *dst, size_t size, const char *src)
{
    size_t n = 0;
    for (; src && src[n] && n + 1 < size; n++) dst[n] = src[n] == '\t' || src[n] == '\n' ? ' ' : src[n];
    /* Do not end inside a UTF-8 sequence. */
    if (src && src[n]) while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    dst[n] = '\0';
}

static const char *extension(const char *name)
{
    const char *slash = strrchr(name, '/'), *dot = strrchr(name, '.');
    return dot && (!slash || dot > slash) ? dot + 1 : "";
}

static const char *base_name(const char *name)
{
    const char *slash = strrchr(name, '/');
    return slash ? slash + 1 : name;
}

/* Okular's natural order (generators/comicbook/qnatsort.cpp), case-sensitive
 * as its comic books sort their pages. */
static int compare_right(const char *a, const char *b)
{
    int bias = 0;
    for (;; a++, b++) {
        int da = isdigit((unsigned char)*a), db = isdigit((unsigned char)*b);
        if (!da && !db) return bias;
        if (!da) return -1;
        if (!db) return +1;
        if (*a < *b && !bias) bias = -1;
        else if (*a > *b && !bias) bias = +1;
    }
}

static int compare_left(const char *a, const char *b)
{
    for (;; a++, b++) {
        int da = isdigit((unsigned char)*a), db = isdigit((unsigned char)*b);
        if (!da && !db) return 0;
        if (!da) return -1;
        if (!db) return +1;
        if (*a != *b) return *a < *b ? -1 : +1;
    }
}

static int natural_compare(const void *pa, const void *pb)
{
    const char *a = *(char *const *)pa, *b = *(char *const *)pb;
    for (;;) {
        while (isspace((unsigned char)*a)) a++;
        while (isspace((unsigned char)*b)) b++;
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            int r = *a == '0' || *b == '0' ? compare_left(a, b) : compare_right(a, b);
            if (r) return r;
        }
        if (!*a && !*b) return 0;
        if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : +1;
        a++;
        b++;
    }
}

/* Plain text, as Okular's txt generator reads it: a BOM (UTF-8, UTF-16LE or
 * UTF-16BE) decides; else valid UTF-8; else GB18030; else Latin-1. MuPDF is
 * given it as UTF-8 with a BOM. */
static int valid_utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + len > n || (len == 2 && c < 0xC2)) return 0;
        for (size_t k = 1; k < len; k++) if ((s[i + k] & 0xC0) != 0x80) return 0;
        if (len == 3 && ((c == 0xE0 && s[i + 1] < 0xA0) || (c == 0xED && s[i + 1] >= 0xA0))) return 0;
        if (len == 4 && ((c == 0xF0 && s[i + 1] < 0x90) || c > 0xF4 || (c == 0xF4 && s[i + 1] >= 0x90))) return 0;
        i += len;
    }
    return 1;
}

/* in converted from encoding to UTF-8, appended to out: 0, or -1 if it is not that encoding. */
static int convert(fz_buffer *out, const char *encoding, const unsigned char *in, size_t n)
{
    iconv_t cd = iconv_open("UTF-8", encoding);
    if (cd == (iconv_t)-1) return -1;
    char chunk[16384];
    char *src = (char *)in;
    size_t left = n;
    int ok = 0;
    while (left > 0) {
        char *dst = chunk;
        size_t room = sizeof chunk;
        size_t r = iconv(cd, &src, &left, &dst, &room);
        put(out, chunk, sizeof chunk - room);
        if (r == (size_t)-1 && errno != E2BIG) {
            ok = -1;
            break;
        }
    }
    iconv_close(cd);
    return ok;
}

static fz_buffer *text_to_utf8(fz_buffer *in)
{
    unsigned char *s;
    size_t n = fz_buffer_storage(ctx, in, &s);
    fz_buffer *out = fz_new_buffer(ctx, n + 16);
    fz_try(ctx) {
        put(out, "\xEF\xBB\xBF", 3);
        if (n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) {
            put(out, s + 3, n - 3);
        } else if (n >= 2 && (!memcmp(s, "\xFF\xFE", 2) || !memcmp(s, "\xFE\xFF", 2))) {
            convert(out, s[0] == 0xFF ? "UTF-16LE" : "UTF-16BE", s + 2, n - 2);
        } else if (valid_utf8(s, n)) {
            put(out, s, n);
        } else {
            fz_resize_buffer(ctx, out, 3);
            if (convert(out, "GB18030", s, n) < 0) {
                fz_resize_buffer(ctx, out, 3);
                for (size_t i = 0; i < n; i++) fz_append_rune(ctx, out, s[i]);
            }
        }
    }
    fz_catch(ctx) {
        fz_drop_buffer(ctx, out);
        fz_rethrow(ctx);
    }
    return out;
}

/* A file MuPDF has no handler for is text when it has no NUL bytes. */
static int looks_like_text(const unsigned char *m, size_t n)
{
    if (n >= 2 && (!memcmp(m, "\xFF\xFE", 2) || !memcmp(m, "\xFE\xFF", 2))) return 1;
    return n > 0 && !memchr(m, 0, n);
}

/* WebP, HEIF/AVIF and JPEG XL: what Okular shows through Qt's image plugins
 * (kimageformats, qtimageformats), decoded to RGBA and given to MuPDF as a
 * PAM image. The first frame, or the primary image, as kimgio shows it. */
static int is_webp(const unsigned char *m, size_t n)
{
    return n >= 12 && !memcmp(m, "RIFF", 4) && !memcmp(m + 8, "WEBP", 4);
}

static int is_heif(const unsigned char *m, size_t n)
{
    return n >= 12 && !memcmp(m + 4, "ftyp", 4) && heif_check_filetype(m, (int)(n > INT_MAX ? INT_MAX : n)) == heif_filetype_yes_supported;
}

static int is_jxl(const unsigned char *m, size_t n)
{
    JxlSignature s = JxlSignatureCheck(m, n);
    return s == JXL_SIG_CODESTREAM || s == JXL_SIG_CONTAINER;
}

static int is_kimgio(const unsigned char *m, size_t n)
{
    return is_webp(m, n) || is_heif(m, n) || is_jxl(m, n);
}

static fz_buffer *pam(int w, int h, const unsigned char *rgba, size_t stride)
{
    if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > VIEW_MAX_PIXELS) {
        fz_throw(ctx, FZ_ERROR_LIMIT, "image of %d x %d pixels", w, h);
    }
    fz_buffer *b = fz_new_buffer(ctx, (size_t)w * (size_t)h * 4 + 80);
    fz_try(ctx) {
        fz_append_printf(ctx, b, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
        for (int y = 0; y < h; y++) put(b, rgba + (size_t)y * stride, (size_t)w * 4);
    }
    fz_catch(ctx) {
        fz_drop_buffer(ctx, b);
        fz_rethrow(ctx);
    }
    return b;
}

static fz_buffer *webp_pam(const unsigned char *data, size_t n)
{
    WebPBitstreamFeatures f;
    if (WebPGetFeatures(data, n, &f) != VP8_STATUS_OK) fz_throw(ctx, FZ_ERROR_FORMAT, "broken WebP image");
    if (!f.has_animation) {
        int w, h;
        uint8_t *rgba = WebPDecodeRGBA(data, n, &w, &h);
        if (!rgba) fz_throw(ctx, FZ_ERROR_FORMAT, "broken WebP image");
        fz_buffer *b = NULL;
        fz_var(b);
        fz_try(ctx) b = pam(w, h, rgba, (size_t)w * 4);
        fz_always(ctx) WebPFree(rgba);
        fz_catch(ctx) fz_rethrow(ctx);
        return b;
    }
    WebPAnimDecoderOptions opt;
    WebPAnimDecoderOptionsInit(&opt);
    opt.color_mode = MODE_RGBA;
    WebPData wd = { data, n };
    WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &opt);
    WebPAnimInfo info;
    uint8_t *frame;
    int ts;
    if (!dec || !WebPAnimDecoderGetInfo(dec, &info) || !WebPAnimDecoderGetNext(dec, &frame, &ts)) {
        WebPAnimDecoderDelete(dec);
        fz_throw(ctx, FZ_ERROR_FORMAT, "broken animated WebP image");
    }
    fz_buffer *b = NULL;
    fz_var(b);
    fz_try(ctx) b = pam((int)info.canvas_width, (int)info.canvas_height, frame, (size_t)info.canvas_width * 4);
    fz_always(ctx) WebPAnimDecoderDelete(dec);
    fz_catch(ctx) fz_rethrow(ctx);
    return b;
}

static fz_buffer *heif_pam(const unsigned char *data, size_t n)
{
    static int initialized;
    if (!initialized) {
        heif_init(NULL);
        initialized = 1;
    }
    struct heif_context *hc = heif_context_alloc();
    struct heif_image_handle *handle = NULL;
    struct heif_image *img = NULL;
    fz_buffer *b = NULL;
    char why[256] = "";
    fz_var(b);
    struct heif_error e =heif_context_read_from_memory_without_copy(hc, data, n, NULL);
    if (!e.code) e = heif_context_get_primary_image_handle(hc, &handle);
    if (!e.code) e = heif_decode_image(handle, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, NULL);
    if (e.code) {
        snprintf(why, sizeof why, "%s", e.message ? e.message : "cannot decode");
    } else {
        size_t stride = 0;
        const uint8_t *p = heif_image_get_plane_readonly2(img, heif_channel_interleaved, &stride);
        int w = heif_image_get_width(img, heif_channel_interleaved), h = heif_image_get_height(img, heif_channel_interleaved);
        fz_try(ctx) {
            if (!p || stride < (size_t)w * 4) fz_throw(ctx, FZ_ERROR_FORMAT, "no RGBA pixels");
            b = pam(w, h, p, stride);
        }
        fz_catch(ctx) snprintf(why, sizeof why, "%s", fz_caught_message(ctx));
    }
    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(hc);
    if (!b) fz_throw(ctx, FZ_ERROR_FORMAT, "HEIF/AVIF image: %s", why);
    return b;
}

static fz_buffer *jxl_pam(const unsigned char *data, size_t n)
{
    JxlDecoder *dec = JxlDecoderCreate(NULL);
    JxlPixelFormat fmt = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
    JxlBasicInfo info = { 0 };
    unsigned char *pixels = NULL;
    size_t size = 0;
    int w = 0, h = 0, done = 0;
    if (!dec || JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec, data, n) != JXL_DEC_SUCCESS) {
        JxlDecoderDestroy(dec);
        fz_throw(ctx, FZ_ERROR_LIBRARY, "cannot start the JPEG XL decoder");
    }
    JxlDecoderCloseInput(dec);
    while (!done) {
        JxlDecoderStatus s = JxlDecoderProcessInput(dec);
        if (s == JXL_DEC_BASIC_INFO && JxlDecoderGetBasicInfo(dec, &info) == JXL_DEC_SUCCESS) {
            /* Pixels come out oriented; the size is before that. */
            w = (int)(info.orientation > 4 ? info.ysize : info.xsize);
            h = (int)(info.orientation > 4 ? info.xsize : info.ysize);
            if (!w || !h || (uint64_t)w * (uint64_t)h > VIEW_MAX_PIXELS) break;
        } else if (s == JXL_DEC_NEED_IMAGE_OUT_BUFFER && !pixels) {
            if (JxlDecoderImageOutBufferSize(dec, &fmt, &size) != JXL_DEC_SUCCESS || size != (size_t)w * (size_t)h * 4 ||
                !(pixels = malloc(size)) || JxlDecoderSetImageOutBuffer(dec, &fmt, pixels, size) != JXL_DEC_SUCCESS) {
                break;
            }
        } else if (s == JXL_DEC_FULL_IMAGE && pixels) {
            done = 1;
        } else {
            break;
        }
    }
    JxlDecoderDestroy(dec);
    fz_buffer *b = NULL;
    fz_var(b);
    fz_try(ctx) {
        if (!done) fz_throw(ctx, FZ_ERROR_FORMAT, "broken or too large JPEG XL image");
        b = pam(w, h, pixels, (size_t)w * 4);
    }
    fz_always(ctx) free(pixels);
    fz_catch(ctx) fz_rethrow(ctx);
    return b;
}

/* The rest of kimgio's types, read by ImageMagick (view-image.c): ICO, CUR,
 * MNG, PCX, SGI (image/x-rgb), TGA, XBM, XCF, XPM, DDS, OpenEXR and Radiance
 * HDR, each by the coder named for it. */
static fz_buffer *magick_pam(const unsigned char *data, size_t n, const char *coder)
{
    char why[256];
    int w = 0, h = 0;
    unsigned char *px = magick_rgba(data, n, coder, VIEW_MAX_PIXELS, &w, &h, why, sizeof why);
    if (!px) fz_throw(ctx, FZ_ERROR_FORMAT, "%s image: %s", coder, why);
    fz_buffer *b = NULL;
    fz_var(b);
    fz_try(ctx) b = pam(w, h, px, (size_t)w * 4);
    fz_always(ctx) free(px);
    fz_catch(ctx) fz_rethrow(ctx);
    return b;
}

/* An image MuPDF cannot read, by the decoders above (name: for those
 * ImageMagick's coders know only by name), as PAM. */
static fz_buffer *convert_pam(const unsigned char *data, size_t n, const char *name)
{
    if (is_webp(data, n)) return webp_pam(data, n);
    if (is_heif(data, n)) return heif_pam(data, n);
    if (is_jxl(data, n)) return jxl_pam(data, n);
    const char *coder = magick_coder_content(data, n);
    if (!coder && name) coder = magick_coder_name(name);
    if (coder) return magick_pam(data, n, coder);
    fz_throw(ctx, FZ_ERROR_UNSUPPORTED, "unknown image type");
}

/* A G3 or G4 fax as Okular's fax generator reads it (faxdocument.cpp): a
 * raw stream of FAX_COLUMNS columns, its height what it holds; .g3 is one-
 * dimensional G3 (Okular's g31expand), anything else G4; a 64-byte
 * ghostscript/PC Research header is skipped, its byte 29 saying fine (196
 * lines an inch) or normal (98) resolution. Made a PNG of 204 dots an inch
 * (the fax's across), so the page has the fax's paper size. */
static const char FAX_MAGIC[] = "\000PC Research, Inc\000\000\000\000\000\000";

static fz_buffer *fax_png(fz_buffer *in, int g4)
{
    unsigned char *s;
    size_t n = fz_buffer_storage(ctx, in, &s), skip = 0;
    int fine = 1;
    if (n >= 64 && !memcmp(s, FAX_MAGIC, sizeof FAX_MAGIC - 1)) {
        fine = s[29] != 0;
        skip = 64;
    }
    const int stride = (FAX_COLUMNS + 7) / 8;
    fz_stream *mem = NULL, *fax = NULL;
    fz_buffer *rows = NULL, *png = NULL;
    fz_pixmap *pix = NULL;
    fz_var(mem);
    fz_var(fax);
    fz_var(rows);
    fz_var(pix);
    fz_try(ctx) {
        mem = fz_open_memory(ctx, s + skip, n - skip);
        fax = fz_open_faxd(ctx, mem, g4 ? -1 : 0, 0, 0, FAX_COLUMNS, 0, 1, 0);
        rows = fz_read_all(ctx, fax, (size_t)stride * 1024);
        unsigned char *bits;
        size_t len = fz_buffer_storage(ctx, rows, &bits);
        int h = (int)(len / (size_t)stride);
        if (h > FAX_MAX_ROWS) h = FAX_MAX_ROWS;
        if (h == 0) fz_throw(ctx, FZ_ERROR_FORMAT, "no fax lines decoded");
        /* Lines repeated to square pixels of 204 dpi, as MuPDF's images
         * have one resolution for both. */
        int yres = fine ? 196 : 98, sh = (h * 204 + yres / 2) / yres;
        pix = fz_new_pixmap(ctx, fz_device_gray(ctx), FAX_COLUMNS, sh, NULL, 0);
        pix->xres = pix->yres = 204;
        for (int y = 0; y < sh; y++) {
            const unsigned char *row = bits + (size_t)(y * yres / 204) * stride;
            for (int x = 0; x < FAX_COLUMNS; x++) pix->samples[(size_t)y * pix->stride + x] = row[x / 8] & (0x80 >> (x % 8)) ? 255 : 0;
        }
        png = fz_new_buffer_from_pixmap_as_png(ctx, pix, fz_default_color_params);
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pix);
        fz_drop_buffer(ctx, rows);
        fz_drop_stream(ctx, fax);
        fz_drop_stream(ctx, mem);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    return png;
}

/* gzip or bzip2 (Okular: x-gzpdf, x-bzpdf, x-gzeps, gz/bz2 PostScript and
 * DVI) unpacked into a memfd: its fd. */
static int is_packed(const unsigned char *m, size_t n)
{
    return (n >= 2 && m[0] == 0x1F && m[1] == 0x8B) || (n >= 3 && !memcmp(m, "BZh", 3));
}

static int unpack(const char *path)
{
    struct archive *a = archive_read_new();
    struct archive_entry *e;
    int fd = -1;
    uint64_t total = 0;
    char chunk[65536];
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_bzip2(a);
    archive_read_support_format_raw(a);
    if (archive_read_open_filename(a, path, sizeof chunk) != ARCHIVE_OK ||
        archive_read_next_header(a, &e) != ARCHIVE_OK) {
        goto bad;
    }
    fd = memfd_create("view-render-data", MFD_CLOEXEC);
    if (fd < 0) goto bad;
    for (;;) {
        la_ssize_t n = archive_read_data(a, chunk, sizeof chunk);
        if (n == 0) break;
        if (n < 0 || (total += (uint64_t)n) > MAX_UNPACKED || write(fd, chunk, (size_t)n) != n) goto bad;
    }
    archive_read_free(a);
    return fd;
bad:
    say("cannot unpack %s: %s", path, archive_error_string(a) ? archive_error_string(a) : total > MAX_UNPACKED ? "too large" : strerror(errno));
    archive_read_free(a);
    if (fd >= 0) close(fd);
    return -1;
}

/* DVI: Okular draws it with TeX's fonts; here dvipdfmx (TeX Live) makes a
 * PDF of it in a private directory, which MuPDF opens. */
static fz_document *open_dvi(const char *path, struct view_reply *r)
{
    if (access(DVIPDFMX, X_OK) != 0) {
        fail(r, ENOENT, "DVI needs TeX: install texlive-bin");
        return NULL;
    }
    char dir[] = "/tmp/view-render-XXXXXX", pdf[64], in[PATH_MAX], cwd[PATH_MAX];
    if (!mkdtemp(dir)) {
        fail(r, errno, "Cannot make a temporary directory: %s", strerror(errno));
        return NULL;
    }
    snprintf(pdf, sizeof pdf, "%s/out.pdf", dir);
    /* The DVI's own directory, where its \special files are found. */
    snprintf(in, sizeof in, "%s", path);
    snprintf(cwd, sizeof cwd, "%s", path);
    char *slash = strrchr(cwd, '/');
    if (slash && slash != cwd) *slash = '\0';
    else snprintf(cwd, sizeof cwd, "%s", slash ? "/" : ".");
    if (!strncmp(path, "/proc/self/fd/", 14)) snprintf(cwd, sizeof cwd, "%s", dir);
    pid_t pid = fork();
    if (pid == 0) {
        /* An unpacked DVI is our memfd: the child has it as the same fd. */
        if (data_fd >= 0) fcntl(data_fd, F_SETFD, 0);
        if (chdir(cwd) < 0) _exit(127);
        execl(DVIPDFMX, "dvipdfmx", "-q", "-o", pdf, in, (char *)NULL);
        _exit(127);
    }
    int status = -1;
    while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    fz_document *d = NULL;
    if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fz_try(ctx) d = fz_open_document(ctx, pdf);
        fz_catch(ctx) fail(r, EINVAL, "dvipdfmx's PDF: %s", fz_caught_message(ctx));
    } else {
        fail(r, EINVAL, "dvipdfmx could not convert this DVI file");
    }
    unlink(pdf); /* MuPDF keeps it open */
    rmdir(dir);
    return d;
}

/* Comic books, as Okular's comicbook generator reads them: the images of a
 * zip, tar, RAR or 7z archive or of a folder (and its folders one level
 * down), in natural order of their paths, one a page. Zip and tar are read
 * by MuPDF's archive code, folders from the disk, the rest by libarchive
 * into memory. A page is decoded when it is shown, by MuPDF's image code or
 * the decoders above (WebP, HEIF, JPEG XL, ImageMagick). */
typedef struct {
    fz_document super;
    fz_archive *arch;
    char **names;
    int count, cap;
} comic_doc;

typedef struct {
    fz_page super;
    fz_image *image;
} comic_page;

static int is_image_name(const char *name)
{
    static const char *const exts[] = {
        "bmp", "gif", "jpg", "jpeg", "jpe", "jfif", "png", "tif", "tiff", "jp2", "j2k", "jpx", "jxr", "wdp", "hdp",
        "pbm", "pgm", "ppm", "pnm", "pam", "psd", "webp", "avif", "heic", "heif", "jxl", "ico", "cur", "mng", "pcx",
        "sgi", "rgb", "rgba", "bw", "tga", "icb", "vda", "vst", "xbm", "xcf", "xpm", "dds", "exr", "hdr", NULL
    };
    const char *base = base_name(name), *e = extension(name);
    /* Hidden files, and the resource forks macOS adds, are not pages. */
    if (base[0] == '.' || !strncmp(name, "__MACOSX/", 9)) return 0;
    for (int i = 0; exts[i]; i++) if (!strcasecmp(e, exts[i])) return 1;
    return 0;
}

static void comic_add(comic_doc *d, const char *name)
{
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 64;
        d->names = fz_realloc_array(ctx, d->names, d->cap, char *);
    }
    d->names[d->count] = fz_strdup(ctx, name);
    d->count++;
}

static void comic_drop(fz_context *c, fz_document *doc_)
{
    comic_doc *d = (comic_doc *)doc_;
    for (int i = 0; i < d->count; i++) fz_free(c, d->names[i]);
    fz_free(c, d->names);
    fz_drop_archive(c, d->arch);
}

static int comic_count(fz_context *c, fz_document *doc_, int chapter)
{
    (void)c;
    (void)chapter;
    return ((comic_doc *)doc_)->count;
}

/* The page's size as MuPDF's cbz pages have it: the image at its resolution,
 * its EXIF orientation applied. */
static fz_rect comic_bound(fz_context *c, fz_page *page_, fz_box_type box)
{
    (void)box;
    fz_image *image = ((comic_page *)page_)->image;
    if (!image) return fz_make_rect(0, 0, 612, 792); /* undecodable: a blank page */
    int xres, yres;
    fz_image_resolution(image, &xres, &yres);
    float w = image->w * 72.0f / xres, h = image->h * 72.0f / yres;
    uint8_t o = fz_image_orientation(c, image);
    return o == 0 || (o & 1) ? fz_make_rect(0, 0, w, h) : fz_make_rect(0, 0, h, w);
}

static void comic_run(fz_context *c, fz_page *page_, fz_device *dev, fz_matrix ctm, fz_cookie *ck)
{
    (void)ck;
    fz_image *image = ((comic_page *)page_)->image;
    if (!image) return;
    fz_rect b = comic_bound(c, page_, FZ_MEDIA_BOX);
    fz_matrix m = fz_post_scale(fz_image_orientation_matrix(c, image), b.x1, b.y1);
    fz_fill_image(c, dev, image, fz_concat(m, ctm), 1, fz_default_color_params);
}

static void comic_drop_page(fz_context *c, fz_page *page_)
{
    fz_drop_image(c, ((comic_page *)page_)->image);
}

static fz_page *comic_load(fz_context *c, fz_document *doc_, int chapter, int number)
{
    (void)chapter;
    comic_doc *d = (comic_doc *)doc_;
    if (number < 0 || number >= d->count) fz_throw(c, FZ_ERROR_ARGUMENT, "no page %d", number);
    comic_page *p = fz_new_derived_page(c, comic_page, doc_);
    p->super.bound_page = comic_bound;
    p->super.run_page_contents = comic_run;
    p->super.drop_page = comic_drop_page;
    fz_buffer *buf = NULL, *conv = NULL;
    fz_var(buf);
    fz_var(conv);
    fz_try(c) {
        buf = fz_read_archive_entry(c, d->arch, d->names[number]);
        unsigned char *s;
        size_t n = fz_buffer_storage(c, buf, &s);
        if (n >= 8 && fz_recognize_image_format(c, s) != FZ_IMAGE_UNKNOWN) {
            p->image = fz_new_image_from_buffer(c, buf);
        } else {
            /* No resolution in them: MuPDF's 96 dpi for such images. */
            conv = convert_pam(s, n, d->names[number]);
            p->image = fz_new_image_from_buffer(c, conv);
            p->image->xres = p->image->yres = 96;
        }
    }
    fz_always(c) {
        fz_drop_buffer(c, buf);
        fz_drop_buffer(c, conv);
    }
    fz_catch(c) {
        fz_report_error(c);
        say("comic book page %s: cannot decode it, leaving it blank", d->names[number]);
    }
    return &p->super;
}

/* A folder's images, and those of its folders, as Okular lists them (not
 * hidden ones; one level down). */
static void comic_list_dir(comic_doc *d, const char *dir, const char *prefix, int depth)
{
    char path[PATH_MAX], name[PATH_MAX];
    snprintf(path, sizeof path, "%s%s%s", dir, *prefix ? "/" : "", prefix);
    DIR *dp = opendir(path);
    if (!dp) {
        if (depth == 0) throw_errno(errno, "cannot read the folder");
        return;
    }
    struct dirent *e;
    fz_try(ctx) {
        while ((e = readdir(dp)) != NULL) {
            if (e->d_name[0] == '.') continue;
            if (snprintf(name, sizeof name, "%s%s%s", prefix, *prefix ? "/" : "", e->d_name) >= (int)sizeof name) continue;
            char full[PATH_MAX];
            struct stat st;
            if (snprintf(full, sizeof full, "%s/%s", dir, name) >= (int)sizeof full || stat(full, &st) < 0) continue;
            if (S_ISDIR(st.st_mode) && depth < 1) comic_list_dir(d, dir, name, depth + 1);
            else if (S_ISREG(st.st_mode) && is_image_name(name)) comic_add(d, name);
        }
    }
    fz_always(ctx) closedir(dp);
    fz_catch(ctx) fz_rethrow(ctx);
}

/* An archive MuPDF cannot read (RAR, 7z, ...) by libarchive: its images
 * kept in memory, in an archive of MuPDF's. */
static void comic_read_archive(comic_doc *d, const char *path)
{
    struct archive *a = archive_read_new();
    struct archive_entry *e;
    fz_buffer *buf = NULL;
    fz_var(buf);
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    fz_try(ctx) {
        if (archive_read_open_filename(a, path, 65536) != ARCHIVE_OK) {
            fz_throw(ctx, FZ_ERROR_FORMAT, "%s", archive_error_string(a));
        }
        d->arch = fz_new_tree_archive(ctx, NULL);
        int r;
        while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
            const char *name = archive_entry_pathname_utf8(e);
            if (!name) name = archive_entry_pathname(e);
            if (archive_entry_filetype(e) != AE_IFREG || !name || !is_image_name(name)) continue;
            buf = fz_new_buffer(ctx, archive_entry_size(e) > 0 ? (size_t)archive_entry_size(e) : 65536);
            for (;;) {
                char chunk[65536];
                la_ssize_t n = archive_read_data(a, chunk, sizeof chunk);
                if (n == 0) break;
                if (n < 0) fz_throw(ctx, FZ_ERROR_FORMAT, "%s: %s", name, archive_error_string(a));
                put(buf, chunk, (size_t)n);
            }
            fz_tree_archive_add_buffer(ctx, d->arch, name, buf);
            fz_drop_buffer(ctx, buf);
            buf = NULL;
            comic_add(d, name);
        }
        if (r != ARCHIVE_EOF) fz_throw(ctx, FZ_ERROR_FORMAT, "%s", archive_error_string(a));
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
        archive_read_free(a);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

enum { COMIC_MUPDF, COMIC_DIR, COMIC_LIBARCHIVE };

static fz_document *open_comic(const char *path, int how)
{
    comic_doc *d = fz_new_derived_document(ctx, comic_doc);
    d->super.drop_document = comic_drop;
    d->super.count_pages = comic_count;
    d->super.load_page = comic_load;
    fz_try(ctx) {
        if (how == COMIC_DIR) {
            d->arch = fz_open_directory(ctx, path);
            comic_list_dir(d, path, "", 0);
        } else if (how == COMIC_MUPDF) {
            d->arch = fz_open_archive(ctx, path);
            int n = fz_count_archive_entries(ctx, d->arch);
            for (int i = 0; i < n; i++) {
                const char *name = fz_list_archive_entry(ctx, d->arch, i);
                if (name && is_image_name(name)) comic_add(d, name);
            }
        } else {
            comic_read_archive(d, path);
        }
        if (d->count == 0) fz_throw(ctx, FZ_ERROR_FORMAT, "no images in this comic book");
        qsort(d->names, (size_t)d->count, sizeof *d->names, natural_compare);
    }
    fz_catch(ctx) {
        fz_drop_document(ctx, &d->super);
        fz_rethrow(ctx);
    }
    return &d->super;
}

/* DjVu, by djvulibre as in Okular. Its text layer is words with boxes in
 * page pixels from the bottom left; here they are points from the top left
 * like MuPDF's, and search and selection work on them by words. A page's
 * initial rotation is drawn, but its text is taken as unrotated. */
static ddjvu_context_t *djvu_ctx;
static ddjvu_document_t *djvu_doc;

struct djvu_word {
    fz_rect box;
    int line;
    char *text;
};

static struct {
    int page, count, cap;
    struct djvu_word *words;
} dtext = { .page = -1 };

static void djvu_messages(int wait)
{
    const ddjvu_message_t *m;
    if (wait) ddjvu_message_wait(djvu_ctx);
    while ((m = ddjvu_message_peek(djvu_ctx)) != NULL) {
        if (m->m_any.tag == DDJVU_ERROR) say("DjVu: %s", m->m_error.message);
        ddjvu_message_pop(djvu_ctx);
    }
}

static void djvu_text_drop(void)
{
    for (int i = 0; i < dtext.count; i++) free(dtext.words[i].text);
    free(dtext.words);
    memset(&dtext, 0, sizeof dtext);
    dtext.page = -1;
}

static void djvu_close(void)
{
    djvu_text_drop();
    if (djvu_doc) ddjvu_document_release(djvu_doc);
    if (djvu_ctx) ddjvu_context_release(djvu_ctx);
    djvu_doc = NULL;
    djvu_ctx = NULL;
}

static int djvu_open(const char *path, struct view_reply *r)
{
    djvu_ctx = ddjvu_context_create("view-render");
    djvu_doc = djvu_ctx ? ddjvu_document_create_by_filename_utf8(djvu_ctx, path, 1) : NULL;
    if (!djvu_doc) {
        fail(r, EINVAL, "Cannot open this DjVu document");
        return -1;
    }
    while (!ddjvu_document_decoding_done(djvu_doc)) djvu_messages(1);
    djvu_messages(0);
    if (ddjvu_document_decoding_error(djvu_doc) || (npages = ddjvu_document_get_pagenum(djvu_doc)) <= 0) {
        fail(r, EINVAL, "Broken DjVu document");
        return -1;
    }
    return 0;
}

static ddjvu_pageinfo_t djvu_info(int n)
{
    ddjvu_pageinfo_t info;
    ddjvu_status_t s;
    if (n < 0 || n >= npages) fz_throw(ctx, FZ_ERROR_ARGUMENT, "no page %d", n);
    while ((s = ddjvu_document_get_pageinfo(djvu_doc, n, &info)) < DDJVU_JOB_OK) djvu_messages(1);
    if (s != DDJVU_JOB_OK || info.dpi <= 0 || info.width <= 0 || info.height <= 0) {
        fz_throw(ctx, FZ_ERROR_FORMAT, "broken DjVu page %d", n + 1);
    }
    return info;
}

static void djvu_size(int n, float *pw, float *ph)
{
    ddjvu_pageinfo_t info = djvu_info(n);
    *pw = info.width * 72.0f / info.dpi;
    *ph = info.height * 72.0f / info.dpi;
}

/* Page n's index for a DjVu link: "#12" (the 12th page), "#+1"/"#-1"
 * (relative to page from), or "#name" (a page's file id, name or title). */
static int djvu_target(const char *url, int from)
{
    if (!url || url[0] != '#') return -1;
    const char *s = url + 1;
    char *end;
    if (*s == '+' || *s == '-') {
        long d = strtol(s, &end, 10);
        return *end || from + d < 0 || from + d >= npages ? -1 : (int)(from + d);
    }
    long n = strtol(s, &end, 10);
    if (*s && !*end) return n >= 1 && n <= npages ? (int)n - 1 : -1;
    int files = ddjvu_document_get_filenum(djvu_doc);
    for (int i = 0; i < files; i++) {
        ddjvu_fileinfo_t fi;
        if (ddjvu_document_get_fileinfo(djvu_doc, i, &fi) != DDJVU_JOB_OK || fi.type != 'P') continue;
        if ((fi.id && !strcmp(fi.id, s)) || (fi.name && !strcmp(fi.name, s)) || (fi.title && !strcmp(fi.title, s))) return fi.pageno;
    }
    return -1;
}

static fz_rect djvu_rect(int x0, int y0, int x1, int y1, const ddjvu_pageinfo_t *info)
{
    float s = 72.0f / info->dpi;
    return fz_make_rect(x0 * s, (info->height - y1) * s, x1 * s, (info->height - y0) * s);
}

static void djvu_walk(miniexp_t e, const ddjvu_pageinfo_t *info, int *line, int depth)
{
    if (!miniexp_consp(e) || depth > 32) return;
    miniexp_t type = miniexp_car(e);
    int c[4];
    for (int i = 0; i < 4; i++) {
        miniexp_t v = miniexp_nth(i + 1, e);
        if (!miniexp_numberp(v)) return;
        c[i] = miniexp_to_int(v);
    }
    miniexp_t rest = miniexp_cdr(miniexp_cdr(miniexp_cdr(miniexp_cdr(miniexp_cdr(e)))));
    if (miniexp_symbolp(type) && !strcmp(miniexp_to_name(type), "line")) (*line)++;
    if (miniexp_stringp(miniexp_car(rest))) {
        if (dtext.count == dtext.cap) {
            int cap = dtext.cap ? dtext.cap * 2 : 256;
            struct djvu_word *w = realloc(dtext.words, (size_t)cap * sizeof *w);
            if (!w) return;
            dtext.words = w;
            dtext.cap = cap;
        }
        char *text = strdup(miniexp_to_str(miniexp_car(rest)));
        if (!text) return;
        dtext.words[dtext.count++] = (struct djvu_word){ djvu_rect(c[0], c[1], c[2], c[3], info), *line, text };
        return;
    }
    for (; miniexp_consp(rest); rest = miniexp_cdr(rest)) djvu_walk(miniexp_car(rest), info, line, depth + 1);
}

/* Page n's words, kept for the requests about it in a row. */
static void djvu_text(int n)
{
    if (dtext.page == n) return;
    ddjvu_pageinfo_t info = djvu_info(n);
    djvu_text_drop();
    miniexp_t e;
    while ((e = ddjvu_document_get_pagetext(djvu_doc, n, "word")) == miniexp_dummy) djvu_messages(1);
    int line = 0;
    djvu_walk(e, &info, &line, 0);
    ddjvu_miniexp_release(djvu_doc, e);
    dtext.page = n;
}

static void djvu_page(int n, struct view_reply *r)
{
    djvu_size(n, &r->pw, &r->ph);
    djvu_text(n);
    if (dtext.count) r->flags |= VIEW_HAS_TEXT;
}

/* The rendered page's size in pixels at zoom and rotate, and the part of it
 * asked for (all of it when w is 0): 0, or the reply's error. */
static int render_area(const struct view_request *q, float pw, float ph, int *W, int *H, fz_irect *area, struct view_reply *r)
{
    if (!(q->zoom > 0) || !isfinite(q->zoom) || pw * q->zoom > 1e6f || ph * q->zoom > 1e6f) {
        fail(r, EINVAL, "Bad zoom");
        return -1;
    }
    if (q->rotate % 90 || q->rotate < 0 || q->rotate >= 360) {
        fail(r, EINVAL, "Bad rotation");
        return -1;
    }
    int w = (int)(pw * q->zoom + 0.5f), h = (int)(ph * q->zoom + 0.5f);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *W = q->rotate % 180 ? h : w;
    *H = q->rotate % 180 ? w : h;
    if (q->w == 0) {
        *area = (fz_irect){ 0, 0, *W, *H };
    } else {
        if (q->x < 0 || q->y < 0 || q->w < 0 || q->h <= 0 || q->x >= *W || q->y >= *H) {
            fail(r, EINVAL, "The area is outside the page");
            return -1;
        }
        *area = (fz_irect){ q->x, q->y, q->x + (q->w < *W - q->x ? q->w : *W - q->x), q->y + (q->h < *H - q->y ? q->h : *H - q->y) };
    }
    if ((uint64_t)(area->x1 - area->x0) * (uint64_t)(area->y1 - area->y0) > VIEW_MAX_PIXELS) {
        fail(r, EFBIG, "More than %u pixels", VIEW_MAX_PIXELS);
        return -1;
    }
    r->w = area->x1 - area->x0;
    r->h = area->y1 - area->y0;
    return 0;
}

static void djvu_render(const struct view_request *q, struct view_reply *r, struct out *o)
{
    float pw, ph;
    int W, H;
    fz_irect area;
    djvu_size(q->page, &pw, &ph);
    if (render_area(q, pw, ph, &W, &H, &area, r) < 0) return;
    ddjvu_page_t *pg = ddjvu_page_create_by_pageno(djvu_doc, q->page);
    if (!pg) fz_throw(ctx, FZ_ERROR_LIBRARY, "cannot decode DjVu page %d", q->page + 1);
    while (!ddjvu_page_decoding_done(pg) && !cookie.abort) djvu_messages(1);
    if (cookie.abort) {
        ddjvu_job_stop(ddjvu_page_job(pg));
        ddjvu_page_release(pg);
        fail(r, ECANCELED, "Cancelled");
        return;
    }
    if (ddjvu_page_decoding_error(pg)) {
        ddjvu_page_release(pg);
        fz_throw(ctx, FZ_ERROR_FORMAT, "broken DjVu page %d", q->page + 1);
    }
    /* djvulibre turns counter-clockwise, from the page's initial rotation. */
    int turn = ((int)ddjvu_page_get_initial_rotation(pg) + 4 - q->rotate / 90) % 4;
    ddjvu_page_set_rotation(pg, (ddjvu_page_rotation_t)turn);
    unsigned masks[3] = { 0xFF0000, 0x00FF00, 0x0000FF };
    ddjvu_format_t *fmt = ddjvu_format_create(DDJVU_FORMAT_RGBMASK32, 3, masks);
    ddjvu_format_set_row_order(fmt, 1);
    ddjvu_format_set_y_direction(fmt, 1);
    ddjvu_rect_t prect = { 0, 0, (unsigned)W, (unsigned)H };
    ddjvu_rect_t rrect = { area.x0, area.y0, (unsigned)r->w, (unsigned)r->h };
    uint32_t *px = NULL;
    fz_try(ctx) {
        px = new_pixels(o, (size_t)r->w * (size_t)r->h * 4);
        if (!ddjvu_page_render(pg, DDJVU_RENDER_COLOR, &prect, &rrect, fmt, (unsigned long)r->w * 4, (char *)px)) {
            memset(px, 0xFF, o->size); /* nothing to draw: a white page */
        }
        for (size_t i = 0; i < o->size / 4; i++) px[i] &= 0xFFFFFF;
    }
    fz_always(ctx) {
        ddjvu_format_release(fmt);
        ddjvu_page_release(pg);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* The page's words as one string (spaces between words, newlines between
 * lines), with each character's word and place in it, for searching. */
struct runes {
    int *cp, *word, *index, n;
};

static void djvu_runes(struct runes *s)
{
    size_t cap = 16;
    for (int i = 0; i < dtext.count; i++) cap += strlen(dtext.words[i].text) + 1;
    s->cp = fz_malloc_array(ctx, cap, int);
    s->word = fz_malloc_array(ctx, cap, int);
    s->index = fz_malloc_array(ctx, cap, int);
    s->n = 0;
    for (int i = 0; i < dtext.count; i++) {
        if (i > 0) {
            s->cp[s->n] = ' ';
            s->word[s->n] = -1;
            s->index[s->n++] = 0;
        }
        int k = 0, c;
        for (const char *t = dtext.words[i].text; *t; k++) {
            t += fz_chartorune(&c, t);
            s->cp[s->n] = fz_tolower(c);
            s->word[s->n] = i;
            s->index[s->n++] = k;
        }
    }
}

static void runes_free(struct runes *s)
{
    fz_free(ctx, s->cp);
    fz_free(ctx, s->word);
    fz_free(ctx, s->index);
}

/* The boxes of characters from to to (a word's box divided evenly among
 * its characters), one a line, appended as view_rects of hit. */
static int djvu_boxes(fz_buffer *out, const struct runes *s, int from, int to, int hit)
{
    int count = 0;
    struct view_rect cur = { 0 };
    int cur_line = -1;
    for (int i = from; i < to; i++) {
        int w = s->word[i];
        if (w < 0) continue;
        const struct djvu_word *wd = &dtext.words[w];
        int len = fz_utflen(wd->text);
        float cw = (wd->box.x1 - wd->box.x0) / (len > 0 ? len : 1);
        fz_rect b = { wd->box.x0 + cw * s->index[i], wd->box.y0, wd->box.x0 + cw * (s->index[i] + 1), wd->box.y1 };
        if (wd->line != cur_line) {
            if (cur_line >= 0) { put(out, &cur, sizeof cur); count++; }
            cur = (struct view_rect){ b.x0, b.y0, b.x1, b.y1, hit };
            cur_line = wd->line;
        } else {
            cur.x0 = fminf(cur.x0, b.x0);
            cur.y0 = fminf(cur.y0, b.y0);
            cur.x1 = fmaxf(cur.x1, b.x1);
            cur.y1 = fmaxf(cur.y1, b.y1);
        }
    }
    if (cur_line >= 0) { put(out, &cur, sizeof cur); count++; }
    return count;
}

static void djvu_search(const struct view_request *q, struct view_reply *r, struct out *o)
{
    djvu_text(q->page);
    int needle[VIEW_TEXT], m = 0, c;
    for (const char *t = q->text; *t && m < VIEW_TEXT;) {
        t += fz_chartorune(&c, t);
        needle[m++] = isspace(c) ? ' ' : fz_tolower(c);
    }
    if (m == 0) return;
    struct runes s = { 0 };
    fz_var(s);
    o->buf = fz_new_buffer(ctx, 256);
    fz_try(ctx) {
        djvu_runes(&s);
        int hit = 0;
        for (int i = 0; i + m <= s.n; i++) {
            if (memcmp(&s.cp[i], needle, (size_t)m * sizeof *needle)) continue;
            r->count += djvu_boxes(o->buf, &s, i, i + m, hit++);
            i += m - 1;
        }
    }
    fz_always(ctx) runes_free(&s);
    fz_catch(ctx) fz_rethrow(ctx);
}

static int djvu_nearest(float x, float y)
{
    int best = -1;
    float bd = INFINITY;
    for (int i = 0; i < dtext.count; i++) {
        fz_rect b = dtext.words[i].box;
        float dx = x < b.x0 ? b.x0 - x : x > b.x1 ? x - b.x1 : 0;
        float dy = y < b.y0 ? b.y0 - y : y > b.y1 ? y - b.y1 : 0;
        if (dx * dx + dy * dy < bd) {
            bd = dx * dx + dy * dy;
            best = i;
        }
    }
    return best;
}

/* DjVu has no character boxes: a selection is of whole words (and of whole
 * lines in VIEW_SELECT_LINES). */
static void djvu_select(const struct view_request *q, struct view_reply *r, struct out *o)
{
    djvu_text(q->page);
    o->buf = fz_new_buffer(ctx, 256);
    int a = djvu_nearest(q->ax, q->ay), b = djvu_nearest(q->bx, q->by);
    if (a < 0 || b < 0) return;
    if (a > b) { int t = a; a = b; b = t; }
    if (q->mode == VIEW_SELECT_LINES) {
        while (a > 0 && dtext.words[a - 1].line == dtext.words[a].line) a--;
        while (b + 1 < dtext.count && dtext.words[b + 1].line == dtext.words[b].line) b++;
    }
    struct runes s = { 0 };
    fz_var(s);
    fz_buffer *text = fz_new_buffer(ctx, 256);
    fz_try(ctx) {
        djvu_runes(&s);
        int from = 0, to = 0;
        while (from < s.n && s.word[from] != a) from++;
        for (to = from; to < s.n && (s.word[to] < 0 || s.word[to] <= b); to++) {}
        r->count = djvu_boxes(o->buf, &s, from, to, 0);
        for (int i = a; i <= b; i++) {
            if (i > a) fz_append_byte(ctx, text, dtext.words[i].line != dtext.words[i - 1].line ? '\n' : ' ');
            fz_append_string(ctx, text, dtext.words[i].text);
        }
        unsigned char *t;
        size_t n = fz_buffer_storage(ctx, text, &t);
        put(o->buf, t, n);
        r->w = (int)n;
    }
    fz_always(ctx) {
        runes_free(&s);
        fz_drop_buffer(ctx, text);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* VIEW_WORDS: a word with a point in (ax, ay)-(bx, by) (page points). */
static int word_in(const struct view_request *q, fz_rect b)
{
    return b.x0 < fmaxf(q->ax, q->bx) && b.x1 > fminf(q->ax, q->bx) && b.y0 < fmaxf(q->ay, q->by) && b.y1 > fminf(q->ay, q->by);
}

static void words_reply(fz_buffer *boxes, fz_buffer *texts, struct view_reply *r, struct out *o)
{
    unsigned char *s;
    size_t n = fz_buffer_storage(ctx, boxes, &s);
    o->buf = fz_new_buffer(ctx, n + 16);
    put(o->buf, s, n);
    r->count = (int)(n / sizeof(struct view_rect));
    n = fz_buffer_storage(ctx, texts, &s);
    put(o->buf, s, n);
    r->w = (int)n;
}

static void djvu_words(const struct view_request *q, struct view_reply *r, struct out *o)
{
    djvu_text(q->page);
    fz_buffer *boxes = fz_new_buffer(ctx, 256), *texts = NULL;
    fz_var(texts);
    fz_try(ctx) {
        texts = fz_new_buffer(ctx, 256);
        for (int i = 0; i < dtext.count; i++) {
            const struct djvu_word *w = &dtext.words[i];
            if (!word_in(q, w->box)) continue;
            struct view_rect v = { w->box.x0, w->box.y0, w->box.x1, w->box.y1, w->line };
            put(boxes, &v, sizeof v);
            fz_append_string(ctx, texts, w->text);
            fz_append_byte(ctx, texts, 0);
        }
        words_reply(boxes, texts, r, o);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, boxes);
        fz_drop_buffer(ctx, texts);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static void djvu_links(const struct view_request *q, struct view_reply *r, struct out *o)
{
    ddjvu_pageinfo_t info = djvu_info(q->page);
    miniexp_t anno;
    while ((anno = ddjvu_document_get_pageanno(djvu_doc, q->page)) == miniexp_dummy) djvu_messages(1);
    miniexp_t *links = ddjvu_anno_get_hyperlinks(anno);
    o->buf = fz_new_buffer(ctx, 256);
    fz_try(ctx) {
        for (int i = 0; links && links[i]; i++) {
            /* (maparea URL COMMENT AREA ...); URL is "href" or (url "href" "target"). */
            miniexp_t url = miniexp_nth(1, links[i]), area = miniexp_nth(3, links[i]);
            if (miniexp_consp(url)) url = miniexp_nth(1, url);
            if (!miniexp_stringp(url) || !miniexp_consp(area)) continue;
            int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN, k = 0;
            const char *shape = miniexp_symbolp(miniexp_car(area)) ? miniexp_to_name(miniexp_car(area)) : "";
            int v[64];
            for (miniexp_t p = miniexp_cdr(area); miniexp_consp(p) && k < 64; p = miniexp_cdr(p)) {
                if (miniexp_numberp(miniexp_car(p))) v[k++] = miniexp_to_int(miniexp_car(p));
            }
            if (!strcmp(shape, "poly") || !strcmp(shape, "line")) {
                for (int j = 0; j + 1 < k; j += 2) {
                    x0 = v[j] < x0 ? v[j] : x0;
                    x1 = v[j] > x1 ? v[j] : x1;
                    y0 = v[j + 1] < y0 ? v[j + 1] : y0;
                    y1 = v[j + 1] > y1 ? v[j + 1] : y1;
                }
            } else if (k >= 4) {
                x0 = v[0], y0 = v[1], x1 = v[0] + v[2], y1 = v[1] + v[3];
            }
            if (x0 > x1 || y0 > y1) continue;
            fz_rect b = djvu_rect(x0, y0, x1, y1, &info);
            struct view_link l = { b.x0, b.y0, b.x1, b.y1, -1, 0, 0, "" };
            const char *href = miniexp_to_str(url);
            snprintf(l.uri, sizeof l.uri, "%s", href);
            if (href[0] == '#' && (l.page = djvu_target(href, q->page)) < 0) continue;
            put(o->buf, &l, sizeof l);
            r->count++;
        }
    }
    fz_always(ctx) {
        free(links);
        ddjvu_miniexp_release(djvu_doc, anno);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static void djvu_outline_walk(fz_buffer *out, miniexp_t items, int level, int *count)
{
    for (; miniexp_consp(items) && level < 64; items = miniexp_cdr(items)) {
        /* ("title" "#page" children ...) */
        miniexp_t item = miniexp_car(items);
        if (!miniexp_consp(item) || !miniexp_stringp(miniexp_car(item))) continue;
        miniexp_t url = miniexp_nth(1, item);
        struct view_outline v = { level, -1, 0, "" };
        snprintf(v.title, sizeof v.title, "%s", miniexp_to_str(miniexp_car(item)));
        if (miniexp_stringp(url)) v.page = djvu_target(miniexp_to_str(url), 0);
        put(out, &v, sizeof v);
        (*count)++;
        djvu_outline_walk(out, miniexp_cddr(item), level + 1, count);
    }
}

static miniexp_t djvu_outline_exp(void)
{
    miniexp_t e;
    while ((e = ddjvu_document_get_outline(djvu_doc)) == miniexp_dummy) djvu_messages(1);
    return e;
}

static void djvu_outline(struct view_reply *r, struct out *o)
{
    miniexp_t e = djvu_outline_exp();
    o->buf = fz_new_buffer(ctx, 256);
    fz_try(ctx) {
        if (miniexp_consp(e) && miniexp_symbolp(miniexp_car(e)) && !strcmp(miniexp_to_name(miniexp_car(e)), "bookmarks")) {
            djvu_outline_walk(o->buf, miniexp_cdr(e), 0, &r->count);
        }
    }
    fz_always(ctx) ddjvu_miniexp_release(djvu_doc, e);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void djvu_info_lines(fz_buffer *b)
{
    static const char *const keys[][2] = {
        { "title", "Title" }, { "author", "Author" }, { "subject", "Subject" }, { "keywords", "Keywords" },
        { "creator", "Creator" }, { "producer", "Producer" }, { "creationdate", "Created" }, { "moddate", "Modified" },
    };
    miniexp_t anno;
    while ((anno = ddjvu_document_get_anno(djvu_doc, 1)) == miniexp_dummy) djvu_messages(1);
    miniexp_t *names = ddjvu_anno_get_metadata_keys(anno);
    for (size_t k = 0; k < sizeof keys / sizeof *keys; k++) {
        for (int i = 0; names && names[i]; i++) {
            if (strcasecmp(miniexp_to_name(names[i]), keys[k][0])) continue;
            put_info(b, keys[k][1], ddjvu_anno_get_metadata(anno, names[i]));
        }
    }
    free(names);
    ddjvu_miniexp_release(djvu_doc, anno);
    ddjvu_pageinfo_t info = djvu_info(0);
    fz_append_printf(ctx, b, "Resolution\t%d dpi\n", info.dpi);
}

static void djvu_export_page(fz_buffer *b, int n)
{
    djvu_text(n);
    for (int i = 0; i < dtext.count; i++) {
        if (i > 0) fz_append_byte(ctx, b, dtext.words[i].line != dtext.words[i - 1].line ? '\n' : ' ');
        fz_append_string(ctx, b, dtext.words[i].text);
    }
    if (dtext.count) fz_append_byte(ctx, b, '\n');
}

static int djvu_has_outline(void)
{
    miniexp_t e = djvu_outline_exp();
    int has = miniexp_consp(e) && miniexp_consp(miniexp_cdr(e));
    ddjvu_miniexp_release(djvu_doc, e);
    return has;
}

/* PostScript and EPS, by libspectre (Ghostscript) as Okular's spectre
 * generator: pages drawn, no text. */
static SpectreDocument *ps;

static void ps_close(void)
{
    if (ps) spectre_document_free(ps);
    ps = NULL;
}

static int ps_open(const char *path, struct view_reply *r)
{
    ps = spectre_document_new();
    spectre_document_load(ps, path);
    SpectreStatus s = spectre_document_status(ps);
    if (s != SPECTRE_STATUS_SUCCESS) {
        fail(r, EINVAL, "PostScript: %s", spectre_status_to_string(s));
        return -1;
    }
    if ((npages = (int)spectre_document_get_n_pages(ps)) <= 0) {
        fail(r, EINVAL, "PostScript without pages");
        return -1;
    }
    return 0;
}

static SpectrePage *ps_page(int n)
{
    SpectrePage *p = n >= 0 && n < npages ? spectre_document_get_page(ps, (unsigned)n) : NULL;
    if (!p) fz_throw(ctx, FZ_ERROR_ARGUMENT, "no page %d", n);
    return p;
}

static void ps_size(int n, float *pw, float *ph)
{
    SpectrePage *p = ps_page(n);
    int w = 0, h = 0;
    spectre_page_get_size(p, &w, &h);
    spectre_page_free(p);
    *pw = w > 0 ? (float)w : 612;
    *ph = h > 0 ? (float)h : 792;
}

/* libspectre draws the part asked for of the unturned page; it is turned
 * here (its own rotation does not draw parts of turned pages right). */
static void ps_render(const struct view_request *q, struct view_reply *r, struct out *o)
{
    float pw, ph;
    int W, H;
    fz_irect a, u;
    ps_size(q->page, &pw, &ph);
    if (render_area(q, pw, ph, &W, &H, &a, r) < 0) return;
    int turned = q->rotate % 180 != 0, uw = turned ? H : W, uh = turned ? W : H;
    switch (q->rotate) {
    case 90: u = (fz_irect){ a.y0, uh - a.x1, a.y1, uh - a.x0 }; break;
    case 180: u = (fz_irect){ uw - a.x1, uh - a.y1, uw - a.x0, uh - a.y0 }; break;
    case 270: u = (fz_irect){ uw - a.y1, a.x0, uw - a.y0, a.x1 }; break;
    default: u = a;
    }
    SpectrePage *p = ps_page(q->page);
    SpectreRenderContext *rc = spectre_render_context_new();
    unsigned char *data = NULL;
    int stride = 0;
    spectre_render_context_set_scale(rc, uw / pw, uh / ph);
    spectre_page_render_slice(p, rc, u.x0, u.y0, u.x1 - u.x0, u.y1 - u.y0, &data, &stride);
    SpectreStatus s = spectre_page_status(p);
    spectre_render_context_free(rc);
    spectre_page_free(p);
    fz_try(ctx) {
        if (s != SPECTRE_STATUS_SUCCESS || !data || stride < (u.x1 - u.x0) * 4) {
            fz_throw(ctx, FZ_ERROR_LIBRARY, "PostScript: %s", s ? spectre_status_to_string(s) : "nothing drawn");
        }
        uint32_t *px = new_pixels(o, (size_t)r->w * (size_t)r->h * 4);
        for (int Y = a.y0; Y < a.y1; Y++) {
            for (int X = a.x0; X < a.x1; X++) {
                int x = q->rotate == 90 ? Y : q->rotate == 180 ? uw - 1 - X : q->rotate == 270 ? uw - 1 - Y : X;
                int y = q->rotate == 90 ? uh - 1 - X : q->rotate == 180 ? uh - 1 - Y : q->rotate == 270 ? X : Y;
                const uint32_t *row = (const uint32_t *)(data + (size_t)(y - u.y0) * (size_t)stride);
                px[(size_t)(Y - a.y0) * r->w + (X - a.x0)] = row[x - u.x0] & 0xFFFFFF;
            }
        }
    }
    fz_always(ctx) free(data);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void ps_info_lines(fz_buffer *b)
{
    put_info(b, "Title", spectre_document_get_title(ps));
    put_info(b, "Author", spectre_document_get_for(ps));
    put_info(b, "Creator", spectre_document_get_creator(ps));
    put_info(b, "Created", spectre_document_get_creation_date(ps));
    put_info(b, "Document type", spectre_document_get_format(ps));
    if (spectre_document_get_language_level(ps) > 0) {
        fz_append_printf(ctx, b, "Language level\t%d\n", spectre_document_get_language_level(ps));
    }
}

/* MuPDF documents. Coordinates are moved so the page's top left is (0, 0). */
static void drop_page(void)
{
    fz_drop_stext_page(ctx, page_text);
    fz_drop_page(ctx, page);
    page_text = NULL;
    page = NULL;
    page_no = -1;
}

/* Page n, loaded once for the requests about it in a row. */
static void load_page(int n)
{
    if (n < 0 || n >= npages) fz_throw(ctx, FZ_ERROR_ARGUMENT, "no page %d", n);
    if (n == page_no) return;
    drop_page();
    page = fz_load_page(ctx, doc, n);
    page_no = n;
    page_box = fz_bound_page(ctx, page);
}

/* Its text, made with the cookie SIGUSR1 aborts. */
static fz_stext_page *load_text(int n)
{
    load_page(n);
    if (page_text) return page_text;
    fz_stext_options opts;
    fz_init_stext_options(ctx, &opts);
    fz_stext_page *t = fz_new_stext_page(ctx, page_box);
    fz_device *dev = NULL;
    fz_var(dev);
    fz_try(ctx) {
        dev = fz_new_stext_device(ctx, t, &opts);
        fz_run_page(ctx, page, dev, fz_identity, &cookie);
        fz_close_device(ctx, dev);
        if (cookie.abort) fz_throw(ctx, FZ_ERROR_ABORT, "cancelled");
    }
    fz_always(ctx) fz_drop_device(ctx, dev);
    fz_catch(ctx) {
        fz_drop_stext_page(ctx, t);
        fz_rethrow(ctx);
    }
    page_text = t;
    return t;
}

static int stext_has_chars(fz_stext_page *t)
{
    for (fz_stext_block *b = t->first_block; b; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line *l = b->u.t.first_line; l; l = l->next) if (l->first_char) return 1;
    }
    return 0;
}

/* Page n's top left in MuPDF's coordinates (0, 0 if it cannot be loaded). */
static fz_point origin(int n)
{
    if (n == page_no) return fz_make_point(page_box.x0, page_box.y0);
    fz_point o = { 0, 0 };
    fz_page *p = NULL;
    fz_var(p);
    fz_try(ctx) {
        p = fz_load_page(ctx, doc, n);
        fz_rect b = fz_bound_page(ctx, p);
        o = fz_make_point(b.x0, b.y0);
    }
    fz_always(ctx) fz_drop_page(ctx, p);
    fz_catch(ctx) fz_report_error(ctx);
    return o;
}

static float or_zero(float v)
{
    return isfinite(v) ? v : 0;
}

/* An internal link's page and place on it: the page, or -1. */
static int resolve(const char *uri, float *x, float *y)
{
    fz_link_dest d = fz_resolve_link_dest(ctx, doc, uri);
    int n = d.loc.chapter < 0 ? -1 : fz_page_number_from_location(ctx, doc, d.loc);
    if (n < 0 || n >= npages) return -1;
    fz_point o = origin(n);
    *x = or_zero(d.x) ? or_zero(d.x) - o.x : 0;
    *y = or_zero(d.y) ? or_zero(d.y) - o.y : 0;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    return n;
}

static void mupdf_render(const struct view_request *q, struct view_reply *r, struct out *o)
{
    load_page(q->page);
    float pw = page_box.x1 - page_box.x0, ph = page_box.y1 - page_box.y0;
    int W, H;
    fz_irect area;
    if (render_area(q, pw, ph, &W, &H, &area, r) < 0) return;
    /* Page to pixels: scaled, turned clockwise, its top left at (0, 0). */
    fz_matrix m = fz_concat(fz_translate(-page_box.x0, -page_box.y0), fz_scale(q->zoom, q->zoom));
    m = fz_concat(m, fz_rotate((float)q->rotate));
    fz_rect turned = fz_transform_rect(fz_make_rect(0, 0, pw, ph), fz_scale(q->zoom, q->zoom));
    turned = fz_transform_rect(turned, fz_rotate((float)q->rotate));
    m = fz_concat(m, fz_translate(-turned.x0, -turned.y0));
    unsigned char *px = new_pixels(o, (size_t)r->w * (size_t)r->h * 4);
    fz_pixmap *pix = fz_new_pixmap_with_bbox_and_data(ctx, fz_device_bgr(ctx), area, NULL, 1, px);
    fz_device *dev = NULL;
    fz_var(dev);
    fz_try(ctx) {
        fz_clear_pixmap_with_value(ctx, pix, 0xFF);
        if (q->paper & VIEW_PAPER) {
            /* Okular's paper colour: the page drawn on it (poppler's setPaperColor). */
            for (size_t i = 0; i + 3 < o->size; i += 4) {
                px[i] = (unsigned char)q->paper;
                px[i + 1] = (unsigned char)(q->paper >> 8);
                px[i + 2] = (unsigned char)(q->paper >> 16);
            }
        }
        dev = fz_new_draw_device(ctx, fz_identity, pix);
        fz_run_page(ctx, page, dev, m, &cookie);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
        fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    if (cookie.abort) {
        fail(r, ECANCELED, "Cancelled");
        return;
    }
    /* BGRA bytes are 0xAARRGGBB words: clear the A. */
    for (size_t i = 3; i < o->size; i += 4) px[i] = 0;
}

struct hits {
    fz_buffer *buf;
    fz_point o;
    int hit, count;
};

static int on_hit(fz_context *c, void *opaque, int n, fz_quad *quads)
{
    struct hits *h = opaque;
    for (int i = 0; i < n; i++) {
        fz_rect b = fz_rect_from_quad(quads[i]);
        struct view_rect v = { b.x0 - h->o.x, b.y0 - h->o.y, b.x1 - h->o.x, b.y1 - h->o.y, h->hit };
        fz_append_data(c, h->buf, &v, sizeof v);
        h->count++;
    }
    h->hit++;
    return 0;
}

static void mupdf_search(const struct view_request *q, struct view_reply *r, struct out *o)
{
    fz_stext_page *t = load_text(q->page);
    o->buf = fz_new_buffer(ctx, 256);
    struct hits h = { o->buf, { page_box.x0, page_box.y0 }, 0, 0 };
    if (q->text[0]) fz_search_stext_page_cb(ctx, t, q->text, on_hit, &h);
    r->count = h.count;
}

static void mupdf_select(const struct view_request *q, struct view_reply *r, struct out *o)
{
    static const int modes[] = { FZ_SELECT_CHARS, FZ_SELECT_WORDS, FZ_SELECT_LINES };
    fz_stext_page *t = load_text(q->page);
    fz_point a = { q->ax + page_box.x0, q->ay + page_box.y0 }, b = { q->bx + page_box.x0, q->by + page_box.y0 };
    int mode = q->mode >= 0 && q->mode <= VIEW_SELECT_LINES ? modes[q->mode] : FZ_SELECT_CHARS;
    fz_quad h = fz_snap_selection(ctx, t, &a, &b, mode);
    /* From the middles of the snapped ends: MuPDF ends a selection at a
     * line's last character on its lower corner, which a closely set next
     * line (reflowed text) would then claim. */
    a = fz_make_point((h.ul.x + h.ll.x) / 2, (h.ul.y + h.ll.y) / 2);
    b = fz_make_point((h.ur.x + h.lr.x) / 2, (h.ur.y + h.lr.y) / 2);
    fz_quad *quads = fz_malloc_array(ctx, MAX_QUADS, fz_quad);
    char *text = NULL;
    fz_var(text);
    fz_try(ctx) {
        int n = fz_highlight_selection(ctx, t, a, b, quads, MAX_QUADS);
        o->buf = fz_new_buffer(ctx, (size_t)n * sizeof(struct view_rect) + 256);
        for (int i = 0; i < n; i++) {
            fz_rect box = fz_rect_from_quad(quads[i]);
            struct view_rect v = { box.x0 - page_box.x0, box.y0 - page_box.y0, box.x1 - page_box.x0, box.y1 - page_box.y0, 0 };
            put(o->buf, &v, sizeof v);
        }
        text = fz_copy_selection(ctx, t, a, b, 0);
        put(o->buf, text, strlen(text));
        r->count = n;
        r->w = (int)strlen(text);
    }
    fz_always(ctx) {
        fz_free(ctx, quads);
        fz_free(ctx, text);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* The page's words: its characters between white space, on a line. */
static void mupdf_words(const struct view_request *q, struct view_reply *r, struct out *o)
{
    fz_stext_page *t = load_text(q->page);
    fz_buffer *boxes = fz_new_buffer(ctx, 256), *texts = NULL, *word = NULL;
    fz_var(texts);
    fz_var(word);
    fz_try(ctx) {
        texts = fz_new_buffer(ctx, 256);
        word = fz_new_buffer(ctx, 64);
        int line = 0;
        for (fz_stext_block *b = t->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line *l = b->u.t.first_line; l; l = l->next, line++) {
                fz_rect box = fz_empty_rect;
                for (fz_stext_char *c = l->first_char;; c = c->next) {
                    int space = !c || c->c == ' ' || c->c == '\t' || c->c == 0xA0 || c->c == 0x3000;
                    if (space && !fz_is_empty_rect(box)) {
                        fz_rect at = fz_translate_rect(box, -page_box.x0, -page_box.y0);
                        if (word_in(q, at)) {
                            struct view_rect v = { at.x0, at.y0, at.x1, at.y1, line };
                            unsigned char *s;
                            size_t n = fz_buffer_storage(ctx, word, &s);
                            put(boxes, &v, sizeof v);
                            put(texts, s, n);
                            fz_append_byte(ctx, texts, 0);
                        }
                        fz_clear_buffer(ctx, word);
                        box = fz_empty_rect;
                    }
                    if (!c) break;
                    if (space) continue;
                    box = fz_union_rect(box, fz_rect_from_quad(c->quad));
                    fz_append_rune(ctx, word, c->c);
                }
            }
        }
        words_reply(boxes, texts, r, o);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, boxes);
        fz_drop_buffer(ctx, texts);
        fz_drop_buffer(ctx, word);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static void mupdf_links(const struct view_request *q, struct view_reply *r, struct out *o)
{
    load_page(q->page);
    fz_link *links = fz_load_links(ctx, page);
    fz_try(ctx) {
        o->buf = fz_new_buffer(ctx, 256);
        for (fz_link *l = links; l; l = l->next) {
            if (!l->uri) continue;
            struct view_link v = { l->rect.x0 - page_box.x0, l->rect.y0 - page_box.y0,
                                   l->rect.x1 - page_box.x0, l->rect.y1 - page_box.y0, -1, 0, 0, "" };
            copy_text(v.uri, sizeof v.uri, l->uri);
            if (!fz_is_external_link(ctx, l->uri) && (v.page = resolve(l->uri, &v.x, &v.y)) < 0) continue;
            put(o->buf, &v, sizeof v);
            r->count++;
        }
    }
    fz_always(ctx) fz_drop_link(ctx, links);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void outline_walk(fz_buffer *out, fz_outline *item, int level, int *count)
{
    for (; item && level < 64; item = item->next) {
        struct view_outline v = { level, -1, 0, "" };
        copy_text(v.title, sizeof v.title, item->title);
        if (item->uri && !fz_is_external_link(ctx, item->uri)) {
            float x;
            v.page = resolve(item->uri, &x, &v.y);
        } else if (!item->uri && item->page.chapter >= 0) {
            v.page = fz_page_number_from_location(ctx, doc, item->page);
            v.y = or_zero(item->y) - origin(v.page).y;
        }
        put(out, &v, sizeof v);
        (*count)++;
        outline_walk(out, item->down, level + 1, count);
    }
}

static void mupdf_outline(struct view_reply *r, struct out *o)
{
    fz_outline *ol = fz_load_outline(ctx, doc);
    fz_try(ctx) {
        o->buf = fz_new_buffer(ctx, 256);
        outline_walk(o->buf, ol, 0, &r->count);
    }
    fz_always(ctx) fz_drop_outline(ctx, ol);
    fz_catch(ctx) fz_rethrow(ctx);
}

/* PDF fonts, as Okular's Properties > Fonts lists them (poppler's
 * FontInfoScanner): those of every page's resources and of the forms they
 * use, each once. */
struct seen {
    int *num, n, cap;
};

static int seen_before(struct seen *s, pdf_obj *obj)
{
    int num = pdf_to_num(ctx, obj);
    if (!num) return 0; /* a direct object: cannot be shared */
    for (int i = 0; i < s->n; i++) if (s->num[i] == num) return 1;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->num = fz_realloc_array(ctx, s->num, s->cap, int);
    }
    s->num[s->n++] = num;
    return 0;
}

static void put_font(fz_buffer *b, pdf_obj *font)
{
    pdf_obj *subtype = pdf_dict_get(ctx, font, PDF_NAME(Subtype)), *desc_font = font;
    const char *name = pdf_to_name(ctx, pdf_dict_get(ctx, font, PDF_NAME(BaseFont))), *type = "Unknown";
    if (pdf_name_eq(ctx, subtype, PDF_NAME(Type0))) {
        desc_font = pdf_array_get(ctx, pdf_dict_get(ctx, font, PDF_NAME(DescendantFonts)), 0);
        type = pdf_name_eq(ctx, pdf_dict_get(ctx, desc_font, PDF_NAME(Subtype)), PDF_NAME(CIDFontType2)) ? "CID TrueType" : "CID Type 0";
    } else if (pdf_name_eq(ctx, subtype, PDF_NAME(Type1)) || pdf_name_eq(ctx, subtype, PDF_NAME(MMType1))) {
        type = "Type 1";
    } else if (pdf_name_eq(ctx, subtype, PDF_NAME(TrueType))) {
        type = "TrueType";
    } else if (pdf_name_eq(ctx, subtype, PDF_NAME(Type3))) {
        type = "Type 3";
    }
    pdf_obj *desc = pdf_dict_get(ctx, desc_font, PDF_NAME(FontDescriptor));
    pdf_obj *file3 = pdf_dict_get(ctx, desc, PDF_NAME(FontFile3));
    int embedded = pdf_dict_get(ctx, desc, PDF_NAME(FontFile)) || pdf_dict_get(ctx, desc, PDF_NAME(FontFile2)) || file3;
    const char *f3 = pdf_to_name(ctx, pdf_dict_get(ctx, file3, PDF_NAME(Subtype)));
    char kind_[32];
    snprintf(kind_, sizeof kind_, "%s%s", type, !strcmp(f3, "Type1C") || !strcmp(f3, "CIDFontType0C") ? "C" : !strcmp(f3, "OpenType") ? " (OpenType)" : "");
    /* "ABCDEF+Name" is a subset. */
    int subset = strlen(name) > 7 && name[6] == '+';
    for (int i = 0; subset && i < 6; i++) subset = name[i] >= 'A' && name[i] <= 'Z';
    if (pdf_name_eq(ctx, subtype, PDF_NAME(Type3)) && !*name) name = "[Type 3 font]";
    fz_append_printf(ctx, b, "Font\t%s (%s, %s)\n", *name ? name + (subset ? 7 : 0) : "[none]", kind_,
                     embedded ? subset ? "embedded subset" : "embedded" : "not embedded");
}

static void scan_resources(fz_buffer *b, pdf_obj *res, struct seen *s, int depth)
{
    if (!res || depth > 8) return;
    pdf_obj *fonts = pdf_dict_get(ctx, res, PDF_NAME(Font));
    for (int i = 0, n = pdf_dict_len(ctx, fonts); i < n; i++) {
        pdf_obj *f = pdf_dict_get_val(ctx, fonts, i);
        if (pdf_is_dict(ctx, f) && !seen_before(s, f)) put_font(b, f);
    }
    pdf_obj *xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    for (int i = 0, n = pdf_dict_len(ctx, xobjs); i < n; i++) {
        pdf_obj *x = pdf_dict_get_val(ctx, xobjs, i);
        if (pdf_name_eq(ctx, pdf_dict_get(ctx, x, PDF_NAME(Subtype)), PDF_NAME(Form)) && !seen_before(s, x)) {
            scan_resources(b, pdf_dict_get(ctx, x, PDF_NAME(Resources)), s, depth + 1);
        }
    }
}

static void pdf_fonts(fz_buffer *b, pdf_document *pdf)
{
    struct seen s = { 0 };
    fz_var(s);
    fz_try(ctx) {
        for (int i = 0; i < npages; i++) {
            pdf_obj *p = pdf_lookup_page_obj(ctx, pdf, i);
            pdf_obj *res = pdf_dict_get_inheritable(ctx, p, PDF_NAME(Resources));
            if (!seen_before(&s, res)) scan_resources(b, res, &s, 0);
        }
    }
    fz_always(ctx) fz_free(ctx, s.num);
    fz_catch(ctx) fz_rethrow(ctx);
}

/* "D:20240102030405+01'00'" as "2024-01-02 03:04:05 +01:00". */
static void put_date(fz_buffer *b, const char *key, const char *d)
{
    if (!strncmp(d, "D:", 2)) d += 2;
    int digits = 0;
    while (isdigit((unsigned char)d[digits])) digits++;
    if (digits < 8) {
        put_info(b, key, d);
        return;
    }
    char out[64];
    snprintf(out, sizeof out, "%.4s-%.2s-%.2s", d, d + 4, d + 6);
    if (digits >= 12) snprintf(out + 10, sizeof out - 10, " %.2s:%.2s:%.2s", d + 8, d + 10, digits >= 14 ? d + 12 : "00");
    const char *z = d + digits;
    if (*z == 'Z') strcat(out, " UTC");
    else if ((*z == '+' || *z == '-') && isdigit((unsigned char)z[1]) && isdigit((unsigned char)z[2])) {
        size_t l = strlen(out);
        snprintf(out + l, sizeof out - l, " %c%.2s:%.2s", *z, z + 1, isdigit((unsigned char)z[4]) ? z + 4 : "00");
    }
    put_info(b, key, out);
}

static void mupdf_info_lines(fz_buffer *b)
{
    static const char *const keys[][2] = {
        { "Title", FZ_META_INFO_TITLE }, { "Author", FZ_META_INFO_AUTHOR }, { "Subject", FZ_META_INFO_SUBJECT },
        { "Keywords", FZ_META_INFO_KEYWORDS }, { "Creator", FZ_META_INFO_CREATOR }, { "Producer", FZ_META_INFO_PRODUCER },
        { "Created", FZ_META_INFO_CREATIONDATE }, { "Modified", FZ_META_INFO_MODIFICATIONDATE },
        { "Encryption", FZ_META_ENCRYPTION },
    };
    char v[1024];
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        if (fz_lookup_metadata(ctx, doc, keys[i][1], v, sizeof v) <= 0 || !v[0]) continue;
        if (!strcmp(keys[i][0], "Created") || !strcmp(keys[i][0], "Modified")) put_date(b, keys[i][0], v);
        else put_info(b, keys[i][0], v);
    }
    if (fz_lookup_metadata(ctx, doc, FZ_META_FORMAT, v, sizeof v) > 0 && strcmp(v, format)) put_info(b, "Version", v);
}

/* A PDF's embedded files (the EmbeddedFiles name tree, as Okular lists
 * them), the file spec of index, or their count when index is -1. */
static pdf_obj *embedded(int index, int *count, fz_buffer *list)
{
    pdf_document *pdf = pdf_specifics(ctx, doc);
    pdf_obj *found = NULL;
    fz_var(found);
    *count = 0;
    if (!pdf) return NULL;
    pdf_obj *tree = pdf_load_name_tree(ctx, pdf, PDF_NAME(EmbeddedFiles));
    fz_try(ctx) {
        for (int i = 0, n = pdf_dict_len(ctx, tree); i < n; i++) {
            pdf_obj *fs = pdf_dict_get_val(ctx, tree, i);
            if (!pdf_is_embedded_file(ctx, fs)) continue;
            if (list) {
                pdf_filespec_params p;
                pdf_get_filespec_params(ctx, fs, &p);
                long long size = p.size;
                if (size < 0) {
                    fz_buffer *c = pdf_load_embedded_file_contents(ctx, fs);
                    size = (long long)fz_buffer_storage(ctx, c, NULL);
                    fz_drop_buffer(ctx, c);
                }
                char name[512], line[600];
                copy_text(name, sizeof name, p.filename ? p.filename : "");
                snprintf(line, sizeof line, "%d\t%s\t%lld\n", *count, name, size);
                fz_append_string(ctx, list, line);
            }
            if (*count == index) found = pdf_keep_obj(ctx, fs);
            (*count)++;
        }
    }
    fz_always(ctx) pdf_drop_obj(ctx, tree);
    fz_catch(ctx) fz_rethrow(ctx);
    return found;
}

static void mupdf_save(const struct view_request *q, struct view_reply *r)
{
    int count;
    pdf_obj *fs = embedded(q->page, &count, NULL);
    if (!fs) {
        fail(r, ENOENT, "No embedded file %d", q->page);
        return;
    }
    fz_buffer *c = NULL;
    fz_var(c);
    fz_try(ctx) {
        c = pdf_load_embedded_file_contents(ctx, fs);
        unsigned char *data;
        size_t n = fz_buffer_storage(ctx, c, &data);
        int fd = open(q->text, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0666);
        if (fd < 0) throw_errno(errno, q->text);
        for (size_t done = 0; done < n;) {
            ssize_t w = write(fd, data + done, n - done);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) {
                int err = w < 0 ? errno : EIO;
                close(fd);
                throw_errno(err, q->text);
            }
            done += (size_t)w;
        }
        if (close(fd) < 0) throw_errno(errno, q->text);
        r->size = 0;
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, c);
        pdf_drop_obj(ctx, fs);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* PDF interaction (view-proto.h's VIEW_PDF_*): annotations, form fields,
 * signatures and saving, by view-pdf.c on MuPDF's journal, for a document
 * opened as a PDF (not a DVI made into one, nor one unpacked from gzip,
 * whose file is not a PDF to save over). The session is attached at the
 * first such request, after any password, and kept with that document.
 * Nothing in a request is trusted: every field it uses is checked first. A
 * page edited is dropped, so the next RENDER draws it anew. */
#include "view-pdf.c"

static view_pdf *vp_session;
static fz_document *vp_session_doc; /* kept by the session: not reused while attached */
static int vp_signatures, vp_signed; /* signature fields, and those signed */

static void vp_detach(void)
{
    view_pdf_drop(vp_session);
    vp_session = NULL;
    vp_session_doc = NULL;
    vp_signatures = vp_signed = 0;
}

static view_pdf *vp_attached(struct view_reply *r)
{
    if (kind != MUPDF || !doc || locked || strcmp(format, "PDF") != 0 || !pdf_specifics(ctx, doc)) {
        fail(r, ENOTSUP, "%s documents cannot be annotated or filled in", format[0] ? format : "These");
        return NULL;
    }
    if (data_fd >= 0) {
        fail(r, ENOTSUP, "A compressed PDF cannot be annotated or filled in");
        return NULL;
    }
    if (vp_session && vp_session_doc != doc) vp_detach();
    if (vp_session) return vp_session;
    vp_session = view_pdf_attach(ctx, doc);
    vp_session_doc = doc;
    view_pdf_signatures s = { 0 };
    fz_var(s);
    fz_try(ctx) {
        s = view_pdf_list_signatures(vp_session, NULL);
        vp_signatures = s.count;
        for (int i = 0; i < s.count; i++) vp_signed += s.items[i].signed_value != 0;
    }
    fz_always(ctx) view_pdf_drop_signatures(ctx, &s);
    fz_catch(ctx) {
        vp_detach();
        fz_rethrow(ctx);
    }
    return vp_session;
}

static void vp_status(view_pdf *p, struct view_reply *r)
{
    view_pdf_status s = view_pdf_get_status(p);
    pdf_document *pdf = pdf_specifics(ctx, doc);
    r->flags |= (s.dirty ? VIEW_PDF_DIRTY : 0) | (s.can_undo ? VIEW_PDF_CAN_UNDO : 0) |
                (s.can_redo ? VIEW_PDF_CAN_REDO : 0) | (s.can_annotate ? VIEW_PDF_CAN_ANNOTATE : 0) |
                (s.can_fill_forms ? VIEW_PDF_CAN_FILL : 0) | (s.has_xfa ? VIEW_PDF_HAS_XFA : 0) |
                (vp_signed ? VIEW_PDF_HAS_SIGNED : 0);
    if (pdf_array_len(ctx, pdf_dict_getp(ctx, pdf_trailer(ctx, pdf), "Root/AcroForm/Fields")) > 0) {
        r->flags |= VIEW_PDF_HAS_FORMS;
    }
    r->revision = s.revision;
}

/* s into dst of size bytes, NUL ended, cut between UTF-8 characters: 1 if cut. */
static int vp_copy(char *dst, size_t size, const char *s)
{
    size_t n = s ? strlen(s) : 0, k = n < size - 1 ? n : size - 1;
    if (k < n) while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
    if (k) memcpy(dst, s, k);
    dst[k] = '\0';
    return k < n;
}

static const struct { int view; enum pdf_annot_type mupdf; } vp_types[] = {
    { VIEW_ANNOT_TEXT, PDF_ANNOT_TEXT }, { VIEW_ANNOT_FREE_TEXT, PDF_ANNOT_FREE_TEXT },
    { VIEW_ANNOT_LINE, PDF_ANNOT_LINE }, { VIEW_ANNOT_SQUARE, PDF_ANNOT_SQUARE },
    { VIEW_ANNOT_CIRCLE, PDF_ANNOT_CIRCLE }, { VIEW_ANNOT_POLYGON, PDF_ANNOT_POLYGON },
    { VIEW_ANNOT_POLY_LINE, PDF_ANNOT_POLY_LINE }, { VIEW_ANNOT_HIGHLIGHT, PDF_ANNOT_HIGHLIGHT },
    { VIEW_ANNOT_UNDERLINE, PDF_ANNOT_UNDERLINE }, { VIEW_ANNOT_SQUIGGLY, PDF_ANNOT_SQUIGGLY },
    { VIEW_ANNOT_STRIKE_OUT, PDF_ANNOT_STRIKE_OUT }, { VIEW_ANNOT_STAMP, PDF_ANNOT_STAMP },
    { VIEW_ANNOT_INK, PDF_ANNOT_INK },
};

static int vp_view_type(enum pdf_annot_type t)
{
    for (size_t i = 0; i < sizeof vp_types / sizeof *vp_types; i++) if (vp_types[i].mupdf == t) return vp_types[i].view;
    return VIEW_ANNOT_OTHER;
}

static int vp_markup(int view_type)
{
    return view_type == VIEW_ANNOT_HIGHLIGHT || view_type == VIEW_ANNOT_UNDERLINE ||
           view_type == VIEW_ANNOT_SQUIGGLY || view_type == VIEW_ANNOT_STRIKE_OUT;
}

static void vp_annots(view_pdf *p, const struct view_request *q, struct view_reply *r, struct out *o)
{
    load_page(q->page);
    fz_point at = { page_box.x0, page_box.y0 };
    view_pdf_annotations list = view_pdf_list_annotations(p, q->page);
    fz_try(ctx) {
        o->buf = fz_new_buffer(ctx, 1024);
        for (int i = 0; i < list.count; i++) {
            const view_pdf_annotation *a = &list.items[i];
            /* Okular lists neither pop-up windows nor links and widgets. */
            if (a->type == PDF_ANNOT_POPUP || a->type == PDF_ANNOT_LINK || a->type == PDF_ANNOT_WIDGET) continue;
            if (r->count == VIEW_PDF_ITEMS) {
                r->flags |= VIEW_PDF_MORE;
                break;
            }
            struct view_annot v;
            memset(&v, 0, sizeof v);
            v.object = a->id.object;
            v.generation = a->id.generation;
            v.type = vp_view_type(a->type);
            v.flags = a->flags;
            v.x0 = or_zero(a->rect.x0 - at.x);
            v.y0 = or_zero(a->rect.y0 - at.y);
            v.x1 = or_zero(a->rect.x1 - at.x);
            v.y1 = or_zero(a->rect.y1 - at.y);
            v.color_n = a->color_n >= 0 && a->color_n <= 4 ? a->color_n : 0;
            for (int k = 0; k < 4; k++) v.color[k] = or_zero(a->color[k]);
            v.opacity = or_zero(a->opacity);
            v.width = or_zero(a->width);
            v.editable = a->editable;
            vp_copy(v.subtype, sizeof v.subtype, pdf_string_from_annot_type(ctx, a->type));
            vp_copy(v.icon, sizeof v.icon, a->icon);
            v.truncated = vp_copy(v.author, sizeof v.author, a->author) | vp_copy(v.contents, sizeof v.contents, a->contents);
            put(o->buf, &v, sizeof v);
            r->count++;
        }
    }
    fz_always(ctx) view_pdf_drop_annotations(ctx, &list);
    fz_catch(ctx) fz_rethrow(ctx);
}

static int vp_field_type(enum pdf_widget_type t)
{
    switch (t) {
    case PDF_WIDGET_TYPE_TEXT: return VIEW_FIELD_TEXT;
    case PDF_WIDGET_TYPE_CHECKBOX: return VIEW_FIELD_CHECKBOX;
    case PDF_WIDGET_TYPE_RADIOBUTTON: return VIEW_FIELD_RADIO;
    case PDF_WIDGET_TYPE_COMBOBOX: return VIEW_FIELD_COMBO;
    case PDF_WIDGET_TYPE_LISTBOX: return VIEW_FIELD_LIST;
    case PDF_WIDGET_TYPE_BUTTON: return VIEW_FIELD_BUTTON;
    case PDF_WIDGET_TYPE_SIGNATURE: return VIEW_FIELD_SIGNATURE;
    default: return VIEW_FIELD_OTHER;
    }
}

static void vp_fields(view_pdf *p, const struct view_request *q, struct view_reply *r, struct out *o)
{
    load_page(q->page);
    fz_point at = { page_box.x0, page_box.y0 };
    view_pdf_widgets list = view_pdf_list_widgets(p, q->page);
    fz_buffer *options = NULL;
    fz_var(options);
    fz_try(ctx) {
        o->buf = fz_new_buffer(ctx, 1024);
        options = fz_new_buffer(ctx, 256);
        for (int i = 0; i < list.count; i++) {
            const view_pdf_widget *w = &list.items[i];
            if (r->count == VIEW_PDF_ITEMS) {
                r->flags |= VIEW_PDF_MORE;
                break;
            }
            struct view_field f;
            memset(&f, 0, sizeof f);
            f.object = w->id.object;
            f.generation = w->id.generation;
            f.type = vp_field_type(w->type);
            f.flags = w->flags;
            f.x0 = or_zero(w->rect.x0 - at.x);
            f.y0 = or_zero(w->rect.y0 - at.y);
            f.x1 = or_zero(w->rect.x1 - at.x);
            f.y1 = or_zero(w->rect.y1 - at.y);
            f.readonly = w->readonly;
            f.required = w->required;
            f.max_length = w->max_length > 0 ? w->max_length : 0;
            f.multiselect = w->multiselect;
            f.editable_choice = f.type == VIEW_FIELD_COMBO && (w->flags & PDF_CH_FIELD_IS_EDIT);
            f.truncated = vp_copy(f.name, sizeof f.name, w->name) | vp_copy(f.on_value, sizeof f.on_value, w->on_value) |
                          vp_copy(f.value, sizeof f.value, w->value);
            f.first_option = r->w;
            for (int k = 0; k < w->option_count; k++) {
                if (r->w == VIEW_PDF_OPTIONS) {
                    f.truncated = 1; /* not all its options: not chosen from here */
                    r->flags |= VIEW_PDF_MORE;
                    break;
                }
                struct view_option v;
                memset(&v, 0, sizeof v);
                v.truncated = vp_copy(v.label, sizeof v.label, w->options[k].label) |
                              vp_copy(v.value, sizeof v.value, w->options[k].value);
                for (int s = 0; s < w->value_count; s++) v.selected |= !strcmp(w->values[s], w->options[k].value);
                put(options, &v, sizeof v);
                f.options++;
                r->w++;
            }
            put(o->buf, &f, sizeof f);
            r->count++;
        }
        unsigned char *data;
        size_t n = fz_buffer_storage(ctx, options, &data);
        put(o->buf, data, n);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, options);
        view_pdf_drop_widgets(ctx, &list);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static int vp_sig_status(int e)
{
    switch (e) {
    case VIEW_PDF_SIGNATURE_UNAVAILABLE: return VIEW_SIG_UNAVAILABLE;
    case VIEW_PDF_SIGNATURE_FAILED: return VIEW_SIG_FAILED;
    case PDF_SIGNATURE_ERROR_OKAY: return VIEW_SIG_OKAY;
    case PDF_SIGNATURE_ERROR_NO_SIGNATURES: return VIEW_SIG_NO_SIGNATURES;
    case PDF_SIGNATURE_ERROR_NO_CERTIFICATE: return VIEW_SIG_NO_CERTIFICATE;
    case PDF_SIGNATURE_ERROR_DIGEST_FAILURE: return VIEW_SIG_DIGEST_FAILURE;
    case PDF_SIGNATURE_ERROR_SELF_SIGNED: return VIEW_SIG_SELF_SIGNED;
    case PDF_SIGNATURE_ERROR_SELF_SIGNED_IN_CHAIN: return VIEW_SIG_SELF_SIGNED_IN_CHAIN;
    case PDF_SIGNATURE_ERROR_NOT_TRUSTED: return VIEW_SIG_NOT_TRUSTED;
    case PDF_SIGNATURE_ERROR_NOT_SIGNED: return VIEW_SIG_NOT_SIGNED;
    default: return VIEW_SIG_UNKNOWN;
    }
}

/* "D:20240102030405+01'00'" as "2024-01-02 03:04:05 +01:00", as put_date. */
static void vp_date(char *out, size_t size, const char *d)
{
    if (!strncmp(d, "D:", 2)) d += 2;
    int digits = 0;
    while (isdigit((unsigned char)d[digits])) digits++;
    if (digits < 8) {
        vp_copy(out, size, d);
        return;
    }
    char s[64];
    snprintf(s, sizeof s, "%.4s-%.2s-%.2s", d, d + 4, d + 6);
    if (digits >= 12) snprintf(s + 10, sizeof s - 10, " %.2s:%.2s:%.2s", d + 8, d + 10, digits >= 14 ? d + 12 : "00");
    const char *z = d + digits;
    size_t l = strlen(s);
    if (*z == 'Z') snprintf(s + l, sizeof s - l, " UTC");
    else if ((*z == '+' || *z == '-') && isdigit((unsigned char)z[1]) && isdigit((unsigned char)z[2]))
        snprintf(s + l, sizeof s - l, " %c%.2s:%.2s", *z, z + 1, isdigit((unsigned char)z[4]) ? z + 4 : "00");
    vp_copy(out, size, s);
}

static void vp_signature_list(view_pdf *p, struct view_reply *r, struct out *o)
{
    view_pdf_signatures list = view_pdf_list_signatures(p, NULL);
    fz_try(ctx) {
        o->buf = fz_new_buffer(ctx, 1024);
        for (int i = 0; i < list.count && i < VIEW_PDF_ITEMS; i++) {
            const view_pdf_signature *s = &list.items[i];
            struct view_signature v;
            memset(&v, 0, sizeof v);
            v.page = s->id.page;
            v.object = s->id.object;
            v.generation = s->id.generation;
            v.is_signed = s->signed_value != 0;
            v.digest = vp_sig_status(s->digest_status);
            v.certificate = vp_sig_status(s->certificate_status);
            v.changes = s->change_status >= -2 ? s->change_status : -2;
            v.modified = s->document_modified;
            vp_copy(v.field, sizeof v.field, s->field);
            vp_copy(v.name, sizeof v.name, s->name);
            vp_copy(v.reason, sizeof v.reason, s->reason);
            vp_copy(v.location, sizeof v.location, s->location);
            vp_date(v.date, sizeof v.date, s->date ? s->date : "");
            vp_copy(v.signatory, sizeof v.signatory, s->signatory);
            vp_copy(v.error, sizeof v.error, s->error);
            put(o->buf, &v, sizeof v);
            r->count++;
        }
    }
    fz_always(ctx) view_pdf_drop_signatures(ctx, &list);
    fz_catch(ctx) fz_rethrow(ctx);
}

static int vp_finite(float v, float lo, float hi)
{
    return isfinite(v) && v >= lo && v <= hi;
}

/* A string of at most size bytes with its NUL in them, valid UTF-8. */
static int vp_text_ok(const char *s, size_t size)
{
    const char *end = memchr(s, '\0', size);
    return end && valid_utf8((const unsigned char *)s, (size_t)(end - s));
}

/* A PDF name for an icon: letters, digits, and nothing else. */
static int vp_icon_ok(const char *s, size_t size)
{
    if (!memchr(s, '\0', size)) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s)) return 0;
    return 1;
}

/* What every request's fields must be, before any is used. */
static int vp_request_ok(const struct view_request *q, struct view_reply *r)
{
    const float far = 1e5f;
    int ok = q->color_n == 0 || q->color_n == 1 || q->color_n == 3 || q->color_n == 4;
    for (int k = 0; ok && k < 4; k++) ok = vp_finite(q->color[k], 0, 1);
    ok = ok && vp_finite(q->opacity, 0, 1) && vp_finite(q->width, 0, 100);
    ok = ok && vp_finite(q->ax, -far, far) && vp_finite(q->ay, -far, far) && vp_finite(q->bx, -far, far) &&
         vp_finite(q->by, -far, far);
    ok = ok && vp_icon_ok(q->icon, sizeof q->icon) && vp_text_ok(q->author, sizeof q->author);
    ok = ok && q->object >= 0 && q->generation >= 0 && q->generation <= 65535 && !(q->mask & ~63u);
    if (!ok) {
        fail(r, EINVAL, "A malformed PDF request");
        return 0;
    }
    if (q->op == VIEW_PDF_CREATE) {
        int total = 0;
        ok = q->count >= 0 && q->count <= VIEW_PDF_POINTS && q->strokes >= 0 && q->strokes <= VIEW_PDF_STROKES;
        for (int i = 0; ok && i < q->strokes; i++) {
            ok = q->stroke[i] >= 1 && q->stroke[i] <= VIEW_PDF_POINTS;
            total += ok ? q->stroke[i] : 0;
        }
        ok = ok && (!q->strokes || total == q->count);
        for (int i = 0; ok && i < 2 * q->count; i++) ok = vp_finite(q->points[i], -far, far);
        ok = ok && vp_text_ok(q->text, sizeof q->text);
    } else if (q->op == VIEW_PDF_UPDATE) {
        ok = q->mask != 0 && (!(q->mask & VIEW_SET_CONTENTS) || vp_text_ok(q->text, sizeof q->text));
    } else if (q->op == VIEW_PDF_SET) {
        ok = q->count >= 0 && q->count <= 64;
    } else if (q->op == VIEW_PDF_WRITE) {
        ok = q->text[0] == '/' && vp_text_ok(q->text, sizeof q->text) && (q->mode == 0 || q->mode == VIEW_PDF_INVALIDATE_SIGNATURES);
    }
    if (!ok) fail(r, EINVAL, "A malformed PDF request");
    return ok;
}

static fz_rect vp_rect(const struct view_request *q, fz_point at)
{
    return fz_make_rect(fminf(q->ax, q->bx) + at.x, fminf(q->ay, q->by) + at.y,
                        fmaxf(q->ax, q->bx) + at.x, fmaxf(q->ay, q->by) + at.y);
}

/* VIEW_ANNOT_* as vp_types' index: -1 if it is not one made here. Not
 * inlined into vp_create's fz_try, where its loop would be clobbered. */
__attribute__((noinline))
static int vp_type_index(int view_type)
{
    for (size_t i = 0; i < sizeof vp_types / sizeof *vp_types; i++) if (vp_types[i].view == view_type) return (int)i;
    return -1;
}

static void vp_create(view_pdf *p, const struct view_request *q, struct view_reply *r)
{
    const int t = vp_type_index(q->mode);
    if (t < 0) {
        fail(r, EINVAL, "No such annotation type %d", q->mode);
        return;
    }
    const int view_type = vp_types[t].view, n = q->count;
    load_page(q->page);
    fz_point at = { page_box.x0, page_box.y0 };
    fz_rect rect = vp_rect(q, at);
    fz_quad *quads = NULL;
    fz_point *points = NULL;
    fz_var(quads);
    fz_var(points);
    fz_try(ctx) {
        view_pdf_annotation_spec s = view_pdf_default_annotation(vp_types[t].mupdf, rect);
        int whole = n; /* ink of one stroke */
        points = fz_malloc_array(ctx, n > 0 ? n : 1, fz_point);
        for (int i = 0; i < n; i++) points[i] = fz_make_point(q->points[2 * i] + at.x, q->points[2 * i + 1] + at.y);
        if (vp_markup(view_type)) {
            /* The text between the two ends, as a selection takes it. */
            memset(&cookie, 0, sizeof cookie);
            fz_stext_page *text = load_text(q->page);
            fz_point a = { q->ax + at.x, q->ay + at.y }, b = { q->bx + at.x, q->by + at.y };
            fz_quad h = fz_snap_selection(ctx, text, &a, &b, FZ_SELECT_CHARS);
            a = fz_make_point((h.ul.x + h.ll.x) / 2, (h.ul.y + h.ll.y) / 2);
            b = fz_make_point((h.ur.x + h.lr.x) / 2, (h.ur.y + h.lr.y) / 2);
            quads = fz_malloc_array(ctx, MAX_QUADS, fz_quad);
            int found = fz_highlight_selection(ctx, text, a, b, quads, MAX_QUADS);
            /* MuPDF snaps to the nearest text however far: only what the drag
             * spans is marked, as Okular's text selector has it. */
            fz_rect span = fz_expand_rect(vp_rect(q, at), 2);
            s.quad_count = 0;
            for (int i = 0; i < found; i++)
                if (!fz_is_empty_rect(fz_intersect_rect(fz_rect_from_quad(quads[i]), span))) quads[s.quad_count++] = quads[i];
            if (s.quad_count < 1) fz_throw(ctx, FZ_ERROR_ARGUMENT, "There is no text there to mark");
            s.quads = quads;
            s.rect = fz_empty_rect;
            for (int i = 0; i < s.quad_count; i++) s.rect = fz_union_rect(s.rect, fz_rect_from_quad(quads[i]));
        } else if (view_type == VIEW_ANNOT_LINE || view_type == VIEW_ANNOT_POLYGON || view_type == VIEW_ANNOT_POLY_LINE ||
                   view_type == VIEW_ANNOT_INK) {
            int least = view_type == VIEW_ANNOT_POLYGON ? 3 : 2;
            if (view_type == VIEW_ANNOT_INK) least = 1;
            if (n < least || (view_type == VIEW_ANNOT_LINE && n != 2)) fz_throw(ctx, FZ_ERROR_ARGUMENT, "Too few points");
            s.rect = fz_make_rect(points[0].x, points[0].y, points[0].x, points[0].y);
            for (int i = 1; i < n; i++) s.rect = fz_include_point_in_rect(s.rect, points[i]);
            /* A rectangle with area, around a line's stroke too. */
            float pad = q->width / 2 + 1;
            s.rect = fz_expand_rect(s.rect, pad);
            if (view_type == VIEW_ANNOT_LINE) {
                s.line[0] = points[0];
                s.line[1] = points[1];
            } else if (view_type == VIEW_ANNOT_INK) {
                s.stroke_count = q->strokes ? q->strokes : 1;
                s.stroke_sizes = q->strokes ? q->stroke : &whole;
                s.point_count = n;
                s.points = points;
            } else {
                s.vertex_count = n;
                s.vertices = points;
            }
        } else if (!(rect.x1 > rect.x0 && rect.y1 > rect.y0)) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "An annotation needs a rectangle with an area");
        }
        s.contents = q->text;
        s.author = q->author;
        s.color_n = q->color_n;
        for (int k = 0; k < 4; k++) s.color[k] = q->color[k];
        s.opacity = q->opacity;
        s.width = q->width;
        if (q->icon[0]) s.icon = q->icon;
        if (view_type == VIEW_ANNOT_FREE_TEXT) s.font_size = 10; /* Okular's tools: 10 points */
        view_pdf_id id = view_pdf_create_annotation(p, q->page, &s);
        r->w = id.object;
        r->h = id.generation;
    }
    fz_always(ctx) {
        fz_free(ctx, quads);
        fz_free(ctx, points);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static void vp_update(view_pdf *p, const struct view_request *q)
{
    view_pdf_id id = { q->page, q->object, q->generation };
    view_pdf_annotation_spec s = view_pdf_default_annotation(PDF_ANNOT_TEXT, fz_make_rect(0, 0, 1, 1));
    unsigned mask = 0;
    if (q->mask & VIEW_SET_CONTENTS) mask |= VIEW_PDF_CONTENTS, s.contents = q->text;
    if (q->mask & VIEW_SET_AUTHOR) mask |= VIEW_PDF_AUTHOR, s.author = q->author;
    if (q->mask & VIEW_SET_COLOR) {
        mask |= VIEW_PDF_COLOR;
        s.color_n = q->color_n;
        for (int k = 0; k < 4; k++) s.color[k] = q->color[k];
    }
    if (q->mask & VIEW_SET_OPACITY) mask |= VIEW_PDF_OPACITY, s.opacity = q->opacity;
    if (q->mask & VIEW_SET_WIDTH) mask |= VIEW_PDF_WIDTH, s.width = q->width;
    if (q->mask & VIEW_SET_ICON) {
        if (!q->icon[0]) fz_throw(ctx, FZ_ERROR_ARGUMENT, "No icon given");
        mask |= VIEW_PDF_ICON;
        s.icon = q->icon;
    }
    view_pdf_update_annotation(p, id, mask, &s);
}

static void vp_set(view_pdf *p, const struct view_request *q)
{
    const char *values[64];
    size_t at = 0;
    for (int i = 0; i < q->count; i++) {
        const char *end = at < VIEW_TEXT ? memchr(q->text + at, '\0', VIEW_TEXT - at) : NULL;
        if (!end || !valid_utf8((const unsigned char *)q->text + at, (size_t)(end - (q->text + at)))) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "A malformed field value");
        }
        values[i] = q->text + at;
        at = (size_t)(end - q->text) + 1;
    }
    view_pdf_id id = { q->page, q->object, q->generation };
    view_pdf_set_widget(p, id, q->count, values);
}

static void vp_write(view_pdf *p, const struct view_request *q, struct view_reply *r)
{
    int consent = 0;
    fz_var(consent);
    fz_try(ctx) view_pdf_save(p, q->text, q->mode == VIEW_PDF_INVALIDATE_SIGNATURES ? VIEW_PDF_ALLOW_SIGNATURE_INVALIDATION : 0);
    fz_catch(ctx) {
        if (fz_caught(ctx) != FZ_ERROR_ARGUMENT || !strstr(fz_caught_message(ctx), "invalidation consent")) fz_rethrow(ctx);
        consent = 1;
        fz_ignore_error(ctx);
    }
    if (consent) fail(r, EPERM, "Saving the changes would invalidate the document's signatures");
}

static void vp_request(const struct view_request *q, struct view_reply *r, struct out *o)
{
    view_pdf *p = vp_attached(r);
    if (!p || !vp_request_ok(q, r)) return;
    int edits = q->op == VIEW_PDF_CREATE || q->op == VIEW_PDF_UPDATE || q->op == VIEW_PDF_MOVE ||
                q->op == VIEW_PDF_DELETE || q->op == VIEW_PDF_UNDO || q->op == VIEW_PDF_REDO || q->op == VIEW_PDF_SET;
    view_pdf_status s = view_pdf_get_status(p);
    fz_try(ctx) {
        switch (q->op) {
        case VIEW_PDF_STATUS: r->count = vp_signatures; break;
        case VIEW_PDF_ANNOTS: vp_annots(p, q, r, o); break;
        case VIEW_PDF_FIELDS: vp_fields(p, q, r, o); break;
        case VIEW_PDF_SIGNATURES: vp_signature_list(p, r, o); break;
        case VIEW_PDF_CREATE: vp_create(p, q, r); break;
        case VIEW_PDF_UPDATE: vp_update(p, q); break;
        case VIEW_PDF_MOVE:
            load_page(q->page);
            view_pdf_transform_annotation(p, (view_pdf_id){ q->page, q->object, q->generation },
                                          vp_rect(q, fz_make_point(page_box.x0, page_box.y0)));
            break;
        case VIEW_PDF_DELETE: view_pdf_delete_annotation(p, (view_pdf_id){ q->page, q->object, q->generation }); break;
        case VIEW_PDF_UNDO:
            if (!s.can_undo) fz_throw(ctx, FZ_ERROR_ARGUMENT, "There is nothing to undo");
            view_pdf_undo(p);
            break;
        case VIEW_PDF_REDO:
            if (!s.can_redo) fz_throw(ctx, FZ_ERROR_ARGUMENT, "There is nothing to redo");
            view_pdf_redo(p);
            break;
        case VIEW_PDF_SET: vp_set(p, q); break;
        case VIEW_PDF_WRITE: vp_write(p, q, r); break;
        }
    }
    fz_always(ctx) {
        if (edits) drop_page();
    }
    fz_catch(ctx) fz_rethrow(ctx);
    if (!r->status) vp_status(p, r);
}

/* Opening. */
static float open_w, open_h, open_em; /* OPEN's layout, for after AUTH */

static void close_document(void)
{
    vp_detach();
    drop_page();
    fz_drop_document(ctx, doc);
    doc = NULL;
    djvu_close();
    ps_close();
    if (data_fd >= 0) close(data_fd);
    data_fd = -1;
    kind = NONE;
    locked = npages = has_text = 0;
    format[0] = title[0] = '\0';
}

/* ~/VIEW_USER_CSS, read as the user it belongs to (NULL if there is none). */
static char *read_user_css(void)
{
    struct passwd *pw = getpwuid(getuid());
    char path[PATH_MAX];
    if (!pw || snprintf(path, sizeof path, "%s/%s", pw->pw_dir, VIEW_USER_CSS) >= (int)sizeof path) return NULL;
    FILE *f = fopen(path, "re");
    if (!f) return NULL;
    char *css = malloc(MAX_CSS + 1);
    size_t n = css ? fread(css, 1, MAX_CSS, f) : 0;
    fclose(f);
    if (css) css[n] = '\0';
    return css;
}

static void set_format(const char *name, int text)
{
    snprintf(format, sizeof format, "%s", name);
    has_text = text;
}

/* Okular's name for what MuPDF's handler h opened, and whether its pages
 * can have text. */
static void mupdf_format(const fz_document_handler *h)
{
    static const struct { const fz_document_handler *h; const char *name; } names[] = {
        { &pdf_document_handler, "PDF" }, { &xps_document_handler, "XPS" }, { &epub_document_handler, "EPUB" },
        { &mobi_document_handler, "Mobipocket" }, { &fb2_document_handler, "FictionBook" },
        { &md_document_handler, "Markdown" }, { &html_document_handler, "HTML" }, { &xhtml_document_handler, "HTML" },
        { &svg_document_handler, "SVG" },
    };
    char v[64];
    if (h == &img_document_handler) {
        int tiff = fz_lookup_metadata(ctx, doc, FZ_META_FORMAT, v, sizeof v) > 0 && !strcmp(v, "TIFF");
        set_format(tiff ? "TIFF" : "Image", 0);
        return;
    }
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (h == names[i].h) {
            set_format(names[i].name, 1);
            return;
        }
    }
    set_format(fz_lookup_metadata(ctx, doc, FZ_META_FORMAT, v, sizeof v) > 0 ? v : "Document", 1);
}

static void open_buffer(const char *magic, fz_buffer *b)
{
    fz_try(ctx) doc = fz_open_document_with_buffer(ctx, magic, b);
    fz_always(ctx) fz_drop_buffer(ctx, b);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void open_text(const char *path)
{
    fz_buffer *raw = fz_read_file(ctx, path), *utf8 = NULL;
    fz_try(ctx) utf8 = text_to_utf8(raw);
    fz_always(ctx) fz_drop_buffer(ctx, raw);
    fz_catch(ctx) fz_rethrow(ctx);
    open_buffer("txt", utf8);
    set_format("Plain text", 1);
}

static void open_converted(const char *path, const char *name, int fax_g4, int fax)
{
    fz_buffer *raw = fz_read_file(ctx, path), *conv = NULL;
    fz_var(conv);
    fz_try(ctx) {
        unsigned char *s;
        size_t n = fz_buffer_storage(ctx, raw, &s);
        conv = fax ? fax_png(raw, fax_g4) : convert_pam(s, n, name);
    }
    fz_always(ctx) fz_drop_buffer(ctx, raw);
    fz_catch(ctx) fz_rethrow(ctx);
    open_buffer(fax ? "png" : "pam", conv);
    set_format(fax ? "Fax" : "Image", 0);
}

/* FictionBook in a zip (Okular: application/x-zip-compressed-fb2, *.fb2.zip):
 * its .fb2 read out for MuPDF. */
static void open_fb2_zip(const char *path)
{
    fz_archive *zip = fz_open_archive(ctx, path);
    fz_buffer *b = NULL;
    fz_var(b);
    fz_try(ctx) {
        for (int i = 0, n = fz_count_archive_entries(ctx, zip); i < n && !b; i++) {
            const char *entry = fz_list_archive_entry(ctx, zip, i);
            if (entry && !strcasecmp(extension(entry), "fb2")) b = fz_read_archive_entry(ctx, zip, entry);
        }
        if (!b) fz_throw(ctx, FZ_ERROR_FORMAT, "no .fb2 file in this zip");
    }
    fz_always(ctx) fz_drop_archive(ctx, zip);
    fz_catch(ctx) fz_rethrow(ctx);
    open_buffer("fb2", b);
    set_format("FictionBook", 1);
}

static int ext_is(const char *e, const char *list)
{
    char want[16];
    for (const char *s = list; *s;) {
        size_t n = strcspn(s, " ");
        snprintf(want, sizeof want, "%.*s", (int)n, s);
        if (!strcasecmp(e, want)) return 1;
        s += n + (s[n] == ' ');
    }
    return 0;
}

/* Open path (name: what it is called, for its extension; m: its first n
 * bytes) by what it is, as Okular picks a generator by the file's type. */
static void open_by_type(const char *path, const char *name, int dir, const unsigned char *m, size_t n, struct view_reply *r)
{
    const char *e = extension(name);
    kind = MUPDF;
    if (dir) {
        doc = open_comic(path, COMIC_DIR);
        set_format("Comic book", 0);
    } else if (n >= 8 && !memcmp(m, "AT&TFORM", 8)) {
        kind = DJVU;
        set_format("DjVu", 1);
        djvu_open(path, r);
    } else if ((n >= 2 && !memcmp(m, "%!", 2)) || (n >= 4 && !memcmp(m, "\xC5\xD0\xD3\xC6", 4))) {
        kind = PS;
        set_format("PostScript", 0);
        ps_open(path, r);
    } else if (n >= 2 && m[0] == 0xF7 && m[1] == 0x02) {
        set_format("DVI", 1);
        doc = open_dvi(path, r);
    } else if ((n >= 6 && !memcmp(m, "Rar!\x1A\x07", 6)) || (n >= 6 && !memcmp(m, "7z\xBC\xAF\x27\x1C", 6))) {
        doc = open_comic(path, COMIC_LIBARCHIVE);
        set_format("Comic book", 0);
    } else if (is_kimgio(m, n) || magick_coder_content(m, n)) {
        open_converted(path, name, 0, 0);
    } else if ((n >= 64 && !memcmp(m, FAX_MAGIC, sizeof FAX_MAGIC - 1)) || ext_is(e, "g3 fax")) {
        open_converted(path, name, !ext_is(e, "g3"), 1);
    } else if (ext_is(e, "txt")) {
        open_text(path);
    } else if (strlen(name) > 8 && !strcasecmp(name + strlen(name) - 8, ".fb2.zip")) {
        open_fb2_zip(path);
    } else {
        /* Markdown by its names (text/markdown's), not by MuPDF's guess
         * from a first line starting with "#", which is also many a text
         * file's. */
        int md = ext_is(e, "md markdown mkd mdown mkdn");
        fz_stream *stm = fz_open_file(ctx, path);
        fz_archive *dir = NULL;
        const fz_document_handler *h = NULL;
        fz_var(h);
        fz_var(dir);
        fz_try(ctx) {
            h = md ? &md_document_handler : fz_recognize_document_stream_content(ctx, stm, name);
            if (h == &md_document_handler && !md) h = &txt_document_handler;
            if (md && data_fd < 0) {
                char folder[PATH_MAX];
                snprintf(folder, sizeof folder, "%s", path);
                char *slash = strrchr(folder, '/');
                if (slash) *slash = '\0';
                dir = fz_open_directory(ctx, slash ? (*folder ? folder : "/") : ".");
                doc = fz_open_document_with_stream_and_dir(ctx, "text/markdown", stm, dir);
                mupdf_format(h);
            } else if (h && h != &cbz_document_handler && h != &txt_document_handler) {
                doc = data_fd < 0 ? fz_open_document(ctx, path) : fz_open_document_with_stream(ctx, md ? "text/markdown" : name, stm);
                mupdf_format(h);
            }
        }
        fz_always(ctx) {
            fz_drop_archive(ctx, dir);
            fz_drop_stream(ctx, stm);
        }
        fz_catch(ctx) fz_rethrow(ctx);
        if (doc) {
            /* MuPDF's own. */
        } else if (h == &cbz_document_handler) {
            doc = open_comic(path, COMIC_MUPDF);
            set_format("Comic book", 0);
        } else if (h == &txt_document_handler) {
            open_text(path);
        } else if (ext_is(e, "ps eps")) {
            kind = PS;
            set_format("PostScript", 0);
            ps_open(path, r);
        } else if (ext_is(e, "djvu djv")) {
            kind = DJVU;
            set_format("DjVu", 1);
            djvu_open(path, r);
        } else if (ext_is(e, "dvi")) {
            set_format("DVI", 1);
            doc = open_dvi(path, r);
        } else if (ext_is(e, "cbz cbr cb7 cbt zip rar 7z tar")) {
            doc = open_comic(path, COMIC_LIBARCHIVE);
            set_format("Comic book", 0);
        } else if (magick_coder_name(name)) {
            open_converted(path, name, 0, 0);
        } else if (looks_like_text(m, n)) {
            open_text(path);
        } else {
            fail(r, ENOTSUP, "Unknown file type");
        }
    }
    if (kind == MUPDF && !doc && !r->status) fail(r, EINVAL, "Cannot open this file");
    if (kind == MUPDF && doc && fz_needs_password(ctx, doc)) {
        locked = 1;
        r->flags |= VIEW_NEEDS_PASSWORD;
        fail(r, EACCES, "This document needs a password");
    }
}

/* The rest of OPEN, once the document is readable: the reply. */
static void finish_open(struct view_reply *r)
{
    char v[512];
    if (kind == MUPDF && fz_is_document_reflowable(ctx, doc)) {
        char *css = read_user_css();
        fz_var(css);
        fz_try(ctx) {
            fz_style_document(ctx, doc, 1, css);
            if (open_w > 0 && open_h > 0 && open_em > 0) fz_layout_document(ctx, doc, open_w, open_h, open_em);
        }
        fz_always(ctx) free(css);
        fz_catch(ctx) fz_rethrow(ctx);
        r->flags |= VIEW_REFLOWABLE;
    }
    if (kind == MUPDF) {
        npages = fz_count_pages(ctx, doc);
        if (fz_lookup_metadata(ctx, doc, FZ_META_INFO_TITLE, v, sizeof v) > 0 && v[0]) copy_text(title, sizeof title, v);
        fz_outline *ol = fz_load_outline(ctx, doc);
        if (ol) r->flags |= VIEW_HAS_OUTLINE;
        fz_drop_outline(ctx, ol);
        int count = 0;
        embedded(-1, &count, NULL);
        if (count) r->flags |= VIEW_HAS_EMBEDDED;
    } else if (kind == DJVU) {
        if (djvu_has_outline()) r->flags |= VIEW_HAS_OUTLINE;
    } else if (kind == PS) {
        const char *t = spectre_document_get_title(ps);
        if (t && *t) copy_text(title, sizeof title, t);
    }
    if (npages <= 0) fz_throw(ctx, FZ_ERROR_FORMAT, "no pages");
    if (has_text) r->flags |= VIEW_HAS_TEXT;
    r->pages = npages;
    snprintf(r->text, sizeof r->text, "%s\t%s", format, title);
}

static void op_open(const struct view_request *q, struct view_reply *r)
{
    char name[PATH_MAX], fdpath[64];
    unsigned char m[4096];
    struct stat st;
    const char *path = q->text;
    close_document();
    open_w = q->ax;
    open_h = q->ay;
    open_em = q->bx;
    snprintf(name, sizeof name, "%s", path);
    copy_text(title, sizeof title, base_name(path));
    /* Not blocking on a FIFO someone named. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0 || fstat(fd, &st) < 0) {
        fail(r, errno, "Cannot open %s: %s", path, strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    ssize_t n = S_ISREG(st.st_mode) ? pread(fd, m, sizeof m, 0) : 0;
    close(fd);
    if (n < 0) {
        fail(r, errno, "Cannot read %s: %s", path, strerror(errno));
        return;
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        fail(r, EINVAL, "%s is not a file", path);
        return;
    }
    if (is_packed(m, (size_t)n)) {
        if ((data_fd = unpack(path)) < 0) {
            fail(r, EINVAL, "Cannot unpack %s", path);
            return;
        }
        snprintf(fdpath, sizeof fdpath, "/proc/self/fd/%d", data_fd);
        path = fdpath;
        char *dot = strrchr(name, '.');
        if (dot && ext_is(dot + 1, "gz bz2 bz gzip")) *dot = '\0';
        if ((n = pread(data_fd, m, sizeof m, 0)) < 0) n = 0;
    }
    open_by_type(path, name, S_ISDIR(st.st_mode), m, (size_t)n, r);
    if (!r->status) finish_open(r);
}

static void op_auth(const struct view_request *q, struct view_reply *r)
{
    if (kind == NONE) {
        fail(r, EINVAL, "No document");
        return;
    }
    if (locked) {
        if (!fz_authenticate_password(ctx, doc, q->text)) {
            r->flags |= VIEW_NEEDS_PASSWORD;
            fail(r, EACCES, "Wrong password");
            return;
        }
        locked = 0;
    }
    finish_open(r);
}

/* The other requests. */
static void page_size(int n, float *pw, float *ph)
{
    if (kind == DJVU) {
        djvu_size(n, pw, ph);
    } else if (kind == PS) {
        ps_size(n, pw, ph);
    } else {
        load_page(n);
        *pw = page_box.x1 - page_box.x0;
        *ph = page_box.y1 - page_box.y0;
    }
}

static void op_info(struct view_reply *r, struct out *o)
{
    float pw, ph;
    o->buf = fz_new_buffer(ctx, 1024);
    put_info(o->buf, "Format", format);
    if (kind == MUPDF) mupdf_info_lines(o->buf);
    else if (kind == DJVU) djvu_info_lines(o->buf);
    else if (kind == PS) ps_info_lines(o->buf);
    fz_append_printf(ctx, o->buf, "Pages\t%d\n", npages);
    page_size(0, &pw, &ph);
    fz_append_printf(ctx, o->buf, "Page size\t%.0f x %.0f points (%.0f x %.0f mm)\n", pw, ph, pw * 25.4f / 72, ph * 25.4f / 72);
    if (kind == MUPDF && pdf_specifics(ctx, doc)) pdf_fonts(o->buf, pdf_specifics(ctx, doc));
    r->count = 0;
}

static void op_export(struct out *o)
{
    o->buf = fz_new_buffer(ctx, 4096);
    for (int i = 0; i < npages; i++) {
        if (i > 0) fz_append_byte(ctx, o->buf, '\f');
        if (kind == DJVU) {
            djvu_export_page(o->buf, i);
        } else if (kind == MUPDF && has_text) {
            fz_stext_page *t = load_text(i);
            fz_buffer *b = fz_new_buffer_from_stext_page(ctx, t);
            unsigned char *s;
            size_t n = fz_buffer_storage(ctx, b, &s);
            fz_try(ctx) put(o->buf, s, n);
            fz_always(ctx) fz_drop_buffer(ctx, b);
            fz_catch(ctx) fz_rethrow(ctx);
        }
    }
}

static void op_layout(const struct view_request *q, struct view_reply *r)
{
    if (kind != MUPDF || !fz_is_document_reflowable(ctx, doc)) {
        fail(r, EINVAL, "This document is not reflowable");
        return;
    }
    if (!(q->ax > 0 && q->ay > 0 && q->bx > 0) || !isfinite(q->ax) || !isfinite(q->ay) || !isfinite(q->bx)) {
        fail(r, EINVAL, "Bad layout");
        return;
    }
    int n = q->page < 0 ? 0 : q->page >= npages ? npages - 1 : q->page;
    fz_bookmark mark = fz_make_bookmark(ctx, doc, fz_location_from_page_number(ctx, doc, n));
    drop_page();
    fz_layout_document(ctx, doc, q->ax, q->ay, q->bx);
    npages = fz_count_pages(ctx, doc);
    open_w = q->ax;
    open_h = q->ay;
    open_em = q->bx;
    r->pages = npages;
    r->page = fz_page_number_from_location(ctx, doc, fz_lookup_bookmark(ctx, doc, mark));
    if (r->page < 0 || r->page >= npages) r->page = 0;
}

/* VIEW_MARK, VIEW_FIND_MARK: a place in a reflowable document by MuPDF's
 * bookmarks (the position in its flow), whatever the layout. */
static void op_mark(const struct view_request *q, struct view_reply *r)
{
    if (kind != MUPDF || !fz_is_document_reflowable(ctx, doc)) {
        fail(r, EINVAL, "This document is not reflowable");
        return;
    }
    if (q->op == VIEW_MARK) {
        fz_bookmark mark = fz_make_bookmark(ctx, doc, fz_location_from_page_number(ctx, doc, q->page));
        snprintf(r->text, sizeof r->text, "%lld", (long long)mark);
        r->page = q->page;
        return;
    }
    char *end;
    errno = 0;
    long long mark = strtoll(q->text, &end, 10);
    if (errno || end == q->text || *end) {
        fail(r, EINVAL, "Bad bookmark");
        return;
    }
    r->page = fz_page_number_from_location(ctx, doc, fz_lookup_bookmark(ctx, doc, (fz_bookmark)mark));
    if (r->page < 0 || r->page >= npages) fail(r, ENOENT, "The bookmark is not in the document");
}

/* Raster content bounds, including images and vector marks, excluding white
 * paint (a PDF's full-page white background is not content). Like Okular's
 * generator fallback, this operates on the rendered page, not text bounds. */
static void content_bounds(const struct view_request *q, struct view_reply *r, struct out *o)
{
    float pw, ph;
    page_size(q->page, &pw, &ph);
    struct view_request a = { .op = VIEW_RENDER, .page = q->page,
        .zoom = isfinite(q->zoom) && q->zoom > 0 ? q->zoom : 1 };
    struct view_reply rep = { 0 };
    struct out image = { .fd = -1 };
    fz_var(image);
    fz_try(ctx) {
        if (kind == DJVU) djvu_render(&a, &rep, &image);
        else if (kind == PS) ps_render(&a, &rep, &image);
        else mupdf_render(&a, &rep, &image);
        if (rep.status) fail(r, rep.status, "%s", rep.text);
        else {
            int x0 = rep.w, y0 = rep.h, x1 = 0, y1 = 0;
            const uint32_t *px = (const uint32_t *)image.map;
            for (int y = 0; y < rep.h; y++) for (int x = 0; x < rep.w; x++) {
                uint32_t c = px[(size_t)y * rep.w + x];
                /* Okular Utils::imageBoundingBox compares exact paper RGB;
                 * near-white antialiased marks are still page content. */
                if ((c & 0xFFFFFF) == 0xFFFFFF) continue;
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x + 1 > x1) x1 = x + 1;
                if (y + 1 > y1) y1 = y + 1;
            }
            if (x1 > x0 && y1 > y0) {
                struct view_rect b = { x0 / (float)rep.w, y0 / (float)rep.h,
                    (x1 - 1) / (float)rep.w, (y1 - 1) / (float)rep.h, 0 };
                o->buf = fz_new_buffer(ctx, sizeof b);
                fz_append_data(ctx, o->buf, &b, sizeof b);
                r->count = 1;
            }
        }
    }
    fz_always(ctx) {
        /* new_pixels uses an anonymous mmap, owned by this temporary result. */
        if (image.map) munmap(image.map, image.size);
        if (image.fd >= 0) close(image.fd);
        fz_drop_buffer(ctx, image.buf);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static void dispatch(const struct view_request *q, struct view_reply *r, struct out *o)
{
    if (q->op == VIEW_OPEN) {
        op_open(q, r);
        return;
    }
    if (q->op == VIEW_AUTH) {
        op_auth(q, r);
        return;
    }
    if (kind == NONE) {
        fail(r, EINVAL, "No document");
        return;
    }
    if (locked) {
        r->flags |= VIEW_NEEDS_PASSWORD;
        fail(r, EACCES, "This document needs a password");
        return;
    }
    int has_page = q->page >= 0 && q->page < npages;
    switch (q->op) {
    case VIEW_BBOX:
    case VIEW_PAGE:
    case VIEW_RENDER:
    case VIEW_SEARCH:
    case VIEW_SELECT:
    case VIEW_WORDS:
    case VIEW_MARK:
    case VIEW_LINKS:
    case VIEW_PDF_ANNOTS:
    case VIEW_PDF_CREATE:
    case VIEW_PDF_UPDATE:
    case VIEW_PDF_MOVE:
    case VIEW_PDF_DELETE:
    case VIEW_PDF_FIELDS:
    case VIEW_PDF_SET:
        if (!has_page) {
            fail(r, EINVAL, "No page %d", q->page);
            return;
        }
        break;
    }
    switch (q->op) {
    case VIEW_BBOX:
        memset(&cookie, 0, sizeof cookie);
        content_bounds(q, r, o);
        break;
    case VIEW_PAGE:
        r->page = q->page;
        if (kind == DJVU) {
            djvu_page(q->page, r);
        } else {
            page_size(q->page, &r->pw, &r->ph);
            if (kind == MUPDF && has_text && stext_has_chars(load_text(q->page))) r->flags |= VIEW_HAS_TEXT;
        }
        break;
    case VIEW_RENDER:
        memset(&cookie, 0, sizeof cookie);
        if (kind == DJVU) djvu_render(q, r, o);
        else if (kind == PS) ps_render(q, r, o);
        else mupdf_render(q, r, o);
        break;
    case VIEW_SEARCH:
        memset(&cookie, 0, sizeof cookie);
        if (kind == DJVU) djvu_search(q, r, o);
        else if (kind == MUPDF && has_text) mupdf_search(q, r, o);
        else fail(r, ENOTSUP, "%s has no text", format);
        break;
    case VIEW_SELECT:
        if (kind == DJVU) djvu_select(q, r, o);
        else if (kind == MUPDF && has_text) mupdf_select(q, r, o);
        else fail(r, ENOTSUP, "%s has no text", format);
        break;
    case VIEW_WORDS:
        if (kind == DJVU) djvu_words(q, r, o);
        else if (kind == MUPDF && has_text) mupdf_words(q, r, o);
        else fail(r, ENOTSUP, "%s has no text", format);
        break;
    case VIEW_MARK:
    case VIEW_FIND_MARK:
        op_mark(q, r);
        break;
    case VIEW_LINKS:
        if (kind == DJVU) djvu_links(q, r, o);
        else if (kind == MUPDF) mupdf_links(q, r, o);
        break;
    case VIEW_OUTLINE:
        if (kind == DJVU) djvu_outline(r, o);
        else if (kind == MUPDF) mupdf_outline(r, o);
        break;
    case VIEW_INFO:
        op_info(r, o);
        break;
    case VIEW_EXPORT:
        if (!has_text) fail(r, ENOTSUP, "%s has no text", format);
        else op_export(o);
        break;
    case VIEW_EMBEDDED:
        o->buf = fz_new_buffer(ctx, 256);
        if (kind == MUPDF) embedded(-1, &r->count, o->buf);
        break;
    case VIEW_SAVE:
        if (kind == MUPDF) mupdf_save(q, r);
        else fail(r, ENOENT, "No embedded file %d", q->page);
        break;
    case VIEW_LAYOUT:
        op_layout(q, r);
        break;
    case VIEW_RESOLVE:
        r->page = -1;
        r->x = r->y = -1;
        if (kind == MUPDF) {
            r->page = resolve(q->text, &r->x, &r->y);
        } else if (kind == DJVU) {
            r->page = djvu_target(q->text, 0);
            r->x = r->y = 0;
        }
        if (r->page < 0) r->x = r->y = -1;
        break;
    case VIEW_PDF_STATUS:
    case VIEW_PDF_ANNOTS:
    case VIEW_PDF_CREATE:
    case VIEW_PDF_UPDATE:
    case VIEW_PDF_MOVE:
    case VIEW_PDF_DELETE:
    case VIEW_PDF_UNDO:
    case VIEW_PDF_REDO:
    case VIEW_PDF_FIELDS:
    case VIEW_PDF_SET:
    case VIEW_PDF_SIGNATURES:
    case VIEW_PDF_WRITE:
        vp_request(q, r, o);
        break;
    default:
        fail(r, EINVAL, "Unknown request %u", q->op);
    }
}

static int error_status(int code)
{
    switch (code) {
    case FZ_ERROR_ABORT: return ECANCELED;
    case FZ_ERROR_SYSTEM: return sys_errno ? sys_errno : EIO;
    case FZ_ERROR_LIMIT: return EFBIG;
    case FZ_ERROR_UNSUPPORTED: return ENOTSUP;
    default: return EINVAL;
    }
}

static void handle(const struct view_request *q, struct view_reply *r, struct out *o)
{
    sys_errno = 0;
    fz_try(ctx) dispatch(q, r, o);
    fz_catch(ctx) {
        int code = fz_caught(ctx);
        fail(r, error_status(code), "%s", fz_caught_message(ctx));
        if (code != FZ_ERROR_ABORT) fz_report_error(ctx);
        else fz_ignore_error(ctx);
    }
    if (r->status && (q->op == VIEW_OPEN || q->op == VIEW_AUTH) && !locked) close_document();
    if (r->status && q->op == VIEW_LAYOUT) drop_page();
}

static int reply(const struct view_reply *r, int fd)
{
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } control;
    struct iovec iov = { (void *)r, sizeof *r };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (fd >= 0) {
        msg.msg_control = control.buf;
        msg.msg_controllen = sizeof control.buf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof fd);
    }
    ssize_t n;
    do n = sendmsg(VIEW_RENDER_FD, &msg, MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof *r ? 0 : -1;
}

/* The bulk result as a sealed memfd for the reply: its fd, or -1. */
static int finish_out(struct view_reply *r, struct out *o)
{
    int fd = -1;
    if (o->map) munmap(o->map, o->size);
    if (!r->status && o->fd >= 0) {
        if (seal(o->fd) == 0) {
            fd = o->fd;
            r->size = o->size;
            o->fd = -1;
        } else {
            fail(r, errno, "Cannot seal the pixels: %s", strerror(errno));
        }
    } else if (!r->status && o->buf) {
        unsigned char *data;
        size_t n = fz_buffer_storage(ctx, o->buf, &data);
        if (n && (fd = sealed(data, n)) < 0) fail(r, errno, "Cannot pass the result: %s", strerror(errno));
        else r->size = n;
    }
    if (o->fd >= 0) close(o->fd);
    fz_drop_buffer(ctx, o->buf);
    if (r->status) {
        r->size = 0;
        r->count = 0;
    }
    return fd;
}

int main(void)
{
    if (!sandboxed()) return 1;
    struct sigaction sa = { .sa_handler = on_usr1, .sa_flags = SA_RESTART };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    sigprocmask(SIG_UNBLOCK, &usr1, NULL);
    signal(SIGPIPE, SIG_IGN);

    ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx) {
        say("cannot start MuPDF");
        return 1;
    }
    fz_register_document_handlers(ctx);

    static struct view_request q;
    static struct view_reply r;
    for (;;) {
        ssize_t n;
        do n = recv(VIEW_RENDER_FD, &q, sizeof q, MSG_TRUNC);
        while (n < 0 && errno == EINTR);
        if (n == 0) break;
        if (n != (ssize_t)sizeof q) {
            say(n < 0 ? "cannot read requests: %s" : "a garbled request%s", n < 0 ? strerror(errno) : "");
            break;
        }
        q.text[VIEW_TEXT - 1] = '\0';
        memset(&r, 0, sizeof r);
        r.op = q.op;
        r.serial = q.serial;
        struct out o = { NULL, -1, NULL, 0 };
        handle(&q, &r, &o);
        int fd = finish_out(&r, &o);
        int sent = reply(&r, fd);
        if (fd >= 0) close(fd);
        if (sent < 0) break;
    }
    close_document();
    fz_drop_context(ctx);
    return 0;
}
