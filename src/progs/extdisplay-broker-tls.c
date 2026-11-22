/* extdisplay-broker-tls.c: the parts of the extended-display broker that need no
 * namespace (extdisplay-broker-protocol.h): the pairing code, the cable TLS profile
 * on OpenSSL and the pump between the TLS session and the plaintext stream.
 * Shared by extdisplay-broker and its tests. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "extdisplay-broker-protocol.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

int extdisplay_broker_code_encode(const unsigned char key[EXTDISPLAY_BROKER_KEY_BYTES], char *text, size_t size)
{
    unsigned buffer = 0;
    int bits = 0, symbols = 0;
    size_t out = 0;
    if (!key || !text || size < EXTDISPLAY_BROKER_CODE_TEXT) return -1;
    for (int i = 0; i <= EXTDISPLAY_BROKER_KEY_BYTES; i++) {
        if (i < EXTDISPLAY_BROKER_KEY_BYTES) {
            buffer = (buffer << 8) | key[i];
            bits += 8;
        } else if (bits > 0) {
            /* 128 = 25 * 5 + 3: the last symbol's two low bits are zero. */
            buffer <<= 5 - bits;
            bits = 5;
        }
        while (bits >= 5) {
            if (symbols && symbols % EXTDISPLAY_BROKER_CODE_GROUP == 0) text[out++] = '-';
            text[out++] = alphabet[(buffer >> (bits - 5)) & 31];
            symbols++;
            bits -= 5;
            buffer &= (1u << bits) - 1;
        }
    }
    text[out] = 0;
    OPENSSL_cleanse(&buffer, sizeof buffer);
    return symbols == EXTDISPLAY_BROKER_CODE_SYMBOLS ? 0 : -1;
}

int extdisplay_broker_code_decode(const char *text, size_t length, unsigned char key[EXTDISPLAY_BROKER_KEY_BYTES])
{
    unsigned char output[EXTDISPLAY_BROKER_KEY_BYTES];
    uint32_t accumulator = 0;
    int pending = 0, symbols = 0, count = 0, status = -1;
    if (!text || !key) return -1;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)text[i];
        uint32_t value;
        if (byte == '-' || byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') continue;
        if (byte >= 'A' && byte <= 'Z') value = byte - 'A';
        else if (byte >= 'a' && byte <= 'z') value = byte - 'a';
        else if (byte >= '2' && byte <= '7') value = byte - '2' + 26;
        else goto done;
        if (++symbols > EXTDISPLAY_BROKER_CODE_SYMBOLS) goto done;
        accumulator = (accumulator << 5) | value;
        pending += 5;
        if (pending >= 8) {
            pending -= 8;
            output[count++] = (unsigned char)((accumulator >> pending) & 0xff);
            accumulator &= (1u << pending) - 1;
        }
    }
    if (symbols == EXTDISPLAY_BROKER_CODE_SYMBOLS && count == EXTDISPLAY_BROKER_KEY_BYTES && pending == 2 &&
        accumulator == 0) {
        memcpy(key, output, EXTDISPLAY_BROKER_KEY_BYTES);
        status = 0;
    }
done:
    OPENSSL_cleanse(output, sizeof output);
    OPENSSL_cleanse(&accumulator, sizeof accumulator);
    return status;
}

long long extdisplay_broker_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int extdisplay_broker_tls_init(void)
{
    return OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, NULL) == 1 ? 0 : -1;
}

/* The identity is checked here and only here: OpenSSL itself does not
 * compare it with anything. Anything else gets no key, which fails the
 * handshake. */
static unsigned int psk_server(SSL *ssl, const char *identity, unsigned char *psk, unsigned int max)
{
    struct extdisplay_broker_key *key = SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
    if (!key || !key->set || !identity || strcmp(identity, EXTDISPLAY_BROKER_IDENTITY) != 0 ||
        max < EXTDISPLAY_BROKER_KEY_BYTES) {
        return 0;
    }
    memcpy(psk, key->key, EXTDISPLAY_BROKER_KEY_BYTES);
    return EXTDISPLAY_BROKER_KEY_BYTES;
}

