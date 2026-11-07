/*
 * The private-file drag's shared pieces (tools/DOCUMENTS.md), for files,
 * locachy-docs and container-documents: the host document service's paths,
 * the FileTransfer key a drag offers, a container's document-portal app id
 * and the kinds of peers the service tells apart. libc only.
 *
 * The control socket (SOCK_SEQPACKET, one request per message, root only):
 *   begin\nNAME...             with one O_PATH fd per NAME (at most
 *   begin-more SESSION\nNAME...  DOCS_FDS_MAX a message), in that order:
 *                              documents nobody may read yet; "ok SESSION\n"
 *                              and one container path a line, or "error WHY"
 *                              (every document of the session is gone then)
 *   end SESSION done|cancelled the drag ended; cancelled deletes them all.
 *                              No answer: it is the connection's last
 *                              message (files closes it right after)
 *   drop PID container NAME    from labwc, before it lets the drop through:
 *   drop PID host|xwayland     "ok" once container NAME may read the
 *                              documents of PID's drag, or "refuse WHY"
 *   view NAME                  "ok" with an O_PATH fd of the document
 *                              portal's view for container NAME
 */
#ifndef LOCACHY_DOCS_PROTO_H
#define LOCACHY_DOCS_PROTO_H
#include <stddef.h>
#include <string.h>

#ifndef DOCS_DIR
#define DOCS_DIR "/run/locachy/docs"
#endif
#define DOCS_CONTROL DOCS_DIR "/control"
#define DOCS_VIEWS DOCS_DIR "/views"   /* NAME: container NAME's view, a shared mount */
#define DOCS_FDS_MAX 16
#define DOCS_MSG_MAX 16384
#define DOCS_KEY_PREFIX "LOCACHYDOC"
#define DOCS_MIME_TRANSFER "application/vnd.portal.filetransfer"
#define DOCS_MIME_FILES "application/vnd.portal.files" /* GTK 4.6 and older */
#define DOCS_APP_PREFIX "org.locachy.app."

/* A container's name, as link/locachy-app has them: a letter or digit, then
 * letters, digits, '.', '_' or '-'; 64 at most. */
static inline int docs_valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '.' && c != '_' && c != '-'))) return 0;
    }
    return 1;
}

/* A native window's name, window-PROGRAM-N (process 1's, init/apps.c; its
 * cgroup app_window-PROGRAM-N): a root program of the host on the host's own
 * Wayland socket, never a container (link/locachy-app refuses such names). */
static inline int docs_window_name(const char *name)
{
    return strncmp(name, "window-", 7) == 0;
}

/* A container's name: a valid name that is not a native window's. */
static inline int docs_container_name(const char *name)
{
    return docs_valid_name(name) && !docs_window_name(name);
}

/* Container NAME's app id at the document portal: DOCS_APP_PREFIX and NAME
 * with '_' as "__" and '.' as "_d", after a '_' when NAME begins with a digit
 * (an element of an app id cannot; '-' may be in the last one). Two names
 * never get the same id. -1 if NAME is no container name. */
static inline int docs_app_id(const char *name, char *out, size_t size)
{
    if (!docs_container_name(name) || size < sizeof DOCS_APP_PREFIX + 2 * strlen(name) + 1) return -1;
    size_t n = sizeof DOCS_APP_PREFIX - 1;
    memcpy(out, DOCS_APP_PREFIX, n);
    if (name[0] >= '0' && name[0] <= '9') out[n++] = '_';
    for (const char *p = name; *p; p++) {
        if (*p == '_' || *p == '.') {
            out[n++] = '_';
            out[n++] = *p == '_' ? '_' : 'd';
        } else {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return 0;
}

/* The key: DOCS_KEY_PREFIX and base64url without padding (RFC 4648 section
 * 5) of the paths, NUL between them. No ':' (Firefox takes data that parses
 * as a URI for something else), plain ASCII (a D-Bus string). Its length
 * with the terminating NUL. -1 if it does not fit in size. */
static inline long docs_key_encode(const char *data, size_t n, char *out, size_t size)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t len = sizeof DOCS_KEY_PREFIX - 1;
    if (n > ((size_t)-1 - 64) / 4 || size < len + (n * 4 + 2) / 3 + 1) return -1;
    memcpy(out, DOCS_KEY_PREFIX, len);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)data[i] << 16;
        if (i + 1 < n) v |= (unsigned char)data[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)data[i + 2];
        out[len++] = abc[v >> 18 & 63];
        out[len++] = abc[v >> 12 & 63];
        if (i + 1 < n) out[len++] = abc[v >> 6 & 63];
        if (i + 2 < n) out[len++] = abc[v & 63];
    }
    out[len] = '\0';
    return (long)len + 1;
}

/* The bytes of a key (NUL between the paths), into out; their count, or -1
 * for anything that is not a key docs_key_encode made: another prefix, a
 * character outside the alphabet, padding, a length no encoding has, or
 * bits set past the last byte. */
static inline long docs_key_decode(const char *key, char *out, size_t size)
{
    size_t plen = sizeof DOCS_KEY_PREFIX - 1;
    if (strncmp(key, DOCS_KEY_PREFIX, plen) != 0) return -1;
    const char *s = key + plen;
    size_t n = strlen(s), len = 0;
    if (n % 4 == 1 || size < n / 4 * 3 + 2) return -1;
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        unsigned d = c >= 'A' && c <= 'Z' ? (unsigned)(c - 'A') :
                     c >= 'a' && c <= 'z' ? (unsigned)(c - 'a' + 26) :
                     c >= '0' && c <= '9' ? (unsigned)(c - '0' + 52) :
                     c == '-' ? 62u : c == '_' ? 63u : 64u;
        if (d == 64) return -1;
        v = v << 6 | d;
        if (i % 4 == 3) {
            out[len++] = (char)(v >> 16);
            out[len++] = (char)(v >> 8);
            out[len++] = (char)v;
            v = 0;
        }
    }
    if (n % 4 == 2) {
        if (v & 15) return -1;
        out[len++] = (char)(v >> 4);
    } else if (n % 4 == 3) {
        if (v & 3) return -1;
        out[len++] = (char)(v >> 10);
        out[len++] = (char)(v >> 2);
    }
    return (long)len;
}

/* Who a peer of the control socket is, by the first line of its
 * /proc/PID/cgroup ("0::/PATH"): a container's process (app_NAME), a native
 * window's (app_window-PROGRAM-N, root programs such as files), or
 * anything else of the host. DOCS_PEER_UNKNOWN without a cgroup v2 line. */
enum { DOCS_PEER_HOST, DOCS_PEER_WINDOW, DOCS_PEER_CONTAINER, DOCS_PEER_UNKNOWN };
static inline int docs_peer_kind(const char *line)
{
    if (strncmp(line, "0::/", 4) != 0) return DOCS_PEER_UNKNOWN;
    const char *path = line + 4;
    if (strncmp(path, "app_", 4) != 0) return DOCS_PEER_HOST;
    return docs_window_name(path + 4) ? DOCS_PEER_WINDOW : DOCS_PEER_CONTAINER;
}

#endif
