/* Runs only in the disposable Linux root. No framebuffer or real editor. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../progs/edit-session.h"
#include <json-glib/json-glib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #x, errno); exit(1); } } while (0)
static char directory[4096], account_home[128];
struct passwd *__real_getpwuid(uid_t uid);
struct passwd *__wrap_getpwuid(uid_t uid)
{
    struct passwd *pw = __real_getpwuid(uid);
    if (!pw || uid != EDIT_USER || !*account_home) return pw;
    static struct passwd isolated;
    isolated = *pw;
    isolated.pw_dir = account_home;
    return &isolated;
}
static int inject_write, inject_rename, inject_fsync, directory_syncs, interrupt_write, short_write;
static int write_calls, rename_calls, crash_write, crash_rename;
ssize_t __real_write(int fd, const void *data, size_t size);
int __real_renameat(int olddir, const char *old, int newdir, const char *new);
int __real_fsync(int fd);
ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    write_calls++;
    if (interrupt_write && write_calls == interrupt_write) { errno = EINTR; return -1; }
    if (short_write && size > 7) size = 7;
    if (crash_write && write_calls == crash_write) {
        (void)__real_write(fd, data, size > 1 ? size / 2 : size);
        _exit(81);
    }
    if (inject_write && write_calls == inject_write) { errno = ENOSPC; return -1; }
    return __real_write(fd, data, size);
}
int __wrap_renameat(int olddir, const char *old, int newdir, const char *new)
{
    rename_calls++;
    if (inject_rename && rename_calls == inject_rename) { errno = EIO; return -1; }
    if (crash_rename && rename_calls == crash_rename) {
        if (__real_renameat(olddir, old, newdir, new)) _exit(83);
        _exit(82);
    }
    return __real_renameat(olddir, old, newdir, new);
}
int __wrap_fsync(int fd)
{
    struct stat st;
    CHECK(fstat(fd, &st) == 0);
    if (S_ISDIR(st.st_mode)) {
        directory_syncs++;
        if (inject_fsync && directory_syncs == inject_fsync) { errno = EIO; return -1; }
    }
    return __real_fsync(fd);
}

static struct edit_session_state sample(void)
{
    struct edit_session_state s = {0};
    s.ndocuments = 3;
    s.documents = calloc(s.ndocuments, sizeof(*s.documents));
    CHECK(s.documents);
    static const char bytes[] = "\xe4\xb8\xad\xe6\x96\x87\0\xf0\x9f\x8c\x8d\nsecond\r\nlast\r";
    s.documents[0] = (struct edit_session_document){
        .path = strdup("/home/tester/source file.c"), .text = malloc(sizeof(bytes)),
        .length = sizeof(bytes) - 1, .format = {ENC_UTF16BE, 1, EOL_CRLF}, .modified = 1
    };
    CHECK(s.documents[0].path && s.documents[0].text);
    strcpy(s.documents[0].base, "0123456789abcdef0123456789abcdef01234567");
    memcpy(s.documents[0].text, bytes, sizeof(bytes));
    s.documents[1] = (struct edit_session_document){
        .text = strdup("untitled\n\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\n"), .length = 19,
        .format = {ENC_GB18030, 0, EOL_CR}, .readonly = 1
    };
    s.documents[1].length = strlen(s.documents[1].text);
    s.documents[2] = (struct edit_session_document){
        .path = strdup("/home/tester/clean\xe4\xb8\xad.txt"), .text = strdup("do not store unmodified bytes"),
        .length = 28, .format = {ENC_UTF8, 0, EOL_LF}
    };
    s.nspaces = 2;
    s.spaces = calloc(s.nspaces, sizeof(*s.spaces));
    CHECK(s.spaces);
    s.spaces[0].ntabs = 3;
    s.spaces[0].current = 1;
    s.spaces[0].tabs = calloc(3, sizeof(*s.spaces[0].tabs));
    s.spaces[1].ntabs = 1;
    s.spaces[1].tabs = calloc(1, sizeof(*s.spaces[1].tabs));
    CHECK(s.spaces[0].tabs && s.spaces[1].tabs);
    s.spaces[0].tabs[0] = (struct edit_session_view){0, 15, 6, 2, 4, 1, 1, 1, 24};
    s.spaces[0].tabs[1] = (struct edit_session_view){1, 3, 0, 0, 0, 0, 0, 1, 0};
    s.spaces[0].tabs[2] = (struct edit_session_view){2, 5, 5, 1, 2, 0, 1, 0, 16};
    s.spaces[1].tabs[0] = (struct edit_session_view){0, 2, 1, 0, 1, 0, 0, 0, 32};
    s.active = 1;
    s.sidebar = 1;
    s.sidebar_width = 31;
    s.layout = strdup("V(3750,0,1)");
    return s;
}

static char *checkpoint_path(const char *name)
{
    char *p = NULL;
    CHECK(asprintf(&p, "%s/%s.json", directory, name) > 0);
    return p;
}
static char *read_raw(const char *name, size_t *len)
{
    char *p = checkpoint_path(name);
    file_as_user(1);
    int fd = open(p, O_RDONLY | O_NOFOLLOW);
    free(p);
    CHECK(fd >= 0);
    struct stat st;
    CHECK(!fstat(fd, &st));
    char *data = malloc((size_t)st.st_size + 1);
    CHECK(data);
    size_t at = 0;
    while (at < (size_t)st.st_size) {
        ssize_t n = read(fd, data + at, (size_t)st.st_size - at);
        CHECK(n > 0);
        at += (size_t)n;
    }
    CHECK(!close(fd));
    file_as_user(0);
    data[at] = 0;
    *len = at;
    return data;
}
static void write_raw(const char *name, const char *data, size_t len)
{
    char *p = checkpoint_path(name);
    file_as_user(1);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    free(p);
    CHECK(fd >= 0);
    CHECK(write(fd, data, len) == (ssize_t)len);
    CHECK(!close(fd));
    file_as_user(0);
}
static void same_checkpoint(const char *name, const char *bytes, size_t length)
{
    size_t got;
    char *data = read_raw(name, &got);
    CHECK(got == length && !memcmp(data, bytes, got));
    free(data);
}
static void compare(const struct edit_session_state *want, const struct edit_session_state *got, int recovered)
{
    CHECK(got->ndocuments == want->ndocuments && got->nspaces == want->nspaces);
    CHECK(got->recovered == recovered && got->active == want->active && got->sidebar == want->sidebar);
    CHECK(got->sidebar_width == want->sidebar_width && !strcmp(got->layout, want->layout));
    for (size_t i = 0; i < got->ndocuments; i++) {
        const struct edit_session_document *a = want->documents + i, *b = got->documents + i;
        CHECK((!a->path && !b->path) || (a->path && b->path && !strcmp(a->path, b->path)));
        CHECK(a->length == b->length && a->modified == b->modified && a->readonly == b->readonly);
        CHECK(!strcmp(a->base, b->base));
        CHECK(a->format.encoding == b->format.encoding && a->format.eol == b->format.eol && a->format.bom == b->format.bom);
        if (a->modified || !a->path) CHECK(b->text && !memcmp(a->text, b->text, a->length) && !b->text[b->length]);
        else CHECK(!b->text);
    }
    for (size_t i = 0; i < got->nspaces; i++) {
        CHECK(got->spaces[i].ntabs == want->spaces[i].ntabs && got->spaces[i].current == want->spaces[i].current);
        for (size_t j = 0; j < got->spaces[i].ntabs; j++) {
            const struct edit_session_view *a = want->spaces[i].tabs + j, *b = got->spaces[i].tabs + j;
            CHECK(a->document == b->document && a->cursor == b->cursor && a->anchor == b->anchor && a->top == b->top);
            CHECK(a->left == b->left && a->sub == b->sub && a->wrap == b->wrap && a->numbers == b->numbers && a->font_px == b->font_px);
        }
    }
}

static void roundtrips(struct edit_session_state *s)
{
    char **names = NULL;
    size_t count = 9;
    CHECK(edit_session_list(&names, &count) == 0 && !count && !names);
    struct edit_session_state loaded = {0};
    CHECK(edit_session_load(NULL, &loaded) == ENOENT);
    CHECK(edit_session_save(NULL, s, 0) == 0);
    CHECK(edit_session_load(NULL, &loaded) == 0);
    compare(s, &loaded, 1);
    CHECK(edit_session_save(NULL, s, 1) == 0);
    CHECK(edit_session_load(NULL, &loaded) == 0);
    compare(s, &loaded, 0);
    CHECK(edit_session_create("work", s, 0) == 0);
    CHECK(edit_session_create("work", s, 1) == EEXIST);
    CHECK(edit_session_save("\xe4\xb8\xad\xe6\x96\x87 session", s, 1) == 0);
    CHECK(edit_session_list(&names, &count) == 0 && count == 3);
    CHECK(!strcmp(names[0], "default") && !strcmp(names[1], "work") && !strcmp(names[2], "\xe4\xb8\xad\xe6\x96\x87 session"));
    edit_session_list_free(names, count);
    char *original = s->documents[2].path;
    s->documents[2].path = strdup("/home/tester/literal\\u0000.txt");
    CHECK(s->documents[2].path && edit_session_save("literal", s, 1) == 0);
    CHECK(edit_session_load("literal", &loaded) == 0);
    compare(s, &loaded, 0);
    CHECK(edit_session_remove("literal") == 0);
    free(s->documents[2].path);
    s->documents[2].path = original;
    CHECK(edit_session_load("work", &loaded) == 0);
    compare(s, &loaded, 1);
    CHECK(edit_session_remove("work") == 0 && edit_session_load("work", &loaded) == ENOENT);
    CHECK(edit_session_remove("\xe4\xb8\xad\xe6\x96\x87 session") == 0);
    edit_session_free(&loaded);
    size_t n;
    char *raw = read_raw("default", &n);
    CHECK(!strstr(raw, "do not store unmodified bytes"));
    free(raw);
    char *p = checkpoint_path("default");
    struct stat st;
    CHECK(!stat(p, &st) && st.st_uid == EDIT_USER && (st.st_mode & 0777) == 0600);
    free(p);
    CHECK(!stat(directory, &st) && st.st_uid == EDIT_USER && (st.st_mode & 0777) == 0700);
    struct edit_session_space welcome = { .current = -1 };
    struct edit_session_state empty = { .spaces = &welcome, .nspaces = 1, .layout = "0" };
    CHECK(edit_session_save("empty", &empty, 0) == 0);
    CHECK(edit_session_load("empty", &loaded) == 0 && loaded.ndocuments == 0 && loaded.spaces[0].current == -1);
    edit_session_free(&loaded);
    CHECK(edit_session_remove("empty") == 0);
    char *selected = NULL;
    CHECK(edit_session_selected(&selected) == ENOENT && !selected);
    CHECK(edit_session_select("default") == 0);
    CHECK(edit_session_selected(&selected) == 0 && !strcmp(selected, "default"));
    free(selected);
    CHECK(edit_session_select("../unsafe") == EINVAL);
    CHECK(edit_session_select("work") == 0);
    CHECK(edit_session_selected(&selected) == 0 && !strcmp(selected, "work"));
    free(selected);
    puts("PASS clean/unclean, named/empty sessions, complete byte-counted recovery, views/font/layout/encoding/EOL, private uid1000 state");
}

static JsonObject *first_document(JsonObject *o)
{
    return json_array_get_object_element(json_object_get_array_member(o, "documents"), 0);
}
static void invalid_case(const char *valid, size_t length, struct edit_session_state *s, int which)
{
    JsonParser *p = json_parser_new();
    CHECK(json_parser_load_from_data(p, valid, (gssize)length, NULL));
    JsonNode *root = json_parser_get_root(p);
    JsonObject *o = json_node_get_object(root);
    JsonObject *d = first_document(o);
    JsonObject *sp = json_array_get_object_element(json_object_get_array_member(o, "spaces"), 0);
    JsonObject *v = json_array_get_object_element(json_object_get_array_member(sp, "tabs"), 0);
    switch (which) {
    case 0: json_object_set_int_member(o, "version", 99); break;
    case 1: json_object_set_string_member(o, "clean", "false"); break;
    case 2: json_object_set_int_member(o, "active", 2); break;
    case 3: json_object_set_int_member(d, "encoding", ENC_COUNT); break;
    case 4: json_object_set_int_member(d, "eol", 99); break;
    case 5: json_object_set_int_member(d, "length", EDIT_SESSION_MAX_BUFFER + 1); break;
    case 6: json_object_set_string_member(d, "data", "!!!!"); break;
    case 7: json_object_set_string_member(d, "path", "relative"); break;
    case 8: json_object_set_string_member(o, "layout", "V(5000,0,0)"); break;
    case 9: json_object_set_string_member(o, "layout", "V(5000,0,2)"); break;
    case 10: json_object_set_string_member(o, "layout", "V(5000,0,1"); break;
    case 11: json_object_set_string_member(o, "layout", "V(0500,0,1)"); break;
    case 12: json_object_set_int_member(v, "cursor", INT64_MAX); break;
    case 13: json_object_set_int_member(v, "document", 55); break;
    case 14: json_object_set_int_member(o, "unexpected", 1); break;
    case 15: json_object_set_double_member(v, "anchor", 1.5); break;
    case 16: json_object_set_int_member(d, "length", -1); break;
    case 17: json_object_set_int_member(sp, "current", 9999); break;
    case 18: json_object_set_null_member(o, "spaces"); break;
    case 19: json_object_set_string_member(o, "layout", "0"); break;
    case 20: {
        JsonArray *a = json_array_new();
        for (unsigned i = 0; i <= EDIT_SESSION_MAX_DOCUMENTS; i++) json_array_add_null_element(a);
        json_object_set_array_member(o, "documents", a);
        break;
    }
    case 21: {
        JsonArray *a = json_array_new();
        for (unsigned i = 0; i <= EDIT_SESSION_MAX_SPACES; i++) json_array_add_null_element(a);
        json_object_set_array_member(o, "spaces", a);
        break;
    }
    case 22: {
        JsonArray *a = json_array_new();
        for (unsigned i = 0; i <= EDIT_SESSION_MAX_TABS; i++) json_array_add_null_element(a);
        json_object_set_array_member(sp, "tabs", a);
        break;
    }
    case 23: json_object_set_int_member(v, "anchor", (gint64)s->documents[0].length + 1); break;
    case 24: {
        JsonObject *second = json_array_get_object_element(json_object_get_array_member(sp, "tabs"), 1);
        json_object_set_int_member(second, "document", 0);
        break;
    }
    case 25: json_object_set_int_member(v, "font_px", 11); break;
    case 26: json_object_set_int_member(v, "font_px", 97); break;
    case 27: json_object_set_string_member(d, "base", "0123456789ABCDEF0123456789abcdef01234567"); break;
    case 28: json_object_set_string_member(d, "base", "abc"); break;
    case 29: {
        JsonObject *untitled = json_array_get_object_element(json_object_get_array_member(o, "documents"), 1);
        json_object_set_string_member(untitled, "base", "0123456789abcdef0123456789abcdef01234567");
        break;
    }
    default: abort();
    }
    JsonGenerator *g = json_generator_new();
    json_generator_set_root(g, root);
    gsize n;
    char *data = json_generator_to_data(g, &n);
    write_raw("bad", data, n);
    struct edit_session_state loaded = {0};
    CHECK(edit_session_load("default", &loaded) == 0);
    char *old_layout = loaded.layout;
    CHECK(edit_session_load("bad", &loaded) != 0 && loaded.layout == old_layout);
    CHECK(edit_session_save("bad", s, 0) != 0);
    same_checkpoint("bad", data, n);
    edit_session_free(&loaded);
    CHECK(edit_session_remove("bad") == 0);
    g_free(data);
    g_object_unref(g);
    g_object_unref(p);
}
static void malformed(struct edit_session_state *s)
{
    size_t n;
    char *data = read_raw("default", &n);
    for (int i = 0; i < 30; i++) invalid_case(data, n, s, i);
    const char *invalid[] = {
        "{", "[]", "{\"version\":1,\"version\":2}",
        "{\"version\":1,\"clean\":false,}", "{\"version\\u0000x\":1}",
        "{\"layout\":\"L0\\u0000junk\"}", "{\"x\":NaN}",
        "{\"x\":/* comment */true}", "{} trailing", "\xff"
    };
    struct edit_session_state loaded = {0};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        write_raw("bad", invalid[i], strlen(invalid[i]));
        CHECK(edit_session_load("bad", &loaded) != 0);
        CHECK(edit_session_save("bad", s, 1) != 0);
        same_checkpoint("bad", invalid[i], strlen(invalid[i]));
        CHECK(edit_session_remove("bad") == 0);
    }
    char *duplicate = NULL;
    CHECK(asprintf(&duplicate, "{\"version\":1,%s", data + 1) > 0);
    write_raw("bad", duplicate, strlen(duplicate));
    CHECK(edit_session_load("bad", &loaded) == EINVAL);
    CHECK(edit_session_remove("bad") == 0);
    free(duplicate);
    const char *badnames[] = {"", "..", ".hidden", "a/b", "a\\b", "line\nbreak", "\xff"};
    for (size_t i = 0; i < sizeof(badnames) / sizeof(badnames[0]); i++) {
        CHECK(edit_session_save(badnames[i], s, 0) == EINVAL);
        CHECK(edit_session_load(badnames[i], &loaded) == EINVAL);
        CHECK(edit_session_remove(badnames[i]) == EINVAL);
    }
    char over[EDIT_SESSION_MAX_NAME + 2];
    memset(over, 'x', sizeof(over) - 1);
    over[sizeof(over) - 1] = 0;
    CHECK(edit_session_save(over, s, 0) == EINVAL);
    char *path = checkpoint_path("bad");
    file_as_user(1);
    int fd = open(path, O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0 && !ftruncate(fd, (off_t)EDIT_SESSION_MAX_FILE + 1) && !close(fd));
    file_as_user(0);
    CHECK(edit_session_load("bad", &loaded) == EFBIG && edit_session_save("bad", s, 0) == EFBIG);
    CHECK(edit_session_remove("bad") == 0);
    free(path);
    size_t oldlength = s->documents[0].length;
    s->documents[0].length = EDIT_SESSION_MAX_BUFFER + 1;
    CHECK(edit_session_save(NULL, s, 0) == EFBIG);
    s->documents[0].length = oldlength;
    same_checkpoint("default", data, n);
    free(data);
    puts("PASS malformed/tampered JSON, duplicate keys, schema/types/base64/layout/position bounds, preserved invalid recovery");
}

