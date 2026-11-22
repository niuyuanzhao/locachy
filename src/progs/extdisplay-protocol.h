/*
 * The LOCachy sender protocol of the extended-display receiver
 * (tools/EXTDISPLAY.md): OpenDisplay pv 3 framing and hello/streamConfig shapes,
 * plus a mandatory LOCachy capability handshake and key/button/cancel input.
 * It is not VNC and not an unauthenticated OpenDisplay endpoint: it runs only
 * over a stream the broker has already paired.
 *
 * Every message, both ways: [uint32 big-endian payload length][payload].
 * A payload is a control message (JSON object, below 32 KiB, starting with
 * '{', no NUL) or a video access unit (H.264 or HEVC Annex-B, four-byte start
 * codes, after an optional OpenDisplay telemetry prefix that is skipped).
 */
#ifndef LOCACHY_EXTDISPLAY_PROTOCOL_H
#define LOCACHY_EXTDISPLAY_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>

#define EXTDISPLAY_WIRE_MAX (8u * 1024u * 1024u) /* one payload */
#define EXTDISPLAY_CONTROL_MAX 32767u            /* OpenDisplay's control demux limit */
#define EXTDISPLAY_PREFIX_MAX 256u               /* a telemetry prefix before the first start code */
#define EXTDISPLAY_DIM_MAX 4096u
#define EXTDISPLAY_PIXELS_MAX (16u * 1024u * 1024u)
#define EXTDISPLAY_FPS_MAX 60
#define EXTDISPLAY_SENDER_VERSION 1

/* Input enums on the wire (numbers): base.h's values, fixed here. */
enum { EXTDISPLAY_PHASE_NONE, EXTDISPLAY_PHASE_BEGIN, EXTDISPLAY_PHASE_UPDATE, EXTDISPLAY_PHASE_END, EXTDISPLAY_PHASE_CANCEL };
enum { EXTDISPLAY_SOURCE_NONE, EXTDISPLAY_SOURCE_WHEEL, EXTDISPLAY_SOURCE_FINGER, EXTDISPLAY_SOURCE_CONTINUOUS, EXTDISPLAY_SOURCE_TOUCH };
/* key.modifiers bits (base.h KEYS_*), key.locks bits. */
#define EXTDISPLAY_MOD_SHIFT 1u
#define EXTDISPLAY_MOD_CTRL  2u
#define EXTDISPLAY_MOD_ALT   4u
#define EXTDISPLAY_MOD_META  8u
#define EXTDISPLAY_MOD_ALTGR 16u
#define EXTDISPLAY_MOD_CAPS  32u
#define EXTDISPLAY_MOD_NUM   64u
#define EXTDISPLAY_LOCK_CAPS 1u
#define EXTDISPLAY_LOCK_NUM  2u

/* Optional peer input capabilities negotiated in welcome.input. */
#define EXTDISPLAY_CAP_TOUCH_NAME  "touch"
#define EXTDISPLAY_CAP_TABLET_NAME "tablet"
#define EXTDISPLAY_INPUT_TOUCH  (1u << 0)
#define EXTDISPLAY_INPUT_TABLET (1u << 1)

/* Touch point states on the wire (strings: "down", "move", "up", "cancel"). */
enum extdisplay_touch_state {
    EXTDISPLAY_TOUCH_DOWN,
    EXTDISPLAY_TOUCH_MOVE,
    EXTDISPLAY_TOUCH_UP,
    EXTDISPLAY_TOUCH_CANCEL,
};
#define EXTDISPLAY_TOUCH_POINTS_MAX 10u /* max contacts in one touch frame */
struct extdisplay_touch_point {
    int32_t id;
    enum extdisplay_touch_state state;
    double x, y;     /* normalized [0, 1] */
    double xmm, ymm; /* millimetres from device resolution */
};
struct extdisplay_touch_frame {
    struct extdisplay_touch_point points[EXTDISPLAY_TOUCH_POINTS_MAX];
    size_t count;
};
/* Append one contact change in order: 0, or -1 when the frame is full (send it first). */
int extdisplay_touch_frame_add(struct extdisplay_touch_frame *f, int32_t id, enum extdisplay_touch_state state,
                               double x, double y, double xmm, double ymm);

/* Tablet tool types on the wire. */
enum extdisplay_tablet_tool {
    EXTDISPLAY_TOOL_OTHER,
    EXTDISPLAY_TOOL_PEN,
    EXTDISPLAY_TOOL_ERASER,
    EXTDISPLAY_TOOL_BRUSH,
    EXTDISPLAY_TOOL_PENCIL,
    EXTDISPLAY_TOOL_AIRBRUSH,
    EXTDISPLAY_TOOL_LENS,
    EXTDISPLAY_TOOL_MOUSE,
};

enum extdisplay_codec { EXTDISPLAY_CODEC_NONE, EXTDISPLAY_CODEC_H264, EXTDISPLAY_CODEC_HEVC };
enum extdisplay_message { EXTDISPLAY_MSG_UNKNOWN, EXTDISPLAY_MSG_WELCOME, EXTDISPLAY_MSG_CONFIG,
                   EXTDISPLAY_MSG_PING, EXTDISPLAY_MSG_PONG, EXTDISPLAY_MSG_CLOSING, EXTDISPLAY_MSG_VIDEO };
