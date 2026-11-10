/* Fixed, non-shell configuration client. The public command accepts only the
 * startup file or one named container, so callers cannot redirect writes through
 * an environment variable or an arbitrary path. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

/* The focused test supplies compile-time paths, never runtime overrides. */
#ifndef CONFIG_RC_PATH
#define CONFIG_RC_PATH "/etc/locachy/rc.conf"
#endif
#ifndef CONFIG_CONTAINER_DIR
#define CONFIG_CONTAINER_DIR "/etc/locachy/containers"
#endif

static int valid_container_name(const char *s)
{
    if (!s || !s[0] || !strncmp(s, "window-", 7) || strlen(s) > 64) return 0;
    const unsigned char *p = (const unsigned char *)s;
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return 0;
    for (; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return 0;
    }
    return 1;
}

static int valid_key_name(const char *s)
{
    if (!s || !s[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    }
    return 1;
}

static void usage(void)
{
    fprintf(stderr, "usage: locachy-config rc (set NAME VALUE | unset NAME) ...\n"
                    "       locachy-config container NAME (set NAME VALUE | unset NAME) ...\n");
}

static int apply(const char *path, int argc, char **argv)
{
    struct conf_change *changes;
    int count = 0;
    for (int i = 0; i < argc; ) {
        if (!strcmp(argv[i], "set")) {
            if (++i >= argc || i + 1 >= argc) { errno = EINVAL; return -1; }
            i += 2; count++;
        } else if (!strcmp(argv[i], "unset")) {
            if (++i >= argc) { errno = EINVAL; return -1; }
            i++; count++;
        } else {
            errno = EINVAL;
            return -1;
        }
    }
    if (!count) { errno = EINVAL; return -1; }
    changes = calloc((size_t)count, sizeof *changes);
    if (!changes) return -1;
    int n = 0;
    for (int i = 0; i < argc; ) {
        if (!strcmp(argv[i], "set")) {
            changes[n].name = argv[++i];
            changes[n++].value = argv[++i];
        } else {
            changes[n].name = argv[++i];
            changes[n++].value = NULL;
        }
        i++;
    }
    for (int i = 0; i < count; i++) {
        if (!valid_key_name(changes[i].name)) {
            free(changes);
            errno = EINVAL;
            return -1;
        }
    }
    int result = conf_update(path, changes, (size_t)count);
    free(changes);
    return result;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int first;
    char container[256];
    if (argc < 4 || !strcmp(argv[1], "--help")) {
        usage();
        return 2;
    }
    if (!strcmp(argv[1], "rc")) {
        if (argc < 4) { usage(); return 2; }
        path = CONFIG_RC_PATH;
        first = 2;
    } else if (!strcmp(argv[1], "container") && argc >= 5 && valid_container_name(argv[2])) {
        if (snprintf(container, sizeof container, "%s/%s.conf", CONFIG_CONTAINER_DIR, argv[2]) >= (int)sizeof container) {
            fprintf(stderr, "locachy-config: container name is too long\n");
            return 2;
        }
        path = container;
        first = 3;
    } else {
        usage();
        return 2;
    }
    if (apply(path, argc - first, argv + first) < 0) {
        perror("locachy-config");
        return 1;
    }
    return 0;
}
