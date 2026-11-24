/*
 * Loading kernel modules, for process 1: the drivers for the devices there
 * are, as udev and modprobe would. The kernel is a distribution's, with its
 * drivers as modules under ROOT/lib/modules/VERSION:
 *   modules.alias   "alias PATTERN NAME": a device whose modalias matches
 *                   PATTERN (shell wildcards) wants module NAME
 *   modules.dep     "PATH: DEP...": a module's file and every module it needs
 *   modules.softdep "softdep NAME pre: ... post: ...": optional pre/post deps
 * Configuration follows kmod and systemd-modules-load precedence (/etc over
 * /usr/local/lib over /usr/lib, basename shadowing, lexicographical order):
 *   modprobe.d (.conf)     options, blacklist, softdep
 *   /proc/cmdline          modprobe.blacklist=mod1,mod2
 *   modules-load.d (.conf) modules loaded explicitly before coldplug
 * At start, explicit modules are loaded, then every device under /sys/devices
 * with a modalias gets its modules (coldplug); after that, devices that appear
 * are announced on a netlink socket (uevents), and get theirs the same way. A
 * module is loaded with finit_module; the kernel decompresses .ko.zst itself.
 * Firmware is the kernel's own business (firmware_class.path on its command line).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "internal.h"

#define MODULE_INIT_COMPRESSED_FILE 4

#ifndef MODLOAD_FINIT_MODULE
#define MODLOAD_FINIT_MODULE(fd, opts, flags) syscall(SYS_finit_module, (fd), (opts), (flags))
#endif

struct alias {
    char *pattern, *name;
};

struct module {
    char *name;   /* with '-' turned into '_' */
    char *path;   /* relative to the module directory */
    char **deps;  /* names */
    int ndeps;
    int state;    /* 0 not tried, 1 loading, 2 loaded (or built in), 3 failed */
};

struct mod_option {
    char *name;   /* normalised ('-' -> '_') */
    char *params; /* space-separated options passed to finit_module */
};

struct softdep {
    char *name;   /* normalised ('-' -> '_') */
    char **pre;
    int npre, cappre;
    char **post;
    int npost, cappost;
};

struct alias_bucket {
    char prefix[32]; /* subsystem prefix before ':', or "" for wildcard/other */
    int *indices;
    int count, cap;
};

static char dir[512];
static char *dep_buf;
static char *alias_buf;
static struct alias *aliases;
static int naliases;
static int aliases_indexed;
static struct alias_bucket *buckets;
static int nbuckets, capbuckets;
static int *wild_aliases;
static int nwild_aliases, capwild_aliases;
static struct module *modules;
static int nmodules;
static int *mod_hash; /* open-addressing table: entry = module index + 1, 0 = empty */
static int mod_hash_size;
static struct mod_option *mod_options;
static int nmod_options, capmod_options;
static char **blacklist;
static int nblacklist, capblacklist;
static struct softdep *softdeps;
static int nsoftdeps, capsoftdeps;
static const char *modload_cmdline_path = "/proc/cmdline";
static const char *modload_sysfs_devices = "/sys/devices";
static int modload_quiet_ms = 500;
static int uevents = -1;
static int input_changed;
static int loaded, unused, failed;

static int load(struct module *m);
static int load_for(const char *modalias);

static unsigned int hash_str(const char *s)
{
    unsigned int h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static char *read_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;

    if (fd < 0 || fstat(fd, &st) < 0) {
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    ssize_t n = buf ? read(fd, buf, (size_t)st.st_size) : -1;
    close(fd);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/* Read a regular .conf file without blocking on FIFOs or device nodes.
 * Logs unreadable regular files; skips symlinks to /dev/null quietly. */
static char *read_conf_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        say("modules: cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        say("modules: cannot stat %s: %s", path, strerror(errno));
        close(fd);
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return NULL; /* e.g. /dev/null override or FIFO: never block boot */
    }
    if (st.st_size <= 0) {
        close(fd);
        return strdup("");
    }
    size_t want = (size_t)st.st_size;
    if (want > 1024 * 1024) {
        want = 1024 * 1024;
    }
    char *buf = malloc(want + 1);
    if (buf == NULL) {
        close(fd);
        return NULL;
    }
    size_t total = 0;
    while (total < want) {
        ssize_t n = read(fd, buf + total, want - total);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            if (n < 0) {
                say("modules: cannot read %s: %s", path, strerror(errno));
                free(buf);
                close(fd);
                return NULL;
            }
            break;
        }
        total += (size_t)n;
    }
    close(fd);
    buf[total] = '\0';
    return buf;
}

