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
/* The shared display has said display-ready: programs start on it from now
 * on, and on a later session's display once that one says it too. */
void apps_display_ready(void);
int apps_reap(pid_t pid, int status);
int apps_busy(void);
int apps_display_state(void);
void apps_list(char *buf, size_t size);
int apps_action(const char *action, const char *name, char *why, size_t size);
int apps_open(const char *name, const char *target, char *why, size_t size);
/* Open a whitelisted host tool as a managed Wayland window on the shared
 * display: view, files, settings, mixer. For an already running settings
 * window, updates its page via its request channel without spawning a second
 * instance. */
int apps_window_open(const char *program, const char *target, char *why, size_t size);

#endif
