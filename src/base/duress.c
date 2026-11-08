/*
 * Duress password verifier and cryptographic wipe (duress.h).
 *
 * Self-contained SHA-256 + HMAC-SHA256 + PBKDF2-HMAC-SHA256 (600,000 rounds,
 * 16-byte random salt, 32-byte tag, constant-time compare) with no external
 * library dependencies, so process 1 (musl static in initramfs), lock,
 * settings and PAM modules share the exact same implementation.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#include "duress.h"

#define DURESS_ITERATIONS 600000u
#define DURESS_SALT_BYTES 16
#define DURESS_TAG_BYTES  32
/* Default LUKS2 binary+JSON metadata + keyslots area is 16 MiB; wiping 32 MiB
 * destroys both primary/secondary headers and every keyslot (passphrase + TPM). */
#define DURESS_WIPE_BYTES (32u * 1024u * 1024u)

static void wipe_mem(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* ---- SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 (FIPS 180-4, RFC 8018) ---- */

struct sha256_ctx {
    uint32_t state[8];
    uint64_t count;
    uint8_t buf[64];
};

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t st[8], const uint8_t blk[64])
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16) |
               ((uint32_t)blk[i * 4 + 2] << 8) | (uint32_t)blk[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    wipe_mem(w, sizeof w);
}

static void sha256_init(struct sha256_ctx *c)
{
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->count = 0;
}

static void sha256_update(struct sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t idx = (size_t)(c->count & 63u);
    c->count += len;
    if (idx) {
        size_t take = 64 - idx < len ? 64 - idx : len;
        memcpy(c->buf + idx, p, take);
        if (idx + take < 64) return;
        sha256_block(c->state, c->buf);
        p += take;
        len -= take;
    }
    while (len >= 64) {
        sha256_block(c->state, p);
        p += 64;
        len -= 64;
    }
    if (len) memcpy(c->buf, p, len);
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->count * 8;
    uint8_t pad = 0x80, zero = 0, len_be[8];
    sha256_update(c, &pad, 1);
    while ((c->count & 63u) != 56) sha256_update(c, &zero, 1);
    for (int i = 0; i < 8; i++) len_be[i] = (uint8_t)(bits >> ((7 - i) * 8));
    sha256_update(c, len_be, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->state[i];
    }
    wipe_mem(c, sizeof *c);
}

static void pbkdf2_sha256(const char *pass, size_t pass_len, const uint8_t *salt, size_t salt_len,
                          uint32_t rounds, uint8_t out[32])
{
    uint8_t key[64] = { 0 }, ipad[64], opad[64], u[32], t[32], blk[4] = { 0, 0, 0, 1 };
    struct sha256_ctx ictx, octx, c;
    if (pass_len > 64) {
        sha256_init(&c);
        sha256_update(&c, pass, pass_len);
        sha256_final(&c, key);
    } else {
        memcpy(key, pass, pass_len);
    }
    for (int i = 0; i < 64; i++) {
        ipad[i] = key[i] ^ 0x36u;
        opad[i] = key[i] ^ 0x5cu;
    }
    sha256_init(&ictx);
    sha256_update(&ictx, ipad, 64);
    sha256_init(&octx);
    sha256_update(&octx, opad, 64);

    c = ictx;
    sha256_update(&c, salt, salt_len);
    sha256_update(&c, blk, 4);
    sha256_final(&c, u);
    c = octx;
    sha256_update(&c, u, 32);
    sha256_final(&c, u);
    memcpy(t, u, 32);

    for (uint32_t r = 1; r < rounds; r++) {
        c = ictx;
        sha256_update(&c, u, 32);
        sha256_final(&c, u);
        c = octx;
        sha256_update(&c, u, 32);
        sha256_final(&c, u);
        for (int j = 0; j < 32; j++) t[j] ^= u[j];
    }
    memcpy(out, t, 32);
    wipe_mem(key, sizeof key);
    wipe_mem(ipad, sizeof ipad);
    wipe_mem(opad, sizeof opad);
    wipe_mem(u, sizeof u);
    wipe_mem(t, sizeof t);
    wipe_mem(&ictx, sizeof ictx);
    wipe_mem(&octx, sizeof octx);
}

