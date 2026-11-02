/* The wake alarm (init.c's wake_alarm, wake_alarm_fired): a timerfd on the
 * kernel's alarm timer, which sets the RTC itself at suspend; the timerfd
 * syscalls are doubled, so no RTC, privilege or sleep is involved. The sysfs
 * wakealarm it replaced is never written. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
static int fixture_open(const char *, int, ...);
static ssize_t fixture_read(int, void *, size_t);
static int fixture_close(int);
static int fixture_clock_getres(clockid_t, struct timespec *);
static int fixture_timerfd_create(int, int);
static int fixture_timerfd_settime(int, int, const struct itimerspec *, struct itimerspec *);
#define open fixture_open
#define read fixture_read
#define close fixture_close
#define clock_getres fixture_clock_getres
#define timerfd_create fixture_timerfd_create
#define timerfd_settime fixture_timerfd_settime
#define main init_main
#include "../init/init.c"
#undef main
#undef timerfd_settime
#undef timerfd_create
#undef clock_getres
#undef close
#undef read
#undef open

#define TIMER_FD 20001
static int getres_errno, create_errno, settime_errno, created, closed, expired, opens;
static int clock_id, create_flags;
static struct itimerspec armed;
static char log_text[2048];

void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(log_text);
    vsnprintf(log_text + n, sizeof log_text - n, fmt, ap);
    va_end(ap);
}
static int fixture_open(const char *path, int flags, ...)
{
    if (strstr(path, "rtc") || strstr(path, "wakealarm")) {
        opens++; /* the alarm opens no file: no /sys/class/rtc/rtcN/wakealarm */
        errno = EACCES;
        return -1;
    }
    return open(path, flags);
}
static int fixture_clock_getres(clockid_t id, struct timespec *res)
{
    assert(id == CLOCK_BOOTTIME_ALARM);
    if (getres_errno) { errno = getres_errno; return -1; }
    res->tv_sec = 0;
    res->tv_nsec = 1;
    return 0;
}
static int fixture_timerfd_create(int id, int flags)
{
    clock_id = id;
    create_flags = flags;
    if (create_errno) { errno = create_errno; return -1; }
    created++;
    return TIMER_FD;
}
static int fixture_timerfd_settime(int fd, int flags, const struct itimerspec *value, struct itimerspec *old)
{
    assert(fd == TIMER_FD && flags == 0 && old == NULL); /* relative: from now on CLOCK_BOOTTIME */
    if (settime_errno) { errno = settime_errno; return -1; }
    armed = *value;
    return 0;
}
static ssize_t fixture_read(int fd, void *buf, size_t size)
{
    if (fd != TIMER_FD) return read(fd, buf, size);
    assert(size == sizeof(uint64_t));
    if (!expired) { errno = EAGAIN; return -1; } /* TFD_NONBLOCK, not yet */
    uint64_t ticks = 1;
    memcpy(buf, &ticks, sizeof ticks);
    return (ssize_t)sizeof ticks;
}
static int fixture_close(int fd)
{
    if (fd != TIMER_FD) return close(fd);
    closed++;
    return 0;
}

int main(void)
{
    int fd = wake_alarm(29.25);
    assert(fd == TIMER_FD && clock_id == CLOCK_BOOTTIME_ALARM && create_flags == (TFD_NONBLOCK | TFD_CLOEXEC));
    assert(armed.it_value.tv_sec == 29 && armed.it_value.tv_nsec == 250000000);
    assert(armed.it_interval.tv_sec == 0 && armed.it_interval.tv_nsec == 0); /* one-shot */
    assert(!wake_alarm_fired(fd));
    expired = 1;
    assert(wake_alarm_fired(fd) && !wake_alarm_fired(-1));
    puts("PASS wake alarm: CLOCK_BOOTTIME_ALARM timerfd, non-blocking, one-shot, relative; fired = readable");

    getres_errno = EINVAL; /* alarm_clock_getres: no RTC whose parent may wake the machine */
    created = 0;
    assert(wake_alarm(30) == -1 && created == 0 && strstr(log_text, "no RTC can wake the machine"));
    getres_errno = 0;
    create_errno = EPERM; /* without CAP_WAKE_ALARM */
    assert(wake_alarm(30) == -1 && strstr(log_text, "no wake alarm: Operation not permitted"));
    create_errno = 0;
    settime_errno = EINVAL;
    closed = 0;
    assert(wake_alarm(30) == -1 && closed == 1 && strstr(log_text, "no wake alarm: Invalid argument"));
    assert(opens == 0);
    puts("PASS wake alarm unavailable (no wake-capable RTC, no CAP_WAKE_ALARM, refused): -1, said, nothing left "
         "open, no sysfs wakealarm");
    return 0;
}
