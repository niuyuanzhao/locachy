/* Versioned session checkpoints, independent of edit's screen and buffers.
 * json-glib handles JSON; byte-counted recovery buffers use canonical base64.
 * The directory is locked for each operation, never by a background service. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "edit-session.h"
#include <json-glib/json-glib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_PATH 4096u
#define MAX_LAYOUT 8192u
#define MAX_POSITION (1u << 30)
#define MAX_NAMES 4096u
#define SESSION_SUFFIX ".json"

static int valid_name(const char *s)
{
    if (!s || !*s || *s == '.' || strlen(s) > EDIT_SESSION_MAX_NAME ||
        !g_utf8_validate(s, -1, NULL)) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\') return 0;
    return 1;
}

static int filename(const char *name, char out[EDIT_SESSION_MAX_NAME + 6])
{
    if (!name) name = "default";
    if (!valid_name(name)) return EINVAL;
    snprintf(out, EDIT_SESSION_MAX_NAME + 6, "%s%s", name, SESSION_SUFFIX);
    return 0;
}

/* Traverse from a directory fd without following any component symlinks.
 * Existing home/.local/state may have normal readable modes; the application
 * directories must be private. All opens/mkdirs run with the user's fsuid. */
static int descend(int fd, const char *part, int create, int private, int *out)
{
    int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next < 0 && errno == ENOENT && create) {
        if (mkdirat(fd, part, 0700) && errno != EEXIST) return errno;
        if (fsync(fd)) return errno;
        next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (next < 0) return errno;
    struct stat st;
    int e = 0;
    if (fstat(next, &st)) e = errno;
    else if (st.st_uid != EDIT_USER || (st.st_mode & 0022) ||
             (private && (st.st_mode & 0777) != 0700)) e = EACCES;
    if (e) close(next); else *out = next;
    return e;
}

static int session_directory(int create, int *out)
{
    const char *home = NULL;
    if (getuid() == EDIT_USER) home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(EDIT_USER);
        if (!pw) return ENOENT;
        home = pw->pw_dir;
    }
    if (!home || home[0] != '/' || strlen(home) >= MAX_PATH) return EINVAL;
    char path[MAX_PATH];
    strcpy(path, home);
    /* Ancestors need only search permission (a 0711 parent is fine): walk
     * them with O_PATH, then open the home itself for reading. */
    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno;
    int e = 0;
    char *save = NULL;
    for (char *part = strtok_r(path, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { e = EINVAL; break; }
        int next = openat(fd, part, O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) { e = errno; break; }
        close(fd);
        fd = next;
    }
    if (!e) {
        int next = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (next < 0) e = errno;
        else { close(fd); fd = next; }
    }
    if (!e) {
        struct stat st;
        if (fstat(fd, &st)) e = errno;
        else if (st.st_uid != EDIT_USER || (st.st_mode & 0022)) e = EACCES;
    }
    const char *parts[] = {".local", "state", "locachy", "edit", "sessions"};
    for (size_t i = 0; !e && i < sizeof(parts) / sizeof(parts[0]); i++) {
        int next = -1;
        e = descend(fd, parts[i], create, i >= 2, &next);
        if (!e) { close(fd); fd = next; }
    }
    if (e) close(fd); else *out = fd;
    return e;
}

static int locked_directory(int create, int exclusive, int *out)
{
    int e = session_directory(create, out);
    if (!e && flock(*out, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB)) {
        e = errno;
        close(*out);
        *out = -1;
    }
    return e;
}

static int regular_file(int fd, struct stat *st)
{
    if (fstat(fd, st)) return errno;
    if (!S_ISREG(st->st_mode) || st->st_uid != EDIT_USER || st->st_nlink != 1 ||
        (st->st_mode & 0077)) return EACCES;
    if (st->st_size <= 0) return EINVAL;
    if ((uint64_t)st->st_size > EDIT_SESSION_MAX_FILE) return EFBIG;
    return 0;
}

