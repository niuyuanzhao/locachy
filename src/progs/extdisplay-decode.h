#ifndef LOCACHY_EXTDISPLAY_DECODE_H
#define LOCACHY_EXTDISPLAY_DECODE_H
#include "extdisplay-protocol.h"
struct extdisplay_decoder;
/* A callback borrows native-endian 0x00RRGGBB pixels until it returns. */
typedef int (*extdisplay_pixels_fn)(void *, const uint32_t *, uint32_t, uint32_t);
int extdisplay_decoder_available(enum extdisplay_codec codec);
struct extdisplay_decoder *extdisplay_decoder_open(enum extdisplay_codec codec, uint32_t width, uint32_t height);
int extdisplay_decoder_push(struct extdisplay_decoder *, const void *, size_t, extdisplay_pixels_fn, void *);
void extdisplay_decoder_close(struct extdisplay_decoder *);
#endif
