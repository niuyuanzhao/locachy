# Independent loopback listener for the cable TLS profile (test only).
# Accepts TLS connections, enforces the expected PSK identity and key through
# the server-side PSK callback, and logs what was negotiated and received.
# A failed handshake is logged and the listener keeps serving; it stops only
# when killed. Usage:
#   tls-listener.py PORT KEY_HEX EXPECTED_IDENTITY ALPN CIPHERS VERSION LOG
import socket
import ssl
import sys

port, key_hex, expected, alpn, ciphers, version, log_path = sys.argv[1:8]
key = bytes.fromhex(key_hex)


def log(text):
    with open(log_path, "a", encoding="utf-8") as handle:
        handle.write(text + "\n")


def psk_for(identity):
    # Only the expected identity gets the key; anything else is refused.
    log("IDENTITY " + repr(identity))
    # Python passes the identity as str (None when absent).
    return key if identity == expected else b""


context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
if version == "1.2":
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.maximum_version = ssl.TLSVersion.TLSv1_2
else:
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
context.set_ciphers(ciphers)
context.set_psk_server_callback(psk_for)
context.set_alpn_protocols([alpn])

listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", int(port)))
listener.listen(8)
log("LISTENING")

while True:
    raw, _ = listener.accept()
    raw.settimeout(5)
    try:
        conn = context.wrap_socket(raw, server_side=True)
    except (ssl.SSLError, OSError) as error:
        log("HANDSHAKE-REFUSED " + type(error).__name__)
        raw.close()
        continue
    log("CIPHER " + repr(conn.cipher()[0]) + " VERSION " + conn.version())
    log("ALPN " + repr(conn.selected_alpn_protocol()))
    log("OFFERED " + " ".join(sorted(name for name, _, _ in (conn.shared_ciphers() or []))))
    received = b""
    try:
        while b'"type":"ping"' not in received:
            chunk = conn.recv(65536)
            if not chunk:
                break
            received += chunk
    except (ssl.SSLError, OSError) as error:
        log("RECEIVE-ERROR " + type(error).__name__)
    log("FRAME-PING " + str(b'"type":"ping"' in received) + " BYTES " + str(len(received)))
    conn.close()
