/*
 * A keyboard made with uinput, for tests: prints its device number
 * (MAJOR:MINOR) once it exists, then reads lines from stdin:
 *   CODE          press and release the evdev key CODE
 *   +CODE, -CODE  press, release
 *   sleep MS
 */
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int fd;

static void emit(int type, int code, int value)
{
    struct input_event ev = { .type = type, .code = code, .value = value };
    if (write(fd, &ev, sizeof ev) != sizeof ev) {
        perror("test-uinput: write");
        exit(1);
    }
}

static void key(int code, int value)
{
    emit(EV_KEY, code, value);
    emit(EV_SYN, SYN_REPORT, 0);
    usleep(20000);
}

int main(void)
{
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("test-uinput: /dev/uinput");
        return 1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int code = 1; code < 256; code++) {
        ioctl(fd, UI_SET_KEYBIT, code);
    }
    struct uinput_setup setup = { .id = { .bustype = BUS_VIRTUAL, .vendor = 1, .product = 1 } };
    snprintf(setup.name, sizeof setup.name, "locachy test keyboard");
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("test-uinput: create");
        return 1;
    }
    char sysname[64], path[128];
    struct stat st;
    if (ioctl(fd, UI_GET_SYSNAME(sizeof sysname), sysname) < 0) {
        perror("test-uinput: sysname");
        return 1;
    }
    /* The event node appears under the input device a moment later. */
    for (int tries = 0; tries < 100; tries++) {
        for (int i = 0; i < 64; i++) {
            snprintf(path, sizeof path, "/sys/devices/virtual/input/%s/event%d", sysname, i);
            if (stat(path, &st) == 0) {
                snprintf(path, sizeof path, "/dev/input/event%d", i);
                if (stat(path, &st) == 0) {
                    printf("%u:%u\n", major(st.st_rdev), minor(st.st_rdev));
                    fflush(stdout);
                    goto ready;
                }
            }
        }
        usleep(20000);
    }
    fprintf(stderr, "test-uinput: no event node\n");
    return 1;
ready:;
    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        if (strncmp(line, "sleep ", 6) == 0) {
            usleep((useconds_t)atoi(line + 6) * 1000);
        } else if (line[0] == '+') {
            key(atoi(line + 1), 1);
        } else if (line[0] == '-') {
            key(atoi(line + 1), 0);
        } else if (atoi(line) > 0) {
            key(atoi(line), 1);
            key(atoi(line), 0);
        }
    }
    ioctl(fd, UI_DEV_DESTROY);
    return 0;
}
