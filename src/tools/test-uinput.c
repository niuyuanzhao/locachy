/*
 * Keyboards made with uinput, for tests:
 *   test-uinput [KEYS...]
 * one device for each KEYS argument, with those evdev key codes (FIRST or
 * FIRST-LAST, comma separated: "1-29"); without one, a single device with
 * the codes 1-255. Prints each device's number (MAJOR:MINOR), a line each in
 * the order given, once they all exist, then reads lines from stdin:
 *   CODE          press and release the evdev key CODE
 *   +CODE, -CODE  press, release
 *   sleep MS
 * on the first device that has CODE.
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

#define MAX_DEVICES 4

static int fds[MAX_DEVICES], ndevices;
static unsigned char has[MAX_DEVICES][256];

static void emit(int fd, int type, int code, int value)
{
    struct input_event ev = { .type = type, .code = code, .value = value };
    if (write(fd, &ev, sizeof ev) != sizeof ev) {
        perror("test-uinput: write");
        exit(1);
    }
}

static void key(int code, int value)
{
    for (int d = 0; d < ndevices; d++) {
        if (code > 0 && code < 256 && has[d][code]) {
            emit(fds[d], EV_KEY, code, value);
            emit(fds[d], EV_SYN, SYN_REPORT, 0);
            usleep(20000);
            return;
        }
    }
    fprintf(stderr, "test-uinput: no device has key %d\n", code);
}

/* "1-29,57": the codes into has[d]; 0, or -1 if it is not such a list. */
static int parse_keys(const char *list, unsigned char *keys)
{
    const char *p = list;
    while (*p) {
        char *end;
        long first = strtol(p, &end, 10), last = first;
        if (end == p) return -1;
        if (*end == '-') {
            p = end + 1;
            last = strtol(p, &end, 10);
            if (end == p) return -1;
        }
        if (first < 1 || last > 255 || first > last) return -1;
        for (long c = first; c <= last; c++) keys[c] = 1;
        p = *end == ',' ? end + 1 : end;
        if (*end && *end != ',') return -1;
    }
    return 0;
}

/* Device d with its keys; its event node's number into out. */
static int make(int d, char *out, size_t size)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("test-uinput: /dev/uinput");
        return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int code = 1; code < 256; code++) {
        if (has[d][code]) ioctl(fd, UI_SET_KEYBIT, code);
    }
    struct uinput_setup setup = { .id = { .bustype = BUS_VIRTUAL, .vendor = 1, .product = (unsigned short)(1 + d) } };
    snprintf(setup.name, sizeof setup.name, d ? "locachy test keyboard %d" : "locachy test keyboard", d + 1);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("test-uinput: create");
        return -1;
    }
    fds[d] = fd;
    char sysname[64], path[128];
    struct stat st;
    if (ioctl(fd, UI_GET_SYSNAME(sizeof sysname), sysname) < 0) {
        perror("test-uinput: sysname");
        return -1;
    }
    /* The event node appears under the input device a moment later. */
    for (int tries = 0; tries < 100; tries++) {
        for (int i = 0; i < 64; i++) {
            snprintf(path, sizeof path, "/sys/devices/virtual/input/%s/event%d", sysname, i);
            if (stat(path, &st) == 0) {
                snprintf(path, sizeof path, "/dev/input/event%d", i);
                if (stat(path, &st) == 0) {
                    snprintf(out, size, "%u:%u", major(st.st_rdev), minor(st.st_rdev));
                    return 0;
                }
            }
        }
        usleep(20000);
    }
    fprintf(stderr, "test-uinput: no event node\n");
    return -1;
}

int main(int argc, char **argv)
{
    char numbers[MAX_DEVICES][32];
    ndevices = argc > 1 ? argc - 1 : 1;
    if (ndevices > MAX_DEVICES) {
        fprintf(stderr, "test-uinput: %d devices at most\n", MAX_DEVICES);
        return 2;
    }
    for (int d = 0; d < ndevices; d++) {
        if (argc > 1 && parse_keys(argv[d + 1], has[d]) < 0) {
            fprintf(stderr, "test-uinput: %s is not a list of key codes\n", argv[d + 1]);
            return 2;
        }
        if (argc == 1) memset(has[d] + 1, 1, 255);
        if (make(d, numbers[d], sizeof numbers[d]) < 0) return 1;
    }
    for (int d = 0; d < ndevices; d++) printf("%s\n", numbers[d]);
    fflush(stdout);
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
    for (int d = 0; d < ndevices; d++) ioctl(fds[d], UI_DEV_DESTROY);
    return 0;
}