/* Read a procfs-style file (whose st_size may be 0) up to 64 KiB. */
static char *read_proc_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        if (errno != ENOENT) {
            say("modules: cannot open %s: %s", path, strerror(errno));
        }
        return NULL;
    }
    const size_t max_size = 64 * 1024;
    size_t cap = 4096;
    size_t total = 0;
    char *buf = malloc(cap + 1);
    if (buf == NULL) {
        close(fd);
        return NULL;
    }
    for (;;) {
        if (total == cap) {
            if (cap >= max_size) {
                break;
            }
            size_t next = cap * 2;
            if (next > max_size) {
                next = max_size;
            }
            char *p = realloc(buf, next + 1);
            if (p == NULL) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = p;
            cap = next;
        }
        ssize_t n = read(fd, buf + total, cap - total);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            say("modules: cannot read %s: %s", path, strerror(errno));
            free(buf);
            close(fd);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    close(fd);
    buf[total] = '\0';
    return buf;
}

static void normalise(char *s)
{
    for (; *s; s++) {
        if (*s == '-') {
            *s = '_';
        }
    }
}

/* The module's name from its path: "kernel/a/b/foo-bar.ko.zst" -> "foo_bar". */
static char *name_of(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t len = strcspn(base, ".");
    char *name = strndup(base, len);
    if (name != NULL) {
        normalise(name);
    }
    return name;
}

static struct module *find(const char *name)
{
    if (mod_hash != NULL && mod_hash_size > 0) {
        unsigned int mask = (unsigned int)(mod_hash_size - 1);
        unsigned int slot = hash_str(name) & mask;
        for (int probes = 0; probes < mod_hash_size; probes++) {
            int entry = mod_hash[slot];
            if (entry == 0) {
                return NULL;
            }
            if (strcmp(modules[entry - 1].name, name) == 0) {
                return &modules[entry - 1];
            }
            slot = (slot + 1) & mask;
        }
        return NULL;
    }
    for (int i = 0; i < nmodules; i++) {
        if (strcmp(modules[i].name, name) == 0) {
            return &modules[i];
        }
    }
    return NULL;
}

static void build_module_index(void)
{
    int size = 64;
    while (size < nmodules * 2) {
        size *= 2;
    }
    int *table = calloc((size_t)size, sizeof *table);
    if (table == NULL) {
        return; /* fallback to linear scan in find() */
    }
    unsigned int mask = (unsigned int)(size - 1);
    for (int i = 0; i < nmodules; i++) {
        unsigned int slot = hash_str(modules[i].name) & mask;
        while (table[slot] != 0) {
            if (strcmp(modules[table[slot] - 1].name, modules[i].name) == 0) {
                break;
            }
            slot = (slot + 1) & mask;
        }
        if (table[slot] == 0) {
            table[slot] = i + 1;
        }
    }
    free(mod_hash);
    mod_hash = table;
    mod_hash_size = size;
}

static int is_wild(const char *s, size_t len)
{
    for (size_t i = 0; i < len && s[i] != '\0'; i++) {
        if (s[i] == '*' || s[i] == '?' || s[i] == '[') {
            return 1;
        }
    }
    return 0;
}

static struct alias_bucket *find_bucket(const char *prefix, size_t len)
{
    for (int i = 0; i < nbuckets; i++) {
        if (strlen(buckets[i].prefix) == len && memcmp(buckets[i].prefix, prefix, len) == 0) {
            return &buckets[i];
        }
    }
    return NULL;
}

static int append_int(int **arr, int *n, int *cap, int value)
{
    if (*n == *cap) {
        int next = *cap ? *cap * 2 : 32;
        int *p = realloc(*arr, sizeof *p * (size_t)next);
        if (p == NULL) {
            return -1;
        }
        *arr = p;
        *cap = next;
    }
    (*arr)[(*n)++] = value;
    return 0;
}

static void build_alias_buckets(void)
{
    aliases_indexed = 0;
    for (int i = 0; i < naliases; i++) {
        const char *pat = aliases[i].pattern;
        const char *colon = strchr(pat, ':');
        size_t plen = colon != NULL ? (size_t)(colon - pat) : 0;
        if (colon == NULL || plen == 0 || plen >= sizeof buckets[0].prefix || is_wild(pat, plen)) {
            if (append_int(&wild_aliases, &nwild_aliases, &capwild_aliases, i) < 0) {
                return;
            }
            continue;
        }
        struct alias_bucket *b = find_bucket(pat, plen);
        if (b == NULL) {
            if (nbuckets == capbuckets) {
                int next = capbuckets ? capbuckets * 2 : 32;
                struct alias_bucket *p = realloc(buckets, sizeof *p * (size_t)next);
                if (p == NULL) {
                    return;
                }
                buckets = p;
                capbuckets = next;
            }
            b = &buckets[nbuckets++];
            memset(b, 0, sizeof *b);
            memcpy(b->prefix, pat, plen);
            b->prefix[plen] = '\0';
        }
        if (append_int(&b->indices, &b->count, &b->cap, i) < 0) {
            return;
        }
    }
    aliases_indexed = 1;
}

static const char *options_for(const char *name)
{
    for (int i = 0; i < nmod_options; i++) {
        if (strcmp(mod_options[i].name, name) == 0) {
            return mod_options[i].params;
        }
    }
    return "";
}

