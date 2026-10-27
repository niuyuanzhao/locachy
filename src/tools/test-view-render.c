/*
 * test-view-render SAMPLES RENDERER: runs the real view-render through
 * view_spawn (built with -DVIEW_RENDER_TEST_PATH) as uid 1000 on the
 * samples of make-view-samples.sh and the images made here, and checks
 * every request on them, its sandbox, ImageMagick's policy in it
 * (view-image.c, linked here), and that it refuses to run outside one. Run
 * as root in the development root. A PASS or FAIL line for each check, a
 * GAP line for each DDS variant not read; exits 1 on a FAIL.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <MagickWand/MagickWand.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../progs/view-image.h"
#include "../progs/view-proto.h"

#define USER_CSS "/home/tester/.config/locachy/view.css"

static const char *samples, *renderer;
static int sock = -1, failures;
static pid_t pid;

__attribute__((format(printf, 2, 3)))
static int check(int ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(ok ? "PASS " : "FAIL ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    fflush(stdout);
    if (!ok) failures++;
    return ok;
}

static const char *sample(const char *name)
{
    static char path[4][4096];
    static int i;
    i = (i + 1) % 4;
    snprintf(path[i], sizeof path[i], "%s/%s", samples, name);
    return path[i];
}

static void start(void)
{
    if (sock >= 0) close(sock);
    if (pid > 0) waitpid(pid, NULL, 0);
    pid = view_spawn(&sock);
    if (pid < 0) {
        check(0, "view_spawn");
        exit(1);
    }
}

/* One request: its reply, the memfd's bytes in *data (free them). */
static struct view_reply call(struct view_request *q, void **data)
{
    struct view_reply r;
    int fd;
    if (data) *data = NULL;
    if (view_call(sock, q, &r, &fd) < 0) {
        check(0, "view_call of op %u: view-render has gone", q->op);
        start();
        memset(&r, 0, sizeof r);
        r.status = -1;
        return r;
    }
    if (fd >= 0) {
        void *p = view_map(fd, r.size);
        if (data && p) {
            *data = malloc(r.size + 1);
            memcpy(*data, p, r.size);
            ((char *)*data)[r.size] = '\0';
        }
        if (p) munmap(p, r.size);
        else check(0, "view_map of a reply of %llu bytes", (unsigned long long)r.size);
        close(fd);
    }
    return r;
}

static struct view_request req(int op, int page)
{
    struct view_request q;
    memset(&q, 0, sizeof q);
    q.op = (uint32_t)op;
    q.page = page;
    return q;
}

static struct view_reply open_doc(const char *path, float w, float h, float em)
{
    struct view_request q = req(VIEW_OPEN, 0);
    snprintf(q.text, sizeof q.text, "%s", path);
    q.ax = w, q.ay = h, q.bx = em;
    return call(&q, NULL);
}

static struct view_reply page_info(int page)
{
    struct view_request q = req(VIEW_PAGE, page);
    return call(&q, NULL);
}

/* RENDER of page at zoom and rotate (the whole page): its pixels. */
static uint32_t *render(int page, float zoom, int rotate, struct view_reply *r)
{
    struct view_request q = req(VIEW_RENDER, page);
    void *px;
    q.zoom = zoom;
    q.rotate = rotate;
    *r = call(&q, &px);
    return px;
}

static int not_blank(const uint32_t *px, int w, int h)
{
    for (long i = 0; px && i < (long)w * h; i++) if (px[i] != 0xFFFFFF) return 1;
    return 0;
}

static int top_byte_clear(const uint32_t *px, int w, int h)
{
    for (long i = 0; px && i < (long)w * h; i++) if (px[i] >> 24) return 0;
    return px != NULL;
}

static int near(float a, float b, float tolerance)
{
    return fabsf(a - b) <= tolerance;
}

/* SEARCH: the hits' rects (count in *n; free them). */
static struct view_rect *search(int page, const char *needle, int *n)
{
    struct view_request q = req(VIEW_SEARCH, page);
    void *data;
    snprintf(q.text, sizeof q.text, "%s", needle);
    struct view_reply r = call(&q, &data);
    *n = r.status ? -r.status : r.count;
    return data;
}

/* SELECT from a to b in mode: the text (free it), rects in *n. */
static char *select_text(int page, float ax, float ay, float bx, float by, int mode, int *n)
{
    struct view_request q = req(VIEW_SELECT, page);
    void *data;
    q.ax = ax, q.ay = ay, q.bx = bx, q.by = by, q.mode = mode;
    struct view_reply r = call(&q, &data);
    *n = r.count;
    if (r.status || !data) {
        free(data);
        return strdup("");
    }
    char *text = strndup((char *)data + (size_t)r.count * sizeof(struct view_rect), (size_t)r.w);
    free(data);
    return text;
}

static char *bulk(int op)
{
    struct view_request q = req(op, 0);
    void *data;
    struct view_reply r = call(&q, &data);
    if (r.status) {
        free(data);
        return NULL;
    }
    return data ? data : strdup("");
}

static uint32_t pixel(const uint32_t *px, int w, int x, int y)
{
    return px ? px[(size_t)y * w + x] : 0xFFFFFFFF;
}

static int is_color(uint32_t p, int r, int g, int b)
{
    int pr = p >> 16 & 0xFF, pg = p >> 8 & 0xFF, pb = p & 0xFF;
    return p >> 24 == 0 && abs(pr - r) < 70 && abs(pg - g) < 70 && abs(pb - b) < 70;
}

struct expect {
    const char *file, *format;
    int pages;      /* 0: at least one (reflowed) */
    float pw, ph;   /* page 0, points */
    const char *word; /* SEARCH finds it on page 0; NULL: no text */
    unsigned flags; /* in OPEN's */
};

#define TEXT VIEW_HAS_TEXT
#define FLOW (VIEW_REFLOWABLE | VIEW_HAS_TEXT)
static const struct expect formats[] = {
    { "sample.pdf", "PDF", 2, 612, 792, "quokka", TEXT | VIEW_HAS_OUTLINE | VIEW_HAS_EMBEDDED },
    { "sample.pdf.gz", "PDF", 2, 612, 792, "quokka", TEXT },
    { "sample.xps", "XPS", 1, 612, 792, "penguin", TEXT },
    { "sample.oxps", "XPS", 1, 612, 792, "penguin", TEXT },
    { "sample.epub", "EPUB", 0, 400, 600, "zebra", FLOW | VIEW_HAS_OUTLINE },
    { "sample.mobi", "Mobipocket", 0, 400, 600, "otter", FLOW },
    { "sample.fb2", "FictionBook", 0, 400, 600, "badger", FLOW },
    { "sample.fb2.zip", "FictionBook", 0, 400, 600, "badger", FLOW },
    { "sample.md", "Markdown", 0, 400, 600, "walrus", FLOW },
    { "sample.html", "HTML", 0, 400, 600, "heron", FLOW },
    { "sample-utf8.txt", "Plain text", 0, 400, 600, "narwhal", FLOW },
    { "README", "Plain text", 0, 400, 600, "narwhal", FLOW },
    { "sample-utf16le.txt", "Plain text", 0, 400, 600, "narwhal", FLOW },
    { "sample-utf16be.txt", "Plain text", 0, 400, 600, "narwhal", FLOW },
    { "sample-gb18030.txt", "Plain text", 0, 400, 600, "narwhal", FLOW },
    { "sample-latin1.txt", "Plain text", 0, 400, 600, "caf\xC3\xA9", FLOW },
    { "sample.conf", "Plain text", 0, 400, 600, "yak", FLOW },
    { "sample.cbz", "Comic book", 3, 100, 150, NULL, 0 },
    { "sample.cbt", "Comic book", 3, 100, 150, NULL, 0 },
    { "sample.cb7", "Comic book", 3, 100, 150, NULL, 0 },
    { "comic-dir", "Comic book", 5, 100, 150, NULL, 0 },
    { "sample.png", "Image", 1, 200, 100, NULL, 0 },
    { "sample.jpg", "Image", 1, 200, 100, NULL, 0 },
    { "sample.gif", "Image", 1, 150, 75, NULL, 0 },
    { "sample.bmp", "Image", 1, 200, 100, NULL, 0 },
    { "sample.ppm", "Image", 1, 150, 75, NULL, 0 },
    { "sample.psd", "Image", 1, 150, 75, NULL, 0 },
    { "sample.jp2", "Image", 1, 200, 100, NULL, 0 },
    { "sample.webp", "Image", 1, 150, 75, NULL, 0 }, /* no resolution: MuPDF's 96 dpi */
    { "sample.avif", "Image", 1, 150, 75, NULL, 0 },
    { "sample.heic", "Image", 1, 150, 75, NULL, 0 },
    { "sample.jxl", "Image", 1, 150, 75, NULL, 0 },
    { "sample.ico", "Image", 1, 48, 48, NULL, 0 }, /* ImageMagick's, 96 dpi */
    { "sample-png.ico", "Image", 1, 150, 75, NULL, 0 },
    { "sample.cur", "Image", 1, 48, 48, NULL, 0 },
    { "sample-layers.xcf", "Image", 1, 48, 36, NULL, 0 },
    { "dds-dxt1.dds", "Image", 1, 48, 36, NULL, 0 },
    { "sample.exr", "Image", 1, 48, 36, NULL, 0 },
    { "sample-tiled.exr", "Image", 1, 48, 36, NULL, 0 },
    { "sample.hdr", "Image", 1, 48, 36, NULL, 0 },
    { "sample.mng", "Image", 1, 150, 75, NULL, 0 },
    { "sample.pcx", "Image", 1, 150, 75, NULL, 0 },
    { "sample.rgb", "Image", 1, 150, 75, NULL, 0 },
    { "sample.tga", "Image", 1, 150, 75, NULL, 0 },
    { "sample.xpm", "Image", 1, 150, 75, NULL, 0 },
    { "sample.xbm", "Image", 1, 150, 75, NULL, 0 },
    { "sample.tiff", "TIFF", 2, 200, 100, NULL, 0 },
    { "sample.svg", "SVG", 1, 0, 0, NULL, TEXT },
    { "sample.djvu", "DjVu", 2, 612, 792, "lynx", TEXT | VIEW_HAS_OUTLINE },
    { "sample.ps", "PostScript", 2, 612, 792, NULL, 0 },
    { "sample.ps.bz2", "PostScript", 2, 612, 792, NULL, 0 },
    { "sample.eps", "PostScript", 1, 200, 100, NULL, 0 },
    { "sample.g3", "Fax", 1, 1728 * 72 / 204.0f, 100 * 72 / 196.0f, NULL, 0 },
    { "sample.fax", "Fax", 1, 1728 * 72 / 204.0f, 100 * 72 / 196.0f, NULL, 0 },
    { "sample-pcr.g3", "Fax", 1, 1728 * 72 / 204.0f, 100 * 72 / 98.0f, NULL, 0 },
};

/* What every format must do: OPEN, PAGE, RENDER (and turned), SEARCH and
 * SELECT where it has text. */
