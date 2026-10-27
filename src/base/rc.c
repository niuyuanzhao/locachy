/*
 * rc.conf, the system's one startup file, read as BSD reads its rc.conf but
 * without a shell: NAME="VALUE" lines, NAME made of letters, digits and _,
 * the quotes needed only when VALUE has blanks; # starts a comment outside
 * the quotes. No expansion, no escapes. A line that is none of that is
 * logged and skipped.
 */
#include <stdio.h>
#include <string.h>

#include "base.h"

#define RC_LINE 512

int conf_read(const char *path, void (*fn)(const char *name, const char *value, void *arg), void *arg)
{
    char line[RC_LINE];
    const char *file = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    FILE *f = fopen(path, "re");

    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (strchr(line, '\n') == NULL && !feof(f)) {
            say("%s: a line is longer than %d bytes; left out", file, RC_LINE - 1);
            int c;
            while ((c = getc(f)) != '\n' && c != EOF) {
            }
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        char *s = line + strspn(line, " \t");
        if (*s == '\0' || *s == '#') {
            continue;
        }
        char *name = s;
        s += strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_");
        char *value = s + 1;
        if (s == name || *s != '=') {
            say("%s: cannot read \"%s\"", file, line);
            continue;
        }
        *s = '\0';
        char *end, *rest;
        if (*value == '"') {
            end = strchr(++value, '"');
            rest = end != NULL ? end + 1 : NULL;
        } else {
            end = rest = value + strcspn(value, " \t#");
        }
        if (rest != NULL) {
            rest += strspn(rest, " \t");
        }
        if (rest == NULL || (*rest != '\0' && *rest != '#')) {
            say("%s: cannot read the value of %s", file, name);
            continue;
        }
        *end = '\0';
        fn(name, value, arg);
    }
    fclose(f);
    return 0;
}

static void find_machine_key(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "machine") == 0) {
        snprintf(arg, 64, "%s", value);
    }
}

struct machine_pass {
    void (*fn)(const char *name, const char *value, void *arg);
    void *arg;
};

static void forward_machine_kv(const char *name, const char *value, void *arg)
{
    struct machine_pass *mp = arg;
    if (strcmp(name, "description") != 0 && strcmp(name, "machine") != 0) {
        mp->fn(name, value, mp->arg);
    }
}

int rc_read(void (*fn)(const char *name, const char *value, void *arg), void *arg)
{
    char mach[64] = "";
    if (conf_read(RC_CONF, find_machine_key, mach) < 0) {
        return -1;
    }
    if (mach[0] && strspn(mach, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == strlen(mach)) {
        char mpath[256];
        struct machine_pass mp = { fn, arg };
        snprintf(mpath, sizeof mpath, "%s/%s.conf", MACHINES_DIR, mach);
        conf_read(mpath, forward_machine_kv, &mp);
    }
    return conf_read(RC_CONF, fn, arg);
}

int conf_set(const char *path, const char *name, const char *value)
{
    char line[RC_LINE], tmp[512];
    size_t n = strlen(name);
    int done = 0;
    if ((value != NULL && strpbrk(value, "\"\n") != NULL) ||
        snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        return -1;
    }
    FILE *in = fopen(path, "re"), *out = fopen(tmp, "we");
    if (out == NULL) {
        if (in) fclose(in);
        return -1;
    }
    while (in != NULL && fgets(line, sizeof line, in) != NULL) {
        const char *s = line + strspn(line, " \t");
        if (strncmp(s, name, n) == 0 && s[n] == '=') {
            if (value != NULL && !done) {
                fprintf(out, "%s=\"%s\"\n", name, value);
            }
            done = 1;
            continue;
        }
        fputs(line, out);
    }
    if (value != NULL && !done) {
        fprintf(out, "%s=\"%s\"\n", name, value);
    }
    if (in) fclose(in);
    if (fclose(out) != 0) {
        return -1;
    }
    return rename(tmp, path);
}

int rc_set(const char *name, const char *value)
{
    return conf_set(RC_CONF, name, value);
}
