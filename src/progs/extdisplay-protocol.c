/* The sender protocol (extdisplay-protocol.h). Runs in extdisplay-worker for anything
 * the peer sent; the receiver itself only formats its own messages. */
#include "extdisplay-protocol.h"
#include <errno.h>
#include <json-c/json.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int extdisplay_dimensions(uint32_t w, uint32_t h, size_t *bytes)
{
    if (!w || !h || w > EXTDISPLAY_DIM_MAX || h > EXTDISPLAY_DIM_MAX || (uint64_t)w * h > EXTDISPLAY_PIXELS_MAX) return -1;
    if (bytes) *bytes = (size_t)w * h * 4;
    return 0;
}

int extdisplay_annex_b(const unsigned char *s, size_t n)
{
    if (!s || n < 5 || n > EXTDISPLAY_WIRE_MAX || memcmp(s, "\0\0\0\1", 4) != 0) return -1;
    /* Emulation prevention leaves 00 00 01 only in start codes: each must
     * be the long form. */
    for (size_t i = 4; i + 2 < n; i++) {
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1 && s[i - 1] != 0) return -1;
    }
    return 0;
}

void extdisplay_parser_clear(struct extdisplay_parser *p)
{
    free(p->payload);
    memset(p, 0, sizeof *p);
}

int extdisplay_parser_feed(struct extdisplay_parser *p, const void *buffer, size_t n, extdisplay_frame_fn receive, void *opaque)
{
    const unsigned char *s = buffer;
    if (p->failed || (!buffer && n) || !receive) return -1;
    while (n) {
        if (p->header_used < 4) {
            p->header[p->header_used++] = *s++;
            n--;
            if (p->header_used < 4) continue;
            p->size = (uint32_t)p->header[0] << 24 | (uint32_t)p->header[1] << 16 |
                      (uint32_t)p->header[2] << 8 | p->header[3];
            if (!p->size || p->size > EXTDISPLAY_WIRE_MAX || !(p->payload = malloc(p->size))) goto fail;
        }
        size_t take = p->size - p->used;
        if (take > n) take = n;
        memcpy(p->payload + p->used, s, take);
        p->used += take;
        s += take;
        n -= take;
        if (p->used == p->size) {
            int r = receive(opaque, p->payload, p->size);
            free(p->payload);
            p->payload = NULL;
            p->size = 0;
            p->used = p->header_used = 0;
            if (r < 0) goto fail;
        }
    }
    return 0;
fail:
    free(p->payload);
    p->payload = NULL;
    p->failed = 1;
    return -1;
}

int extdisplay_parser_eof(const struct extdisplay_parser *p)
{
    return !p->failed && !p->header_used && !p->used ? 0 : -1;
}

/* A string field must not hide a NUL (json-c keeps the length). */
static struct json_object *field(struct json_object *o, const char *name, enum json_type t)
{
    struct json_object *v = NULL;
    if (!json_object_object_get_ex(o, name, &v) || !json_object_is_type(v, t)) return NULL;
    if (t == json_type_string && strlen(json_object_get_string(v)) != (size_t)json_object_get_string_len(v)) return NULL;
    return v;
}

static int integer(struct json_object *o, const char *name, int64_t min, int64_t max, int64_t *out)
{
    struct json_object *v = field(o, name, json_type_int);
    if (!v) return -1;
    errno = 0;
    int64_t x = json_object_get_int64(v);
    if (errno || x < min || x > max) return -1;
    *out = x;
    return 0;
}

/* OpenDisplay's optional telemetry ({"cap":..,"snd":..}) before the first
 * start code: printable ASCII, skipped, never parsed. */
static int video(const unsigned char *s, size_t n, size_t *offset)
{
    size_t i = 0;
    if (s[0] == '{') {
        while (i < n && i < EXTDISPLAY_PREFIX_MAX && s[i] != 0) {
            if (s[i] < 0x20 || s[i] > 0x7e) return -1;
            i++;
        }
        if (i == 0 || i >= n || s[i - 1] != '}') return -1;
    }
    if (extdisplay_annex_b(s + i, n - i) < 0) return -1;
    *offset = i;
    return 0;
}

