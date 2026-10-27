/*
 * Opening files and addresses: which container opens what (base.h).
 *
 * A target is an absolute path or a URL. Its type is a MIME type,
 * inode/directory for a folder, or x-scheme-handler/SCHEME for a URL.
 * The containers that can open a type are those whose installed .desktop
 * files list it, or its major type with a * (image/ then *), in MimeType.
 * OPEN_RULES holds the user's choices, TYPE=CONTAINER or TYPE=ask, a line
 * per type (a major type with * too); settings and the choosers write it.
 *
 * Only files in the shared user directories (rc.conf's user_dirs, in the
 * home of the user, uid 1000) are in every container, at the same path.
 *
 * Types are found and related as GIO does it (glib's content types, over
 * shared-mime-info's database): a file's type from its name, else its first
 * bytes; a type opened by what opens a type it is a kind of.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>

#include "base.h"

#define CONTAINERS "/var/lib/locachy/containers"

static int valid_name(const char *name)
{
    if (!isalnum((unsigned char)name[0]) || strlen(name) >= OPEN_NAME) {
        return 0;
    }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.') {
            return 0;
        }
    }
    return 1;
}

static void user_dirs_setting(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "user_dirs") == 0) {
        snprintf(arg, 256, "%s", value);
    }
}

void open_user_dirs(char *home, size_t hsize, char *dirs, size_t dsize)
{
    struct passwd *pw = getpwuid(1000);
    char list[256] = "Documents Downloads Music Pictures Videos";
    snprintf(home, hsize, "%s", pw && pw->pw_dir ? pw->pw_dir : "");
    rc_read(user_dirs_setting, list);
    snprintf(dirs, dsize, "%s Drives", list); /* USB drives (drives.c), shared too */
}

int open_shared(const char *path, char *why, size_t size)
{
    char home[256], dirs[256], list[256], real[4096];
    open_user_dirs(home, sizeof home, dirs, sizeof dirs);
    snprintf(list, sizeof list, "%s", dirs);
    if (!home[0] || realpath(path, real) == NULL) {
        snprintf(why, size, "%s does not exist", path);
        return 0;
    }
    size_t hlen = strlen(home);
    if (strncmp(real, home, hlen) == 0 && real[hlen] == '/') {
        const char *rest = real + hlen + 1;
        for (char *d = strtok(dirs, " "); d; d = strtok(NULL, " ")) {
            size_t dlen = strlen(d);
            if (strncmp(rest, d, dlen) == 0 && (rest[dlen] == '/' || rest[dlen] == '\0')) {
                return 1;
            }
        }
    }
    snprintf(why, size, "%s is not in a shared folder of %s (%s)", path,
             home[0] ? home : "the user", list);
    return 0;
}

/* file:///a%20b -> /a b */
static int file_url_path(const char *url, char *path, size_t size)
{
    const char *s = url + 7;
    size_t n = 0;
    if (strncmp(s, "localhost/", 10) == 0) {
        s += 9;
    }
    if (*s != '/') {
        return -1;
    }
    for (; *s && n + 1 < size; s++) {
        if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            path[n++] = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            path[n++] = *s;
        }
    }
    path[n] = '\0';
    return 0;
}

int open_normalize(const char *target, char *out, size_t size)
{
    if (strncmp(target, "file://", 7) == 0) {
        return file_url_path(target, out, size);
    }
    if (target[0] == '/' || isalpha((unsigned char)target[0])) {
        snprintf(out, size, "%s", target);
        return 0;
    }
    return -1;
}

static int is_url(const char *target)
{
    const char *s = target;
    if (!isalpha((unsigned char)*s)) {
        return 0;
    }
    while (isalnum((unsigned char)*s) || *s == '+' || *s == '-' || *s == '.') {
        s++;
    }
    return *s == ':';
}

/* The type GIO gives a local file (g_file_query_info's content type): its
 * name's, by the globs, unless that is none or more than one; then its first
 * bytes', by the magic (shared-mime-info's "Recommended checking order"). */