static void test_format(const struct expect *e)
{
    const char *f = e->file;
    struct view_reply r = open_doc(sample(f), 400, 600, 12);
    size_t fl = strlen(e->format);
    if (!check(r.status == 0 && !strncmp(r.text, e->format, fl) && r.text[fl] == '\t', "%s: OPEN is %s (\"%s\", status %d)", f, e->format, r.text, r.status)) return;
    check(e->pages ? r.pages == e->pages : r.pages >= 1, "%s: OPEN pages %d", f, r.pages);
    check((r.flags & e->flags) == e->flags && ((r.flags & VIEW_HAS_TEXT) != 0) == ((e->flags & VIEW_HAS_TEXT) != 0),
          "%s: OPEN flags %#x include %#x", f, r.flags, e->flags);
    struct view_reply p = page_info(0);
    float pw = p.pw, ph = p.ph;
    if (e->pw > 0) check(p.status == 0 && near(pw, e->pw, 0.6f) && near(ph, e->ph, 0.6f), "%s: PAGE 0 is %.1f x %.1f points (%.1f x %.1f)", f, pw, ph, e->pw, e->ph);
    else check(p.status == 0 && pw > 0 && ph > 0, "%s: PAGE 0 is %.1f x %.1f points", f, pw, ph);
    if (e->word) check((p.flags & VIEW_HAS_TEXT) != 0, "%s: PAGE 0 has text", f);
    struct view_reply rr;
    uint32_t *px = render(0, 1.5f, 0, &rr);
    int w = (int)(pw * 1.5f + 0.5f), h = (int)(ph * 1.5f + 0.5f);
    check(rr.status == 0 && rr.w == w && rr.h == h && not_blank(px, rr.w, rr.h) && top_byte_clear(px, rr.w, rr.h),
          "%s: RENDER at 1.5 is %d x %d (%d x %d), not blank, 0x00RRGGBB", f, rr.w, rr.h, w, h);
    free(px);
    px = render(0, 1.5f, 90, &rr);
    check(rr.status == 0 && rr.w == h && rr.h == w && not_blank(px, rr.w, rr.h), "%s: RENDER turned 90 is %d x %d", f, rr.w, rr.h);
    free(px);
    int n;
    struct view_rect *hits = search(0, e->word ? e->word : "x", &n);
    if (!e->word) {
        check(n == -ENOTSUP || (e->flags & VIEW_HAS_TEXT), "%s: SEARCH says no text (%d)", f, n);
        free(hits);
        return;
    }
    if (check(n >= 1 && hits, "%s: SEARCH finds \"%s\" (%d rects)", f, e->word, n)) {
        float cx = (hits[0].x0 + hits[0].x1) / 2, cy = (hits[0].y0 + hits[0].y1) / 2;
        int nr;
        char *t = select_text(0, cx, cy, cx, cy, VIEW_SELECT_WORDS, &nr);
        check(!strcasecmp(t, e->word) && nr == 1, "%s: SELECT of words at it gives \"%s\"", f, t);
        free(t);
    }
    free(hits);
}

/* Chinese renders as glyphs, not boxes: two different characters, found by
 * SEARCH, draw differently and darkly. */
static void test_cjk(const char *f)
{
    struct view_reply r = open_doc(sample(f), 400, 600, 12);
    int n1, n2;
    struct view_rect *a = search(0, "\xE4\xB8\xAD", &n1), *b = search(0, "\xE6\x96\x87", &n2);
    if (!check(r.status == 0 && n1 >= 1 && n2 >= 1, "%s: SEARCH finds Chinese characters", f)) {
        free(a);
        free(b);
        return;
    }
    struct view_reply rr;
    const float z = 4;
    uint32_t *px = render(0, z, 0, &rr);
    int w = (int)((a[0].x1 - a[0].x0) * z), h = (int)((a[0].y1 - a[0].y0) * z), dark_a = 0, dark_b = 0, differ = 0;
    for (int y = 0; px && y < h; y++) {
        for (int x = 0; x < w; x++) {
            int xa = (int)(a[0].x0 * z) + x, ya = (int)(a[0].y0 * z) + y, xb = (int)(b[0].x0 * z) + x, yb = (int)(b[0].y0 * z) + y;
            if (xa >= rr.w || xb >= rr.w || ya >= rr.h || yb >= rr.h) continue;
            int da = (pixel(px, rr.w, xa, ya) & 0xFF) < 128, db = (pixel(px, rr.w, xb, yb) & 0xFF) < 128;
            dark_a += da, dark_b += db, differ += da != db;
        }
    }
    check(dark_a > w * h / 20 && dark_b > w * h / 20 && differ > w * h / 20,
          "%s: Chinese glyphs drawn (dark %d and %d, differing %d of %d pixels)", f, dark_a, dark_b, differ, w * h);
    free(px);
    free(a);
    free(b);
    int n;
    free(search(0, "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95", &n));
    check(n >= 1, "%s: SEARCH finds the word \xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95", f);
}

/* Markdown's links to headings (the ids autoheaderid gives them, in the
 * cmark-gfm libmupdf bundles) go to them. */
static void test_markdown(void)
{
    open_doc(sample("sample.md"), 400, 600, 12);
    struct view_request q = req(VIEW_LINKS, 0);
    struct view_link *links;
    struct view_reply r = call(&q, (void **)&links);
    int internal = 0, external = 0;
    for (int i = 0; links && i < r.count; i++) {
        internal += links[i].page == 0 && !strcmp(links[i].uri, "#section-two") && links[i].y > 50;
        external += links[i].page == -1 && !strcmp(links[i].uri, "https://example.org/");
    }
    check(internal == 1 && external == 1, "Markdown: LINKS to #section-two (on its page, below the top) and to example.org (%d links)", r.count);
    free(links);
}

static void test_pdf(void)
{
    struct view_reply r = open_doc(sample("sample.pdf"), 0, 0, 0);
    struct view_reply rr;
    uint32_t *px = render(0, 1, 0, &rr);
    check(is_color(pixel(px, rr.w, 450, 142), 255, 0, 0), "PDF: the red square is red at (450,142): %06x", pixel(px, rr.w, 450, 142));
    free(px);
    struct view_request q = req(VIEW_RENDER, 0);
    q.zoom = 1, q.x = 410, q.y = 102, q.w = 80, q.h = 80;
    r = call(&q, (void **)&px);
    check(r.status == 0 && r.w == 80 && r.h == 80 && is_color(pixel(px, 80, 0, 0), 255, 0, 0) && is_color(pixel(px, 80, 79, 79), 255, 0, 0),
          "PDF: RENDER of a part (410,102 80x80) is that part of the square");
    free(px);
    q.x = q.y = q.w = q.h = 0;
    q.zoom = 20;
    r = call(&q, NULL);
    check(r.status == EFBIG, "PDF: RENDER of more than VIEW_MAX_PIXELS is EFBIG (%d)", r.status);
    q.zoom = 1, q.rotate = 45;
    check(call(&q, NULL).status == EINVAL, "PDF: RENDER turned 45 is EINVAL");
    check(page_info(7).status == EINVAL, "PDF: PAGE 7 of 2 is EINVAL");

    int n;
    struct view_rect *hits = search(0, "QUOKKA", &n);
    check(n == 1 && hits && hits[0].y0 > 120 && hits[0].y0 < 142 && hits[0].y1 > 138 && hits[0].y1 < 150 && hits[0].x0 > 150,
          "PDF: SEARCH is case-insensitive, quokka at y %.1f-%.1f (baseline 142)", hits ? hits[0].y0 : -1, hits ? hits[0].y1 : -1);
    free(hits);
    free(search(1, "quokka", &n));
    check(n == 1, "PDF: SEARCH on page 2 finds its quokka");
    int nr;
    char *t = select_text(0, 80, 65, 80, 65, VIEW_SELECT_LINES, &nr);
    check(!strcmp(t, "Hello LOCachy viewer") && nr == 1, "PDF: SELECT of a line gives \"%s\"", t);
    free(t);
    t = select_text(0, 72, 60, 300, 140, VIEW_SELECT_CHARS, &nr);
    check(strstr(t, "Hello LOCachy viewer") && strstr(t, "Search target") && nr >= 2, "PDF: SELECT across two lines gives both (%d rects)", nr);
    free(t);

    q = req(VIEW_LINKS, 0);
    struct view_link *links;
    r = call(&q, (void **)&links);
    int internal = -1, external = -1;
    for (int i = 0; links && i < r.count; i++) {
        if (links[i].page == 1) internal = i;
        if (links[i].page == -1 && !strcmp(links[i].uri, "https://example.org/")) external = i;
    }
    check(r.status == 0 && r.count == 2 && internal >= 0 && external >= 0, "PDF: LINKS has the internal and the external link (%d)", r.count);
    if (internal >= 0) {
        struct view_link *l = &links[internal];
        check(near(l->x0, 72, 0.5f) && near(l->y0, 278, 0.5f) && near(l->x1, 250, 0.5f) && near(l->y1, 296, 0.5f) && near(l->x, 72, 0.5f) && near(l->y, 100, 0.5f),
              "PDF: the internal link is at 72,278-250,296 and goes to page 2 at 72,100 (%.1f,%.1f)", l->x, l->y);
        struct view_request rq = req(VIEW_RESOLVE, 0);
        snprintf(rq.text, sizeof rq.text, "%s", l->uri);
        struct view_reply rs = call(&rq, NULL);
        check(rs.page == 1 && near(rs.x, 72, 0.5f) && near(rs.y, 100, 0.5f), "PDF: RESOLVE of %s is page 2 at %.1f,%.1f", l->uri, rs.x, rs.y);
    }
    free(links);

    q = req(VIEW_OUTLINE, 0);
    struct view_outline *ol;
    r = call(&q, (void **)&ol);
    check(r.count == 3 && ol && ol[0].level == 0 && !strcmp(ol[0].title, "Chapter One") && ol[0].page == 0 &&
          ol[1].level == 1 && !strcmp(ol[1].title, "Section 1.1") && ol[1].page == 0 && near(ol[1].y, 130, 0.5f) &&
          ol[2].level == 0 && !strcmp(ol[2].title, "Chapter Two") && ol[2].page == 1, "PDF: OUTLINE is Chapter One > Section 1.1 (y 130), Chapter Two");
    free(ol);

    char *info = bulk(VIEW_INFO);
    check(info && strstr(info, "Format\tPDF\n") && strstr(info, "Title\tLOCachy Sample\n") && strstr(info, "Author\tView Tester\n") &&
          strstr(info, "Pages\t2\n") && strstr(info, "Page size\t612 x 792 points") && strstr(info, "Font\tHelvetica (Type 1, not embedded)\n"),
          "PDF: INFO has format, title, author, pages, page size and the font");
    if (info) printf("     INFO: %s", info);
    free(info);
    char *text = bulk(VIEW_EXPORT);
    check(text && strstr(text, "Hello LOCachy viewer") && strstr(text, "\f") && strstr(strstr(text, "\f"), "Second page marker"), "PDF: EXPORT has both pages' text, a \\f between");
    free(text);
    char *files = bulk(VIEW_EMBEDDED);
    check(files && !strcmp(files, "0\tnotes.txt\t15\n"), "PDF: EMBEDDED lists notes.txt of 15 bytes (\"%s\")", files ? files : "");
    free(files);

    mkdir("/tmp/view-test-out", 0755);
    if (chown("/tmp/view-test-out", 1000, 1000) < 0) perror("chown");
    unlink("/tmp/view-test-out/notes.txt");
    q = req(VIEW_SAVE, 0);
    snprintf(q.text, sizeof q.text, "/tmp/view-test-out/notes.txt");
    r = call(&q, NULL);
    struct stat st = { 0 };
    int saved = stat("/tmp/view-test-out/notes.txt", &st) == 0;
    char buf[64] = "";
    int fd = open("/tmp/view-test-out/notes.txt", O_RDONLY);
    if (fd >= 0) {
        ssize_t got = read(fd, buf, sizeof buf - 1);
        buf[got > 0 ? got : 0] = '\0';
        close(fd);
    }
    check(r.status == 0 && saved && st.st_uid == 1000 && !strcmp(buf, "attached notes\n"),
          "PDF: SAVE writes notes.txt as the user (uid %d)", saved ? (int)st.st_uid : -1);
    snprintf(q.text, sizeof q.text, "/root/notes.txt");
    r = call(&q, NULL);
    check(r.status == EACCES, "PDF: SAVE into /root is EACCES (%d: %s)", r.status, r.text);
}

