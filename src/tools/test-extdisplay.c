/*
 * The extended-display receiver's protocol and decoder (progs/extdisplay-*.c):
 * bounded framing, the sender handshake, input encoding, and real FFmpeg
 * decoding of an Annex-B stream an encoder made (test-extdisplay.sh). Usage:
 *   test-extdisplay                      the protocol cases
 *   test-extdisplay h264|hevc W H FILE   decode FILE through the receiver's path
 */
#include "extdisplay-decode.h"
#include "extdisplay-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int frames;
static int count(void *unused, const unsigned char *p, size_t n) { (void)unused; (void)p; (void)n; frames++; return 0; }
static int parse(const char *s, struct extdisplay_control *c) { return extdisplay_control_parse((const unsigned char *)s, strlen(s), c); }

static void protocol(void)
{
    struct extdisplay_parser p = { 0 };
    const unsigned char two[] = { 0,0,0,5, 0,0,0,1,0x65, 0,0,0,2, '{','}' };
    for (size_t i = 0; i < sizeof two; i++) CHECK(extdisplay_parser_feed(&p, two + i, 1, count, NULL) == 0);
    CHECK(frames == 2 && extdisplay_parser_eof(&p) == 0);
    CHECK(extdisplay_parser_feed(&p, two, 3, count, NULL) == 0 && extdisplay_parser_eof(&p) < 0); /* cut in the header */
    extdisplay_parser_clear(&p);
    const unsigned char zero[] = { 0,0,0,0 }, huge[] = { 0x00,0x80,0x00,0x01 };
    CHECK(EXTDISPLAY_WIRE_MAX == 8u * 1024u * 1024u);
    CHECK(extdisplay_parser_feed(&p, zero, 4, count, NULL) < 0);
    CHECK(extdisplay_parser_feed(&p, two, 4, count, NULL) < 0); /* poisoned */
    extdisplay_parser_clear(&p);
    CHECK(extdisplay_parser_feed(&p, huge, 4, count, NULL) < 0);
    extdisplay_parser_clear(&p);

    struct extdisplay_control c;
    CHECK(parse("{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\"]}", &c) == 0 && c.type == EXTDISPLAY_MSG_WELCOME);
    CHECK(parse("{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,\"input\":[\"key\",\"button\",\"pointer\",\"scroll\"]}", &c) < 0);
    CHECK(parse("{\"type\":\"welcome\",\"locachy\":1,\"extended\":false,\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\"]}", &c) < 0);
    CHECK(parse("{\"type\":\"welcome\",\"pv\":3}", &c) < 0); /* plain OpenDisplay: no session */
    /* capsLock: welcome.input carries it only when accepted; the Mac's state is exactly {"type","caps"}. */
    CHECK(parse("{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"capsLock\"]}", &c) == 0 &&
          c.type == EXTDISPLAY_MSG_WELCOME && c.input_caps == EXTDISPLAY_INPUT_CAPS_LOCK);
    CHECK(parse("{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"touch\",\"tablet\"]}", &c) == 0 &&
          c.input_caps == (EXTDISPLAY_INPUT_TOUCH | EXTDISPLAY_INPUT_TABLET));
    CHECK(parse("{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\"]}", &c) == 0 && c.input_caps == 0);
    CHECK(parse("{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"capsLock\"]}", &c) == 0 &&
          (c.input_caps & EXTDISPLAY_INPUT_CAPS_LOCK) != 0);
    CHECK(parse("{\"type\":\"capsLockState\",\"caps\":true}", &c) == 0 && c.type == EXTDISPLAY_MSG_CAPS_LOCK_STATE && c.caps_lock == 1);
    CHECK(parse("{\"type\":\"capsLockState\",\"caps\":false}", &c) == 0 && c.type == EXTDISPLAY_MSG_CAPS_LOCK_STATE && c.caps_lock == 0);
    CHECK(parse("{\"type\":\"capsLockState\",\"state\":true}", &c) < 0);
    CHECK(parse("{\"type\":\"capsLockState\",\"caps\":1}", &c) < 0);
    CHECK(parse("{\"type\":\"capsLockState\",\"caps\":\"true\"}", &c) < 0);
    CHECK(parse("{\"type\":\"capsLockState\",\"caps\":true,\"state\":true}", &c) < 0);
    CHECK(parse("{\"type\":\"capsLockState\"}", &c) < 0);
    CHECK(parse("{\"type\":\"capsLockToggle\"}", &c) == 0 && c.type == EXTDISPLAY_MSG_CAPS_LOCK_TOGGLE);
    CHECK(parse("{\"type\":\"capsLockToggle\",\"caps\":true}", &c) < 0);
    CHECK(parse("{\"type\":\"streamConfig\",\"codec\":\"h264\",\"width\":1920,\"height\":1080,\"framesPerSecond\":60}", &c) == 0 &&
          c.type == EXTDISPLAY_MSG_CONFIG && c.codec == EXTDISPLAY_CODEC_H264 && c.width == 1920);
    CHECK(parse("{\"type\":\"streamConfig\",\"codec\":\"hevc\",\"width\":4096,\"height\":4096,\"framesPerSecond\":60}", &c) == 0);
    CHECK(parse("{\"type\":\"streamConfig\",\"codec\":\"hevc420\",\"width\":3840,\"height\":2160,\"framesPerSecond\":60}", &c) == 0 &&
          c.type == EXTDISPLAY_MSG_CONFIG && c.codec == EXTDISPLAY_CODEC_HEVC);
    CHECK(parse("{\"type\":\"streamConfig\",\"codec\":\"hevc444\",\"width\":3840,\"height\":2160,\"framesPerSecond\":60}", &c) == 0 &&
          c.type == EXTDISPLAY_MSG_CONFIG && c.codec == EXTDISPLAY_CODEC_HEVC);
    CHECK(parse("{\"type\":\"streamConfig\",\"codec\":\"h264\",\"width\":99999,\"height\":1,\"framesPerSecond\":60}", &c) < 0);
    CHECK(parse("{\"type\":\"streamConfig\",\"codec\":\"vp9\",\"width\":64,\"height\":64,\"framesPerSecond\":60}", &c) < 0);
    CHECK(parse("{\"type\":\"streamConfig\",\"codec\":\"h264\",\"width\":64.5,\"height\":64,\"framesPerSecond\":60}", &c) < 0);
    CHECK(parse("{\"type\":\"ping\",\"t\":12}", &c) == 0 && c.type == EXTDISPLAY_MSG_PING && c.timestamp == 12);
    CHECK(parse("{\"type\":\"ping\",\"t\":-1}", &c) < 0);
    CHECK(parse("{\"type\":\"cursor\",\"x\":0.5}", &c) == 0 && c.type == EXTDISPLAY_MSG_UNKNOWN);
    CHECK(parse("{\"type\":\"pong\"} trailing", &c) < 0);
    CHECK(parse("{\"type\":\"pong\",\"t\":1,}", &c) < 0);
    const unsigned char nul_type[] = "{\"type\":\"wel\\u0000come\"}";
    CHECK(extdisplay_control_parse(nul_type, sizeof nul_type - 1, &c) < 0 || c.type == EXTDISPLAY_MSG_UNKNOWN);
    const unsigned char prefixed[] = "{\"cap\":1,\"snd\":2}\0\0\0\1\x65\x88";
    CHECK(extdisplay_control_parse(prefixed, sizeof prefixed - 1, &c) == 0 && c.type == EXTDISPLAY_MSG_VIDEO && c.video_offset == strlen("{\"cap\":1,\"snd\":2}"));
    const unsigned char short_code[] = { 0,0,0,1,0x65,0x88,0,0,1,0x41,0x9a };
    CHECK(extdisplay_control_parse(short_code, sizeof short_code, &c) < 0);
    const unsigned char not_video[] = { 0x7f,0,0,0,1 };
    CHECK(extdisplay_control_parse(not_video, sizeof not_video, &c) < 0);

    char s[2048];
    CHECK(extdisplay_hello(s, sizeof s, 1920, 1080, 1.25, 1, 1) > 0 && strstr(s, "\"extendedOnly\":true") &&
          strstr(s, "\"panel\":{\"pixelsWide\":1920") && strstr(s, "\"codec\":\"hevc\""));
    CHECK(extdisplay_hello_named(s, sizeof s, 2160, 1440, 2.0, 1, 1, "cf-fv1\n", "CF-\"FV1\\R ") > 0 &&
          strstr(s, "\"host\":\"cf-fv1\"") && strstr(s, "\"model\":\"CF-FV1R\""));
    CHECK(extdisplay_hello(s, sizeof s, 1920, 1080, 1.0, 1, 0) > 0 && !strstr(s, "hevc"));
    CHECK(extdisplay_hello(s, sizeof s, 0, 1080, 1.0, 1, 0) < 0 && extdisplay_hello(s, sizeof s, 64, 64, 1.0, 0, 0) < 0);
    CHECK(extdisplay_hello(s, 32, 1920, 1080, 1.0, 1, 0) < 0);
    /* Hardware capability mapping: Comet Lake / Gen9.5 (hw H.264 + hw HEVC 4:2:0, no 4:4:4)
     * advertises h264 + hevc420, never legacy "hevc" (which triggers 4:4:4 on older senders). */
    uint32_t cml_caps = extdisplay_select_video_caps(1u | 2u | 4u | 8u, "auto");
    CHECK(cml_caps == (EXTDISPLAY_VCAP_H264 | EXTDISPLAY_VCAP_HEVC420));
    CHECK(extdisplay_hello_full(s, sizeof s, 3840, 2160, 2.0, cml_caps, "yoga", "X1 Yoga",
                                EXTDISPLAY_INPUT_TOUCH | EXTDISPLAY_INPUT_TABLET) > 0 &&
          strstr(s, "\"codec\":\"h264\"") && strstr(s, "\"codec\":\"hevc420\"") &&
          !strstr(s, "\"codec\":\"hevc\"") && !strstr(s, "\"codec\":\"hevc444\"") &&
          strstr(s, "\"touch\"") && strstr(s, "\"tablet\""));
    /* Ice Lake / Gen11+ (hw H.264 + hw HEVC 4:2:0 + hw HEVC 4:4:4) advertises all three + legacy hevc. */
    uint32_t icl_caps = extdisplay_select_video_caps(1u | 2u | 4u | 8u | 16u, "auto");
    CHECK(icl_caps == (EXTDISPLAY_VCAP_H264 | EXTDISPLAY_VCAP_HEVC420 | EXTDISPLAY_VCAP_HEVC444 | EXTDISPLAY_VCAP_HEVC));
    /* Software-only fallback (no VA-API) keeps h264 + hevc420 in auto mode, never legacy 4:4:4 hevc. */
    CHECK(extdisplay_select_video_caps(1u | 2u, "auto") == (EXTDISPLAY_VCAP_H264 | EXTDISPLAY_VCAP_HEVC420));
    /* Explicit sw mode keeps full software codecs including legacy hevc. */
    CHECK(extdisplay_select_video_caps(1u | 2u, "sw") ==
          (EXTDISPLAY_VCAP_H264 | EXTDISPLAY_VCAP_HEVC | EXTDISPLAY_VCAP_HEVC420 | EXTDISPLAY_VCAP_HEVC444));
    CHECK(extdisplay_key_json(s, sizeof s, 5, 30, 0x61, 2, 1, 1) > 0 && strstr(s, "\"code\":30") && strstr(s, "\"state\":1"));
    CHECK(extdisplay_key_json(s, sizeof s, 0, 30, 0x61, 0, 0, 1) < 0 && extdisplay_key_json(s, sizeof s, 1, 30, 0x61, 0, 0, 3) < 0);
    CHECK(extdisplay_button_json(s, sizeof s, 272, 1, 0.5, 1.0) > 0 && extdisplay_button_json(s, sizeof s, 275, 1, 0, 0) < 0);
    CHECK(extdisplay_button_json(s, sizeof s, 272, 1, 1.5, 0) < 0);
    CHECK(extdisplay_pointer_json(s, sizeof s, 0, 1) > 0 && extdisplay_pointer_json(s, sizeof s, -0.1, 0) < 0);
    CHECK(extdisplay_scroll_json(s, sizeof s, -7.5, 0.25, EXTDISPLAY_PHASE_UPDATE, EXTDISPLAY_SOURCE_FINGER) > 0 && strstr(s, "\"dx\":-7.5000"));
    CHECK(extdisplay_scroll_json(s, sizeof s, 0, 0, 5, 1) < 0 && extdisplay_scroll_json(s, sizeof s, 1e9, 0, 2, 1) < 0);
    CHECK(extdisplay_cancel_json(s, sizeof s) > 0 && strcmp(s, "{\"type\":\"cancel\",\"scope\":\"all\"}") == 0);
    /* capsLock is offered in hello.input (and only then), and the offer reads back exactly. */
    uint32_t offered = 0;
    CHECK(extdisplay_hello_full(s, sizeof s, 1920, 1080, 1.0, EXTDISPLAY_VCAP_H264, "h", "m",
                                EXTDISPLAY_INPUT_CAPS_LOCK) > 0 &&
          strstr(s, "\"scroll\",\"capsLock\"]}") && extdisplay_hello_input_caps(s, strlen(s), &offered) == 0 &&
          offered == EXTDISPLAY_INPUT_CAPS_LOCK);
    CHECK(extdisplay_hello_full(s, sizeof s, 1920, 1080, 1.0, EXTDISPLAY_VCAP_H264, "h", "m", 0) > 0 &&
          !strstr(s, "capsLock") && extdisplay_hello_input_caps(s, strlen(s), &offered) == 0 && offered == 0);
    CHECK(extdisplay_hello_input_caps("{\"type\":\"welcome\",\"input\":[\"key\"]}",
                                      sizeof "{\"type\":\"welcome\",\"input\":[\"key\"]}" - 1, &offered) < 0);
    CHECK(extdisplay_hello_input_caps("{\"type\":\"hello\",\"input\":[\"key\",\"button\",\"cancel\",\"pointer\"]}",
                                      sizeof "{\"type\":\"hello\",\"input\":[\"key\",\"button\",\"cancel\",\"pointer\"]}" - 1,
                                      &offered) < 0);
    CHECK(extdisplay_caps_lock_state_json(s, sizeof s, 1) > 0 && strcmp(s, "{\"type\":\"capsLockState\",\"caps\":true}") == 0);
    CHECK(extdisplay_caps_lock_state_json(s, sizeof s, 0) > 0 && strcmp(s, "{\"type\":\"capsLockState\",\"caps\":false}") == 0);
    CHECK(extdisplay_caps_lock_state_json(s, sizeof s, 2) < 0 && extdisplay_caps_lock_state_json(s, 8, 1) < 0);
    CHECK(extdisplay_caps_lock_toggle_json(s, sizeof s) > 0 && strcmp(s, "{\"type\":\"capsLockToggle\"}") == 0);
    CHECK(extdisplay_caps_lock_toggle_json(s, 8) < 0);
    CHECK(extdisplay_input_droppable("{\"type\":\"capsLockState\",\"caps\":true}",
                                     sizeof "{\"type\":\"capsLockState\",\"caps\":true}" - 1) == 0);
    CHECK(extdisplay_input_droppable("{\"type\":\"capsLockToggle\"}", sizeof "{\"type\":\"capsLockToggle\"}" - 1) == 0);
    CHECK(extdisplay_dimensions(4096, 4096, NULL) == 0 && extdisplay_dimensions(4097, 1, NULL) < 0);
}