static int welcome(struct json_object *o, uint32_t *caps_out)
{
    int64_t version;
    struct json_object *v;
    if (integer(o, "locachy", EXTDISPLAY_SENDER_VERSION, EXTDISPLAY_SENDER_VERSION, &version) < 0 ||
        !(v = field(o, "extended", json_type_boolean)) || !json_object_get_boolean(v)) return -1;
    /* The whole input set, or no session: no mirror or touch-only fallback. */
    if (!(v = field(o, "input", json_type_array)) || json_object_array_length(v) > 16) return -1;
    static const char *const needed[] = { "key", "button", "cancel", "pointer", "scroll" };
    unsigned mask = 0;
    uint32_t extra = 0;
    for (size_t i = 0; i < json_object_array_length(v); i++) {
        struct json_object *cap = json_object_array_get_idx(v, i);
        if (!json_object_is_type(cap, json_type_string)) return -1;
        const char *c = json_object_get_string(cap);
        if (strlen(c) != (size_t)json_object_get_string_len(cap)) return -1;
        for (unsigned k = 0; k < sizeof needed / sizeof needed[0]; k++) {
            if (strcmp(c, needed[k]) == 0) mask |= 1u << k;
        }
        if (strcmp(c, EXTDISPLAY_CAP_TOUCH_NAME) == 0) extra |= EXTDISPLAY_INPUT_TOUCH;
        else if (strcmp(c, EXTDISPLAY_CAP_TABLET_NAME) == 0) extra |= EXTDISPLAY_INPUT_TABLET;
    }
    if (mask != 31) return -1;
    if (caps_out) *caps_out = extra;
    return 0;
}

int extdisplay_control_parse(const unsigned char *s, size_t n, struct extdisplay_control *out)
{
    memset(out, 0, sizeof *out);
    if (!s || !n || n > EXTDISPLAY_WIRE_MAX) return -1;
    /* OpenDisplay's demux (its section 4), kept in this one place. */
    if (s[0] != '{' || n > EXTDISPLAY_CONTROL_MAX || memchr(s, 0, n)) {
        if (video(s, n, &out->video_offset) < 0) return -1;
        out->type = EXTDISPLAY_MSG_VIDEO;
        return 0;
    }
    struct json_tokener *tok = json_tokener_new_ex(16);
    if (!tok) return -1;
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *o = json_tokener_parse_ex(tok, (const char *)s, (int)n);
    int ok = o && json_tokener_get_error(tok) == json_tokener_success &&
             json_tokener_get_parse_end(tok) == n && json_object_is_type(o, json_type_object);
    json_tokener_free(tok);
    if (!ok) {
        json_object_put(o);
        return -1;
    }
    int r = -1;
    struct json_object *v = field(o, "type", json_type_string);
    const char *t = v ? json_object_get_string(v) : NULL;
    if (!t) {
        goto done;
    } else if (strcmp(t, "welcome") == 0) {
        if (welcome(o, &out->input_caps) < 0) goto done;
        out->type = EXTDISPLAY_MSG_WELCOME;
    } else if (strcmp(t, "streamConfig") == 0) {
        int64_t w, h, fps;
        if (integer(o, "width", 1, EXTDISPLAY_DIM_MAX, &w) < 0 || integer(o, "height", 1, EXTDISPLAY_DIM_MAX, &h) < 0 ||
            integer(o, "framesPerSecond", 1, EXTDISPLAY_FPS_MAX, &fps) < 0 ||
            extdisplay_dimensions((uint32_t)w, (uint32_t)h, NULL) < 0 || !(v = field(o, "codec", json_type_string))) {
            goto done;
        }
        const char *codec = json_object_get_string(v);
        out->codec = strcmp(codec, "h264") == 0 ? EXTDISPLAY_CODEC_H264 :
            (strcmp(codec, "hevc") == 0 || strcmp(codec, "hevc420") == 0 || strcmp(codec, "hevc444") == 0)
                ? EXTDISPLAY_CODEC_HEVC
                : EXTDISPLAY_CODEC_NONE;
        if (out->codec == EXTDISPLAY_CODEC_NONE) goto done;
        out->type = EXTDISPLAY_MSG_CONFIG;
        out->width = (uint32_t)w;
        out->height = (uint32_t)h;
        out->fps = (uint32_t)fps;
    } else if (strcmp(t, "ping") == 0 || strcmp(t, "pong") == 0) {
        out->type = strcmp(t, "ping") == 0 ? EXTDISPLAY_MSG_PING : EXTDISPLAY_MSG_PONG;
        if (json_object_object_get_ex(o, "t", NULL) && integer(o, "t", 0, INT64_MAX, &out->timestamp) < 0) goto done;
    } else if (strcmp(t, "closing") == 0) {
        out->type = EXTDISPLAY_MSG_CLOSING;
    }
    r = 0;
done:
    json_object_put(o);
    return r;
}