static void test_locked(void)
{
    struct view_reply r = open_doc(sample("locked.pdf"), 0, 0, 0);
    check(r.status == EACCES && (r.flags & VIEW_NEEDS_PASSWORD), "locked.pdf: OPEN asks for a password");
    check(page_info(0).status == EACCES, "locked.pdf: PAGE before AUTH is EACCES");
    struct view_request q = req(VIEW_AUTH, 0);
    snprintf(q.text, sizeof q.text, "wrong");
    r = call(&q, NULL);
    check(r.status == EACCES && (r.flags & VIEW_NEEDS_PASSWORD), "locked.pdf: AUTH with a wrong password is EACCES");
    snprintf(q.text, sizeof q.text, "secret");
    r = call(&q, NULL);
    check(r.status == 0 && r.pages == 2 && !strncmp(r.text, "PDF\tLOCachy Sample", 18), "locked.pdf: AUTH with \"secret\" opens it (%s)", r.text);
    int n;
    free(search(0, "quokka", &n));
    check(n == 1, "locked.pdf: SEARCH after AUTH");
    char *info = bulk(VIEW_INFO);
    check(info && strstr(info, "Encryption\t") && strstr(info, "AES"), "locked.pdf: INFO says the encryption");
    free(info);
}

/* The page of the reflowed EPUB that has "kiwi". */
static int kiwi_page(int pages)
{
    for (int i = 0; i < pages; i++) {
        int n;
        free(search(i, "kiwi", &n));
        if (n > 0) return i;
    }
    return -1;
}

static void test_epub(void)
{
    struct view_reply r = open_doc(sample("sample.epub"), 400, 600, 12);
    int pages = r.pages, k = kiwi_page(pages);
    check(r.status == 0 && pages >= 3 && k > 0, "EPUB: %d pages at em 12, kiwi on page %d", pages, k + 1);
    struct view_request q = req(VIEW_LINKS, 0);
    struct view_link *links;
    r = call(&q, (void **)&links);
    check(r.count >= 1 && links && links[0].page == k, "EPUB: LINKS on page 1 goes to chapter two (page %d)", r.count && links ? links[0].page + 1 : 0);
    free(links);
    q = req(VIEW_OUTLINE, 0);
    struct view_outline *ol;
    r = call(&q, (void **)&ol);
    check(r.count == 2 && ol && !strcmp(ol[0].title, "Chapter One") && ol[0].page == 0 && !strcmp(ol[1].title, "Chapter Two") && ol[1].page == k,
          "EPUB: OUTLINE has both chapters, the second on page %d", r.count == 2 && ol ? ol[1].page + 1 : 0);
    free(ol);

    q = req(VIEW_LAYOUT, k);
    q.ax = 400, q.ay = 600, q.bx = 24;
    r = call(&q, NULL);
    int k2 = kiwi_page(r.pages);
    check(r.status == 0 && r.pages > pages && r.page == k2, "EPUB: LAYOUT at em 24 has %d pages (> %d) and keeps the place: page %d, kiwi on %d",
          r.pages, pages, r.page + 1, k2 + 1);
    q = req(VIEW_LAYOUT, k2);
    q.ax = 400, q.ay = 600, q.bx = 12;
    r = call(&q, NULL);
    check(r.status == 0 && r.pages == pages && r.page == k, "EPUB: LAYOUT back at em 12: %d pages, page %d", r.pages, r.page + 1);

    /* The user's CSS restyles it: larger text, more pages. */
    mkdir("/home/tester/.config", 0755);
    mkdir("/home/tester/.config/locachy", 0755);
    if (chown("/home/tester/.config", 1000, 1000) < 0 || chown("/home/tester/.config/locachy", 1000, 1000) < 0) perror("chown");
    FILE *f = fopen(USER_CSS, "w");
    if (f) {
        fputs("p { font-size: 300% }\n", f);
        fclose(f);
    }
    if (chown(USER_CSS, 1000, 1000) < 0) perror("chown");
    r = open_doc(sample("sample.epub"), 400, 600, 12);
    check(r.status == 0 && r.pages > pages, "EPUB: %s makes it %d pages (> %d)", USER_CSS, r.pages, pages);
    unlink(USER_CSS);
    r = open_doc(sample("sample.epub"), 400, 600, 12);
    check(r.pages == pages, "EPUB: without it %d pages again", r.pages);
    q = req(VIEW_LAYOUT, 0);
    r = call(&q, NULL);
    check(r.status == EINVAL, "EPUB: LAYOUT without a size is EINVAL");
    open_doc(sample("sample.pdf"), 0, 0, 0);
    q = req(VIEW_LAYOUT, 0);
    q.ax = 400, q.ay = 600, q.bx = 12;
    check(call(&q, NULL).status == EINVAL, "PDF: LAYOUT is EINVAL (not reflowable)");
}

static void test_djvu(void)
{
    open_doc(sample("sample.djvu"), 0, 0, 0);
    int n;
    struct view_rect *hits = search(0, "LYNX", &n);
    /* (520,1400)-(700,1450) pixels at 150 dpi from the bottom of 1650. */
    check(n == 1 && hits && near(hits[0].x0, 249.6f, 0.6f) && near(hits[0].y0, 96, 0.6f) && near(hits[0].x1, 336, 0.6f) && near(hits[0].y1, 120, 0.6f),
          "DjVu: SEARCH finds lynx at 249.6,96-336,120 (%.1f,%.1f-%.1f,%.1f)", hits ? hits[0].x0 : 0, hits ? hits[0].y0 : 0, hits ? hits[0].x1 : 0, hits ? hits[0].y1 : 0);
    free(hits);
    hits = search(0, "marker lynx", &n);
    check(n == 1 && hits && hits[0].hit == 0, "DjVu: SEARCH over two words is one rect of one hit");
    free(hits);
    int nr;
    char *t = select_text(0, 100, 105, 100, 105, VIEW_SELECT_LINES, &nr);
    check(!strcmp(t, "DjVu marker lynx") && nr == 1, "DjVu: SELECT of a line gives \"%s\"", t);
    free(t);
    t = select_text(0, 80, 100, 100, 140, VIEW_SELECT_CHARS, &nr);
    check(!strcmp(t, "DjVu marker lynx\nsecond") && nr == 2, "DjVu: SELECT across lines gives \"%s\"", t);
    free(t);
    struct view_request q = req(VIEW_LINKS, 0);
    struct view_link *links;
    struct view_reply r = call(&q, (void **)&links);
    check(r.count == 2 && links && links[0].page == 1 && near(links[0].y0, (1650 - 1100) * 72 / 150.0f, 0.6f) && links[1].page == -1 &&
          !strcmp(links[1].uri, "https://example.org/"), "DjVu: LINKS has #2 and the external link");
    free(links);
    q = req(VIEW_OUTLINE, 0);
    struct view_outline *ol;
    r = call(&q, (void **)&ol);
    check(r.count == 3 && ol && ol[0].page == 0 && ol[1].page == 1 && ol[2].level == 1 && !strcmp(ol[2].title, "Sub"), "DjVu: OUTLINE");
    free(ol);
    char *info = bulk(VIEW_INFO);
    check(info && strstr(info, "Title\tDjVu Sample\n") && strstr(info, "Author\tView Tester\n") && strstr(info, "Resolution\t150 dpi"), "DjVu: INFO has its metadata");
    free(info);
    char *text = bulk(VIEW_EXPORT);
    check(text && strstr(text, "DjVu marker lynx\nsecond line\n\flynx"), "DjVu: EXPORT");
    free(text);
    check(page_info(1).flags & VIEW_HAS_TEXT, "DjVu: PAGE 2 has text");
}

static void test_ps(void)
{
    open_doc(sample("sample.ps"), 0, 0, 0);
    struct view_reply r;
    uint32_t *px = render(0, 1, 0, &r);
    int dark = 0;
    for (int y = 50; px && y < 75; y++) for (int x = 72; x < 300; x++) dark += (pixel(px, r.w, x, y) & 0xFF) < 128;
    check(is_color(pixel(px, r.w, 450, 142), 255, 0, 0) && dark > 200, "PostScript: the red square is red (%06x), the text drawn (%d dark pixels)", pixel(px, r.w, 450, 142), dark);
    free(px);
    px = render(0, 1, 90, &r);
    /* Turned clockwise: (450,142) of 612x792 is at (792-142, 450). */
    check(r.w == 792 && is_color(pixel(px, r.w, 650, 450), 255, 0, 0), "PostScript: turned 90, the square is at (650,450)");
    free(px);
    px = render(1, 1, 0, &r);
    check(is_color(pixel(px, r.w, 120, 670), 0, 0, 255), "PostScript: page 2's blue square");
    free(px);
    struct view_request q = req(VIEW_RENDER, 0);
    q.zoom = 1, q.x = 410, q.y = 102, q.w = 80, q.h = 80;
    r = call(&q, (void **)&px);
    check(r.w == 80 && is_color(pixel(px, 80, 40, 40), 255, 0, 0), "PostScript: RENDER of a part");
    free(px);
    char *info = bulk(VIEW_INFO);
    check(info && strstr(info, "Title\tPostScript Sample\n"), "PostScript: INFO has its title");
    free(info);
    check(bulk(VIEW_EXPORT) == NULL, "PostScript: EXPORT says no text");
}

