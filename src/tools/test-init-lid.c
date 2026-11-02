/* The lid in process 1 (init.c's lid_changed: level read back, holdoff,
 * docked, refused sleeps), the lock before a sleep (lock_wait's 5 s, and the
 * lid failing closed without the lock program), the critical battery
 * (battery_check, battery_act, the trip point and the fallback look) and the
 * settings that steer them. The clocks, the lid switch (EVIOCGSW), the sleep
 * state file, poll and power-off are doubled; batteries and connectors are
 * files in a scratch directory (test-init-lid DIR). No device, no sleep. */
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
static int fixture_close(int);
static int fixture_ioctl(int, unsigned long, ...);
static int fixture_poll(struct pollfd *, nfds_t, int);
static DIR *fixture_opendir(const char *);
static int fixture_clock_gettime(clockid_t, struct timespec *);
static int fixture_clock_getres(clockid_t, struct timespec *);
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
#define close fixture_close
#define ioctl fixture_ioctl
#define poll fixture_poll
#define opendir fixture_opendir
#define clock_gettime fixture_clock_gettime
#define clock_getres fixture_clock_getres
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
#undef clock_getres
#undef clock_gettime
#undef opendir
#undef poll
#undef ioctl
#undef close
#undef write
#undef open

#define STATE_FD 10001
#define LID_FD 10020

static double boot_s, mono_s;
static int lid_shut, sleeps, disk_writes, poweroffs, refuse_errno, open_on_sleep = 1, polls;
static char log_text[32768];

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
/* What arrange() reaches with no program on any screen. */
void screen_flush(void) {}
int screen_refresh(void) { return 1; }
int screen_places(struct screen_output *out, int max) { (void)out; (void)max; return 0; }
int screen_lend(uint32_t *lessee) { (void)lessee; abort(); }
int screen_lend_outputs(const char *const *names, int n, uint32_t *lessee) { (void)names; (void)n; (void)lessee; abort(); }
int screen_take_back(uint32_t lessee) { (void)lessee; return 0; }
int screen_drop_master(uint32_t lessee) { (void)lessee; return 0; }
void keys_leds(int on) { (void)on; }
int keys_sync(int *fds, int *n, int max) { (void)fds; (void)max; *n = 0; return 0; }
int keys_lend(int *fds, int max) { (void)fds; (void)max; return 0; }
void keys_take_back(const int *fds, int n) { (void)fds; (void)n; }
int *pointer_lend(size_t *n, char (*paths)[POINTER_PATH], size_t max) { (void)paths; (void)max; *n = 0; return NULL; }
void pointer_take_back(int *fds, size_t n) { (void)fds; (void)n; }
int monitor_internal(const char *name) { return !strncmp(name, "eDP", 3); }
int control_send(int sock, const char *msg, size_t len, const int *fds, int nfds)
{ (void)sock; (void)msg; (void)len; (void)fds; (void)nfds; return 0; }
int control_send_fields(int sock, const char *const *fields) { (void)sock; (void)fields; return 0; }

