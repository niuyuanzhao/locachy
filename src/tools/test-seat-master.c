/* seat's DRM master handshake and failure handling, without DRM/input.
 * Socket readiness uses a private pipe; ioctls and clock are test doubles. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdarg.h>
static int fake_ioctl(int fd, unsigned long request, ...);
#define ioctl fake_ioctl
#define main seat_main
#include "../progs/seat.c"
#undef main
#undef ioctl
#include <assert.h>

static int acquire_fail, drop_fail, drops, acquires, reply_ok;
static double ticks;
void say(const char *fmt, ...) { (void)fmt; }
double uptime(void) { ticks += 0.02; return ticks; }
int control_send(int fd, const char *msg, size_t len, const int *fds, int n)
{ (void)fd; (void)fds; assert(n == 0 && len == 6 && strcmp(msg, "master") == 0); return 0; }
int control_recv(int fd, char *msg, size_t size, int *fds, int *n)
{ (void)fd; (void)fds; *n = 0; snprintf(msg, size, "%s", reply_ok ? "mastered" : "master-error"); return (int)strlen(msg); }
static int fake_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    if (request == DRM_IOCTL_SET_MASTER) {
        acquires++;
        if (acquire_fail) { errno = EBUSY; return -1; }
        return 0;
    }
    assert(request == DRM_IOCTL_DROP_MASTER);
    drops++;
    if (drop_fail) { errno = EBUSY; return -1; }
    return 0;
}

int main(void)
{
    int p[2];
    assert(pipe(p) == 0 && write(p[1], "x", 1) == 1);
    reply_ok = 0;
    assert(request_master(p[0]) == -1);
    reply_ok = 1;
    assert(request_master(p[0]) == 0);
    assert(request_master(-1) == 0);
    close(p[0]); close(p[1]);
    puts("PASS only an explicit mastered reply enables compositor takeover");
    ndevs = 2;
    devs[0].is_drm = devs[1].is_drm = 1;
    snprintf(devs[0].path, sizeof devs[0].path, "fake-card0");
    snprintf(devs[1].path, sizeof devs[1].path, "fake-card1");
    assert(acquire_drm_masters() == 0 && acquires == 2);
    acquire_fail = 1;
    assert(acquire_drm_masters() == -1 && drops == 2);
    acquire_fail = 0;
    drop_fail = 1;
    ticks = 0;
    assert(drop_all_devices() == -1 && ticks < 0.2);
    drop_fail = 0;
    assert(drop_all_devices() == 0);
    puts("PASS acquire failure returns partial ownership; release has one bounded deadline");
    puts("ALL PASSED");
    return 0;
}