static void test_images(void)
{
    static const char *const comics[] = { "sample.cbz", "sample.cbt", "sample.cb7", "comic-dir" };
    for (int c = 0; c < 4; c++) {
        struct view_reply r = open_doc(sample(comics[c]), 0, 0, 0);
        float w[5] = { 0 };
        for (int i = 0; i < r.pages && i < 5; i++) w[i] = page_info(i).pw;
        /* The PNG and JPEG pages say 72 dpi; the WebP and TGA ones none: 96. */
        check(w[0] == 100 && w[1] == 82.5f && w[2] == 120 && (c < 3 || (w[3] == 130 && w[4] == 150)),
              "%s: pages in natural order p1, p2 (WebP), p10%s (%g %g %g %g %g)", comics[c], c == 3 ? ", sub/p11, sub/p12 (TGA)" : "",
              w[0], w[1], w[2], w[3], w[4]);
        struct view_reply rr;
        uint32_t *px = render(1, 1, 0, &rr);
        check(is_color(pixel(px, rr.w, rr.w * 3 / 4, rr.h * 3 / 4), 0, 0xC0, 0) && pixel(px, rr.w, rr.w / 20, rr.h / 20) == 0, "%s: the WebP page's colors", comics[c]);
        free(px);
    }
    static const struct { const char *file; int r, g, b; } colors[] = {
        { "sample.png", 0xC0, 0, 0 }, { "sample.jpg", 0, 0xC0, 0 }, { "sample.webp", 0, 0, 0xC0 },
        { "sample.gif", 0xC0, 0x60, 0 }, { "sample.bmp", 0x60, 0, 0xC0 }, { "sample.ppm", 0, 0xC0, 0x60 },
        { "sample.psd", 0xC0, 0, 0x60 }, { "sample.jp2", 0, 0xC0, 0x60 },
        { "sample.ico", 0xE0, 0x80, 0 }, { "sample.cur", 0xE0, 0x80, 0 },
        { "sample.mng", 0x40, 0xA0, 0xE0 }, { "sample.pcx", 0x40, 0xA0, 0xE0 }, { "sample.rgb", 0x40, 0xA0, 0xE0 },
        { "sample.tga", 0x40, 0xA0, 0xE0 }, { "sample.xpm", 0x40, 0xA0, 0xE0 }, { "sample.xbm", 0, 0, 0 },
        { "sample.avif", 0xC0, 0, 0xC0 }, { "sample.heic", 0, 0xC0, 0xC0 }, { "sample.jxl", 0xC0, 0xC0, 0 },
    };
    for (size_t i = 0; i < sizeof colors / sizeof *colors; i++) {
        open_doc(sample(colors[i].file), 0, 0, 0);
        struct view_reply rr;
        uint32_t *px = render(0, 1, 0, &rr);
        uint32_t c = pixel(px, rr.w, rr.w * 3 / 4, rr.h * 3 / 4);
        check(is_color(c, colors[i].r, colors[i].g, colors[i].b) && (pixel(px, rr.w, rr.w / 20, rr.h / 20) & 0xFF) < 60,
              "%s: its colors (%06x)", colors[i].file, c);
        free(px);
    }
    /* XBM is black and white: the white between the square and the block. */
    struct view_reply rr;
    open_doc(sample("sample.xbm"), 0, 0, 0);
    uint32_t *px = render(0, 1, 0, &rr);
    check(pixel(px, rr.w, rr.w * 3 / 10, rr.h / 10) == 0xFFFFFF, "sample.xbm: white where the picture is white");
    free(px);
    /* The coder is the one named: PCX data called .tga is not read as PCX. */
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "cp %s /tmp/view-test-pcx.tga && chmod 644 /tmp/view-test-pcx.tga", sample("sample.pcx"));
    int copied = system(cmd) == 0;
    struct view_reply r = open_doc("/tmp/view-test-pcx.tga", 0, 0, 0);
    if (r.status == 0) {
        px = render(0, 1, 0, &rr);
        r.status = rr.status;
        free(px);
    }
    check(copied && r.status != 0, "ImageMagick: PCX data named .tga is not read by its PCX coder (%d: %s)", r.status, r.text);
    unlink("/tmp/view-test-pcx.tga");
    r = open_doc(sample("sample.tiff"), 0, 0, 0);
    struct view_reply p = page_info(1);
    check(r.pages == 2 && p.pw == 100 && p.ph == 200, "sample.tiff: page 2 is 100 x 200");
    open_doc(sample("sample.g3"), 0, 0, 0);
    px = render(0, 204 / 72.0f, 0, &rr);
    /* The bar: rows 20-59, columns 200-999. */
    check(rr.w == 1728 && (pixel(px, rr.w, 600, 40) & 0xFF) < 60 && (pixel(px, rr.w, 1200, 40) & 0xFF) > 200 && (pixel(px, rr.w, 600, 80) & 0xFF) > 200,
          "sample.g3: the fax's black bar where it was drawn");
    free(px);
    open_doc(sample("sample.fax"), 0, 0, 0);
    px = render(0, 204 / 72.0f, 0, &rr);
    check(rr.w == 1728 && (pixel(px, rr.w, 600, 40) & 0xFF) < 60 && (pixel(px, rr.w, 100, 40) & 0xFF) > 200, "sample.fax (G4): the black bar");
    free(px);
}

/* The images made here by their specifications for ImageMagick's DDS, EXR
 * and HDR coders (not by ImageMagick, which reads them): 64 x 48 pixels in
 * four quadrants, top left red, top right green, bottom left blue, bottom
 * right gray. DDS's blocks are each of one colour, so their encoding is
 * exact; OpenEXR and Radiance hold linear light, the gray 0.5 of it, which
 * is 188 in sRGB (Okular converts kimgio's linear images to sRGB). */
#define IW 64
#define IH 48

static void le16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8);
}

static void le32(unsigned char *p, uint32_t v)
{
    le16(p, v & 0xFFFF), le16(p + 2, v >> 16);
}

static void be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24), p[1] = (unsigned char)(v >> 16), p[2] = (unsigned char)(v >> 8), p[3] = (unsigned char)v;
}

static int quadrant(int x, int y)
{
    return (x >= IW / 2) + 2 * (y >= IH / 2);
}

static const unsigned char rgb8[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 128, 128, 128 } };
static const unsigned char gray8[4] = { 255, 0, 128, 64 };
static const unsigned char two8[4][2] = { { 255, 0 }, { 0, 255 }, { 255, 255 }, { 128, 128 } };

static void write_bytes(const char *name, const void *data, size_t n)
{
    FILE *f = fopen(sample(name), "wb");
    if (!f || fwrite(data, 1, n, f) != n || fclose(f) != 0 || chmod(sample(name), 0644) < 0) check(0, "%s: written", name);
}

enum { ARGB8, RGB8, R5G6B5, L8, BC1, BC2, BC3, BC4, BC5, BC6H, BC7, RGBA8, BGRA8, F16, F32, PM2, PM4 };

struct dds_variant {
    const char *file;
    int kind;
    const char *fourcc; /* NULL: a mask format */
    int dxgi;           /* in a DX10 header */
    int faces, mips;    /* a cube map of six, mipmap levels */
    int read;           /* ImageMagick reads it: 1, its colours rgb8's; 2, gray8's; 3, BC5's two */
    const char *kimgio; /* what Okular's kimgio (kimageformats a9faf5fe dds.cpp) does */
};

static const struct dds_variant dds_variants[] = {
    { "dds-argb8.dds", ARGB8, NULL, 0, 0, 1, 1, "reads it" },
    { "dds-rgb8.dds", RGB8, NULL, 0, 0, 1, 1, "reads it" },
    { "dds-r5g6b5.dds", R5G6B5, NULL, 0, 0, 1, 1, "reads it" },
    { "dds-l8.dds", L8, NULL, 0, 0, 1, 2, "reads it" },
    { "dds-dxt1.dds", BC1, "DXT1", 0, 0, 1, 1, "reads it" },
    { "dds-dxt3.dds", BC2, "DXT3", 0, 0, 1, 1, "reads it" },
    { "dds-dxt5.dds", BC3, "DXT5", 0, 0, 1, 1, "reads it" },
    { "dds-ati2.dds", BC5, "ATI2", 0, 0, 1, 3, "reads it with red and green the other way round and blue a normal's z" },
    { "dds-bc5u.dds", BC5, "BC5U", 0, 0, 1, 3, "does not read it" },
    { "dds-dx10-bc1.dds", BC1, "DX10", 71, 0, 1, 1, "reads it" },
    { "dds-dx10-bc2.dds", BC2, "DX10", 74, 0, 1, 1, "reads it" },
    { "dds-dx10-bc3.dds", BC3, "DX10", 77, 0, 1, 1, "reads it" },
    { "dds-dx10-bc5.dds", BC5, "DX10", 83, 0, 1, 3, "reads it with red and green the other way round and blue a normal's z" },
    { "dds-dx10-bc7.dds", BC7, "DX10", 98, 0, 1, 1, "reads it" },
    { "dds-dx10-rgba8.dds", RGBA8, "DX10", 28, 0, 1, 1, "does not read it" },
    { "dds-dx10-bgra8.dds", BGRA8, "DX10", 87, 0, 1, 1, "does not read it" },
    { "dds-mipmaps.dds", BC1, "DXT1", 0, 0, 7, 1, "reads it (the first level)" },
    { "dds-cube.dds", BC1, "DXT1", 0, 6, 1, 1, "shows the six faces as a cross of 4 x 3 faces; view the first face" },
    { "dds-ati1.dds", BC4, "ATI1", 0, 0, 1, 0, "does not read it either" },
    { "dds-bc4u.dds", BC4, "BC4U", 0, 0, 1, 0, "does not read it either" },
    { "dds-dx10-bc4.dds", BC4, "DX10", 80, 0, 1, 0, "reads it, as gray" },
    { "dds-dx10-bc6h.dds", BC6H, "DX10", 95, 0, 1, 0, "does not read it either" },
    { "dds-dxt2.dds", PM2, "DXT2", 0, 0, 1, 0, "reads it" },
    { "dds-dxt4.dds", PM4, "DXT4", 0, 0, 1, 0, "reads it" },
    { "dds-f16.dds", F16, "\x71", 0, 0, 1, 0, "reads it (A16B16G16R16F)" },
    { "dds-f32.dds", F32, "\x74", 0, 0, 1, 0, "reads it (A32B32G32R32F)" },
    { "dds-dx10-f16.dds", F16, "DX10", 10, 0, 1, 0, "reads it (R16G16B16A16_FLOAT)" },
    { "dds-dx10-f32.dds", F32, "DX10", 2, 0, 1, 0, "reads it (R32G32B32A32_FLOAT)" },
};

static void put_bits(unsigned char *b, int *pos, unsigned v, int n)
{
    for (int i = 0; i < n; i++, (*pos)++) if (v >> i & 1) b[*pos / 8] |= (unsigned char)(1 << (*pos % 8));
}

static unsigned rgb565(int q)
{
    return (unsigned)(rgb8[q][0] >> 3) << 11 | (unsigned)(rgb8[q][1] >> 2) << 5 | (unsigned)(rgb8[q][2] >> 3);
}

/* The half float of 0, 0.5 or 1. */
static unsigned half(float v)
{
    return v == 1 ? 0x3C00 : v == 0.5f ? 0x3800 : 0;
}

/* A 4x4 block of quadrant q (black for black) at b: its size. */
static size_t dds_block(unsigned char *b, int kind, int q, int black)
{
    int pos = 0;
    memset(b, 0, 16);
    switch (kind) {
    case BC1:
        le16(b, black ? 0 : rgb565(q));
        return 8;
    case BC2:
    case PM2:
        memset(b, 0xFF, 8);
        return 8 + dds_block(b + 8, BC1, q, black);
    case BC3:
    case PM4:
        b[0] = b[1] = 255;
        return 8 + dds_block(b + 8, BC1, q, black);
    case BC4:
        b[0] = b[1] = gray8[q];
        return 8;
    case BC5:
        b[0] = b[1] = two8[q][0];
        b[8] = b[9] = two8[q][1];
        return 16;
    case BC6H: {
        /* Mode 11 (one region, 10-bit endpoints): 495 unquantizes to 1.0,
         * 462 to 0.5. */
        static const unsigned e[4][3] = { { 495, 0, 0 }, { 0, 495, 0 }, { 0, 0, 495 }, { 462, 462, 462 } };
        put_bits(b, &pos, 0x03, 5);
        for (int i = 0; i < 6; i++) put_bits(b, &pos, e[q][i % 3], 10);
        return 16;
    }
    case BC7:
        /* Mode 6: both endpoints the colour, 7 bits each and a p-bit of 1. */
        put_bits(b, &pos, 1 << 6, 7);
        for (int i = 0; i < 4; i++) {
            unsigned v = i < 3 ? rgb8[q][i] : 255;
            put_bits(b, &pos, v >> 1, 7);
            put_bits(b, &pos, v >> 1, 7);
        }
        put_bits(b, &pos, 3, 2);
        return 16;
    }
    return 0;
}

