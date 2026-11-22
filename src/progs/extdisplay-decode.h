#ifndef LOCACHY_EXTDISPLAY_DECODE_H
#define LOCACHY_EXTDISPLAY_DECODE_H
#include "extdisplay-protocol.h"
#include "extdisplay-ipc.h"
struct extdisplay_decoder;
/* A callback borrows native-endian 0x00RRGGBB pixels until it returns. */
typedef int (*extdisplay_pixels_fn)(void *, const uint32_t *, uint32_t, uint32_t);
int extdisplay_vaapi_init(int render_fd, unsigned *hw_caps);
int extdisplay_decoder_available(enum extdisplay_codec codec);
struct extdisplay_decoder *extdisplay_decoder_open(enum extdisplay_codec codec, uint32_t width, uint32_t height);
int extdisplay_decoder_push(struct extdisplay_decoder *, const void *, size_t, extdisplay_pixels_fn, void *);
/* Split decode / deferred render directly into caller-provided RGB32 memory or dma-buf. */
int extdisplay_decoder_step(struct extdisplay_decoder *, const void *, size_t);
int extdisplay_decoder_size(const struct extdisplay_decoder *, uint32_t *width, uint32_t *height);
/* Export VPP RGB surface as 1..EXTDISPLAY_DMABUF_MAX_PLANES PRIME fds; returns 1 on success,
 * 0 if software render should be used. fds[] must hold EXTDISPLAY_DMABUF_MAX_PLANES entries. */
int extdisplay_decoder_export_dmabuf(struct extdisplay_decoder *, int *fds, struct extdisplay_dmabuf *info);
int extdisplay_decoder_reexport_dmabuf(struct extdisplay_decoder *, int *fds, struct extdisplay_dmabuf *info);
int extdisplay_decoder_render(struct extdisplay_decoder *, uint32_t *dst);
void extdisplay_decoder_disable_dmabuf(struct extdisplay_decoder *);
void extdisplay_decoder_close(struct extdisplay_decoder *);
#endif
