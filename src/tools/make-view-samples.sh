#!/bin/sh
# make-view-samples.sh OUTDIR: the sample documents view-render's test opens
# (see make-view-samples.c), and those made here with the tools of the
# libraries: JPEG 2000 (openjpeg) from a PPM; MNG, PCX, SGI, TGA, XBM and
# XPM (ImageMagick) from PNGs; DjVu (djvulibre) of two pages from PBMs,
# with a text layer, links, an outline and metadata; and, where TeX Live is
# installed, a DVI by TeX of one line of text.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=${1:?usage: make-view-samples.sh OUTDIR}
mkdir -p "$out"
gcc -O2 -Wall -Wextra -Werror -D_GNU_SOURCE "$src/tools/make-view-samples.c" -o "$out/make-view-samples" \
    $(pkg-config --cflags --libs libarchive libheif libjxl libwebp libtiff-4) -lmupdf -lgif
"$out/make-view-samples" "$out" /usr/share/fonts/TTF/DejaVuSans.ttf
cd "$out"
opj_compress -i sample.ppm -o sample.jp2 >/dev/null
magick -delay 100 gm-src.png gm-src2.png sample.mng
magick gm-src.png sample.pcx
magick gm-src.png SGI:sample.rgb
magick gm-src.png sample.tga
magick gm-src.png sample.xpm
magick gm-dark.png sample.xbm
magick gm-src.png comic-dir/sub/p12.tga
if command -v tex >/dev/null; then
    printf '\\nopagenumbers\\hsize=3in Hello DVI view\\bye\n' >sample-tex.tex
    tex -interaction=batchmode sample-tex.tex >/dev/null
    gzip -n -c sample-tex.dvi >sample-tex.dvi.gz
    rm -f sample-tex.tex sample-tex.log
fi
cjb2 -dpi 150 djvu-p1.pbm djvu-p1.djvu
cjb2 -dpi 150 djvu-p2.pbm djvu-p2.djvu
djvm -c sample.djvu djvu-p1.djvu djvu-p2.djvu
# Pixels from the bottom left, 150 dpi: 1275 x 1650 is Letter.
cat >djvu-txt1 <<'EOF'
(page 0 0 1275 1650
 (line 150 1400 700 1450 (word 150 1400 300 1450 "DjVu") (word 320 1400 500 1450 "marker") (word 520 1400 700 1450 "lynx"))
 (line 150 1300 600 1350 (word 150 1300 400 1350 "second") (word 420 1300 600 1350 "line")))
EOF
cat >djvu-txt2 <<'EOF'
(page 0 0 1275 1650 (line 150 1400 500 1450 (word 150 1400 500 1450 "lynx")))
EOF
cat >djvu-ant1 <<'EOF'
(maparea "#2" "to page two" (rect 150 1000 400 100))
(maparea "https://example.org/" "" (rect 150 800 400 100))
EOF
cat >djvu-outline <<'EOF'
(bookmarks ("Page One" "#1") ("Page Two" "#2" ("Sub" "#2")))
EOF
cat >djvu-meta <<'EOF'
title "DjVu Sample"
author "View Tester"
EOF
djvused sample.djvu -e 'select 1; set-txt djvu-txt1; set-ant djvu-ant1; select 2; set-txt djvu-txt2; select; set-outline djvu-outline; create-shared-ant; set-meta djvu-meta' -s
rm -f gm-src.png gm-src2.png gm-dark.png djvu-p1.djvu djvu-p2.djvu djvu-p1.pbm djvu-p2.pbm djvu-txt1 djvu-txt2 djvu-ant1 djvu-outline djvu-meta make-view-samples
chmod -R a+rX "$out"