static void make_dds(const struct dds_variant *v)
{
    static unsigned char d[IW * IH * 16 * 6 + 4096];
    static const float lin[4][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0.5f, 0.5f, 0.5f } };
    static const struct { int kind, flags, bits; uint32_t mask[4]; } masks[] = {
        { ARGB8, 0x41, 32, { 0xFF0000, 0xFF00, 0xFF, 0xFF000000u } }, { RGB8, 0x40, 24, { 0xFF0000, 0xFF00, 0xFF, 0 } },
        { R5G6B5, 0x40, 16, { 0xF800, 0x07E0, 0x001F, 0 } }, { L8, 0x20000, 8, { 0xFF, 0, 0, 0 } },
    };
    memset(d, 0, 148);
    memcpy(d, "DDS ", 4);
    le32(d + 4, 124);
    le32(d + 8, 0x1007 | (v->mips > 1 ? 0x20000 : 0));
    le32(d + 12, IH);
    le32(d + 16, IW);
    le32(d + 28, (uint32_t)v->mips);
    le32(d + 76, 32);
    le32(d + 108, 0x1000 | (v->mips > 1 ? 0x400008 : 0) | (v->faces ? 0x8 : 0));
    le32(d + 112, v->faces ? 0xFE00 : 0);
    size_t n = 128;
    int bits = 0;
    if (v->fourcc) {
        le32(d + 80, 0x4);
        memcpy(d + 84, v->fourcc, strlen(v->fourcc));
        if (v->dxgi) {
            le32(d + 128, (uint32_t)v->dxgi);
            le32(d + 132, 3); /* a 2-D texture */
            le32(d + 140, 1);
            n = 148;
        }
    }
    for (size_t i = 0; i < sizeof masks / sizeof *masks; i++) {
        if (masks[i].kind != v->kind) continue;
        bits = masks[i].bits;
        le32(d + 80, (uint32_t)masks[i].flags);
        le32(d + 88, (uint32_t)bits);
        for (int c = 0; c < 4; c++) le32(d + 92 + 4 * c, masks[i].mask[c]);
    }
    if (bits || v->kind == RGBA8 || v->kind == BGRA8 || v->kind == F16 || v->kind == F32) {
        int size = bits ? bits / 8 : v->kind == F16 ? 8 : v->kind == F32 ? 16 : 4;
        le32(d + 8, 0x100F);
        le32(d + 20, (uint32_t)(IW * size));
        for (int y = 0; y < IH; y++) {
            for (int x = 0; x < IW; x++, n += (size_t)size) {
                int q = quadrant(x, y);
                const unsigned char *c = rgb8[q];
                unsigned char *p = d + n;
                if (v->kind == ARGB8 || v->kind == RGB8) p[0] = c[2], p[1] = c[1], p[2] = c[0], p[3] = 255;
                else if (v->kind == BGRA8) p[0] = c[2], p[1] = c[1], p[2] = c[0], p[3] = 255;
                else if (v->kind == RGBA8) p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = 255;
                else if (v->kind == R5G6B5) le16(p, rgb565(q));
                else if (v->kind == L8) p[0] = gray8[q];
                else if (v->kind == F16) for (int i = 0; i < 4; i++) le16(p + 2 * i, half(i < 3 ? lin[q][i] : 1));
                else for (int i = 0; i < 4; i++) memcpy(p + 4 * i, i < 3 ? &lin[q][i] : &lin[0][0], 4); /* alpha 1 */
            }
        }
        write_bytes(v->file, d, n);
        return;
    }
    size_t block = v->kind == BC1 || v->kind == BC4 ? 8 : 16;
    le32(d + 20, (uint32_t)(IW / 4 * IH / 4 * block));
    for (int face = 0; face < (v->faces ? 6 : 1); face++) {
        /* Level 0 the picture, the smaller levels black. */
        for (int level = 0, w = IW, h = IH; level < v->mips; level++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) {
            for (int by = 0; by < (h + 3) / 4; by++) {
                for (int bx = 0; bx < (w + 3) / 4; bx++) n += dds_block(d + n, v->kind, quadrant(bx * 4, by * 4), level > 0);
            }
        }
    }
    write_bytes(v->file, d, n);
}

static size_t exr_attr(unsigned char *p, const char *name, const char *type, const void *value, uint32_t size)
{
    size_t n = strlen(name) + 1, t = strlen(type) + 1;
    memcpy(p, name, n);
    memcpy(p + n, type, t);
    le32(p + n + t, size);
    memcpy(p + n + t + 4, value, size);
    return n + t + 4 + size;
}

/* OpenEXR, by its file layout: one part of scan lines, uncompressed, with
 * half B, G and R channels (in the channel list's order). */
static void make_exr(void)
{
    static const float lin[4][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0.5f, 0.5f, 0.5f } };
    static unsigned char b[65536];
    unsigned char channels[3 * 18 + 1] = { 0 }, box[16] = { 0 }, center[8] = { 0 }, one[4], none = 0;
    float f = 1;
    memcpy(one, &f, 4);
    for (int i = 0; i < 3; i++) {
        unsigned char *c = channels + i * 18;
        c[0] = (unsigned char)"BGR"[i];
        le32(c + 2, 1);  /* HALF */
        le32(c + 10, 1); /* sampled 1 in x */
        le32(c + 14, 1); /* and in y */
    }
    le32(box + 8, IW - 1);
    le32(box + 12, IH - 1);
    size_t n = 8;
    memcpy(b, "\x76\x2F\x31\x01\x02\x00\x00\x00", 8);
    n += exr_attr(b + n, "channels", "chlist", channels, sizeof channels);
    n += exr_attr(b + n, "compression", "compression", &none, 1);
    n += exr_attr(b + n, "dataWindow", "box2i", box, 16);
    n += exr_attr(b + n, "displayWindow", "box2i", box, 16);
    n += exr_attr(b + n, "lineOrder", "lineOrder", &none, 1);
    n += exr_attr(b + n, "pixelAspectRatio", "float", one, 4);
    n += exr_attr(b + n, "screenWindowCenter", "v2f", center, 8);
    n += exr_attr(b + n, "screenWindowWidth", "float", one, 4);
    b[n++] = 0;
    size_t table = n;
    n += IH * 8;
    for (int y = 0; y < IH; y++) {
        le32(b + table + (size_t)y * 8, (uint32_t)n);
        le32(b + table + (size_t)y * 8 + 4, 0);
        le32(b + n, (uint32_t)y);
        le32(b + n + 4, 3 * IW * 2);
        n += 8;
        for (int c = 2; c >= 0; c--) for (int x = 0; x < IW; x++, n += 2) le16(b + n, half(lin[quadrant(x, y)][c]));
    }
    write_bytes("sample.exr", b, n);
}

/* Radiance HDR: flat RGBE scan lines, multiplied by an EXPOSURE of 2. */
static void make_hdr(void)
{
    static const unsigned char rgbe[4][4] = { { 128, 0, 0, 130 }, { 0, 128, 0, 130 }, { 0, 0, 128, 130 }, { 128, 128, 128, 129 } };
    static unsigned char b[IW * IH * 4 + 256];
    int n = snprintf((char *)b, 256, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\nEXPOSURE=2\n\n-Y %d +X %d\n", IH, IW);
    for (int y = 0; y < IH; y++) for (int x = 0; x < IW; x++, n += 4) memcpy(b + n, rgbe[quadrant(x, y)], 4);
    write_bytes("sample.hdr", b, (size_t)n);
}

/* An ICO holding a PNG (as Windows' large icons do): sample.png in one. */
static void make_png_ico(void)
{
    FILE *f = fopen(sample("sample.png"), "rb");
    static unsigned char b[1 << 20];
    size_t n = f ? fread(b + 22, 1, sizeof b - 22, f) : 0;
    if (f) fclose(f);
    memset(b, 0, 22);
    b[2] = 1, b[4] = 1;
    b[6] = 200, b[7] = 100, b[10] = 1, b[12] = 32;
    le32(b + 14, (uint32_t)n);
    le32(b + 18, 22);
    write_bytes("sample-png.ico", b, n + 22);
}

/* A GIMP XCF of three RGBA layers, RLE-compressed as GIMP saves them: on
 * top a hidden blue one, under it a green one over the bottom right
 * quadrant, at the bottom a red one opaque in its top half and transparent
 * in the rest. */
static void make_xcf(void)
{
    static unsigned char b[32768 + 24576];
    static const struct { int w, h, x, y, visible; unsigned char top[4], bottom[4]; } layers[] = {
        { IW, IH, 0, 0, 0, { 0, 0, 255, 255 }, { 0, 0, 255, 255 } },
        { IW / 2, IH / 2, IW / 2, IH / 2, 1, { 0, 255, 0, 255 }, { 0, 255, 0, 255 } },
        { IW, IH, 0, 0, 1, { 255, 0, 0, 255 }, { 0, 0, 0, 0 } },
    };
    size_t n = 14, at[3];
    memcpy(b, "gimp xcf file", 14);
#define U32(v) (be32(b + n, (uint32_t)(v)), n += 4)
    U32(IW), U32(IH), U32(0);          /* RGB */
    U32(17), U32(1), b[n++] = 1;       /* PROP_COMPRESSION: RLE */
    U32(0), U32(0);                    /* PROP_END */
    size_t offsets = n;
    n += 4 * 4;                        /* three layers' offsets, a 0 */
    U32(0);                            /* no channels */
    for (int i = 0; i < 3; i++) {
        at[i] = n;
        U32(layers[i].w), U32(layers[i].h), U32(1); /* RGBA */
        U32(6), memcpy(b + n, "Layer", 6), n += 6;
        U32(6), U32(4), U32(255);                   /* PROP_OPACITY */
        U32(8), U32(4), U32(layers[i].visible);     /* PROP_VISIBLE */
        U32(15), U32(8), U32(layers[i].x), U32(layers[i].y);
        U32(0), U32(0);
        U32(n + 8), U32(0);                         /* its hierarchy, no mask */
        U32(layers[i].w), U32(layers[i].h), U32(4);
        U32(n + 8), U32(0);                         /* one level */
        U32(layers[i].w), U32(layers[i].h);
        U32(n + 8), U32(0);                         /* one tile (64 x 64 at most) */
        /* Each channel in turn: a run of the top half's value, one of the
         * bottom half's (127, then the length in 16 bits, then the value). */
        unsigned half_n = (unsigned)(layers[i].w * layers[i].h / 2);
        for (int c = 0; c < 4; c++) {
            b[n++] = 127, b[n++] = (unsigned char)(half_n >> 8), b[n++] = (unsigned char)half_n, b[n++] = layers[i].top[c];
            b[n++] = 127, b[n++] = (unsigned char)(half_n >> 8), b[n++] = (unsigned char)half_n, b[n++] = layers[i].bottom[c];
        }
    }
#undef U32
    for (int i = 0; i < 3; i++) be32(b + offsets + 4 * (size_t)i, (uint32_t)at[i]);
    /* Readers take a last tile as up to 1.5 times a full one's size. */
    n += 24576;
    write_bytes("sample-layers.xcf", b, n);
}

/* A CUR: sample.ico with the type of a cursor. */
static void make_cur(void)
{
    FILE *f = fopen(sample("sample.ico"), "rb");
    static unsigned char b[1 << 16];
    size_t n = f ? fread(b, 1, sizeof b, f) : 0;
    if (f) fclose(f);
    b[2] = 2;
    write_bytes("sample.cur", b, n);
}

static void make_images(void)
{
    for (size_t i = 0; i < sizeof dds_variants / sizeof *dds_variants; i++) make_dds(&dds_variants[i]);
    make_exr();
    make_hdr();
    make_png_ico();
    make_xcf();
    make_cur();
    /* The same EXR in tiles of 16 x 16, ZIP-compressed, by OpenEXR's own tool. */
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "exrmaketiled -t 16 16 -z zip %s %s && chmod 644 %s", sample("sample.exr"), sample("sample-tiled.exr"),
             sample("sample-tiled.exr"));
    check(system(cmd) == 0, "sample-tiled.exr: made by OpenEXR's exrmaketiled");
    snprintf(cmd, sizeof cmd, "exrinfo -s %s %s >/dev/null", sample("sample.exr"), sample("sample-tiled.exr"));
    check(system(cmd) == 0, "sample.exr, sample-tiled.exr: OpenEXR's exrinfo -s (strict) takes them");
}