/* A ClientHello without ALPN never reaches the select callback below, so
 * its absence fails the handshake here. */
static int client_hello(SSL *ssl, int *alert, void *arg)
{
    const unsigned char *data;
    size_t length;
    (void)arg;
    if (!SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_application_layer_protocol_negotiation, &data, &length)) {
        *alert = SSL_AD_NO_APPLICATION_PROTOCOL;
        return SSL_CLIENT_HELLO_ERROR;
    }
    return SSL_CLIENT_HELLO_SUCCESS;
}

static int alpn_select(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
                       unsigned int inlen, void *arg)
{
    size_t want = strlen(EXTDISPLAY_BROKER_ALPN);
    (void)ssl;
    (void)arg;
    for (unsigned int i = 0; i < inlen;) {
        unsigned int n = in[i++];
        if (n > inlen - i) break;
        if (n == want && memcmp(in + i, EXTDISPLAY_BROKER_ALPN, n) == 0) {
            *out = in + i;
            *outlen = (unsigned char)n;
            return SSL_TLSEXT_ERR_OK;
        }
        i += n;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

SSL_CTX *extdisplay_broker_tls_context(struct extdisplay_broker_key *key)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    STACK_OF(SSL_CIPHER) *ciphers;
    if (!ctx) return NULL;
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET | SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION |
                                 SSL_OP_NO_TLSv1_3 | SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    /* 128-bit suites without forward secrecy are allowed at level 2. */
    SSL_CTX_set_security_level(ctx, 2);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) ||
        !SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION) || !SSL_CTX_set_num_tickets(ctx, 0) ||
        !SSL_CTX_set_ciphersuites(ctx, "") || !SSL_CTX_set_cipher_list(ctx, EXTDISPLAY_BROKER_CIPHER) ||
        !SSL_CTX_set_app_data(ctx, key)) {
        goto fail;
    }
    ciphers = SSL_CTX_get_ciphers(ctx);
    if (!ciphers || sk_SSL_CIPHER_num(ciphers) != 1 ||
        SSL_CIPHER_get_protocol_id(sk_SSL_CIPHER_value(ciphers, 0)) != EXTDISPLAY_BROKER_CIPHER_ID) {
        goto fail;
    }
    SSL_CTX_set_psk_server_callback(ctx, psk_server);
    SSL_CTX_set_client_hello_cb(ctx, client_hello, NULL);
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select, NULL);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    return ctx;
fail:
    SSL_CTX_free(ctx);
    return NULL;
}

int extdisplay_broker_tls_verify(SSL *ssl)
{
    const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl);
    const unsigned char *alpn = NULL;
    unsigned int length = 0;
    const char *identity = SSL_get_psk_identity(ssl);
    SSL_get0_alpn_selected(ssl, &alpn, &length);
    if (SSL_version(ssl) != TLS1_2_VERSION || !cipher ||
        SSL_CIPHER_get_protocol_id(cipher) != EXTDISPLAY_BROKER_CIPHER_ID || SSL_session_reused(ssl) ||
        length != strlen(EXTDISPLAY_BROKER_ALPN) || !alpn || memcmp(alpn, EXTDISPLAY_BROKER_ALPN, length) != 0 ||
        !identity || strcmp(identity, EXTDISPLAY_BROKER_IDENTITY) != 0) {
        return -1;
    }
    return 0;
}

static int wait_fd(int fd, short events, long long deadline_ms)
{
    for (;;) {
        long long left = deadline_ms < 0 ? -1 : deadline_ms - extdisplay_broker_now_ms();
        struct pollfd p = { fd, events, 0 };
        if (deadline_ms >= 0 && left <= 0) return -1;
        int n = poll(&p, 1, left < 0 ? -1 : left > INT_MAX ? INT_MAX : (int)left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (p.revents & POLLNVAL)) return -1;
        return 0;
    }
}

