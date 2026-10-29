/* Real RTC selection/writes with a private sysfs fixture, no power changes. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char rtc_root[1024];
#define RTC_CLASS rtc_root
#define main init_main
#include "../init/init.c"
#undef main
static char log_text[2048];
void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(log_text);
    vsnprintf(log_text + n, sizeof log_text - n, fmt, ap);
    va_end(ap);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(rtc_root, sizeof rtc_root, "%s", argv[1]);
    assert(mkdir(rtc_root, 0700) == 0);
    char dir[1200], wake[1280], selected[320] = "";
    snprintf(dir, sizeof dir, "%s/rtc0", rtc_root);
    assert(mkdir(dir, 0700) == 0); /* present, but no alarm */
    snprintf(dir, sizeof dir, "%s/rtc7", rtc_root);
    assert(mkdir(dir, 0700) == 0);
    snprintf(wake, sizeof wake, "%s/wakealarm", dir);
    int fd = open(wake, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(rtc_alarm_set(selected, sizeof selected, "+30") == 0);
    assert(strcmp(selected, wake) == 0 && strstr(log_text, "suspend-to-hibernate"));
    char text[32] = "";
    assert(pread(fd, text, sizeof text - 1, 0) == 3 && !strcmp(text, "+30"));
    assert(rtc_alarm_clear(selected) == 0);
    memset(text, 0, sizeof text);
    assert(pread(fd, text, sizeof text - 1, 0) == 3 && text[0] == '0');
    close(fd);
    assert(unlink(wake) == 0);
    assert(rtc_alarm_set(selected, sizeof selected, "+20") == -1 && !selected[0]);
    assert(strstr(log_text, "no usable RTC wake alarm"));
    assert(rmdir(dir) == 0);
    snprintf(dir, sizeof dir, "%s/rtc0", rtc_root);
    assert(rmdir(dir) == 0 && rmdir(rtc_root) == 0);
    puts("PASS wake alarm selects an available RTC, clears it and reports missing/failed alarms");
    return 0;
}
