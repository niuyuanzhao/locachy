/* Test harness for the broker's pairing code and cable TLS profile
 * (progs/kvm-broker-tls.c), on loopback; no namespace, no root.
 *   test-kvm-broker-tls code
 *       code encoding cases, PASS/FAIL lines
 *   test-kvm-broker-tls serve PORT KEY_HEX COUNT
 *       COUNT connections on 127.0.0.1:PORT through the broker's own server
 *       context, handshake and pump; the plaintext side echoes. One line
 *       per connection: HANDSHAKE ok|refused, PROFILE ..., PUMP ... */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include "../progs/kvm-broker-protocol.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("%s: code: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static int decodes(const char *text)
{
    unsigned char key[KVM_BROKER_KEY_BYTES];
    return kvm_broker_code_decode(text, strlen(text), key) == 0;
}

/* Copies text with each '-' replaced by sep, letters lowercased when lower
 * is set: the spellings a person may type. */
static void variant(const char *text, const char *sep, int lower, char *out)
{
    size_t n = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '-') {
            strcpy(out + n, sep);
            n += strlen(sep);
        } else {
            out[n++] = lower && *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
        }
    }
    out[n] = 0;
}

static int code_tests(void)
{
    unsigned char key[KVM_BROKER_KEY_BYTES], back[KVM_BROKER_KEY_BYTES];
    char text[KVM_BROKER_CODE_TEXT];
    int round = 1, shape = 1;
    for (int i = 0; i < 10000; i++) {
        if (getrandom(key, sizeof key, 0) != sizeof key) return 1;
        if (kvm_broker_code_encode(key, text, sizeof text) < 0 ||
            kvm_broker_code_decode(text, strlen(text), back) < 0 || memcmp(key, back, sizeof key) != 0) {
            round = 0;
        }
        /* 26 symbols in groups of 4, uppercase, final symbol's low 2 bits 0. */
        static const char b32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        const char *last = strlen(text) == 32 ? strchr(b32, text[31]) : NULL;
        if (!last || text[4] != '-' || text[29] != '-' || (last - b32) % 4 != 0) shape = 0;
    }
    check(round, "10000 random keys encode and decode back");
    check(shape, "printed as 26 uppercase symbols in '-' groups of 4, last symbol canonical");
    memset(key, 0, sizeof key);
    check(kvm_broker_code_encode(key, text, sizeof text) == 0 && !strcmp(text, "AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AA"),
          "all-zero key vector");
    memset(key, 0xff, sizeof key);
    check(kvm_broker_code_encode(key, text, sizeof text) == 0 && !strcmp(text, "7777-7777-7777-7777-7777-7777-74"),
          "all-ones key vector");
    check(kvm_broker_code_encode(key, text, KVM_BROKER_CODE_TEXT - 1) < 0, "short output buffer refused");
    {
        unsigned char ones[KVM_BROKER_KEY_BYTES], back[KVM_BROKER_KEY_BYTES];
        char spelled[KVM_BROKER_CODE_TEXT + 8];
        int ok = 1;
        memset(ones, 0xff, sizeof ones);
        kvm_broker_code_encode(ones, text, sizeof text);
        variant(text, "-", 1, spelled); /* lowercase */
        ok &= kvm_broker_code_decode(spelled, strlen(spelled), back) == 0 && memcmp(back, ones, sizeof ones) == 0;
        variant(text, " ", 0, spelled); /* spaces for dashes */
        ok &= kvm_broker_code_decode(spelled, strlen(spelled), back) == 0 && memcmp(back, ones, sizeof ones) == 0;
        variant(text, "", 0, spelled); /* no separators */
        ok &= kvm_broker_code_decode(spelled, strlen(spelled), back) == 0 && memcmp(back, ones, sizeof ones) == 0;
        variant(text, "\t", 1, spelled); /* a tab, then CR LF */
        strcat(spelled, "\r\n");
        ok &= kvm_broker_code_decode(spelled, strlen(spelled), back) == 0 && memcmp(back, ones, sizeof ones) == 0;
        check(ok, "case, separators, tab, CR and LF are accepted as the sender's parser does");
    }
    check(!decodes("7777-7777-7777-7777-7777-7777-75") && !decodes("7777-7777-7777-7777-7777-7777-77") &&
              !decodes("AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AB"),
          "non-canonical final symbol (low bits set) refused");
    check(!decodes("AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-A") && !decodes("AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAA") &&
              !decodes(""),
          "25, 27 and 0 symbols refused");
    check(!decodes("AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-A1") && !decodes("AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-A0") &&
              !decodes("AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-A8") && !decodes("AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AA=") &&
              !decodes("AAAA_AAAA-AAAA-AAAA-AAAA-AAAA-AA"),
          "characters outside base32 and padding refused");
    printf("code: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

static int hex_key(const char *text, struct kvm_broker_key *key)
{
    if (strlen(text) != 2 * KVM_BROKER_KEY_BYTES) return -1;
    for (int i = 0; i < KVM_BROKER_KEY_BYTES; i++) {
        unsigned v;
        if (sscanf(text + 2 * i, "%2x", &v) != 1) return -1;
        key->key[i] = (unsigned char)v;
    }
    key->set = 1;
    return 0;
}

static void echo(int fd, long long *bytes)
{
    char buffer[8192];
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n <= 0) return;
        *bytes += n;
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(fd, buffer + off, (size_t)(n - off));
            if (w <= 0) return;
            off += w;
        }
    }
}