static int hex_decode(const char *hex, uint8_t *out, size_t nbytes)
{
    if (strlen(hex) != nbytes * 2) return -1;
    for (size_t i = 0; i < nbytes; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[i * 2 + k];
            unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                         c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
                         c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16u;
            if (d == 16u) return -1;
            v = (v << 4) | d;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int parse_verifier(uint32_t *rounds, uint8_t salt[DURESS_SALT_BYTES], uint8_t tag[DURESS_TAG_BYTES])
{
    int fd = open(DURESS_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    char line[256] = "";
    ssize_t n = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? read(fd, line, sizeof line - 1) : -1;
    close(fd);
    if (n <= 0) return -1;
    line[n] = '\0';
    line[strcspn(line, "\r\n")] = '\0';
    unsigned long r = 0;
    char s_hex[DURESS_SALT_BYTES * 2 + 2] = "", t_hex[DURESS_TAG_BYTES * 2 + 2] = "";
    int end = 0;
    if (sscanf(line, "v1:%lu:%33[^:]:%65s%n", &r, s_hex, t_hex, &end) != 3 || line[end] != '\0' ||
        r < 10000u || r > 5000000u ||
        hex_decode(s_hex, salt, DURESS_SALT_BYTES) < 0 ||
        hex_decode(t_hex, tag, DURESS_TAG_BYTES) < 0) {
        return -1;
    }
    *rounds = (uint32_t)r;
    return 0;
}

int duress_configured(void)
{
    uint32_t rounds;
    uint8_t salt[DURESS_SALT_BYTES], tag[DURESS_TAG_BYTES];
    int ok = parse_verifier(&rounds, salt, tag) == 0;
    wipe_mem(salt, sizeof salt);
    wipe_mem(tag, sizeof tag);
    return ok;
}

static int fill_random(void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t r = getrandom(p, len, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            if (fd < 0) return -1;
            ssize_t u = read(fd, p, len);
            close(fd);
            return u == (ssize_t)len ? 0 : -1;
        }
        p += (size_t)r;
        len -= (size_t)r;
    }
    return 0;
}

int duress_set(const char *secret)
{
    if (!secret || strlen(secret) < DURESS_MIN_LENGTH) return -1;
    uint8_t salt[DURESS_SALT_BYTES], tag[DURESS_TAG_BYTES];
    if (fill_random(salt, sizeof salt) < 0) return -1;
    pbkdf2_sha256(secret, strlen(secret), salt, sizeof salt, DURESS_ITERATIONS, tag);

    char tmp[512], dir[512], hex_s[DURESS_SALT_BYTES * 2 + 1], hex_t[DURESS_TAG_BYTES * 2 + 1];
    for (int i = 0; i < DURESS_SALT_BYTES; i++) snprintf(hex_s + i * 2, 3, "%02x", salt[i]);
    for (int i = 0; i < DURESS_TAG_BYTES; i++) snprintf(hex_t + i * 2, 3, "%02x", tag[i]);
    wipe_mem(salt, sizeof salt);
    wipe_mem(tag, sizeof tag);

    snprintf(dir, sizeof dir, "%s", DURESS_FILE);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        mkdir(dir, 0755);
    }
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", DURESS_FILE, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        wipe_mem(hex_t, sizeof hex_t);
        return -1;
    }
    int ok = fchmod(fd, 0600) == 0 && dprintf(fd, "v1:%u:%s:%s\n", DURESS_ITERATIONS, hex_s, hex_t) > 0 &&
             fsync(fd) == 0;
    close(fd);
    wipe_mem(hex_t, sizeof hex_t);
    if (!ok || rename(tmp, DURESS_FILE) < 0) {
        unlink(tmp);
        return -1;
    }
    int dfd = open(slash && slash != dir ? dir : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return 0;
}

int duress_clear(void)
{
    if (unlink(DURESS_FILE) < 0 && errno != ENOENT) return -1;
    return 0;
}

int duress_matches(const char *secret)
{
    if (!secret || !secret[0]) return 0;
    uint32_t rounds = 0;
    uint8_t salt[DURESS_SALT_BYTES], expected[DURESS_TAG_BYTES], got[DURESS_TAG_BYTES];
    if (parse_verifier(&rounds, salt, expected) < 0) return 0;
    pbkdf2_sha256(secret, strlen(secret), salt, sizeof salt, rounds, got);
    unsigned diff = 0;
    for (size_t i = 0; i < DURESS_TAG_BYTES; i++) diff |= expected[i] ^ got[i];
    wipe_mem(salt, sizeof salt);
    wipe_mem(expected, sizeof expected);
    wipe_mem(got, sizeof got);
    return diff == 0;
}

/* ---- LUKS2 partitions and their destruction ---- */

static int is_locachy_luks(const char *dev)
{
    uint8_t hdr[72];
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = pread(fd, hdr, sizeof hdr, 0);
    close(fd);
    if (n != (ssize_t)sizeof hdr || memcmp(hdr, "LUKS\xba\xbe", 6) != 0 || hdr[6] != 0 || hdr[7] != 2) return 0;
    hdr[71] = 0; /* the 48-byte label at 24, NUL-padded */
    const char *label = (const char *)(hdr + 24);
    return strcmp(label, "locachy-crypt") == 0 || strcmp(label, "locachy-swap-crypt") == 0;
}

/* Each LOCachy LUKS2 partition's device path, through fn; the count. */
static int each_luks(void (*fn)(const char *dev))
{
    DIR *d = opendir(DURESS_SYS_BLOCK);
    struct dirent *e;
    int found = 0;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char dev[160];
        snprintf(dev, sizeof dev, "%s/%.64s", DURESS_DEV_DIR, e->d_name);
        if (is_locachy_luks(dev)) {
            found++;
            if (fn) fn(dev);
        }
    }
    if (d) closedir(d);
    return found;
}