/* The quadrants of f's page 0 at 96 dpi (one pixel a pixel) within 10 of
 * want (top left, top right, bottom left, bottom right). */
static int quadrants(const char *f, const unsigned char want[4][3], char *got, size_t size)
{
    struct view_reply r = open_doc(sample(f), 0, 0, 0), rr;
    if (r.status) {
        snprintf(got, size, "OPEN: %d %.200s", r.status, r.text);
        return -1;
    }
    uint32_t *px = render(0, 96 / 72.0f, 0, &rr);
    int ok = px && abs(rr.w - IW) <= 1 && abs(rr.h - IH) <= 1;
    got[0] = '\0';
    for (int q = 0; q < 4; q++) {
        uint32_t p = pixel(px, rr.w, IW / 4 + (q & 1) * IW / 2, IH / 4 + (q >> 1) * IH / 2);
        int c[3] = { (int)(p >> 16 & 0xFF), (int)(p >> 8 & 0xFF), (int)(p & 0xFF) };
        for (int i = 0; i < 3; i++) ok &= abs(c[i] - want[q][i]) <= 10;
        size_t len = strlen(got);
        snprintf(got + len, size - len, "%s%06x", q ? " " : "", p & 0xFFFFFF);
    }
    free(px);
    return ok;
}

static void test_magick(void)
{
    static const unsigned char colors[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 128, 128, 128 } };
    static const unsigned char grays[4][3] = { { 255, 255, 255 }, { 0, 0, 0 }, { 128, 128, 128 }, { 64, 64, 64 } };
    /* ImageMagick's BC5: red the first block, green the second, blue 0. */
    static const unsigned char two[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 255, 255, 0 }, { 128, 128, 0 } };
    static const unsigned char linear[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 188, 188, 188 } };
    char got[256];
    for (size_t i = 0; i < sizeof dds_variants / sizeof *dds_variants; i++) {
        const struct dds_variant *v = &dds_variants[i];
        int ok = quadrants(v->file, v->read == 2 ? grays : v->read == 3 ? two : colors, got, sizeof got);
        if (v->read) {
            check(ok == 1, "%s: the quadrants red, green, blue, gray%s (%s); kimgio %s", v->file,
                  v->read == 2 ? " as grays" : v->read == 3 ? " as BC5's red and green" : "", got, v->kimgio);
        } else if (ok < 0) {
            printf("GAP  %s: ImageMagick does not read it (%s); kimgio %s\n", v->file, got, v->kimgio);
        } else {
            check(0, "%s: now read (%s): list it as read", v->file, got);
        }
    }
    /* The XCF's layers made one: the bottom one's red top half, the green
     * over the bottom right, the transparent rest white, the hidden blue
     * nowhere. */
    static const unsigned char layered[4][3] = { { 255, 0, 0 }, { 255, 0, 0 }, { 255, 255, 255 }, { 0, 255, 0 } };
    check(quadrants("sample-layers.xcf", layered, got, sizeof got) == 1, "sample-layers.xcf: its visible layers made one (%s)", got);
    /* Uncompressed XCF: ImageMagick 7.1.2 coders/xcf.c load_tile turns
     * alpha==255 transparent, so it draws white; RLE XCF (GIMP's default,
     * sample-layers.xcf above) is fine. Check OPEN/PAGE/RENDER and record the
     * upstream colour gap until ImageMagick fixes it. */
    struct view_reply xr = open_doc(sample("sample.xcf"), 400, 600, 12), xp = page_info(0), xrr;
    uint32_t *xpx = render(0, 1, 0, &xrr);
    check(xr.status == 0 && !strncmp(xr.text, "Image\t", 6) && xr.pages == 1 &&
          fabsf(xp.pw - 48) < 1 && fabsf(xp.ph - 48) < 1 && xrr.status == 0 && xrr.w == 48 && xrr.h == 48,
          "sample.xcf: OPEN, PAGE 48x48 and RENDER succeed");
    uint32_t xc = pixel(xpx, xrr.w, xrr.w * 3 / 4, xrr.h * 3 / 4);
    if (is_color(xc, 0x80, 0, 0xE0)) {
        check(1, "sample.xcf: its colors (%06x)", xc);
    } else {
        printf("GAP  sample.xcf: uncompressed XCF draws %06x (upstream ImageMagick coders/xcf.c alpha==255 bug; RLE XCF passes)\n", xc & 0xFFFFFF);
    }
    free(xpx);
    static const char *const hdr[] = { "sample.exr", "sample-tiled.exr", "sample.hdr" };
    for (int i = 0; i < 3; i++) {
        check(quadrants(hdr[i], linear, got, sizeof got) == 1, "%s: linear light as sRGB, gray 0.5 at 188%s (%s)", hdr[i],
              i == 2 ? ", its EXPOSURE divided out" : "", got);
    }
    open_doc(sample("sample-png.ico"), 0, 0, 0);
    struct view_reply rr;
    uint32_t *px = render(0, 1, 0, &rr);
    check(is_color(pixel(px, rr.w, rr.w * 3 / 4, rr.h * 3 / 4), 0xC0, 0, 0), "sample-png.ico: the PNG it holds, by ImageMagick's PNG coder");
    free(px);
    /* A DDS saying 16384 x 16384 pixels (more than VIEW_MAX_PIXELS) and
     * holding none: refused before its pixels are allocated. */
    unsigned char d[128] = "DDS ";
    le32(d + 4, 124), le32(d + 8, 0x81007), le32(d + 12, 16384), le32(d + 16, 16384), le32(d + 20, 16384 / 4 * 16384 / 4 * 8);
    le32(d + 76, 32), le32(d + 80, 4), memcpy(d + 84, "DXT1", 4), le32(d + 108, 0x1000);
    write_bytes("dds-huge.dds", d, sizeof d);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct view_reply r = open_doc(sample("dds-huge.dds"), 0, 0, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    check(r.status != 0 && s < 2 && open_doc(sample("dds-dxt1.dds"), 0, 0, 0).status == 0,
          "dds-huge.dds: 16384 x 16384 refused in %.2f s (%d: %s), the next opens", s, r.status, r.text);
    /* The policy view-render gives ImageMagick: these coders only, no
     * delegate programs (as in this process, view-image.c's). */
    check(magick_start(), "ImageMagick: view-render's security policy is set");
    static const char *const allowed[] = { "CUR", "DDS", "EXR", "HDR", "ICO", "MNG", "PCX", "PNG", "SGI", "TGA", "XBM", "XCF", "XPM" };
    static const char *const refused[] = { "SVG", "MSVG", "MVG", "MSL", "PS", "EPS", "PDF", "TXT", "TEXT", "URL", "HTTPS", "JPEG", "BMP", "VIDEO", "EPHEMERAL", "INLINE" };
    int all = 1, none = 1;
    for (size_t i = 0; i < sizeof allowed / sizeof *allowed; i++) all &= IsRightsAuthorized(CoderPolicyDomain, ReadPolicyRights, allowed[i]) == MagickTrue;
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++) none &= IsRightsAuthorized(CoderPolicyDomain, ReadPolicyRights, refused[i]) == MagickFalse;
    check(all && none && IsRightsAuthorized(DelegatePolicyDomain, ExecutePolicyRights, "ps:alpha") == MagickFalse &&
          IsRightsAuthorized(DelegatePolicyDomain, ExecutePolicyRights, "https") == MagickFalse,
          "ImageMagick: read rights for the 13 coders only, no delegate programs");
    static const char svg[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\"/>";
    char why[256];
    int w, h;
    unsigned char *rgba = magick_rgba(svg, sizeof svg - 1, "SVG", VIEW_MAX_PIXELS, &w, &h, why, sizeof why);
    check(!rgba && strstr(why, "not authorized"), "ImageMagick: an SVG read with its coder is refused (%s)", why);
    free(rgba);
}

/* Fonts dvipdfmx (or kpathsea) could not find, in what it said. */
static int font_trouble(const char *log)
{
    static const char *const words[] = { "font", "tfm", "kpathsea", "mktex", "not found", "could not", "unable" };
    for (size_t i = 0; i < sizeof words / sizeof *words; i++) if (strcasestr(log, words[i])) return 1;
    return 0;
}

static void test_dvi(void)
{
    int have = access("/usr/bin/dvipdfmx", X_OK) == 0;
    struct view_reply r = open_doc(sample("sample.dvi"), 0, 0, 0);
    if (!have) {
        check(r.status == ENOENT && !strcmp(r.text, "DVI needs TeX: install texlive-bin"), "sample.dvi: without dvipdfmx OPEN says \"%s\"", r.text);
        r = open_doc(sample("sample.dvi.gz"), 0, 0, 0);
        check(r.status == ENOENT && strstr(r.text, "texlive-bin"), "sample.dvi.gz: unpacked, the same");
        return;
    }
    check(r.status == 0 && !strncmp(r.text, "DVI\t", 4), "sample.dvi: dvipdfmx is installed, OPEN converts it (%s)", r.text);
    /* TeX's DVI (make-view-samples.sh), its text in Computer Modern: what
     * dvipdfmx says goes to view-render's stderr, here a file. */
    char log[] = "/tmp/view-test-dvi-XXXXXX", said[4096] = "";
    int fd = mkstemp(log), saved = dup(2);
    fflush(stderr);
    if (fd < 0 || saved < 0 || dup2(fd, 2) < 0) {
        check(0, "DVI: view-render's stderr to a file");
        return;
    }
    start();
    static const char *const files[] = { "sample-tex.dvi", "sample-tex.dvi.gz" };
    int ok[2];
    char text[2][256];
    for (int i = 0; i < 2; i++) {
        r = open_doc(sample(files[i]), 0, 0, 0);
        int n;
        free(search(0, "Hello", &n));
        char *t = r.status ? NULL : bulk(VIEW_EXPORT);
        ok[i] = r.status == 0 && r.pages == 1 && n == 1 && t && strstr(t, "Hello DVI view");
        snprintf(text[i], sizeof text[i], "%d: %.100s, %d pages, \"%.60s\"", r.status, r.text, r.pages, t ? t : "");
        free(t);
    }
    dup2(saved, 2);
    close(saved);
    ssize_t got = pread(fd, said, sizeof said - 1, 0);
    said[got > 0 ? got : 0] = '\0';
    close(fd);
    start();
    unlink(log);
    for (int i = 0; i < 2; i++) check(ok[i], "%s: TeX's DVI is one page with \"Hello DVI view\" (%s)", files[i], text[i]);
    check(!font_trouble(said), "DVI: nothing said of missing fonts (\"%s\")", said);
}

