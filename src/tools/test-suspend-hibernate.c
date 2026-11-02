/* The actual suspend loop (init.c's suspend) with the clock, sleep, wake
 * alarm and lid syscalls doubled; no real sleep, no device. The boot-time
 * clock moves on while "asleep", the monotonic one does not; the wake alarm
 * is a timerfd double that is readable once the boot-time clock reaches it;
 * the lid is an EVIOCGSW double; batteries and connectors are files in a
 * scratch directory (test-suspend-hibernate DIR). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
static int fixture_open(const char *, int, ...);
static ssize_t fixture_write(int, const void *, size_t);
static ssize_t fixture_read(int, void *, size_t);
static int fixture_close(int);
static int fixture_ioctl(int, unsigned long, ...);
static DIR *fixture_opendir(const char *);
static int fixture_clock_gettime(clockid_t, struct timespec *);
static int fixture_clock_getres(clockid_t, struct timespec *);
static int fixture_timerfd_create(int, int);
static int fixture_timerfd_settime(int, int, const struct itimerspec *, struct itimerspec *);
static int fixture_klogctl(int, char *, int);
static void fixture_sync(void);
static pid_t fixture_fork(void);
static int fixture_mkdir(const char *, mode_t);
static int fixture_kill(pid_t, int);
static int fixture_reboot(int);
static int fixture_swapoff(const char *);
static int fixture_mount(const char *, const char *, const char *, unsigned long, const void *);
static char root[1024], supply_root[1200];
#define POWER_SUPPLY_CLASS supply_root
#define open fixture_open
#define write fixture_write
#define read fixture_read
#define close fixture_close
#define ioctl fixture_ioctl
#define opendir fixture_opendir
#define clock_gettime fixture_clock_gettime
#define clock_getres fixture_clock_getres
#define timerfd_create fixture_timerfd_create
#define timerfd_settime fixture_timerfd_settime
#define klogctl fixture_klogctl
#define sync fixture_sync
#define fork fixture_fork
#define mkdir fixture_mkdir
#define kill fixture_kill
#define reboot fixture_reboot
#define swapoff fixture_swapoff
#define mount fixture_mount
#define main init_main
#include "../init/init.c"
#undef main
#undef mount
#undef swapoff
#undef reboot
#undef kill
#undef mkdir
#undef fork
#undef sync
#undef klogctl
#undef timerfd_settime
#undef timerfd_create
#undef clock_getres
#undef clock_gettime
#undef opendir
#undef ioctl
#undef close
#undef read
#undef write
#undef open

#define STATE_FD 10001
#define MEM_SLEEP_FD 10002
#define TIMER_FD 10010
#define LID_FD 10020

static double boot_s, mono_s;            /* the doubled clocks */
static int lid_shut;                     /* the lid switch's level */
static int sleeps, disk_writes, poweroffs, timers, timer_clock, timer_flags, timer_armed;
static double timer_at, timer_value;     /* boot-time expiry; its relative value */
static int no_rtc, no_timerfd, failed_disk, refuse_errno, refuse_from, docked_on_wake;
static double sleep_for[8];              /* seconds asleep per write "mem" (or until the alarm) */
static int open_on_sleep[8];             /* the lid opens during that sleep */
static double wake_early;                /* the RTC wakes this much before the alarm (whole seconds) */
static int battery_on_wake;              /* the trip point woke it: the battery at this percent */
static int disk_lid_wakes;               /* the closed lid wakes it from the first image (the X230) */
static char mem_sleep_written[16];
static char log_text[16384];

void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(log_text);
    vsnprintf(log_text + n, sizeof log_text - n, fmt, ap);
    va_end(ap);
    n = strlen(log_text);
    if (n + 1 < sizeof log_text) {
        log_text[n] = '\n';
        log_text[n + 1] = '\0';
    }
}
void screen_flush(void) {}
int monitor_internal(const char *name) { return !strncmp(name, "eDP", 3); }