static int fits(int n, size_t size) { return n >= 0 && (size_t)n < size ? n : -1; }

static int unit(double v) { return isfinite(v) && v >= 0 && v <= 1; }

static void sanitize_label(const char *src, char *dst, size_t size)
{
    size_t used = 0;
    if (!src || !dst || !size) {
        if (dst && size) dst[0] = '\0';
        return;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p && used + 1 < size; p++) {
        unsigned char c = *p;
        if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') continue;
        if (c == ' ' && (used == 0 || dst[used - 1] == ' ')) continue;
        dst[used++] = (char)c;
    }
    while (used > 0 && dst[used - 1] == ' ') used--;
    dst[used] = '\0';
}

uint32_t extdisplay_select_video_caps(uint32_t worker_caps, const char *decode_mode)
{
    int sw_h264 = (worker_caps & 1u) != 0;
    int sw_hevc = (worker_caps & 2u) != 0;
    int hw_h264 = (worker_caps & 4u) != 0;
    int hw_hevc420 = (worker_caps & 8u) != 0;
    int hw_hevc444 = (worker_caps & 16u) != 0;
    int any_hw = hw_h264 || hw_hevc420 || hw_hevc444;
    uint32_t out = 0;

    if (decode_mode && strcmp(decode_mode, "sw") == 0) {
        if (sw_h264) out |= EXTDISPLAY_VCAP_H264;
        if (sw_hevc) out |= EXTDISPLAY_VCAP_HEVC | EXTDISPLAY_VCAP_HEVC420 | EXTDISPLAY_VCAP_HEVC444;
        return out;
    }
    if (any_hw) {
        /* Hardware decode active: advertise exact hardware-decoded profiles so
         * senders never send HEVC 4:4:4 to a GPU that only decodes 4:2:0.
         * Legacy "hevc" (which older senders answer with HEVC 4:4:4) is only
         * offered when VA-API actually decodes HEVC 4:4:4. */
        if (hw_h264 || sw_h264) out |= EXTDISPLAY_VCAP_H264;
        if (hw_hevc420 && sw_hevc) out |= EXTDISPLAY_VCAP_HEVC420;
        if (hw_hevc444 && sw_hevc) out |= EXTDISPLAY_VCAP_HEVC444 | EXTDISPLAY_VCAP_HEVC;
        return out;
    }
    if (decode_mode && strcmp(decode_mode, "hw") == 0) {
        /* Explicit hw mode without VA-API: still keep H.264 available as last
         * resort if FFmpeg has it, without claiming 4:4:4 HEVC. */
        if (sw_h264) out |= EXTDISPLAY_VCAP_H264;
        return out;
    }
    /* No VA-API available (e.g. VM or very old GPU): keep software H.264 and
     * HEVC 4:2:0 available without triggering 4:4:4 on older senders. */
    if (sw_h264) out |= EXTDISPLAY_VCAP_H264;
    if (sw_hevc) out |= EXTDISPLAY_VCAP_HEVC420;
    return out;
}