static int status_field(pid_t p, const char *key, char *out, size_t size)
{
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/status", (int)p);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, strlen(key)) && line[strlen(key)] == ':') {
            snprintf(out, size, "%s", line + strlen(key) + 2);
            out[strcspn(out, "\n")] = '\0';
            found = 0;
        }
    }
    fclose(f);
    return found;
}

/* Interfaces in p's network namespace, from its /proc/PID/net/dev. */
static void interfaces(pid_t p, char *out, size_t size)
{
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/net/dev", (int)p);
    FILE *f = fopen(path, "r");
    out[0] = '\0';
    for (int i = 0; f && fgets(line, sizeof line, f); i++) {
        char *colon = strchr(line, ':');
        if (i < 2 || !colon) continue;
        *colon = '\0';
        char *name = line + strspn(line, " ");
        size_t n = strlen(out);
        snprintf(out + n, size - n, "%s%s", n ? " " : "", name);
    }
    if (f) fclose(f);
}

static void test_sandbox(void)
{
    char v[512];
    start();
    open_doc(sample("sample.pdf"), 0, 0, 0);
    check(status_field(pid, "Uid", v, sizeof v) == 0 && !strcmp(v, "1000\t1000\t1000\t1000"), "sandbox: uids are 1000 (%s)", v);
    check(status_field(pid, "Gid", v, sizeof v) == 0 && !strcmp(v, "1000\t1000\t1000\t1000"), "sandbox: gids are 1000 (%s)", v);
    status_field(pid, "Groups", v, sizeof v);
    check(!strstr(v, " 0 ") && strncmp(v, "0 ", 2) != 0, "sandbox: groups are the user's (%s)", v);
    check(status_field(pid, "NoNewPrivs", v, sizeof v) == 0 && !strcmp(v, "1"), "sandbox: NoNewPrivs is %s", v);
    status_field(pid, "CapEff", v, sizeof v);
    check(!strcmp(v, "0000000000000000"), "sandbox: no capabilities (%s)", v);
    interfaces(pid, v, sizeof v);
    check(!strcmp(v, "lo"), "sandbox: network interfaces: %s", v);
    char path[64], env[1024] = "";
    snprintf(path, sizeof path, "/proc/%d/environ", (int)pid);
    int fd = open(path, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, env, sizeof env - 1) : 0;
    if (fd >= 0) close(fd);
    for (ssize_t i = 0; i < n; i++) if (!env[i]) env[i] = ' ';
    env[n > 0 ? n : 0] = '\0';
    check(!strcmp(env, "HOME=/home/tester USER=tester LANG=C.UTF-8 PATH=/usr/bin "), "sandbox: environment is \"%s\"", env);
    snprintf(path, sizeof path, "/proc/%d/fd", (int)pid);
    int fds = 0;
    for (int i = 0; i < 64; i++) {
        char p[96];
        snprintf(p, sizeof p, "%s/%d", path, i);
        if (access(p, F_OK) == 0) fds++;
    }
    check(fds >= 4 && fds <= 8, "sandbox: %d descriptors open (0-3, and the document's)", fds);

    /* A file only root can read. */
    const char *secret = "/tmp/view-test-secret.pdf";
    int in = open(sample("sample.pdf"), O_RDONLY), outfd = open(secret, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    char buf[65536];
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(outfd, buf, (size_t)n) != n) break;
    close(in);
    close(outfd);
    struct view_reply r = open_doc(secret, 0, 0, 0);
    check(r.status == EACCES && !(r.flags & VIEW_NEEDS_PASSWORD), "sandbox: OPEN of a 0600 root file is EACCES (%s)", r.text);
    unlink(secret);
}

/* view-render started directly, as root, or as the user without no_new_privs
 * or with the network: it must refuse. */
static void refuses(const char *what, int uid, int nnp, int netns, const char *expect)
{
    int err[2], sv[2];
    if (pipe(err) < 0 || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) return;
    pid_t p = fork();
    if (p == 0) {
        dup2(err[1], 2);
        dup2(sv[1], 3);
        if (netns && unshare(CLONE_NEWNET) < 0) _exit(126);
        if (uid && (setgroups(0, NULL) < 0 || setgid(1000) < 0 || setuid(1000) < 0)) _exit(126);
        if (nnp && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) _exit(126);
        execl(renderer, "view-render", (char *)NULL);
        _exit(127);
    }
    close(err[1]);
    close(sv[1]);
    char msg[512] = "";
    size_t got = 0;
    for (ssize_t n; got + 1 < sizeof msg && (n = read(err[0], msg + got, sizeof msg - 1 - got)) > 0;) got += (size_t)n;
    msg[got] = '\0';
    int status;
    waitpid(p, &status, 0);
    close(err[0]);
    close(sv[0]);
    msg[strcspn(msg, "\n")] = '\0';
    check(WIFEXITED(status) && WEXITSTATUS(status) == 1 && strstr(msg, expect), "sandbox: %s: refuses (\"%s\")", what, msg);
}

static void test_refusals(void)
{
    refuses("started as root", 0, 1, 1, "refusing to run as root");
    refuses("without no_new_privs", 1, 0, 1, "without no_new_privs");
    char v[256];
    interfaces(getpid(), v, sizeof v);
    if (strcmp(v, "lo") != 0) refuses("with the network", 1, 1, 0, "refusing to run with network interface");
    else printf("SKIP sandbox: with the network: this namespace has only lo\n");
}

