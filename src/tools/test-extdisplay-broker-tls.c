/* Test harness for the broker's pairing code and cable TLS profile
 * (progs/extdisplay-broker-tls.c), on loopback; no namespace, no root.
 *   test-extdisplay-broker-tls code
 *       code encoding cases, PASS/FAIL lines
 *   test-extdisplay-broker-tls pump
 *       in-process TLS pump cases (partial encrypted record does not spin),
 *       PASS/FAIL lines
 *   test-extdisplay-broker-tls serve PORT KEY_HEX COUNT
 *       COUNT connections on 127.0.0.1:PORT through the broker's own server
 *       context, handshake and pump; the plaintext side echoes. One line
 *       per connection: HANDSHAKE ok|refused, PROFILE ..., PUMP ... */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "../progs/extdisplay-broker-protocol.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int failures;

static void check_in(const char *group, int ok, const char *what)
{
    printf("%s: %s: %s\n", ok ? "PASS" : "FAIL", group, what);
    if (!ok) failures++;
}

static void check(int ok, const char *what)
{
    check_in("code", ok, what);
}

static int decodes(const char *text)
{
    unsigned char key[EXTDISPLAY_BROKER_KEY_BYTES];
    return extdisplay_broker_code_decode(text, strlen(text), key) == 0;
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

static int pump_partial_record_test(void);

static int code_tests(void)
{
    unsigned char key[EXTDISPLAY_BROKER_KEY_BYTES], back[EXTDISPLAY_BROKER_KEY_BYTES];
    char text[EXTDISPLAY_BROKER_CODE_TEXT];
    int round = 1, shape = 1;
    for (int i = 0; i < 10000; i++) {
        if (getrandom(key, sizeof key, 0) != sizeof key) return 1;
        if (extdisplay_broker_code_encode(key, text, sizeof text) < 0 ||
            extdisplay_broker_code_decode(text, strlen(text), back) < 0 || memcmp(key, back, sizeof key) != 0) {
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
    check(extdisplay_broker_code_encode(key, text, sizeof text) == 0 && !strcmp(text, "AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AA"),
          "all-zero key vector");
    memset(key, 0xff, sizeof key);
    check(extdisplay_broker_code_encode(key, text, sizeof text) == 0 && !strcmp(text, "7777-7777-7777-7777-7777-7777-74"),
          "all-ones key vector");
    check(extdisplay_broker_code_encode(key, text, EXTDISPLAY_BROKER_CODE_TEXT - 1) < 0, "short output buffer refused");
    {
        unsigned char ones[EXTDISPLAY_BROKER_KEY_BYTES], back[EXTDISPLAY_BROKER_KEY_BYTES];
        char spelled[EXTDISPLAY_BROKER_CODE_TEXT + 8];
        int ok = 1;
        memset(ones, 0xff, sizeof ones);
        extdisplay_broker_code_encode(ones, text, sizeof text);
        variant(text, "-", 1, spelled); /* lowercase */
        ok &= extdisplay_broker_code_decode(spelled, strlen(spelled), back) == 0 && memcmp(back, ones, sizeof ones) == 0;
        variant(text, " ", 0, spelled); /* spaces for dashes */
        ok &= extdisplay_broker_code_decode(spelled, strlen(spelled), back) == 0 && memcmp(back, ones, sizeof ones) == 0;
        variant(text, "", 0, spelled); /* no separators */
        ok &= extdisplay_broker_code_decode(spelled, strlen(spelled), back) == 0 && memcmp(back, ones, sizeof ones) == 0;
        variant(text, "\t", 1, spelled); /* a tab, then CR LF */
        strcat(spelled, "\r\n");
        ok &= extdisplay_broker_code_decode(spelled, strlen(spelled), back) == 0 && memcmp(back, ones, sizeof ones) == 0;
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
    (void)pump_partial_record_test();
    printf("code: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

static int hex_key(const char *text, struct extdisplay_broker_key *key)
{
    if (strlen(text) != 2 * EXTDISPLAY_BROKER_KEY_BYTES) return -1;
    for (int i = 0; i < EXTDISPLAY_BROKER_KEY_BYTES; i++) {
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

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

static unsigned int psk_client(SSL *ssl, const char *hint, char *identity, unsigned int max_id,
                               unsigned char *psk, unsigned int max_psk)
{
    const struct extdisplay_broker_key *key = SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
    size_t id_len = strlen(EXTDISPLAY_BROKER_IDENTITY);
    (void)hint;
    if (!key || !key->set || max_id <= id_len || max_psk < EXTDISPLAY_BROKER_KEY_BYTES) return 0;
    memcpy(identity, EXTDISPLAY_BROKER_IDENTITY, id_len + 1);
    memcpy(psk, key->key, EXTDISPLAY_BROKER_KEY_BYTES);
    return EXTDISPLAY_BROKER_KEY_BYTES;
}

static SSL_CTX *client_tls_context(struct extdisplay_broker_key *key)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    unsigned char alpn[1 + sizeof EXTDISPLAY_BROKER_ALPN];
    size_t alpn_len = strlen(EXTDISPLAY_BROKER_ALPN);
    if (!ctx) return NULL;
    alpn[0] = (unsigned char)alpn_len;
    memcpy(alpn + 1, EXTDISPLAY_BROKER_ALPN, alpn_len);
    SSL_CTX_set_security_level(ctx, 2);
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) ||
        !SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION) ||
        !SSL_CTX_set_cipher_list(ctx, EXTDISPLAY_BROKER_CIPHER) ||
        SSL_CTX_set_alpn_protos(ctx, alpn, (unsigned)(1 + alpn_len)) != 0 ||
        !SSL_CTX_set_app_data(ctx, key)) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_CTX_set_psk_client_callback(ctx, psk_client);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    return ctx;
}

static int write_all_fd(int fd, const unsigned char *buf, size_t len, long long deadline_ms)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            long long left = deadline_ms - extdisplay_broker_now_ms();
            struct pollfd p = { fd, POLLOUT, 0 };
            if (left <= 0 || poll(&p, 1, (int)left) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int read_exact_fd(int fd, unsigned char *buf, size_t len, long long deadline_ms)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            long long left = deadline_ms - extdisplay_broker_now_ms();
            struct pollfd p = { fd, POLLIN, 0 };
            if (left <= 0 || poll(&p, 1, (int)left) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int flush_wbio(BIO *wbio, int fd, long long deadline_ms)
{
    unsigned char buf[4096];
    int n;
    while ((n = BIO_read(wbio, buf, (int)sizeof buf)) > 0) {
        if (write_all_fd(fd, buf, (size_t)n, deadline_ms) < 0) return -1;
    }
    return 0;
}

static int feed_rbio_once(BIO *rbio, int fd, long long deadline_ms)
{
    unsigned char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) return BIO_write(rbio, buf, (int)n) == (int)n ? 0 : -1;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            long long left = deadline_ms - extdisplay_broker_now_ms();
            struct pollfd p = { fd, POLLIN, 0 };
            if (left <= 0 || poll(&p, 1, (int)left) <= 0) return -1;
            continue;
        }
        return -1;
    }
}

static int client_handshake_bio(SSL *ssl, BIO *rbio, BIO *wbio, int fd, long long deadline_ms)
{
    SSL_set_connect_state(ssl);
    for (;;) {
        int r = SSL_do_handshake(ssl);
        if (flush_wbio(wbio, fd, deadline_ms) < 0) return -1;
        if (r == 1) return 0;
        int e = SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ) {
            if (feed_rbio_once(rbio, fd, deadline_ms) < 0) return -1;
            continue;
        }
        if (e == SSL_ERROR_WANT_WRITE) continue;
        return -1;
    }
}

struct pump_result {
    int rc;
    long long cpu_ms;
    char detail[128];
};

static long long rusage_cpu_ms(const struct rusage *a, const struct rusage *b)
{
    long long ua = (long long)a->ru_utime.tv_sec * 1000 + a->ru_utime.tv_usec / 1000 +
                   (long long)a->ru_stime.tv_sec * 1000 + a->ru_stime.tv_usec / 1000;
    long long ub = (long long)b->ru_utime.tv_sec * 1000 + b->ru_utime.tv_usec / 1000 +
                   (long long)b->ru_stime.tv_sec * 1000 + b->ru_stime.tv_usec / 1000;
    return ub - ua;
}

/* Sends the first half of an encrypted TLS record, pauses 200 ms, verifies
 * the pump sleeps in poll() rather than spinning on SSL_has_pending, then
 * sends the second half and checks plaintext delivery and clean close detail. */
static int pump_partial_record_test(void)
{
    struct extdisplay_broker_key key = { .set = 1 };
    int tcp[2] = { -1, -1 }, plain[2] = { -1, -1 }, report[2] = { -1, -1 };
    SSL_CTX *srv_ctx = NULL, *cli_ctx = NULL;
    SSL *cli = NULL;
    BIO *rbio = NULL, *wbio = NULL;
    pid_t child = -1;
    int got_res = 0, ok_no_spin = 0, ok_payload = 0, ok_clean = 0;

    for (int i = 0; i < EXTDISPLAY_BROKER_KEY_BYTES; i++) key.key[i] = (unsigned char)(0x10 + i);
    signal(SIGPIPE, SIG_IGN);
    if (extdisplay_broker_tls_init() < 0 || !(srv_ctx = extdisplay_broker_tls_context(&key)) ||
        !(cli_ctx = client_tls_context(&key)) ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, tcp) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, plain) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, report) < 0) {
        goto fail;
    }

    child = fork();
    if (child < 0) goto fail;
    if (child == 0) {
        struct pump_result res;
        struct rusage before, after;
        SSL *srv = SSL_new(srv_ctx);
        memset(&res, 0, sizeof res);
        res.rc = -1;
        close(tcp[0]);
        close(plain[0]);
        close(report[0]);
        if (srv && set_nonblocking(tcp[1]) == 0 &&
            extdisplay_broker_tls_accept(srv, tcp[1], extdisplay_broker_now_ms() + 5000) == 0) {
            getrusage(RUSAGE_SELF, &before);
            res.rc = extdisplay_broker_pump(srv, tcp[1], plain[1], -1, res.detail, sizeof res.detail);
            getrusage(RUSAGE_SELF, &after);
            res.cpu_ms = rusage_cpu_ms(&before, &after);
        }
        if (srv) SSL_free(srv);
        (void)!write(report[1], &res, sizeof res);
        _exit(0);
    }
    close(tcp[1]);
    close(plain[1]);
    close(report[1]);
    tcp[1] = plain[1] = report[1] = -1;

    if (set_nonblocking(tcp[0]) < 0 || set_nonblocking(plain[0]) < 0) goto fail;
    cli = SSL_new(cli_ctx);
    rbio = BIO_new(BIO_s_mem());
    wbio = BIO_new(BIO_s_mem());
    if (!cli || !rbio || !wbio) goto fail;
    SSL_set_bio(cli, rbio, wbio);
    rbio = BIO_up_ref(rbio) ? rbio : NULL;
    wbio = BIO_up_ref(wbio) ? wbio : NULL;
    if (!rbio || !wbio) goto fail;

    long long deadline = extdisplay_broker_now_ms() + 5000;
    if (client_handshake_bio(cli, rbio, wbio, tcp[0], deadline) < 0) goto fail;

    unsigned char msg[256], rec[1024], got[256];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (unsigned char)((i * 37 + 11) & 0xff);
    if (SSL_write(cli, msg, (int)sizeof msg) != (int)sizeof msg) goto fail;
    int rec_len = BIO_read(wbio, rec, (int)sizeof rec);
    if (rec_len <= 32) goto fail;

    /* Send the 5-byte TLS record header plus only part of the ciphertext. */
    size_t first_half = (size_t)(rec_len / 2);
    if (write_all_fd(tcp[0], rec, first_half, deadline) < 0) goto fail;

    /* Hold the incomplete TLS record open for 200 ms. */
    struct timespec pause_ts = { 0, 200 * 1000 * 1000L };
    nanosleep(&pause_ts, NULL);

    /* No plaintext may appear while the record is incomplete. */
    unsigned char probe_byte;
    ssize_t early = read(plain[0], &probe_byte, 1);
    int no_early_data = (early < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));

    /* Now send the remainder of the encrypted TLS record. */
    if (write_all_fd(tcp[0], rec + first_half, (size_t)rec_len - first_half, deadline) < 0) goto fail;
    if (read_exact_fd(plain[0], got, sizeof got, deadline) == 0 && no_early_data &&
        memcmp(got, msg, sizeof msg) == 0) {
        ok_payload = 1;
    }

    /* Send an orderly TLS close_notify and verify the pump's result and CPU time. */
    (void)SSL_shutdown(cli);
    if (flush_wbio(wbio, tcp[0], deadline) < 0) goto fail;
    shutdown(tcp[0], SHUT_WR);

    struct pump_result res;
    memset(&res, 0, sizeof res);
    res.rc = -1;
    if (read_exact_fd(report[0], (unsigned char *)&res, sizeof res, deadline) == 0) {
        got_res = 1;
        ok_no_spin = (res.cpu_ms >= 0 && res.cpu_ms < 80);
        ok_clean = (res.rc == 0 && strcmp(res.detail, "peer-eof") == 0);
    }