/* ---- decoding: the encoder's stream as the sender's access units ---- */

static int pictures;
static uint32_t first_pixel, last_pixel;
static int picture(void *unused, const uint32_t *px, uint32_t w, uint32_t h)
{
    (void)unused;
    if (!pictures) first_pixel = px[0];
    last_pixel = px[(size_t)w * h - 1];
    pictures++;
    return 0;
}

struct nal { size_t at, len; };

/* NAL units of an Annex-B byte stream with three- or four-byte codes. */
static size_t nals(const unsigned char *s, size_t n, struct nal *out, size_t max)
{
    size_t count = 0, i = 0, start = 0;
    int open = 0;
    while (i + 3 <= n) {
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
            if (open && count < max) {
                size_t end = i;
                while (end > start && s[end - 1] == 0) end--;
                out[count++] = (struct nal){ start, end - start };
            }
            i += 3;
            start = i;
            open = 1;
        } else {
            i++;
        }
    }
    if (open && count < max && start < n) out[count++] = (struct nal){ start, n - start };
    return count;
}

/* VCL and first slice of a picture, or a unit that starts a new one. */
static void kind(int hevc, const unsigned char *u, size_t len, int *vcl, int *first, int *leading)
{
    int type = hevc ? (u[0] >> 1) & 0x3f : u[0] & 0x1f;
    *vcl = hevc ? type < 32 : type >= 1 && type <= 5;
    *first = *vcl && len > (hevc ? 2u : 1u) && (u[hevc ? 2 : 1] & 0x80);
    *leading = hevc ? (type >= 32 && type <= 35) || type == 39 : type == 6 || type == 7 || type == 8 || type == 9;
}

