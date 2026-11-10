/* Device-free helper for test-config-transactions.py. Paths come only from
 * that test's scratch directory; this is never installed. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base.h"
#include "config.h"

#ifdef CONFIG_TEST_FAULTS
int __real_fsync(int);
int __real_renameat(int, const char *, int, const char *);
int __wrap_fsync(int fd)
{
    static int calls;
    const char *fail = getenv("CONFIG_TEST_FSYNC");
    if (fail && ++calls == atoi(fail)) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
int __wrap_renameat(int from, const char *a, int to, const char *b)
{
    if (getenv("CONFIG_TEST_RENAME")) { errno = EIO; return -1; }
    return __real_renameat(from, a, to, b);
}
#endif

void say(const char *fmt, ...) { (void)fmt; }

struct increment_arg { const char *key; };
static int increment(const char *old, size_t length, int exists, char **next, size_t *size, void *arg)
{
    (void)length; (void)exists;
    const char *key = ((struct increment_arg *)arg)->key;
    size_t n = strlen(key);
    const char *at = old;
    long value = 0;
    while (at && *at) {
        if (!strncmp(at, key, n) && at[n] == '=') value = strtol(at + n + 2, NULL, 10);
        at = strchr(at, '\n');
        if (at) at++;
    }
    int len = asprintf(next, "%s=\"%ld\"\n", key, value + 1);
    if (len < 0) return -1;
    *size = (size_t)len;
    return 0;
}

static int pause_transform(const char *old, size_t length, int exists, char **next, size_t *size, void *arg)
{
    (void)exists; (void)arg;
    puts("READY");
    fflush(stdout);
    if (getchar() != '\n') { errno = ECANCELED; return -1; }
    *next = malloc(length ? length : 1);
    if (!*next) return -1;
    memcpy(*next, old, length);
    *size = length;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *op = argv[1], *path = argv[2];
    if (!strcmp(op, "pause")) {
        int result = conf_transaction(path, pause_transform, NULL);
        if (result < 0 && errno == ESTALE) return 3;
        return result < 0;
    }
    if (!strcmp(op, "increment")) {
        int count = atoi(argv[3]);
        struct increment_arg arg = { "counter" };
        for (int i = 0; i < count; i++) if (conf_transaction(path, increment, &arg) < 0) return 1;
        return 0;
    }
    if (!strcmp(op, "set") && argc == 5) return conf_set(path, argv[3], argv[4]) < 0;
    if (!strcmp(op, "unset") && argc == 4) return conf_set(path, argv[3], NULL) < 0;
    if (!strcmp(op, "batch") && argc >= 5 && (argc - 3) % 2 == 0) {
        size_t n = (size_t)(argc - 3) / 2;
        struct conf_change *changes = calloc(n, sizeof *changes);
        if (!changes) return 1;
        for (size_t i = 0; i < n; i++) {
            changes[i].name = argv[3 + i * 2];
            changes[i].value = strcmp(argv[4 + i * 2], "<unset>") ? argv[4 + i * 2] : NULL;
        }
        int result = conf_update(path, changes, n);
        free(changes);
        return result < 0;
    }
    if (!strcmp(op, "cas") && argc == 5) {
        int result = conf_compare_exchange(path, !strcmp(argv[3], "<absent>") ? NULL : argv[3],
                                           !strcmp(argv[3], "<absent>") ? 0 : strlen(argv[3]),
                                           argv[4], strlen(argv[4]));
        if (result < 0 && errno == ESTALE) return 3;
        return result < 0;
    }
    return 2;
}
