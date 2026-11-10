/*
 * The LOCachy companion protocol of the extended-display receiver
 * (tools/KVM.md): OpenDisplay pv 3 framing and hello/streamConfig shapes,
 * plus a mandatory LOCachy capability handshake and key/button/cancel input.
 * It is not VNC and not an unauthenticated OpenDisplay endpoint: it runs only
 * over a stream the broker has already paired.
 *
 * Every message, both ways: [uint32 big-endian payload length][payload].
 * A payload is a control message (JSON object, below 32 KiB, starting with
 * '{', no NUL) or a video access unit (H.264 or HEVC Annex-B, four-byte start
 * codes, after an optional OpenDisplay telemetry prefix that is skipped).
 */
#ifndef LOCACHY_KVM_PROTOCOL_H
#define LOCACHY_KVM_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>

#define KVM_WIRE_MAX (4u * 1024u * 1024u) /* one payload */
#define KVM_CONTROL_MAX 32767u            /* OpenDisplay's control demux limit */
#define KVM_PREFIX_MAX 256u               /* a telemetry prefix before the first start code */
#define KVM_DIM_MAX 4096u
#define KVM_PIXELS_MAX (16u * 1024u * 1024u)
#define KVM_FPS_MAX 60
#define KVM_COMPANION_VERSION 1

/* Input enums on the wire (numbers): base.h's values, fixed here. */
enum { KVM_PHASE_NONE, KVM_PHASE_BEGIN, KVM_PHASE_UPDATE, KVM_PHASE_END, KVM_PHASE_CANCEL };
enum { KVM_SOURCE_NONE, KVM_SOURCE_WHEEL, KVM_SOURCE_FINGER, KVM_SOURCE_CONTINUOUS, KVM_SOURCE_TOUCH };
/* key.modifiers bits (base.h KEYS_*), key.locks bits. */
#define KVM_MOD_SHIFT 1u
#define KVM_MOD_CTRL  2u
#define KVM_MOD_ALT   4u
#define KVM_MOD_META  8u
#define KVM_MOD_ALTGR 16u
#define KVM_MOD_CAPS  32u
#define KVM_MOD_NUM   64u
#define KVM_LOCK_CAPS 1u
#define KVM_LOCK_NUM  2u

enum kvm_codec { KVM_CODEC_NONE, KVM_CODEC_H264, KVM_CODEC_HEVC };
enum kvm_message { KVM_MSG_UNKNOWN, KVM_MSG_WELCOME, KVM_MSG_CONFIG,
                   KVM_MSG_PING, KVM_MSG_PONG, KVM_MSG_CLOSING, KVM_MSG_VIDEO };
struct kvm_control {
    enum kvm_message type;
    enum kvm_codec codec;
    uint32_t width, height, fps;
    int64_t timestamp;
    size_t video_offset; /* KVM_MSG_VIDEO: the first start code, after any prefix */
};

/* A callback consumes its borrowed buffer synchronously. Returning -1 poisons
 * the parser: a malformed connection is never resumed. */
typedef int (*kvm_frame_fn)(void *, const unsigned char *, size_t);
struct kvm_parser {
    unsigned char header[4], *payload;
    size_t header_used, used;
    uint32_t size;
    int failed;
};
void kvm_parser_clear(struct kvm_parser *p);
int kvm_parser_feed(struct kvm_parser *p, const void *data, size_t size,
                    kvm_frame_fn receive, void *opaque);
int kvm_parser_eof(const struct kvm_parser *p); /* 0: ended between payloads */

/* 0 and *bytes (ARGB) for a picture within the limits; -1 otherwise. */
int kvm_dimensions(uint32_t width, uint32_t height, size_t *bytes);
/* Every NAL starts with a four-byte code (OpenDisplay 5.1); 0 or -1. */
int kvm_annex_b(const unsigned char *data, size_t size);

/* Unknown control types are KVM_MSG_UNKNOWN (ignored); a malformed control,
 * a known type with invalid fields, or video that is not Annex-B is -1. */
int kvm_control_parse(const unsigned char *data, size_t size, struct kvm_control *out);

/* The receiver's messages: their length, or -1 (invalid or does not fit). */
int kvm_hello(char *out, size_t size, uint32_t width, uint32_t height, double scale,
              int h264, int hevc);
int kvm_key_json(char *out, size_t size, uint32_t device, uint32_t code,
                 uint32_t keysym, uint32_t mods, uint32_t locks, int state);
int kvm_button_json(char *out, size_t size, uint32_t button, int pressed,
                    double x, double y);
int kvm_pointer_json(char *out, size_t size, double x, double y);
int kvm_scroll_json(char *out, size_t size, double dx, double dy, int phase, int source);
int kvm_cancel_json(char *out, size_t size);
#endif
