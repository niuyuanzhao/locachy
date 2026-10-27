/*
 * The log and the clock. Every line goes into the kernel's log through
 * /dev/kmsg, so lines from process 1, the programs and the kernel come out
 * whole and in order on the console (the serial port in QEMU) and stay
 * readable later (dmesg). The kernel takes printk.devkmsg=on, or it would
 * drop lines past a burst of ten. Before /dev is mounted, or if writing
 * there fails, lines go to stderr.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "base.h"

static int kmsg = -1;

void say(const char *fmt, ...)
{
    char line[512] = "<6>"; /* KERN_INFO */
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(line + 3, sizeof line - 4, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    n += 3;
    if (n > (int)sizeof line - 2) {
        n = (int)sizeof line - 2;
    }
    line[n++] = '\n';
    if (kmsg < 0) {
        kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    }
    if (kmsg >= 0 && write(kmsg, line, (size_t)n) == n) {
        return;
    }
    write(2, line + 3, (size_t)n - 3);
}

double uptime(void)
{
    struct timespec t;

    clock_gettime(CLOCK_BOOTTIME, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
