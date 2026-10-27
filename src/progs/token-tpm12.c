/*
 * token-tpm12.c (/usr/lib/cryptsetup/libcryptsetup-token-locachy-tpm12.so):
 * LUKS2 external token plugin (CRYPTSETUP_TOKEN_1.0) for TPM 1.2 hardware-bound
 * disk keys (such as ThinkPad X230).
 *
 * Performs TCG TPM 1.2 OIAP + TPM_Unseal (0x00000018) directly over /dev/tpm0
 * (built-in tpm_tis driver), using OpenSSL libcrypto (already linked by
 * cryptsetup) for SHA-1 and HMAC-SHA1. Activate with standard:
 *   cryptsetup open --type luks --token-only <device> <name>
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <libcryptsetup.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int hex_decode(const char *hex, uint8_t *out, size_t max_out, size_t *out_len)
{
    size_t n = hex ? strlen(hex) : 0;
    if (n < 64 || (n & 1) != 0 || n / 2 > max_out) return -EINVAL;
    for (size_t i = 0; i < n; i += 2) {
        if (!isxdigit((unsigned char)hex[i]) || !isxdigit((unsigned char)hex[i + 1])) return -EINVAL;
        unsigned int v = 0;
        char pair[3] = { hex[i], hex[i + 1], '\0' };
        v = (unsigned int)strtoul(pair, NULL, 16);
        out[i / 2] = (uint8_t)v;
    }
    *out_len = n / 2;
    return 0;
}

static void put_be16(uint8_t *p, uint16_t v)
{
    uint16_t b = htons(v);
    memcpy(p, &b, 2);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    uint32_t b = htonl(v);
    memcpy(p, &b, 4);
}

static uint32_t get_be32(const uint8_t *p)
{
    uint32_t b;
    memcpy(&b, p, 4);
    return ntohl(b);
}

static int tpm12_tx(int fd, const uint8_t *req, size_t req_len, uint8_t *resp, size_t max_resp,
                    uint32_t *rc_out, size_t *body_len)
{
    if (write(fd, req, req_len) != (ssize_t)req_len) return -EIO;
    ssize_t n = read(fd, resp, max_resp);
    if (n < 10) return -EIO;
    uint32_t total = get_be32(resp + 2);
    uint32_t rc = get_be32(resp + 6);
    if (total < 10 || (size_t)n < total) return -EIO;
    *rc_out = rc;
    *body_len = total - 10;
    return 0;
}

static int tpm12_oiap(int fd, uint32_t *handle, uint8_t nonce_even[20])
{
    uint8_t req[10], resp[128];
    put_be16(req, 0x00C1);
    put_be32(req + 2, 10);
    put_be32(req + 6, 0x0000000A); /* TPM_ORD_OIAP */
    uint32_t rc = 0;
    size_t blen = 0;
    int r = tpm12_tx(fd, req, sizeof req, resp, sizeof resp, &rc, &blen);
    if (r < 0) return r;
    if (rc != 0 || blen < 24) return -EPERM;
    *handle = get_be32(resp + 10);
    memcpy(nonce_even, resp + 14, 20);
    return 0;
}

static void auth_hmac_zero(const uint8_t in_digest[20], const uint8_t neven[20],
                           const uint8_t nodd[20], uint8_t out_mac[20])
{
    static const uint8_t zero_key[20] = { 0 };
    uint8_t msg[61];
    memcpy(msg, in_digest, 20);
    memcpy(msg + 20, neven, 20);
    memcpy(msg + 40, nodd, 20);
    msg[60] = 0x00; /* continueAuthSession = FALSE */
    unsigned int mlen = 0;
    HMAC(EVP_sha1(), zero_key, sizeof zero_key, msg, sizeof msg, out_mac, &mlen);
}

static int tpm12_unseal(const uint8_t *stored, size_t stored_len, uint8_t *secret, size_t max_secret,
                        size_t *secret_len)
{
    const char *dev = getenv("LOCACHY_TPM_DEV");
    if (!dev || !dev[0]) dev = "/dev/tpm0";
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -EAGAIN;

    /* Ensure TPM_Startup(CLEAR) was issued (0x26 = already started). */
    {
        uint8_t sreq[12], sresp[64];
        put_be16(sreq, 0x00C1);
        put_be32(sreq + 2, 12);
        put_be32(sreq + 6, 0x00000099);
        put_be16(sreq + 10, 0x0001);
        uint32_t src = 0;
        size_t sblen = 0;
        tpm12_tx(fd, sreq, sizeof sreq, sresp, sizeof sresp, &src, &sblen);
    }

    uint32_t ah1 = 0, ah2 = 0;
    uint8_t neven1[20], neven2[20], nodd1[20], nodd2[20];
    int r = tpm12_oiap(fd, &ah1, neven1);
    if (r == 0) r = tpm12_oiap(fd, &ah2, neven2);
    if (r < 0 || RAND_bytes(nodd1, 20) != 1 || RAND_bytes(nodd2, 20) != 1) {
        close(fd);
        return r < 0 ? r : -EIO;
    }

    /* inParamDigest = SHA1(ordinal=0x00000018 || inData=stored) */
    uint8_t ord_be[4], in_digest[20], mac1[20], mac2[20];
    put_be32(ord_be, 0x00000018);
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (!md) {
        close(fd);
        return -ENOMEM;
    }
    EVP_DigestInit_ex(md, EVP_sha1(), NULL);
    EVP_DigestUpdate(md, ord_be, 4);
    EVP_DigestUpdate(md, stored, stored_len);
    EVP_DigestFinal_ex(md, in_digest, NULL);
    EVP_MD_CTX_free(md);

    auth_hmac_zero(in_digest, neven1, nodd1, mac1);
    auth_hmac_zero(in_digest, neven2, nodd2, mac2);

    size_t req_len = 14 + stored_len + 2 * 45;
    uint8_t *req = malloc(req_len);
    if (!req) {
        close(fd);
        return -ENOMEM;
    }
    put_be16(req, 0x00C3); /* TPM_TAG_RQU_AUTH2_COMMAND */
    put_be32(req + 2, (uint32_t)req_len);
    put_be32(req + 6, 0x00000018); /* TPM_ORD_Unseal */
    put_be32(req + 10, 0x40000000); /* TPM_KH_SRK */
    memcpy(req + 14, stored, stored_len);

    uint8_t *t1 = req + 14 + stored_len;
    put_be32(t1, ah1);
    memcpy(t1 + 4, nodd1, 20);
    t1[24] = 0x00;
    memcpy(t1 + 25, mac1, 20);

    uint8_t *t2 = t1 + 45;
    put_be32(t2, ah2);
    memcpy(t2 + 4, nodd2, 20);
    t2[24] = 0x00;
    memcpy(t2 + 25, mac2, 20);

    uint8_t resp[2048];
    uint32_t rc = 0;
    size_t blen = 0;
    r = tpm12_tx(fd, req, req_len, resp, sizeof resp, &rc, &blen);
    free(req);
    close(fd);
    if (r < 0) return r;
    if (rc != 0 || blen < 4 + 82) {
        explicit_bzero(resp, sizeof resp);
        return -EPERM;
    }
    uint32_t out_len = get_be32(resp + 10);
    if (out_len == 0 || out_len > max_secret || 4 + (size_t)out_len + 82 > blen) {
        explicit_bzero(resp, sizeof resp);
        return -EPERM;
    }
    memcpy(secret, resp + 14, out_len);
    *secret_len = out_len;
    explicit_bzero(resp, sizeof resp);
    return 0;
}

