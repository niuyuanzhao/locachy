/* Distribution FFmpeg only. No hardware decoder or codec implementation here. */
#include "extdisplay-decode.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef CONFIG_EXTDISPLAY_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
struct extdisplay_decoder {
    AVCodecContext *context;
    AVFrame *frame;
    struct SwsContext *scale;
    uint32_t width, height;
    uint32_t *pixels;
};
static const AVCodec *codec_for(enum extdisplay_codec codec)
{
    return avcodec_find_decoder(codec == EXTDISPLAY_CODEC_H264 ? AV_CODEC_ID_H264 :
                                codec == EXTDISPLAY_CODEC_HEVC ? AV_CODEC_ID_HEVC : AV_CODEC_ID_NONE);
}
int extdisplay_decoder_available(enum extdisplay_codec codec) { return codec_for(codec) != NULL; }
void extdisplay_decoder_close(struct extdisplay_decoder *d)
{
    if (!d) return;
    sws_freeContext(d->scale);
    av_frame_free(&d->frame);
    avcodec_free_context(&d->context);
    free(d->pixels); free(d);
}
struct extdisplay_decoder *extdisplay_decoder_open(enum extdisplay_codec codec, uint32_t w, uint32_t h)
{
    size_t bytes;
    const AVCodec *c = codec_for(codec);
    if (!c || extdisplay_dimensions(w, h, &bytes) < 0) { errno = ENOTSUP; return NULL; }
    struct extdisplay_decoder *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->context = avcodec_alloc_context3(c);
    d->frame = av_frame_alloc();
    d->pixels = malloc(bytes);
    d->width = w; d->height = h;
    if (!d->context || !d->frame || !d->pixels) goto fail;
    av_log_set_level(AV_LOG_QUIET);
    d->context->width = (int)w; d->context->height = (int)h;
    d->context->max_pixels = EXTDISPLAY_PIXELS_MAX;
    d->context->thread_count = 1;
    d->context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d->context->err_recognition = AV_EF_CAREFUL | AV_EF_EXPLODE;
    if (avcodec_open2(d->context, c, NULL) < 0) goto fail;
    return d;
fail:
    extdisplay_decoder_close(d); errno = EINVAL; return NULL;
}
int extdisplay_decoder_push(struct extdisplay_decoder *d, const void *data, size_t n,
                     extdisplay_pixels_fn receive, void *opaque)
{
    if (!d || extdisplay_annex_b(data, n) < 0 || !receive) return -1;
    AVPacket *packet = av_packet_alloc();
    if (!packet) return -1;
    /* av_new_packet supplies the padding required by FFmpeg's bit reader. */
    int r = av_new_packet(packet, (int)n);
    if (r >= 0) {
        memcpy(packet->data, data, n);
        r = avcodec_send_packet(d->context, packet);
    }
    av_packet_free(&packet);
    if (r < 0) return -1;
    while ((r = avcodec_receive_frame(d->context, d->frame)) >= 0) {
        AVFrame *f = d->frame;
        if (f->width != (int)d->width || f->height != (int)d->height ||
            extdisplay_dimensions((uint32_t)f->width, (uint32_t)f->height, NULL) < 0 ||
            f->crop_top || f->crop_bottom || f->crop_left || f->crop_right) return -1;
        d->scale = sws_getCachedContext(d->scale, f->width, f->height, f->format,
                        f->width, f->height, AV_PIX_FMT_RGB32, SWS_BILINEAR, NULL, NULL, NULL);
        if (!d->scale) return -1;
        uint8_t *dest[4] = { (uint8_t *)d->pixels, NULL, NULL, NULL };
        int strides[4] = { (int)d->width * 4, 0, 0, 0 };
        if (sws_scale(d->scale, (const uint8_t *const *)f->data, f->linesize, 0,
                      f->height, dest, strides) != f->height) return -1;
        for (size_t i = 0, end = (size_t)d->width * d->height; i < end; i++) d->pixels[i] &= 0x00ffffff;
        if (receive(opaque, d->pixels, d->width, d->height) < 0) return -1;
        av_frame_unref(f);
    }
    return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : -1;
}
#else
struct extdisplay_decoder { int unavailable; };
int extdisplay_decoder_available(enum extdisplay_codec c) { (void)c; return 0; }
struct extdisplay_decoder *extdisplay_decoder_open(enum extdisplay_codec c, uint32_t w, uint32_t h)
{ (void)c; (void)w; (void)h; errno = ENOTSUP; return NULL; }
int extdisplay_decoder_push(struct extdisplay_decoder *d, const void *p, size_t n, extdisplay_pixels_fn fn, void *o)
{ (void)d; (void)p; (void)n; (void)fn; (void)o; errno = ENOTSUP; return -1; }
void extdisplay_decoder_close(struct extdisplay_decoder *d) { (void)d; }
#endif