int extdisplay_broker_tls_accept(SSL *ssl, int fd, long long deadline_ms)
{
    if (!SSL_set_fd(ssl, fd)) return -1;
    for (;;) {
        ERR_clear_error();
        int r = SSL_accept(ssl);
        if (r == 1) return extdisplay_broker_tls_verify(ssl);
        switch (SSL_get_error(ssl, r)) {
        case SSL_ERROR_WANT_READ:
            if (wait_fd(fd, POLLIN, deadline_ms) < 0) return -1;
            break;
        case SSL_ERROR_WANT_WRITE:
            if (wait_fd(fd, POLLOUT, deadline_ms) < 0) return -1;
            break;
        default:
            return -1;
        }
    }
}

static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

static void __attribute__((format(printf, 3, 4))) detail_set(char *detail, size_t size, const char *fmt, ...)
{
    if (!detail || size == 0 || detail[0]) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, size, fmt, args);
    va_end(args);
    for (size_t i = 0; detail[i]; i++) {
        unsigned char c = (unsigned char)detail[i];
        if (c < 0x20 || c > 0x7e) detail[i] = ' ';
    }
}

static void detail_ssl(char *detail, size_t size, const char *op, int ssl_err, int saved_errno)
{
    if (!detail || size == 0 || detail[0]) return;
    if (ssl_err == SSL_ERROR_SYSCALL) {
        if (saved_errno == 0) detail_set(detail, size, "%s-eof", op);
        else detail_set(detail, size, "%s-syscall: %s", op, strerror(saved_errno));
        return;
    }
    unsigned long code = ERR_get_error();
    if (code) {
        char buf[128];
        ERR_error_string_n(code, buf, sizeof buf);
        detail_set(detail, size, "%s: %s", op, buf);
    } else {
        detail_set(detail, size, "%s: err=%d", op, ssl_err);
    }
}

/* Down: TLS to plaintext; up: plaintext to TLS. Each direction holds at most
 * one buffer; a side that does not take its bytes stops the other side from
 * being read (TCP and the socket buffers push back). */