static void add_module_options(const char *raw_name, const char *params)
{
    while (*params == ' ' || *params == '\t') {
        params++;
    }
    size_t plen = strlen(params);
    while (plen > 0 && (params[plen - 1] == ' ' || params[plen - 1] == '\t' || params[plen - 1] == '\r')) {
        plen--;
    }
    if (!raw_name[0] || plen == 0) {
        return;
    }
    char *name = strdup(raw_name);
    if (name == NULL) {
        return;
    }
    normalise(name);
    for (int i = 0; i < nmod_options; i++) {
        if (strcmp(mod_options[i].name, name) == 0) {
            size_t old_len = strlen(mod_options[i].params);
            char *joined = realloc(mod_options[i].params, old_len + 1 + plen + 1);
            if (joined != NULL) {
                joined[old_len] = ' ';
                memcpy(joined + old_len + 1, params, plen);
                joined[old_len + 1 + plen] = '\0';
                mod_options[i].params = joined;
            }
            free(name);
            return;
        }
    }
    if (nmod_options == capmod_options) {
        int next = capmod_options ? capmod_options * 2 : 16;
        struct mod_option *p = realloc(mod_options, sizeof *p * (size_t)next);
        if (p == NULL) {
            free(name);
            return;
        }
        mod_options = p;
        capmod_options = next;
    }
    char *copy = strndup(params, plen);
    if (copy == NULL) {
        free(name);
        return;
    }
    mod_options[nmod_options++] = (struct mod_option){ .name = name, .params = copy };
}

