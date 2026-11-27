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
/* Pipelined 3-slot VA-API VPP + DRM PRIME export: prepare_dmabuf runs VPP into
 * slot (0..EXTDISPLAY_DMABUF_SLOTS-1) and syncs VPP completion (vaSyncSurface)
 * before returning so DRM never scans out an unfinished frame; export_slot
 * exports that slot's PRIME fds once so the frontend can cache the DRM FB. */
int extdisplay_decoder_prepare_dmabuf(struct extdisplay_decoder *, uint32_t slot);
int extdisplay_decoder_export_slot(struct extdisplay_decoder *, uint32_t slot, int *fds, struct extdisplay_dmabuf *info);
uint32_t extdisplay_decoder_dmabuf_gen(const struct extdisplay_decoder *);
/* Legacy single-step prepare+export and re-export helpers. */
int extdisplay_decoder_export_dmabuf(struct extdisplay_decoder *, int *fds, struct extdisplay_dmabuf *info);
int extdisplay_decoder_reexport_dmabuf(struct extdisplay_decoder *, int *fds, struct extdisplay_dmabuf *info);
int extdisplay_decoder_render(struct extdisplay_decoder *, uint32_t *dst);
void extdisplay_decoder_disable_dmabuf(struct extdisplay_decoder *);
void extdisplay_decoder_close(struct extdisplay_decoder *);
#endif