static void test_cancel(void)
{
    open_doc(sample("heavy.pdf"), 0, 0, 0);
    struct view_request q = req(VIEW_RENDER, 0);
    q.zoom = 3;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct view_reply r = call(&q, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double full = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    check(r.status == 0, "cancel: heavy.pdf draws in %.2f s uncancelled", full);
    view_send(sock, &q);
    struct timespec pause = { 0, (long)(full * 0.3 * 1e9) };
    nanosleep(&pause, NULL);
    kill(pid, SIGUSR1);
    int fd;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (view_recv(sock, &r, &fd) == 0 && fd >= 0) close(fd);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double rest = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    check(r.status == ECANCELED && r.size == 0, "cancel: SIGUSR1 after %.2f s: RENDER is ECANCELED %.2f s later (%d)", full * 0.3, rest, r.status);
    q.zoom = 0.5;
    r = call(&q, NULL);
    check(r.status == 0, "cancel: the next RENDER is not cancelled");
    /* SEARCH runs the page into text the same way, cancelled alike. */
    q = req(VIEW_SEARCH, 0);
    snprintf(q.text, sizeof q.text, "nothing");
    view_send(sock, &q);
    nanosleep(&pause, NULL);
    kill(pid, SIGUSR1);
    if (view_recv(sock, &r, &fd) == 0 && fd >= 0) close(fd);
    check(r.status == ECANCELED, "cancel: SIGUSR1 during SEARCH: ECANCELED (%d)", r.status);
    r = call(&q, NULL);
    check(r.status == 0 && r.count == 0, "cancel: the next SEARCH runs (%d hits)", r.count);
}

static void test_garbled(void)
{
    start();
    struct view_reply r;
    int fd;
    send(sock, "short", 5, MSG_NOSIGNAL);
    check(view_recv(sock, &r, &fd) < 0, "a garbled request: view-render stops (EOF)");
    start();
}

/* PDF interaction (VIEW_PDF_*) on a copy of sample.pdf the user owns: the
 * session's state, annotations made, moved, changed, deleted, undone and
 * listed, a save reopened; every malformed request refused with the
 * document unchanged; and none of it for a document that is not a PDF. */
static struct view_request pdf_req(int op, int page)
{
    struct view_request q = req(op, page);
    q.color_n = 3;
    q.color[0] = 1;
    q.opacity = 1;
    q.width = 1;
    return q;
}

static int pdf_annots_of(int page, struct view_annot **out)
{
    struct view_request q = pdf_req(VIEW_PDF_ANNOTS, page);
    void *data;
    struct view_reply r = call(&q, &data);
    *out = data;
    return r.status ? -1 : r.count;
}

static const struct view_annot *pdf_annot_type(const struct view_annot *a, int n, int type)
{
    for (int k = 0; k < n; k++) if (a[k].type == type) return &a[k];
    return NULL;
}

static uint64_t pdf_revision(void)
{
    struct view_request q = pdf_req(VIEW_PDF_STATUS, 0);
    return call(&q, NULL).revision;
}

static int pdf_refused(struct view_request *q, uint64_t revision)
{
    struct view_reply r = call(q, NULL);
    return r.status == EINVAL && r.size == 0 && pdf_revision() == revision;
}

static void test_pdf_edit(void)
{
    const char *dir = "/tmp/view-test-pdf", *path = "/tmp/view-test-pdf/edit.pdf", *saved = "/tmp/view-test-pdf/saved.pdf";
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -m 700 %s && cp '%s' %s && chown -R 1000 %s", dir, dir, sample("sample.pdf"), path, dir);
    if (!check(system(cmd) == 0, "PDF edit: a copy of sample.pdf for uid 1000")) return;
    struct view_reply r = open_doc(path, 0, 0, 0);
    struct view_request q = pdf_req(VIEW_PDF_STATUS, 0);
    r = call(&q, NULL);
    check(r.status == 0 && !(r.flags & VIEW_PDF_DIRTY) && (r.flags & VIEW_PDF_CAN_ANNOTATE) && !(r.flags & VIEW_PDF_HAS_FORMS) &&
          !(r.flags & VIEW_PDF_CAN_UNDO) && r.count == 0, "PDF edit: STATUS of a clean PDF (flags %x)", r.flags);
    uint64_t revision = r.revision;

    q = pdf_req(VIEW_PDF_CREATE, 0);
    q.mode = VIEW_ANNOT_SQUARE;
    q.width = 4;
    q.ax = 100, q.ay = 300, q.bx = 200, q.by = 360;
    snprintf(q.text, sizeof q.text, "Box");
    snprintf(q.author, sizeof q.author, "Tester");
    r = call(&q, NULL);
    int object = r.w, generation = r.h;
    check(r.status == 0 && object > 0 && (r.flags & VIEW_PDF_DIRTY) && (r.flags & VIEW_PDF_CAN_UNDO) && r.revision > revision,
          "PDF edit: CREATE a rectangle: object %d, dirty, undoable, a new revision", object);
    q = pdf_req(VIEW_PDF_CREATE, 0);
    q.mode = VIEW_ANNOT_HIGHLIGHT;
    q.color[1] = 1;
    q.ax = 70, q.ay = 65, q.bx = 127, q.by = 65;
    check(call(&q, NULL).status == 0, "PDF edit: CREATE a highlight from the ends of the text \"Hello\"");
    q.ax = 300, q.ay = 700, q.bx = 310, q.by = 700;
    r = call(&q, NULL);
    check(r.status == EINVAL && strstr(r.text, "no text"), "PDF edit: CREATE a highlight where there is no text: EINVAL (%s)", r.text);

    struct view_annot *a;
    int n = pdf_annots_of(0, &a);
    const struct view_annot *box = pdf_annot_type(a, n, VIEW_ANNOT_SQUARE), *mark = pdf_annot_type(a, n, VIEW_ANNOT_HIGHLIGHT);
    check(n == 2 && box && mark && box->object == object && fabsf(box->x0 - 100) < .01 && fabsf(box->y1 - 360) < .01 &&
          !strcmp(box->contents, "Box") && !strcmp(box->author, "Tester") && box->editable && box->color_n == 3 && box->color[0] > .99,
          "PDF edit: ANNOTS lists both, the rectangle as made");
    check(mark && mark->x0 > 60 && mark->x0 < 80 && mark->x1 > 90 && mark->y0 < 65 && mark->y1 > 60,
          "PDF edit: the highlight covers \"Hello\" (%.0f,%.0f)-(%.0f,%.0f)", mark ? mark->x0 : 0, mark ? mark->y0 : 0,
          mark ? mark->x1 : 0, mark ? mark->y1 : 0);
    free(a);
    struct view_reply rr;
    uint32_t *px = render(0, 1, 0, &rr);
    int red = 0;
    for (int x = 98; px && x <= 106; x++) red |= is_color(pixel(px, rr.w, x, 330), 255, 0, 0);
    check(rr.status == 0 && red, "PDF edit: RENDER after CREATE draws the rectangle's red border (%06x %06x %06x)",
          pixel(px, rr.w, 100, 330), pixel(px, rr.w, 102, 330), pixel(px, rr.w, 150, 300));
    free(px);

    q = pdf_req(VIEW_PDF_MOVE, 0);
    q.object = object, q.generation = generation;
    q.ax = 150, q.ay = 400, q.bx = 250, q.by = 460;
    check(call(&q, NULL).status == 0, "PDF edit: MOVE the rectangle");
    n = pdf_annots_of(0, &a);
    box = pdf_annot_type(a, n, VIEW_ANNOT_SQUARE);
    check(box && fabsf(box->x0 - 150) < .01 && fabsf(box->y0 - 400) < .01, "PDF edit: ANNOTS has it moved");
    free(a);
    q = pdf_req(VIEW_PDF_UNDO, 0);
    check(call(&q, NULL).status == 0, "PDF edit: UNDO");
    n = pdf_annots_of(0, &a);
    box = pdf_annot_type(a, n, VIEW_ANNOT_SQUARE);
    check(box && fabsf(box->x0 - 100) < .01, "PDF edit: UNDO puts it back");
    free(a);
    q = pdf_req(VIEW_PDF_REDO, 0);
    r = call(&q, NULL);
    check(r.status == 0 && !(r.flags & VIEW_PDF_CAN_REDO), "PDF edit: REDO, nothing more to redo");
    q = pdf_req(VIEW_PDF_UPDATE, 0);
    q.object = object, q.generation = generation, q.mask = VIEW_SET_CONTENTS | VIEW_SET_OPACITY;
    q.opacity = .5f;
    snprintf(q.text, sizeof q.text, "Box 2\nsecond line");
    check(call(&q, NULL).status == 0, "PDF edit: UPDATE contents and opacity");
    q = pdf_req(VIEW_PDF_DELETE, 0);
    q.object = object, q.generation = generation;
    check(call(&q, NULL).status == 0 && pdf_annots_of(0, &a) == 1, "PDF edit: DELETE leaves one");
    free(a);
    q = pdf_req(VIEW_PDF_UNDO, 0);
    call(&q, NULL);
    n = pdf_annots_of(0, &a);
    box = pdf_annot_type(a, n, VIEW_ANNOT_SQUARE);
    check(n == 2 && box && !strcmp(box->contents, "Box 2\nsecond line") && fabsf(box->opacity - .5f) < .01,
          "PDF edit: UNDO of DELETE: the rectangle back with its new contents (new line kept) and opacity");
    free(a);

    /* Malformed requests: refused, nothing changed. */
    revision = pdf_revision();
    struct { const char *what; struct view_request q; } bad[24];
    int nbad = 0;
#define BAD(text, op) (bad[nbad].what = text, bad[nbad].q = pdf_req(op, 0), &bad[nbad++].q)
    struct view_request *b;
    b = BAD("an unknown annotation type", VIEW_PDF_CREATE); b->mode = 99; b->ax = 1, b->ay = 1, b->bx = 9, b->by = 9;
    b = BAD("2 colour components", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_SQUARE; b->color_n = 2; b->bx = 9, b->by = 9;
    b = BAD("a colour of 1.5", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_SQUARE; b->color[2] = 1.5f; b->bx = 9, b->by = 9;
    b = BAD("opacity NaN", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_SQUARE; b->opacity = NAN; b->bx = 9, b->by = 9;
    b = BAD("width -1", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_SQUARE; b->width = -1; b->bx = 9, b->by = 9;
    b = BAD("an infinite corner", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_SQUARE; b->bx = INFINITY, b->by = 9;
    b = BAD("a rectangle without area", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_SQUARE; b->ax = b->bx = 5, b->by = 9;
    b = BAD("an icon with a space", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_STAMP; snprintf(b->icon, sizeof b->icon, "Dr aft"); b->bx = 9, b->by = 9;
    b = BAD("an icon without its NUL", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_STAMP; memset(b->icon, 'A', sizeof b->icon); b->bx = 9, b->by = 9;
    b = BAD("an author without its NUL", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_SQUARE; memset(b->author, 'a', sizeof b->author); b->bx = 9, b->by = 9;
    b = BAD("contents not UTF-8", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_SQUARE; snprintf(b->text, sizeof b->text, "\xff\xfe"); b->bx = 9, b->by = 9;
    b = BAD("too many points", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_INK; b->count = VIEW_PDF_POINTS + 1;
    b = BAD("ink strokes not adding up", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_INK; b->count = 3; b->strokes = 2; b->stroke[0] = b->stroke[1] = 1;
    b = BAD("a NaN point", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_LINE; b->count = 2; b->points[3] = NAN;
    b = BAD("a line of three points", VIEW_PDF_CREATE); b->mode = VIEW_ANNOT_LINE; b->count = 3; b->points[2] = b->points[5] = 9;
    b = BAD("UPDATE of nothing", VIEW_PDF_UPDATE); b->object = object, b->generation = generation;
    b = BAD("UPDATE of an unknown property", VIEW_PDF_UPDATE); b->object = object, b->generation = generation; b->mask = 1u << 10;
    b = BAD("MOVE of no such annotation", VIEW_PDF_MOVE); b->object = 99999; b->bx = 9, b->by = 9;
    b = BAD("DELETE with generation -1", VIEW_PDF_DELETE); b->object = object, b->generation = -1;
    b = BAD("DELETE on page 7 of 2", VIEW_PDF_DELETE); b->page = 7; b->object = object;
    b = BAD("SET of 65 values", VIEW_PDF_SET); b->object = object; b->count = 65;
    b = BAD("SET of no such field", VIEW_PDF_SET); b->object = object; b->count = 1;
    b = BAD("WRITE to a relative path", VIEW_PDF_WRITE); snprintf(b->text, sizeof b->text, "relative.pdf");
    b = BAD("WRITE with an unknown mode", VIEW_PDF_WRITE); snprintf(b->text, sizeof b->text, "%s", saved); b->mode = 2;
#undef BAD
    for (int k = 0; k < nbad; k++) check(pdf_refused(&bad[k].q, revision), "PDF edit: refused (EINVAL), nothing changed: %s", bad[k].what);
    check(access(saved, F_OK) < 0, "PDF edit: no file written by a refused WRITE");

    q = pdf_req(VIEW_PDF_FIELDS, 0);
    r = call(&q, NULL);
    check(r.status == 0 && r.count == 0 && r.size == 0, "PDF edit: FIELDS of a page without forms: none");
    q = pdf_req(VIEW_PDF_SIGNATURES, 0);
    r = call(&q, NULL);
    check(r.status == 0 && r.count == 0, "PDF edit: SIGNATURES of an unsigned PDF: none");

    q = pdf_req(VIEW_PDF_WRITE, 0);
    snprintf(q.text, sizeof q.text, "%s", saved);
    r = call(&q, NULL);
    struct stat st;
    check(r.status == 0 && !(r.flags & VIEW_PDF_DIRTY) && stat(saved, &st) == 0 && st.st_uid == 1000 && (st.st_mode & 0777) == 0600,
          "PDF edit: WRITE as uid 1000 (a new file, mode 600), then clean");
    q = pdf_req(VIEW_PDF_WRITE, 0);
    snprintf(q.text, sizeof q.text, "/root/view-test-not-allowed.pdf");
    r = call(&q, NULL);
    check(r.status != 0 && pdf_revision() == revision, "PDF edit: WRITE where uid 1000 may not: refused (%s)", r.text);
    open_doc(saved, 0, 0, 0);
    n = pdf_annots_of(0, &a);
    box = n > 0 ? pdf_annot_type(a, n, VIEW_ANNOT_SQUARE) : NULL;
    check(n == 2 && box && !strcmp(box->contents, "Box 2\nsecond line") && fabsf(box->x0 - 150) < .01,
          "PDF edit: the saved file reopened: both annotations, as edited");
    free(a);

    open_doc(sample("sample.epub"), 400, 600, 12);
    q = pdf_req(VIEW_PDF_STATUS, 0);
    r = call(&q, NULL);
    check(r.status == ENOTSUP, "PDF edit: STATUS of an EPUB: ENOTSUP (%s)", r.text);
    q = pdf_req(VIEW_PDF_UNDO, 0);
    check(call(&q, NULL).status == ENOTSUP, "PDF edit: UNDO of an EPUB: ENOTSUP");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: test-view-render SAMPLES RENDERER\n");
        return 2;
    }
    samples = argv[1];
    renderer = argv[2];
    signal(SIGPIPE, SIG_IGN);
    make_images();
    start();
    for (size_t i = 0; i < sizeof formats / sizeof *formats; i++) test_format(&formats[i]);
    static const char *const cjk[] = { "sample.epub", "sample.md", "sample-utf8.txt", "sample-gb18030.txt", "sample.html" };
    for (size_t i = 0; i < sizeof cjk / sizeof *cjk; i++) test_cjk(cjk[i]);
    test_markdown();
    test_pdf();
    test_locked();
    test_epub();
    test_djvu();
    test_ps();
    test_images();
    test_magick();
    test_dvi();
    test_pdf_edit();
    test_cancel();
    test_sandbox();
    test_refusals();
    test_garbled();
    close(sock);
    waitpid(pid, NULL, 0);
    printf("%s: %d failed\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