static int read_checkpoint(int dir, const char *name, char **out, size_t *length, int writing)
{
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno;
    struct stat st;
    int e = regular_file(fd, &st);
    if (!e && writing && !(st.st_mode & S_IWUSR)) e = EACCES;
    char *data = NULL;
    size_t n = 0;
    if (!e) {
        n = (size_t)st.st_size;
        data = malloc(n + 1);
        if (!data) e = ENOMEM;
    }
    size_t at = 0;
    while (!e && at < n) {
        ssize_t got = read(fd, data + at, n - at);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { e = got ? errno : EIO; break; }
        at += (size_t)got;
    }
    if (!e) {
        char extra;
        ssize_t got;
        do got = read(fd, &extra, 1); while (got < 0 && errno == EINTR);
        if (got) e = got < 0 ? errno : EFBIG;
    }
    if (close(fd) && !e) e = errno;
    if (e) free(data);
    else { data[n] = 0; *out = data; *length = n; }
    return e;
}

static int decimal(const char **p, unsigned limit, unsigned *out)
{
    const char *s = *p;
    if (*s < '0' || *s > '9') return 0;
    unsigned n = 0;
    do {
        unsigned d = (unsigned)(*s++ - '0');
        if (n > limit / 10 || (n == limit / 10 && d > limit % 10)) return 0;
        n = 10 * n + d;
    } while (*s >= '0' && *s <= '9');
    if (**p == '0' && s != *p + 1) return 0;
    *p = s;
    *out = n;
    return 1;
}

static int layout_node(const char **p, size_t count, uint64_t *seen, unsigned depth)
{
    if (depth >= EDIT_SESSION_MAX_SPACES) return 0;
    unsigned value;
    if (**p >= '0' && **p <= '9') {
        if (!decimal(p, (unsigned)count - 1, &value) || (*seen & (UINT64_C(1) << value))) return 0;
        *seen |= UINT64_C(1) << value;
        return 1;
    }
    char kind = *(*p)++;
    if ((kind != 'V' && kind != 'H') || *(*p)++ != '(' ||
        !decimal(p, 9999, &value) || !value || *(*p)++ != ',') return 0;
    if (!layout_node(p, count, seen, depth + 1) || *(*p)++ != ',') return 0;
    if (!layout_node(p, count, seen, depth + 1) || *(*p)++ != ')') return 0;
    return 1;
}

static int valid_layout(const char *s, size_t spaces)
{
    if (!s || !*s || strlen(s) > MAX_LAYOUT || !spaces || spaces > EDIT_SESSION_MAX_SPACES) return 0;
    const char *p = s;
    uint64_t seen = 0;
    uint64_t all = spaces == 64 ? UINT64_MAX : (UINT64_C(1) << spaces) - 1;
    return layout_node(&p, spaces, &seen, 0) && !*p && seen == all;
}

static int bit(int n) { return n == 0 || n == 1; }

static int validate(const struct edit_session_state *s)
{
    if (!s || (s->ndocuments && !s->documents) || s->ndocuments > EDIT_SESSION_MAX_DOCUMENTS ||
        !s->spaces || !s->nspaces || s->nspaces > EDIT_SESSION_MAX_SPACES ||
        s->active < 0 || (size_t)s->active >= s->nspaces || !bit(s->sidebar) ||
        s->sidebar_width < 0 || s->sidebar_width > 65536 || !valid_layout(s->layout, s->nspaces)) return EINVAL;
    size_t total = 0, tabs = 0;
    unsigned char used[EDIT_SESSION_MAX_DOCUMENTS] = {0};
    for (size_t i = 0; i < s->ndocuments; i++) {
        const struct edit_session_document *d = s->documents + i;
        if (d->path && *d->path && (d->path[0] != '/' || strlen(d->path) >= MAX_PATH ||
            !g_utf8_validate(d->path, -1, NULL))) return EINVAL;
        if (!bit(d->modified) || !bit(d->readonly) || d->format.encoding < 0 || d->format.encoding >= ENC_COUNT ||
            !bit(d->format.bom) || d->format.eol < EOL_LF || d->format.eol > EOL_CR ||
            d->length > MAX_POSITION) return EINVAL;
        size_t base = strnlen(d->base, sizeof d->base);
        if (base == sizeof d->base || (base && (base != 2 * DIGEST_SIZE || !d->modified || !d->path || !*d->path)))
            return EINVAL;
        for (size_t k = 0; k < base; k++)
            if (!((d->base[k] >= '0' && d->base[k] <= '9') || (d->base[k] >= 'a' && d->base[k] <= 'f'))) return EINVAL;
        if (d->modified || !d->path || !*d->path) {
            if ((!d->text && d->length) || d->length > EDIT_SESSION_MAX_BUFFER ||
                d->length > EDIT_SESSION_MAX_TEXT - total) return EFBIG;
            total += d->length;
        }
    }
    for (size_t i = 0; i < s->nspaces; i++) {
        const struct edit_session_space *sp = s->spaces + i;
        if ((sp->ntabs && !sp->tabs) || sp->ntabs > EDIT_SESSION_MAX_TABS - tabs ||
            (sp->ntabs ? (sp->current < 0 || (size_t)sp->current >= sp->ntabs) : sp->current != -1)) return EINVAL;
        tabs += sp->ntabs;
        for (size_t j = 0; j < sp->ntabs; j++) {
            const struct edit_session_view *v = sp->tabs + j;
            if (v->document < 0 || (size_t)v->document >= s->ndocuments ||
                v->cursor > s->documents[v->document].length || v->anchor > s->documents[v->document].length ||
                v->top > MAX_POSITION || v->left < 0 || v->left > (int)MAX_POSITION ||
                v->sub < 0 || v->sub > (int)MAX_POSITION || !bit(v->wrap) || !bit(v->numbers) ||
                (v->font_px && (v->font_px < 12 || v->font_px > 96))) return EINVAL;
            for (size_t k = 0; k < j; k++)
                if (sp->tabs[k].document == v->document) return EINVAL;
            used[v->document] = 1;
        }
    }
    for (size_t i = 0; i < s->ndocuments; i++) if (!used[i]) return EINVAL;
    return 0;
}

