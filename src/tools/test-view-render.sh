#!/bin/sh
# test-view-render.sh: view-render's test, run as root in the development
# root with the source at /work: the samples made (and those of the formats
# checked here), then the renderer and its tests built normally and with
# ASan/UBSan, and each run.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
dir=/tmp/view-test
rm -rf "$dir"
mkdir -p "$dir"
chmod 755 "$dir"
sh "$src/tools/make-view-samples.sh" "$dir/samples"
# OpenRaster, Krita and OpenDocument text samples: zips with their
# mimetype first, a flat .fodt, the same by their content (no extension),
# an encrypted ODT and an OpenRaster file without its merged image; camera
# RAW and CHM samples (below).
python3 -I - "$dir/formats" <<'PY'
import base64, os, shutil, struct, sys, zipfile, zlib
out = sys.argv[1]
os.makedirs(out)
ODT = 'application/vnd.oasis.opendocument.text'

def png(w, h, color):
    rows = b''.join(b'\0' + b''.join(bytes(color(x, y)) for x in range(w)) for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))

def halves(left, right):
    return png(64, 48, lambda x, y: left if x < 32 else right)

BLACK, RED, GREEN, BLUE = (0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255)

def package(name, mimetype, files):
    with zipfile.ZipFile(os.path.join(out, name), 'w') as z:
        z.writestr(zipfile.ZipInfo('mimetype'), mimetype)
        for entry, data in files:
            z.writestr(entry, data, zipfile.ZIP_DEFLATED)

stack = '<?xml version="1.0"?><image w="64" h="48"><stack><layer src="data/layer0.png"/></stack></image>'
package('sample.ora', 'image/openraster', [('stack.xml', stack), ('data/layer0.png', halves(RED, RED)),
        ('Thumbnails/thumbnail.png', halves(BLACK, BLACK)), ('mergedimage.png', halves(BLUE, GREEN))])
shutil.copy(os.path.join(out, 'sample.ora'), os.path.join(out, 'ora-by-content'))
package('nomerged.ora', 'image/openraster', [('stack.xml', stack), ('data/layer0.png', halves(RED, RED))])
package('sample.kra', 'application/x-krita', [('maindoc.xml', '<?xml version="1.0"?><DOC/>'),
        ('preview.png', halves(BLACK, BLACK)), ('mergedimage.png', halves(GREEN, RED))])

NS = ' '.join('xmlns:%s="urn:oasis:names:tc:opendocument:xmlns:%s"' % p for p in [
    ('office', 'office:1.0'), ('style', 'style:1.0'), ('text', 'text:1.0'), ('table', 'table:1.0'),
    ('draw', 'drawing:1.0'), ('fo', 'xsl-fo-compatible:1.0'), ('svg', 'svg-compatible:1.0'), ('meta', 'meta:1.0')])
NS += ' xmlns:xlink="http://www.w3.org/1999/xlink" xmlns:dc="http://purl.org/dc/elements/1.1/"'
STYLES = ('<office:styles><style:default-style style:family="paragraph"><style:text-properties fo:font-size="12pt"/>'
          '</style:default-style><style:style style:name="Standard" style:family="paragraph"/>'
          '<style:style style:name="Heading_20_1" style:family="paragraph" style:parent-style-name="Standard">'
          '<style:text-properties fo:font-size="150%" fo:font-weight="bold"/></style:style></office:styles>')
AUTO = ('<office:automatic-styles>'
        '<style:style style:name="P1" style:family="paragraph" style:parent-style-name="Standard">'
        '<style:paragraph-properties fo:text-align="center"/></style:style>'
        '<style:style style:name="T1" style:family="text"><style:text-properties fo:color="#0000ff" fo:font-weight="bold"/></style:style>'
        '<style:style style:name="T2" style:family="text"><style:text-properties fo:font-size="24pt"/></style:style>'
        '<style:style style:name="Cell" style:family="table-cell"><style:table-cell-properties fo:border="3pt solid #00aa00" fo:padding="2pt"/></style:style>'
        '<text:list-style style:name="L1"><text:list-level-style-number text:level="1" style:num-format="1"/></text:list-style>'
        '</office:automatic-styles>')

