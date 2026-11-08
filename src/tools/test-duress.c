/* base/duress.c against files of its own, never a real disk, key or TPM:
 * build with -DDURESS_TEST_NO_POWEROFF and every DURESS_* path pointing
 * into SCRATCH (tools/test-duress.sh does).
 *   test-duress SCRATCH */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "duress.h"
/* The implementation itself, for its hash functions' known answers. */
#include "../base/duress.c"

static const char *scratch;

static void put(const char *name, const void *data, size_t len)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", scratch, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, data, len) == (ssize_t)len && close(fd) == 0);
}

/* A LUKS2 partition image: magic, version 2, label, a JSON area naming the
 * data offset, key slot bytes and data after it. */
static void luks_image(const char *name, const char *label, unsigned long long offset, size_t total)
{
    char *img = calloc(1, total);
    assert(img);
    memcpy(img, "LUKS\xba\xbe\x00\x02", 8);
    snprintf(img + 24, 48, "%s", label);
    snprintf(img + 4096, 256, "{\"keyslots\":{},\"segments\":{\"0\":{\"type\":\"crypt\",\"offset\":\"%llu\"}}}",
             offset);
    memset(img + 32768, 'K', 4096);               /* a key slot */
    memset(img + offset, 'D', total - offset);    /* encrypted data */
    put(name, img, total);
    free(img);
}

static int all_zero(const char *name, size_t from, size_t to)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", scratch, name);
    FILE *f = fopen(path, "rb");
    assert(f);
    int zero = 1;
    for (size_t i = 0; i < to; i++) {
        int c = fgetc(f);
        if (i >= from && c != 0) zero = 0;
    }
    fclose(f);
    return zero;
}

static int byte_at(const char *name, size_t at)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", scratch, name);
    FILE *f = fopen(path, "rb");
    assert(f && fseek(f, (long)at, SEEK_SET) == 0);
    int c = fgetc(f);
    fclose(f);
    return c;
}

static int exists(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", scratch, name);
    return access(path, F_OK) == 0;
}

static int eq_hex(const uint8_t *b, size_t n, const char *hex)
{
    char got[2 * 64 + 1];
    for (size_t i = 0; i < n; i++) snprintf(got + 2 * i, 3, "%02x", b[i]);
    return strcmp(got, hex) == 0;
}

/* Known answers, computed with Python's hashlib: SHA-256 (FIPS 180-4) and
 * PBKDF2-HMAC-SHA256 (RFC 8018), including a password longer than a block. */
static void known_answers(void)
{
    uint8_t out[32];
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, "abc", 3);
    sha256_final(&c, out);
    assert(eq_hex(out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    sha256_init(&c);
    sha256_final(&c, out);
    assert(eq_hex(out, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    pbkdf2_sha256("password", 8, (const uint8_t *)"salt", 4, 1, out);
    assert(eq_hex(out, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    pbkdf2_sha256("password", 8, (const uint8_t *)"salt", 4, 2, out);
    assert(eq_hex(out, 32, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
    char longpw[100], longsalt[40];
    memset(longpw, 'p', sizeof longpw);
    memset(longsalt, 's', sizeof longsalt);
    pbkdf2_sha256(longpw, sizeof longpw, (const uint8_t *)longsalt, sizeof longsalt, 3, out);
    assert(eq_hex(out, 32, "680f82699c21d6fe4e72fe739abc439358c93aeeb367b3630a689ab84d59f7ce"));
    puts("PASS SHA-256 and PBKDF2-HMAC-SHA256 known answers (hashlib)");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    known_answers();
    scratch = argv[1];
    /* No verifier: nothing matches, nothing is configured. */
    assert(!duress_configured() && !duress_matches("anything at all"));
    /* Too short: refused, nothing written. */
    assert(duress_set("short") < 0 && !duress_configured());
    assert(duress_set("correct horse battery") == 0 && duress_configured());
    struct stat st;
    assert(stat(DURESS_FILE, &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(duress_matches("correct horse battery"));
    assert(!duress_matches("correct horse batterY") && !duress_matches("") && !duress_matches(NULL));
    puts("PASS verifier: 0600, PBKDF2 match, near misses and empty refused");

    /* A damaged verifier never matches (fails safe: no wipe). */
    put("duress", "v1:600000:zz:00\n", 16);
    assert(!duress_configured() && !duress_matches("correct horse battery"));
    put("duress", "v1:10:00112233445566778899aabbccddeeff:"
                  "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n", 104);
    assert(!duress_configured()); /* too few rounds: refused */
    puts("PASS damaged or weakened verifiers are not accepted");

    /* Nothing to wipe: duress_wipe does nothing, keys stay. */
    assert(duress_set("correct horse battery") == 0);
    put("pcr.pem", "PCR KEY", 7);
    assert(!duress_has_luks());
    duress_wipe();
    assert(exists("pcr.pem") && duress_configured());
    puts("PASS without LOCachy's LUKS2 partitions the wipe does nothing");

    /* LOCachy's partitions (root and swap), one of another label, one LUKS1. */
    const size_t total = 40u << 20;
    luks_image("sda2", "locachy-crypt", 16u << 20, total);
    luks_image("sda3", "locachy-swap-crypt", 24u << 20, total);
    luks_image("sdb1", "someone-else", 16u << 20, total);
    char luks1[512] = "LUKS\xba\xbe\x00\x01";
    put("sdc1", luks1, sizeof luks1);
    assert(duress_has_luks());
    char found[4][160];
    assert(duress_luks_paths(found, 4) == 2);
    assert(strstr(found[0], "sda2") || strstr(found[1], "sda2"));
    assert(strstr(found[0], "sda3") || strstr(found[1], "sda3"));
    duress_wipe();
    assert(all_zero("sda2", 0, 16u << 20) && byte_at("sda2", (16u << 20) + 1) == 'D');
    assert(all_zero("sda3", 0, 24u << 20) && byte_at("sda3", (24u << 20) + 1) == 'D');
    assert(byte_at("sdb1", 0) == 'L' && byte_at("sdb1", 32768) == 'K');
    assert(byte_at("sdc1", 0) == 'L');
    assert(!exists("pcr.pem") && !duress_configured());
    puts("PASS wipe: both LOCachy headers and key slots up to the data offset, others untouched, keys gone");
    return 0;
}