void edit_session_free(struct edit_session_state *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->ndocuments; i++) {
        free(s->documents[i].path);
        free(s->documents[i].text);
    }
    free(s->documents);
    for (size_t i = 0; i < s->nspaces; i++) free(s->spaces[i].tabs);
    free(s->spaces);
    free(s->layout);
    memset(s, 0, sizeof(*s));
}

static int fields(JsonObject *o, const char *const *names, size_t n)
{
    if (!o || json_object_get_size(o) != n) return 0;
    for (size_t i = 0; i < n; i++) if (!json_object_has_member(o, names[i])) return 0;
    return 1;
}
#define FIELDS(o, ...) fields((o), (const char *const[]){__VA_ARGS__}, sizeof((const char *const[]){__VA_ARGS__}) / sizeof(char *))

static int integer(JsonObject *o, const char *key, gint64 low, gint64 high, gint64 *out)
{
    JsonNode *n = json_object_get_member(o, key);
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return 0;
    gint64 value = json_node_get_int(n);
    if (value < low || value > high) return 0;
    *out = value;
    return 1;
}

static int boolean(JsonObject *o, const char *key, int *out)
{
    JsonNode *n = json_object_get_member(o, key);
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN) return 0;
    *out = json_node_get_boolean(n);
    return 1;
}

static const char *string(JsonObject *o, const char *key, size_t maximum)
{
    JsonNode *n = json_object_get_member(o, key);
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
    const char *s = json_node_get_string(n);
    return s && strlen(s) <= maximum ? s : NULL;
}

static JsonArray *array(JsonObject *o, const char *key, size_t maximum)
{
    JsonNode *n = json_object_get_member(o, key);
    if (!n || !JSON_NODE_HOLDS_ARRAY(n)) return NULL;
    JsonArray *a = json_node_get_array(n);
    guint count = json_array_get_length(a);
    return count <= maximum ? a : NULL;
}

static JsonObject *object_at(JsonArray *a, size_t i)
{
    JsonNode *n = json_array_get_element(a, (guint)i);
    return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
}

/* Reject duplicate keys rather than accepting the last interpretation. */
struct parse_check { GHashTable *objects; int duplicate; };
static void member_seen(JsonParser *parser, JsonObject *object, const char *member, gpointer data)
{
    (void)parser;
    struct parse_check *check = data;
    GHashTable *names = g_hash_table_lookup(check->objects, object);
    if (!names) {
        names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_insert(check->objects, object, names);
    }
    if (!g_hash_table_add(names, g_strdup(member))) check->duplicate = 1;
}