static int decode(const char *codec, uint32_t w, uint32_t h, const char *path)
{
    int hevc = strcmp(codec, "hevc") == 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    static unsigned char data[8u << 20], au[EXTDISPLAY_WIRE_MAX + 4];
    size_t n = fread(data, 1, sizeof data, f);
    fclose(f);
    static struct nal list[4096];
    size_t count = nals(data, n, list, 4096);

    char config[256];
    snprintf(config, sizeof config,
             "{\"type\":\"streamConfig\",\"codec\":\"%s\",\"width\":%u,\"height\":%u,\"framesPerSecond\":30}", codec, w, h);
    struct extdisplay_control c;
    CHECK(parse(config, &c) == 0 && c.type == EXTDISPLAY_MSG_CONFIG);
    CHECK(extdisplay_decoder_available(c.codec));
    struct extdisplay_decoder *d = extdisplay_decoder_open(c.codec, c.width, c.height);
    CHECK(d != NULL);
    if (!d) return 1;
    size_t used = 0, units = 0;
    int has_vcl = 0;
    for (size_t i = 0; i <= count; i++) {
        int vcl = 0, first = 0, leading = 0;
        if (i < count) kind(hevc, data + list[i].at, list[i].len, &vcl, &first, &leading);
        if (i == count || (has_vcl && (leading || first))) {
            /* One wire payload per access unit, through the worker's demux. */
            CHECK(extdisplay_control_parse(au, used, &c) == 0 && c.type == EXTDISPLAY_MSG_VIDEO);
            CHECK(extdisplay_decoder_push(d, au + c.video_offset, used - c.video_offset, picture, NULL) == 0);
            units++;
            used = 0;
            has_vcl = 0;
        }
        if (i == count) break;
        if (used + 4 + list[i].len > EXTDISPLAY_WIRE_MAX) return 1;
        memcpy(au + used, "\0\0\0\1", 4);
        memcpy(au + used + 4, data + list[i].at, list[i].len);
        used += 4 + list[i].len;
        has_vcl |= vcl;
    }
    extdisplay_decoder_close(d);
    CHECK(units >= 5 && pictures >= (int)units - 1);
    CHECK(first_pixel <= 0x00ffffff && last_pixel <= 0x00ffffff);

    /* A stream of another size than its streamConfig is refused. */
    d = extdisplay_decoder_open(hevc ? EXTDISPLAY_CODEC_HEVC : EXTDISPLAY_CODEC_H264, w * 2, h * 2);
    size_t first_unit = 0;
    for (size_t i = 0; i < count; i++) {
        memcpy(au + first_unit, "\0\0\0\1", 4);
        memcpy(au + first_unit + 4, data + list[i].at, list[i].len);
        first_unit += 4 + list[i].len;
        int vcl, first, leading;
        kind(hevc, data + list[i].at, list[i].len, &vcl, &first, &leading);
        if (vcl) break;
    }
    pictures = 0;
    int r = extdisplay_decoder_push(d, au, first_unit, picture, NULL);
    if (r == 0) r = extdisplay_decoder_push(d, au, first_unit, picture, NULL);
    CHECK(r < 0 && pictures == 0);
    extdisplay_decoder_close(d);

    /* Corrupt slice data: an error or no picture, never a crash. */
    d = extdisplay_decoder_open(hevc ? EXTDISPLAY_CODEC_HEVC : EXTDISPLAY_CODEC_H264, w, h);
    for (size_t i = first_unit / 2; i < first_unit; i++) au[i] = (unsigned char)(au[i] * 33 + 7) | 1;
    extdisplay_decoder_push(d, au, first_unit, picture, NULL);
    extdisplay_decoder_close(d);

    /* Split step + deferred direct render matches push output. */
    d = extdisplay_decoder_open(hevc ? EXTDISPLAY_CODEC_HEVC : EXTDISPLAY_CODEC_H264, w, h);
    size_t out_bytes = 0;
    CHECK(extdisplay_dimensions(w, h, &out_bytes) == 0);
    uint32_t *direct = malloc(out_bytes);
    CHECK(direct != NULL);
    int got = 0;
    used = 0; has_vcl = 0;
    for (size_t i = 0; i <= count; i++) {
        int vcl = 0, first = 0, leading = 0;
        if (i < count) kind(hevc, data + list[i].at, list[i].len, &vcl, &first, &leading);
        if (i == count || (has_vcl && (leading || first))) {
            int step = extdisplay_decoder_step(d, au, used);
            CHECK(step >= 0);
            if (step > 0) {
                uint32_t rw = 0, rh = 0;
                CHECK(extdisplay_decoder_size(d, &rw, &rh) == 1 && rw == w && rh == h);
                /* Without VA-API initialized, 3-slot dmabuf helpers cleanly report software fallback. */
                int dfds[EXTDISPLAY_DMABUF_MAX_PLANES] = { -1, -1, -1, -1 };
                struct extdisplay_dmabuf dinfo = { 0 };
                CHECK(extdisplay_decoder_prepare_dmabuf(d, 0) == 0);
                CHECK(extdisplay_decoder_prepare_dmabuf(d, EXTDISPLAY_DMABUF_SLOTS) == 0);
                CHECK(extdisplay_decoder_export_slot(d, 0, dfds, &dinfo) == 0);
                CHECK(extdisplay_decoder_dmabuf_gen(d) == 0);
                CHECK(extdisplay_decoder_render(d, direct) == 0);
                CHECK(extdisplay_decoder_size(d, &rw, &rh) == 0);
                got++;
                break;
            }
            used = 0; has_vcl = 0;
        }
        if (i == count) break;
        memcpy(au + used, "\0\0\0\1", 4);
        memcpy(au + used + 4, data + list[i].at, list[i].len);
        used += 4 + list[i].len;
        has_vcl |= vcl;
    }
    CHECK(got == 1 && direct[0] <= 0x00ffffff && direct[(size_t)w * h - 1] <= 0x00ffffff);
    free(direct);
    extdisplay_decoder_close(d);
    printf("%s: %s: %zu access units, decoded and checked\n", failures ? "FAIL" : "PASS", codec, units);
    return 0;
}

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include "extdisplay-ipc.h"