static int file_type(const char *path, const struct stat *st, char *type, size_t size)
{
    const char *kind = S_ISDIR(st->st_mode) ? "inode/directory" : S_ISFIFO(st->st_mode) ? "inode/fifo" :
                       S_ISCHR(st->st_mode) ? "inode/chardevice" : S_ISBLK(st->st_mode) ? "inode/blockdevice" :
                       S_ISSOCK(st->st_mode) ? "inode/socket" : NULL;
    if (kind) {
        snprintf(type, size, "%s", kind);
        return 0;
    }
    guchar data[4096]; /* GIO's sniffing length for local files */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    ssize_t n = fd >= 0 ? read(fd, data, sizeof data) : -1;
    if (fd >= 0) {
        close(fd);
    }
    gchar *guess = g_content_type_guess(path, data, n > 0 ? (gsize)n : 0, NULL);
    int ok = guess && strchr(guess, '/') && snprintf(type, size, "%s", guess) < (int)size;
    g_free(guess);
    return ok ? 0 : -1;
}

int open_type(const char *target, char *type, size_t size)
{
    struct stat st;
    if (is_url(target)) {
        snprintf(type, size, "x-scheme-handler/%.*s", (int)strcspn(target, ":"), target);
        for (char *p = type; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        return 0;
    }
    if (target[0] != '/' || stat(target, &st) != 0) {
        return -1;
    }
    return file_type(target, &st, type, size);
}

/* Does the MimeType line (a;b;c;) list type, a type it is a kind of (GIO's
 * g_content_type_is_a: aliases, subclasses, a major type with a star), or a
 * URL's scheme? */
static int lists(const char *mimetypes, const char *type)
{
    int scheme = strncmp(type, "x-scheme-handler/", 17) == 0, found = 0;
    gchar **entries = g_strsplit(mimetypes, ";", -1);
    for (gchar **e = entries; *e && !found; e++) {
        found = **e && (scheme ? strcmp(*e, type) == 0 : g_content_type_is_a(type, *e));
    }
    g_strfreev(entries);
    return found;
}

struct program_conf {
    int host;
    char desktop[256];
};

static void on_program(const char *key, const char *value, void *arg)
{
    struct program_conf *c = arg;
    if (strcmp(key, "host") == 0) {
        c->host = strcmp(value, "1") == 0;
    } else if (strcmp(key, "desktop") == 0) {
        snprintf(c->desktop, sizeof c->desktop, "%s", value);
    }
}

static void program_conf(const char *name, struct program_conf *c)
{
    char path[256];
    memset(c, 0, sizeof *c);
    snprintf(path, sizeof path, CONTAINERS_CONF "/%s.conf", name);
    conf_read(path, on_program, c);
}

int open_host(const char *name)
{
    struct program_conf c;
    if (!valid_name(name)) {
        return 0;
    }
    program_conf(name, &c);
    return c.host;
}

/* A framebuffer program's name without "program:", or NULL if name is not
 * one's (program_desktop and program_files are not home's programs). */
static const char *program_name(const char *name)
{
    size_t len = strlen(OPEN_PROGRAM);
    const char *p = name + len;
    return strncmp(name, OPEN_PROGRAM, len) == 0 && valid_name(p) &&
           strcmp(p, "desktop") != 0 && strcmp(p, "files") != 0 ? p : NULL;
}

struct command {
    const char *name; /* program_NAME's */
    char *spec;
    size_t size;
    int found;
};

static void on_command(const char *key, const char *value, void *arg)
{
    struct command *c = arg;
    if (strncmp(key, "program_", 8) == 0 && strcmp(key + 8, c->name) == 0 && value[0]) {
        snprintf(c->spec, c->size, "%s", value);
        c->found = 1;
    }
}

int open_program(const char *name, const char *target, char *spec, size_t size, char *argv[], int max)
{
    struct command c = { program_name(name), spec, size, 0 };
    int argc = 0;
    if (c.name == NULL || max < 3) {
        return 0;
    }
    rc_read(on_command, &c);
    for (char *w = c.found ? strtok(spec, " \t") : NULL; w && argc < max - 2; w = strtok(NULL, " \t")) {
        argv[argc++] = w;
    }
    if (argc == 0) {
        return 0;
    }
    argv[argc++] = (char *)target;
    argv[argc] = NULL;
    return argc;
}

/* The .desktop files of name: a host program's, those its desktop= names in
 * /usr/share/applications (not all of them: they are every program's); a
 * container's, those in its upper (what its own packages added); a
 * framebuffer program's, its own in OPEN_PROGRAM_DESKTOP. */
static int desktop_files(const char *name, char paths[][512], int max)
{
    struct program_conf c;
    int n = 0;
    if (program_name(name)) {
        snprintf(paths[0], 512, OPEN_PROGRAM_DESKTOP "/%s.desktop", program_name(name));
        return max > 0;
    }
    program_conf(name, &c);
    if (c.host) {
        char *save = NULL;
        for (char *f = strtok_r(c.desktop, " ", &save); f && n < max; f = strtok_r(NULL, " ", &save)) {
            size_t len = strlen(f);
            if (len > 8 && !strchr(f, '/') && strcmp(f + len - 8, ".desktop") == 0) {
                snprintf(paths[n++], 512, "/usr/share/applications/%s", f);
            }
        }
        return n;
    }
    char dir_path[256];
    snprintf(dir_path, sizeof dir_path, CONTAINERS "/%s/upper/usr/share/applications", name);
    DIR *dir = opendir(dir_path);
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL && n < max) {
        size_t len = strlen(e->d_name);
        if (len > 8 && strcmp(e->d_name + len - 8, ".desktop") == 0) {
            snprintf(paths[n++], 512, "%s/%s", dir_path, e->d_name);
        }
    }
    if (dir) {
        closedir(dir);
    }
    return n;
}