fail:
    if (child > 0) {
        int st = 0;
        if (!got_res) kill(child, SIGKILL);
        while (waitpid(child, &st, 0) < 0 && errno == EINTR) {}
    }
    if (cli) SSL_free(cli);
    if (rbio) BIO_free(rbio);
    if (wbio) BIO_free(wbio);
    if (cli_ctx) SSL_CTX_free(cli_ctx);
    if (srv_ctx) SSL_CTX_free(srv_ctx);
    for (int i = 0; i < 2; i++) {
        if (tcp[i] >= 0) close(tcp[i]);
        if (plain[i] >= 0) close(plain[i]);
        if (report[i] >= 0) close(report[i]);
    }
    check_in("pump", ok_no_spin, "partial encrypted TLS record sleeps in poll without CPU spin");
    check_in("pump", ok_payload, "partial encrypted TLS record delivers intact plaintext once completed");
    check_in("pump", ok_clean, "partial encrypted TLS record closes cleanly with peer-eof detail");
    return (ok_no_spin && ok_payload && ok_clean) ? 0 : 1;
}

static int serve(const char *port_text, const char *key_text, int count)
{
    struct extdisplay_broker_key key = { .set = 0 };
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons((unsigned short)atoi(port_text)) };
    int one = 1, listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    SSL_CTX *ctx;
    signal(SIGPIPE, SIG_IGN);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (hex_key(key_text, &key) < 0 || extdisplay_broker_tls_init() < 0 || !(ctx = extdisplay_broker_tls_context(&key)) ||
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
        if (extdisplay_broker_tls_accept(ssl, tcp, extdisplay_broker_now_ms() + 5000) < 0) {
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
            char detail[128] = "";
            close(plain[0]);
            _exit(extdisplay_broker_pump(ssl, tcp, plain[1], -1, detail, sizeof detail) == 0 ? 0 : 1);
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
    if (argc == 2 && !strcmp(argv[1], "pump")) return pump_partial_record_test();
    if (argc == 5 && !strcmp(argv[1], "serve")) return serve(argv[2], argv[3], atoi(argv[4]));
    fprintf(stderr, "usage: test-extdisplay-broker-tls code | pump | serve PORT KEY_HEX COUNT\n");
    return 2;
}