static int decode(const char *data, size_t length, struct edit_session_state *s)
{
    /* json-glib's string API is NUL-terminated. This format never emits NUL
     * escapes (buffer bytes are base64), so reject them before parsing rather
     * than accidentally validating only a truncated path/member/string. */
    if (memchr(data, 0, length) || !g_utf8_validate(data, (gssize)length, NULL)) return EINVAL;
    for (size_t i = 0; i + 6 <= length; i++) {
        if (memcmp(data + i, "\\u0000", 6)) continue;
        size_t preceding = 0;
        while (i > preceding && data[i - preceding - 1] == '\\') preceding++;
        if (!(preceding & 1)) return EINVAL;
    }
    JsonParser *parser = json_parser_new();
    json_parser_set_strict(parser, TRUE);
    struct parse_check check = {g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_hash_table_unref), 0};
    g_signal_connect(parser, "object-member", G_CALLBACK(member_seen), &check);
    int e = EINVAL;
    gint64 value;
    if (!json_parser_load_from_data(parser, data, (gssize)length, NULL) || check.duplicate) goto done;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) goto done;
    JsonObject *o = json_node_get_object(root);
    int clean;
    if (!FIELDS(o, "version", "clean", "active", "sidebar", "sidebar_width", "layout", "documents", "spaces") ||
        !integer(o, "version", 1, 1, &value) || !boolean(o, "clean", &clean) ||
        !integer(o, "active", 0, EDIT_SESSION_MAX_SPACES - 1, &value)) goto done;
    s->active = (int)value;
    s->recovered = !clean;
    if (!boolean(o, "sidebar", &s->sidebar) || !integer(o, "sidebar_width", 0, 65536, &value)) goto done;
    s->sidebar_width = (int)value;
    const char *layout = string(o, "layout", MAX_LAYOUT);
    JsonArray *docs = array(o, "documents", EDIT_SESSION_MAX_DOCUMENTS);
    JsonArray *spaces = array(o, "spaces", EDIT_SESSION_MAX_SPACES);
    if (!layout || !docs || !spaces) goto done;
    s->layout = strdup(layout);
    s->documents = calloc(json_array_get_length(docs) ? json_array_get_length(docs) : 1, sizeof(*s->documents));
    s->spaces = calloc(json_array_get_length(spaces) ? json_array_get_length(spaces) : 1, sizeof(*s->spaces));
    if (!s->layout || !s->documents || !s->spaces) { e = ENOMEM; goto done; }
    s->ndocuments = json_array_get_length(docs);
    s->nspaces = json_array_get_length(spaces);
    size_t total = 0;
    for (size_t i = 0; i < s->ndocuments; i++) {
        JsonObject *d = object_at(docs, i);
        struct edit_session_document *out = s->documents + i;
        if (!FIELDS(d, "path", "length", "encoding", "bom", "eol", "modified", "readonly", "base", "data") ||
            !boolean(d, "modified", &out->modified) || !boolean(d, "readonly", &out->readonly) ||
            !integer(d, "length", 0, MAX_POSITION, &value)) goto done;
        out->length = (size_t)value;
        if (!integer(d, "encoding", 0, ENC_COUNT - 1, &value)) goto done;
        out->format.encoding = (int)value;
        if (!boolean(d, "bom", &out->format.bom) || !integer(d, "eol", EOL_LF, EOL_CR, &value)) goto done;
        out->format.eol = (int)value;
        JsonNode *path = json_object_get_member(d, "path");
        if (!JSON_NODE_HOLDS_NULL(path)) {
            const char *p = string(d, "path", MAX_PATH - 1);
            if (!p || !*p || *p != '/') goto done;
            out->path = strdup(p);
            if (!out->path) { e = ENOMEM; goto done; }
        }
        JsonNode *base = json_object_get_member(d, "base");
        if (!JSON_NODE_HOLDS_NULL(base)) {
            const char *hex = string(d, "base", 2 * DIGEST_SIZE);
            if (!hex || strlen(hex) != 2 * DIGEST_SIZE) goto done;
            memcpy(out->base, hex, 2 * DIGEST_SIZE + 1);
        }
        if (out->modified || !out->path) {
            if (out->length > EDIT_SESSION_MAX_BUFFER || out->length > EDIT_SESSION_MAX_TEXT - total) { e = EFBIG; goto done; }
            total += out->length;
            const char *encoded = string(d, "data", 4 * ((out->length + 2) / 3));
            if (!encoded || strlen(encoded) != 4 * ((out->length + 2) / 3)) goto done;
            gsize n;
            guchar *bytes = g_base64_decode(encoded, &n);
            char *canonical = g_base64_encode(bytes, n);
            int valid = n == out->length && !strcmp(encoded, canonical);
            g_free(canonical);
            if (!valid) { g_free(bytes); goto done; }
            out->text = malloc(n + 1);
            if (!out->text) { g_free(bytes); e = ENOMEM; goto done; }
            memcpy(out->text, bytes, n);
            out->text[n] = 0;
            g_free(bytes);
        } else if (!JSON_NODE_HOLDS_NULL(json_object_get_member(d, "data"))) goto done;
    }
    size_t tabs = 0;
    for (size_t i = 0; i < s->nspaces; i++) {
        JsonObject *sp = object_at(spaces, i);
        if (!FIELDS(sp, "current", "tabs") || !integer(sp, "current", -1, EDIT_SESSION_MAX_TABS - 1, &value)) goto done;
        struct edit_session_space *out = s->spaces + i;
        out->current = (int)value;
        JsonArray *a = array(sp, "tabs", EDIT_SESSION_MAX_TABS - tabs);
        if (!a) goto done;
        size_t count = json_array_get_length(a);
        out->tabs = calloc(count ? count : 1, sizeof(*out->tabs));
        if (!out->tabs) { e = ENOMEM; goto done; }
        out->ntabs = count;
        tabs += count;
        for (size_t j = 0; j < count; j++) {
            JsonObject *v = object_at(a, j);
            struct edit_session_view *t = out->tabs + j;
            if (!FIELDS(v, "document", "cursor", "anchor", "top", "left", "sub", "wrap", "numbers", "font_px") ||
                !integer(v, "document", 0, s->ndocuments ? (gint64)s->ndocuments - 1 : -1, &value)) goto done;
            t->document = (int)value;
            if (!integer(v, "cursor", 0, MAX_POSITION, &value)) goto done;
            t->cursor = (size_t)value;
            if (!integer(v, "anchor", 0, MAX_POSITION, &value)) goto done;
            t->anchor = (size_t)value;
            if (!integer(v, "top", 0, MAX_POSITION, &value)) goto done;
            t->top = (size_t)value;
            if (!integer(v, "left", 0, MAX_POSITION, &value)) goto done;
            t->left = (int)value;
            if (!integer(v, "sub", 0, MAX_POSITION, &value)) goto done;
            t->sub = (int)value;
            if (!boolean(v, "wrap", &t->wrap) || !boolean(v, "numbers", &t->numbers) ||
                !integer(v, "font_px", 0, 96, &value)) goto done;
            t->font_px = (int)value;
        }
    }
    e = validate(s);