static int is_blacklisted(const char *name)
{
    for (int i = 0; i < nblacklist; i++) {
        if (strcmp(blacklist[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void add_blacklist(const char *raw_name)
{
    if (!raw_name[0]) {
        return;
    }
    char *name = strdup(raw_name);
    if (name == NULL) {
        return;
    }
    normalise(name);
    if (is_blacklisted(name)) {
        free(name);
        return;
    }
    if (nblacklist == capblacklist) {
        int next = capblacklist ? capblacklist * 2 : 16;
        char **p = realloc(blacklist, sizeof *p * (size_t)next);
        if (p == NULL) {
            free(name);
            return;
        }
        blacklist = p;
        capblacklist = next;
    }
    blacklist[nblacklist++] = name;
}

static struct softdep *find_softdep(const char *name)
{
    for (int i = 0; i < nsoftdeps; i++) {
        if (strcmp(softdeps[i].name, name) == 0) {
            return &softdeps[i];
        }
    }
    return NULL;
}

static struct softdep *get_or_add_softdep(const char *raw_name)
{
    if (!raw_name[0]) {
        return NULL;
    }
    char *name = strdup(raw_name);
    if (name == NULL) {
        return NULL;
    }
    normalise(name);
    struct softdep *existing = find_softdep(name);
    if (existing != NULL) {
        free(name);
        return existing;
    }
    if (nsoftdeps == capsoftdeps) {
        int next = capsoftdeps ? capsoftdeps * 2 : 16;
        struct softdep *p = realloc(softdeps, sizeof *p * (size_t)next);
        if (p == NULL) {
            free(name);
            return NULL;
        }
        softdeps = p;
        capsoftdeps = next;
    }
    struct softdep *sd = &softdeps[nsoftdeps++];
    *sd = (struct softdep){ .name = name };
    return sd;
}

static int same_target(const char *a, const char *b)
{
    if (strcmp(a, b) == 0) {
        return 1;
    }
    if (strchr(a, ':') != NULL || strchr(b, ':') != NULL) {
        return 0;
    }
    for (; *a && *b; a++, b++) {
        char ca = *a == '-' ? '_' : *a;
        char cb = *b == '-' ? '_' : *b;
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

static void add_softdep_target(char ***list, int *n, int *cap, const char *raw_target)
{
    if (!raw_target[0]) {
        return;
    }
    for (int i = 0; i < *n; i++) {
        if (same_target((*list)[i], raw_target)) {
            return;
        }
    }
    if (*n == *cap) {
        int next = *cap ? *cap * 2 : 4;
        char **p = realloc(*list, sizeof *p * (size_t)next);
        if (p == NULL) {
            return;
        }
        *list = p;
        *cap = next;
    }
    char *copy = strdup(raw_target);
    if (copy == NULL) {
        return;
    }
    (*list)[(*n)++] = copy;
}

/* Parse the arguments after "softdep ": "<modulename> pre: ... post: ..." */
static void parse_softdep_line(char *s)
{
    s += strspn(s, " \t");
    size_t mlen = strcspn(s, " \t\r");
    if (mlen == 0 || s[mlen] == '\0') {
        return;
    }
    s[mlen] = '\0';
    const char *modname = s;
    struct softdep *sd = NULL;
    int mode = 0; /* 1 = pre, 2 = post */

    for (char *p = s + mlen + 1; *(p += strspn(p, " \t\r")) != '\0';) {
        size_t len = strcspn(p, " \t\r");
        char saved = p[len];
        p[len] = '\0';
        if (strcmp(p, "pre:") == 0) {
            mode = 1;
        } else if (strcmp(p, "post:") == 0) {
            mode = 2;
        } else if (mode != 0) {
            if (sd == NULL) {
                sd = get_or_add_softdep(modname);
                if (sd == NULL) {
                    return;
                }
            }
            if (mode == 1) {
                add_softdep_target(&sd->pre, &sd->npre, &sd->cappre, p);
            } else {
                add_softdep_target(&sd->post, &sd->npost, &sd->cappost, p);
            }
        }
        if (saved == '\0') {
            break;
        }
        p += len + 1;
    }
}

struct conf_entry {
    char *base;
    char *path;
};

static int cmp_conf_entry(const void *a, const void *b)
{
    const struct conf_entry *x = a;
    const struct conf_entry *y = b;
    return strcmp(x->base, y->base);
}

static void parse_modprobe_file(const char *path)
{
    char *text = read_conf_file(path);
    if (text == NULL) {
        return;
    }
    for (char *line = text; *line;) {
        char *nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        char *s = line + strspn(line, " \t\r");
        if (*s != '\0' && *s != '#') {
            char *hash = strchr(s, '#');
            if (hash != NULL) {
                *hash = '\0';
            }
            if (strncmp(s, "options", 7) == 0 && (s[7] == ' ' || s[7] == '\t')) {
                char *m = s + 7;
                m += strspn(m, " \t");
                size_t mlen = strcspn(m, " \t\r");
                if (mlen > 0 && m[mlen] != '\0') {
                    m[mlen] = '\0';
                    add_module_options(m, m + mlen + 1);
                }
            } else if (strncmp(s, "blacklist", 9) == 0 && (s[9] == ' ' || s[9] == '\t')) {
                char *m = s + 9;
                m += strspn(m, " \t");
                size_t mlen = strcspn(m, " \t\r");
                if (mlen > 0) {
                    m[mlen] = '\0';
                    add_blacklist(m);
                }
            } else if (strncmp(s, "softdep", 7) == 0 && (s[7] == ' ' || s[7] == '\t')) {
                parse_softdep_line(s + 7);
            }
        }
        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }
    free(text);
}

/* Collect .conf entries from dirpath, skipping any basename already seen in a
 * higher-priority directory (matching kmod/modprobe.d and modules-load.d
 * override precedence). */
static void collect_conf_dir(const char *dirpath, struct conf_entry **entries, int *n, int *cap)
{
    DIR *d = opendir(dirpath);
    if (d == NULL) {
        return;
    }
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len <= 5 || strcmp(e->d_name + len - 5, ".conf") != 0) {
            continue;
        }
        int shadowed = 0;
        for (int i = 0; i < *n; i++) {
            if (strcmp((*entries)[i].base, e->d_name) == 0) {
                shadowed = 1;
                break;
            }
        }
        if (shadowed) {
            continue;
        }
        if (*n == *cap) {
            int next = *cap ? *cap * 2 : 16;
            struct conf_entry *p = realloc(*entries, sizeof *p * (size_t)next);
            if (p == NULL) {
                break;
            }
            *entries = p;
            *cap = next;
        }
        char full[1024];
        if (snprintf(full, sizeof full, "%s/%s", dirpath, e->d_name) >= (int)sizeof full) {
            continue;
        }
        char *base = strdup(e->d_name);
        char *path = strdup(full);
        if (base == NULL || path == NULL) {
            free(base);
            free(path);
            continue;
        }
        (*entries)[(*n)++] = (struct conf_entry){ .base = base, .path = path };
    }
    closedir(d);
}

/* Parse /proc/cmdline for modprobe.blacklist=mod1,mod2,... tokens, handling
 * whitespace separation and double-quoted segments as the kernel does. */
static void load_cmdline_blacklist(const char *cmdline_path)
{
    char *buf = read_proc_file(cmdline_path);
    if (buf == NULL) {
        return;
    }
    char *p = buf;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char *tok = p;
        char *dst = p;
        int in_quote = 0;
        while (*p != '\0') {
            if (*p == '"') {
                in_quote = !in_quote;
                p++;
                continue;
            }
            if (!in_quote && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
                p++;
                break;
            }
            *dst++ = *p++;
        }
        *dst = '\0';
        if (strncmp(tok, "modprobe.blacklist=", 19) == 0) {
            char *q = tok + 19;
            while (*q != '\0') {
                char *comma = strchr(q, ',');
                if (comma != NULL) {
                    *comma = '\0';
                }
                add_blacklist(q);
                if (comma == NULL) {
                    break;
                }
                q = comma + 1;
            }
        }
    }
    free(buf);
}

/* Load modprobe.d configuration (options, blacklist, softdep) matching kmod
 * precedence: /etc/modprobe.d > /usr/local/lib/modprobe.d > /usr/lib/modprobe.d
 * (earlier matching basenames shadow later ones, so an empty file or /dev/null
 * symlink masks lower-priority files), parsed in lexicographical basename
 * order, plus modprobe.blacklist= from the kernel command line. */
static void load_modprobe_config(const char *root)
{
    struct conf_entry *entries = NULL;
    int n = 0, cap = 0;
    char path[1024];

    snprintf(path, sizeof path, "%s/etc/modprobe.d", root);
    collect_conf_dir(path, &entries, &n, &cap);
    snprintf(path, sizeof path, "%s/usr/local/lib/modprobe.d", root);
    collect_conf_dir(path, &entries, &n, &cap);
    snprintf(path, sizeof path, "%s/usr/lib/modprobe.d", root);
    collect_conf_dir(path, &entries, &n, &cap);
    if (n > 1) {
        qsort(entries, (size_t)n, sizeof *entries, cmp_conf_entry);
    }
    for (int i = 0; i < n; i++) {
        parse_modprobe_file(entries[i].path);
        free(entries[i].base);
        free(entries[i].path);
    }
    free(entries);
    load_cmdline_blacklist(modload_cmdline_path);
}

static void load_modules_softdep(void)
{
    char path[600];
    snprintf(path, sizeof path, "%s/modules.softdep", dir);
    char *text = read_file(path);
    if (text == NULL) {
        return;
    }
    for (char *line = text; *line;) {
        char *nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        char *s = line + strspn(line, " \t\r");
        if (*s != '\0' && *s != '#') {
            char *hash = strchr(s, '#');
            if (hash != NULL) {
                *hash = '\0';
            }
            if (strncmp(s, "softdep", 7) == 0 && (s[7] == ' ' || s[7] == '\t')) {
                parse_softdep_line(s + 7);
            }
        }
        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }
    free(text);
}

static int load_tables(void)
{
    char path[600];
    int cap_modules = 0, cap_aliases = 0;

    snprintf(path, sizeof path, "%s/modules.dep", dir);
    char *dep = read_file(path);
    snprintf(path, sizeof path, "%s/modules.alias", dir);
    char *alias = read_file(path);
    if (dep == NULL || alias == NULL) {
        free(dep);
        free(alias);
        say("modules: cannot read %s/modules.dep or modules.alias", dir);
        return -1;
    }
    dep_buf = dep;
    alias_buf = alias;
    for (char *line = strtok(dep, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        char *colon = strchr(line, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        if (nmodules == cap_modules) {
            int next = cap_modules ? cap_modules * 2 : 1024;
            struct module *p = realloc(modules, sizeof *modules * (size_t)next);
            if (p == NULL) {
                say("modules: out of memory reading modules.dep");
                return -1;
            }
            modules = p;
            cap_modules = next;
        }
        struct module *m = &modules[nmodules++];
        *m = (struct module){ .path = line, .name = name_of(line) };
        if (m->name == NULL) {
            nmodules--;
            continue;
        }
        int cap_deps = 0;
        for (char *d = colon + 1; (d = d + strspn(d, " ")) && *d;) {
            size_t len = strcspn(d, " ");
            char *one = strndup(d, len);
            if (one == NULL) {
                break;
            }
            if (m->ndeps == cap_deps) {
                int next = cap_deps ? cap_deps * 2 : 4;
                char **dp = realloc(m->deps, sizeof *m->deps * (size_t)next);
                if (dp == NULL) {
                    free(one);
                    break;
                }
                m->deps = dp;
                cap_deps = next;
            }
            char *dname = name_of(one);
            free(one);
            if (dname == NULL) {
                break;
            }
            m->deps[m->ndeps++] = dname;
            d += len;
        }
    }
    for (char *line = strtok(alias, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        char *pattern = NULL, *name = NULL;
        if (strncmp(line, "alias ", 6) != 0 || (pattern = strtok_r(line + 6, " ", &name)) == NULL ||
            name == NULL || *name == '\0') {
            continue;
        }
        normalise(name);
        if (naliases == cap_aliases) {
            int next = cap_aliases ? cap_aliases * 2 : 4096;
            struct alias *p = realloc(aliases, sizeof *aliases * (size_t)next);
            if (p == NULL) {
                say("modules: out of memory reading modules.alias");
                return -1;
            }
            aliases = p;
            cap_aliases = next;
        }
        aliases[naliases++] = (struct alias){ pattern, name };
    }
    build_module_index();
    build_alias_buckets();
    load_modules_softdep();
    return 0;
}

/* Load a module by name or alias, respecting blacklist when requested. */
static int load_named(const char *raw, int respect_blacklist)
{
    if (!raw[0]) {
        return -1;
    }
    char *norm = strdup(raw);
    if (norm == NULL) {
        return -1;
    }
    normalise(norm);
    if (respect_blacklist && is_blacklisted(norm)) {
        free(norm);
        return 0;
    }
    struct module *m = find(norm);
    if (m != NULL) {
        free(norm);
        return load(m);
    }
    int matched = load_for(raw);
    if (matched == 0 && strchr(raw, ':') == NULL && strcmp(norm, raw) != 0) {
        matched = load_for(norm);
    }
    free(norm);
    return matched > 0 ? 0 : -1;
}

/* Load a module, its pre: softdeps, its hard dependencies, and its post: softdeps. */
static int load(struct module *m)
{
    if (m->state == 2 || m->state == 1) {
        return 0;
    }
    if (m->state == 3) {
        return -1;
    }
    m->state = 1;
    struct softdep *sd = find_softdep(m->name);
    if (sd != NULL) {
        for (int i = 0; i < sd->npre; i++) {
            (void)load_named(sd->pre[i], 1);
        }
    }
    for (int i = m->ndeps - 1; i >= 0; i--) {
        struct module *d = find(m->deps[i]);
        if (d != NULL && load(d) < 0) {
            m->state = 3;
            return -1;
        }
    }
    char path[1024];
    double t0 = uptime();
    int plen = snprintf(path, sizeof path, "%s/%s", dir, m->path);
    int fd = (plen < 0 || plen >= (int)sizeof path) ? -1 : open(path, O_RDONLY | O_CLOEXEC);
    if (plen < 0 || plen >= (int)sizeof path) {
        errno = ENAMETOOLONG;
    }
    int flags = strstr(m->path, ".ko.") ? MODULE_INIT_COMPRESSED_FILE : 0;
    const char *opts = options_for(m->name);
    int rc = fd < 0 ? -1 : (int)MODLOAD_FINIT_MODULE(fd, opts, flags);
    int err = errno;
    if (fd >= 0 && rc < 0 && err != EEXIST && opts[0] != '\0') {
        say("modules: %s with options \"%s\" failed (%s); retrying without options",
            m->name, opts, strerror(err));
        if (lseek(fd, 0, SEEK_SET) == 0) {
            rc = (int)MODLOAD_FINIT_MODULE(fd, "", flags);
            err = errno;
        }
    }
    if (fd < 0 || (rc < 0 && err != EEXIST)) {
        if (fd >= 0) {
            close(fd);
        }
        m->state = 3;
        if (err == ENODEV) {
            unused++; /* matched, but the hardware it wants is not here */
        } else {
            say("modules: %s: %s", m->name, strerror(err));
            failed++;
        }
        return -1;
    }
    close(fd);
    m->state = 2;
    loaded++;
    double ms = (uptime() - t0) * 1000;
    if (ms >= 20) {
        say("modules: %s took %.0f ms", m->name, ms); /* to see where boot time goes */
    }
    if (sd != NULL) {
        for (int i = 0; i < sd->npost; i++) {
            (void)load_named(sd->post[i], 1);
        }
    }
    return 0;
}

static int match_alias(int idx, const char *modalias, size_t prefix)
{
    const char *p = aliases[idx].pattern;
    if (modalias[prefix] == ':' && !is_wild(p, prefix) && strncmp(p, modalias, prefix) != 0) {
        return 0;
    }
    if (fnmatch(p, modalias, 0) != 0) {
        return 0;
    }
    if (is_blacklisted(aliases[idx].name)) {
        return 1;
    }
    struct module *m = find(aliases[idx].name);
    if (m != NULL) {
        load(m);
    }
    return 1;
}

/* Load every module whose alias matches this modalias. */
static int load_for(const char *modalias)
{
    size_t prefix = strcspn(modalias, ":");
    int matched = 0;

    if (!aliases_indexed) {
        for (int i = 0; i < naliases; i++) {
            matched += match_alias(i, modalias, prefix);
        }
        return matched;
    }
    if (modalias[prefix] == ':' && prefix > 0 && prefix < sizeof buckets[0].prefix) {
        struct alias_bucket *b = find_bucket(modalias, prefix);
        if (b != NULL) {
            for (int i = 0; i < b->count; i++) {
                matched += match_alias(b->indices[i], modalias, prefix);
            }
        }
    } else {
        for (int bi = 0; bi < nbuckets; bi++) {
            struct alias_bucket *b = &buckets[bi];
            if (strncmp(b->prefix, modalias, prefix) != 0) {
                continue;
            }
            for (int i = 0; i < b->count; i++) {
                matched += match_alias(b->indices[i], modalias, prefix);
            }
        }
    }
    for (int i = 0; i < nwild_aliases; i++) {
        matched += match_alias(wild_aliases[i], modalias, prefix);
    }
    return matched;
}

static void parse_modules_load_file(const char *path)
{
    char *text = read_conf_file(path);
    if (text == NULL) {
        return;
    }
    for (char *line = text; *line;) {
        char *nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        char *s = line + strspn(line, " \t\r");
        if (*s != '\0' && *s != '#' && *s != ';') {
            size_t comment = strcspn(s, "#;");
            s[comment] = '\0';
            size_t len = strlen(s);
            while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) {
                s[--len] = '\0';
            }
            if (len > 0) {
                (void)load_named(s, 1);
            }
        }
        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }
    free(text);
}

/* Load modules listed in modules-load.d (.conf files) following
 * systemd-modules-load precedence: /etc/modules-load.d >
 * /usr/local/lib/modules-load.d > /usr/lib/modules-load.d (basename shadowing,
 * lexicographical order). */
static void load_configured_modules(const char *root)
{
    struct conf_entry *entries = NULL;
    int n = 0, cap = 0;
    char path[1024];

    snprintf(path, sizeof path, "%s/etc/modules-load.d", root);
    collect_conf_dir(path, &entries, &n, &cap);
    snprintf(path, sizeof path, "%s/usr/local/lib/modules-load.d", root);
    collect_conf_dir(path, &entries, &n, &cap);
    snprintf(path, sizeof path, "%s/usr/lib/modules-load.d", root);
    collect_conf_dir(path, &entries, &n, &cap);
    if (n > 1) {
        qsort(entries, (size_t)n, sizeof *entries, cmp_conf_entry);
    }
    for (int i = 0; i < n; i++) {
        parse_modules_load_file(entries[i].path);
        free(entries[i].base);
        free(entries[i].path);
    }
    free(entries);
}

/* Every modalias file under path. */
static void coldplug(const char *path, int depth)
{
    DIR *d = opendir(path);
    char sub[1024];

    if (d == NULL || depth > 24) {
        if (d != NULL) {
            closedir(d);
        }
        return;
    }
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (e->d_name[0] == '.') {
            continue;
        }
        if (snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) >= (int)sizeof sub) {
            continue;
        }
        unsigned char dt = e->d_type;
        if (dt == DT_UNKNOWN) {
            struct stat st;
            if (lstat(sub, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    dt = DT_DIR;
                } else if (S_ISREG(st.st_mode)) {
                    dt = DT_REG;
                }
            }
        }
        if (dt == DT_DIR) {
            coldplug(sub, depth + 1);
        } else if (dt == DT_REG && strcmp(e->d_name, "modalias") == 0) {
            char *alias = read_file(sub);
            if (alias != NULL) {
                alias[strcspn(alias, "\n")] = '\0';
                if (alias[0]) {
                    load_for(alias);
                }
                free(alias);
            }
        }
    }
    closedir(d);
}

static int payload_has_field(const char *buf, size_t size, const char *field)
{
    size_t flen = strlen(field);
    if (size < flen + 1) {
        return 0;
    }
    const char *p = buf;
    const char *end = buf + size;
    while (p < end) {
        const char *z = memchr(p, '\0', (size_t)(end - p));
        if (z == NULL) {
            break;
        }
        if ((size_t)(z - p) == flen && memcmp(p, field, flen) == 0) {
            return 1;
        }
        p = z + 1;
    }
    return 0;
}

int modules_event(void)
{
    char buf[8192];
    ssize_t n;
    int changed = 0;

    input_changed = 0;
    while ((n = recv(uevents, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
        /* Processed udev events wake init to retry input after rule tagging.
         * Their binary header is not a second kernel DRM/module event. */
        if (n >= 8 && memcmp(buf, "libudev", 8) == 0) {
            if (payload_has_field(buf + 8, (size_t)(n - 8), "SUBSYSTEM=input")) {
                input_changed = 1;
            } else if (n >= 40) {
                uint32_t off = 0;
                memcpy(&off, buf + 16, sizeof off);
                if (off >= 24 && (size_t)off < (size_t)n &&
                    payload_has_field(buf + off, (size_t)n - off, "SUBSYSTEM=input")) {
                    input_changed = 1;
                }
            }
            continue;
        }
        char *end = buf + n;
        char *next = memchr(buf, '\0', (size_t)n);
        if (next == NULL) {
            continue;
        }
        int add = next - buf >= 4 && memcmp(buf, "add@", 4) == 0;
        int drm = 0, power = 0, input = 0, action = 0, hotplug = 0, lease = 0, synthetic = 0;
        for (char *k = next + 1; k < end; k = next + 1) {
            next = memchr(k, '\0', (size_t)(end - k));
            if (next == NULL) {
                break; /* Ignore an incomplete trailing field. */
            }
            if (strcmp(k, "SUBSYSTEM=drm") == 0) {
                drm = 1;
            } else if (strcmp(k, "SUBSYSTEM=power_supply") == 0) {
                power = 1;
            } else if (strcmp(k, "SUBSYSTEM=input") == 0) {
                input = 1;
            } else if (strcmp(k, "ACTION=add") == 0 || strcmp(k, "ACTION=change") == 0 ||
                       strcmp(k, "ACTION=remove") == 0) {
                action = 1;
            } else if (strcmp(k, "HOTPLUG=1") == 0) {
                hotplug = 1;
            } else if (strcmp(k, "LEASE=1") == 0) {
                lease = 1;
            } else if (strncmp(k, "SYNTH_UUID=", 11) == 0) {
                synthetic = 1;
            } else if (add && next - k >= 9 && memcmp(k, "MODALIAS=", 9) == 0) {
                load_for(k + 9);
            }
        }
        if (input) {
            input_changed = 1;
        }
        /* A lease made or ended (the screen lent, taken back) is no change
         * of displays: counting it would refresh, lend again, and loop. Nor
         * is a replay (udevadm trigger at boot): it takes the screen back
         * from home while home is starting. Its modules are still loaded. */
        changed |= drm && (action || hotplug) && !lease && !synthetic ? MODULES_DISPLAY : 0;
        /* A battery or charger changed (power_supply_changed_work's
         * KOBJ_CHANGE, a supply added or removed): process 1 looks at the
         * batteries for the critical-battery action. A replay too: it may
         * be the first word of a battery there was already. */
        changed |= power && action ? MODULES_POWER : 0;
    }
    return changed;
}

int modules_input_changed(void)
{
    return input_changed;
}

int modules_fd(void)
{
    return uevents;
}

static void __attribute__((unused)) modload_reset_for_test(void)
{
    if (uevents >= 0) {
        close(uevents);
        uevents = -1;
    }
    for (int i = 0; i < nmodules; i++) {
        free(modules[i].name);
        for (int j = 0; j < modules[i].ndeps; j++) {
            free(modules[i].deps[j]);
        }
        free(modules[i].deps);
    }
    free(modules);
    modules = NULL;
    nmodules = 0;
    free(mod_hash);
    mod_hash = NULL;
    mod_hash_size = 0;
    free(aliases);
    aliases = NULL;
    naliases = 0;
    aliases_indexed = 0;
    for (int i = 0; i < nbuckets; i++) {
        free(buckets[i].indices);
    }
    free(buckets);
    buckets = NULL;
    nbuckets = capbuckets = 0;
    free(wild_aliases);
    wild_aliases = NULL;
    nwild_aliases = capwild_aliases = 0;
    for (int i = 0; i < nmod_options; i++) {
        free(mod_options[i].name);
        free(mod_options[i].params);
    }
    free(mod_options);
    mod_options = NULL;
    nmod_options = capmod_options = 0;
    for (int i = 0; i < nblacklist; i++) {
        free(blacklist[i]);
    }
    free(blacklist);
    blacklist = NULL;
    nblacklist = capblacklist = 0;
    for (int i = 0; i < nsoftdeps; i++) {
        free(softdeps[i].name);
        for (int j = 0; j < softdeps[i].npre; j++) {
            free(softdeps[i].pre[j]);
        }
        free(softdeps[i].pre);
        for (int j = 0; j < softdeps[i].npost; j++) {
            free(softdeps[i].post[j]);
        }
        free(softdeps[i].post);
    }
    free(softdeps);
    softdeps = NULL;
    nsoftdeps = capsoftdeps = 0;
    free(dep_buf);
    dep_buf = NULL;
    free(alias_buf);
    alias_buf = NULL;
    dir[0] = '\0';
    input_changed = 0;
    loaded = unused = failed = 0;
    modload_cmdline_path = "/proc/cmdline";
    modload_sysfs_devices = "/sys/devices";
    modload_quiet_ms = 500;
}

int modules_start(const char *root)
{
    struct utsname u;
    /* Kernel events load drivers; udev's processed events make newly tagged
     * devices available to libinput in an already-running framebuffer front. */
    struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_groups = 1 | 2 };

    uname(&u);
    snprintf(dir, sizeof dir, "%s/lib/modules/%s", root, u.release);
    load_modprobe_config(root);
    if (load_tables() < 0) {
        return -1;
    }
    /* Listen first, so that nothing that appears while we walk is missed. */
    uevents = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (uevents >= 0 && bind(uevents, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(uevents);
        uevents = -1;
    }
    load_configured_modules(root);
    double t0 = uptime();
    coldplug(modload_sysfs_devices, 0);
    double t1 = uptime();
    /* Devices the new drivers made (USB devices, sound codecs, ...): until
     * nothing new has come for half a second. Late lid and power devices are
     * not rescanned here: process 1 rescans its system inputs only once one is
     * lost, as it always has. */
    struct pollfd p = { .fd = uevents, .events = POLLIN };
    while (uevents >= 0 && modload_quiet_ms > 0 && poll(&p, 1, modload_quiet_ms) > 0) {
        modules_event();
    }
    say("modules: coldplug %.0f ms, then new devices until quiet %.0f ms (the last %d ms of it waiting)",
        (t1 - t0) * 1000, (uptime() - t1) * 1000, modload_quiet_ms);
    say("modules: %s: %d modules, %d aliases; loaded %d, %d failed (%d found no device)", u.release,
        nmodules, naliases, loaded, failed, unused);
    return 0;
}