static void failures(struct edit_session_state *s)
{
    size_t n;
    char *old = read_raw("default", &n);
    interrupt_write = 1;
    short_write = 1;
    write_calls = 0;
    CHECK(edit_session_save("short-write", s, 0) == 0);
    interrupt_write = short_write = 0;
    struct edit_session_state retry = {0};
    CHECK(edit_session_load("short-write", &retry) == 0);
    compare(s, &retry, 1);
    edit_session_free(&retry);
    CHECK(edit_session_remove("short-write") == 0);
    int oldwidth = s->sidebar_width;
    s->sidebar_width = 47;
    for (int failure = 0; failure < 5; failure++) {
        inject_write = failure == 0 ? 1 : failure == 1 ? 2 : 0;
        inject_rename = failure == 2 ? 1 : 0;
        inject_fsync = failure == 3 ? 1 : failure == 4 ? 2 : 0;
        write_calls = rename_calls = directory_syncs = 0;
        CHECK(edit_session_save(NULL, s, 0) != 0);
        inject_write = inject_rename = inject_fsync = 0;
        same_checkpoint("default", old, n);
        struct edit_session_state loaded = {0};
        CHECK(edit_session_load(NULL, &loaded) == 0 && loaded.sidebar_width == oldwidth);
        edit_session_free(&loaded);
    }
    inject_fsync = 1;
    directory_syncs = 0;
    CHECK(edit_session_save("new-failure", s, 0) != 0);
    inject_fsync = 0;
    struct edit_session_state loaded = {0};
    CHECK(edit_session_load("new-failure", &loaded) == ENOENT);
    char *path = checkpoint_path("default");
    CHECK(!chmod(path, 0400));
    CHECK(edit_session_save(NULL, s, 0) == EACCES);
    CHECK(!chmod(path, 0600));
    same_checkpoint("default", old, n);
    CHECK(!chmod(directory, 0500));
    CHECK(edit_session_save(NULL, s, 0) == EACCES);
    CHECK(!chmod(directory, 0700));
    same_checkpoint("default", old, n);
    char *bad = checkpoint_path("bad");
    CHECK(!symlink(path, bad));
    CHECK(!lchown(bad, EDIT_USER, EDIT_USER));
    CHECK(edit_session_save("bad", s, 0) != 0 && edit_session_load("bad", &loaded) != 0);
    CHECK(edit_session_remove("bad") != 0);
    same_checkpoint("default", old, n);
    CHECK(!unlink(bad));
    CHECK(!mkfifo(bad, 0600) && !chown(bad, EDIT_USER, EDIT_USER));
    CHECK(edit_session_save("bad", s, 0) == EACCES && edit_session_load("bad", &loaded) == EACCES);
    CHECK(!unlink(bad));
    CHECK(!link(path, bad));
    CHECK(edit_session_save("bad", s, 0) == EACCES && edit_session_load("bad", &loaded) == EACCES);
    CHECK(!unlink(bad));
    free(bad);
    CHECK(!chmod(path, 0644));
    CHECK(edit_session_load(NULL, &loaded) == EACCES);
    CHECK(!chmod(path, 0600));
    char *saved = NULL;
    CHECK(asprintf(&saved, "%s-held", directory) > 0);
    CHECK(!rename(directory, saved));
    CHECK(!symlink(saved, directory));
    CHECK(edit_session_save(NULL, s, 0) != 0 && edit_session_load(NULL, &loaded) != 0);
    CHECK(!unlink(directory) && !rename(saved, directory));
    free(saved);
    same_checkpoint("default", old, n);
    CHECK(!chown(path, 0, 0));
    CHECK(edit_session_load(NULL, &loaded) == EACCES && edit_session_save(NULL, s, 0) == EACCES);
    CHECK(!chown(path, EDIT_USER, EDIT_USER));
    free(path);
    same_checkpoint("default", old, n);
    file_as_user(1);
    int locked = open(directory, O_RDONLY | O_DIRECTORY);
    CHECK(locked >= 0 && flock(locked, LOCK_EX | LOCK_NB) == 0);
    file_as_user(0);
    CHECK(edit_session_save(NULL, s, 0) == EWOULDBLOCK);
    CHECK(edit_session_load(NULL, &loaded) == EWOULDBLOCK);
    CHECK(edit_session_remove(NULL) == EWOULDBLOCK);
    CHECK(close(locked) == 0);
    same_checkpoint("default", old, n);
    s->sidebar_width = oldwidth;
    free(old);
    puts("PASS uid1000 permissions, unsafe path refusal, EINTR/short writes, directory locking and atomic failure rollback");
}