/* The Name of the first .desktop file of name that lists type (any .desktop
 * file if type is NULL); 1 if there is one. */
static int desktop_entry(const char *name, const char *type, char *label, size_t size)
{
    char paths[32][512];
    int n = desktop_files(name, paths, 32), found = 0;
    for (int i = 0; i < n && !found; i++) {
        char line[2048], entry_name[128] = "";
        int in_entry = 0, matches = type == NULL;
        FILE *in = fopen(paths[i], "re");
        while (in && fgets(line, sizeof line, in)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '[') {
                in_entry = strcmp(line, "[Desktop Entry]") == 0;
            } else if (in_entry && strncmp(line, "Name=", 5) == 0 && !entry_name[0]) {
                snprintf(entry_name, sizeof entry_name, "%.127s", line + 5);
            } else if (in_entry && type && strncmp(line, "MimeType=", 9) == 0) {
                matches = lists(line + 9, type);
            }
        }
        if (in) {
            fclose(in);
            if (matches) {
                snprintf(label, size, "%s", entry_name[0] ? entry_name : name);
                found = 1;
            }
        }
    }
    return found;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(a, b);
}

/* The containers there are (CONTAINERS_CONF/NAME.conf), those that open
 * type if it is not NULL. */
static int each_container(const char *type, char names[][OPEN_NAME], int max)
{
    DIR *dir = opendir(CONTAINERS_CONF);
    struct dirent *e;
    int n = 0;
    while (dir && (e = readdir(dir)) != NULL && n < max) {
        char name[OPEN_NAME], label[128];
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= OPEN_NAME || strcmp(e->d_name + len - 5, ".conf") != 0) {
            continue;
        }
        snprintf(name, sizeof name, "%.*s", (int)(len - 5), e->d_name);
        if (!valid_name(name)) {
            continue;
        }
        if (type == NULL || desktop_entry(name, type, label, sizeof label)) {
            memcpy(names[n++], name, OPEN_NAME);
        }
    }
    if (dir) {
        closedir(dir);
    }
    /* A stable order: by name. */
    qsort(names, (size_t)n, OPEN_NAME, by_name);
    return n;
}

/* The framebuffer programs (rc.conf program_NAME) whose .desktop file lists
 * type, as "program:NAME". */
struct programs {
    const char *type;
    char (*names)[OPEN_NAME];
    int n, max;
};

static void on_program_setting(const char *key, const char *value, void *arg)
{
    struct programs *p = arg;
    char name[OPEN_NAME], label[128];
    (void)value;
    if (strncmp(key, "program_", 8) != 0 || p->n == p->max ||
        snprintf(name, sizeof name, OPEN_PROGRAM "%s", key + 8) >= (int)sizeof name ||
        !program_name(name) || !desktop_entry(name, p->type, label, sizeof label)) {
        return;
    }
    for (int i = 0; i < p->n; i++) {
        if (strcmp(p->names[i], name) == 0) {
            return; /* a setting given twice */
        }
    }
    memcpy(p->names[p->n++], name, OPEN_NAME);
}

