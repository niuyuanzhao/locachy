/*
 * view-image: the rest of kimgio's types for view-render, read by
 * ImageMagick (MagickWand): ICO, CUR, MNG, PCX, SGI (image/x-rgb), TGA,
 * XBM, XCF, XPM, DDS, OpenEXR and Radiance HDR. The coder is named for each
 * file, from the content where that tells (magick_coder_content) or else the
 * name (magick_coder_name), and view-render's security policy lets
 * ImageMagick read with those coders only and run no delegate programs.
 * What it reads is made 8-bit sRGB, as Okular converts what kimgio reads.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <MagickWand/MagickWand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "view-image.h"

/* Read rights for the coders below and PNG (the images an ICO may hold),
 * none for any other, and no delegate programs. The last policy that
 * matches wins, so these come after the system's and the user's. */
static const char POLICY[] =
    "<policymap>"
    "<policy domain=\"delegate\" rights=\"none\" pattern=\"*\"/>"
    "<policy domain=\"coder\" rights=\"none\" pattern=\"*\"/>"
    "<policy domain=\"coder\" rights=\"read\" pattern=\"{CUR,DDS,EXR,HDR,ICO,MNG,PCX,PNG,SGI,TGA,XBM,XCF,XPM}\"/>"
    "</policymap>";

const char *magick_coder_content(const unsigned char *m, size_t n)
{
    if (n >= 8 && !memcmp(m, "\x8AMNG\r\n\x1A\n", 8)) return "MNG";
    if (n >= 9 && !memcmp(m, "gimp xcf ", 9)) return "XCF";
    if (n >= 9 && !memcmp(m, "/* XPM */", 9)) return "XPM";
    if (n >= 4 && m[0] == 0x01 && m[1] == 0xDA && m[2] <= 1 && m[3] >= 1 && m[3] <= 2) return "SGI";
    /* As kimgio's dds, exr and hdr plugins tell theirs (canRead). */
    if (n >= 4 && !memcmp(m, "DDS ", 4)) return "DDS";
    if (n >= 4 && !memcmp(m, "\x76\x2F\x31\x01", 4)) return "EXR";
    if ((n >= 11 && !memcmp(m, "#?RADIANCE\n", 11)) || (n >= 7 && !memcmp(m, "#?RGBE\n", 7))) return "HDR";
    return NULL;
}

const char *magick_coder_name(const char *name)
{
    static const char *const coders[][2] = {
        { "ico", "ICO" }, { "cur", "CUR" }, { "mng", "MNG" }, { "pcx", "PCX" }, { "sgi", "SGI" }, { "rgb", "SGI" },
        { "rgba", "SGI" }, { "bw", "SGI" }, { "tga", "TGA" }, { "icb", "TGA" }, { "vda", "TGA" }, { "vst", "TGA" },
        { "xbm", "XBM" }, { "xcf", "XCF" }, { "xpm", "XPM" }, { "dds", "DDS" }, { "exr", "EXR" }, { "hdr", "HDR" },
    };
    const char *slash = strrchr(name, '/'), *dot = strrchr(name, '.');
    const char *e = dot && (!slash || dot > slash) ? dot + 1 : "";
    for (size_t i = 0; i < sizeof coders / sizeof *coders; i++) if (!strcasecmp(e, coders[i][0])) return coders[i][1];
    return NULL;
}

int magick_start(void)
{
    static int state; /* 1 started, -1 without its policy */
    if (!state) {
        /* MagickWandGenesis sets no signal handlers: SIGUSR1 and SIGPIPE
         * stay view-render's. */
        MagickWandGenesis();
        ExceptionInfo *ex = AcquireExceptionInfo();
        state = SetMagickSecurityPolicy(POLICY, ex) == MagickTrue ? 1 : -1;
        DestroyExceptionInfo(ex);
        /* Its pixels in memory, never in a cache on the disk. */
        MagickSetResourceLimit(DiskResource, 0);
    }
    return state > 0;
}

static void wand_why(MagickWand *wand, char *why, size_t size)
{
    ExceptionType severity;
    char *d = MagickGetException(wand, &severity);
    if (d && *d) snprintf(why, size, "%s", d);
    MagickRelinquishMemory(d);
}

unsigned char *magick_rgba(const void *data, size_t n, const char *coder, uint64_t max_pixels, int *w, int *h,
                           char *why, size_t why_size)
{
    snprintf(why, why_size, "cannot read it");
    if (!magick_start()) {
        snprintf(why, why_size, "ImageMagick's security policy could not be set");
        return NULL;
    }
    /* Larger images are refused before their pixels are allocated. */
    MagickSetResourceLimit(AreaResource, max_pixels + 1);
    MagickWand *wand = NewMagickWand();
    unsigned char *px = NULL;
    char name[32];
    int xcf = !strcmp(coder, "XCF");
    /* "CODER:" in the name, or it goes by the content's magic bytes to
     * whichever coder claims them; [0]: the first image, as kimgio shows,
     * but of an XCF all its layers. */
    snprintf(name, sizeof name, xcf ? "%s:image" : "%s:image[0]", coder);
    if (!MagickSetFilename(wand, name) || !MagickReadImageBlob(wand, data, n)) {
        wand_why(wand, why, why_size);
        DestroyMagickWand(wand);
        return NULL;
    }
    MagickSetFirstIterator(wand);
    if (xcf) {
        /* kimgio shows an XCF's visible layers made one, each at its offset
         * over a transparent canvas, as GIMP does: ImageMagick gives them
         * bottom first, the hidden ones not composed. */
        PixelWand *none = NewPixelWand();
        PixelSetColor(none, "none");
        MagickSetImageBackgroundColor(wand, none);
        DestroyPixelWand(none);
        MagickWand *flat = MagickMergeImageLayers(wand, FlattenLayer);
        if (!flat) wand_why(wand, why, why_size);
        DestroyMagickWand(wand);
        if (!flat) return NULL;
        wand = flat;
    }
    size_t iw = MagickGetImageWidth(wand), ih = MagickGetImageHeight(wand);
    if (!iw || !ih || (uint64_t)iw * ih > max_pixels) {
        snprintf(why, why_size, "too large or no pixels");
        DestroyMagickWand(wand);
        return NULL;
    }
    /* OpenEXR's values are linear light, which ImageMagick's reader leaves
     * called sRGB (kimgio calls them linear sRGB); Radiance's were multiplied
     * by their EXPOSURE, which kimgio divides out. */
    MagickBooleanType ok = MagickTrue;
    if (!strcmp(coder, "EXR")) {
        ok = MagickSetImageColorspace(wand, MagickGetImageColorspace(wand) == GRAYColorspace ? LinearGRAYColorspace : RGBColorspace);
    } else if (!strcmp(coder, "HDR")) {
        char *e = MagickGetImageProperty(wand, "hdr:exposure");
        double exposure = e ? strtod(e, NULL) : 0;
        MagickRelinquishMemory(e);
        if (exposure > 0) ok = MagickEvaluateImage(wand, DivideEvaluateOperator, exposure);
    }
    /* Then sRGB, as Okular converts what kimgio reads to it. */
    if (ok) ok = MagickTransformImageColorspace(wand, sRGBColorspace);
    if (ok && !(px = malloc(iw * ih * 4))) {
        snprintf(why, why_size, "out of memory");
    } else if (!ok || !MagickExportImagePixels(wand, 0, 0, iw, ih, "RGBA", CharPixel, px)) {
        wand_why(wand, why, why_size);
        free(px);
        px = NULL;
    } else {
        *w = (int)iw;
        *h = (int)ih;
    }
    DestroyMagickWand(wand);
    return px;
}