static void crash(struct edit_session_state *s)
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) { if (edit_session_save("crashed", s, 0)) _exit(2); _exit(0); }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    struct edit_session_state loaded = {0};
    CHECK(edit_session_load("crashed", &loaded) == 0);
    compare(s, &loaded, 1);
    CHECK(edit_session_save("crashed", &loaded, 1) == 0);
    CHECK(edit_session_load("crashed", &loaded) == 0);
    compare(s, &loaded, 0);
    edit_session_free(&loaded);
    size_t n;
    char *before = read_raw("crashed", &n);
    inject_fsync = 1;
    directory_syncs = 0;
    CHECK(edit_session_remove("crashed") == EIO);
    inject_fsync = 0;
    same_checkpoint("crashed", before, n);
    CHECK(edit_session_remove("crashed") == 0);
    CHECK(edit_session_save("crashed", s, 0) == 0);
    free(before);
    before = read_raw("crashed", &n);
    for (int point = 1; point <= 3; point++) {
        pid = fork();
        CHECK(pid >= 0);
        if (!pid) {
            s->sidebar_width++;
            write_calls = rename_calls = 0;
            crash_write = point <= 2 ? point : 0;
            crash_rename = point == 3 ? 1 : 0;
            (void)edit_session_save("crashed", s, 0);
            _exit(84);
        }
        CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == (point == 3 ? 82 : 81));
        CHECK(edit_session_load("crashed", &loaded) == 0 && loaded.recovered);
        CHECK(loaded.sidebar_width == s->sidebar_width + (point == 3));
        if (point != 3) same_checkpoint("crashed", before, n);
        edit_session_free(&loaded);
    }
    free(before);
    CHECK(edit_session_remove("crashed") == 0);
    puts("PASS process exits mid-write/backup/post-rename, clean-exit marker and failed-remove rollback");
}