int extdisplay_hello_full(char *s, size_t size, uint32_t w, uint32_t h, double scale,
                    uint32_t video_caps, const char *host, const char *model, uint32_t input_caps)
{
    if (extdisplay_dimensions(w, h, NULL) < 0 || !isfinite(scale) || scale < 0.5 || scale > 4 ||
        !(video_caps & (EXTDISPLAY_VCAP_H264 | EXTDISPLAY_VCAP_HEVC | EXTDISPLAY_VCAP_HEVC420 | EXTDISPLAY_VCAP_HEVC444))) {
        return -1;
    }
    char safe_host[64], safe_model[64], extra[192] = "", input[160] = "", vcaps[512] = "";
    sanitize_label(host, safe_host, sizeof safe_host);
    sanitize_label(model, safe_model, sizeof safe_model);
    if (safe_host[0] && safe_model[0]) {
        snprintf(extra, sizeof extra, ",\"host\":\"%s\",\"model\":\"%s\"", safe_host, safe_model);
    } else if (safe_host[0]) {
        snprintf(extra, sizeof extra, ",\"host\":\"%s\"", safe_host);
    } else if (safe_model[0]) {
        snprintf(extra, sizeof extra, ",\"model\":\"%s\"", safe_model);
    }
    static const struct { uint32_t bit; const char *name; } codecs[] = {
        { EXTDISPLAY_VCAP_H264, "h264" },
        { EXTDISPLAY_VCAP_HEVC, "hevc" },
        { EXTDISPLAY_VCAP_HEVC420, "hevc420" },
        { EXTDISPLAY_VCAP_HEVC444, "hevc444" },
    };
    size_t vpos = 0;
    for (size_t i = 0; i < sizeof codecs / sizeof codecs[0]; i++) {
        if (!(video_caps & codecs[i].bit)) continue;
        int added = snprintf(vcaps + vpos, sizeof vcaps - vpos,
                             "%s{\"codec\":\"%s\",\"maxWidth\":4096,\"maxHeight\":4096,\"maxFrameRate\":60}",
                             vpos ? "," : "", codecs[i].name);
        if (added < 0 || (size_t)added >= sizeof vcaps - vpos) return -1;
        vpos += (size_t)added;
    }
    /* The five required inputs, then the optional ones this receiver can take. */
    snprintf(input, sizeof input, ",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"%s%s",
             "key", "button", "cancel", "pointer", "scroll",
             input_caps & EXTDISPLAY_INPUT_TOUCH ? ",\"" EXTDISPLAY_CAP_TOUCH_NAME "\"" : "",
             input_caps & EXTDISPLAY_INPUT_TABLET ? ",\"" EXTDISPLAY_CAP_TABLET_NAME "\"" : "");
    return fits(snprintf(s, size,
        "{\"type\":\"hello\",\"pv\":3,\"locachy\":%d,\"device\":\"LOCachy\",\"extendedOnly\":true%s,"
        "\"pixelsWide\":%u,\"pixelsHigh\":%u,\"scale\":%.3f,"
        "\"panel\":{\"pixelsWide\":%u,\"pixelsHigh\":%u,\"scale\":%.3f},"
        "\"displayMaxFrameRate\":%d,\"maxEncodeWide\":%u,\"maxEncodeHigh\":%u,"
        "\"videoCaps\":[%s],\"input\":[%s]}",
        EXTDISPLAY_SENDER_VERSION, extra, w, h, scale, w, h, scale, EXTDISPLAY_FPS_MAX, EXTDISPLAY_DIM_MAX, EXTDISPLAY_DIM_MAX,
        vcaps, input + 1), size);
}

int extdisplay_hello_caps(char *s, size_t size, uint32_t w, uint32_t h, double scale,
                    int h264, int hevc, const char *host, const char *model, uint32_t input_caps)
{
    uint32_t vcaps = (h264 ? EXTDISPLAY_VCAP_H264 : 0u) | (hevc ? EXTDISPLAY_VCAP_HEVC : 0u);
    return extdisplay_hello_full(s, size, w, h, scale, vcaps, host, model, input_caps);
}

int extdisplay_hello_named(char *s, size_t size, uint32_t w, uint32_t h, double scale,
                    int h264, int hevc, const char *host, const char *model)
{
    return extdisplay_hello_caps(s, size, w, h, scale, h264, hevc, host, model, 0);
}

int extdisplay_hello(char *s, size_t size, uint32_t w, uint32_t h, double scale, int h264, int hevc)
{
    return extdisplay_hello_named(s, size, w, h, scale, h264, hevc, NULL, NULL);
}


int extdisplay_key_json(char *s, size_t size, uint32_t device, uint32_t code, uint32_t sym, uint32_t mods,
                 uint32_t locks, int state)
{
    if (!device || code > 767 || state < 0 || state > 2 || mods > 127 || locks > 3) return -1;
    return fits(snprintf(s, size,
        "{\"type\":\"key\",\"device\":%u,\"code\":%u,\"keysym\":%u,\"modifiers\":%u,\"locks\":%u,"
        "\"state\":%d,\"mapping\":\"logical\"}", device, code, sym, mods, locks, state), size);
}

int extdisplay_button_json(char *s, size_t size, uint32_t button, int pressed, double x, double y)
{
    if (button < 272 || button > 274 || (pressed != 0 && pressed != 1) || !unit(x) || !unit(y)) return -1;
    return fits(snprintf(s, size, "{\"type\":\"button\",\"button\":%u,\"state\":%d,\"x\":%.6f,\"y\":%.6f}",
                         button, pressed, x, y), size);
}

