/*
 * The extended-display receiver's protocol and decoder (progs/extdisplay-*.c):
 * bounded framing, the companion handshake, input encoding, and real FFmpeg
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
    const unsigned char zero[] = { 0,0,0,0 }, huge[] = { 0x00,0x40,0x00,0x01 };
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
    CHECK(parse("{\"type\":\"streamConfig\",\"codec\":\"h264\",\"width\":1920,\"height\":1080,\"framesPerSecond\":60}", &c) == 0 &&
          c.type == EXTDISPLAY_MSG_CONFIG && c.codec == EXTDISPLAY_CODEC_H264 && c.width == 1920);
    CHECK(parse("{\"type\":\"streamConfig\",\"codec\":\"hevc\",\"width\":4096,\"height\":4096,\"framesPerSecond\":60}", &c) == 0);
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
    CHECK(extdisplay_hello(s, sizeof s, 1920, 1080, 1.0, 1, 0) > 0 && !strstr(s, "hevc"));
    CHECK(extdisplay_hello(s, sizeof s, 0, 1080, 1.0, 1, 0) < 0 && extdisplay_hello(s, sizeof s, 64, 64, 1.0, 0, 0) < 0);
    CHECK(extdisplay_hello(s, 32, 1920, 1080, 1.0, 1, 0) < 0);
    CHECK(extdisplay_key_json(s, sizeof s, 5, 30, 0x61, 2, 1, 1) > 0 && strstr(s, "\"code\":30") && strstr(s, "\"state\":1"));
    CHECK(extdisplay_key_json(s, sizeof s, 0, 30, 0x61, 0, 0, 1) < 0 && extdisplay_key_json(s, sizeof s, 1, 30, 0x61, 0, 0, 3) < 0);
    CHECK(extdisplay_button_json(s, sizeof s, 272, 1, 0.5, 1.0) > 0 && extdisplay_button_json(s, sizeof s, 275, 1, 0, 0) < 0);
    CHECK(extdisplay_button_json(s, sizeof s, 272, 1, 1.5, 0) < 0);
    CHECK(extdisplay_pointer_json(s, sizeof s, 0, 1) > 0 && extdisplay_pointer_json(s, sizeof s, -0.1, 0) < 0);
    CHECK(extdisplay_scroll_json(s, sizeof s, -7.5, 0.25, EXTDISPLAY_PHASE_UPDATE, EXTDISPLAY_SOURCE_FINGER) > 0 && strstr(s, "\"dx\":-7.5000"));
    CHECK(extdisplay_scroll_json(s, sizeof s, 0, 0, 5, 1) < 0 && extdisplay_scroll_json(s, sizeof s, 1e9, 0, 2, 1) < 0);
    CHECK(extdisplay_cancel_json(s, sizeof s) > 0 && strcmp(s, "{\"type\":\"cancel\",\"scope\":\"all\"}") == 0);
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
    CHECK(extdisplay_ipc_recv(sv[1], &r, &fd) < 0 && errno == EAGAIN);
    close(sv[0]);
    close(sv[1]);
    if (sealed >= 0) close(sealed);
    if (open_fd >= 0) close(open_fd);
}
#endif

int main(int argc, char **argv)
{
    if (argc == 5) {
        if (decode(argv[1], (uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3]), argv[4]) != 0) failures++;
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
