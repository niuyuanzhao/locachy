/* Output ordering must be independent of DRM and Wayland discovery order. */
#include <assert.h>
#include <stdio.h>
#include "monitor-layout.h"

static int permutations;

static void permute(struct monitor_rect *r, int first, int n)
{
    if (first == n) {
        struct monitor_rect copy[5];
        memcpy(copy, r, (size_t)n * sizeof *r);
        size_t a = monitor_layout(copy, (size_t)n);
        assert(strcmp(copy[a].name, "eDP-1") == 0);
        for (int i = 0; i < n; i++) {
            if (strcmp(copy[i].name, "eDP-1") == 0) assert(copy[i].x == 400 && copy[i].y == 300);
            if (strcmp(copy[i].name, "DP-1") == 0) assert(copy[i].x == 1424 && copy[i].y == 300);
            if (strcmp(copy[i].name, "DP-2") == 0) assert(copy[i].x == 2155 && copy[i].y == 300);
            if (strcmp(copy[i].name, "HDMI-A-1") == 0) assert(copy[i].x == 0 && copy[i].y == 300);
            if (strcmp(copy[i].name, "VGA-1") == 0) assert(copy[i].x == 400 && copy[i].y == 0);
        }
        permutations++;
        return;
    }
    for (int i = first; i < n; i++) {
        struct monitor_rect tmp = r[first]; r[first] = r[i]; r[i] = tmp;
        permute(r, first + 1, n);
        tmp = r[first]; r[first] = r[i]; r[i] = tmp;
    }
}

int main(void)
{
    assert(monitor_layout(NULL, 0) == 0);
    struct monitor_rect r[] = {
        { .name = "eDP-1", .width = 1024, .height = 576, .place = MONITOR_ORIGIN },
        { .name = "DP-1", .width = 731, .height = 411, .place = MONITOR_RIGHT },
        { .name = "DP-2", .width = 1280, .height = 720, .place = MONITOR_RIGHT },
        { .name = "HDMI-A-1", .width = 400, .height = 600, .place = MONITOR_LEFT },
        { .name = "VGA-1", .width = 800, .height = 300, .place = MONITOR_ABOVE },
    };
    permute(r, 0, 5);
    struct monitor_rect missing[] = {
        { .name = "B", .width = 100, .height = 200, .place = MONITOR_LEFT },
        { .name = "A", .width = 300, .height = 400, .place = MONITOR_RIGHT },
    };
    assert(monitor_layout(missing, 2) == 1);
    assert(missing[0].x == 0 && missing[1].x == 100);
    missing[0].place = missing[1].place = MONITOR_ORIGIN;
    assert(monitor_layout(missing, 2) == 1);
    assert(missing[0].x == 300 && missing[1].x == 0);
    missing[0].place = MONITOR_BELOW;
    monitor_layout(missing, 2);
    assert(missing[0].x == 0 && missing[0].y == 400);
    printf("monitor layout: %d discovery permutations, empty, fallback, duplicate origin and below passed.\n", permutations);
    return 0;
}