struct extdisplay_control {
    enum extdisplay_message type;
    enum extdisplay_codec codec;
    uint32_t width, height, fps;
    uint32_t input_caps; /* EXTDISPLAY_MSG_WELCOME: EXTDISPLAY_INPUT_* bitmask */
    int64_t timestamp;
    size_t video_offset; /* EXTDISPLAY_MSG_VIDEO: the first start code, after any prefix */
};

/* A callback consumes its borrowed buffer synchronously. Returning -1 poisons
 * the parser: a malformed connection is never resumed. */
typedef int (*extdisplay_frame_fn)(void *, const unsigned char *, size_t);
struct extdisplay_parser {
    unsigned char header[4], *payload;
    size_t header_used, used;
    uint32_t size;
    int failed;
};
void extdisplay_parser_clear(struct extdisplay_parser *p);
int extdisplay_parser_feed(struct extdisplay_parser *p, const void *data, size_t size,
                    extdisplay_frame_fn receive, void *opaque);
int extdisplay_parser_eof(const struct extdisplay_parser *p); /* 0: ended between payloads */

/* 0 and *bytes (ARGB) for a picture within the limits; -1 otherwise. */
int extdisplay_dimensions(uint32_t width, uint32_t height, size_t *bytes);
/* Every NAL starts with a four-byte code (OpenDisplay 5.1); 0 or -1. */
int extdisplay_annex_b(const unsigned char *data, size_t size);

/* Unknown control types are EXTDISPLAY_MSG_UNKNOWN (ignored); a malformed control,
 * a known type with invalid fields, or video that is not Annex-B is -1. */
int extdisplay_control_parse(const unsigned char *data, size_t size, struct extdisplay_control *out);

/* Video codec capability flags for hello.videoCaps.
 * "h264" and "hevc" (which older senders treat as HEVC 4:4:4) keep legacy
 * interoperability; "hevc420" and "hevc444" express exact chroma profiles so
 * senders pick the highest hardware-decoded format on any GPU. */
#define EXTDISPLAY_VCAP_H264    (1u << 0)
#define EXTDISPLAY_VCAP_HEVC    (1u << 1)
#define EXTDISPLAY_VCAP_HEVC420 (1u << 2)
#define EXTDISPLAY_VCAP_HEVC444 (1u << 3)

/* Derive hello videoCaps bitmask from worker READY flags (bit 0 sw H.264,
 * bit 1 sw HEVC, bit 2 hw H.264, bit 3 hw HEVC 4:2:0, bit 4 hw HEVC 4:4:4)
 * and decode_mode ("auto", "hw", "sw"). */
uint32_t extdisplay_select_video_caps(uint32_t worker_caps, const char *decode_mode);

/* The receiver's messages: their length, or -1 (invalid or does not fit). */
int extdisplay_hello(char *out, size_t size, uint32_t width, uint32_t height, double scale,
              int h264, int hevc);
int extdisplay_hello_named(char *out, size_t size, uint32_t width, uint32_t height, double scale,
                    int h264, int hevc, const char *host, const char *model);
int extdisplay_hello_caps(char *out, size_t size, uint32_t width, uint32_t height, double scale,
                    int h264, int hevc, const char *host, const char *model, uint32_t input_caps);
int extdisplay_hello_full(char *out, size_t size, uint32_t width, uint32_t height, double scale,
                    uint32_t video_caps, const char *host, const char *model, uint32_t input_caps);
int extdisplay_key_json(char *out, size_t size, uint32_t device, uint32_t code,
                 uint32_t keysym, uint32_t mods, uint32_t locks, int state);
int extdisplay_button_json(char *out, size_t size, uint32_t button, int pressed,
                    double x, double y);
int extdisplay_pointer_json(char *out, size_t size, double x, double y);
int extdisplay_pointer_delta_json(char *out, size_t size, double x, double y, double dx, double dy);
int extdisplay_scroll_json(char *out, size_t size, double dx, double dy, int phase, int source);
int extdisplay_cancel_json(char *out, size_t size);

/* Additive tablet and multi-touch wire formatters (used only when the peer's
 * welcome.input includes "tablet" or "touch"). Coordinates are normalized in
 * [0, 1]. Returns message length or -1 on invalid input or insufficient buffer. */
const char *extdisplay_tablet_tool_name(enum extdisplay_tablet_tool tool);
int extdisplay_tablet_proximity_json(char *out, size_t size, int in, enum extdisplay_tablet_tool tool,
                                     uint64_t id, uint32_t caps, double x, double y);
int extdisplay_tablet_axis_json(char *out, size_t size, double x, double y, double pressure,
                                double tilt_x, double tilt_y, double distance, double rotation,
                                uint32_t buttons);
int extdisplay_tablet_tip_json(char *out, size_t size, int down, double x, double y, double pressure,
                               uint32_t buttons);
int extdisplay_tablet_button_json(char *out, size_t size, uint32_t button, int state, double x, double y,
                                  uint32_t buttons);
int extdisplay_touch_frame_json(char *out, size_t size, const struct extdisplay_touch_point *points,
                                size_t count);
/* 1 if a touch frame contains only "move" points (may be shed under backpressure),
 * 0 if it contains any down/up/cancel transition (never drop). */
int extdisplay_touch_frame_droppable(const struct extdisplay_touch_point *points, size_t count);
/* 1 if a serialized input message may be shed under backpressure (pointer motion,
 * scroll update, tablet axis, or move-only touch frame); 0 otherwise. */
int extdisplay_input_droppable(const char *json, size_t len);
#endif