static void file_text(const char *suffix, const char *text)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", root, suffix);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}
static void directory(const char *suffix)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", root, suffix);
    assert(mkdir(path, 0700) == 0);
}
static void external(int connected)
{
    file_text("drm/card0-HDMI-A-1/status", connected ? "connected\n" : "disconnected\n");
}
static void battery(const char *status, int percent)
{
    char text[32];
    file_text("power/BAT0/status", status);
    snprintf(text, sizeof text, "%d\n", percent * 500000);
    file_text("power/BAT0/energy_now", text);
    snprintf(text, sizeof text, "%d\n", percent);
    file_text("power/BAT0/capacity", text);
    file_text("power/AC/online", strcmp(status, "Discharging") == 0 ? "0\n" : "1\n");
}

static int fixture_open(const char *path, int flags, ...)
{
    char mapped[1400];
    if (!strcmp(path, "/sys/power/state")) return STATE_FD;
    if (!strcmp(path, "/sys/power/mem_sleep")) return MEM_SLEEP_FD;
    if (!strncmp(path, "/sys/power/suspend_stats/", 25)) {
        snprintf(mapped, sizeof mapped, "%s/stats/%s", root, path + 25);
        return open(mapped, flags);
    }
    if (!strncmp(path, "/sys/class/drm/", 15)) {
        snprintf(mapped, sizeof mapped, "%s/drm/%s", root, path + 15);
        return open(mapped, flags);
    }
    if (!strncmp(path, root, strlen(root))) return open(path, flags & ~O_CREAT);
    errno = ENOENT;
    return -1;
}
static ssize_t fixture_write(int fd, const void *bytes, size_t size)
{
    if (fd == MEM_SLEEP_FD) {
        snprintf(mem_sleep_written, sizeof mem_sleep_written, "%.*s", (int)size, (const char *)bytes);
        return (ssize_t)size;
    }
    if (fd != STATE_FD) return write(fd, bytes, size);
    if (size == 3 && !memcmp(bytes, "mem", 3)) {
        int i = sleeps++;
        assert(sleeps <= 6);
        if (refuse_errno && i >= refuse_from) {
            boot_s += sleep_for[i]; /* the time the attempt took */
            mono_s += sleep_for[i];
            errno = refuse_errno;
            return -1;
        }
        double asleep = sleep_for[i];
        if (timer_armed && boot_s + asleep >= timer_at - wake_early) {
            asleep = timer_at - wake_early - boot_s; /* the RTC woke it */
        }
        boot_s += asleep;
        if (open_on_sleep[i]) lid_shut = 0;
        if (docked_on_wake) external(1);
        if (battery_on_wake) battery("Discharging", battery_on_wake);
        return (ssize_t)size;
    }
    assert(size == 4 && !memcmp(bytes, "disk", 4));
    disk_writes++;
    if (failed_disk) {
        errno = EIO;
        return -1;
    }
    boot_s += 100;
    if (disk_lid_wakes && disk_writes == 1) return (ssize_t)size; /* back, the lid still closed */
    lid_shut = 0; /* back from the image after opening the lid */
    return (ssize_t)size;
}
static ssize_t fixture_read(int fd, void *buf, size_t size)
{
    if (fd != TIMER_FD) return read(fd, buf, size);
    assert(size == sizeof(uint64_t));
    if (!timer_armed || boot_s < timer_at) {
        errno = EAGAIN;
        return -1;
    }
    uint64_t ticks = 1;
    memcpy(buf, &ticks, sizeof ticks);
    return (ssize_t)sizeof ticks;
}
static int fixture_close(int fd)
{
    if (fd == TIMER_FD) {
        assert(timer_armed);
        timer_armed = 0; /* timerfd_release cancels it */
        return 0;
    }
    return fd >= STATE_FD ? 0 : close(fd);
}
static int fixture_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    unsigned long *bits = va_arg(ap, unsigned long *);
    va_end(ap);
    assert(fd == LID_FD && _IOC_TYPE(request) == 'E' && _IOC_NR(request) == 0x1b); /* EVIOCGSW */
    memset(bits, 0, _IOC_SIZE(request));
    if (lid_shut) bits[SW_LID / (8 * sizeof *bits)] |= 1ul << (SW_LID % (8 * sizeof *bits));
    return (int)_IOC_SIZE(request);
}
static DIR *fixture_opendir(const char *path)
{
    char mapped[1400];
    if (!strcmp(path, "/sys/class/drm")) snprintf(mapped, sizeof mapped, "%s/drm", root);
    else if (!strncmp(path, root, strlen(root))) return opendir(path);
    else { errno = ENOENT; return NULL; }
    return opendir(mapped);
}
static int fixture_clock_gettime(clockid_t id, struct timespec *out)
{
    double t = id == CLOCK_MONOTONIC ? mono_s : boot_s;
    out->tv_sec = (time_t)t;
    out->tv_nsec = (long)((t - (double)out->tv_sec) * 1e9);
    return 0;
}
static int fixture_clock_getres(clockid_t id, struct timespec *out)
{
    assert(id == CLOCK_BOOTTIME_ALARM);
    if (no_rtc) { errno = EINVAL; return -1; }
    out->tv_sec = 0;
    out->tv_nsec = 1;
    return 0;
}
static int fixture_timerfd_create(int clockid, int flags)
{
    if (no_timerfd) { errno = EPERM; return -1; }
    assert(!timer_armed);
    timers++;
    timer_clock = clockid;
    timer_flags = flags;
    return TIMER_FD;
}
static int fixture_timerfd_settime(int fd, int flags, const struct itimerspec *value, struct itimerspec *old)
{
    assert(fd == TIMER_FD && flags == 0 && old == NULL);
    assert(value->it_interval.tv_sec == 0 && value->it_interval.tv_nsec == 0);
    timer_value = (double)value->it_value.tv_sec + (double)value->it_value.tv_nsec / 1e9;
    assert(timer_value > 0);
    timer_at = boot_s + timer_value;
    timer_armed = 1;
    return 0;
}
static int fixture_klogctl(int action, char *buf, int size) { (void)action; (void)buf; (void)size; return -1; }
static void fixture_sync(void) {}
static pid_t fixture_fork(void) { return -1; }
static int fixture_mkdir(const char *path, mode_t mode) { (void)path; (void)mode; errno = EACCES; return -1; }
static int fixture_kill(pid_t pid, int sig) { (void)pid; (void)sig; return 0; }
static int fixture_reboot(int how) { assert(how == RB_POWER_OFF); poweroffs++; return 0; }
static int fixture_swapoff(const char *path) { (void)path; errno = EINVAL; return -1; }
static int fixture_mount(const char *a, const char *b, const char *c, unsigned long d, const void *e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }

static void reset_case(void)
{
    sleeps = disk_writes = poweroffs = timers = timer_armed = 0;
    no_rtc = no_timerfd = failed_disk = refuse_errno = refuse_from = docked_on_wake = 0;
    battery_on_wake = disk_lid_wakes = 0;
    for (int i = 0; i < 8; i++) {
        sleep_for[i] = 1000; /* until the alarm, or long */
        open_on_sleep[i] = 0;
    }
    wake_early = 0;
    boot_s = 5000;
    mono_s = 4000;
    lid_shut = 1;
    hibernate_after = 30;
    hibernate_at = -1;
    lid_holdoff = 30;
    battery_critical = 5;
    lid_wakes_from_disk = 1;
    mem_sleep[0] = '\0';
    mem_sleep_written[0] = '\0';
    snprintf(resume_dev, sizeof resume_dev, "8:3");
    snprintf(swap_dev, sizeof swap_dev, "/dev/mapper/swap");
    log_text[0] = '\0';
    external(0);
    battery("Discharging", 60);
}
static int logged(const char *text) { return strstr(log_text, text) != NULL; }
static int near(double a, double b) { return a - b < 0.01 && b - a < 0.01; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(root, sizeof root, "%s", argv[1]);
    assert(mkdir(root, 0700) == 0);
    directory("drm"); directory("drm/card0-HDMI-A-1"); directory("stats");
    directory("power"); directory("power/BAT0"); directory("power/AC");
    snprintf(supply_root, sizeof supply_root, "%s/power", root);
    file_text("power/BAT0/type", "Battery\n");
    file_text("power/BAT0/energy_full", "50000000\n");
    file_text("power/BAT0/power_now", "8000000\n");
    file_text("power/BAT0/alarm", "2500000\n");
    file_text("power/AC/type", "Mains\n");
    file_text("stats/last_failed_dev", "usb3-port1\n");
    file_text("stats/last_failed_errno", "-16\n");
    file_text("stats/last_failed_step", "suspend\n");
    nlid = 1;
    lid_fds[0] = LID_FD;

    reset_case();
    assert(near(suspend(1), 130) && sleeps == 1 && disk_writes == 1 && timers == 1);
    assert(timer_clock == CLOCK_BOOTTIME_ALARM && timer_flags == (TFD_NONBLOCK | TFD_CLOEXEC));
    assert(near(timer_value, 30) && !timer_armed && hibernate_at < 0 && !mem_sleep_written[0]);
    assert(logged("the wake alarm with the lid closed: hibernating") && logged("hibernating to /dev/mapper/swap"));
    puts("PASS the wake alarm (CLOCK_BOOTTIME_ALARM timerfd, hibernate_after ahead) went off: hibernated; "
         "mem_sleep left to the kernel");

    reset_case(); sleep_for[0] = 5; open_on_sleep[0] = 1; snprintf(mem_sleep, sizeof mem_sleep, "deep");
    assert(near(suspend(1), 5) && sleeps == 1 && disk_writes == 0 && hibernate_at < 0 && !timer_armed);
    assert(!strcmp(mem_sleep_written, "deep"));
    puts("PASS the lid opened: awake, no hibernation, the deadline dropped; mem_sleep=deep written when set");

    reset_case(); sleep_for[0] = 10;
    assert(near(suspend(1), 10) && sleeps == 1 && disk_writes == 0 && near(hibernate_at, 5030));
    assert(logged("looking again in 30 s") && near(lid_holdoff_until, 4000 * 1e3 + 30 * 1e3));
    puts("PASS woken early with the lid closed: up, locked, the deadline kept, looked at again after the holdoff");
    double kept = hibernate_at;
    sleep_for[0] = 1000;
    sleeps = 0; disk_writes = 0; timers = 0;
    assert(suspend(1) > 0 && sleeps == 1 && disk_writes == 1 && near(timer_value, kept - 5010));
    puts("PASS the next sleep keeps the deadline: the alarm armed for what is left of it (20 s)");

    reset_case(); lid_shut = 0; /* closed for a moment: open again by the time it locked and synced */
    assert(suspend(1) >= 0 && sleeps == 0 && disk_writes == 0 && timers == 0 && hibernate_at < 0);
    assert(logged("the lid is open again: not sleeping"));
    puts("PASS the lid open again before the write (a bounce shorter than the lock and sync): no sleep");

    reset_case(); lid_holdoff = 0; sleep_for[0] = 10; docked_on_wake = 1;
    assert(near(suspend(1), 10) && sleeps == 1 && disk_writes == 0 && hibernate_at < 0);
    assert(logged("another screen connected: staying up"));
    reset_case(); lid_holdoff = 0; sleep_for[0] = 10;
    assert(near(suspend(1), 130) && sleeps == 2 && disk_writes == 1 && near(timer_value, 20));
    assert(logged("back to sleep"));
    puts("PASS lid_holdoff 0: docked on the wake it stays up, otherwise straight back to sleep");

    reset_case(); wake_early = 0.6;
    assert(suspend(1) > 29 && sleeps == 1 && disk_writes == 1 && logged("the hibernation deadline"));
    puts("PASS the RTC wakes up to a second early (whole seconds): under ALARM_MIN_S left, it hibernates");

    reset_case(); refuse_errno = EBUSY; sleep_for[0] = 29;
    assert(suspend(1) > 29 && sleeps == 1 && disk_writes == 1 && logged("will not suspend"));
    reset_case(); hibernate_at = boot_s + 1.5;
    assert(suspend(1) >= 0 && sleeps == 0 && disk_writes == 1);
    puts("PASS EBUSY under 2 s from the deadline (alarmtimer_suspend), or under 2 s left: hibernates at once");

    reset_case(); refuse_errno = EIO; sleep_for[0] = 0.5;
    assert(suspend(1) == -1 && sleeps == 1 && disk_writes == 0 && near(hibernate_at, 5030) && !timer_armed);
    assert(logged("cannot sleep: Input/output error") && logged("device \"usb3-port1\", errno -16, step \"suspend\""));
    puts("PASS the kernel refuses: -1, the deadline kept, suspend_stats' last device, errno and step logged");

    reset_case(); resume_dev[0] = '\0'; sleep_for[0] = 1000;
    assert(near(suspend(1), 1000) && timers == 0 && disk_writes == 0 && logged("no resume device"));
    reset_case(); no_rtc = 1; sleep_for[0] = 1000;
    assert(near(suspend(1), 1000) && timers == 0 && disk_writes == 0 && logged("no RTC can wake the machine"));
    assert(logged("sleeping until woken"));
    reset_case(); no_timerfd = 1; sleep_for[0] = 1000;
    assert(near(suspend(1), 1000) && disk_writes == 0 && logged("no wake alarm: Operation not permitted"));
    puts("PASS no resume device, no wake-capable RTC, no alarm timer: it sleeps until woken");

    reset_case(); failed_disk = 1; sleep_for[1] = 5; open_on_sleep[1] = 1;
    assert(suspend(1) > 30 && sleeps == 2 && disk_writes == 1 && logged("cannot hibernate"));
    assert(timers == 1); /* the second sleep without an alarm, as systemd-sleep after a failed hibernation */
    puts("PASS the image cannot be written: asleep again until woken, without the alarm");

    reset_case(); battery("Discharging", 4);
    assert(suspend(1) >= 0 && sleeps == 0 && disk_writes == 1 && logged("battery critical with the lid closed"));
    reset_case(); battery("Discharging", 4); resume_dev[0] = '\0';
    suspend(1);
    assert(sleeps == 0 && disk_writes == 0 && poweroffs == 1 && logged("no resume device: powering off"));
    reset_case(); battery("Charging", 4);
    assert(near(suspend(1), 130) && sleeps == 1 && disk_writes == 1 && logged("the wake alarm"));
    reset_case(); sleep_for[0] = 10; battery_on_wake = 3; /* the trip point woke it (acpi_battery_update) */
    assert(near(suspend(1), 110) && sleeps == 1 && disk_writes == 1);
    assert(logged("battery critical with the lid closed: hibernating"));
    puts("PASS on battery at or below battery_critical with the lid closed: hibernates without sleeping, "
         "powers off without a resume device; charging, the timer as usual");

    reset_case(); disk_lid_wakes = 1; /* the X230's firmware: woken from disk for the closed lid */
    assert(suspend(1) > 30 && sleeps == 1 && disk_writes == 2 && lid_wakes_from_disk == 0);
    assert(logged("the closed lid woke the machine from disk; again without lid wakeup"));
    puts("PASS the closed lid wakes it from disk: hibernated again without lid wakeup, and so from then on");

    reset_case(); sleep_for[0] = 20;
    assert(suspend(0) >= 0 && timers == 0 && disk_writes == 0 && hibernate_at < 0);
    puts("PASS a sleep not from the lid (locachy.sleeptest): no deadline, no alarm of its own");

    char path[1400];
    const char *files[] = { "drm/card0-HDMI-A-1/status", "stats/last_failed_dev", "stats/last_failed_errno",
                            "stats/last_failed_step", "power/BAT0/type", "power/BAT0/energy_full",
                            "power/BAT0/power_now", "power/BAT0/alarm", "power/BAT0/status",
                            "power/BAT0/energy_now", "power/BAT0/capacity", "power/AC/online", "power/AC/type" };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(path, sizeof path, "%s/%s", root, files[i]);
        assert(unlink(path) == 0);
    }
    const char *dirs[] = { "drm/card0-HDMI-A-1", "drm", "stats", "power/BAT0", "power/AC", "power" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, sizeof path, "%s/%s", root, dirs[i]);
        assert(rmdir(path) == 0);
    }
    assert(rmdir(root) == 0);
    puts("PASS actual sleep loop: alarm-timer hibernation, lid, holdoff, refusal, critical battery");
    return 0;
}
