/* The extended-display broker (extdisplay-broker, tools/EXTDISPLAY.md): its control lines
 * with the frontend, the frozen cable TLS profile, the pairing code and the
 * TLS pump. extdisplay-broker-tls.c implements everything below that does not touch
 * namespaces, so the tests can run it on loopback. */
#ifndef LOCACHY_EXTDISPLAY_BROKER_PROTOCOL_H
#define LOCACHY_EXTDISPLAY_BROKER_PROTOCOL_H

#include <stddef.h>

/* Control: one ASCII line per SOCK_SEQPACKET message, no trailing newline. */
#define EXTDISPLAY_BROKER_LINE_MAX 512
#define EXTDISPLAY_BROKER_SCAN_MAX 16

/* The listener's TCP port inside the session namespace. The sender is run
 * with --port; this is the value the frontend shows next to the address. */
#define EXTDISPLAY_BROKER_PORT 27183

/* TLS 1.2, TLS_PSK_WITH_AES_128_GCM_SHA256 (0x00A8), identity and ALPN. */
#define EXTDISPLAY_BROKER_IDENTITY "locachy-extdisplay-v1"
#define EXTDISPLAY_BROKER_ALPN "locachy-extdisplay-v1"
#define EXTDISPLAY_BROKER_CIPHER "PSK-AES128-GCM-SHA256"
#define EXTDISPLAY_BROKER_CIPHER_ID 0x00A8

/* The pairing secret: 16 random bytes as RFC 4648 base32, 26 symbols (the
 * last carries 3 bits and two zero bits), uppercase, in groups of 4. */
#define EXTDISPLAY_BROKER_KEY_BYTES 16
#define EXTDISPLAY_BROKER_CODE_SYMBOLS 26
#define EXTDISPLAY_BROKER_CODE_GROUP 4
#define EXTDISPLAY_BROKER_CODE_TEXT 33 /* 26 symbols, 6 '-', NUL */

/* Waiting for the sender: from the code on screen to a finished handshake. */
#ifdef EXTDISPLAY_BROKER_TEST_ATTEMPT_SECONDS
#define EXTDISPLAY_BROKER_ATTEMPT_SECONDS EXTDISPLAY_BROKER_TEST_ATTEMPT_SECONDS
#else
#define EXTDISPLAY_BROKER_ATTEMPT_SECONDS 300
#endif
/* One accepted connection must finish its handshake within this. */
#define EXTDISPLAY_BROKER_HANDSHAKE_SECONDS 20

/* Pump buffers, per direction. */
#define EXTDISPLAY_BROKER_PUMP_BUFFER (64 * 1024)

/* Code text: 0 on success. Decoding follows the macOS sender's parser
 * exactly: case-insensitive, '-', space, tab, CR and LF ignored, exactly 26
 * symbols, and the two trailing bits must be zero (canonical). */
int extdisplay_broker_code_encode(const unsigned char key[EXTDISPLAY_BROKER_KEY_BYTES], char *text, size_t size);
int extdisplay_broker_code_decode(const char *text, size_t length, unsigned char key[EXTDISPLAY_BROKER_KEY_BYTES]);

#ifndef EXTDISPLAY_BROKER_NO_OPENSSL
#include <openssl/ssl.h>

/* The key of the current attempt, held by the server context (app data). */
struct extdisplay_broker_key {
    unsigned char key[EXTDISPLAY_BROKER_KEY_BYTES];
    int set;
};

/* Library setup without the system openssl.cnf, so the profile is exactly
 * the one below whatever the distribution configures. */
int extdisplay_broker_tls_init(void);
/* A server context for exactly the cable profile; key stays owned by the
 * caller and must outlive the context. NULL on any failure. */
SSL_CTX *extdisplay_broker_tls_context(struct extdisplay_broker_key *key);
/* Server handshake on a connected non-blocking socket, bounded by the
 * monotonic deadline in milliseconds. 0 when the handshake finished and the
 * negotiated version, suite, ALPN and identity are the profile's. */
int extdisplay_broker_tls_accept(SSL *ssl, int fd, long long deadline_ms);
int extdisplay_broker_tls_verify(SSL *ssl);
/* Bytes both ways between the TLS session and the plaintext stream until
 * either side ends, or stop (-1: none) becomes readable or hangs up, which
 * sends close_notify; bounded buffers. 0 on an orderly end. */
int extdisplay_broker_pump(SSL *ssl, int tcp, int plain, int stop);
long long extdisplay_broker_now_ms(void);
#endif

#endif
