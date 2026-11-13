/* Host-owned application lifecycle, independent of the shared compositor. */
#ifndef LOCACHY_APPS_H
#define LOCACHY_APPS_H

#include <stddef.h>
#include <sys/types.h>
#include <poll.h>

enum {
    APPS_DISPLAY_STOPPED = 0,
    APPS_DISPLAY_FROZEN  = 1,
    APPS_DISPLAY_ACTIVE  = 2,
};

/* Dedicated policy owner channels, bound by process 1 to a frontend PID and
 * an open cgroup directory. The returned descriptor belongs to the child.
 * Weak: tests that link process 1 without apps.c leave them out. */
#define APPS_SESSION_POLL_MAX 128
int apps_session_create(const char *cgroup, int *child);
void apps_session_bind(int owner, pid_t pid);
void apps_session_close(int owner);
__attribute__((weak)) void apps_sessions_revoke(const char *cgroup); /* NULL: every owner */
__attribute__((weak)) void apps_sessions_locked(int locked);
__attribute__((weak)) int apps_sessions_poll(struct pollfd *p, int max);
__attribute__((weak)) void apps_sessions_dispatch(const struct pollfd *p, int n);
__attribute__((weak)) int apps_sleep_inhibited(void);
/* The cgroup's owner holds a live link (base_session_hold): not frozen. */
__attribute__((weak)) int apps_session_holding(const char *cgroup);

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
int apps_open_mode(const char *name, const char *target, int own_tor, char *why, size_t size);
/* Open a whitelisted host tool as a managed Wayland window on the shared
 * display: view, files, settings, mixer, bluetooth, network, term. For an already running settings
 * window, updates its page via its request channel without spawning a second
 * instance. */
int apps_window_open(const char *program, const char *target, char *why, size_t size);

#endif