static void file_text(const char *suffix, const char *text)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", root, suffix);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}
static long long file_number(const char *suffix)
{
    char path[1400], text[64] = "";
    snprintf(path, sizeof path, "%s/%s", root, suffix);
    FILE *f = fopen(path, "r");
    assert(f && fgets(text, sizeof text, f) && fclose(f) == 0);
    return atoll(text);
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
    if (fd != STATE_FD) return write(fd, bytes, size);
    if (size == 3 && !memcmp(bytes, "mem", 3)) {
        sleeps++;
        assert(sleeps < 20);
        if (refuse_errno) { errno = refuse_errno; return -1; }
        boot_s += 60;
        if (open_on_sleep) lid_shut = 0;
        return (ssize_t)size;
    }
    assert(size == 4 && !memcmp(bytes, "disk", 4));
    disk_writes++;
    boot_s += 600;
    lid_shut = 0;
    return (ssize_t)size;
}
static int fixture_close(int fd) { return fd >= STATE_FD ? 0 : close(fd); }
static int fixture_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    unsigned long *bits = va_arg(ap, unsigned long *);
    va_end(ap);
    assert(fd == LID_FD && _IOC_TYPE(request) == 'E' && _IOC_NR(request) == 0x1b); /* EVIOCGSW only */
    memset(bits, 0, _IOC_SIZE(request));
    if (lid_shut) bits[SW_LID / (8 * sizeof *bits)] |= 1ul << (SW_LID % (8 * sizeof *bits));
    return (int)_IOC_SIZE(request);
}
/* The lock's socket never answers: the wait runs out (the clock moves on). */
static int fixture_poll(struct pollfd *p, nfds_t n, int timeout)
{
    assert(n == 1 && timeout >= 0 && timeout <= LOCK_WAIT_MS);
    (void)p;
    polls++;
    mono_s += timeout / 1e3;
    return 0;
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
static int fixture_clock_getres(clockid_t id, struct timespec *out) { (void)id; (void)out; errno = EINVAL; return -1; }
static int fixture_klogctl(int action, char *buf, int size) { (void)action; (void)buf; (void)size; return -1; }
static void fixture_sync(void) {}
static pid_t fixture_fork(void) { return -1; }
static int fixture_mkdir(const char *path, mode_t mode) { (void)path; (void)mode; errno = EACCES; return -1; }
static int fixture_kill(pid_t pid, int sig) { (void)pid; (void)sig; return 0; }
static int fixture_reboot(int how) { assert(how == RB_POWER_OFF); poweroffs++; return 0; }
static int fixture_swapoff(const char *path) { (void)path; errno = EINVAL; return -1; }
static int fixture_mount(const char *a, const char *b, const char *c, unsigned long d, const void *e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }

static int logged(const char *text) { return strstr(log_text, text) != NULL; }
static void reset_case(void)
{
    sleeps = disk_writes = poweroffs = refuse_errno = polls = suspend_failures = 0;
    open_on_sleep = 1;
    boot_s = 1000;
    mono_s = 100; /* past the boot holdoff */
    lid_holdoff = 30;
    lid_holdoff_until = 30 * 1e3;
    lid_check_at = hibernate_at = battery_act_at = battery_check_at = -1;
    hibernate_after = 1800;
    battery_critical = 5;
    snprintf(resume_dev, sizeof resume_dev, "8:3");
    locked = 0;
    lock_argv[0] = "/nonexistent/lock";
    log_text[0] = '\0';
    lid_shut = 0;
    external(0);
    battery("Discharging", 60);
    file_text("power/BAT0/alarm", "2500000\n");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(root, sizeof root, "%s", argv[1]);
    assert(mkdir(root, 0700) == 0);
    directory("drm"); directory("drm/card0-HDMI-A-1");
    directory("power"); directory("power/BAT0"); directory("power/AC");
    snprintf(supply_root, sizeof supply_root, "%s/power", root);
    file_text("power/BAT0/type", "Battery\n");
    file_text("power/BAT0/energy_full", "50000000\n");
    file_text("power/BAT0/power_now", "10000000\n");
    file_text("power/AC/type", "Mains\n");
    nlid = 1;
    lid_fds[0] = LID_FD;

    reset_case();
    lid_changed(1); /* the events were 1 then 0: the level is open */
    assert(!locked && sleeps == 0 && lid_check_at < 0);
    puts("PASS a bounce (closed, open again in the queue): the level is open, nothing happens");

    reset_case(); mono_s = 10; lid_shut = 1;
    lid_changed(1);
    assert(locked && sleeps == 0 && lid_check_at == 30 * 1e3);
    assert(logged("SECURITY: /nonexistent/lock is missing") && logged("within the holdoff"));
    puts("PASS closed within the boot holdoff: locked (without the lock program too: it fails closed), "
         "no sleep, looked at again at its end");
    mono_s = 30;
    lid_check_at = -1; /* the main loop, at lid_check_at */
    lid_changed(0);
    assert(sleeps == 1 && lid_check_at < 0 && hibernate_at < 0 && !lid_shut);
    puts("PASS the holdoff over, the lid still closed: it sleeps; woken by opening it, nothing more is due");

    reset_case(); lid_shut = 1; external(1); hibernate_at = boot_s - 100; /* from a sleep before it was docked */
    lid_changed(1);
    assert(locked && sleeps == 0 && logged("another screen connected: not sleeping") && hibernate_at < 0);
    locked = 0; /* unlocked on the other screen */
    lid_changed(0); /* a display change, the screen still there */
    assert(!locked && sleeps == 0);
    external(0);
    lid_changed(0); /* the screen gone */
    assert(locked && sleeps == 1 && logged("the lid is closed, no other screen"));
    assert(disk_writes == 0); /* the old deadline dropped while docked: asleep, not hibernated */
    puts("PASS docked: locked at the closing, up, an old deadline dropped; a display change with it still there "
         "changes nothing; with it gone, locked and asleep");

    reset_case(); lid_shut = 1; refuse_errno = EBUSY;
    lid_changed(1);
    assert(sleeps == 1 && suspend_failures == 1 && disk_writes == 0 && lid_check_at == mono_s * 1e3 + 30e3);
    assert(logged("cannot sleep: Device or resource busy"));
    lid_check_at = -1;
    mono_s += 30;
    lid_changed(0);
    assert(sleeps == 2 && disk_writes == 1 && suspend_failures == 0 && lid_check_at < 0);
    assert(logged("refused to suspend 2 times with the lid closed: hibernating"));
    puts("PASS a refused sleep is looked at again once, at the holdoff's end; refused again: hibernated");

    reset_case(); lid_shut = 1; refuse_errno = EIO; resume_dev[0] = '\0'; lid_holdoff = 0;
    double expect[] = { 2, 4, 8, 16, 32, 32 };
    for (int i = 0; i < 6; i++) {
        lid_check_at = -1;
        lid_changed(i == 0);
        assert(sleeps == i + 1 && disk_writes == 0 && lid_check_at == mono_s * 1e3 + expect[i] * 1e3);
        mono_s += expect[i];
    }
    puts("PASS refused again and again without a resume device: looked at 2, 4, 8, 16, then 32 s apart");
    lid_shut = 0;
    lid_changed(1);
    assert(suspend_failures == 0 && lid_check_at < 0 && logged("the lid is open"));
    puts("PASS the lid opened: nothing due, the count of refusals cleared");

    reset_case(); lid_holdoff = 0; lid_holdoff_until = 0;
    {
        /* The lock program there but silent: 5 s, then on. */
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
        locked = 1;
        lock_shown = 0;
        locker.pid = 4242;
        locker.ctl = pair[0];
        double t0 = mono_s;
        lock_wait();
        assert(mono_s - t0 >= LOCK_WAIT_MS / 1e3 && polls >= 1 && logged("not on the screen after 5.0 s"));
        locker.pid = 0;
        close(pair[0]);
        close(pair[1]);
    }
    puts("PASS before a sleep the lock gets 5 s (logind's InhibitDelayMaxSec), then the sleep goes on, said");

    reset_case(); battery("Discharging", 4);
    battery_check();
    assert(battery_act_at == mono_s * 1e3 + BATTERY_ACTION_MS && logged("battery critical: 4 % left"));
    battery("Charging", 4);
    battery_check();
    assert(battery_act_at < 0 && logged("power is back"));
    battery("Discharging", 4);
    battery_check();
    mono_s += BATTERY_ACTION_MS / 1e3;
    battery_act();
    assert(locked && disk_writes == 1 && poweroffs == 0 && logged("battery critical: hibernating"));
    assert(battery_act_at >= 0); /* back from the image, still critical on battery: due again */
    reset_case(); battery("Discharging", 4); resume_dev[0] = '\0';
    battery_check();
    battery_act();
    assert(disk_writes == 0 && poweroffs == 1 && logged("powers off in 20 s") && logged("powering off"));
    reset_case(); battery("Discharging", 4); file_text("power/AC/online", "1\n");
    battery_check(); /* mains online, every battery discharging (a charger too weak): on battery */
    assert(battery_act_at >= 0);
    reset_case(); battery("Discharging", 4); battery_critical = 0;
    battery_check();
    assert(battery_act_at < 0 && battery_check_at < 0);
    puts("PASS critical battery: warned, 20 s, then hibernated (locked) or powered off; called off when power "
         "comes back; on mains with every battery discharging it counts; battery_critical 0: never");

    reset_case();
    battery_check();
    assert(file_number("power/BAT0/alarm") == 2500000 && battery_check_at < 0);
    battery_critical = 10;
    battery_check();
    assert(file_number("power/BAT0/alarm") == 5000000 && logged("trip point 2500000 -> 5000000"));
    puts("PASS the trip point (_BTP alarm) set to battery_critical of the full charge, only where it differs; "
         "with one, no timer");

    reset_case(); file_text("power/BAT0/alarm", "0\n"); battery("Discharging", 50);
    battery_check(); /* 25 Wh, 2.5 Wh at 5 %, 10 W: 8100 s to go, a look at half of it */
    assert(battery_check_at == mono_s * 1e3 + 4050e3);
    file_text("power/BAT0/power_now", "0\n");
    battery_check();
    assert(battery_check_at == mono_s * 1e3 + 45 * 30e3);
    battery("Discharging", 6);
    battery_check();
    assert(battery_check_at == mono_s * 1e3 + BATTERY_RECHECK_MIN_S * 1e3);
    battery("Charging", 50);
    battery_check();
    assert(battery_check_at < 0);
    file_text("power/BAT0/power_now", "10000000\n");
    puts("PASS no trip point: one look at half the time left (at least 30 s), only while discharging");

    struct settings_read next = { .lid_holdoff = 30, .battery_critical = 5 };
    setting("lid_holdoff", "0", &next);
    setting("battery_critical", "7", &next);
    setting("mem_sleep", "deep", &next);
    assert(next.lid_holdoff == 0 && next.battery_critical == 7 && !strcmp(next.mem_sleep, "deep"));
    setting("lid_holdoff", "-3", &next);
    setting("lid_holdoff", "soon", &next);
    setting("battery_critical", "60", &next);
    setting("mem_sleep", "standby", &next);
    assert(next.lid_holdoff == 0 && next.battery_critical == 7 && !strcmp(next.mem_sleep, "deep"));
    assert(logged("mem_sleep \"standby\" is none of deep, s2idle, shallow"));
    setting("mem_sleep", "", &next); /* rc.conf after the profile */
    assert(!next.mem_sleep[0]);
    puts("PASS settings: lid_holdoff, battery_critical (at most 50), mem_sleep (deep, s2idle, shallow, or the "
         "kernel's); bad values left out");

    char path[1400];
    const char *files[] = { "drm/card0-HDMI-A-1/status", "power/BAT0/type", "power/BAT0/energy_full",
                            "power/BAT0/power_now", "power/BAT0/alarm", "power/BAT0/status",
                            "power/BAT0/energy_now", "power/BAT0/capacity", "power/AC/online", "power/AC/type" };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(path, sizeof path, "%s/%s", root, files[i]);
        assert(unlink(path) == 0);
    }
    const char *dirs[] = { "drm/card0-HDMI-A-1", "drm", "power/BAT0", "power/AC", "power" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, sizeof path, "%s/%s", root, dirs[i]);
        assert(rmdir(path) == 0);
    }
    assert(rmdir(root) == 0);
    puts("PASS the lid, the lock before a sleep and the critical battery in process 1");
    return 0;
}
