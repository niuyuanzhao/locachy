/* The companion protocol (kvm-protocol.h). Runs in kvm-worker for anything
 * the peer sent; the receiver itself only formats its own messages. */
#include "kvm-protocol.h"
#include <errno.h>
#include <json-c/json.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kvm_dimensions(uint32_t w, uint32_t h, size_t *bytes)
{
    if (!w || !h || w > KVM_DIM_MAX || h > KVM_DIM_MAX || (uint64_t)w * h > KVM_PIXELS_MAX) return -1;
    if (bytes) *bytes = (size_t)w * h * 4;
    return 0;
}

int kvm_annex_b(const unsigned char *s, size_t n)
{
    if (!s || n < 5 || n > KVM_WIRE_MAX || memcmp(s, "\0\0\0\1", 4) != 0) return -1;
    /* Emulation prevention leaves 00 00 01 only in start codes: each must
     * be the long form. */
    for (size_t i = 4; i + 2 < n; i++) {
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1 && s[i - 1] != 0) return -1;
    }
    return 0;
}

void kvm_parser_clear(struct kvm_parser *p)
{
    free(p->payload);
    memset(p, 0, sizeof *p);
}

int kvm_parser_feed(struct kvm_parser *p, const void *buffer, size_t n, kvm_frame_fn receive, void *opaque)
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
            if (!p->size || p->size > KVM_WIRE_MAX || !(p->payload = malloc(p->size))) goto fail;
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

int kvm_parser_eof(const struct kvm_parser *p)
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
        while (i < n && i < KVM_PREFIX_MAX && s[i] != 0) {
            if (s[i] < 0x20 || s[i] > 0x7e) return -1;
            i++;
        }
        if (i == 0 || i >= n || s[i - 1] != '}') return -1;
    }
    if (kvm_annex_b(s + i, n - i) < 0) return -1;
    *offset = i;
    return 0;
}

static int welcome(struct json_object *o)
{
    int64_t version;
    struct json_object *v;
    if (integer(o, "locachy", KVM_COMPANION_VERSION, KVM_COMPANION_VERSION, &version) < 0 ||
        !(v = field(o, "extended", json_type_boolean)) || !json_object_get_boolean(v)) return -1;
    /* The whole input set, or no session: no mirror or touch-only fallback. */
    if (!(v = field(o, "input", json_type_array)) || json_object_array_length(v) > 16) return -1;
    static const char *const needed[] = { "key", "button", "cancel", "pointer", "scroll" };
    unsigned mask = 0;
    for (size_t i = 0; i < json_object_array_length(v); i++) {
        struct json_object *cap = json_object_array_get_idx(v, i);
        if (!json_object_is_type(cap, json_type_string)) return -1;
        const char *c = json_object_get_string(cap);
        if (strlen(c) != (size_t)json_object_get_string_len(cap)) return -1;
        for (unsigned k = 0; k < sizeof needed / sizeof needed[0]; k++) {
            if (strcmp(c, needed[k]) == 0) mask |= 1u << k;
        }
    }
    return mask == 31 ? 0 : -1;
}

int kvm_control_parse(const unsigned char *s, size_t n, struct kvm_control *out)
{
    memset(out, 0, sizeof *out);
    if (!s || !n || n > KVM_WIRE_MAX) return -1;
    /* OpenDisplay's demux (its section 4), kept in this one place. */
    if (s[0] != '{' || n > KVM_CONTROL_MAX || memchr(s, 0, n)) {
        if (video(s, n, &out->video_offset) < 0) return -1;
        out->type = KVM_MSG_VIDEO;
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
        if (welcome(o) < 0) goto done;
        out->type = KVM_MSG_WELCOME;
    } else if (strcmp(t, "streamConfig") == 0) {
        int64_t w, h, fps;
        if (integer(o, "width", 1, KVM_DIM_MAX, &w) < 0 || integer(o, "height", 1, KVM_DIM_MAX, &h) < 0 ||
            integer(o, "framesPerSecond", 1, KVM_FPS_MAX, &fps) < 0 ||
            kvm_dimensions((uint32_t)w, (uint32_t)h, NULL) < 0 || !(v = field(o, "codec", json_type_string))) {
            goto done;
        }
        const char *codec = json_object_get_string(v);
        out->codec = strcmp(codec, "h264") == 0 ? KVM_CODEC_H264 : strcmp(codec, "hevc") == 0 ? KVM_CODEC_HEVC
                                                                                               : KVM_CODEC_NONE;
        if (out->codec == KVM_CODEC_NONE) goto done;
        out->type = KVM_MSG_CONFIG;
        out->width = (uint32_t)w;
        out->height = (uint32_t)h;
        out->fps = (uint32_t)fps;
    } else if (strcmp(t, "ping") == 0 || strcmp(t, "pong") == 0) {
        out->type = strcmp(t, "ping") == 0 ? KVM_MSG_PING : KVM_MSG_PONG;
        if (json_object_object_get_ex(o, "t", NULL) && integer(o, "t", 0, INT64_MAX, &out->timestamp) < 0) goto done;
    } else if (strcmp(t, "closing") == 0) {
        out->type = KVM_MSG_CLOSING;
    }
    r = 0;
done:
    json_object_put(o);
    return r;
}