int open_candidates(const char *type, char names[][OPEN_NAME], int max)
{
    struct programs p = { type, names, 0, max };
    rc_read(on_program_setting, &p);
    return p.n < max ? p.n + each_container(type, names + p.n, max - p.n) : p.n;
}

int open_containers(char names[][OPEN_NAME], int max)
{
    return each_container(NULL, names, max);
}

int open_window_ids(const char *name, char ids[][OPEN_WINDOW_ID], int max)
{
    char paths[32][512];
    if (!valid_name(name)) {
        return 0;
    }
    int files = desktop_files(name, paths, 32), n = 0;
    for (int i = 0; i < files && n < max; i++) {
        FILE *in = fopen(paths[i], "re");
        if (!in) {
            continue;
        }
        const char *slash = strrchr(paths[i], '/'), *base = slash ? slash + 1 : paths[i];
        snprintf(ids[n++], OPEN_WINDOW_ID, "%.*s", (int)(strlen(base) - 8), base);
        char line[256];
        while (fgets(line, sizeof line, in) && n < max) {
            if (strncmp(line, "StartupWMClass=", 15) == 0) {
                line[strcspn(line, "\r\n")] = '\0';
                snprintf(ids[n++], OPEN_WINDOW_ID, "%.127s", line + 15);
                break;
            }
        }
        fclose(in);
    }
    return n;
}

void open_label(const char *name, const char *type, char *label, size_t size)
{
    char entry[128];
    const char *program = program_name(name);
    if (desktop_entry(name, type, entry, sizeof entry) || desktop_entry(name, NULL, entry, sizeof entry)) {
        snprintf(label, size, "%s (%s)", program ? program : name, entry);
    } else {
        snprintf(label, size, "%s", program ? program : name);
    }
}

int open_rule(const char *type, char *name, size_t size)
{
    FILE *in = fopen(OPEN_RULES, "re");
    char line[256], major[128];
    int found = OPEN_NO_RULE;
    snprintf(major, sizeof major, "%.*s/*", (int)strcspn(type, "/"), type);
    while (in && fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) {
            continue;
        }
        *eq++ = '\0';
        /* A rule written for an alias (text/x-c) is the type's. */
        int exact = g_content_type_equals(line, type);
        if (exact || (found == OPEN_NO_RULE && strcmp(line, major) == 0)) {
            found = strcmp(eq, "ask") == 0 ? OPEN_RULE_ASK :
                    valid_name(eq) || program_name(eq) ? OPEN_RULE_CONTAINER : OPEN_NO_RULE;
            if (found == OPEN_RULE_CONTAINER) {
                snprintf(name, size, "%s", eq);
            }
            if (exact) {
                break;
            }
        }
    }
    if (in) {
        fclose(in);
    }
    return found;
}

int open_set_rule(const char *type, const char *value)
{
    FILE *in = fopen(OPEN_RULES, "re");
    FILE *out = fopen(OPEN_RULES ".tmp", "we");
    char line[256];
    size_t tlen = strlen(type);
    if (!out) {
        if (in) {
            fclose(in);
        }
        return -1;
    }
    while (in && fgets(line, sizeof line, in)) {
        if (strncmp(line, type, tlen) != 0 || line[tlen] != '=') {
            fputs(line, out);
        }
    }
    if (in) {
        fclose(in);
    }
    if (value) {
        fprintf(out, "%s=%s\n", type, value);
    }
    if (fclose(out) != 0) {
        return -1;
    }
    return rename(OPEN_RULES ".tmp", OPEN_RULES);
}

int open_rules(char types[][OPEN_TYPE], char values[][OPEN_NAME], int max)
{
    FILE *in = fopen(OPEN_RULES, "re");
    char line[256];
    int n = 0;
    while (in && n < max && fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line) {
            continue;
        }
        *eq++ = '\0';
        snprintf(types[n], OPEN_TYPE, "%.127s", line);
        snprintf(values[n], OPEN_NAME, "%.63s", eq);
        n++;
    }
    if (in) {
        fclose(in);
    }
    return n;
}