int extdisplay_pointer_json(char *s, size_t size, double x, double y)
{
    if (!unit(x) || !unit(y)) return -1;
    return fits(snprintf(s, size, "{\"type\":\"pointer\",\"x\":%.6f,\"y\":%.6f}", x, y), size);
}

int extdisplay_pointer_delta_json(char *s, size_t size, double x, double y, double dx, double dy)
{
    if (!unit(x) || !unit(y) || !isfinite(dx) || !isfinite(dy) || fabs(dx) > 16384 || fabs(dy) > 16384) return -1;
    return fits(snprintf(s, size, "{\"type\":\"pointer\",\"x\":%.6f,\"y\":%.6f,\"dx\":%.4f,\"dy\":%.4f}", x, y, dx, dy), size);
}

int extdisplay_scroll_json(char *s, size_t size, double dx, double dy, int phase, int source)
{
    if (!isfinite(dx) || !isfinite(dy) || fabs(dx) > 100000 || fabs(dy) > 100000 ||
        phase < EXTDISPLAY_PHASE_NONE || phase > EXTDISPLAY_PHASE_CANCEL || source < EXTDISPLAY_SOURCE_NONE || source > EXTDISPLAY_SOURCE_TOUCH) {
        return -1;
    }
    return fits(snprintf(s, size, "{\"type\":\"scroll\",\"dx\":%.4f,\"dy\":%.4f,\"phase\":%d,\"source\":%d}",
                         dx, dy, phase, source), size);
}

int extdisplay_cancel_json(char *s, size_t size)
{
    return fits(snprintf(s, size, "{\"type\":\"cancel\",\"scope\":\"all\"}"), size);
}

/* ---- tablet tools and multi-touch (peer capabilities "tablet" and "touch") ---- */

static int finite_within(double v, double limit) { return isfinite(v) && v >= -limit && v <= limit; }

static double clean_zero(double v) { return v == 0 ? 0.0 : v; }

static int tablet_button(uint32_t button) { return button >= 0x100 && button <= 0x2ff; }

const char *extdisplay_tablet_tool_name(enum extdisplay_tablet_tool tool)
{
    static const char *const names[] = { "other", "pen", "eraser", "brush", "pencil", "airbrush", "lens", "mouse" };
    return (unsigned)tool < sizeof names / sizeof names[0] ? names[tool] : "other";
}

static const char *touch_state_name(enum extdisplay_touch_state state)
{
    static const char *const names[] = { "down", "move", "up", "cancel" };
    return names[state];
}

int extdisplay_tablet_proximity_json(char *s, size_t size, int in, enum extdisplay_tablet_tool tool,
                                     uint64_t id, uint32_t caps, double x, double y)
{
    if (!unit(x) || !unit(y) || (unsigned)tool > EXTDISPLAY_TOOL_MOUSE) return -1;
    return fits(snprintf(s, size,
        "{\"type\":\"tablet\",\"event\":\"proximity\",\"in\":%s,\"tool\":\"%s\",\"id\":%llu,\"caps\":%u,"
        "\"x\":%.6f,\"y\":%.6f}",
        in ? "true" : "false", extdisplay_tablet_tool_name(tool), (unsigned long long)id, caps,
        clean_zero(x), clean_zero(y)), size);
}

int extdisplay_tablet_axis_json(char *s, size_t size, double x, double y, double pressure,
                                double tilt_x, double tilt_y, double distance, double rotation,
                                uint32_t buttons)
{
    if (!unit(x) || !unit(y) || !unit(pressure) || !unit(distance) || !finite_within(tilt_x, 90) ||
        !finite_within(tilt_y, 90) || !finite_within(rotation, 360) || buttons > 0xffu) {
        return -1;
    }
    return fits(snprintf(s, size,
        "{\"type\":\"tablet\",\"event\":\"axis\",\"x\":%.6f,\"y\":%.6f,\"pressure\":%.4f,"
        "\"tiltX\":%.1f,\"tiltY\":%.1f,\"distance\":%.4f,\"rotation\":%.1f,\"buttons\":%u}",
        clean_zero(x), clean_zero(y), clean_zero(pressure), clean_zero(tilt_x), clean_zero(tilt_y),
        clean_zero(distance), clean_zero(rotation), buttons), size);
}

