/*
 * view-render: the documents of view (view-proto.h), each request on fd 3
 * answered in turn until view closes its end. It runs as the user, with
 * no_new_privs and no network, and refuses to run otherwise.
 *
 * The formats are those of Okular's generators. MuPDF reads most of them;
 * DjVu is djvulibre's and PostScript libspectre's (Ghostscript), as in
 * Okular. What MuPDF cannot read is first made into something it can:
 * plain text into UTF-8 (its encoding detected), WebP, HEIF/AVIF, JPEG XL,
 * camera RAW (LibRaw) and the images ImageMagick reads (ICO, MNG, PCX, SGI,
 * TGA, XBM, XCF, XPM, DDS, OpenEXR, Radiance HDR: view-image.c) into PAM, a
 * G3/G4 fax into PNG, DVI into PDF (dvipdfmx), gzip and bzip2 into what they
 * hold. Comic books (zip, tar, RAR, 7z or a folder) are a document of their
 * images in Okular's natural order, read by MuPDF's archive and image
 * readers, or by libarchive and the libraries above. OpenDocument text
 * (Okular's ooo generator) is made XHTML for MuPDF, CHM (its chm generator,
 * by chmlib) an EPUB of its pages; OpenRaster and Krita images are the
 * merged image they carry.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <archive.h>
#include <archive_entry.h>
#include <chm_lib.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <jxl/decode.h>
#include <libdjvu/ddjvuapi.h>
#include <libdjvu/miniexp.h>
#include <libheif/heif.h>
#include <libraw.h>
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
#include <time.h>
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

static struct heif_context *heif_new(void)
{
    static int initialized;
    if (!initialized) {
        heif_init(NULL);
        initialized = 1;
    }
    return heif_context_alloc();
}

static fz_buffer *heif_pam(const unsigned char *data, size_t n)
{
    struct heif_context *hc = heif_new();
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

/* Camera RAW, which kimageformats' raw plugin reads with LibRaw: by the
 * plugin's suffixes, or the signatures shared-mime-info knows them by (the
 * TIFF-based ones have none; their names tell). */
#define RAW_SUFFIXES "3fr arw crw cr2 cr3 dcr dng erf fff iiq k25 kdc mdc mef mos mrw nef nrw orf pef raf raw rwl rw2 " \
                     "sr2 srf srw sti x3f"

static int is_raw(const unsigned char *m, size_t n, const char *name)
{
    return ext_is(extension(name), RAW_SUFFIXES) ||
           (n >= 14 && !memcmp(m, "II\x1A\0\0\0HEAPCCDR", 14)) || (n >= 16 && !memcmp(m, "FUJIFILMCCD-RAW ", 16)) ||
           (n >= 4 && !memcmp(m, "\0MRM", 4)) || (n >= 8 && !memcmp(m, "IIRO\x08\0\0\0", 8)) ||
           (n >= 8 && !memcmp(m, "IIU\0\x08\0\0\0", 8)) || (n >= 8 && !memcmp(m, "IIU\0\x18\0\0\0", 8)) ||
           (n >= 4 && !memcmp(m, "FOVb", 4));
}

/* The image as the plugin makes it by default: the camera's white balance
 * (the image's average without one), AHD interpolation, sRGB, the camera's
 * orientation applied; 8 bits a sample here, as view shows them. */