static int serve(const char *port_text, const char *key_text, int count)
{
    struct kvm_broker_key key = { .set = 0 };
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons((unsigned short)atoi(port_text)) };
    int one = 1, listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    SSL_CTX *ctx;
    signal(SIGPIPE, SIG_IGN);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (hex_key(key_text, &key) < 0 || kvm_broker_tls_init() < 0 || !(ctx = kvm_broker_tls_context(&key)) ||
        listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
        bind(listener, (struct sockaddr *)&local, sizeof local) < 0 || listen(listener, 4) < 0) {
        printf("SETUP failed\n");
        return 1;
    }
    printf("READY\n");
    fflush(stdout);
    for (int i = 0; i < count; i++) {
        int tcp = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK), plain[2];
        SSL *ssl = SSL_new(ctx);
        if (tcp < 0 || !ssl) return 1;
        if (kvm_broker_tls_accept(ssl, tcp, kvm_broker_now_ms() + 5000) < 0) {
            printf("HANDSHAKE refused\n");
            fflush(stdout);
            SSL_free(ssl);
            close(tcp);
            continue;
        }
        const unsigned char *alpn;
        unsigned alpn_len;
        SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
        printf("HANDSHAKE ok\nPROFILE version=%s cipher=%s alpn=%.*s identity=%s reused=%d\n", SSL_get_version(ssl),
               SSL_CIPHER_get_name(SSL_get_current_cipher(ssl)), (int)alpn_len, (const char *)alpn,
               SSL_get_psk_identity(ssl), SSL_session_reused(ssl));
        fflush(stdout);
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, plain) < 0) return 1;
        pid_t child = fork();
        if (child == 0) {
            close(plain[0]);
            _exit(kvm_broker_pump(ssl, tcp, plain[1], -1) == 0 ? 0 : 1);
        }
        close(plain[1]);
        long long bytes = 0;
        echo(plain[0], &bytes);
        close(plain[0]);
        int status = 0;
        waitpid(child, &status, 0);
        printf("PUMP bytes=%lld end=%s\n", bytes, WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "orderly" : "error");
        fflush(stdout);
        SSL_free(ssl);
        close(tcp);
    }
    SSL_CTX_free(ctx);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "code")) return code_tests();
    if (argc == 5 && !strcmp(argv[1], "serve")) return serve(argv[2], argv[3], atoi(argv[4]));
    fprintf(stderr, "usage: test-kvm-broker-tls code | serve PORT KEY_HEX COUNT\n");
    return 2;
}
