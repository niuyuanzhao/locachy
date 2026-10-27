/*
 * view-image.h: the images of kimgio's types that view-render has
 * ImageMagick read (view-image.c).
 */
#ifndef VIEW_IMAGE_H
#define VIEW_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/* The ImageMagick coder of an image: by its first n bytes, or else by its
 * name; NULL if it is none of these. */
const char *magick_coder_content(const unsigned char *m, size_t n);
const char *magick_coder_name(const char *name);

/* ImageMagick started with view-render's security policy: 1, or 0 if the
 * policy could not be set (then nothing is read). */
int magick_start(void);

/* The first image of data, read by coder (one of the above) as 8-bit sRGB
 * RGBA of *w x *h pixels, at most max_pixels: its pixels (free them), or
 * NULL with why said. */
unsigned char *magick_rgba(const void *data, size_t n, const char *coder, uint64_t max_pixels, int *w, int *h,
                           char *why, size_t why_size);

#endif
