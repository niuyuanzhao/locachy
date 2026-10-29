/* Actual suspend loop with clock/power syscalls doubled; no real sleep. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int fixture_open(const char *, int, ...);
static ssize_t fixture_write(int, const void *, size_t);
static int fixture_close(int);
static DIR *fixture_opendir(const char *);
static int fixture_clock_gettime(clockid_t, struct timespec *);
static int fixture_klogctl(int, char *, int);
static void fixture_sync(void);
static pid_t fixture_fork(void);
static int fixture_mkdir(const char *, mode_t);
static char root[1024], rtc_root[1200];
#define RTC_CLASS rtc_root
#define open fixture_open
#define write fixture_write
#define close fixture_close
#define opendir fixture_opendir
#define clock_gettime fixture_clock_gettime
#define klogctl fixture_klogctl
#define sync fixture_sync
#define fork fixture_fork
#define mkdir fixture_mkdir
#define main init_main
#include "../init/init.c"
#undef main
#undef mkdir
#undef fork
#undef sync
#undef klogctl
#undef clock_gettime
#undef opendir
#undef close
#undef write
#undef open

static time_t wall;
static int sleeps, disk_writes, sleep_seconds, open_on_sleep, plugged_on_sleep, failed_disk;
static char log_text[8192];
void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(log_text);
    vsnprintf(log_text + n, sizeof log_text - n, fmt, ap);
    va_end(ap);
}
void screen_flush(void) {}
static void lid_state(int shut)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/lid/LID0/state", root);
    FILE *f = fopen(path, "w");
    assert(f && fputs(shut ? "state: closed\n" : "state: open\n", f) >= 0 && fclose(f) == 0);
}
static void external_state(void)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/drm/card0-HDMI-A-1/status", root);
    FILE *f = fopen(path, "w");
    assert(f && fputs("connected\n", f) >= 0 && fclose(f) == 0);
}
static int fixture_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/sys/power/state")) return 10001;
    if (!strcmp(path, "/sys/power/mem_sleep")) return 10002;
    char mapped[1400];
    const char *suffix = NULL;
    if (!strncmp(path, "/proc/acpi/button/lid/", 22)) suffix = path + 22;
    if (suffix) {
        snprintf(mapped, sizeof mapped, "%s/lid/%s", root, suffix);
        return open(mapped, flags);
    }
    if (!strncmp(path, "/sys/class/drm/", 15)) {
        snprintf(mapped, sizeof mapped, "%s/drm/%s", root, path + 15);
        return open(mapped, flags);
    }
    if (!strncmp(path, root, strlen(root))) return open(path, flags);
    errno = ENOENT;
    return -1;
}
static ssize_t fixture_write(int fd, const void *bytes, size_t size)
{
    if (fd == 10002) return (ssize_t)size;
    if (fd != 10001) return write(fd, bytes, size);
    if (size == 3 && !memcmp(bytes, "mem", 3)) {
        sleeps++;
        assert(sleeps <= 3);
        wall += sleep_seconds;
        if (open_on_sleep) lid_state(0);
        if (plugged_on_sleep) external_state();
        if (failed_disk && sleeps > 1) lid_state(0);
    } else {
        assert(size == 4 && !memcmp(bytes, "disk", 4));
        disk_writes++;
        if (failed_disk) { errno = EIO; return -1; }
        lid_state(0); /* resumed from the image after opening the lid */
    }
    return (ssize_t)size;
}
static int fixture_close(int fd) { return fd >= 10001 ? 0 : close(fd); }
static DIR *fixture_opendir(const char *path)
{
    char mapped[1400];
    if (!strcmp(path, "/proc/acpi/button/lid")) snprintf(mapped, sizeof mapped, "%s/lid", root);
    else if (!strcmp(path, "/sys/class/drm")) snprintf(mapped, sizeof mapped, "%s/drm", root);
    else if (!strncmp(path, root, strlen(root))) return opendir(path);
    else { errno = ENOENT; return NULL; }
    return opendir(mapped);
}
static int fixture_clock_gettime(clockid_t id, struct timespec *out)
{ (void)id;out->tv_sec = wall;out->tv_nsec = 0;return 0; }
static int fixture_klogctl(int action, char *buf, int size) { (void)action;(void)buf;(void)size;return -1; }
static void fixture_sync(void) {}
static pid_t fixture_fork(void) { return -1; }
static int fixture_mkdir(const char *path, mode_t mode) { (void)path;(void)mode;errno=EACCES;return -1; }
int monitor_internal(const char *name) { return !strncmp(name, "eDP", 3); }
static void directory(const char *suffix)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", root, suffix);
    assert(mkdir(path, 0700) == 0);
}
static void reset_case(void)
{
    sleeps = disk_writes = open_on_sleep = plugged_on_sleep = failed_disk = 0;
    sleep_seconds = 30;
    wall = 1000;
    hibernate_after = 30;
    lid_wakes_from_disk = 1;
    snprintf(resume_dev, sizeof resume_dev, "8:3");
    snprintf(swap_dev, sizeof swap_dev, "/dev/mapper/swap");
    log_text[0] = '\0';
    lid_state(1);
    char path[1400];
    snprintf(path, sizeof path, "%s/drm/card0-HDMI-A-1/status", root);
    unlink(path);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(root, sizeof root, "%s", argv[1]);
    assert(mkdir(root, 0700) == 0);
    directory("lid");directory("lid/LID0");directory("drm");directory("drm/card0-HDMI-A-1");directory("rtc");directory("rtc/rtc4");
    snprintf(rtc_root, sizeof rtc_root, "%s/rtc", root);
    char alarm[1400];
    snprintf(alarm, sizeof alarm, "%s/rtc4/wakealarm", rtc_root);
    int fd = open(alarm, O_CREAT | O_RDWR, 0600);
    assert(fd >= 0);close(fd);
    reset_case();
    assert(suspend(1) == 30 && sleeps == 1 && disk_writes == 1);
    assert(strstr(log_text, "hibernating") && strstr(log_text, "rtc4/wakealarm"));
    reset_case();open_on_sleep = 1;sleep_seconds = 5;
    assert(suspend(1) == 5 && sleeps == 1 && disk_writes == 0);
    reset_case();plugged_on_sleep = 1;
    assert(suspend(1) == 30 && sleeps == 1 && disk_writes == 0);
    reset_case();resume_dev[0] = '\0';open_on_sleep = 1;
    assert(suspend(1) == 30 && disk_writes == 0 && strstr(log_text,"no resume device"));
    reset_case();failed_disk = 1;
    assert(suspend(1) >= 30 && disk_writes >= 1 && strstr(log_text,"cannot hibernate"));
    reset_case();open_on_sleep = 1;
    assert(unlink(alarm) == 0);
    assert(suspend(1) == 30 && disk_writes == 0 && sleeps == 1);
    assert(strstr(log_text,"timed hibernation unavailable; sleeping until woken"));
    char path[1400];
    snprintf(path, sizeof path, "%s/lid/LID0/state", root); assert(unlink(path) == 0);
    snprintf(path, sizeof path, "%s/lid/LID0", root); assert(rmdir(path) == 0);
    snprintf(path, sizeof path, "%s/lid", root); assert(rmdir(path) == 0);
    snprintf(path, sizeof path, "%s/drm/card0-HDMI-A-1", root); assert(rmdir(path) == 0);
    snprintf(path, sizeof path, "%s/drm", root); assert(rmdir(path) == 0);
    snprintf(path, sizeof path, "%s/rtc/rtc4", root); assert(rmdir(path) == 0);
    snprintf(path, sizeof path, "%s/rtc", root); assert(rmdir(path) == 0);
    assert(rmdir(root) == 0);
    puts("PASS actual sleep loop: timed hibernate, early lid wake, docked wake, missing resume, "
         "missing alarm keeps sleep, image-write failure");
    return 0;
}