static fz_buffer *raw_pam(const unsigned char *data, size_t n)
{
    static const char *const tuple[] = { NULL, "GRAYSCALE", NULL, "RGB", "RGB_ALPHA" };
    libraw_data_t *lr = libraw_init(0);
    if (!lr) fz_throw(ctx, FZ_ERROR_LIBRARY, "cannot start LibRaw");
    lr->params.use_camera_wb = 1;
    lr->params.use_auto_wb = 1;
    lr->params.user_qual = 3;
    lr->params.output_color = 1;
    lr->params.output_bps = 8;
    libraw_processed_image_t *img = NULL;
    fz_buffer *b = NULL;
    fz_var(img);
    fz_var(b);
    fz_try(ctx) {
        int e = libraw_open_buffer(lr, data, n);
        if (e == LIBRAW_SUCCESS && (uint64_t)lr->sizes.width * lr->sizes.height > VIEW_MAX_PIXELS) {
            fz_throw(ctx, FZ_ERROR_LIMIT, "RAW image of %d x %d pixels", lr->sizes.width, lr->sizes.height);
        }
        if (e == LIBRAW_SUCCESS) e = libraw_unpack(lr);
        if (e == LIBRAW_SUCCESS) e = libraw_dcraw_process(lr);
        if (e == LIBRAW_SUCCESS) img = libraw_dcraw_make_mem_image(lr, &e);
        if (!img) fz_throw(ctx, FZ_ERROR_FORMAT, "RAW image: %s", libraw_strerror(e));
        int w = img->width, h = img->height, c = img->colors;
        size_t size = (size_t)w * (size_t)h * (size_t)c;
        if (img->type != LIBRAW_IMAGE_BITMAP || img->bits != 8 || c < 1 || c > 4 || !tuple[c] || img->data_size < size) {
            fz_throw(ctx, FZ_ERROR_FORMAT, "RAW image: no 8-bit pixels");
        }
        if (!w || !h || (uint64_t)w * (uint64_t)h > VIEW_MAX_PIXELS) fz_throw(ctx, FZ_ERROR_LIMIT, "image of %d x %d pixels", w, h);
        b = fz_new_buffer(ctx, size + 80);
        fz_append_printf(ctx, b, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH %d\nMAXVAL 255\nTUPLTYPE %s\nENDHDR\n", w, h, c, tuple[c]);
        put(b, img->data, size);
    }
    fz_always(ctx) {
        if (img) libraw_dcraw_clear_mem(img);
        libraw_close(lr);
    }
    fz_catch(ctx) {
        fz_drop_buffer(ctx, b);
        fz_rethrow(ctx);
    }
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
    if (is_raw(data, n, name ? name : "")) return raw_pam(data, n);
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

/* EXIF, an image's TIFF-structured metadata, where each format keeps it:
 * JPEG's APP1 segment, TIFF itself, PNG's eXIf chunk, WebP's EXIF chunk
 * (libwebp's demuxer), HEIF's Exif item (libheif) and JPEG XL's Exif box;
 * the tags Gwenview shows and dates images by (Exiv2's names below). Camera
 * RAW's are LibRaw's. */
struct exif {
    char date[20];             /* Exif.Photo.DateTimeOriginal, "YYYY:MM:DD HH:MM:SS" */
    char date_image[20];       /* Exif.Image.DateTimeOriginal */
    char digitized[20];        /* Exif.Photo.DateTimeDigitized */
    char date_file[20];        /* Exif.Image.DateTime */
    char make[64], model[64];  /* Exif.Image.Make, Exif.Image.Model */
    double exposure, fnumber, focal; /* Exif.Photo.ExposureTime, FNumber, FocalLength; 0: none */
    int iso, flash;            /* Exif.Photo.ISOSpeedRatings; Exif.Photo.Flash, -1: none */
};

static unsigned exif16(const unsigned char *p, int le)
{
    return le ? p[0] | p[1] << 8 : p[0] << 8 | p[1];
}

static uint32_t exif32(const unsigned char *p, int le)
{
    return le ? p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24 : (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void exif_ascii(char *out, size_t size, const unsigned char *s, uint32_t n)
{
    size_t k = 0;
    for (uint32_t i = 0; i < n && s[i] && k + 1 < size; i++) out[k++] = s[i] < ' ' || s[i] == 0x7F ? ' ' : (char)s[i];
    while (k && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

/* The IFD at off of TIFF data t (n bytes), and the Exif IFD it points to. */
static void exif_ifd(const unsigned char *t, size_t n, int le, uint32_t off, struct exif *x, int depth)
{
    static const unsigned char sizes[] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8 };
    if (depth > 1 || off < 8 || (uint64_t)off + 2 > n) return;
    unsigned count = exif16(t + off, le);
    for (unsigned i = 0; i < count && (uint64_t)off + 2 + 12 * (i + 1) <= n; i++) {
        const unsigned char *e = t + off + 2 + 12 * i;
        unsigned tag = exif16(e, le), type = exif16(e + 2, le);
        uint32_t cnt = exif32(e + 4, le), at = exif32(e + 8, le);
        uint64_t size = type < sizeof sizes ? (uint64_t)sizes[type] * cnt : 0;
        const unsigned char *d = size <= 4 ? e + 8 : at + size <= n ? t + at : NULL;
        if (!size || !d) continue;
        double ratio = type == 5 && exif32(d + 4, le) ? (double)exif32(d, le) / exif32(d + 4, le) : 0;
        unsigned short_value = type == 3 ? exif16(d, le) : type == 4 ? exif32(d, le) : 0;
        if (tag == 0x010F && type == 2) exif_ascii(x->make, sizeof x->make, d, cnt);
        else if (tag == 0x0110 && type == 2) exif_ascii(x->model, sizeof x->model, d, cnt);
        else if (tag == 0x0132 && type == 2) exif_ascii(x->date_file, sizeof x->date_file, d, cnt);
        else if (tag == 0x9003 && type == 2 && depth) exif_ascii(x->date, sizeof x->date, d, cnt);
        else if (tag == 0x9003 && type == 2) exif_ascii(x->date_image, sizeof x->date_image, d, cnt);
        else if (tag == 0x9004 && type == 2 && depth) exif_ascii(x->digitized, sizeof x->digitized, d, cnt);
        else if (tag == 0x829A) x->exposure = ratio;
        else if (tag == 0x829D) x->fnumber = ratio;
        else if (tag == 0x920A) x->focal = ratio;
        else if (tag == 0x8827 && short_value) x->iso = (int)short_value;
        else if (tag == 0x9209 && (type == 3 || type == 4)) x->flash = (int)short_value;
        else if (tag == 0x8769 && type == 4) exif_ifd(t, n, le, at, x, depth + 1);
    }
}

static int exif_tiff(const unsigned char *t, size_t n, struct exif *x)
{
    if (n >= 6 && !memcmp(t, "Exif\0\0", 6)) {
        t += 6;
        n -= 6;
    }
    int le = n >= 8 && !memcmp(t, "II", 2);
    if (n < 8 || (!le && memcmp(t, "MM", 2)) || exif16(t + 2, le) != 42) return 0;
    exif_ifd(t, n, le, exif32(t + 4, le), x, 0);
    return 1;
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* The EXIF of image data s (n bytes, maybe only its start): 1 if found. */
static int exif_read(const unsigned char *s, size_t n, struct exif *x)
{
    memset(x, 0, sizeof *x);
    x->flash = -1;
    if (n >= 4 && (!memcmp(s, "II*\0", 4) || !memcmp(s, "MM\0*", 4))) return exif_tiff(s, n, x);
    if (n >= 4 && s[0] == 0xFF && s[1] == 0xD8) {
        for (size_t i = 2; i + 4 <= n && s[i] == 0xFF && s[i + 1] != 0xDA;) {
            size_t len = (size_t)s[i + 2] << 8 | s[i + 3];
            if (s[i + 1] == 0xE1 && len >= 8 && i + 2 + len <= n && !memcmp(s + i + 4, "Exif\0\0", 6)) {
                return exif_tiff(s + i + 4, len - 2, x);
            }
            i += 2 + len;
        }
        return 0;
    }
    if (n >= 8 && !memcmp(s, "\x89PNG\r\n\x1A\n", 8)) {
        for (size_t i = 8; i + 12 <= n;) {
            uint32_t len = be32(s + i);
            if ((uint64_t)i + 12 + len > n) break;
            if (!memcmp(s + i + 4, "eXIf", 4)) return exif_tiff(s + i + 8, len, x);
            if (!memcmp(s + i + 4, "IDAT", 4) || !memcmp(s + i + 4, "IEND", 4)) break;
            i += 12 + len;
        }
        return 0;
    }
    if (is_webp(s, n)) {
        WebPData wd = { s, n };
        WebPDemuxer *dmux = WebPDemux(&wd);
        WebPChunkIterator it;
        int found = dmux && WebPDemuxGetChunk(dmux, "EXIF", 1, &it) && exif_tiff(it.chunk.bytes, it.chunk.size, x);
        if (dmux && found) WebPDemuxReleaseChunkIterator(&it);
        WebPDemuxDelete(dmux);
        return found;
    }
    if (is_heif(s, n)) {
        struct heif_context *hc = heif_new();
        struct heif_image_handle *handle = NULL;
        heif_item_id id;
        int found = 0;
        if (hc && !heif_context_read_from_memory_without_copy(hc, s, n, NULL).code &&
            !heif_context_get_primary_image_handle(hc, &handle).code &&
            heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &id, 1) == 1) {
            size_t size = heif_image_handle_get_metadata_size(handle, id);
            unsigned char *d = size >= 4 ? malloc(size) : NULL;
            /* After a 4-byte offset to the TIFF header (ISO/IEC 23008-12). */
            if (d && !heif_image_handle_get_metadata(handle, id, d).code && 4 + (uint64_t)be32(d) < size) {
                found = exif_tiff(d + 4 + be32(d), size - 4 - be32(d), x);
            }
            free(d);
        }
        heif_image_handle_release(handle);
        heif_context_free(hc);
        return found;
    }
    if (n >= 12 && !memcmp(s, "\0\0\0\x0CJXL \r\n\x87\n", 12)) {
        for (size_t i = 0; i + 8 <= n;) {
            uint64_t len = be32(s + i);
            if (len < 8 || i + len > n) break;
            if (!memcmp(s + i + 4, "Exif", 4) && len >= 12 && 12 + (uint64_t)be32(s + i + 8) < len) {
                return exif_tiff(s + i + 12 + be32(s + i + 8), len - 12 - be32(s + i + 8), x);
            }
            i += len;
        }
    }
    return 0;
}

/* Comic books, as Okular's comicbook generator reads them: the images of a
 * zip, tar, RAR or 7z archive or of a folder (and its folders one level
 * down; for Gwenview's gallery, VIEW_GALLERY, only its own), in natural
 * order of their paths (or OPEN's: by date taken or size), one a page. Zip and tar are read
 * by MuPDF's archive code, folders from the disk, the rest by libarchive
 * into memory. A page is decoded when it is shown, by MuPDF's image code or
 * the decoders above (WebP, HEIF, JPEG XL, camera RAW, ImageMagick). */
typedef struct {
    fz_document super;
    fz_archive *arch;
    char *dir;                 /* a folder's path (its files' sizes and times) */
    char **names;
    int count, cap;
} comic_doc;

static comic_doc *comic;       /* the document, if it is one */

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
    return ext_is(e, RAW_SUFFIXES); /* camera RAW, LibRaw's (raw_pam) */
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
    fz_free(c, d->dir);
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
        /* A camera RAW is LibRaw's, TIFF-based or not. */
        if (n >= 8 && !ext_is(extension(d->names[number]), RAW_SUFFIXES) && fz_recognize_image_format(c, s) != FZ_IMAGE_UNKNOWN) {
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
 * hidden ones; down to max levels: one, or none for a gallery). */
static void comic_list_dir(comic_doc *d, const char *dir, const char *prefix, int depth, int max)
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
            if (S_ISDIR(st.st_mode) && depth < max) comic_list_dir(d, dir, name, depth + 1, max);
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

/* What is known of page i of a comic book without decoding it: its EXIF
 * (a RAW's, LibRaw's), and a folder's file's size and time. */
struct page_info {
    int64_t size;              /* -1: unknown */
    time_t mtime;              /* 0: unknown */
    struct exif x;
};

#define INFO_HEAD (256 << 10)  /* where JPEG, PNG and TIFF keep their EXIF */
#define INFO_MAX (64 << 20)    /* WebP, HEIF and JPEG XL read whole, up to this */

static void exif_time(char *out, size_t size, time_t t)
{
    struct tm tm;
    if (!t || !localtime_r(&t, &tm) || !strftime(out, size, "%Y:%m:%d %H:%M:%S", &tm)) out[0] = '\0';
}

/* An EXIF date as shown: "YYYY-MM-DD HH:MM:SS". */
static char *exif_shown(char *d)
{
    if (strlen(d) >= 10 && d[4] == ':' && d[7] == ':') d[4] = d[7] = '-';
    return d;
}

/* A RAW's metadata, as LibRaw has it, from its path or its bytes. */
static void raw_info(const char *path, const unsigned char *s, size_t n, struct exif *x)
{
    libraw_data_t *lr = libraw_init(0);
    if (!lr) return;
    if ((path ? libraw_open_file(lr, path) : libraw_open_buffer(lr, s, n)) == LIBRAW_SUCCESS) {
        exif_time(x->date, sizeof x->date, lr->other.timestamp);
        snprintf(x->make, sizeof x->make, "%s", lr->idata.make);
        snprintf(x->model, sizeof x->model, "%s", lr->idata.model);
        x->exposure = lr->other.shutter > 0 ? lr->other.shutter : 0;
        x->fnumber = lr->other.aperture > 0 ? lr->other.aperture : 0;
        x->focal = lr->other.focal_len > 0 ? lr->other.focal_len : 0;
        x->iso = lr->other.iso_speed > 0 ? (int)lroundf(lr->other.iso_speed) : 0;
    }
    libraw_close(lr);
}

static void comic_info(comic_doc *d, int i, struct page_info *info)
{
    const char *name = d->names[i];
    char full[PATH_MAX];
    struct stat st;
    memset(info, 0, sizeof *info);
    info->size = -1;
    info->x.flash = -1;
    int file = d->dir && snprintf(full, sizeof full, "%s/%s", d->dir, name) < (int)sizeof full;
    if (file && stat(full, &st) == 0) {
        info->size = st.st_size;
        info->mtime = st.st_mtime;
    }
    int raw = ext_is(extension(name), RAW_SUFFIXES);
    if (raw && file) {
        raw_info(full, NULL, 0, &info->x);
        return;
    }
    unsigned char *head = NULL, *whole = NULL;
    size_t n = 0;
    fz_buffer *b = NULL;
    fz_var(head);
    fz_var(whole);
    fz_var(b);
    fz_try(ctx) {
        if (file) {
            int fd = open(full, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
            head = fd >= 0 ? malloc(INFO_HEAD) : NULL;
            ssize_t got = head ? pread(fd, head, INFO_HEAD, 0) : -1;
            n = got > 0 ? (size_t)got : 0;
            if (n == INFO_HEAD && info->size > INFO_HEAD && info->size <= INFO_MAX &&
                (is_webp(head, n) || is_heif(head, n) || is_jxl(head, n)) && (whole = malloc((size_t)info->size))) {
                got = pread(fd, whole, (size_t)info->size, 0);
                n = got > 0 ? (size_t)got : 0;
            }
            if (fd >= 0) close(fd);
            exif_read(whole ? whole : head, n, &info->x);
        } else {
            b = fz_read_archive_entry(ctx, d->arch, name);
            unsigned char *s;
            n = fz_buffer_storage(ctx, b, &s);
            info->size = (int64_t)n;
            if (raw) raw_info(NULL, s, n, &info->x);
            else exif_read(s, n, &info->x);
        }
    }
    fz_always(ctx) {
        free(head);
        free(whole);
        fz_drop_buffer(ctx, b);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        say("comic book page %s: cannot read its information", name);
    }
}

/* The date Gwenview dates an image by (timeutils.cpp dateTimeForFileItem):
 * the first of Exif.Photo.DateTimeOriginal, Exif.Image.DateTimeOriginal,
 * Exif.Photo.DateTimeDigitized and Exif.Image.DateTime that is a date (in
 * local time), else the file's time. */
static void page_date(const struct page_info *info, char *out, size_t size)
{
    const char *const tags[] = { info->x.date, info->x.date_image, info->x.digitized, info->x.date_file };
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++) {
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        const char *end = strptime(tags[i], "%Y:%m:%d %H:%M:%S", &tm);
        if (end && !*end) {
            snprintf(out, size, "%s", tags[i]);
            return;
        }
    }
    exif_time(out, size, info->mtime);
}

/* KIO's order of names (Gwenview's, KDirSortFilterProxyModel): natural,
 * case-insensitive, then case-sensitive. */
static int kio_compare(const void *pa, const void *pb)
{
    const char *a = *(char *const *)pa, *b = *(char *const *)pb;
    char fa[PATH_MAX], fb[PATH_MAX];
    size_t i;
    for (i = 0; a[i] && i + 1 < sizeof fa; i++) fa[i] = (char)tolower((unsigned char)a[i]);
    fa[i] = '\0';
    for (i = 0; b[i] && i + 1 < sizeof fb; i++) fb[i] = (char)tolower((unsigned char)b[i]);
    fb[i] = '\0';
    const char *pfa = fa, *pfb = fb;
    int r = natural_compare(&pfa, &pfb);
    return r ? r : strcmp(a, b);
}

struct comic_key {
    char *name;
    char date[20];
    int64_t size;
    time_t mtime;
};
static int sort_descending, sort_kio;

/* Gwenview's: by date, then the file's time; by size; then the name. */
static int by_key(const void *pa, const void *pb, int date)
{
    const struct comic_key *a = pa, *b = pb;
    int r = date ? strcmp(a->date, b->date) : (a->size > b->size) - (a->size < b->size);
    if (!r && date) r = (a->mtime > b->mtime) - (a->mtime < b->mtime);
    if (!r) r = sort_kio ? kio_compare(&a->name, &b->name) : natural_compare(&a->name, &b->name);
    return sort_descending ? -r : r;
}

static int by_date(const void *a, const void *b) { return by_key(a, b, 1); }
static int by_size(const void *a, const void *b) { return by_key(a, b, 0); }

static int by_name(const void *a, const void *b)
{
    int r = sort_kio ? kio_compare(a, b) : natural_compare(a, b);
    return sort_descending ? -r : r;
}

/* The pages in OPEN's order (mode: VIEW_SORT_*): a gallery's names in
 * KIO's order, a comic book's in Okular's. */
static void comic_sort(comic_doc *d, int mode)
{
    int key = mode & VIEW_SORT_MASK;
    sort_descending = (mode & VIEW_SORT_DESCENDING) != 0;
    sort_kio = (mode & VIEW_GALLERY) != 0;
    if (key != VIEW_SORT_DATE && key != VIEW_SORT_SIZE) {
        qsort(d->names, (size_t)d->count, sizeof *d->names, by_name);
        return;
    }
    struct comic_key *k = fz_malloc_array(ctx, d->count, struct comic_key);
    for (int i = 0; i < d->count; i++) {
        struct page_info info;
        comic_info(d, i, &info);
        k[i].name = d->names[i];
        k[i].size = info.size;
        k[i].mtime = info.mtime;
        page_date(&info, k[i].date, sizeof k[i].date);
    }
    qsort(k, (size_t)d->count, sizeof *k, key == VIEW_SORT_DATE ? by_date : by_size);
    for (int i = 0; i < d->count; i++) d->names[i] = k[i].name;
    fz_free(ctx, k);
}

enum { COMIC_MUPDF, COMIC_DIR, COMIC_LIBARCHIVE };

static fz_document *open_comic(const char *path, int how, int mode)
{
    comic_doc *d = fz_new_derived_document(ctx, comic_doc);
    d->super.drop_document = comic_drop;
    d->super.count_pages = comic_count;
    d->super.load_page = comic_load;
    fz_try(ctx) {
        if (how == COMIC_DIR) {
            d->arch = fz_open_directory(ctx, path);
            d->dir = fz_strdup(ctx, path);
            comic_list_dir(d, path, "", 0, mode & VIEW_GALLERY ? 0 : 1);
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
        comic_sort(d, mode);
    }
    fz_catch(ctx) {
        fz_drop_document(ctx, &d->super);
        fz_rethrow(ctx);
    }
    comic = d;
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
static int open_mode;                 /* OPEN's mode: a comic book's order (VIEW_SORT_*), VIEW_GALLERY */
static char open_path[PATH_MAX];      /* the document's file */

static void close_document(void)
{
    vp_detach();
    drop_page();
    fz_drop_document(ctx, doc);
    doc = NULL;
    comic = NULL;
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

/* A zip whose first entry is "mimetype", stored, holding type: how
 * OpenDocument, OpenRaster and Krita files say what they are. */
static int zip_type_is(const unsigned char *m, size_t n, const char *type)
{
    size_t len = strlen(type);
    if (n < 40 + len) return 0;
    uint32_t size = m[22] | m[23] << 8 | m[24] << 16 | (uint32_t)m[25] << 24;
    return !memcmp(m, "PK\003\004", 4) && !m[8] && !m[9] && m[26] == 8 && !m[27] && !m[28] && !m[29] &&
           (size == len || (m[6] & 8)) && !memcmp(m + 30, "mimetype", 8) && !memcmp(m + 38, type, len) &&
           !memcmp(m + 38 + len, "PK", 2);
}

/* OpenRaster and Krita, as kimageformats' ora and kra plugins read them:
 * the merged image they carry. */
static void open_merged(const char *path)
{
    fz_archive *zip = fz_open_archive(ctx, path);
    fz_buffer *b = NULL;
    fz_try(ctx) {
        b = fz_try_read_archive_entry(ctx, zip, "mergedimage.png");
        if (!b) fz_throw(ctx, FZ_ERROR_FORMAT, "no merged image (mergedimage.png) in this file");
    }
    fz_always(ctx) fz_drop_archive(ctx, zip);
    fz_catch(ctx) fz_rethrow(ctx);
    open_buffer("png", b);
    set_format("Image", 0);
}

/* OpenDocument text, which Okular's ooo generator read (to Okular 19.12),
 * and its flat XML (.fodt): the text of content.xml made XHTML, reflowed
 * by MuPDF as an EPUB is. The styles (styles.xml's and the automatic ones)
 * are each element's inline CSS, their parents' first; font sizes are made
 * relative to the default paragraph's, so the text grows with the layout's
 * size; headings are h1 to h6 (the outline); lists, tables, links,
 * bookmarks, the pictures the zip holds (or the flat file's base64 ones),
 * and the footnotes at the end. Annotations and deleted changes are not
 * shown, as in Okular. */
#define ODT_TYPE "application/vnd.oasis.opendocument.text"

struct odt_style {
    const char *family, *name, *parent; /* in the XML, kept while converting */
    char css[480];
    float pt, pct;     /* fo:font-size in points, or a fraction of the parent's (0: not set) */
    char num[10];      /* a list style's levels: their number format ('1', 'a', ...), 0 for bullets */
};

static struct {
    struct odt_style *s;
    int count, cap;
    float base;        /* the default paragraph's font size, in points */
} odt;

/* The XHTML being made: where it goes, and the font size and list here. */
struct odt_walk {
    fz_buffer *out, *notes;
    fz_archive *zip;
    float size;        /* a factor of the default size */
    const char *list;
    int level, depth;
};

static fz_xml *odt_down(fz_xml *x, const char *tag)
{
    return x ? fz_xml_find_down(x, tag) : NULL;
}

static const char *odt_text(fz_xml *x)
{
    x = x ? fz_xml_down(x) : NULL;
    return x ? fz_xml_text(x) : NULL;
}

static void put_escaped(fz_buffer *b, const char *s)
{
    for (; s && *s; s++) {
        if (*s == '&') fz_append_string(ctx, b, "&amp;");
        else if (*s == '<') fz_append_string(ctx, b, "&lt;");
        else if (*s == '>') fz_append_string(ctx, b, "&gt;");
        else if (*s == '"') fz_append_string(ctx, b, "&quot;");
        else if ((unsigned char)*s >= 0x20 || *s == '\t' || *s == '\n' || *s == '\r') fz_append_byte(ctx, b, *s);
    }
}

/* A length ("12pt", "0.5in", "1cm", ...) in points; 0 if it is not one. */
static float odt_points(const char *v)
{
    char *end;
    float f = v ? strtof(v, &end) : 0;
    if (!(f > 0) || !isfinite(f)) return 0;
    return !strcmp(end, "pt") ? f : !strcmp(end, "in") ? f * 72 : !strcmp(end, "cm") ? f * 72 / 2.54f :
           !strcmp(end, "mm") ? f * 72 / 25.4f : !strcmp(end, "pc") ? f * 12 : !strcmp(end, "px") ? f * 0.75f : 0;
}

/* "prop:value;" appended to css, the value as CSS has it; values of other
 * characters than lengths, colours and names have are left out. */
static void odt_css(char *css, size_t size, const char *prop, const char *v)
{
    if (!v || !*v) return;
    for (const char *s = v; *s; s++) if (!isalnum((unsigned char)*s) && !strchr(" .,%#+-()", *s)) return;
    if (!strcmp(prop, "text-align")) v = !strcmp(v, "start") ? "left" : !strcmp(v, "end") ? "right" : v;
    if (!strncmp(prop, "page-break-", 11)) {
        if (strcmp(v, "page")) return;
        v = "always";
    }
    size_t n = strlen(css);
    if (n + strlen(prop) + strlen(v) + 3 <= size) snprintf(css + n, size - n, "%s:%s;", prop, v);
}

static const char *const odt_props[][2] = {
    { "fo:font-weight", "font-weight" }, { "fo:font-style", "font-style" }, { "fo:font-variant", "font-variant" },
    { "fo:text-transform", "text-transform" }, { "fo:color", "color" }, { "fo:background-color", "background-color" },
    { "fo:text-align", "text-align" }, { "fo:text-indent", "text-indent" }, { "fo:line-height", "line-height" },
    { "fo:margin", "margin" }, { "fo:margin-top", "margin-top" }, { "fo:margin-bottom", "margin-bottom" },
    { "fo:margin-left", "margin-left" }, { "fo:margin-right", "margin-right" }, { "fo:padding", "padding" },
    { "fo:padding-top", "padding-top" }, { "fo:padding-bottom", "padding-bottom" }, { "fo:padding-left", "padding-left" },
    { "fo:padding-right", "padding-right" }, { "fo:border", "border" }, { "fo:border-top", "border-top" },
    { "fo:border-bottom", "border-bottom" }, { "fo:border-left", "border-left" }, { "fo:border-right", "border-right" },
    { "fo:break-before", "page-break-before" }, { "fo:break-after", "page-break-after" },
};

/* A style:style (or, family "list", a text:list-style) kept. */
static void odt_add_style(fz_xml *x, const char *family)
{
    const char *name = fz_xml_att(x, "style:name");
    if (!name) return;
    if (odt.count == odt.cap) {
        odt.cap = odt.cap ? odt.cap * 2 : 64;
        odt.s = fz_realloc_array(ctx, odt.s, odt.cap, struct odt_style);
    }
    struct odt_style *st = &odt.s[odt.count++];
    memset(st, 0, sizeof *st);
    st->family = family ? family : "";
    st->name = name;
    st->parent = fz_xml_att(x, "style:parent-style-name");
    for (fz_xml *p = fz_xml_down(x); p; p = fz_xml_next(p)) {
        const char *tag = fz_xml_tag(p);
        if (!tag) continue;
        if (!strcmp(st->family, "list")) {
            const char *l = fz_xml_att(p, "text:level"), *f = fz_xml_att(p, "style:num-format");
            int level = l ? atoi(l) : 0;
            if (level >= 1 && level <= 10 && !strcmp(tag, "list-level-style-number")) st->num[level - 1] = f && *f ? f[0] : '1';
            continue;
        }
        if (!strstr(tag, "-properties")) continue;
        for (size_t i = 0; i < sizeof odt_props / sizeof *odt_props; i++) {
            odt_css(st->css, sizeof st->css, odt_props[i][1], fz_xml_att(p, odt_props[i][0]));
        }
        const char *u = fz_xml_att(p, "style:text-underline-style"), *l = fz_xml_att(p, "style:text-line-through-style");
        int under = u && strcmp(u, "none"), through = l && strcmp(l, "none");
        if (under || through) {
            odt_css(st->css, sizeof st->css, "text-decoration", under && through ? "underline line-through" : under ? "underline" : "line-through");
        }
        const char *pos = fz_xml_att(p, "style:text-position");
        if (pos) {
            odt_css(st->css, sizeof st->css, "vertical-align", !strncmp(pos, "sub", 3) || pos[0] == '-' ? "sub" :
                    !strncmp(pos, "0%", 2) || !strcmp(pos, "0") ? "baseline" : "super");
        }
        const char *size = fz_xml_att(p, "fo:font-size");
        if (size && strchr(size, '%')) st->pct = strtof(size, NULL) / 100;
        else if (size) st->pt = odt_points(size);
    }
}

static void odt_styles(fz_xml *container)
{
    for (fz_xml *x = container ? fz_xml_down(container) : NULL; x; x = fz_xml_next(x)) {
        if (fz_xml_is_tag(x, "style")) {
            odt_add_style(x, fz_xml_att(x, "style:family"));
        } else if (fz_xml_is_tag(x, "list-style")) {
            odt_add_style(x, "list");
        } else if (fz_xml_is_tag(x, "default-style") && fz_xml_att_eq(x, "style:family", "paragraph")) {
            float pt = odt_points(fz_xml_att(odt_down(x, "text-properties"), "fo:font-size"));
            if (pt > 0) odt.base = pt;
        }
    }
}

/* The last one of that name: content.xml's are after styles.xml's. */
static const struct odt_style *odt_find(const char *family, const char *name)
{
    for (int i = odt.count - 1; name && i >= 0; i--) {
        if (!strcmp(odt.s[i].name, name) && !strcmp(odt.s[i].family, family)) return &odt.s[i];
    }
    return NULL;
}

/* A style's font size as a factor of the default: *abs when a style of its
 * chain gives it in points, else a factor of the size around it. */
static float odt_size(const struct odt_style *st, int *abs, int depth)
{
    *abs = 0;
    if (!st || depth > 16) return 1;
    if (st->pt > 0) {
        *abs = 1;
        return st->pt / odt.base;
    }
    float parent = odt_size(odt_find(st->family, st->parent), abs, depth + 1);
    return st->pct > 0 ? parent * st->pct : parent;
}

static void odt_chain_css(fz_buffer *b, const struct odt_style *st, int depth)
{
    if (!st || depth > 16) return;
    odt_chain_css(b, odt_find(st->family, st->parent), depth + 1);
    fz_append_string(ctx, b, st->css);
}

/* ' style="..."': extra, then the element's style (of family, named by its
 * attribute att) with its font size in em; *size: the size inside it. */
static void odt_style_att(struct odt_walk *w, fz_xml *x, const char *family, const char *att, const char *extra, float *size)
{
    const struct odt_style *st = att ? odt_find(family, fz_xml_att(x, att)) : NULL;
    *size = w->size;
    if (!st && !extra) return;
    fz_append_printf(ctx, w->out, " style=\"%s", extra ? extra : "");
    odt_chain_css(w->out, st, 0);
    int abs;
    float f = st ? odt_size(st, &abs, 0) : 1, em = 1;
    if (st && abs) {
        em = f / w->size;
        *size = f;
    } else if (st) {
        em = f;
        *size = w->size * f;
    }
    if (fabsf(em - 1) > 0.001f && em > 0.01f && em < 100) {
        char v[32];
        snprintf(v, sizeof v, "font-size:%.3fem;", em);
        fz_append_string(ctx, w->out, v);
    }
    fz_append_byte(ctx, w->out, '"');
}

static void odt_walk(struct odt_walk *w, fz_xml *x);

static void odt_children(const struct odt_walk *w, fz_xml *x, float size)
{
    struct odt_walk in = *w;
    in.size = size;
    if (++in.depth > 200) return;
    for (fz_xml *c = fz_xml_down(x); c; c = fz_xml_next(c)) odt_walk(&in, c);
}

/* A frame's picture (the first MuPDF can read: a file of the zip, or base64
 * data in a flat file) at the frame's size, or its text box. */
static void odt_frame(struct odt_walk *w, fz_xml *frame)
{
    char css[160] = "";
    odt_css(css, sizeof css, "width", fz_xml_att(frame, "svg:width"));
    odt_css(css, sizeof css, "height", fz_xml_att(frame, "svg:height"));
    for (fz_xml *c = fz_xml_down(frame); c; c = fz_xml_next(c)) {
        if (fz_xml_is_tag(c, "text-box")) {
            fz_append_string(ctx, w->out, "<div>");
            odt_children(w, c, w->size);
            fz_append_string(ctx, w->out, "</div>");
            return;
        }
        if (!fz_xml_is_tag(c, "image")) continue;
        const char *href = fz_xml_att(c, "xlink:href"), *data = odt_text(odt_down(c, "binary-data"));
        if (href && w->zip && ext_is(extension(href), "png jpg jpeg jpe gif bmp tif tiff jp2 jpx jxr svg") &&
            fz_has_archive_entry(ctx, w->zip, href)) {
            fz_append_string(ctx, w->out, "<img alt=\"\" src=\"");
            put_escaped(w->out, href);
        } else if (data) {
            /* Its first bytes tell what it is. */
            char head[16];
            int n = 0;
            for (const char *s = data; *s && n < (int)sizeof head; s++) if (!isspace((unsigned char)*s)) head[n++] = *s;
            fz_buffer *b = fz_new_buffer_from_base64(ctx, head, (size_t)n);
            unsigned char *p;
            size_t len = fz_buffer_storage(ctx, b, &p);
            int svg = len >= 4 && (!memcmp(p, "<?xm", 4) || !memcmp(p, "<svg", 4));
            int image = len >= 8 && fz_recognize_image_format(ctx, p) != FZ_IMAGE_UNKNOWN;
            fz_drop_buffer(ctx, b);
            if (!svg && !image) continue;
            fz_append_printf(ctx, w->out, "<img alt=\"\" src=\"data:image/%s;base64,", svg ? "svg+xml" : "png");
            for (const char *s = data; *s; s++) if (isalnum((unsigned char)*s) || *s == '+' || *s == '/' || *s == '=') fz_append_byte(ctx, w->out, *s);
        } else {
            continue;
        }
        fz_append_printf(ctx, w->out, "\" style=\"%s\"/>", css);
        return;
    }
}

static void odt_walk(struct odt_walk *w, fz_xml *x)
{
    const char *tag = fz_xml_tag(x), *v;
    fz_buffer *out = w->out;
    float size;
    if (!tag) {
        put_escaped(out, fz_xml_text(x));
        return;
    }
    size_t len = strlen(tag);
    if (!strcmp(tag, "p") || !strcmp(tag, "h")) {
        char name[4] = "p";
        if (tag[0] == 'h') {
            int level = (v = fz_xml_att(x, "text:outline-level")) ? atoi(v) : 1;
            snprintf(name, sizeof name, "h%d", level < 1 ? 1 : level > 6 ? 6 : level);
        }
        fz_append_printf(ctx, out, "<%s", name);
        odt_style_att(w, x, "paragraph", "text:style-name", NULL, &size);
        fz_append_byte(ctx, out, '>');
        if (!fz_xml_down(x)) fz_append_string(ctx, out, "&#160;"); /* an empty paragraph keeps its line */
        odt_children(w, x, size);
        fz_append_printf(ctx, out, "</%s>", name);
    } else if (!strcmp(tag, "span")) {
        fz_append_string(ctx, out, "<span");
        odt_style_att(w, x, "text", "text:style-name", NULL, &size);
        fz_append_byte(ctx, out, '>');
        odt_children(w, x, size);
        fz_append_string(ctx, out, "</span>");
    } else if (!strcmp(tag, "a") && (v = fz_xml_att(x, "xlink:href"))) {
        fz_append_string(ctx, out, "<a href=\"");
        put_escaped(out, v);
        fz_append_string(ctx, out, "\">");
        odt_children(w, x, w->size);
        fz_append_string(ctx, out, "</a>");
    } else if (!strcmp(tag, "s")) {
        int n = (v = fz_xml_att(x, "text:c")) ? atoi(v) : 1;
        for (int i = 0; i < (n < 1 ? 1 : n > 1000 ? 1000 : n); i++) fz_append_string(ctx, out, "&#160;");
    } else if (!strcmp(tag, "tab")) {
        fz_append_string(ctx, out, "&#160;&#160;&#160;&#160;"); /* as Okular */
    } else if (!strcmp(tag, "line-break")) {
        fz_append_string(ctx, out, "<br/>");
    } else if ((!strcmp(tag, "bookmark") || !strcmp(tag, "bookmark-start")) && (v = fz_xml_att(x, "text:name"))) {
        fz_append_string(ctx, out, "<a id=\"");
        put_escaped(out, v);
        fz_append_string(ctx, out, "\"></a>");
    } else if (!strcmp(tag, "list")) {
        struct odt_walk in = *w;
        in.list = (v = fz_xml_att(x, "text:style-name")) ? v : w->list;
        in.level = w->level + 1;
        const struct odt_style *st = odt_find("list", in.list);
        char f = st && in.level <= 10 ? st->num[in.level - 1] : 0;
        if (f) {
            fz_append_printf(ctx, out, "<ol style=\"list-style-type:%s\">", f == 'a' ? "lower-alpha" : f == 'A' ? "upper-alpha" :
                             f == 'i' ? "lower-roman" : f == 'I' ? "upper-roman" : "decimal");
        } else {
            fz_append_string(ctx, out, "<ul>");
        }
        for (fz_xml *c = fz_xml_down(x); c; c = fz_xml_next(c)) {
            int item = fz_xml_is_tag(c, "list-item"), header = fz_xml_is_tag(c, "list-header");
            if (!item && !header) continue;
            fz_append_string(ctx, out, header ? "<li style=\"list-style-type:none\">" : "<li>");
            odt_children(&in, c, w->size);
            fz_append_string(ctx, out, "</li>");
        }
        fz_append_string(ctx, out, f ? "</ol>" : "</ul>");
    } else if (!strcmp(tag, "table")) {
        fz_append_string(ctx, out, "<table");
        odt_style_att(w, x, "table", "table:style-name", "border-collapse:collapse;", &size);
        fz_append_byte(ctx, out, '>');
        odt_children(w, x, size);
        fz_append_string(ctx, out, "</table>");
    } else if (!strcmp(tag, "table-row")) {
        fz_append_string(ctx, out, "<tr>");
        odt_children(w, x, w->size);
        fz_append_string(ctx, out, "</tr>");
    } else if (!strcmp(tag, "table-cell")) {
        fz_append_string(ctx, out, "<td");
        int cols = (v = fz_xml_att(x, "table:number-columns-spanned")) ? atoi(v) : 1;
        int rows = (v = fz_xml_att(x, "table:number-rows-spanned")) ? atoi(v) : 1;
        if (cols > 1 && cols < 1000) fz_append_printf(ctx, out, " colspan=\"%d\"", cols);
        if (rows > 1 && rows < 1000) fz_append_printf(ctx, out, " rowspan=\"%d\"", rows);
        odt_style_att(w, x, "table-cell", "table:style-name", NULL, &size);
        fz_append_byte(ctx, out, '>');
        odt_children(w, x, size);
        fz_append_string(ctx, out, "</td>");
    } else if (!strcmp(tag, "frame")) {
        odt_frame(w, x);
    } else if (!strcmp(tag, "note")) {
        const char *cit = odt_text(odt_down(x, "note-citation"));
        fz_append_string(ctx, out, "<sup>");
        put_escaped(out, cit ? cit : "*");
        fz_append_string(ctx, out, "</sup>");
        if (w->notes) {
            struct odt_walk in = *w;
            in.out = w->notes;
            in.notes = NULL;
            fz_append_string(ctx, w->notes, "<div><sup>");
            put_escaped(w->notes, cit ? cit : "*");
            fz_append_string(ctx, w->notes, "</sup>");
            fz_xml *body = odt_down(x, "note-body");
            if (body) odt_children(&in, body, 1);
            fz_append_string(ctx, w->notes, "</div>");
        }
    } else if (!strcmp(tag, "annotation") || !strcmp(tag, "tracked-changes") || !strcmp(tag, "forms") ||
               !strcmp(tag, "table-columns") || !strcmp(tag, "table-column") || !strcmp(tag, "covered-table-cell") ||
               (len > 6 && !strcmp(tag + len - 6, "-decls")) || (len > 7 && !strcmp(tag + len - 7, "-source"))) {
        /* Not shown: annotations, deleted text, declarations, an index's template. */
    } else {
        odt_children(w, x, w->size); /* sections, fields, shapes, indexes: their text */
    }
}

static fz_buffer *odt_html(fz_xml *content, fz_xml *styles, fz_xml *meta, fz_archive *zip)
{
    fz_buffer *out = fz_new_buffer(ctx, 65536), *notes = NULL;
    fz_var(notes);
    odt.base = 12;
    odt.count = 0;
    fz_try(ctx) {
        notes = fz_new_buffer(ctx, 1024);
        odt_styles(odt_down(styles, "styles"));
        odt_styles(odt_down(content, "styles")); /* a flat file's */
        odt_styles(odt_down(content, "automatic-styles"));
        fz_xml *text = odt_down(odt_down(content, "body"), "text");
        if (!text) fz_throw(ctx, FZ_ERROR_FORMAT, "no text in this OpenDocument file");
        fz_append_string(ctx, out, "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>");
        put_escaped(out, odt_text(odt_down(odt_down(meta, "meta"), "title")));
        fz_append_string(ctx, out, "</title></head><body>");
        struct odt_walk w = { out, notes, zip, 1, NULL, 0, 0 };
        odt_children(&w, text, 1);
        unsigned char *s;
        size_t n = fz_buffer_storage(ctx, notes, &s);
        if (n) {
            fz_append_string(ctx, out, "<hr/>");
            put(out, s, n);
        }
        fz_append_string(ctx, out, "</body></html>");
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, notes);
        fz_free(ctx, odt.s);
        odt.s = NULL;
        odt.count = odt.cap = 0;
    }
    fz_catch(ctx) {
        fz_drop_buffer(ctx, out);
        fz_rethrow(ctx);
    }
    return out;
}

static void open_odt(const char *path, int flat)
{
    fz_archive *zip = NULL;
    fz_xml *content = NULL, *styles = NULL, *meta = NULL;
    fz_buffer *raw = NULL, *html = NULL;
    fz_stream *stm = NULL;
    fz_var(zip);
    fz_var(content);
    fz_var(styles);
    fz_var(meta);
    fz_var(raw);
    fz_var(html);
    fz_var(stm);
    fz_try(ctx) {
        if (flat) {
            raw = fz_read_file(ctx, path);
            content = fz_parse_xml(ctx, raw, 1);
        } else {
            zip = fz_open_archive(ctx, path);
            raw = fz_try_read_archive_entry(ctx, zip, "META-INF/manifest.xml");
            unsigned char *s;
            size_t n = raw ? fz_buffer_storage(ctx, raw, &s) : 0;
            if (n && memmem(s, n, "encryption-data", 15)) fz_throw(ctx, FZ_ERROR_UNSUPPORTED, "Encrypted OpenDocument files cannot be read");
            content = fz_parse_xml_archive_entry(ctx, zip, "content.xml", 1);
            styles = fz_try_parse_xml_archive_entry(ctx, zip, "styles.xml", 1);
            meta = fz_try_parse_xml_archive_entry(ctx, zip, "meta.xml", 1);
        }
        fz_xml *root = fz_xml_root(content);
        html = odt_html(root, styles ? fz_xml_root(styles) : NULL, meta ? fz_xml_root(meta) : root, zip);
        stm = fz_open_buffer(ctx, html);
        /* Its pictures are read from the zip as the pages are laid out. */
        doc = fz_open_document_with_stream_and_dir(ctx, "application/xhtml+xml", stm, zip);
    }
    fz_always(ctx) {
        fz_drop_stream(ctx, stm);
        fz_drop_buffer(ctx, html);
        fz_drop_buffer(ctx, raw);
        fz_drop_xml(ctx, meta);
        fz_drop_xml(ctx, styles);
        fz_drop_xml(ctx, content);
        fz_drop_archive(ctx, zip);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    set_format("OpenDocument text", 1);
}

/* CHM, as Okular's chm generator shows it (generator_chm.cpp, ebook_chm.cpp):
 * its HTML pages in the order of the table of contents (the binary one,
 * #TOCIDX, where there is one, else the .hhc sitemap), then the archive's
 * other pages, the home page first; the table of contents is the outline,
 * the title #SYSTEM's. Here the pages are an EPUB's chapters, reflowed by
 * MuPDF: chmlib's files are an archive of MuPDF's, with a package file and
 * an NCX made for them. A page that is not UTF-8 and says no encoding is
 * read in the code page of the file's language (its LCID), as Okular
 * guesses it; the table of contents and the title too. */
#define CHM_OPF "view-chm.opf"
#define CHM_NCX "view-chm.ncx"

typedef struct {
    fz_archive super;
    struct chmFile *chm;
    const char *cp;
} chm_archive;

struct chm_entry {
    int level;
    char *name;            /* XML text */
    char *path, *frag;     /* the page (NULL: none) and the place on it */
};

struct chm_book {
    const char *cp;
    struct chm_entry *toc;
    int ntoc, tcap;
    char **files;          /* the archive's, as chmlib lists them ("/dir/name") */
    int nfiles, fcap;
    int steps;             /* left for the binary table of contents */
};

/* Windows' code page for the language, as ebook_chm_encoding.cpp has them. */
static const char *chm_codepage(unsigned lcid)
{
    switch (lcid) {
    case 0x0804: case 0x1004: return "GB18030";
    case 0x0404: case 0x1404: return "BIG5";
    case 0x0C04: return "BIG5-HKSCS";
    case 0x0C1A: case 0x082C: case 0x0843: return "CP1251";
    case 0x042C: case 0x0443: return "CP1254";
    }
    switch (lcid & 0x3FF) {
    case 0x01: case 0x20: case 0x29: return "CP1256";
    case 0x25: case 0x26: case 0x27: return "CP1257";
    case 0x05: case 0x0E: case 0x15: case 0x18: case 0x1A: case 0x1B: case 0x1C: case 0x24: return "CP1250";
    case 0x02: case 0x19: case 0x22: case 0x23: case 0x2F: case 0x44: return "CP1251";
    case 0x08: return "CP1253";
    case 0x0D: return "CP1255";
    case 0x11: return "CP932";
    case 0x12: return "CP949";
    case 0x1E: return "CP874";
    case 0x1F: return "CP1254";
    case 0x2A: return "CP1258";
    case 0x49: return "TSCII";
    default: return "CP1252";
    }
}

static uint32_t le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* word in the n bytes at s, in any case: where, or NULL. */
static char *find_case(const char *s, size_t n, const char *word)
{
    size_t k = strlen(word);
    for (size_t i = 0; i + k <= n; i++) if (!strncasecmp(s + i, word, k)) return (char *)s + i;
    return NULL;
}

/* name ("dir/file", or "/dir/file") in the CHM: its unit in *ui. */
static int chm_find(struct chmFile *h, const char *name, struct chmUnitInfo *ui)
{
    char path[CHM_MAX_PATHLEN + 1];
    return snprintf(path, sizeof path, "/%s", name + (name[0] == '/')) < (int)sizeof path &&
           chm_resolve_object(h, path, ui) == CHM_RESOLVE_SUCCESS;
}

/* A file of the CHM, or NULL if it has none. */
static fz_buffer *chm_read(struct chmFile *h, const char *name)
{
    struct chmUnitInfo ui;
    if (!chm_find(h, name, &ui)) return NULL;
    if (ui.length > MAX_UNPACKED) fz_throw(ctx, FZ_ERROR_LIMIT, "%s is too large", name);
    fz_buffer *b = fz_new_buffer(ctx, (size_t)ui.length + 1);
    if (ui.length && chm_retrieve_object(h, &ui, b->data, 0, (LONGINT64)ui.length) != (LONGINT64)ui.length) {
        fz_drop_buffer(ctx, b);
        fz_throw(ctx, FZ_ERROR_FORMAT, "cannot read %s of this CHM file", name);
    }
    b->len = (size_t)ui.length;
    return b;
}

/* s appended to out in UTF-8: as it is if it is UTF-8, else from the code
 * page (or Latin-1, where that fails). */
static void chm_text(fz_buffer *out, const unsigned char *s, size_t n, const char *cp)
{
    unsigned char *o;
    size_t at = fz_buffer_storage(ctx, out, &o);
    if (valid_utf8(s, n)) {
        put(out, s, n);
    } else if (convert(out, cp, s, n) < 0) {
        fz_resize_buffer(ctx, out, at);
        for (size_t i = 0; i < n; i++) fz_append_rune(ctx, out, s[i]);
    }
}

/* Where a page says its encoding: an XML declaration's "encoding=", else
 * the first <meta> tag's "charset=" (http-equiv's, or HTML5's); NULL. */
static char *chm_declaration(char *s)
{
    char *x = strstr(s, "<?xml"), *at = NULL;
    if (x && x < s + strcspn(s, ">")) at = find_case(x, strcspn(x, ">"), "encoding=");
    for (char *m = strcasestr(s, "<meta"); !at && m; m = strcasestr(m + 5, "<meta")) at = find_case(m, strcspn(m, ">"), "charset=");
    return at;
}

/* A page for MuPDF, in UTF-8: by its BOM, else the encoding it says, else
 * as it is if it is UTF-8, else from the code page; what it says of its
 * encoding is then made nothing ("encoding=", "charset=" lose their "="),
 * so MuPDF does not convert it again: its own conversion, of the encodings
 * it knows, leaks the text when the page is then not XML (MuPDF 1.28). */
static fz_buffer *chm_page(fz_buffer *b, const char *cp)
{
    char enc[64] = "", *at = chm_declaration((char *)fz_string_from_buffer(ctx, b));
    unsigned char *s;
    size_t n = fz_buffer_storage(ctx, b, &s), k = 0;
    if (n >= 2 && (!memcmp(s, "\xFF\xFE", 2) || !memcmp(s, "\xFE\xFF", 2))) {
        snprintf(enc, sizeof enc, "UTF-16");
    } else if (n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) {
        snprintf(enc, sizeof enc, "UTF-8");
    } else if (at) {
        const char *v = strchr(at, '=') + 1;
        v += strspn(v, "\"' ");
        while (k + 1 < sizeof enc && v[k] && (isalnum((unsigned char)v[k]) || strchr("-_.:", v[k]))) k++;
        snprintf(enc, sizeof enc, "%.*s", (int)k, v);
    }
    if (!enc[0] && valid_utf8(s, n)) return b;
    fz_buffer *u = NULL;
    fz_var(u);
    fz_try(ctx) {
        u = fz_new_buffer(ctx, n * 2 + 16);
        if (enc[0] && convert(u, enc, s, n) < 0) {
            fz_drop_buffer(ctx, u);
            u = NULL;
            u = fz_new_buffer(ctx, n * 2 + 16);
            enc[0] = '\0';
        }
        if (!enc[0]) chm_text(u, s, n, cp);
        char *t = (char *)fz_string_from_buffer(ctx, u), *d;
        while ((d = chm_declaration(t)) != NULL) *strchr(d, '=') = ' ';
    }
    fz_catch(ctx) {
        fz_drop_buffer(ctx, u);
        fz_drop_buffer(ctx, b);
        fz_rethrow(ctx);
    }
    fz_drop_buffer(ctx, b);
    return u;
}

static int chm_has_entry(fz_context *c, fz_archive *a, const char *name)
{
    (void)c;
    struct chmUnitInfo ui;
    return chm_find(((chm_archive *)a)->chm, name, &ui);
}

static fz_buffer *chm_read_entry(fz_context *c, fz_archive *a, const char *name)
{
    chm_archive *arch = (chm_archive *)a;
    fz_buffer *b = chm_read(arch->chm, name);
    if (!b) fz_throw(c, FZ_ERROR_FORMAT, "cannot find entry %s", name);
    return ext_is(extension(name), "htm html") ? chm_page(b, arch->cp) : b;
}

static void chm_drop_archive(fz_context *c, fz_archive *a)
{
    (void)c;
    if (((chm_archive *)a)->chm) chm_close(((chm_archive *)a)->chm);
}

/* An entry of the table of contents (name: with its entities if it is
 * HTML's; one that is empty is none). Its URL is a page as Okular's
 * pathToUrl takes it: the fragment apart, percent-decoded, from the root;
 * a web address is none. */
static void chm_add(struct chm_book *bk, int level, const char *name, int html, const char *url)
{
    while (isspace((unsigned char)*name)) name++;
    size_t len = strlen(name);
    while (len && isspace((unsigned char)name[len - 1])) len--;
    if (!len) return;
    if (bk->ntoc == bk->tcap) {
        bk->tcap = bk->tcap ? bk->tcap * 2 : 64;
        bk->toc = fz_realloc_array(ctx, bk->toc, bk->tcap, struct chm_entry);
    }
    struct chm_entry *e = &bk->toc[bk->ntoc++];
    memset(e, 0, sizeof *e);
    e->level = level;
    char *x = e->name = fz_malloc(ctx, len * 5 + 1);
    for (size_t i = 0; i < len; i++) {
        const char *v = name[i] == '<' ? "&lt;" : name[i] == '>' ? "&gt;" : name[i] == '&' && !html ? "&amp;" : NULL;
        if (v) x = stpcpy(x, v);
        else if ((unsigned char)name[i] >= 0x20 || name[i] == '\t') *x++ = name[i];
    }
    *x = '\0';
    while (url && *url == '/') url++;
    if (url && *url && !strstr(url, "://") && strncasecmp(url, "mailto:", 7) && strncasecmp(url, "javascript:", 11)) {
        const char *hash = strchr(url, '#');
        size_t n = hash ? (size_t)(hash - url) : strlen(url);
        if (n) {
            e->path = fz_malloc(ctx, n + 1);
            memcpy(e->path, url, n);
            e->path[n] = '\0';
            fz_cleanname(fz_urldecode(e->path));
        }
        if (hash && hash[1]) e->frag = fz_strdup(ctx, hash + 1);
    }
}

/* An entry of the binary table of contents, in the code page. */
static void chm_add_cp(struct chm_book *bk, int level, const char *name, const char *url)
{
    fz_buffer *n = fz_new_buffer(ctx, 256), *u = NULL;
    fz_var(u);
    fz_try(ctx) {
        chm_text(n, (const unsigned char *)name, strlen(name), bk->cp);
        u = fz_new_buffer(ctx, 256);
        if (url) chm_text(u, (const unsigned char *)url, strlen(url), bk->cp);
        chm_add(bk, level, fz_string_from_buffer(ctx, n), 0, url ? fz_string_from_buffer(ctx, u) : NULL);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, n);
        fz_drop_buffer(ctx, u);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static void chm_drop_toc(struct chm_book *bk)
{
    for (int i = 0; i < bk->ntoc; i++) {
        fz_free(ctx, bk->toc[i].name);
        fz_free(ctx, bk->toc[i].path);
        fz_free(ctx, bk->toc[i].frag);
    }
    bk->ntoc = 0;
}

/* The NUL-terminated string at off in b, or NULL. */
static const char *chm_string(fz_buffer *b, uint64_t off)
{
    unsigned char *s;
    size_t n = fz_buffer_storage(ctx, b, &s);
    return off < n && memchr(s + off, 0, n - off) ? (const char *)s + off : NULL;
}

enum { CHM_TOCIDX, CHM_TOPICS, CHM_URLTBL, CHM_URLSTR, CHM_STRINGS };

/* The binary table of contents from off on, as Okular's RecurseLoadBTOC
 * reads it (a book by its name, a topic by #TOPICS' name and URL): 0 if it
 * is broken. */
static int chm_btoc(struct chm_book *bk, fz_buffer **t, uint32_t off, int level)
{
    unsigned char *idx, *top, *tbl;
    size_t ni = fz_buffer_storage(ctx, t[CHM_TOCIDX], &idx), nt = fz_buffer_storage(ctx, t[CHM_TOPICS], &top);
    size_t nu = fz_buffer_storage(ctx, t[CHM_URLTBL], &tbl);
    for (; off; off = le32(idx + off + 16)) {
        if ((uint64_t)off + 20 > ni) return 1;
        if (--bk->steps < 0 || level > 64) return 0;
        uint32_t flags = le32(idx + off + 4), index = le32(idx + off + 8);
        if (flags & 0x0C) {
            const char *name, *url = NULL;
            if (!(flags & 8)) {
                name = chm_string(t[CHM_STRINGS], index);
            } else {
                if ((uint64_t)index * 16 + 12 > nt) return 0;
                uint32_t title = le32(top + (size_t)index * 16 + 4), u = le32(top + (size_t)index * 16 + 8);
                name = title == 0xFFFFFFFF ? "" : chm_string(t[CHM_STRINGS], title);
                if ((uint64_t)u + 12 > nu || !(url = chm_string(t[CHM_URLSTR], (uint64_t)le32(tbl + u + 8) + 8))) return 0;
            }
            if (!name) return 0;
            chm_add_cp(bk, level, name, url);
        }
        if (flags & 4) {
            if ((uint64_t)off + 24 > ni) return 0;
            uint32_t child = le32(idx + off + 20);
            if (child && !chm_btoc(bk, t, child, level + 1)) return 0;
        }
    }
    return 1;
}

/* The value of the attribute att in a tag's text (len bytes), in v: 0 if
 * it has none. */
static int chm_att(const char *tag, size_t len, const char *att, char *v, size_t size)
{
    size_t n = strlen(att);
    for (size_t i = 1; i + n < len; i++) {
        if (strncasecmp(tag + i, att, n) || tag[i + n] != '=' || isalnum((unsigned char)tag[i - 1])) continue;
        const char *s = tag + i + n + 1, *end = tag + len;
        char quote = s < end && (*s == '"' || *s == '\'') ? *s++ : 0;
        size_t k = 0;
        for (; s < end && (quote ? *s != quote : !isspace((unsigned char)*s)) && k + 1 < size; s++) v[k++] = *s;
        v[k] = '\0';
        return 1;
    }
    return 0;
}

/* A .hhc sitemap (in UTF-8), as Okular's parseFileAndFillArray reads it:
 * each <object type="text/sitemap"> an entry of its "Name" (or "Keyword")
 * and "Local" params, at the depth of the <ul> lists around it. */
static void chm_hhc(struct chm_book *bk, const char *s)
{
    char name[1024] = "", url[1024] = "", v[1024], param[32];
    int depth = 0, root = -1, in = 0;
    for (const char *p = s; (p = strchr(p, '<')) != NULL;) {
        /* The tag ends at a '>' out of quotes (or, a quote not closed, the next one). */
        const char *tag = ++p, *end = p;
        while (*end && *end != '>') {
            if (*end == '"' || *end == '\'') {
                const char *q = strchr(end + 1, *end);
                if (!q) q = strchr(end + 1, '>');
                if (!q) q = end + strlen(end);
                end = q;
                if (*end != '"' && *end != '\'') break;
            }
            end++;
        }
        size_t len = (size_t)(end - tag), w = 0;
        while (w < len && (isalnum((unsigned char)tag[w]) || tag[w] == '/')) w++;
        if (w == 6 && !strncasecmp(tag, "object", 6)) {
            in = find_case(tag, len, "text/sitemap") != NULL;
            name[0] = url[0] = '\0';
        } else if (w == 7 && !strncasecmp(tag, "/object", 7) && in) {
            if (name[0] || url[0]) {
                if (root < 0) root = depth;
                chm_add(bk, depth - root, name[0] ? name : url, 1, url);
            }
            in = 0;
        } else if (w == 5 && !strncasecmp(tag, "param", 5) && in && chm_att(tag, len, "name", param, sizeof param) &&
                   chm_att(tag, len, "value", v, sizeof v)) {
            if ((!strcasecmp(param, "name") || !strcasecmp(param, "keyword")) && v[0]) memcpy(name, v, sizeof name);
            else if (!strcasecmp(param, "local") && !url[0]) memcpy(url, v, sizeof url);
        } else if (w == 2 && !strncasecmp(tag, "ul", 2)) {
            if (depth < 256) depth++;
        } else if (w == 3 && !strncasecmp(tag, "/ul", 3)) {
            if (--depth < (root < 0 ? 0 : root)) depth = root < 0 ? 0 : root;
        }
        if (!*end) break;
        p = end + 1;
    }
}

static int chm_list(struct chmFile *h, struct chmUnitInfo *ui, void *opaque)
{
    (void)h;
    struct chm_book *bk = opaque;
    int ok = 1;
    fz_try(ctx) {
        if (bk->nfiles == bk->fcap) {
            bk->fcap = bk->fcap ? bk->fcap * 2 : 256;
            bk->files = fz_realloc_array(ctx, bk->files, bk->fcap, char *);
        }
        bk->files[bk->nfiles] = fz_strdup(ctx, ui->path);
        bk->nfiles++;
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = 0;
    }
    return ok ? CHM_ENUMERATOR_CONTINUE : CHM_ENUMERATOR_FAILURE;
}

/* Whether path is a page not seen before (CHM's names are case-insensitive). */
static int chm_new_page(fz_tree **seen, const char *path)
{
    char key[CHM_MAX_PATHLEN + 1];
    size_t i = 0;
    for (; path[i] && i + 1 < sizeof key; i++) key[i] = (char)tolower((unsigned char)path[i]);
    key[i] = '\0';
    if (!key[0] || fz_tree_lookup(ctx, *seen, key)) return 0;
    *seen = fz_tree_insert(ctx, *seen, key, (void *)key); /* any value but NULL */
    return 1;
}

/* A path as an href: percent-encoded, but for its slashes and what URLs keep. */
static void put_href(fz_buffer *b, const char *path)
{
    for (const unsigned char *s = (const unsigned char *)path; *s; s++) {
        if (*s < 0x80 && (isalnum(*s) || strchr("-._~/", *s))) fz_append_byte(ctx, b, *s);
        else fz_append_printf(ctx, b, "%%%02X", *s);
    }
}

static void open_chm(const char *path)
{
    static const char container[] = "<?xml version=\"1.0\"?><container version=\"1.0\" "
        "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"" CHM_OPF "\" "
        "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    static const char *const tables[] = { "#TOCIDX", "#TOPICS", "#URLTBL", "#URLSTR", "#STRINGS" };
    struct chm_book bk;
    memset(&bk, 0, sizeof bk);
    fz_archive *chm = NULL, *made = NULL, *multi = NULL;
    fz_buffer *b = NULL, *text = NULL, *opf = NULL, *ncx = NULL, *t[5] = { NULL };
    fz_tree *seen = NULL;
    const char **pages = NULL;
    int npages = 0;
    fz_var(bk);
    fz_var(chm);
    fz_var(made);
    fz_var(multi);
    fz_var(b);
    fz_var(text);
    fz_var(opf);
    fz_var(ncx);
    fz_var(t);
    fz_var(seen);
    fz_var(pages);
    fz_var(npages);
    fz_try(ctx) {
        struct chmFile *h = chm_open(path);
        if (!h) fz_throw(ctx, FZ_ERROR_FORMAT, "not a CHM file");
        fz_try(ctx) chm = &fz_new_derived_archive(ctx, NULL, chm_archive)->super;
        fz_catch(ctx) {
            chm_close(h);
            fz_rethrow(ctx);
        }
        chm->format = "chm";
        chm->has_entry = chm_has_entry;
        chm->read_entry = chm_read_entry;
        chm->drop_archive = chm_drop_archive;
        ((chm_archive *)chm)->chm = h;

        /* #SYSTEM: the table of contents' file, the home page, the title,
         * the language, the name of the files it was made of. */
        char topics[CHM_MAX_PATHLEN + 1] = "", home[CHM_MAX_PATHLEN + 1] = "", title[512] = "", hhc[CHM_MAX_PATHLEN + 1] = "";
        unsigned lcid = 0;
        unsigned char *s;
        size_t n = (b = chm_read(h, "#SYSTEM")) ? fz_buffer_storage(ctx, b, &s) : 0;
        for (size_t i = 4; i + 4 <= n;) {
            unsigned code = s[i] | s[i + 1] << 8, len = s[i + 2] | s[i + 3] << 8;
            const char *v = (const char *)s + i + 4;
            if (len > n - i - 4) break;
            int vlen = (int)strnlen(v, len);
            if (code == 0 && !topics[0]) snprintf(topics, sizeof topics, "%.*s", vlen, v);
            else if (code == 2) snprintf(home, sizeof home, "%.*s", vlen, v);
            else if (code == 3) snprintf(title, sizeof title, "%.*s", vlen, v);
            else if (code == 4 && len >= 2) lcid = s[i + 4] | s[i + 5] << 8;
            else if (code == 6) snprintf(hhc, sizeof hhc, "%.*s.hhc", vlen, v);
            i += 4 + len;
        }
        bk.cp = ((chm_archive *)chm)->cp = chm_codepage(lcid);
        if (!chm_enumerate(h, CHM_ENUMERATE_NORMAL | CHM_ENUMERATE_FILES, chm_list, &bk)) {
            fz_throw(ctx, FZ_ERROR_FORMAT, "cannot list the files of this CHM file");
        }
        /* As Okular looks for the sitemap; then the first there is. */
        struct chmUnitInfo ui;
        if (!topics[0] && hhc[0] && chm_find(h, hhc, &ui)) memcpy(topics, hhc, sizeof topics);
        if (!topics[0] && chm_find(h, "toc.hhc", &ui)) snprintf(topics, sizeof topics, "toc.hhc");
        for (int i = 0; !topics[0] && i < bk.nfiles; i++) {
            if (ext_is(extension(bk.files[i]), "hhc")) snprintf(topics, sizeof topics, "%s", bk.files[i]);
        }

        int all = 1;
        for (int i = 0; i < 5; i++) all &= (t[i] = chm_read(h, tables[i])) != NULL;
        if (all && (n = fz_buffer_storage(ctx, t[CHM_TOCIDX], &s)) >= 4) {
            bk.steps = (int)(n / 20 + 1);
            if (!chm_btoc(&bk, t, le32(s), 0)) chm_drop_toc(&bk);
        }
        fz_drop_buffer(ctx, b);
        b = NULL;
        if (!bk.ntoc && topics[0] && (b = chm_read(h, topics))) {
            n = fz_buffer_storage(ctx, b, &s);
            text = fz_new_buffer(ctx, n + 16);
            chm_text(text, s, n, bk.cp);
            chm_hhc(&bk, fz_string_from_buffer(ctx, text));
        }

        /* The pages: those of the table of contents, once each; the home
         * page; the other HTML files. */
        pages = fz_malloc_array(ctx, bk.ntoc + bk.nfiles + 1, const char *);
        for (int i = 0; i < bk.ntoc; i++) {
            const char *p = bk.toc[i].path;
            if (p && chm_find(h, p, &ui) && chm_new_page(&seen, p)) pages[npages++] = p;
        }
        char *hp = home;
        while (*hp == '/') hp++;
        hp[strcspn(hp, "#")] = '\0';
        if (ext_is(extension(hp), "htm html") && chm_find(h, hp, &ui) && chm_new_page(&seen, hp)) pages[npages++] = hp;
        for (int i = 0; i < bk.nfiles; i++) {
            const char *p = bk.files[i] + 1;
            if (ext_is(extension(p), "htm html") && chm_new_page(&seen, p)) pages[npages++] = p;
        }
        if (!npages) fz_throw(ctx, FZ_ERROR_FORMAT, "no pages in this CHM file");

        opf = fz_new_buffer(ctx, 1024 + (size_t)npages * 128);
        fz_append_string(ctx, opf, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><package xmlns=\"http://www.idpf.org/2007/opf\" "
                         "version=\"2.0\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">");
        if (title[0]) {
            fz_buffer *tb = fz_new_buffer(ctx, sizeof title * 2);
            fz_try(ctx) {
                chm_text(tb, (const unsigned char *)title, strlen(title), bk.cp);
                fz_append_string(ctx, opf, "<dc:title>");
                put_escaped(opf, fz_string_from_buffer(ctx, tb));
                fz_append_string(ctx, opf, "</dc:title>");
            }
            fz_always(ctx) fz_drop_buffer(ctx, tb);
            fz_catch(ctx) fz_rethrow(ctx);
        }
        fz_append_string(ctx, opf, "</metadata><manifest><item id=\"ncx\" href=\"" CHM_NCX "\" media-type=\"application/x-dtbncx+xml\"/>");
        for (int i = 0; i < npages; i++) {
            fz_append_printf(ctx, opf, "<item id=\"p%d\" href=\"", i);
            put_href(opf, pages[i]);
            fz_append_string(ctx, opf, "\" media-type=\"application/xhtml+xml\"/>");
        }
        fz_append_string(ctx, opf, "</manifest><spine toc=\"ncx\">");
        for (int i = 0; i < npages; i++) fz_append_printf(ctx, opf, "<itemref idref=\"p%d\"/>", i);
        fz_append_string(ctx, opf, "</spine></package>");

        /* The outline: a navPoint an entry, inside the one before it when
         * it is deeper; one without a page has an empty target. */
        ncx = fz_new_buffer(ctx, 1024 + (size_t)bk.ntoc * 128);
        fz_append_string(ctx, ncx, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" "
                         "version=\"2005-1\"><navMap>");
        int open = 0;
        for (int i = 0; i < bk.ntoc; i++) {
            int level = bk.toc[i].level < open ? bk.toc[i].level : open;
            for (; open > level; open--) fz_append_string(ctx, ncx, "</navPoint>");
            fz_append_string(ctx, ncx, "<navPoint><navLabel><text>");
            fz_append_string(ctx, ncx, bk.toc[i].name);
            fz_append_string(ctx, ncx, "</text></navLabel><content src=\"");
            if (bk.toc[i].path) put_href(ncx, bk.toc[i].path);
            if (bk.toc[i].path && bk.toc[i].frag) {
                fz_append_byte(ctx, ncx, '#');
                put_href(ncx, bk.toc[i].frag);
            }
            fz_append_string(ctx, ncx, "\"/>");
            open++;
        }
        for (; open > 0; open--) fz_append_string(ctx, ncx, "</navPoint>");
        fz_append_string(ctx, ncx, "</navMap></ncx>");

        made = fz_new_tree_archive(ctx, NULL);
        fz_tree_archive_add_data(ctx, made, "META-INF/container.xml", container, sizeof container - 1);
        fz_tree_archive_add_buffer(ctx, made, CHM_OPF, opf);
        fz_tree_archive_add_buffer(ctx, made, CHM_NCX, ncx);
        multi = fz_new_multi_archive(ctx);
        fz_mount_multi_archive(ctx, multi, chm, NULL);
        fz_mount_multi_archive(ctx, multi, made, NULL); /* the last mounted is looked in first */
        doc = epub_document_handler.open(ctx, &epub_document_handler, NULL, NULL, multi, NULL);
    }
    fz_always(ctx) {
        fz_drop_archive(ctx, multi);
        fz_drop_archive(ctx, made);
        fz_drop_archive(ctx, chm);
        fz_drop_buffer(ctx, b);
        fz_drop_buffer(ctx, text);
        fz_drop_buffer(ctx, opf);
        fz_drop_buffer(ctx, ncx);
        for (int i = 0; i < 5; i++) fz_drop_buffer(ctx, t[i]);
        fz_drop_tree(ctx, seen, NULL);
        fz_free(ctx, pages);
        chm_drop_toc(&bk);
        fz_free(ctx, bk.toc);
        for (int i = 0; i < bk.nfiles; i++) fz_free(ctx, bk.files[i]);
        fz_free(ctx, bk.files);
    }
    fz_catch(ctx) fz_rethrow(ctx);
    set_format("CHM", 1);
}

/* Open path (name: what it is called, for its extension; m: its first n
 * bytes) by what it is, as Okular picks a generator by the file's type. */
static void open_by_type(const char *path, const char *name, int dir, const unsigned char *m, size_t n, struct view_reply *r)
{
    const char *e = extension(name);
    kind = MUPDF;
    if (dir) {
        doc = open_comic(path, COMIC_DIR, open_mode);
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
        doc = open_comic(path, COMIC_LIBARCHIVE, open_mode);
        set_format("Comic book", 0);
    } else if (zip_type_is(m, n, "image/openraster") || zip_type_is(m, n, "application/x-krita") || ext_is(e, "ora kra krz")) {
        open_merged(path);
    } else if (zip_type_is(m, n, ODT_TYPE) || ext_is(e, "odt")) {
        open_odt(path, 0);
    } else if (ext_is(e, "fodt") || (n >= 5 && !memcmp(m, "<?xml", 5) && memmem(m, n, "office:mimetype=\"" ODT_TYPE "\"", sizeof "office:mimetype=\"" ODT_TYPE "\"" - 1))) {
        open_odt(path, 1);
    } else if ((n >= 4 && !memcmp(m, "ITSF", 4)) || ext_is(e, "chm")) {
        open_chm(path);
    } else if (is_raw(m, n, name) || is_kimgio(m, n) || magick_coder_content(m, n)) {
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
            doc = open_comic(path, COMIC_MUPDF, open_mode);
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
            doc = open_comic(path, COMIC_LIBARCHIVE, open_mode);
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

static void op_open(const struct view_request *q, struct view_reply *r, struct out *o)
{
    char name[PATH_MAX], fdpath[64];
    unsigned char m[4096];
    struct stat st;
    const char *path = q->text;
    close_document();
    open_w = q->ax;
    open_h = q->ay;
    open_em = q->bx;
    open_mode = q->mode;
    snprintf(name, sizeof name, "%s", path);
    snprintf(open_path, sizeof open_path, "%s", path);
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
    /* A comic book's pages' names, in their order. */
    if (!r->status && comic) {
        o->buf = fz_new_buffer(ctx, 4096);
        for (int i = 0; i < comic->count; i++) put(o->buf, comic->names[i], strlen(comic->names[i]) + 1);
    }
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

/* The image the document is: its EXIF, and (page 0 of one MuPDF reads) its
 * pixels' size from its header. */
static void image_info(int page_, struct exif *x, int *w, int *h)
{
    fz_buffer *b = fz_read_file(ctx, open_path);
    fz_image *image = NULL;
    fz_var(image);
    fz_try(ctx) {
        unsigned char *s;
        size_t n = fz_buffer_storage(ctx, b, &s);
        exif_read(s, n, x);
        if (page_ == 0 && n >= 8 && fz_recognize_image_format(ctx, s) != FZ_IMAGE_UNKNOWN) {
            image = fz_new_image_from_buffer(ctx, b);
            uint8_t turn = fz_image_orientation(ctx, image);
            *w = turn > 4 ? image->h : image->w;
            *h = turn > 4 ? image->w : image->h;
        }
    }
    fz_always(ctx) {
        fz_drop_image(ctx, image);
        fz_drop_buffer(ctx, b);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

/* VIEW_META: an image page's information, as Gwenview's Information panel
 * has it: a comic book's page (its file), or the image the document is. */
static void op_meta(const struct view_request *q, struct out *o)
{
    struct page_info info;
    const char *name = comic ? comic->names[q->page] : open_path;
    char v[256];
    int w = 0, h = 0;
    if (comic) {
        comic_info(comic, q->page, &info);
    } else {
        memset(&info, 0, sizeof info);
        info.size = -1;
        info.x.flash = -1;
        struct stat st;
        if (data_fd < 0 && stat(open_path, &st) == 0) {
            info.size = st.st_size;
            info.mtime = st.st_mtime;
        }
        if (ext_is(extension(open_path), RAW_SUFFIXES)) {
            raw_info(open_path, NULL, 0, &info.x);
        } else {
            image_info(q->page, &info.x, &w, &h);
        }
    }
    o->buf = fz_new_buffer(ctx, 512);
    put_info(o->buf, "Name", base_name(name));
    if (info.size >= 0) fz_append_printf(ctx, o->buf, "File Size\t%lld\n", (long long)info.size);
    exif_time(v, sizeof v, info.mtime);
    put_info(o->buf, "Date Modified", exif_shown(v));
    /* The picture's pixels, upright: else a comic book page's image, else
     * the page at the 96 dots an inch MuPDF gives an image without them (the
     * ones view-render converted). */
    if (!w) {
        load_page(q->page);
        w = (int)lroundf((page_box.x1 - page_box.x0) * 96 / 72);
        h = (int)lroundf((page_box.y1 - page_box.y0) * 96 / 72);
    }
    if (comic && ((comic_page *)page)->image) {
        fz_image *image = ((comic_page *)page)->image;
        uint8_t turn = fz_image_orientation(ctx, image);
        w = turn > 4 ? image->h : image->w;
        h = turn > 4 ? image->w : image->h;
    }
    if (w > 0 && h > 0) fz_append_printf(ctx, o->buf, "Image Size\t%dx%d\n", w, h);
    put_info(o->buf, "Exif.Photo.DateTimeOriginal", exif_shown(info.x.date));
    put_info(o->buf, "Exif.Image.DateTime", exif_shown(info.x.date_file));
    put_info(o->buf, "Exif.Image.Make", info.x.make);
    put_info(o->buf, "Exif.Image.Model", info.x.model);
    if (info.x.exposure > 0) {
        if (info.x.exposure < 1) snprintf(v, sizeof v, "1/%.0f s", 1 / info.x.exposure);
        else snprintf(v, sizeof v, "%.1f s", info.x.exposure);
        put_info(o->buf, "Exif.Photo.ExposureTime", v);
    }
    if (info.x.fnumber > 0) {
        snprintf(v, sizeof v, "F%.1f", info.x.fnumber);
        put_info(o->buf, "Exif.Photo.FNumber", v);
    }
    if (info.x.iso > 0) fz_append_printf(ctx, o->buf, "Exif.Photo.ISOSpeedRatings\t%d\n", info.x.iso);
    if (info.x.focal > 0) fz_append_printf(ctx, o->buf, "Exif.Photo.FocalLength\t%.1f mm\n", info.x.focal);
    if (info.x.flash >= 0) fz_append_printf(ctx, o->buf, "Exif.Photo.Flash\t%d\n", info.x.flash);
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
        op_open(q, r, o);
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
    case VIEW_META:
        if (!has_page) fail(r, EINVAL, "No page %d", q->page);
        else if (kind != MUPDF || (!comic && strcmp(format, "Image") && strcmp(format, "TIFF"))) fail(r, ENOTSUP, "%s is not an image", format);
        else op_meta(q, o);
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