static int fits(int n, size_t size) { return n >= 0 && (size_t)n < size ? n : -1; }

static int unit(double v) { return isfinite(v) && v >= 0 && v <= 1; }

int kvm_hello(char *s, size_t size, uint32_t w, uint32_t h, double scale, int h264, int hevc)
{
    if (kvm_dimensions(w, h, NULL) < 0 || !isfinite(scale) || scale < 0.5 || scale > 4 || (!h264 && !hevc)) return -1;
    return fits(snprintf(s, size,
        "{\"type\":\"hello\",\"pv\":3,\"locachy\":%d,\"device\":\"LOCachy\",\"extendedOnly\":true,"
        "\"pixelsWide\":%u,\"pixelsHigh\":%u,\"scale\":%.3f,"
        "\"panel\":{\"pixelsWide\":%u,\"pixelsHigh\":%u,\"scale\":%.3f},"
        "\"displayMaxFrameRate\":%d,\"maxEncodeWide\":%u,\"maxEncodeHigh\":%u,"
        "\"videoCaps\":[%s%s%s],\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\"]}",
        KVM_COMPANION_VERSION, w, h, scale, w, h, scale, KVM_FPS_MAX, KVM_DIM_MAX, KVM_DIM_MAX,
        h264 ? "{\"codec\":\"h264\",\"maxWidth\":4096,\"maxHeight\":4096,\"maxFrameRate\":60}" : "",
        h264 && hevc ? "," : "",
        hevc ? "{\"codec\":\"hevc\",\"maxWidth\":4096,\"maxHeight\":4096,\"maxFrameRate\":60}" : ""), size);
}

int kvm_key_json(char *s, size_t size, uint32_t device, uint32_t code, uint32_t sym, uint32_t mods,
                 uint32_t locks, int state)
{
    if (!device || code > 767 || state < 0 || state > 2 || mods > 127 || locks > 3) return -1;
    return fits(snprintf(s, size,
        "{\"type\":\"key\",\"device\":%u,\"code\":%u,\"keysym\":%u,\"modifiers\":%u,\"locks\":%u,"
        "\"state\":%d,\"mapping\":\"logical\"}", device, code, sym, mods, locks, state), size);
}

int kvm_button_json(char *s, size_t size, uint32_t button, int pressed, double x, double y)
{
    if (button < 272 || button > 274 || (pressed != 0 && pressed != 1) || !unit(x) || !unit(y)) return -1;
    return fits(snprintf(s, size, "{\"type\":\"button\",\"button\":%u,\"state\":%d,\"x\":%.6f,\"y\":%.6f}",
                         button, pressed, x, y), size);
}

int kvm_pointer_json(char *s, size_t size, double x, double y)
{
    if (!unit(x) || !unit(y)) return -1;
    return fits(snprintf(s, size, "{\"type\":\"pointer\",\"x\":%.6f,\"y\":%.6f}", x, y), size);
}

int kvm_scroll_json(char *s, size_t size, double dx, double dy, int phase, int source)
{
    if (!isfinite(dx) || !isfinite(dy) || fabs(dx) > 100000 || fabs(dy) > 100000 ||
        phase < KVM_PHASE_NONE || phase > KVM_PHASE_CANCEL || source < KVM_SOURCE_NONE || source > KVM_SOURCE_TOUCH) {
        return -1;
    }
    return fits(snprintf(s, size, "{\"type\":\"scroll\",\"dx\":%.4f,\"dy\":%.4f,\"phase\":%d,\"source\":%d}",
                         dx, dy, phase, source), size);
}

int kvm_cancel_json(char *s, size_t size)
{
    return fits(snprintf(s, size, "{\"type\":\"cancel\",\"scope\":\"all\"}"), size);
}
