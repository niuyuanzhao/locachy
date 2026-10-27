/* Pointer handover on the real control parser: one FD per packet with the
 * path libinput opens it by, never counted as a keyboard, and all copies
 * closed on release. Synthetic pipe
 * descriptors only; no physical device or process 1 is contacted. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../base/control.c"
#include <assert.h>
#include <errno.h>

static int taken_pointer = -1, keyboard_fd = -1, resumes, released, takes;
void pointer_resume(void) { resumes++; }
int pointer_take(int fd, const char *path)
{ assert(taken_pointer == -1 && strcmp(path, "/dev/input/event5") == 0); taken_pointer = fd; return 0; }
void pointer_release(void) { if (taken_pointer >= 0) close(taken_pointer); taken_pointer = -1; released++; }
void pointer_retry(void) {}
void pointer_remove(const char *path) { (void)path; abort(); }
void keys_update(const int *fds, int count) { (void)fds; (void)count; abort(); }
void keys_take(const int *fds, int count) { assert(count == 1); keyboard_fd = fds[0]; }
void keys_release(void) { close(keyboard_fd); keyboard_fd = -1; }
int screen_take(int fd) { close(fd); takes++; return 1; }
void screen_release(void) { released++; }

int main(void)
{
    int pair[2], data[2], fds[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) == 0);
    assert(pipe(data) == 0);
    ctl = pair[0];
    fds[0] = data[0]; fds[1] = data[1];
    assert(control_send(pair[1], "take", 4, fds, 2) == 0);
    assert(control_service(NULL, 0, NULL) == 1 && takes == 1 && resumes == 1 && keyboard_fd >= 0);
    assert(control_send(pair[1], "pointer\0/dev/input/event5", 25, &data[0], 1) == 0);
    assert(control_service(NULL, 0, NULL) == 1 && taken_pointer >= 0 && taken_pointer != keyboard_fd);
    int old_pointer = taken_pointer, old_keyboard = keyboard_fd;
    assert(control_send(pair[1], "release", 7, NULL, 0) == 0);
    assert(control_service(NULL, 0, NULL) == 1 && released == 2);
    assert(fcntl(old_pointer, F_GETFD) < 0 && errno == EBADF);
    assert(fcntl(old_keyboard, F_GETFD) < 0 && errno == EBADF);
    char message[32]; int n;
    assert(control_recv(pair[1], message, sizeof message, fds, &n) == 8 && n == 0 && strcmp(message, "released") == 0);
    close(data[0]); close(data[1]); close(pair[0]); close(pair[1]);
    puts("PASS screen/keyboard and optional pointer packets remain separate, release closes all frontend input");
    puts("ALL PASSED");
    return 0;
}