done:
    g_hash_table_unref(check.objects);
    g_object_unref(parser);
    if (e) edit_session_free(s);
    return e;
}

static void put_int(JsonBuilder *b, const char *key, gint64 n)
{
    json_builder_set_member_name(b, key);
    json_builder_add_int_value(b, n);
}
static void put_bool(JsonBuilder *b, const char *key, int n)
{
    json_builder_set_member_name(b, key);
    json_builder_add_boolean_value(b, n);
}
static void put_string(JsonBuilder *b, const char *key, const char *s)
{
    json_builder_set_member_name(b, key);
    if (s) json_builder_add_string_value(b, s); else json_builder_add_null_value(b);
}

static int encode(const struct edit_session_state *s, int clean, char **data, size_t *length)
{
    int e = validate(s);
    if (e || !bit(clean)) return e ? e : EINVAL;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    put_int(b, "version", 1);
    put_bool(b, "clean", clean);
    put_int(b, "active", s->active);
    put_bool(b, "sidebar", s->sidebar);
    put_int(b, "sidebar_width", s->sidebar_width);
    put_string(b, "layout", s->layout);
    json_builder_set_member_name(b, "documents");
    json_builder_begin_array(b);
    for (size_t i = 0; i < s->ndocuments; i++) {
        const struct edit_session_document *d = s->documents + i;
        json_builder_begin_object(b);
        put_string(b, "path", d->path && *d->path ? d->path : NULL);
        put_int(b, "length", (gint64)d->length);
        put_int(b, "encoding", d->format.encoding);
        put_bool(b, "bom", d->format.bom);
        put_int(b, "eol", d->format.eol);
        put_bool(b, "modified", d->modified);
        put_bool(b, "readonly", d->readonly);
        put_string(b, "base", d->base[0] ? d->base : NULL);
        char *encoded = NULL;
        if (d->modified || !d->path || !*d->path)
            encoded = g_base64_encode((const guchar *)(d->text ? d->text : ""), d->length);
        put_string(b, "data", encoded);
        g_free(encoded);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "spaces");
    json_builder_begin_array(b);
    for (size_t i = 0; i < s->nspaces; i++) {
        const struct edit_session_space *sp = s->spaces + i;
        json_builder_begin_object(b);
        put_int(b, "current", sp->current);
        json_builder_set_member_name(b, "tabs");
        json_builder_begin_array(b);
        for (size_t j = 0; j < sp->ntabs; j++) {
            const struct edit_session_view *v = sp->tabs + j;
            json_builder_begin_object(b);
            put_int(b, "document", v->document);
            put_int(b, "cursor", (gint64)v->cursor);
            put_int(b, "anchor", (gint64)v->anchor);
            put_int(b, "top", (gint64)v->top);
            put_int(b, "left", v->left);
            put_int(b, "sub", v->sub);
            put_bool(b, "wrap", v->wrap);
            put_bool(b, "numbers", v->numbers);
            put_int(b, "font_px", v->font_px);
            json_builder_end_object(b);
        }
        json_builder_end_array(b);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    JsonNode *root = json_builder_get_root(b);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    gsize n;
    char *bytes = json_generator_to_data(generator, &n);
    g_object_unref(generator);
    json_node_free(root);
    g_object_unref(b);
    if (!bytes) return ENOMEM;
    if (n > EDIT_SESSION_MAX_FILE) { g_free(bytes); return EFBIG; }
    *data = bytes;
    *length = n;
    return 0;
}

static void temporary_name(char out[80], const char *kind)
{
    snprintf(out, 80, ".%s-%lu-%08x%08x", kind, (unsigned long)getpid(), g_random_int(), g_random_int());
}

static int atomic_write(int dir, const char *name, const char *data, size_t length,
                        const char *previous, size_t previous_length)
{
    char temp[80] = "", backup[80] = "";
    int fd = -1, e = EEXIST;
    for (int tries = 0; tries < 32; tries++) {
        temporary_name(temp, "checkpoint");
        fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) break;
        e = errno;
        if (e != EEXIST) return e;
    }
    if (fd < 0) return e;
    size_t at = 0;
    e = 0;
    while (at < length) {
        ssize_t put = write(fd, data + at, length - at);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) { e = put ? errno : EIO; break; }
        at += (size_t)put;
    }
    if (!e && fchmod(fd, 0600)) e = errno;
    if (!e && fsync(fd)) e = errno;
    if (close(fd) && !e) e = errno;
    int have_backup = 0;
    if (!e && previous) {
        int old = -1;
        for (int tries = 0; tries < 32; tries++) {
            temporary_name(backup, "previous");
            old = openat(dir, backup, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (old >= 0) break;
            e = errno;
            if (e != EEXIST) break;
        }
        if (old >= 0) {
            have_backup = 1;
            e = 0;
            size_t pos = 0;
            while (pos < previous_length) {
                ssize_t put = write(old, previous + pos, previous_length - pos);
                if (put < 0 && errno == EINTR) continue;
                if (put <= 0) { e = put ? errno : EIO; break; }
                pos += (size_t)put;
            }
            if (!e && fchmod(old, 0600)) e = errno;
            if (!e && fsync(old)) e = errno;
            if (close(old) && !e) e = errno;
        }
        if (!e && fsync(dir)) e = errno;
    }
    int renamed = 0;
    if (!e) {
        if (renameat(dir, temp, dir, name)) e = errno;
        else renamed = 1;
    }
    if (!e && fsync(dir)) e = errno;
    if (e && renamed) {
        /* A directory-sync error must not turn a reported failed save into
         * loss of the last valid checkpoint. Keep the backup if rollback
         * itself fails; never unlink the only remaining old copy. */
        if (have_backup) {
            if (!renameat(dir, backup, dir, name)) { have_backup = 0; (void)fsync(dir); }
            else return e;
        } else { (void)unlinkat(dir, name, 0); (void)fsync(dir); }
    }
    (void)unlinkat(dir, temp, 0);
    if (have_backup) { (void)unlinkat(dir, backup, 0); (void)fsync(dir); }
    return e;
}