const char *cryptsetup_token_version(void)
{
    return "1.0";
}

int cryptsetup_token_validate(struct crypt_device *cd, const char *json)
{
    (void)cd;
    if (!json) return -EINVAL;
    json_object *root = json_tokener_parse(json);
    if (!root) return -EINVAL;
    json_object *jtype = NULL, *jblob = NULL, *jslots = NULL;
    uint8_t tmp[2048];
    size_t tlen = 0;
    int ok = json_object_object_get_ex(root, "type", &jtype) &&
             json_object_is_type(jtype, json_type_string) &&
             strcmp(json_object_get_string(jtype), "locachy-tpm12") == 0 &&
             json_object_object_get_ex(root, "keyslots", &jslots) &&
             json_object_is_type(jslots, json_type_array) &&
             json_object_object_get_ex(root, "blob", &jblob) &&
             json_object_is_type(jblob, json_type_string) &&
             hex_decode(json_object_get_string(jblob), tmp, sizeof tmp, &tlen) == 0;
    json_object_put(root);
    return ok ? 0 : -EINVAL;
}

void cryptsetup_token_dump(struct crypt_device *cd, const char *json)
{
    if (!json) return;
    json_object *root = json_tokener_parse(json);
    if (!root) return;
    json_object *jpcrs = NULL;
    char pcrs[64] = "";
    if (json_object_object_get_ex(root, "pcrs", &jpcrs) && json_object_is_type(jpcrs, json_type_array)) {
        size_t pos = 0, len = json_object_array_length(jpcrs);
        for (size_t i = 0; i < len && pos + 8 < sizeof pcrs; i++) {
            json_object *el = json_object_array_get_idx(jpcrs, i);
            int n = snprintf(pcrs + pos, sizeof pcrs - pos, "%s%d", i ? "+" : "", json_object_get_int(el));
            if (n > 0) pos += (size_t)n;
        }
    }
    crypt_log(cd, CRYPT_LOG_NORMAL, "\ttpm12-pcrs:   ");
    crypt_log(cd, CRYPT_LOG_NORMAL, pcrs[0] ? pcrs : "SRK");
    crypt_log(cd, CRYPT_LOG_NORMAL, "\n");
    json_object_put(root);
}

void cryptsetup_token_buffer_free(void *buffer, size_t buffer_len)
{
    if (buffer) {
        explicit_bzero(buffer, buffer_len);
        free(buffer);
    }
}

int cryptsetup_token_open(struct crypt_device *cd, int token, char **buffer, size_t *buffer_len, void *usrptr)
{
    (void)usrptr;
    if (!cd || !buffer || !buffer_len) return -EINVAL;

    const char *json = NULL;
    int r = crypt_token_json_get(cd, token, &json);
    if (r < 0 || !json) return r < 0 ? r : -EINVAL;

    json_object *root = json_tokener_parse(json);
    if (!root) return -EINVAL;
    json_object *jblob = NULL;
    uint8_t stored[2048];
    size_t stored_len = 0;
    if (!json_object_object_get_ex(root, "blob", &jblob) || !json_object_is_type(jblob, json_type_string) ||
        hex_decode(json_object_get_string(jblob), stored, sizeof stored, &stored_len) < 0) {
        json_object_put(root);
        return -EINVAL;
    }
    json_object_put(root);

    uint8_t secret[128];
    size_t secret_len = 0;
    r = tpm12_unseal(stored, stored_len, secret, sizeof secret, &secret_len);
    if (r < 0) {
        explicit_bzero(secret, sizeof secret);
        return r;
    }

    char *out = malloc(secret_len);
    if (!out) {
        explicit_bzero(secret, sizeof secret);
        return -ENOMEM;
    }
    memcpy(out, secret, secret_len);
    explicit_bzero(secret, sizeof secret);
    *buffer = out;
    *buffer_len = secret_len;
    return 0;
}