/* A memfd of n bytes filled with fill, sealed as the worker seals a picture
 * (seal 0: left open). Its descriptor, or -1. */
static int picture_fd(size_t n, unsigned char fill, int seal)
{
    int fd = memfd_create("extdisplay-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)n) < 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    unsigned char *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return -1;
    }
    memset(p, fill, n);
    munmap(p, n);
    if (seal && fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* One message exactly as given (no magic fix-up), with n descriptors. */
static int send_raw(int s, const struct extdisplay_ipc *m, const int *fds, int n)
{
    union { struct cmsghdr align; char bytes[CMSG_SPACE(2 * sizeof(int))]; } c;
    struct iovec iov = { (void *)m, sizeof *m };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    memset(&c, 0, sizeof c);
    if (n) {
        msg.msg_control = c.bytes;
        msg.msg_controllen = CMSG_SPACE(n * sizeof(int));
        struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
        h->cmsg_level = SOL_SOCKET;
        h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(n * sizeof(int));
        memcpy(CMSG_DATA(h), fds, n * sizeof(int));
    }
    return sendmsg(s, &msg, MSG_NOSIGNAL) == (ssize_t)sizeof *m ? 0 : -1;
}

static void ipc(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
    if (failures) return;
    int sealed = picture_fd(16, 0x5a, 1), open_fd = picture_fd(16, 0x5a, 0);
    struct extdisplay_ipc frame = { .magic = EXTDISPLAY_IPC_MAGIC, .op = EXTDISPLAY_IPC_FRAME, .width = 2, .height = 2, .bytes = 16 }, r;
    int fd = -1;
    /* A sealed 2x2 picture arrives and maps to its bytes. */
    CHECK(send_raw(sv[0], &frame, &sealed, 1) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) == 0 && r.op == EXTDISPLAY_IPC_FRAME && fd >= 0);
    unsigned char *px = fd >= 0 ? extdisplay_frame_map(fd, &r) : NULL;
    CHECK(px != NULL && px[0] == 0x5a && px[15] == 0x5a);
    if (px) munmap(px, 16);
    if (fd >= 0) close(fd);
    /* An open (unsealed) picture is refused at the mapping. */
    CHECK(send_raw(sv[0], &frame, &open_fd, 1) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) == 0 && fd >= 0 && extdisplay_frame_map(fd, &r) == NULL);
    if (fd >= 0) close(fd);
    /* A size that is not the picture's is refused at the mapping. */
    struct extdisplay_ipc big = frame;
    big.bytes = 32;
    CHECK(send_raw(sv[0], &big, &sealed, 1) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) == 0 && fd >= 0 && extdisplay_frame_map(fd, &r) == NULL);
    if (fd >= 0) close(fd);
    /* Two descriptors in one message: the whole message is refused. */
    int two[2] = { sealed, open_fd };
    CHECK(send_raw(sv[0], &frame, two, 2) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) < 0 && fd < 0);
    /* A descriptor on a message that is not a picture is refused. */
    struct extdisplay_ipc input = { .magic = EXTDISPLAY_IPC_MAGIC, .op = EXTDISPLAY_IPC_INPUT, .length = 2 };
    memcpy(input.text, "{}", 2);
    CHECK(send_raw(sv[0], &input, &sealed, 1) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) < 0 && fd < 0);
    /* Wrong magic, and reserved bits set: both refused. */
    struct extdisplay_ipc bad = input;
    bad.magic = 0;
    CHECK(send_raw(sv[0], &bad, NULL, 0) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) < 0);
    bad = input;
    bad.reserved = 1;
    CHECK(send_raw(sv[0], &bad, NULL, 0) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) < 0);
    /* A well-formed input message round-trips; then the socket is empty. */
    CHECK(extdisplay_ipc_send(sv[0], &input) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) == 0 && r.op == EXTDISPLAY_IPC_INPUT && r.length == 2 && fd < 0);
    /* A dma-buf frame message round-trips with one or two descriptors and slot/generation metadata. */
    CHECK(EXTDISPLAY_DMABUF_SLOTS == 3u && sizeof(struct extdisplay_dmabuf) <= EXTDISPLAY_IPC_TEXT);
    struct extdisplay_dmabuf info = {
        .fourcc = 0x34325258u, .num_planes = 1, .num_objects = 1, .slot = 1, .generation = 7,
        .object_index = { 0 }, .pitches = { 8704 }, .offsets = { 0 }, .modifiers = { 0 },
    };
    struct extdisplay_ipc dmabuf = { .magic = EXTDISPLAY_IPC_MAGIC, .op = EXTDISPLAY_IPC_DMABUF,
                                     .width = 2160, .height = 1440, .length = sizeof info };
    memcpy(dmabuf.text, &info, sizeof info);
    CHECK(send_raw(sv[0], &dmabuf, &sealed, 1) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) == 0 && r.op == EXTDISPLAY_IPC_DMABUF && fd >= 0);
    if (fd >= 0) {
        struct extdisplay_dmabuf got_info;
        memcpy(&got_info, r.text, sizeof got_info);
        CHECK(got_info.slot == 1 && got_info.generation == 7 && got_info.num_planes == 1);
        close(fd);
    }
    /* Two-plane (e.g. CCS modifier) dma-buf with 2 objects round-trips via recv_fds. */
    struct extdisplay_dmabuf info2 = {
        .fourcc = 0x34325258u, .num_planes = 2, .num_objects = 2, .slot = 2, .generation = 7,
        .object_index = { 0, 1 }, .pitches = { 15360, 1024 }, .offsets = { 0, 0 },
        .modifiers = { 0x0100000000000004ULL, 0x0100000000000004ULL },
    };
    memcpy(dmabuf.text, &info2, sizeof info2);
    CHECK(send_raw(sv[0], &dmabuf, two, 2) == 0);
    int rfds[EXTDISPLAY_DMABUF_MAX_PLANES] = { -1, -1, -1, -1 };
    unsigned rnfds = 0;
    CHECK(extdisplay_ipc_recv_fds(sv[1], &r, rfds, EXTDISPLAY_DMABUF_MAX_PLANES, &rnfds) == 0 &&
          r.op == EXTDISPLAY_IPC_DMABUF && rnfds == 2 && rfds[0] >= 0 && rfds[1] >= 0);
    for (unsigned i = 0; i < rnfds; i++) if (rfds[i] >= 0) close(rfds[i]);
    /* Cached-slot fast path: zero planes/objects, valid slot & nonzero generation, zero fds. */
    struct extdisplay_dmabuf cached = { .slot = 2, .generation = 7 };
    memcpy(dmabuf.text, &cached, sizeof cached);
    CHECK(send_raw(sv[0], &dmabuf, NULL, 0) == 0);
    CHECK(extdisplay_ipc_recv_fds(sv[1], &r, rfds, EXTDISPLAY_DMABUF_MAX_PLANES, &rnfds) == 0 &&
          r.op == EXTDISPLAY_IPC_DMABUF && rnfds == 0 && rfds[0] < 0);
    {
        struct extdisplay_dmabuf got_cached;
        memcpy(&got_cached, r.text, sizeof got_cached);
        CHECK(got_cached.num_planes == 0 && got_cached.num_objects == 0 &&
              got_cached.slot == 2 && got_cached.generation == 7);
    }
    /* Cached-slot message with unexpected fd attached is refused. */
    CHECK(send_raw(sv[0], &dmabuf, &sealed, 1) == 0);
    CHECK(extdisplay_ipc_recv_fds(sv[1], &r, rfds, EXTDISPLAY_DMABUF_MAX_PLANES, &rnfds) < 0 && rnfds == 0);
    /* Cached-slot message with generation == 0 or out-of-range slot is refused. */
    cached.generation = 0;
    memcpy(dmabuf.text, &cached, sizeof cached);
    CHECK(send_raw(sv[0], &dmabuf, NULL, 0) == 0);
    CHECK(extdisplay_ipc_recv_fds(sv[1], &r, rfds, EXTDISPLAY_DMABUF_MAX_PLANES, &rnfds) < 0);
    cached.generation = 7;
    cached.slot = EXTDISPLAY_DMABUF_SLOTS;
    memcpy(dmabuf.text, &cached, sizeof cached);
    CHECK(send_raw(sv[0], &dmabuf, NULL, 0) == 0);
    CHECK(extdisplay_ipc_recv_fds(sv[1], &r, rfds, EXTDISPLAY_DMABUF_MAX_PLANES, &rnfds) < 0);
    /* Exported dma-buf message with out-of-range slot is refused. */
    info.slot = EXTDISPLAY_DMABUF_SLOTS;
    memcpy(dmabuf.text, &info, sizeof info);
    CHECK(send_raw(sv[0], &dmabuf, &sealed, 1) == 0);
    CHECK(extdisplay_ipc_recv_fds(sv[1], &r, rfds, EXTDISPLAY_DMABUF_MAX_PLANES, &rnfds) < 0 && rnfds == 0);
    /* Object count mismatch (claims 2 objects, sends 1 fd) is refused. */
    memcpy(dmabuf.text, &info2, sizeof info2);
    CHECK(send_raw(sv[0], &dmabuf, &sealed, 1) == 0);
    CHECK(extdisplay_ipc_recv_fds(sv[1], &r, rfds, EXTDISPLAY_DMABUF_MAX_PLANES, &rnfds) < 0 && rnfds == 0);
    /* FRAME_ACK round-trips retired slot+1 (1..3) and 0. */
    for (uint32_t retired = 0; retired <= EXTDISPLAY_DMABUF_SLOTS; retired++) {
        struct extdisplay_ipc ack = { .op = EXTDISPLAY_IPC_FRAME_ACK, .width = retired };
        CHECK(extdisplay_ipc_send(sv[0], &ack) == 0);
        CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) == 0 && r.op == EXTDISPLAY_IPC_FRAME_ACK &&
              r.width == retired && fd < 0);
    }
    /* A refresh request round-trips with zero descriptors and optional active slot+1. */
    struct extdisplay_ipc refresh = { .op = EXTDISPLAY_IPC_REFRESH, .width = 2 };
    CHECK(extdisplay_ipc_send(sv[0], &refresh) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) == 0 && r.op == EXTDISPLAY_IPC_REFRESH && r.width == 2 && fd < 0);
    /* EXTDISPLAY_IPC_FAILED carries an optional ASCII reason string through recv. */
    struct extdisplay_ipc failed = { .op = EXTDISPLAY_IPC_FAILED };
    const char *why = "video-decode-err";
    failed.length = (uint32_t)strlen(why);
    memcpy(failed.text, why, failed.length);
    CHECK(extdisplay_ipc_send(sv[0], &failed) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) == 0 && r.op == EXTDISPLAY_IPC_FAILED &&
          r.length == strlen(why) && memcmp(r.text, why, r.length) == 0 && fd < 0);
    /* capsLock: the worker's state is one absolute boolean without a descriptor, and the
     * frontend's toggle has no payload; anything else is refused. */
    struct extdisplay_ipc caps = { .op = EXTDISPLAY_IPC_CAPS_LOCK_STATE, .width = 1 };
    CHECK(extdisplay_ipc_send(sv[0], &caps) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) == 0 && r.op == EXTDISPLAY_IPC_CAPS_LOCK_STATE && r.width == 1 && fd < 0);
    caps.width = 0;
    CHECK(extdisplay_ipc_send(sv[0], &caps) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) == 0 && r.op == EXTDISPLAY_IPC_CAPS_LOCK_STATE && r.width == 0 && fd < 0);
    caps.width = 2;
    CHECK(extdisplay_ipc_send(sv[0], &caps) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) < 0 && fd < 0);
    caps.width = 1;
    CHECK(send_raw(sv[0], &caps, &sealed, 1) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) < 0 && fd < 0);
    struct extdisplay_ipc toggle = { .op = EXTDISPLAY_IPC_CAPS_LOCK_TOGGLE };
    CHECK(extdisplay_ipc_send(sv[0], &toggle) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) == 0 && r.op == EXTDISPLAY_IPC_CAPS_LOCK_TOGGLE && fd < 0);
    toggle.width = 1;
    CHECK(extdisplay_ipc_send(sv[0], &toggle) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) < 0 && fd < 0);
    /* Malformed dma-buf metadata (wrong length or nonzero bytes) is refused. */
    struct extdisplay_ipc bad_dma = dmabuf;
    bad_dma.length = 4;
    CHECK(send_raw(sv[0], &bad_dma, &sealed, 1) == 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) < 0 && fd < 0);
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) < 0 && errno == EAGAIN);
    close(sv[0]);
    close(sv[1]);
    if (sealed >= 0) close(sealed);
    if (open_fd >= 0) close(open_fd);
}
#endif