def body(image):
    return ('<office:body><office:text><text:sequence-decls><text:sequence-decl text:name="Illustration"/></text:sequence-decls>'
            '<text:h text:style-name="Heading_20_1" text:outline-level="1">Chapter One</text:h>'
            '<text:p text:style-name="P1">The <text:span text:style-name="T1">bluejay</text:span> met a pangolin'
            '<text:note text:id="ftn1" text:note-class="footnote"><text:note-citation>1</text:note-citation>'
            '<text:note-body><text:p>footnotebody</text:p></text:note-body></text:note>'
            '<office:annotation><dc:creator>A</dc:creator><text:p>annotationsecret</text:p></office:annotation>.</text:p>'
            '<text:p>Big <text:span text:style-name="T2">mammoth</text:span> here</text:p>'
            '<text:list text:style-name="L1"><text:list-item><text:p>listitemone</text:p></text:list-item>'
            '<text:list-item><text:p>listitemtwo</text:p></text:list-item></text:list>'
            '<table:table table:name="Table1"><table:table-column table:number-columns-repeated="2"/><table:table-row>'
            '<table:table-cell table:style-name="Cell"><text:p>cellay</text:p></table:table-cell>'
            '<table:table-cell table:style-name="Cell"><text:p>cellbee</text:p></table:table-cell></table:table-row></table:table>'
            '<text:p><draw:frame svg:width="1in" svg:height="0.5in">' + image + '</draw:frame></text:p>'
            '<text:p><draw:frame svg:width="1in" svg:height="1in"><draw:object xlink:href="./Object 1"/>'
            '<draw:image xlink:href="./ObjectReplacements/Object 1"/></draw:frame></text:p>'
            '<text:h text:outline-level="2">Section Two</text:h>'
            '<text:p>Go <text:a xlink:type="simple" xlink:href="https://example.org/">weblink</text:a> now</text:p>'
            '</office:text></office:body>')

head = '<?xml version="1.0" encoding="UTF-8"?>'
manifest = (head + '<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0">'
            '<manifest:file-entry manifest:full-path="/" manifest:media-type="' + ODT + '"/>%s</manifest:manifest>')
red = png(72, 36, lambda x, y: RED)
package('sample.odt', ODT, [
    ('content.xml', head + '<office:document-content ' + NS + '>' + AUTO + body('<draw:image xlink:href="Pictures/red.png"/>') +
     '</office:document-content>'),
    ('styles.xml', head + '<office:document-styles ' + NS + '>' + STYLES + '</office:document-styles>'),
    ('meta.xml', head + '<office:document-meta ' + NS + '><office:meta><dc:title>ODT Sample</dc:title></office:meta></office:document-meta>'),
    ('META-INF/manifest.xml', manifest % ''), ('Pictures/red.png', red), ('ObjectReplacements/Object 1', b'not an image')])
shutil.copy(os.path.join(out, 'sample.odt'), os.path.join(out, 'odt-by-content'))
package('encrypted.odt', ODT, [('content.xml', b'\x8f\x00\x13 not XML'), ('META-INF/manifest.xml', manifest %
        '<manifest:file-entry manifest:full-path="content.xml"><manifest:encryption-data manifest:checksum="x"/></manifest:file-entry>')])
image = '<draw:image><office:binary-data>' + base64.encodebytes(red).decode() + '</office:binary-data></draw:image>'
with open(os.path.join(out, 'sample.fodt'), 'w') as f:
    f.write(head + '<office:document ' + NS + ' office:mimetype="' + ODT + '"><office:meta><dc:title>FODT Sample</dc:title>'
            '</office:meta>' + STYLES + AUTO + body(image) + '</office:document>')
shutil.copy(os.path.join(out, 'sample.fodt'), os.path.join(out, 'fodt-by-content'))