static int save_session(const char *name, const struct edit_session_state *state, int clean, int create)
{
    char namebuf[EDIT_SESSION_MAX_NAME + 6];
    int e = filename(name, namebuf);
    char *data = NULL;
    size_t length;
    if (!e) e = encode(state, clean, &data, &length);
    if (e) return e;
    file_as_user(1);
    int dir = -1;
    e = locked_directory(1, 1, &dir);
    char *previous = NULL;
    size_t previous_length = 0;
    if (!e && create) {
        struct stat st;
        if (!fstatat(dir, namebuf, &st, AT_SYMLINK_NOFOLLOW)) e = EEXIST;
        else if (errno != ENOENT) e = errno;
    }
    if (!e) {
        e = read_checkpoint(dir, namebuf, &previous, &previous_length, 1);
        if (e == ENOENT) e = 0;
        else if (!e) {
            struct edit_session_state old = {0};
            e = decode(previous, previous_length, &old);
            edit_session_free(&old);
        }
    }
    if (!e) e = atomic_write(dir, namebuf, data, length, previous, previous_length);
    free(previous);
    if (dir >= 0) close(dir);
    file_as_user(0);
    g_free(data);
    return e;
}

int edit_session_save(const char *name, const struct edit_session_state *state, int clean)
{
    return save_session(name, state, clean, 0);
}

