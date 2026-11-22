/* Distribution FFmpeg only; optional VA-API hardware decode and VPP zero-copy
 * export over a pre-opened DRM render descriptor. */
#include "extdisplay-decode.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef CONFIG_EXTDISPLAY_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#ifdef CONFIG_EXTDISPLAY_VAAPI
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
static VADisplay va_disp;
static AVBufferRef *va_hwctx;
static int va_vpp_ok;
#endif
struct extdisplay_decoder {
    AVCodecContext *context;
    AVFrame *frame, *ready, *sw_frame;
    struct SwsContext *scale;
    uint32_t width, height;
    int has_ready, dmabuf_off;
    uint32_t *pixels;
#ifdef CONFIG_EXTDISPLAY_VAAPI
    VASurfaceID vpp_surf[3];
    VAConfigID vpp_cfg;
    VAContextID vpp_ctx;
    int vpp_ready, vpp_idx, vpp_has_last;
#endif
};
int extdisplay_vaapi_init(int render_fd, unsigned *hw_caps)
{
    if (hw_caps) *hw_caps = 0;
#ifdef CONFIG_EXTDISPLAY_VAAPI
    if (render_fd < 0) return -1;
    VADisplay vd = vaGetDisplayDRM(render_fd);
    int maj = 0, min = 0;
    if (!vd || vaInitialize(vd, &maj, &min) != VA_STATUS_SUCCESS) return -1;
    int maxp = vaMaxNumProfiles(vd);
    if (maxp <= 0 || maxp > 256) maxp = 256;
    VAProfile *profs = calloc((size_t)maxp, sizeof *profs);
    VAEntrypoint *eps = calloc(32, sizeof *eps);
    int np = 0, has_vpp = 0;
    unsigned caps = 0;
    if (profs && eps && vaQueryConfigProfiles(vd, profs, &np) == VA_STATUS_SUCCESS) {
        int maxep = vaMaxNumEntrypoints(vd);
        if (maxep > 32) maxep = 32;
        for (int i = 0; i < np; i++) {
            int nep = 0;
            if (vaQueryConfigEntrypoints(vd, profs[i], eps, &nep) != VA_STATUS_SUCCESS) continue;
            for (int k = 0; k < nep; k++) {
                if (profs[i] == VAProfileNone && eps[k] == VAEntrypointVideoProc) has_vpp = 1;
                if (eps[k] != VAEntrypointVLD) continue;
                if (profs[i] == VAProfileH264High || profs[i] == VAProfileH264Main) caps |= 1u;
                else if (profs[i] == VAProfileHEVCMain) caps |= 2u;
                else if (profs[i] == VAProfileHEVCMain444) caps |= 4u;
            }
        }
    }
    free(profs);
    free(eps);
    if (!has_vpp || !caps) {
        vaTerminate(vd);
        return -1;
    }
    AVBufferRef *hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
    if (!hw) {
        vaTerminate(vd);
        return -1;
    }
    ((AVVAAPIDeviceContext *)((AVHWDeviceContext *)hw->data)->hwctx)->display = vd;
    if (av_hwdevice_ctx_init(hw) < 0) {
        av_buffer_unref(&hw);
        vaTerminate(vd);
        return -1;
    }
    va_disp = vd;
    va_hwctx = hw;
    va_vpp_ok = 1;
    if (hw_caps) *hw_caps = caps;
    return 0;
#else
    (void)render_fd;
    return -1;
#endif
}
static const AVCodec *codec_for(enum extdisplay_codec codec)
{
    return avcodec_find_decoder(codec == EXTDISPLAY_CODEC_H264 ? AV_CODEC_ID_H264 :
                                codec == EXTDISPLAY_CODEC_HEVC ? AV_CODEC_ID_HEVC : AV_CODEC_ID_NONE);
}
#ifdef CONFIG_EXTDISPLAY_VAAPI
static enum AVPixelFormat pick_vaapi_or_sw(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
    (void)ctx;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI) return *p;
    }
    return fmts[0];
}
#endif
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O3")))
#endif
static void yuv444p_to_rgb32(const AVFrame *f, uint32_t * restrict dst, int w, int h)
{
    const int full = (f->color_range == AVCOL_RANGE_JPEG);
    const int bt709 = (f->colorspace == AVCOL_SPC_BT709);
    const int y_off = full ? 0 : 16;
    const int y_mul = full ? 256 : 298;
    const int rv = bt709 ? (full ? 403 : 459) : (full ? 359 : 409);
    const int gu = bt709 ? (full ? -48 : -55) : (full ? -88 : -100);
    const int gv = bt709 ? (full ? -120 : -136) : (full ? -183 : -208);
    const int bu = bt709 ? (full ? 475 : 541) : (full ? 454 : 516);
    const uint8_t * restrict ybase = f->data[0];
    const uint8_t * restrict ubase = f->data[1];
    const uint8_t * restrict vbase = f->data[2];
    const int lsy = f->linesize[0], lsu = f->linesize[1], lsv = f->linesize[2];
    for (int y = 0; y < h; y++) {
        const uint8_t * restrict yp = ybase + y * lsy;
        const uint8_t * restrict up = ubase + y * lsu;
        const uint8_t * restrict vp = vbase + y * lsv;
        uint32_t * restrict dr = dst + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int Y = ((int)yp[x] - y_off) * y_mul + 128;
            int U = (int)up[x] - 128;
            int V = (int)vp[x] - 128;
            int r = (Y + rv * V) >> 8;
            int g = (Y + gu * U + gv * V) >> 8;
            int b = (Y + bu * U) >> 8;
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            dr[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}
static int validate_frame(const AVFrame *f, const struct extdisplay_decoder *d)
{
    return (f->width != (int)d->width || f->height != (int)d->height ||
            extdisplay_dimensions((uint32_t)f->width, (uint32_t)f->height, NULL) < 0 ||
            f->crop_top || f->crop_bottom || f->crop_left || f->crop_right) ? -1 : 0;
}
static int render_frame(struct extdisplay_decoder *d, const AVFrame *f, uint32_t * restrict dst)
{
    if (f->format == AV_PIX_FMT_YUV444P || f->format == AV_PIX_FMT_YUVJ444P) {
        yuv444p_to_rgb32(f, dst, f->width, f->height);
        return 0;
    }
    d->scale = sws_getCachedContext(d->scale, f->width, f->height, f->format,
                    f->width, f->height, AV_PIX_FMT_RGB32, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!d->scale) return -1;
    /* The stream's own matrix and range (its VUI), not swscale's guess. */
    const int *coefficients = sws_getCoefficients(f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
    sws_setColorspaceDetails(d->scale, coefficients, f->color_range == AVCOL_RANGE_JPEG,
                             coefficients, 1, 0, 1 << 16, 1 << 16);
    uint8_t *dest[4] = { (uint8_t *)dst, NULL, NULL, NULL };
    int strides[4] = { (int)d->width * 4, 0, 0, 0 };
    if (sws_scale(d->scale, (const uint8_t *const *)f->data, f->linesize, 0,
                  f->height, dest, strides) != f->height) return -1;
    for (size_t i = 0, end = (size_t)d->width * d->height; i < end; i++) dst[i] &= 0x00ffffff;
    return 0;
}
int extdisplay_decoder_available(enum extdisplay_codec codec) { return codec_for(codec) != NULL; }
void extdisplay_decoder_disable_dmabuf(struct extdisplay_decoder *d)
{
    if (d) d->dmabuf_off = 1;
}
void extdisplay_decoder_close(struct extdisplay_decoder *d)
{
    if (!d) return;
#ifdef CONFIG_EXTDISPLAY_VAAPI
    if (d->vpp_ready && va_disp) {
        if (d->vpp_ctx != VA_INVALID_ID) vaDestroyContext(va_disp, d->vpp_ctx);
        if (d->vpp_cfg != VA_INVALID_ID) vaDestroyConfig(va_disp, d->vpp_cfg);
        for (int i = 0; i < 3; i++) {
            if (d->vpp_surf[i] != VA_INVALID_ID) vaDestroySurfaces(va_disp, &d->vpp_surf[i], 1);
        }
    }
#endif
    sws_freeContext(d->scale);
    av_frame_free(&d->sw_frame);
    av_frame_free(&d->ready);
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
#ifdef CONFIG_EXTDISPLAY_VAAPI
    d->vpp_surf[0] = d->vpp_surf[1] = d->vpp_surf[2] = VA_INVALID_ID;
    d->vpp_cfg = VA_INVALID_ID;
    d->vpp_ctx = VA_INVALID_ID;
#endif
    d->context = avcodec_alloc_context3(c);
    d->frame = av_frame_alloc();
    d->ready = av_frame_alloc();
    d->width = w; d->height = h;
    if (!d->context || !d->frame || !d->ready) goto fail;
    av_log_set_level(AV_LOG_QUIET);
    d->context->width = (int)w; d->context->height = (int)h;
    d->context->max_pixels = EXTDISPLAY_PIXELS_MAX;
    d->context->thread_count = 1;
    d->context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d->context->err_recognition = AV_EF_CAREFUL | AV_EF_EXPLODE;
#ifdef CONFIG_EXTDISPLAY_VAAPI
    if (va_hwctx) {
        d->context->hw_device_ctx = av_buffer_ref(va_hwctx);
        if (!d->context->hw_device_ctx) goto fail;
        d->context->get_format = pick_vaapi_or_sw;
    }
#endif
    if (avcodec_open2(d->context, c, NULL) < 0) goto fail;
    return d;
fail:
    extdisplay_decoder_close(d); errno = EINVAL; return NULL;
}
static int send_packet(struct extdisplay_decoder *d, const void *data, size_t n)
{
    if (!d || extdisplay_annex_b(data, n) < 0) return -1;
    AVPacket *packet = av_packet_alloc();
    if (!packet) return -1;
    /* av_new_packet supplies the padding required by FFmpeg's bit reader. */
    int r = av_new_packet(packet, (int)n);
    if (r >= 0) {
        memcpy(packet->data, data, n);
        r = avcodec_send_packet(d->context, packet);
    }
    av_packet_free(&packet);
    return r < 0 ? -1 : 0;
}
int extdisplay_decoder_step(struct extdisplay_decoder *d, const void *data, size_t n)
{
    if (send_packet(d, data, n) < 0) return -1;
    int r, got = 0;
    while ((r = avcodec_receive_frame(d->context, d->frame)) >= 0) {
        if (validate_frame(d->frame, d) < 0) return -1;
        av_frame_unref(d->ready);
        av_frame_move_ref(d->ready, d->frame);
        d->has_ready = 1;
        got = 1;
    }
    return r == AVERROR(EAGAIN) || r == AVERROR(EOF) ? got : -1;
}
int extdisplay_decoder_size(const struct extdisplay_decoder *d, uint32_t *w, uint32_t *h)
{
    if (!d || !d->has_ready) return 0;
    if (w) *w = d->width;
    if (h) *h = d->height;
    return 1;
}
#ifdef CONFIG_EXTDISPLAY_VAAPI
static int export_surface_prime(VASurfaceID dst, int *fds, struct extdisplay_dmabuf *info)
{
    VADRMPRIMESurfaceDescriptor prime;
    memset(&prime, 0, sizeof prime);
    if (vaExportSurfaceHandle(va_disp, dst, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &prime) != VA_STATUS_SUCCESS) {
        return 0;
    }
    int valid = prime.num_objects >= 1 && prime.num_objects <= EXTDISPLAY_DMABUF_MAX_PLANES &&
                prime.num_layers == 1 &&
                prime.layers[0].num_planes >= 1 && prime.layers[0].num_planes <= EXTDISPLAY_DMABUF_MAX_PLANES;
    for (uint32_t i = 0; valid && i < prime.num_objects; i++) {
        if (prime.objects[i].fd < 0) valid = 0;
    }
    for (uint32_t i = 0; valid && i < prime.layers[0].num_planes; i++) {
        if (prime.layers[0].object_index[i] >= prime.num_objects) valid = 0;
    }
    if (!valid) {
        for (uint32_t i = 0; i < prime.num_objects && i < 4; i++) {
            if (prime.objects[i].fd >= 0) close(prime.objects[i].fd);
        }
        return 0;
    }
    memset(info, 0, sizeof *info);
    info->fourcc = prime.layers[0].drm_format;
    info->num_planes = prime.layers[0].num_planes;
    info->num_objects = prime.num_objects;
    for (uint32_t i = 0; i < EXTDISPLAY_DMABUF_MAX_PLANES; i++) fds[i] = -1;
    for (uint32_t i = 0; i < prime.num_objects; i++) fds[i] = prime.objects[i].fd;
    for (uint32_t i = 0; i < prime.layers[0].num_planes; i++) {
        uint32_t oi = prime.layers[0].object_index[i];
        info->object_index[i] = oi;
        info->pitches[i] = prime.layers[0].pitch[i];
        info->offsets[i] = prime.layers[0].offset[i];
        info->modifiers[i] = prime.objects[oi].drm_format_modifier;
    }
    return 1;
}
#endif

int extdisplay_decoder_export_dmabuf(struct extdisplay_decoder *d, int *fds, struct extdisplay_dmabuf *info)
{
#ifdef CONFIG_EXTDISPLAY_VAAPI
    if (!d || !d->has_ready || d->dmabuf_off || !va_vpp_ok || !va_disp ||
        d->ready->format != AV_PIX_FMT_VAAPI) {
        return 0;
    }
    if (!d->vpp_ready) {
        VASurfaceAttrib attrs[2] = {
            { .type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
              .value = { .type = VAGenericValueTypeInteger, .value = { .i = VA_FOURCC_BGRX } } },
            { .type = VASurfaceAttribUsageHint, .flags = VA_SURFACE_ATTRIB_SETTABLE,
              .value = { .type = VAGenericValueTypeInteger,
                         .value = { .i = VA_SURFACE_ATTRIB_USAGE_HINT_VPP_WRITE |
                                         VA_SURFACE_ATTRIB_USAGE_HINT_DISPLAY |
                                         VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT } } },
        };
        if (vaCreateSurfaces(va_disp, VA_RT_FORMAT_RGB32, d->width, d->height,
                             d->vpp_surf, 3, attrs, 2) != VA_STATUS_SUCCESS ||
            vaCreateConfig(va_disp, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &d->vpp_cfg) != VA_STATUS_SUCCESS ||
            vaCreateContext(va_disp, d->vpp_cfg, (int)d->width, (int)d->height, VA_PROGRESSIVE,
                            d->vpp_surf, 3, &d->vpp_ctx) != VA_STATUS_SUCCESS) {
            d->dmabuf_off = 1;
            return 0;
        }
        d->vpp_ready = 1;
    }
    VASurfaceID src = (VASurfaceID)(uintptr_t)d->ready->data[3];
    VASurfaceID dst = d->vpp_surf[d->vpp_idx];
    VAProcColorStandardType cs = d->ready->colorspace == AVCOL_SPC_BT709
        ? VAProcColorStandardBT709 : VAProcColorStandardBT601;
    VAProcPipelineParameterBuffer param = {
        .surface = src,
        .surface_color_standard = cs,
        .output_color_standard = cs,
        .input_color_properties = {
            .color_range = d->ready->color_range == AVCOL_RANGE_JPEG ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED,
        },
        .output_color_properties = {
            .color_range = VA_SOURCE_RANGE_FULL,
        },
    };
    VABufferID vbuf = VA_INVALID_ID;
    if (vaCreateBuffer(va_disp, d->vpp_ctx, VAProcPipelineParameterBufferType, sizeof param, 1, &param, &vbuf) != VA_STATUS_SUCCESS) {
        d->dmabuf_off = 1;
        return 0;
    }
    VAStatus st = vaBeginPicture(va_disp, d->vpp_ctx, dst);
    if (st == VA_STATUS_SUCCESS) st = vaRenderPicture(va_disp, d->vpp_ctx, &vbuf, 1);
    if (st == VA_STATUS_SUCCESS) st = vaEndPicture(va_disp, d->vpp_ctx);
    vaDestroyBuffer(va_disp, vbuf);
    if (st != VA_STATUS_SUCCESS || vaSyncSurface(va_disp, dst) != VA_STATUS_SUCCESS) {
        d->dmabuf_off = 1;
        return 0;
    }
    if (!export_surface_prime(dst, fds, info)) {
        d->dmabuf_off = 1;
        return 0;
    }
    d->vpp_idx = (d->vpp_idx + 1) % 3;
    d->vpp_has_last = 1;
    av_frame_unref(d->ready);
    d->has_ready = 0;
    return 1;
#else
    (void)d; (void)fds; (void)info;
    return 0;
#endif
}
int extdisplay_decoder_reexport_dmabuf(struct extdisplay_decoder *d, int *fds, struct extdisplay_dmabuf *info)
{
#ifdef CONFIG_EXTDISPLAY_VAAPI
    if (!d || d->dmabuf_off || !va_vpp_ok || !va_disp || !d->vpp_ready || !d->vpp_has_last) return 0;
    VASurfaceID dst = d->vpp_surf[(d->vpp_idx + 2) % 3];
    return export_surface_prime(dst, fds, info);
#else
    (void)d; (void)fds; (void)info;
    return 0;
#endif
}
int extdisplay_decoder_render(struct extdisplay_decoder *d, uint32_t *dst)
{
    if (!d || !d->has_ready || !dst) return -1;
    const AVFrame *src = d->ready;
#ifdef CONFIG_EXTDISPLAY_VAAPI
    if (d->ready->format == AV_PIX_FMT_VAAPI) {
        if (!d->sw_frame && !(d->sw_frame = av_frame_alloc())) return -1;
        av_frame_unref(d->sw_frame);
        if (av_hwframe_transfer_data(d->sw_frame, d->ready, 0) < 0) return -1;
        d->sw_frame->colorspace = d->ready->colorspace;
        d->sw_frame->color_range = d->ready->color_range;
        src = d->sw_frame;
    }
#endif
    int r = render_frame(d, src, dst);
    av_frame_unref(d->ready);
    if (d->sw_frame) av_frame_unref(d->sw_frame);
    d->has_ready = 0;
    return r;
}
int extdisplay_decoder_push(struct extdisplay_decoder *d, const void *data, size_t n,
                     extdisplay_pixels_fn receive, void *opaque)
{
    if (!d || !receive) return -1;
    if (!d->pixels) {
        size_t bytes;
        if (extdisplay_dimensions(d->width, d->height, &bytes) < 0 || !(d->pixels = malloc(bytes))) return -1;
    }
    if (send_packet(d, data, n) < 0) return -1;
    int r;
    while ((r = avcodec_receive_frame(d->context, d->frame)) >= 0) {
        if (validate_frame(d->frame, d) < 0 || render_frame(d, d->frame, d->pixels) < 0) return -1;
        if (receive(opaque, d->pixels, d->width, d->height) < 0) return -1;
        av_frame_unref(d->frame);
    }
    return r == AVERROR(EAGAIN) || r == AVERROR(EOF) ? 0 : -1;
}
#else
struct extdisplay_decoder { int unavailable; };
int extdisplay_vaapi_init(int render_fd, unsigned *hw_caps) { (void)render_fd; if (hw_caps) *hw_caps = 0; return -1; }
int extdisplay_decoder_available(enum extdisplay_codec c) { (void)c; return 0; }
struct extdisplay_decoder *extdisplay_decoder_open(enum extdisplay_codec c, uint32_t w, uint32_t h)
{ (void)c; (void)w; (void)h; errno = ENOTSUP; return NULL; }
int extdisplay_decoder_push(struct extdisplay_decoder *d, const void *p, size_t n, extdisplay_pixels_fn fn, void *o)
{ (void)d; (void)p; (void)n; (void)fn; (void)o; errno = ENOTSUP; return -1; }
int extdisplay_decoder_step(struct extdisplay_decoder *d, const void *p, size_t n)
{ (void)d; (void)p; (void)n; errno = ENOTSUP; return -1; }
int extdisplay_decoder_size(const struct extdisplay_decoder *d, uint32_t *w, uint32_t *h)
{ (void)d; (void)w; (void)h; return 0; }
int extdisplay_decoder_export_dmabuf(struct extdisplay_decoder *d, int *fds, struct extdisplay_dmabuf *info)
{ (void)d; (void)fds; (void)info; return 0; }
int extdisplay_decoder_reexport_dmabuf(struct extdisplay_decoder *d, int *fds, struct extdisplay_dmabuf *info)
{ (void)d; (void)fds; (void)info; return 0; }
int extdisplay_decoder_render(struct extdisplay_decoder *d, uint32_t *dst)
{ (void)d; (void)dst; errno = ENOTSUP; return -1; }
void extdisplay_decoder_disable_dmabuf(struct extdisplay_decoder *d) { (void)d; }
void extdisplay_decoder_close(struct extdisplay_decoder *d) { (void)d; }
#endif