int duress_has_luks(void)
{
    return each_luks(NULL) > 0;
}

static char (*path_out)[160];
static int path_max, path_n;

static void collect_path(const char *dev)
{
    if (path_n < path_max) {
        snprintf(path_out[path_n++], 160, "%s", dev);
    }
}

int duress_luks_paths(char paths[][160], int max)
{
    path_out = paths;
    path_max = max;
    path_n = 0;
    each_luks(collect_path);
    return path_n;
}

/* Where the encrypted data starts: segment 0's "offset" in the header's
 * JSON (at 4096), at least 16 MiB (LUKS2's default) and at most 1 GiB. All
 * key slots lie before it; everything up to it is overwritten. */
static uint64_t data_offset(int fd)
{
    static char json[12288];
    uint64_t offset = 16u * 1024u * 1024u;
    ssize_t n = pread(fd, json, sizeof json - 1, 4096);
    if (n > 0) {
        json[n] = '\0';
        const char *seg = strstr(json, "\"segments\"");
        const char *at = seg ? strstr(seg, "\"offset\":\"") : NULL;
        if (at) {
            unsigned long long v = strtoull(at + 10, NULL, 10);
            if (v > offset && v <= 1024ull * 1024u * 1024u) offset = v;
        }
    }
    return offset;
}

static void destroy_header(const char *dev)
{
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    uint64_t size = 0, end = data_offset(fd);
    struct stat st;
    if (ioctl(fd, BLKGETSIZE64, &size) < 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        size = (uint64_t)st.st_size;
    }
    if (size && end > size) end = size;
    uint64_t range[2] = { 0, end };
    if (ioctl(fd, BLKDISCARD, range) < 0) {
        /* not every device discards; the overwrite below is what counts */
    }
    static uint8_t buf[65536];
    for (int pass = 0; pass < 2; pass++) {
        for (uint64_t off = 0; off < end; off += sizeof buf) {
            size_t chunk = (size_t)(end - off < sizeof buf ? end - off : sizeof buf);
            if (pass == 0) {
                if (fill_random(buf, chunk) < 0) memset(buf, 0xA5, chunk);
            } else {
                memset(buf, 0, chunk);
            }
            if (pwrite(fd, buf, chunk, (off_t)off) < 0) break;
        }
        if (fdatasync(fd) < 0) {
            /* a device without a cache to flush */
        }
    }
    wipe_mem(buf, sizeof buf);
    if (ioctl(fd, BLKFLSBUF, 0) < 0) {
        /* a regular file in tests */
    }
    close(fd);
}

static void shred_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 65536) {
        uint8_t zero[4096] = { 0 };
        for (off_t off = 0; off < st.st_size; off += (off_t)sizeof zero) {
            size_t n = (size_t)(st.st_size - off < (off_t)sizeof zero ? st.st_size - off : (off_t)sizeof zero);
            if (pwrite(fd, zero, n, off) < 0) break;
        }
        if (fsync(fd) < 0) {
            /* unlinked below either way */
        }
    }
    if (fd >= 0) close(fd);
    unlink(path);
}

void duress_wipe(void)
{
    sigset_t all;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, NULL);
    /* The headers first: the keyslots hold the passphrase and the TPM's
     * sealed key, so with them gone nothing else matters. The TPM is left
     * alone (a clear would also cost any other system's sealed keys). */
    if (each_luks(destroy_header) == 0) {
        sigprocmask(SIG_UNBLOCK, &all, NULL);
        return;
    }
    static const char *const keys[] = { DURESS_FILE, DURESS_KEY_PCR, DURESS_KEY_PK, DURESS_KEY_KEK, DURESS_KEY_DB };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        shred_file(keys[i]);
    }
    sync();
#ifndef DURESS_TEST_NO_POWEROFF
    reboot(RB_POWER_OFF);
    reboot(RB_HALT_SYSTEM);
    for (;;) pause();
#endif
}