int extdisplay_broker_pump(SSL *ssl, int tcp, int plain, int stop, char *detail, size_t detail_size)
{
    unsigned char *down = malloc(EXTDISPLAY_BROKER_PUMP_BUFFER), *up = malloc(EXTDISPLAY_BROKER_PUMP_BUFFER);
    size_t down_len = 0, down_off = 0, up_len = 0, up_off = 0;
    int tls_end = 0, plain_end = 0, status = 0;
    int read_wants_write = 0, write_wants_read = 0;
    if (detail && detail_size > 0) detail[0] = 0;
    if (!down || !up || nonblocking(tcp) < 0 || nonblocking(plain) < 0) {
        detail_set(detail, detail_size, "pump-init: %s", strerror(errno));
        status = -1;
        goto done;
    }
    for (;;) {
        int read_skipped = write_wants_read;
        if (!tls_end && down_len == 0 && !write_wants_read) {
            size_t got = 0;
            ERR_clear_error();
            errno = 0;
            int r = SSL_read_ex(ssl, down, EXTDISPLAY_BROKER_PUMP_BUFFER, &got);
            read_wants_write = 0;
            if (r == 1) {
                down_len = got;
                down_off = 0;
            } else {
                int saved = errno;
                int e = SSL_get_error(ssl, r);
                switch (e) {
                case SSL_ERROR_WANT_READ:
                    break;
                case SSL_ERROR_WANT_WRITE:
                    read_wants_write = 1;
                    break;
                case SSL_ERROR_ZERO_RETURN:
                    tls_end = 1;
                    detail_set(detail, detail_size, "peer-eof");
                    break;
                default: /* reset, unexpected EOF, a bad record */
                    detail_ssl(detail, detail_size, "ssl-read", e, saved);
                    tls_end = 1;
                    status = -1;
                    goto done;
                }
            }
        }
        while (down_off < down_len) {
            ssize_t n = send(plain, down + down_off, down_len - down_off, MSG_NOSIGNAL);
            if (n > 0) {
                down_off += (size_t)n;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                detail_set(detail, detail_size, "plain-write: %s", n < 0 ? strerror(errno) : "eof");
                status = -1; /* the frontend closed its end */
                goto done;
            }
        }
        if (down_off == down_len) down_len = down_off = 0;

        if (!plain_end && up_len == 0) {
            ssize_t n = read(plain, up, EXTDISPLAY_BROKER_PUMP_BUFFER);
            if (n > 0) {
                up_len = (size_t)n;
                up_off = 0;
            } else if (n == 0) {
                detail_set(detail, detail_size, "worker-eof");
                plain_end = 1;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                detail_set(detail, detail_size, "plain-read: %s", strerror(errno));
                plain_end = 1;
                status = -1;
            }
        }
        if (up_off < up_len && !read_wants_write) {
            write_wants_read = 0;
            while (up_off < up_len) {
                size_t wrote = 0;
                ERR_clear_error();
                errno = 0;
                int r = SSL_write_ex(ssl, up + up_off, up_len - up_off, &wrote);
                if (r == 1) {
                    up_off += wrote;
                    continue;
                }
                int saved = errno;
                int e = SSL_get_error(ssl, r);
                if (e == SSL_ERROR_WANT_WRITE) break;
                if (e == SSL_ERROR_WANT_READ) {
                    write_wants_read = 1;
                    break;
                }
                detail_ssl(detail, detail_size, "ssl-write", e, saved);
                status = -1;
                goto done;
            }
        }
        if (up_off == up_len) up_len = up_off = 0;

        if (tls_end && down_len == 0) {
            if (status == 0) {
                detail_set(detail, detail_size, "peer-eof");
                ERR_clear_error();
                (void)SSL_shutdown(ssl); /* answer the peer's close_notify */
            }
            goto done;
        }
        if (plain_end && up_len == 0) {
            detail_set(detail, detail_size, "worker-eof");
            ERR_clear_error();
            (void)SSL_shutdown(ssl); /* close_notify, best effort */
            goto done;
        }
        /* Decrypted plaintext bytes buffered inside OpenSSL never wake poll.
         * SSL_has_pending also returns 1 for partial undecrypted records,
         * which would spin without waiting for more TCP bytes. */
        if (!tls_end && down_len == 0 && !write_wants_read && !read_wants_write &&
            (SSL_pending(ssl) > 0 || read_skipped)) {
            continue;
        }

        struct pollfd p[3] = { { tcp, 0, 0 }, { plain, 0, 0 }, { stop, POLLIN, 0 } };
        if ((!tls_end && down_len == 0 && !read_wants_write) || write_wants_read) p[0].events |= POLLIN;
        if ((up_len > 0 && !write_wants_read) || read_wants_write) p[0].events |= POLLOUT;
        if (down_len > 0) p[1].events |= POLLOUT;
        if (!plain_end && up_len == 0) p[1].events |= POLLIN;
        /* A side with nothing wanted is left out, so a hang-up there cannot
         * spin this loop; the next read or write reports it. One side always
         * has something wanted while the pump runs. */
        for (int i = 0; i < 2; i++)
            if (!p[i].events) p[i].fd = -1;
        if (poll(p, 3, -1) < 0 && errno != EINTR) {
            detail_set(detail, detail_size, "poll-error: %s", strerror(errno));
            status = -1;
            goto done;
        }
        if ((p[0].revents | p[1].revents | p[2].revents) & POLLNVAL) {
            detail_set(detail, detail_size, "poll-error");
            status = -1;
            goto done;
        }
        if (p[2].revents & (POLLIN | POLLHUP | POLLERR)) {
            detail_set(detail, detail_size, "stopped");
            ERR_clear_error();
            (void)SSL_shutdown(ssl); /* asked to end: close_notify, best effort */
            goto done;
        }
    }
done:
    if (down) OPENSSL_cleanse(down, EXTDISPLAY_BROKER_PUMP_BUFFER);
    if (up) OPENSSL_cleanse(up, EXTDISPLAY_BROKER_PUMP_BUFFER);
    free(down);
    free(up);
    return status;
}
