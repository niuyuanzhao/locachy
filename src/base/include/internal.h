/* Declarations shared inside the base library and with process 1 (the
 * switcher, init/init.c), which is part of the base; programs see only
 * base.h. The one copy: the base, process 1 and seat must agree on it. */
#ifndef BASE_INTERNAL_H
#define BASE_INTERNAL_H

#include "base.h"
#include <sys/stat.h>

/* Device nodes can reuse a path (and device number) after disconnecting.
 * A still-open inode identifies the old device, including across SCM_RIGHTS. */
static inline int input_same(int a, int b)
{
    struct stat x, y;
    return fstat(a, &x) == 0 && fstat(b, &y) == 0 &&
           x.st_dev == y.st_dev && x.st_ino == y.st_ino && x.st_rdev == y.st_rdev;
}

/*
 * The control channel between process 1 and a program it started: a Unix
 * SOCK_SEQPACKET socket, at the fd number in $LOCACHY_CONTROL in the program.
 * A message is fields separated by NUL bytes, the first naming it.
 *   process 1 -> program
 *     take                 with fds: a DRM lease, then keyboards; the
 *                          program owns them until "release"
 *     keys                 with fds: current keyboard leases, including zero;
 *                          keep unchanged queues, no new screen activation
 *     pointer-remove PATH  remove a disconnected libinput device
 *     pointer-retry        retry leases not yet tagged by udev
 *     pointer PATH         with an fd: an input device for libinput (a
 *                          pointer, or a keyboard to pair with a touchpad),
 *                          after "pointer?" and on hotplug; one per device
 *     release              give them up now
 *     returned NAME STATUS the program it called has exited
 *     back NAME            (home) the user came back from task NAME
 *     ended NAME STATUS    (home) the last program of task NAME exited
 *     tasks LINE...        (home) the answer to "tasks?"
 *   program -> process 1
 *     released             done: the fds are closed
 *     pointer?             (the program on the screen) its input devices
 *     call ARG...          run ARG... and come back here when it exits
 *     open ARG...          (home) show the task ARG..., starting it if need be
 *     tasks?               (home) which tasks there are
 *     home                 (the program on the screen) show home, as Ctrl+Alt+Esc
 *     poweroff             power off
 * Process 1 revokes the screen and keyboards after "release" whether or not
 * the answer comes, so a program that hangs cannot keep them.
 */
#define CONTROL_MAX_FDS 9
#define CONTROL_MAX 16384
int control_fd(void); /* -1 if this program was not started by process 1 */
int control_send(int sock, const char *msg, size_t len, const int *fds, int nfds);
/* Send fields (up to a NULL) as one message. */
int control_send_fields(int sock, const char *const *fields);
/* Receive one message into msg (NUL-terminated as well); its fds into fds.
 * Returns the message length, 0 when the other side has gone, -1 on error. */
int control_recv(int sock, char *msg, size_t size, int *fds, int *nfds);
/* Split a message into its fields; returns how many (at most max). */
int control_fields(char *msg, int len, char **fields, int max);
/* Program side: handle one message from process 1. "take" and "release"
 * are dealt with here; any other message is copied to other (if given),
 * with its length in *len. Returns 0 if process 1 has gone, 1 if the
 * message was dealt with here, 2 if it was copied. */
int control_service(char *other, size_t size, int *len);
/* Program side: wait until the screen and keyboards have been given; and
 * whether they were given since the last call (key_get turns that into
 * K_SHOWN). */
void control_wait_take(void);
int control_shown(void);

/* The screen, program side: show the shadow buffer on this lease, or stop
 * showing it. */
int screen_take(int lease);
void screen_release(void);

/* The screen, process 1 side: lend the output to a program as a DRM lease
 * (returns the lease fd, -1 on error), and take it back (revoke the lease,
 * show our own buffer again). */
int screen_lend(uint32_t *lessee);
int screen_drop_master(uint32_t lessee);
int screen_take_back(uint32_t lessee);
/* Rescan only while master and no active lease: 1 usable, 0 no output,
 * -1 error. The logical canvas dimensions remain unchanged. */
int screen_refresh(void);

/* The keyboards. Program side: use these fds, or close them all. Process 1
 * side: open a second set of fds for a program; revoke and close that set
 * (EVIOCREVOKE also cuts off the program's copies, which share the open
 * file). */
void keys_take(const int *fds, int n);
void keys_update(const int *fds, int n);
void keys_release(void);
void keys_refresh(void); /* process 1: rescan, preserving live queues */
int keys_sync(int *fds, int *n, int max); /* reconcile leases; nonzero if changed */
int keys_lend(int *fds, int max);
void keys_take_back(const int *fds, int n);

/* Process 1: load the drivers for the devices there are, from the modules
 * under root/lib/modules/VERSION (modload.c); then keep doing so for devices
 * that appear: poll modules_fd() and call modules_event() when it is
 * readable. Processed udev events also wake this fd for input rescans. */
int modules_start(const char *root);
int modules_fd(void);
int modules_event(void); /* Nonzero if a DRM topology event was received. */

/* Process 1: the keyboards' own fds, to poll them with other things, and the
 * key for one evdev event (0 if none). Ctrl+Alt+Esc gives K_HOME. */
#define K_HOME 0x1FE
int keys_fds(int *fds, int max);
int keys_event(int fd);

/* The pointer. Process 1: open every pointer and keyboard device again for
 * the program on the screen, with their paths (at most max), and revoke and
 * close them. Program side: take one device by the path libinput opens it
 * by, give them all up, ask for them again after a take; poll pointer_fd()
 * and call pointer_dispatch() when it is readable, then pointer_next() moves
 * each event into pointer_last(). */
#define POINTER_PATH 32
int *pointer_lend(size_t *count, char paths[][POINTER_PATH], size_t max);
void pointer_take_back(int *fds, size_t count);
int pointer_take(int fd, const char *path);
void pointer_remove(const char *path);
void pointer_retry(void); /* retry leases not yet tagged by udev */
void pointer_release(void);
void pointer_resume(void);
int pointer_fd(void);
void pointer_dispatch(void);
int pointer_next(void);

int pointer_open(void); /* keys_open: ask process 1 for the devices */

/* The cursor, drawn by the screen where the pointer is (pixels), and the
 * scale the picture is drawn at (1 until known). */
void screen_cursor_move(int x, int y);
void screen_cursor_visible(int on);
double screen_scale(void);

#endif