int edit_session_create(const char *name, const struct edit_session_state *state, int clean)
{
    return save_session(name, state, clean, 1);
}

int edit_session_load(const char *name, struct edit_session_state *state)
{
    if (!state) return EINVAL;
    char namebuf[EDIT_SESSION_MAX_NAME + 6];
    int e = filename(name, namebuf);
    if (e) return e;
    file_as_user(1);
    int dir = -1;
    char *data = NULL;
    size_t n = 0;
    e = locked_directory(0, 0, &dir);
    if (!e) e = read_checkpoint(dir, namebuf, &data, &n, 0);
    if (dir >= 0) close(dir);
    file_as_user(0);
    struct edit_session_state loaded = {0};
    if (!e) e = decode(data, n, &loaded);
    free(data);
    if (!e) { edit_session_free(state); *state = loaded; }
    return e;
}

int edit_session_remove(const char *name)
{
    char namebuf[EDIT_SESSION_MAX_NAME + 6];
    int e = filename(name, namebuf);
    if (e) return e;
    file_as_user(1);
    int dir = -1;
    e = locked_directory(0, 1, &dir);
    if (!e) {
        struct stat st;
        if (fstatat(dir, namebuf, &st, AT_SYMLINK_NOFOLLOW)) e = errno;
        else if (!S_ISREG(st.st_mode) || st.st_uid != EDIT_USER || st.st_nlink != 1 || (st.st_mode & 0077)) e = EACCES;
        else {
            char removed[80];
            int fd = -1;
            for (int tries = 0; tries < 32; tries++) {
                temporary_name(removed, "removed");
                fd = openat(dir, removed, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                if (fd >= 0 || errno != EEXIST) break;
            }
            if (fd < 0) e = errno;
            else {
                close(fd);
                if (renameat(dir, namebuf, dir, removed)) e = errno;
                else if (fsync(dir)) {
                    e = errno;
                    if (renameat(dir, removed, dir, namebuf)) {
                        /* Keep the last copy if a failing filesystem also
                         * rejects rollback. Never unlink it on error. */
                        close(dir);
                        file_as_user(0);
                        return e;
                    }
                    (void)fsync(dir);
                }
                /* The synced rename already made the removal durable.
                 * Cleanup cannot turn a successful removal into failure. */
                (void)unlinkat(dir, removed, 0);
                (void)fsync(dir);
            }
        }
    }
    if (dir >= 0) close(dir);
    file_as_user(0);
    return e;
}

static int decode_selection(const char *data, size_t length, char **out)
{
    if (length > 1024 || memchr(data, 0, length) ||
        !g_utf8_validate(data, (gssize)length, NULL)) return EINVAL;
    JsonParser *parser = json_parser_new();
    json_parser_set_strict(parser, TRUE);
    struct parse_check check = {g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_hash_table_unref), 0};
    g_signal_connect(parser, "object-member", G_CALLBACK(member_seen), &check);
    int e = EINVAL;
    if (json_parser_load_from_data(parser, data, (gssize)length, NULL) && !check.duplicate) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *o = json_node_get_object(root);
            gint64 version;
            const char *name = string(o, "name", EDIT_SESSION_MAX_NAME);
            if (FIELDS(o, "version", "name") && integer(o, "version", 1, 1, &version) && name && valid_name(name)) {
                /* Compare against canonical output too: json-glib strings
                 * must not let a NUL escape truncate the selected name. */
                const char *nul = strstr(data, "\\u0000");
                if (!nul) {
                    *out = strdup(name);
                    e = *out ? 0 : ENOMEM;
                }
            }
        }
    }
    g_hash_table_unref(check.objects);
    g_object_unref(parser);
    return e;
}

