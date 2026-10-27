/* Host-owned application lifecycle, independent of the shared compositor. */
#ifndef LOCACHY_APPS_H
#define LOCACHY_APPS_H

#include <stddef.h>
#include <sys/types.h>

enum {
    APPS_DISPLAY_STOPPED = 0,
    APPS_DISPLAY_FROZEN  = 1,
    APPS_DISPLAY_ACTIVE  = 2,
};

void apps_init(void);
void apps_tick(void);
int apps_reap(pid_t pid, int status);
int apps_busy(void);
int apps_display_state(void);
void apps_list(char *buf, size_t size);
int apps_action(const char *action, const char *name, char *why, size_t size);
int apps_open(const char *name, const char *target, char *why, size_t size);

#endif