static void user_home(struct edit_session_state *s)
{
    char home[] = "/tmp/edit-session-home-XXXXXX";
    CHECK(mkdtemp(home) && !chown(home, EDIT_USER, EDIT_USER));
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        CHECK(setenv("HOME", home, 1) == 0);
        CHECK(setgid(EDIT_USER) == 0 && setuid(EDIT_USER) == 0);
        CHECK(edit_session_save("user-home", s, 0) == 0);
        struct edit_session_state loaded = {0};
        CHECK(edit_session_load("user-home", &loaded) == 0);
        compare(s, &loaded, 1);
        edit_session_free(&loaded);
        CHECK(edit_session_remove("user-home") == 0);
        _exit(0);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char path[4096];
    snprintf(path, sizeof(path), "%s/.local/state/locachy/edit/sessions", home);
    struct stat st;
    CHECK(stat(path, &st) == 0 && st.st_uid == EDIT_USER && (st.st_mode & 0777) == 0700);
    puts("PASS actual uid1000 HOME environment honored; root HOME never used");
}

int main(void)
{
    CHECK(getuid() == 0);
    struct passwd *pw = __real_getpwuid(EDIT_USER);
    CHECK(pw);
    /* getpwuid is wrapped only in this test binary: root callers still test
     * the passwd lookup path, but never touch the account's real sessions. */
    strcpy(account_home, "/tmp/edit-session-account-XXXXXX");
    CHECK(mkdtemp(account_home) && !chown(account_home, EDIT_USER, EDIT_USER));
    CHECK(strlen(account_home) < 4000);
    snprintf(directory, sizeof(directory), "%s/.local/state/locachy/edit/sessions", account_home);
    CHECK(setenv("HOME", "/root/never-use-edit-session-root-home", 1) == 0);
    struct edit_session_state s = sample();
    roundtrips(&s);
    malformed(&s);
    failures(&s);
    crash(&s);
    user_home(&s);
    CHECK(edit_session_remove(NULL) == 0);
    edit_session_free(&s);
    puts("PASS all edit session tests");
    return 0;
}