int edit_session_selected(char **name)
{
    if (!name) return EINVAL;
    *name = NULL;
    file_as_user(1);
    int dir = -1;
    int e = locked_directory(0, 0, &dir);
    char *data = NULL;
    size_t length = 0;
    if (!e) e = read_checkpoint(dir, ".selected", &data, &length, 0);
    if (dir >= 0) close(dir);
    file_as_user(0);
    if (!e) e = decode_selection(data, length, name);
    free(data);
    return e;
}

int edit_session_select(const char *name)
{
    if (!name) name = "default";
    if (!valid_name(name)) return EINVAL;
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    put_int(builder, "version", 1);
    put_string(builder, "name", name);
    json_builder_end_object(builder);
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    gsize length;
    char *data = json_generator_to_data(generator, &length);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    if (!data) return ENOMEM;
    file_as_user(1);
    int dir = -1;
    int e = locked_directory(1, 1, &dir);
    char *previous = NULL, *old_name = NULL;
    size_t old_length = 0;
    if (!e) {
        e = read_checkpoint(dir, ".selected", &previous, &old_length, 1);
        if (e == ENOENT) e = 0;
        else if (!e) e = decode_selection(previous, old_length, &old_name);
    }
    if (!e) e = atomic_write(dir, ".selected", data, length, previous, old_length);
    free(previous);
    free(old_name);
    if (dir >= 0) close(dir);
    file_as_user(0);
    g_free(data);
    return e;
}

void edit_session_list_free(char **names, size_t count)
{
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int edit_session_list(char ***names, size_t *count)
{
    if (!names || !count) return EINVAL;
    *names = NULL;
    *count = 0;
    file_as_user(1);
    int dir = -1;
    int e = locked_directory(0, 0, &dir);
    if (e == ENOENT) { file_as_user(0); return 0; }
    DIR *stream = NULL;
    char **items = NULL;
    size_t n = 0;
    if (!e) {
        stream = fdopendir(dir);
        if (!stream) e = errno;
        else dir = -1;
    }
    while (!e) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) { if (errno) e = errno; break; }
        size_t len = strlen(entry->d_name);
        if (len <= 5 || len > EDIT_SESSION_MAX_NAME + 5 || strcmp(entry->d_name + len - 5, SESSION_SUFFIX)) continue;
        char name[EDIT_SESSION_MAX_NAME + 1];
        memcpy(name, entry->d_name, len - 5);
        name[len - 5] = 0;
        if (!valid_name(name)) continue;
        struct stat st;
        if (fstatat(dirfd(stream), entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { e = errno; break; }
        if (!S_ISREG(st.st_mode) || st.st_uid != EDIT_USER || st.st_nlink != 1 || (st.st_mode & 0077)) { e = EACCES; break; }
        if (n == MAX_NAMES) { e = EFBIG; break; }
        char **grown = realloc(items, (n + 1) * sizeof(*items));
        if (!grown) { e = ENOMEM; break; }
        items = grown;
        items[n] = strdup(name);
        if (!items[n]) { e = ENOMEM; break; }
        n++;
    }
    if (stream) closedir(stream);
    if (dir >= 0) close(dir);
    file_as_user(0);
    if (e) edit_session_list_free(items, n);
    else { if (n > 1) qsort(items, n, sizeof(*items), compare_names); *names = items; *count = n; }
    return e;
}