#ifdef __linux__
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

/* The real extdisplay-worker, built with EXTDISPLAY_WORKER_TEST_NO_SANDBOX so an
 * unprivileged runner can start it: its fd 3 is our end of a SEQPACKET channel,
 * its fd 4 our end of a stream that stands for the paired peer. */
struct rig { pid_t pid; int ipc, peer; };

static int wait_readable(int fd)
{
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, 5000) == 1 && (p.revents & POLLIN) ? 0 : -1;
}

static int rig_start(const char *path, struct rig *w)
{
    int sv[2], st[2];
    w->pid = -1;
    w->ipc = w->peer = -1;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, st) < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int a = fcntl(sv[1], F_DUPFD, 10), b = fcntl(st[1], F_DUPFD, 10);
        if (a < 0 || b < 0 || dup2(a, EXTDISPLAY_IPC_FD) < 0 || dup2(b, EXTDISPLAY_TRANSPORT_FD) < 0) _exit(127);
        for (int fd = EXTDISPLAY_RENDER_FD; fd < 1024; fd++) close(fd);
        execl(path, "extdisplay-worker", (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    close(st[1]);
    if (pid < 0) {
        close(sv[0]);
        close(st[0]);
        return -1;
    }
    w->pid = pid;
    w->ipc = sv[0];
    w->peer = st[0];
    return 0;
}

static void rig_stop(struct rig *w)
{
    if (w->pid > 0) {
        kill(w->pid, SIGKILL);
        waitpid(w->pid, NULL, 0);
    }
    if (w->ipc >= 0) close(w->ipc);
    if (w->peer >= 0) close(w->peer);
    w->pid = -1;
    w->ipc = w->peer = -1;
}

static int ipc_send_op(struct rig *w, uint32_t op, const char *text, uint32_t width)
{
    struct extdisplay_ipc m = { .op = op, .width = width };
    size_t n = text ? strlen(text) : 0;
    if (n > EXTDISPLAY_IPC_TEXT) return -1;
    m.length = (uint32_t)n;
    if (n) memcpy(m.text, text, n);
    return extdisplay_ipc_send(w->ipc, &m);
}

static int ipc_next(struct rig *w, struct extdisplay_ipc *m)
{
    int fd = -1;
    if (wait_readable(w->ipc) < 0) return -1;
    return extdisplay_ipc_recv(w->ipc, m, &fd) == 0 && fd < 0 ? 0 : -1;
}

static int ipc_expect(struct rig *w, uint32_t op, struct extdisplay_ipc *m)
{
    return ipc_next(w, m) == 0 && m->op == op ? 0 : -1;
}

/* The worker's FAILED with exactly this reason, after any messages it sent first. */
static int ipc_expect_failed(struct rig *w, const char *reason)
{
    struct extdisplay_ipc m;
    for (int i = 0; i < 8; i++) {
        if (ipc_next(w, &m) < 0) return -1;
        if (m.op == EXTDISPLAY_IPC_FAILED) {
            return m.length == strlen(reason) && memcmp(m.text, reason, m.length) == 0 ? 0 : -1;
        }
    }
    return -1;
}

static int peer_send(struct rig *w, const char *json)
{
    size_t n = strlen(json);
    unsigned char frame[4 + 1024];
    if (n == 0 || n > 1024) return -1;
    frame[0] = (unsigned char)(n >> 24);
    frame[1] = (unsigned char)(n >> 16);
    frame[2] = (unsigned char)(n >> 8);
    frame[3] = (unsigned char)n;
    memcpy(frame + 4, json, n);
    return send(w->peer, frame, n + 4, MSG_NOSIGNAL) == (ssize_t)(n + 4) ? 0 : -1;
}

static int read_full(int fd, unsigned char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        if (wait_readable(fd) < 0) return -1;
        ssize_t r = recv(fd, buf + got, n - got, MSG_DONTWAIT);
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* The worker's next message toward the peer, skipping its keepalive pings:
 * its length, or -1. */
static ssize_t peer_next(struct rig *w, char *text, size_t size)
{
    for (int i = 0; i < 8; i++) {
        unsigned char head[4];
        if (read_full(w->peer, head, 4) < 0) return -1;
        size_t n = (size_t)head[0] << 24 | (size_t)head[1] << 16 | (size_t)head[2] << 8 | head[3];
        if (n == 0 || n >= size || read_full(w->peer, (unsigned char *)text, n) < 0) return -1;
        text[n] = '\0';
        if (strncmp(text, "{\"type\":\"ping\"", 14) != 0) return (ssize_t)n;
    }
    return -1;
}

static int rig_hello(const char *path, struct rig *w, uint32_t offer_caps, const char **codec_out)
{
    struct extdisplay_ipc m;
    char hello[EXTDISPLAY_IPC_TEXT], peer_buf[EXTDISPLAY_IPC_TEXT];
    if (rig_start(path, w) < 0 || ipc_expect(w, EXTDISPLAY_IPC_READY, &m) < 0) return -1;
    uint32_t vcaps = (m.width & 1u) ? EXTDISPLAY_VCAP_H264 : (m.width & 2u) ? EXTDISPLAY_VCAP_HEVC : 0u;
    if (!vcaps) return -1;
    if (codec_out) *codec_out = (vcaps & EXTDISPLAY_VCAP_H264) ? "h264" : "hevc";
    if (extdisplay_hello_full(hello, sizeof hello, 320, 240, 1.0, vcaps, "host", "model", offer_caps) < 0) return -1;
    if (ipc_send_op(w, EXTDISPLAY_IPC_HELLO, hello, 0) < 0) return -1;
    return peer_next(w, peer_buf, sizeof peer_buf) > 0 && strcmp(peer_buf, hello) == 0 ? 0 : -1;
}

static void worker_caps(const char *path)
{
    const char *codec = "h264";
    char cfg[192], peer_buf[256];
    struct extdisplay_ipc m;
    struct rig w = { .pid = -1, .ipc = -1, .peer = -1 };

    /* 1. Negotiated Caps Lock: initial state arrives before ACTIVE, later changes follow,
     * and the frontend's toggle reaches the peer verbatim. */
    CHECK(rig_hello(path, &w, EXTDISPLAY_INPUT_CAPS_LOCK, &codec) == 0);
    snprintf(cfg, sizeof cfg,
             "{\"type\":\"streamConfig\",\"codec\":\"%s\",\"width\":320,\"height\":240,\"framesPerSecond\":30}", codec);
    CHECK(peer_send(&w, "{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,"
                        "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"capsLock\"]}") == 0);
    CHECK(peer_send(&w, "{\"type\":\"capsLockState\",\"caps\":true}") == 0);
    CHECK(peer_send(&w, cfg) == 0);
    CHECK(ipc_expect(&w, EXTDISPLAY_IPC_CAPS_LOCK_STATE, &m) == 0 && m.width == 1);
    CHECK(ipc_expect(&w, EXTDISPLAY_IPC_ACTIVE, &m) == 0 && m.width == EXTDISPLAY_INPUT_CAPS_LOCK);
    CHECK(peer_send(&w, "{\"type\":\"capsLockState\",\"caps\":false}") == 0);
    CHECK(ipc_expect(&w, EXTDISPLAY_IPC_CAPS_LOCK_STATE, &m) == 0 && m.width == 0);
    CHECK(ipc_send_op(&w, EXTDISPLAY_IPC_CAPS_LOCK_TOGGLE, NULL, 0) == 0);
    CHECK(peer_next(&w, peer_buf, sizeof peer_buf) > 0 && strcmp(peer_buf, "{\"type\":\"capsLockToggle\"}") == 0);
    /* Frontend sending CAPS_LOCK_STATE toward worker is refused (Mac is sole authority). */
    CHECK(ipc_send_op(&w, EXTDISPLAY_IPC_CAPS_LOCK_STATE, NULL, 1) == 0);
    CHECK(ipc_expect_failed(&w, "frontend-err") == 0);
    rig_stop(&w);

    /* 2. Reconnect: a fresh session receives the latest pre-streamConfig state before ACTIVE. */
    CHECK(rig_hello(path, &w, EXTDISPLAY_INPUT_CAPS_LOCK, &codec) == 0);
    snprintf(cfg, sizeof cfg,
             "{\"type\":\"streamConfig\",\"codec\":\"%s\",\"width\":320,\"height\":240,\"framesPerSecond\":30}", codec);
    CHECK(peer_send(&w, "{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,"
                        "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"capsLock\"]}") == 0);
    CHECK(peer_send(&w, "{\"type\":\"capsLockState\",\"caps\":true}") == 0);
    CHECK(peer_send(&w, "{\"type\":\"capsLockState\",\"caps\":false}") == 0);
    CHECK(peer_send(&w, cfg) == 0);
    CHECK(ipc_expect(&w, EXTDISPLAY_IPC_CAPS_LOCK_STATE, &m) == 0 && m.width == 0);
    CHECK(ipc_expect(&w, EXTDISPLAY_IPC_ACTIVE, &m) == 0 && m.width == EXTDISPLAY_INPUT_CAPS_LOCK);
    rig_stop(&w);

    /* 3. Negotiated peer skipping initial capsLockState before streamConfig fails config-err. */
    CHECK(rig_hello(path, &w, EXTDISPLAY_INPUT_CAPS_LOCK, &codec) == 0);
    CHECK(peer_send(&w, "{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,"
                        "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"capsLock\"]}") == 0);
    CHECK(peer_send(&w, cfg) == 0);
    CHECK(ipc_expect_failed(&w, "config-err") == 0);
    rig_stop(&w);

    /* 4. Peer sending capsLockState before welcome fails parse-err. */
    CHECK(rig_hello(path, &w, EXTDISPLAY_INPUT_CAPS_LOCK, &codec) == 0);
    CHECK(peer_send(&w, "{\"type\":\"capsLockState\",\"caps\":true}") == 0);
    CHECK(ipc_expect_failed(&w, "parse-err") == 0);
    rig_stop(&w);

    /* 5. Peer sending receiver-only capsLockToggle fails parse-err. */
    CHECK(rig_hello(path, &w, EXTDISPLAY_INPUT_CAPS_LOCK, &codec) == 0);
    CHECK(peer_send(&w, "{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,"
                        "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"capsLock\"]}") == 0);
    CHECK(peer_send(&w, "{\"type\":\"capsLockToggle\"}") == 0);
    CHECK(ipc_expect_failed(&w, "parse-err") == 0);
    rig_stop(&w);

    /* 6. Legacy peer (welcome omits capsLock): streamConfig works with no capsLockState,
     * ACTIVE clears EXTDISPLAY_INPUT_CAPS_LOCK, and unsolicited capsLockState fails parse-err. */
    CHECK(rig_hello(path, &w, EXTDISPLAY_INPUT_CAPS_LOCK, &codec) == 0);
    CHECK(peer_send(&w, "{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,"
                        "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\"]}") == 0);
    CHECK(peer_send(&w, cfg) == 0);
    CHECK(ipc_expect(&w, EXTDISPLAY_IPC_ACTIVE, &m) == 0 && m.width == 0);
    CHECK(peer_send(&w, "{\"type\":\"capsLockState\",\"caps\":true}") == 0);
    CHECK(ipc_expect_failed(&w, "parse-err") == 0);
    rig_stop(&w);

    /* 7. Asymmetric offer (receiver omits capsLock, peer lists it): intersection clears
     * capsLock; streamConfig succeeds without capsLockState; frontend toggle fails frontend-err. */
    CHECK(rig_hello(path, &w, 0, &codec) == 0);
    CHECK(peer_send(&w, "{\"type\":\"welcome\",\"locachy\":1,\"extended\":true,"
                        "\"input\":[\"key\",\"button\",\"cancel\",\"pointer\",\"scroll\",\"capsLock\"]}") == 0);
    CHECK(peer_send(&w, cfg) == 0);
    CHECK(ipc_expect(&w, EXTDISPLAY_IPC_ACTIVE, &m) == 0 && m.width == 0);
    CHECK(ipc_send_op(&w, EXTDISPLAY_IPC_CAPS_LOCK_TOGGLE, NULL, 0) == 0);
    CHECK(ipc_expect_failed(&w, "frontend-err") == 0);
    rig_stop(&w);
}
#endif

int main(int argc, char **argv)
{
    if (argc == 5) {
        if (decode(argv[1], (uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3]), argv[4]) != 0) failures++;
#ifdef __linux__
    } else if (argc == 3 && strcmp(argv[1], "worker") == 0) {
        worker_caps(argv[2]);
        printf("%s: extdisplay worker Caps Lock cases\n", failures ? "FAIL" : "PASS");
#endif
    } else {
        protocol();
        printf("%s: extdisplay protocol cases\n", failures ? "FAIL" : "PASS");
#ifdef __linux__
        ipc();
        printf("%s: extdisplay IPC boundary cases\n", failures ? "FAIL" : "PASS");
#endif
    }
    return failures ? 1 : 0;
}