int extdisplay_tablet_tip_json(char *s, size_t size, int down, double x, double y, double pressure,
                               uint32_t buttons)
{
    if ((down != 0 && down != 1) || !unit(x) || !unit(y) || !unit(pressure) || buttons > 0xffu) return -1;
    return fits(snprintf(s, size,
        "{\"type\":\"tablet\",\"event\":\"tip\",\"down\":%s,\"x\":%.6f,\"y\":%.6f,\"pressure\":%.4f,\"buttons\":%u}",
        down ? "true" : "false", clean_zero(x), clean_zero(y), clean_zero(pressure), buttons), size);
}

int extdisplay_tablet_button_json(char *s, size_t size, uint32_t button, int state, double x, double y,
                                  uint32_t buttons)
{
    if (!tablet_button(button) || (state != 0 && state != 1) || !unit(x) || !unit(y) || buttons > 0xffu) return -1;
    return fits(snprintf(s, size,
        "{\"type\":\"tablet\",\"event\":\"button\",\"button\":%u,\"state\":%d,\"x\":%.6f,\"y\":%.6f,\"buttons\":%u}",
        button, state, clean_zero(x), clean_zero(y), buttons), size);
}

int extdisplay_touch_frame_add(struct extdisplay_touch_frame *f, int32_t id, enum extdisplay_touch_state state,
                               double x, double y, double xmm, double ymm)
{
    if (!f || f->count >= EXTDISPLAY_TOUCH_POINTS_MAX) return -1;
    f->points[f->count++] = (struct extdisplay_touch_point){ .id = id, .state = state, .x = x, .y = y,
                                                             .xmm = xmm, .ymm = ymm };
    return 0;
}

int extdisplay_touch_frame_json(char *s, size_t size, const struct extdisplay_touch_point *points, size_t count)
{
    if (!points || !count || count > EXTDISPLAY_TOUCH_POINTS_MAX || !size) return -1;
    int n = snprintf(s, size, "{\"type\":\"touch\",\"phase\":\"frame\",\"points\":[");
    if (n < 0 || (size_t)n >= size) return -1;
    size_t used = (size_t)n;
    for (size_t i = 0; i < count; i++) {
        const struct extdisplay_touch_point *p = &points[i];
        if (p->id < 0 || p->id >= (int32_t)EXTDISPLAY_TOUCH_POINTS_MAX || (unsigned)p->state > EXTDISPLAY_TOUCH_CANCEL ||
            !unit(p->x) || !unit(p->y) || !finite_within(p->xmm, 10000) || !finite_within(p->ymm, 10000)) {
            return -1;
        }
        n = snprintf(s + used, size - used,
            "%s{\"id\":%d,\"state\":\"%s\",\"x\":%.6f,\"y\":%.6f,\"xmm\":%.1f,\"ymm\":%.1f}",
            i ? "," : "", (int)p->id, touch_state_name(p->state), clean_zero(p->x), clean_zero(p->y),
            clean_zero(p->xmm), clean_zero(p->ymm));
        if (n < 0 || (size_t)n >= size - used) return -1;
        used += (size_t)n;
    }
    n = snprintf(s + used, size - used, "]}");
    if (n < 0 || (size_t)n >= size - used) return -1;
    return (int)(used + (size_t)n);
}

int extdisplay_touch_frame_droppable(const struct extdisplay_touch_point *points, size_t count)
{
    if (!points || !count) return 0;
    for (size_t i = 0; i < count; i++) {
        if (points[i].state != EXTDISPLAY_TOUCH_MOVE) return 0;
    }
    return 1;
}

/* Whether the n bytes at s contain the NUL-terminated needle. */
static int contains(const char *s, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; m <= n && i <= n - m; i++) {
        if (memcmp(s + i, needle, m) == 0) return 1;
    }
    return 0;
}

static int starts(const char *s, size_t n, const char *prefix)
{
    size_t m = strlen(prefix);
    return n >= m && memcmp(s, prefix, m) == 0;
}

int extdisplay_input_droppable(const char *s, size_t n)
{
    if (!s) return 0;
    if (starts(s, n, "{\"type\":\"pointer\"")) return 1;
    if (starts(s, n, "{\"type\":\"scroll\"")) return contains(s, n, "\"phase\":2");
    if (starts(s, n, "{\"type\":\"tablet\",\"event\":\"axis\"")) return 1;
    if (starts(s, n, "{\"type\":\"touch\",\"phase\":\"frame\"")) {
        return contains(s, n, "\"state\":\"move\"") && !contains(s, n, "\"state\":\"down\"") &&
               !contains(s, n, "\"state\":\"up\"") && !contains(s, n, "\"state\":\"cancel\"");
    }
    return 0;
}

