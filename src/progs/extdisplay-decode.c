/* Distribution FFmpeg only; optional VA-API hardware decode and VPP zero-copy
 * export over a pre-opened DRM render descriptor. */
#include "extdisplay-decode.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef CONFIG_EXTDISPLAY_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#ifdef CONFIG_EXTDISPLAY_VAAPI
#include <drm/drm_fourcc.h>
#ifdef CONFIG_EXTDISPLAY_GBM
#include <gbm.h>
#endif
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
static VADisplay va_disp;
static AVBufferRef *va_hwctx;
static int va_vpp_ok;
#ifdef CONFIG_EXTDISPLAY_GBM
static struct gbm_device *va_gbm;
#endif
static uint32_t vpp_gen_seq;
#endif
struct extdisplay_decoder {
    AVCodecContext *context;
    AVFrame *frame, *ready, *sw_frame;
    struct SwsContext *scale;
    uint32_t width, height;
    int has_ready, dmabuf_off;
    uint32_t *pixels;
#ifdef CONFIG_EXTDISPLAY_VAAPI
    VASurfaceID vpp_surf[EXTDISPLAY_DMABUF_SLOTS];
    /* Non-negative when VPP destination slots are backed by driver-agnostic
     * scanout BOs allocated through libgbm and imported via
     * VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2. Keeping the PRIME fd open
     * preserves the BO for re-export (e.g. VT/lease restore) after
     * gbm_bo_destroy(). */
    int vpp_prime_fd[EXTDISPLAY_DMABUF_SLOTS];
    uint32_t vpp_pitch[EXTDISPLAY_DMABUF_SLOTS];
    uint32_t vpp_offset[EXTDISPLAY_DMABUF_SLOTS];
    uint64_t vpp_modifier[EXTDISPLAY_DMABUF_SLOTS];
    int vpp_using_scanout_bo;
    AVFrame *vpp_src[EXTDISPLAY_DMABUF_SLOTS]; /* holds DPB decode surface ref until slot is reused */
    VAConfigID vpp_cfg;
    VAContextID vpp_ctx;
    uint32_t vpp_gen;
    int vpp_ready, vpp_idx, vpp_last_slot, vpp_has_last;
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
#ifdef CONFIG_EXTDISPLAY_GBM
    /* Initialize libgbm before the worker enters Landlock + seccomp so Mesa can
     * load its driver backend once; subsequent gbm_bo_create() calls run inside
     * the sandbox using only DRM ioctls on render_fd. */
    va_gbm = gbm_create_device(render_fd);
#endif
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
static int validate_frame(const AVFrame *f, const struct extdisplay_decoder *d)
{
    return (f->width != (int)d->width || f->height != (int)d->height ||
            extdisplay_dimensions((uint32_t)f->width, (uint32_t)f->height, NULL) < 0 ||
            f->crop_top || f->crop_bottom || f->crop_left || f->crop_right) ? -1 : 0;
}
static int render_frame(struct extdisplay_decoder *d, const AVFrame *f, uint32_t * restrict dst)
{
    /* Same-size format conversion through FFmpeg libswscale with full horizontal
     * chroma interpolation and accurate rounding (avoids SWS_FAST_BILINEAR's
     * biased horizontal step and chroma shift on 1:1 desktop frames). */
    d->scale = sws_getCachedContext(d->scale, f->width, f->height, f->format,
                    f->width, f->height, AV_PIX_FMT_RGB32,
                    SWS_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND, NULL, NULL, NULL);
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
#ifdef EXTDISPLAY_DECODE_TEST
/* Test hook only (tools/test-extdisplay-dmabuf.c): exercises render_frame()
 * directly on an AV_PIX_FMT_VUYX frame through FFmpeg libswscale. */
int extdisplay_render_vuyx_for_test(const uint8_t *base, int linesize, int full, int bt709,
                                    uint32_t *dst, int w, int h)
{
#if LIBAVUTIL_VERSION_MAJOR >= 58
    struct extdisplay_decoder d;
    memset(&d, 0, sizeof d);
    d.width = (uint32_t)w;
    d.height = (uint32_t)h;
    AVFrame f;
    memset(&f, 0, sizeof f);
    f.width = w;
    f.height = h;
    f.format = AV_PIX_FMT_VUYX;
    f.color_range = full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    f.colorspace = bt709 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    f.data[0] = (uint8_t *)base;
    f.linesize[0] = linesize;
    int r = render_frame(&d, &f, dst);
    sws_freeContext(d.scale);
    return r;
#else
    (void)base; (void)linesize; (void)full; (void)bt709; (void)dst; (void)w; (void)h;
    return -1;
#endif
}
#endif
int extdisplay_decoder_available(enum extdisplay_codec codec) { return codec_for(codec) != NULL; }
void extdisplay_decoder_disable_dmabuf(struct extdisplay_decoder *d)
{
    if (!d) return;
    d->dmabuf_off = 1;
#ifdef CONFIG_EXTDISPLAY_VAAPI
    for (uint32_t i = 0; i < EXTDISPLAY_DMABUF_SLOTS; i++) {
        if (d->vpp_src[i]) av_frame_unref(d->vpp_src[i]);
    }
#endif
}
void extdisplay_decoder_close(struct extdisplay_decoder *d)
{
    if (!d) return;
#ifdef CONFIG_EXTDISPLAY_VAAPI
    if (va_disp) {
        for (uint32_t i = 0; i < EXTDISPLAY_DMABUF_SLOTS; i++) {
            if (d->vpp_surf[i] != VA_INVALID_ID) (void)vaSyncSurface(va_disp, d->vpp_surf[i]);
        }
    }
    for (uint32_t i = 0; i < EXTDISPLAY_DMABUF_SLOTS; i++) {
        av_frame_free(&d->vpp_src[i]);
    }
    if (va_disp) {
        if (d->vpp_ctx != VA_INVALID_ID) vaDestroyContext(va_disp, d->vpp_ctx);
        if (d->vpp_cfg != VA_INVALID_ID) vaDestroyConfig(va_disp, d->vpp_cfg);
        for (uint32_t i = 0; i < EXTDISPLAY_DMABUF_SLOTS; i++) {
            if (d->vpp_surf[i] != VA_INVALID_ID) vaDestroySurfaces(va_disp, &d->vpp_surf[i], 1);
        }
    }
    for (uint32_t i = 0; i < EXTDISPLAY_DMABUF_SLOTS; i++) {
        if (d->vpp_prime_fd[i] >= 0) {
            close(d->vpp_prime_fd[i]);
            d->vpp_prime_fd[i] = -1;
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
    for (uint32_t i = 0; i < EXTDISPLAY_DMABUF_SLOTS; i++) {
        d->vpp_surf[i] = VA_INVALID_ID;
        d->vpp_prime_fd[i] = -1;
    }
    d->vpp_cfg = VA_INVALID_ID;
    d->vpp_ctx = VA_INVALID_ID;
    d->vpp_last_slot = -1;
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
    d->context->err_recognition = AV_EF_CAREFUL;
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
static void destroy_vpp_resources(struct extdisplay_decoder *d)
{
    if (va_disp) {
        if (d->vpp_ctx != VA_INVALID_ID) {
            vaDestroyContext(va_disp, d->vpp_ctx);
            d->vpp_ctx = VA_INVALID_ID;
        }
        if (d->vpp_cfg != VA_INVALID_ID) {
            vaDestroyConfig(va_disp, d->vpp_cfg);
            d->vpp_cfg = VA_INVALID_ID;
        }
        for (uint32_t i = 0; i < EXTDISPLAY_DMABUF_SLOTS; i++) {
            if (d->vpp_surf[i] != VA_INVALID_ID) {
                vaDestroySurfaces(va_disp, &d->vpp_surf[i], 1);
                d->vpp_surf[i] = VA_INVALID_ID;
            }
        }
    }
    for (uint32_t i = 0; i < EXTDISPLAY_DMABUF_SLOTS; i++) {
        if (d->vpp_prime_fd[i] >= 0) {
            close(d->vpp_prime_fd[i]);
            d->vpp_prime_fd[i] = -1;
        }
        d->vpp_pitch[i] = 0;
        d->vpp_offset[i] = 0;
        d->vpp_modifier[i] = 0;
    }
    d->vpp_using_scanout_bo = 0;
}

/* Allocate EXTDISPLAY_DMABUF_SLOTS scanout linear XRGB8888 BOs through libgbm
 * and import each into VA-API as a VPP BGRX destination via
 * VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2. libgbm asks the GPU's own DRM driver
 * for a scanout-capable linear buffer, so no vendor ioctl appears here. The
 * path is measured on Intel xe only; other drivers use the same calls but are
 * not verified by this code, and VA's refusal of an import falls back below.
 *
 * Returns 1 on success, 0 if libgbm is unavailable or VA import is rejected
 * (caller falls back to VA-allocated surfaces). */
static int init_vpp_scanout_surfaces(struct extdisplay_decoder *d)
{
#ifdef CONFIG_EXTDISPLAY_GBM
    if (!va_gbm) return 0;

    for (uint32_t i = 0; i < EXTDISPLAY_DMABUF_SLOTS; i++) {
        struct gbm_bo *bo = gbm_bo_create(va_gbm, d->width, d->height, GBM_FORMAT_XRGB8888,
                                          GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
        if (!bo) {
            destroy_vpp_resources(d);
            return 0;
        }
        int planes = gbm_bo_get_plane_count(bo);
        int prime_fd = planes == 1 ? gbm_bo_get_fd(bo) : -1;
        uint32_t pitch = gbm_bo_get_stride(bo);
        uint32_t offset = gbm_bo_get_offset(bo, 0);
        uint64_t mod = gbm_bo_get_modifier(bo);
        gbm_bo_destroy(bo);
        if (prime_fd < 0 || !pitch) {
            if (prime_fd >= 0) close(prime_fd);
            destroy_vpp_resources(d);
            return 0;
        }
        if (mod == DRM_FORMAT_MOD_INVALID) mod = DRM_FORMAT_MOD_LINEAR;
        off_t bo_sz = lseek(prime_fd, 0, SEEK_END);
        uint64_t min_sz = (uint64_t)offset + (uint64_t)pitch * d->height;
        uint64_t size = (bo_sz > 0 && (uint64_t)bo_sz >= min_sz) ? (uint64_t)bo_sz : min_sz;
        if (size > UINT32_MAX) {
            close(prime_fd);
            destroy_vpp_resources(d);
            return 0;
        }
        d->vpp_prime_fd[i] = prime_fd;
        d->vpp_pitch[i] = pitch;
        d->vpp_offset[i] = offset;
        d->vpp_modifier[i] = mod;

        VADRMPRIMESurfaceDescriptor desc;
        memset(&desc, 0, sizeof desc);
        desc.fourcc = VA_FOURCC_BGRX;
        desc.width = d->width;
        desc.height = d->height;
        desc.num_objects = 1;
        desc.objects[0].fd = prime_fd;
        desc.objects[0].size = (uint32_t)size;
        desc.objects[0].drm_format_modifier = mod;
        desc.num_layers = 1;
        desc.layers[0].drm_format = DRM_FORMAT_XRGB8888;
        desc.layers[0].num_planes = 1;
        desc.layers[0].object_index[0] = 0;
        desc.layers[0].offset[0] = offset;
        desc.layers[0].pitch[0] = pitch;

        VASurfaceAttrib attrs[2] = {
            { .type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE,
              .value = { .type = VAGenericValueTypeInteger,
                         .value = { .i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 } } },
            { .type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE,
              .value = { .type = VAGenericValueTypePointer, .value = { .p = &desc } } },
        };
        if (vaCreateSurfaces(va_disp, VA_RT_FORMAT_RGB32, d->width, d->height,
                             &d->vpp_surf[i], 1, attrs, 2) != VA_STATUS_SUCCESS) {
            destroy_vpp_resources(d);
            return 0;
        }
    }
    if (vaCreateConfig(va_disp, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &d->vpp_cfg) != VA_STATUS_SUCCESS ||
        vaCreateContext(va_disp, d->vpp_cfg, (int)d->width, (int)d->height, VA_PROGRESSIVE,
                        d->vpp_surf, EXTDISPLAY_DMABUF_SLOTS, &d->vpp_ctx) != VA_STATUS_SUCCESS) {
        destroy_vpp_resources(d);
        return 0;
    }
    d->vpp_using_scanout_bo = 1;
    return 1;
#else
    (void)d;
    return 0;
#endif
}

static int export_surface_prime(struct extdisplay_decoder *d, uint32_t slot, int *fds, struct extdisplay_dmabuf *info)
{
    for (uint32_t i = 0; i < EXTDISPLAY_DMABUF_MAX_PLANES; i++) fds[i] = -1;
    if (d->vpp_using_scanout_bo && d->vpp_prime_fd[slot] >= 0) {
        int dup_fd = fcntl(d->vpp_prime_fd[slot], F_DUPFD_CLOEXEC, 0);
        if (dup_fd < 0) return 0;
        memset(info, 0, sizeof *info);
        info->fourcc = DRM_FORMAT_XRGB8888;
        info->num_planes = 1;
        info->num_objects = 1;
        info->object_index[0] = 0;
        info->pitches[0] = d->vpp_pitch[slot];
        info->offsets[0] = d->vpp_offset[slot];
        info->modifiers[0] = d->vpp_modifier[slot];
        fds[0] = dup_fd;
        return 1;
    }
    VADRMPRIMESurfaceDescriptor prime;
    memset(&prime, 0, sizeof prime);
    if (vaExportSurfaceHandle(va_disp, d->vpp_surf[slot], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
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

int extdisplay_decoder_prepare_dmabuf(struct extdisplay_decoder *d, uint32_t slot)
{
#ifdef CONFIG_EXTDISPLAY_VAAPI
    if (!d || slot >= EXTDISPLAY_DMABUF_SLOTS || !d->has_ready || d->dmabuf_off || !va_vpp_ok || !va_disp ||
        d->ready->format != AV_PIX_FMT_VAAPI) {
        return 0;
    }
    if (!d->vpp_ready) {
        if (!init_vpp_scanout_surfaces(d)) {
            /* Fallback to VA-allocated surfaces when libgbm is unavailable or
             * the VA driver refuses PRIME_2 import for scanout BOs. */
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
                                 d->vpp_surf, EXTDISPLAY_DMABUF_SLOTS, attrs, 2) != VA_STATUS_SUCCESS ||
                vaCreateConfig(va_disp, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &d->vpp_cfg) != VA_STATUS_SUCCESS ||
                vaCreateContext(va_disp, d->vpp_cfg, (int)d->width, (int)d->height, VA_PROGRESSIVE,
                                d->vpp_surf, EXTDISPLAY_DMABUF_SLOTS, &d->vpp_ctx) != VA_STATUS_SUCCESS) {
                destroy_vpp_resources(d);
                d->dmabuf_off = 1;
                return 0;
            }
        }
        if (++vpp_gen_seq == 0) vpp_gen_seq = 1;
        d->vpp_gen = vpp_gen_seq;
        d->vpp_ready = 1;
    }
    VASurfaceID src = (VASurfaceID)(uintptr_t)d->ready->data[3];
    VASurfaceID dst = d->vpp_surf[slot];
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
    /* Hold the decoded DPB surface reference across VPP submission and explicit
     * vaSyncSurface completion so the decoder never recycles src while VPP reads
     * it, and DRM never scans out dst before VPP finishes writing regardless of
     * whether the kernel driver attaches implicit dma_resv fences. Keep d->ready
     * untouched until VPP succeeds so a mid-session VPP failure still falls back
     * cleanly to CPU rendering of the current frame. */
    if (!d->vpp_src[slot]) d->vpp_src[slot] = av_frame_alloc();
    if (!d->vpp_src[slot]) {
        vaDestroyBuffer(va_disp, vbuf);
        d->dmabuf_off = 1;
        return 0;
    }
    /* Ensure any earlier VPP batch targeting this slot has finished before we
     * drop its source DPB reference and submit the new frame into dst. */
    (void)vaSyncSurface(va_disp, dst);
    av_frame_unref(d->vpp_src[slot]);
    if (av_frame_ref(d->vpp_src[slot], d->ready) < 0) {
        vaDestroyBuffer(va_disp, vbuf);
        d->dmabuf_off = 1;
        return 0;
    }

    VAStatus st = vaBeginPicture(va_disp, d->vpp_ctx, dst);
    if (st == VA_STATUS_SUCCESS) st = vaRenderPicture(va_disp, d->vpp_ctx, &vbuf, 1);
    if (st == VA_STATUS_SUCCESS) st = vaEndPicture(va_disp, d->vpp_ctx);
    vaDestroyBuffer(va_disp, vbuf);
    if (st == VA_STATUS_SUCCESS) st = vaSyncSurface(va_disp, dst);
    /* VPP has completed (or failed): release the DPB source ref promptly so the
     * decoder's surface pool never starves while dst is displayed on screen. */
    av_frame_unref(d->vpp_src[slot]);
    if (st != VA_STATUS_SUCCESS) {
        d->dmabuf_off = 1;
        return 0;
    }
    av_frame_unref(d->ready);
    d->has_ready = 0;
    d->vpp_last_slot = (int)slot;
    d->vpp_has_last = 1;
    return 1;
#else
    (void)d; (void)slot;
    return 0;
#endif
}
int extdisplay_decoder_export_slot(struct extdisplay_decoder *d, uint32_t slot, int *fds, struct extdisplay_dmabuf *info)
{
#ifdef CONFIG_EXTDISPLAY_VAAPI
    if (!d || slot >= EXTDISPLAY_DMABUF_SLOTS || d->dmabuf_off || !va_vpp_ok || !va_disp || !d->vpp_ready) {
        return 0;
    }
    if (!export_surface_prime(d, slot, fds, info)) {
        d->dmabuf_off = 1;
        return 0;
    }
    info->slot = slot;
    info->generation = d->vpp_gen;
    return 1;
#else
    (void)d; (void)slot; (void)fds; (void)info;
    return 0;
#endif
}
uint32_t extdisplay_decoder_dmabuf_gen(const struct extdisplay_decoder *d)
{
#ifdef CONFIG_EXTDISPLAY_VAAPI
    return d && !d->dmabuf_off && d->vpp_ready ? d->vpp_gen : 0;
#else
    (void)d;
    return 0;
#endif
}
int extdisplay_decoder_export_dmabuf(struct extdisplay_decoder *d, int *fds, struct extdisplay_dmabuf *info)
{
#ifdef CONFIG_EXTDISPLAY_VAAPI
    if (!d) return 0;
    uint32_t slot = (uint32_t)d->vpp_idx;
    if (!extdisplay_decoder_prepare_dmabuf(d, slot)) return 0;
    if (!extdisplay_decoder_export_slot(d, slot, fds, info)) return 0;
    d->vpp_idx = (d->vpp_idx + 1) % (int)EXTDISPLAY_DMABUF_SLOTS;
    return 1;
#else
    (void)d; (void)fds; (void)info;
    return 0;
#endif
}
int extdisplay_decoder_reexport_dmabuf(struct extdisplay_decoder *d, int *fds, struct extdisplay_dmabuf *info)
{
#ifdef CONFIG_EXTDISPLAY_VAAPI
    if (!d || !d->vpp_has_last || d->vpp_last_slot < 0) return 0;
    return extdisplay_decoder_export_slot(d, (uint32_t)d->vpp_last_slot, fds, info);
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
int extdisplay_decoder_prepare_dmabuf(struct extdisplay_decoder *d, uint32_t slot)
{ (void)d; (void)slot; return 0; }
int extdisplay_decoder_export_slot(struct extdisplay_decoder *d, uint32_t slot, int *fds, struct extdisplay_dmabuf *info)
{ (void)d; (void)slot; (void)fds; (void)info; return 0; }
uint32_t extdisplay_decoder_dmabuf_gen(const struct extdisplay_decoder *d) { (void)d; return 0; }
int extdisplay_decoder_export_dmabuf(struct extdisplay_decoder *d, int *fds, struct extdisplay_dmabuf *info)
{ (void)d; (void)fds; (void)info; return 0; }
int extdisplay_decoder_reexport_dmabuf(struct extdisplay_decoder *d, int *fds, struct extdisplay_dmabuf *info)
{ (void)d; (void)fds; (void)info; return 0; }
int extdisplay_decoder_render(struct extdisplay_decoder *d, uint32_t *dst)
{ (void)d; (void)dst; errno = ENOTSUP; return -1; }
void extdisplay_decoder_disable_dmabuf(struct extdisplay_decoder *d) { (void)d; }
void extdisplay_decoder_close(struct extdisplay_decoder *d) { (void)d; }
#endif