# Camera RAW: linear DNGs of 16-bit RGB, the left half red and the right
# blue (no colour matrix: LibRaw keeps the camera's RGB; neutral as shot),
# one turned by its Orientation (6: a quarter clockwise); a .nef that is none.
def dng(name, orientation):
    w, h = 64, 48
    img = b''.join(struct.pack('<3H', *((65535, 0, 0) if x < w // 2 else (0, 0, 65535))) for y in range(h) for x in range(w))
    SHORT, LONG, ASCII, RATIONAL, BYTE = 3, 4, 2, 5, 1
    def ifd(at):
        tags = [(254, LONG, [0]), (256, LONG, [w]), (257, LONG, [h]), (258, SHORT, [16] * 3), (259, SHORT, [1]),
                (262, SHORT, [34892]), (271, ASCII, b'View\0'), (272, ASCII, b'Tester\0'), (273, LONG, [at]),
                (274, SHORT, [orientation]), (277, SHORT, [3]), (278, LONG, [h]), (279, LONG, [len(img)]),
                (284, SHORT, [1]), (50706, BYTE, [1, 4, 0, 0]), (50708, ASCII, b'View Tester\0'),
                (50728, RATIONAL, [1, 1] * 3)]
        extra_at, entries, extra = 8 + 2 + 12 * len(tags) + 4, b'', b''
        for tag, kind, v in tags:
            raw = (v if kind == ASCII else bytes(v) if kind == BYTE else
                   struct.pack('<%d%s' % (len(v), 'H' if kind == SHORT else 'I'), *v))
            if len(raw) > 4:
                field = struct.pack('<I', extra_at + len(extra))
                extra += raw + b'\0' * (len(raw) % 2)
            else:
                field = raw.ljust(4, b'\0')
            entries += struct.pack('<HHI', tag, kind, len(v) // 2 if kind == RATIONAL else len(v)) + field
        return struct.pack('<H', len(tags)) + entries + b'\0\0\0\0' + extra
    with open(os.path.join(out, name), 'wb') as f:
        f.write(b'II*\0' + struct.pack('<I', 8) + ifd(8 + len(ifd(0))) + img)

dng('sample.dng', 1)
dng('turned.dng', 6)
with open(os.path.join(out, 'broken.nef'), 'wb') as f:
    f.write(b'not a raw image' * 64)

# CHM, as chmlib reads it, uncompressed (every file in content section 0,
# one PMGL directory chunk): pages in another order than their names' in
# the .hhc table of contents, one in GBK saying no encoding (LCID 0x0804),
# one saying windows-1252 (and not XML), one windows-1253 (which MuPDF
# does not know) the HTML5 way, a picture, a style sheet, a link, the home
# page; the same with a binary table of contents of another order; one by
# its content; a broken one.
import uuid

def encint(n):
    b = [n & 0x7F]
    n >>= 7
    while n:
        b.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(b))

def chm(name, files, lcid=0x0804):
    BLOCK = 4096
    data, dirents = b'', b''
    for p in sorted(files, key=str.lower):
        dirents += encint(len(p.encode())) + p.encode() + encint(0) + encint(len(data)) + encint(len(files[p]))
        data += files[p]
    pmgl = (b'PMGL' + struct.pack('<IIii', BLOCK - 0x14 - len(dirents), 0, -1, -1) + dirents).ljust(BLOCK, b'\0')
    itsp = (b'ITSP' + struct.pack('<iiiIiiiiiIiI', 1, 0x54, 10, BLOCK, 2, 1, -1, 0, 0, 1, -1, lcid) +
            uuid.UUID('5D02926A-212E-11D0-9DF9-00A0C922E6EC').bytes_le + b'\0' * 16)
    dir_at = 0x60 + 0x18
    data_at = dir_at + len(itsp) + len(pmgl)
    itsf = (b'ITSF' + struct.pack('<iiiII', 3, 0x60, 1, 0, lcid) +
            uuid.UUID('7C01FD10-7BAA-11D0-9E0C-00A0C922E6EC').bytes_le +
            uuid.UUID('7C01FD11-7BAA-11D0-9E0C-00A0C922E6EC').bytes_le +
            struct.pack('<QQQQQ', 0x60, 0x18, dir_at, len(itsp) + len(pmgl), data_at))
    with open(os.path.join(out, name), 'wb') as f:
        f.write(itsf + struct.pack('<IIQII', 0x1FE, 0, data_at + len(data), 0, 0) + itsp + pmgl + data)

hhc = b'''<HTML><BODY><OBJECT type="text/site properties"><param name="ImageType" value="Folder"></OBJECT>
<UL><LI><OBJECT type="text/sitemap"><param name="Name" value="Zeta Chapter"><param name="Local" value="zeta.htm"></OBJECT>
<UL><LI><OBJECT type="text/sitemap"><param name="Name" value="Section"><param name="Local" value="sub/sec.htm#part"></OBJECT></UL>
<LI><OBJECT type="text/sitemap"><param name="Name" value="Alpha &amp; One"><param name="Local" value="alpha.htm"></OBJECT>
<LI><OBJECT type="text/sitemap"><param name="Name" value="Heading only"></OBJECT>
<UL><LI><OBJECT type="text/sitemap"><param name="Name" value="Web"><param name="Local" value="https://example.org/"></OBJECT></UL>
</UL></BODY></HTML>'''
system = struct.pack('<I', 3) + b''.join(struct.pack('<HH', code, len(v)) + v for code, v in [
    (0, b'toc.hhc\0'), (2, b'index.htm\0'), (3, b'CHM Sample\0'), (4, struct.pack('<I', 0x0804) + b'\0' * 24),
    (6, b'sample\0')])
book = {
    '/#SYSTEM': system,
    '/toc.hhc': hhc,
    '/zeta.htm': b'<html><head><title>Zeta</title></head><body><p>zetaword \xd6\xd0\xce\xc4 here</p></body></html>',
    '/sub/sec.htm': (b'<html><head><meta http-equiv="Content-Type" content="text/html; charset=windows-1252"></head><body>'
                     b'<h2 id="part">secword</h2><p>caf\xe9 au lait</p><p><img src="../pics/red.png"></p></body></html>'),
    '/alpha.htm': (b'<html><head><link rel="stylesheet" type="text/css" href="style.css"></head><body><p>alphaword '
                   b'<span class="jay">bluejay</span> <a href="sub/sec.htm#part">golink</a></p></body></html>'),
    '/index.htm': b'<html><body><p>homeword</p></body></html>',
    '/extra.htm': b'<html><body><p>extraword</p></body></html>',
    '/greek.htm': b'<html><head><meta charset="windows-1253"></head><body><p>greekword \xe1\xe2\xe3</p></body></html>',
    '/pics/red.png': red,
    '/style.css': b'.jay { color: #0000ff; font-weight: bold }',
}
chm('sample.chm', book)
shutil.copy(os.path.join(out, 'sample.chm'), os.path.join(out, 'chm-by-content'))
# The binary table of contents: #TOPICS' entries name a title (#STRINGS)
# and an #URLTBL entry, whose #URLSTR entry holds the URL after 8 bytes;
# #TOCIDX holds the first entry's offset, then the entries (flags 8: a
# topic, 4: a book with children): Alpha, then Zeta, a book holding Sec.
strings, urlstr, urltbl, topics = b'\0', b'', b'', b''
for title, url in [('Alpha First', 'alpha.htm'), ('Zeta Second', 'zeta.htm'), ('Sec Child', 'sub/sec.htm#part')]:
    urltbl += struct.pack('<III', 0, len(topics) // 16, len(urlstr))
    topics += struct.pack('<IIIHH', 0, len(strings), len(urltbl) - 12, 0, 0)
    strings += title.encode() + b'\0'
    urlstr += b'\0' * 8 + url.encode() + b'\0'

def entry(flags, topic, after, child):
    return struct.pack('<7I', 0, flags, topic, 0, after, child, 0)

tocidx = struct.pack('<I', 16).ljust(16, b'\0') + entry(8, 0, 44, 0) + entry(12, 1, 0, 72) + entry(8, 2, 0, 0)
chm('btoc.chm', dict(book, **{'/#TOCIDX': tocidx, '/#TOPICS': topics, '/#URLTBL': urltbl, '/#URLSTR': urlstr,
                               '/#STRINGS': strings}))
with open(os.path.join(out, 'broken.chm'), 'wb') as f:
    f.write(b'ITSF' + b'\xff' * 200)
PY
cat >"$dir/test-view-formats.c" <<'EOF'
/* test-view-formats DIR: the formats above, opened by the real view-render
 * (as test-view-render does): each one renders, with its text, styles,
 * pictures, outline and links. PASS or FAIL lines; exits 1 on a FAIL. */
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "view-proto.h"

static const char *dir;
static int sock = -1, failures;

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

static struct view_reply call(struct view_request *q, void **data)
{
    struct view_reply r;
    int fd;
    if (data) *data = NULL;
    if (view_call(sock, q, &r, &fd) < 0) {
        check(0, "view_call of op %u: view-render has gone", q->op);
        exit(1);
    }
    if (fd >= 0) {
        void *p = view_map(fd, r.size);
        if (data && p) {
            *data = malloc(r.size + 1);
            memcpy(*data, p, r.size);
            ((char *)*data)[r.size] = '\0';
        }
        if (p) munmap(p, r.size);
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

static struct view_reply open_doc(const char *name, float em)
{
    struct view_request q = req(VIEW_OPEN, 0);
    snprintf(q.text, sizeof q.text, "%s/%s", dir, name);
    q.ax = 400, q.ay = 600, q.bx = em;
    return call(&q, NULL);
}

static uint32_t *render(int page, float zoom, struct view_reply *r)
{
    struct view_request q = req(VIEW_RENDER, page);
    void *px;
    q.zoom = zoom;
    *r = call(&q, &px);
    if (r->status) {
        free(px);
        return NULL;
    }
    return px;
}

/* Pixels of the colour (each channel within 60) in x0..x1, y0..y1. */
static int count(const uint32_t *px, int w, int h, int x0, int y0, int x1, int y1, uint32_t rgb)
{
    int n = 0;
    for (int y = y0 < 0 ? 0 : y0; px && y < y1 && y < h; y++) {
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < w; x++) {
            uint32_t p = px[(size_t)y * w + x];
            int ok = 1;
            for (int s = 0; s < 24; s += 8) ok &= abs((int)(p >> s & 0xFF) - (int)(rgb >> s & 0xFF)) <= 60;
            n += ok;
        }
    }
    return n;
}

/* The first hit of word in the document: its page (-1: none), its box. */
static int find(const char *word, int pages, struct view_rect *box)
{
    for (int i = 0; i < pages; i++) {
        struct view_request q = req(VIEW_SEARCH, i);
        void *data;
        snprintf(q.text, sizeof q.text, "%s", word);
        struct view_reply r = call(&q, &data);
        if (!r.status && r.count > 0 && data) {
            *box = *(struct view_rect *)data;
            free(data);
            return i;
        }
        free(data);
    }
    return -1;
}

static float height(const struct view_rect *b)
{
    return b->y1 - b->y0;
}

static void test_merged(const char *name, uint32_t left, uint32_t right)
{
    struct view_reply r = open_doc(name, 12);
    if (!check(r.status == 0 && !strncmp(r.text, "Image\t", 6) && r.pages == 1,
               "%s: OPEN is an image of one page (\"%s\", status %d, %d pages)", name, r.text, r.status, r.pages)) return;
    struct view_reply rr;
    uint32_t *px = render(0, 1, &rr);
    int w = rr.w, h = rr.h;
    check(px && count(px, w, h, w / 4 - 2, h / 2 - 2, w / 4 + 2, h / 2 + 2, left) == 16 &&
          count(px, w, h, 3 * w / 4 - 2, h / 2 - 2, 3 * w / 4 + 2, h / 2 + 2, right) == 16,
          "%s: RENDER shows the merged image (%d x %d), not a layer or the thumbnail", name, w, h);
    free(px);
}

static void test_odt(const char *name, const char *title, int outline)
{
    struct view_reply r = open_doc(name, 12);
    char want[64];
    snprintf(want, sizeof want, "OpenDocument text\t%s", title);
    if (!check(r.status == 0 && !strcmp(r.text, want) && r.pages >= 1 && (r.flags & VIEW_REFLOWABLE) && (r.flags & VIEW_HAS_TEXT),
               "%s: OPEN is \"%s\", reflowable with text (\"%s\", status %d, flags %#x)", name, want, r.text, r.status, r.flags)) return;
    int pages = r.pages;

    struct view_request q = req(VIEW_EXPORT, 0);
    char *text;
    struct view_reply e = call(&q, (void **)&text);
    static const char *const words[] = { "Chapter One", "bluejay", "pangolin", "mammoth", "listitemtwo", "cellbee", "footnotebody", "weblink" };
    int all = e.status == 0 && text;
    for (size_t i = 0; all && i < sizeof words / sizeof *words; i++) all = strstr(text, words[i]) != NULL;
    check(all, "%s: EXPORT has the headings, paragraphs, list, table and footnote", name);
    check(text && !strstr(text, "annotationsecret"), "%s: EXPORT has no annotation", name);
    free(text);

    if (outline) {
        q = req(VIEW_OUTLINE, 0);
        struct view_outline *o;
        struct view_reply ro = call(&q, (void **)&o);
        int one = 0, two = 0;
        for (int i = 0; ro.status == 0 && o && i < ro.count; i++) {
            one |= !strcmp(o[i].title, "Chapter One") && o[i].level == 0 && o[i].page == 0;
            two |= !strcmp(o[i].title, "Section Two") && o[i].level == 1;
        }
        check(one && two, "%s: OUTLINE has the two headings at their levels (%d items)", name, ro.count);
        free(o);
    }

    struct view_rect pangolin, mammoth, heading, bluejay, cell, link;
    int pp = find("pangolin", pages, &pangolin), pm = find("mammoth", pages, &mammoth), ph = find("Chapter", pages, &heading);
    if (check(pp >= 0 && pm >= 0 && ph >= 0, "%s: SEARCH finds the words", name)) {
        float big = height(&mammoth) / height(&pangolin), head = height(&heading) / height(&pangolin);
        check(big > 1.7f && big < 2.3f, "%s: a 24pt span is twice the 12pt text (%.2f)", name, big);
        check(head > 1.3f && head < 1.7f, "%s: a 150%% heading style is 1.5 times the text (%.2f)", name, head);
    }

    struct view_reply rr;
    int pb = find("bluejay", pages, &bluejay);
    uint32_t *px = pb >= 0 ? render(pb, 2, &rr) : NULL;
    int blue = px ? count(px, rr.w, rr.h, (int)(bluejay.x0 * 2), (int)(bluejay.y0 * 2), (int)(bluejay.x1 * 2), (int)(bluejay.y1 * 2), 0x0000FF) : 0;
    check(blue >= 20, "%s: the coloured span is blue (%d pixels)", name, blue);
    free(px);

    int pc = find("cellbee", pages, &cell);
    px = pc >= 0 ? render(pc, 1, &rr) : NULL;
    int green = px ? count(px, rr.w, rr.h, 0, 0, rr.w, rr.h, 0x00AA00) : 0;
    check(green >= 100, "%s: the table cells have their green borders (%d pixels)", name, green);
    free(px);

    int red = 0;
    for (int i = 0; i < pages && red < 2000; i++) {
        px = render(i, 1, &rr);
        red = px ? count(px, rr.w, rr.h, 0, 0, rr.w, rr.h, 0xFF0000) : 0;
        free(px);
    }
    check(red >= 2000 && red <= 2800, "%s: the picture is drawn at its frame size, 1 x 0.5 in (%d of 2592 pixels)", name, red);

    int pl = find("weblink", pages, &link), found = 0;
    q = req(VIEW_LINKS, pl < 0 ? 0 : pl);
    struct view_link *l;
    struct view_reply rl = call(&q, (void **)&l);
    for (int i = 0; rl.status == 0 && l && i < rl.count; i++) found |= l[i].page == -1 && !strcmp(l[i].uri, "https://example.org/");
    check(found, "%s: LINKS has the web link", name);
    free(l);

    /* The layout text size of 24 makes the text twice as large. */
    r = open_doc(name, 24);
    struct view_rect m2;
    if (check(r.status == 0 && find("mammoth", r.pages, &m2) >= 0, "%s: OPEN again with em 24", name)) {
        float k = height(&m2) / height(&mammoth);
        check(k > 1.8f && k < 2.2f, "%s: its sizes follow the layout text size (%.2f)", name, k);
    }
}

/* A RAW image: one page, LibRaw's pixels, red left and blue right (turned:
 * red on top and blue below, the page upright). */
static void test_raw(const char *name, int turned)
{
    struct view_reply r = open_doc(name, 12);
    if (!check(r.status == 0 && !strncmp(r.text, "Image\t", 6) && r.pages == 1,
               "%s: OPEN is an image of one page (\"%s\", status %d, %d pages)", name, r.text, r.status, r.pages)) return;
    struct view_reply rr;
    uint32_t *px = render(0, 1, &rr);
    int w = rr.w, h = rr.h;
    int red = turned ? count(px, w, h, w / 2 - 2, h / 4 - 2, w / 2 + 2, h / 4 + 2, 0xFF0000)
                     : count(px, w, h, w / 4 - 2, h / 2 - 2, w / 4 + 2, h / 2 + 2, 0xFF0000);
    int blue = turned ? count(px, w, h, w / 2 - 2, 3 * h / 4 - 2, w / 2 + 2, 3 * h / 4 + 2, 0x0000FF)
                      : count(px, w, h, 3 * w / 4 - 2, h / 2 - 2, 3 * w / 4 + 2, h / 2 + 2, 0x0000FF);
    check(px && (turned ? h > w : w > h) && red == 16 && blue == 16, "%s: RENDER shows the RAW image%s (%d x %d, %d red, %d blue)",
          name, turned ? " turned upright" : "", w, h, red, blue);
    free(px);
}

/* A CHM: its pages in its table of contents' order (binary: the binary
 * one's), then the home page and the others; the outline; for the .hhc
 * ones also their text in UTF-8 (a page in the language's code page, two
 * in the encodings they say), the link, the picture, the style sheet and
 * the reflow. */
static void test_chm(const char *name, int binary)
{
    struct view_reply r = open_doc(name, 12);
    if (!check(r.status == 0 && !strcmp(r.text, "CHM\tCHM Sample") && (r.flags & VIEW_REFLOWABLE) && (r.flags & VIEW_HAS_TEXT) &&
               (r.flags & VIEW_HAS_OUTLINE), "%s: OPEN is \"CHM\\tCHM Sample\", reflowable with text and an outline (\"%s\", status %d, flags %#x)",
               name, r.text, r.status, r.flags)) return;
    int pages = r.pages;
    struct view_rect alpha, box;
    int pz = find("zetaword", pages, &box), ps = find("secword", pages, &box), pa = find("alphaword", pages, &alpha);
    int ph = find("homeword", pages, &box), pe = find("extraword", pages, &box);
    int order = binary ? pa == 0 && pz > pa && ps > pz : pz == 0 && ps > pz && pa > ps;
    check(order && ph > (binary ? ps : pa) && pe > ph, "%s: the pages follow the %stable of contents, then the home page and the rest "
          "(zeta %d, sec %d, alpha %d, home %d, extra %d of %d)", name, binary ? "binary " : "", pz, ps, pa, ph, pe, pages);

    struct view_request q = req(VIEW_OUTLINE, 0);
    struct view_outline *o;
    struct view_reply ro = call(&q, (void **)&o);
    static const struct { const char *title; int level; } hhc[] = {
        { "Zeta Chapter", 0 }, { "Section", 1 }, { "Alpha & One", 0 }, { "Heading only", 0 }, { "Web", 1 },
    }, btoc[] = { { "Alpha First", 0 }, { "Zeta Second", 0 }, { "Sec Child", 1 } };
    int want = binary ? 3 : 5, ok = ro.status == 0 && o && ro.count == want;
    int hhc_pages[] = { pz, ps, pa, -1, -1 }, btoc_pages[] = { pa, pz, ps };
    for (int i = 0; ok && i < want; i++) {
        ok = !strcmp(o[i].title, binary ? btoc[i].title : hhc[i].title) && o[i].level == (binary ? btoc[i].level : hhc[i].level) &&
             o[i].page == (binary ? btoc_pages[i] : hhc_pages[i]);
    }
    check(ok, "%s: OUTLINE is the table of contents, at its levels, to the pages (%d items)", name, ro.count);
    free(o);
    if (binary) return;

    q = req(VIEW_EXPORT, 0);
    char *text;
    struct view_reply e = call(&q, (void **)&text);
    check(e.status == 0 && text && strstr(text, "zetaword \xE4\xB8\xAD\xE6\x96\x87") && strstr(text, "caf\xC3\xA9") &&
          strstr(text, "greekword \xCE\xB1\xCE\xB2\xCE\xB3"), "%s: EXPORT has the GBK, windows-1252 and windows-1253 pages in UTF-8", name);
    free(text);

    q = req(VIEW_LINKS, pa);
    struct view_link *l;
    struct view_reply rl = call(&q, (void **)&l);
    int found = 0;
    for (int i = 0; rl.status == 0 && l && i < rl.count; i++) found |= l[i].page == ps;
    check(found, "%s: LINKS has the link to the section's page %d", name, ps);
    free(l);

    struct view_reply rr;
    uint32_t *px = render(ps, 1, &rr);
    int red = px ? count(px, rr.w, rr.h, 0, 0, rr.w, rr.h, 0xFF0000) : 0;
    check(red >= 500, "%s: the picture of ../pics is drawn (%d red pixels)", name, red);
    free(px);
    struct view_rect jay;
    int pj = find("bluejay", pages, &jay);
    px = pj >= 0 ? render(pj, 2, &rr) : NULL;
    int blue = px ? count(px, rr.w, rr.h, (int)(jay.x0 * 2), (int)(jay.y0 * 2), (int)(jay.x1 * 2), (int)(jay.y1 * 2), 0x0000FF) : 0;
    check(blue >= 20, "%s: the style sheet makes the span blue (%d pixels)", name, blue);
    free(px);

    r = open_doc(name, 24);
    struct view_rect a2;
    if (check(r.status == 0 && find("alphaword", r.pages, &a2) >= 0, "%s: OPEN again with em 24", name)) {
        float k = height(&a2) / height(&alpha);
        check(k > 1.8f && k < 2.2f, "%s: it reflows, its text twice as large (%.2f)", name, k);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    dir = argv[1];
    signal(SIGPIPE, SIG_IGN);
    pid_t pid = view_spawn(&sock);
    if (!check(pid > 0, "view_spawn")) return 1;
    test_merged("sample.ora", 0x0000FF, 0x00FF00);
    test_merged("ora-by-content", 0x0000FF, 0x00FF00);
    test_merged("sample.kra", 0x00FF00, 0xFF0000);
    struct view_reply r = open_doc("nomerged.ora", 12);
    check(r.status != 0 && strstr(r.text, "mergedimage.png"), "nomerged.ora: OPEN fails: %s", r.text);
    test_odt("sample.odt", "ODT Sample", 1);
    test_odt("odt-by-content", "ODT Sample", 0);
    test_odt("sample.fodt", "FODT Sample", 1);
    test_odt("fodt-by-content", "FODT Sample", 0);
    r = open_doc("encrypted.odt", 12);
    check(r.status == ENOTSUP && strstr(r.text, "Encrypted"), "encrypted.odt: OPEN is ENOTSUP: %s", r.text);
    test_raw("sample.dng", 0);
    test_raw("turned.dng", 1);
    r = open_doc("broken.nef", 12);
    check(r.status != 0 && strstr(r.text, "RAW image"), "broken.nef: OPEN fails: %s", r.text);
    test_chm("sample.chm", 0);
    test_chm("chm-by-content", 0);
    test_chm("btoc.chm", 1);
    r = open_doc("broken.chm", 12);
    check(r.status != 0 && strstr(r.text, "CHM"), "broken.chm: OPEN fails: %s", r.text);
    close(sock);
    int status = -1;
    pid_t got;
    do got = waitpid(pid, &status, 0); while (got < 0 && errno == EINTR);
    check(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "view-render exits cleanly (wait %ld, status %#x)", (long)got, status);
    printf("%s: %d failed\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
EOF
chmod -R a+rX "$dir"
libs="$(pkg-config --cflags --libs libspectre libheif libjxl libwebp libwebpdemux libarchive ddjvuapi MagickWand libraw) -lmupdf -lchm -lm"
status=0
for build in normal sanitize; do
    flags="-O2 -Wall -Wextra -Werror -D_GNU_SOURCE"
    [ "$build" = normal ] || flags="$flags -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
    gcc $flags "$src/progs/view-render.c" "$src/progs/view-image.c" -o "$dir/view-render-$build" $libs
    # The test links view-image.c too, for ImageMagick's policy in it.
    gcc $flags -DVIEW_RENDER_TEST_PATH="\"$dir/view-render-$build\"" \
        "$src/progs/view-client.c" "$src/tools/test-view-render.c" "$src/progs/view-image.c" -o "$dir/test-$build" \
        $(pkg-config --cflags --libs MagickWand) -lm
    echo "== $build build"
    for how in exit signal; do
        rc=0
        "$dir/test-$build" --reap-failure "$how" >"$dir/reap-$build-$how.log" 2>&1 || rc=$?
        # The intentional failure stays in its own log: the process, not
        # just the log, must fail, with exactly the renderer-exit FAIL.
        if [ "$rc" -eq 1 ] && [ "$(grep -c '^FAIL view-render .* exits cleanly' "$dir/reap-$build-$how.log")" -eq 1 ] &&
           [ "$(grep -c '^FAIL' "$dir/reap-$build-$how.log")" -eq 1 ]; then
            echo "PASS renderer restart propagates a previous child's $how failure ($build)"
        else
            echo "FAIL renderer restart did not propagate exactly one $how failure ($build, status $rc)"
            status=1
        fi
    done
    "$dir/test-$build" "$dir/samples" "$dir/view-render-$build" || status=1
    # Trim Margins' content box, as Okular's imageBoundingBox finds it.
    gcc $flags -DVIEW_RENDER_TEST_PATH="\"$dir/view-render-$build\"" \
        "$src/progs/view-client.c" "$src/tools/test-view-bbox.c" -o "$dir/test-bbox-$build" -lm
    "$dir/test-bbox-$build" "$dir/bbox-$build" || status=1
    gcc $flags -I"$src/progs" -DVIEW_RENDER_TEST_PATH="\"$dir/view-render-$build\"" \
        "$src/progs/view-client.c" "$dir/test-view-formats.c" -o "$dir/test-formats-$build" -lm
    "$dir/test-formats-$build" "$dir/formats" || status=1
done
exit $status
