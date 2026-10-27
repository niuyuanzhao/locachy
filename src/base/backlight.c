/* Screen backlight control: standard Linux /sys/class/backlight first, or a
 * declarative USB hidraw rule (backlight_hidraw="VID:PID:READ:WRITE:MAX:SCALE")
 * when provided by the active machine profile in /usr/local/share/locachy/machines.
 * Generic PCs without a machine profile only use /sys/class/backlight. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base.h"

#define SAVED_BRIGHTNESS "/var/lib/locachy/brightness"

struct hidraw_bl_spec {
    int valid;
    unsigned int vid, pid, read_reg, write_reg, max_lvl, scale;
};

static void on_bl_rc(const char *name, const char *value, void *arg)
{
    struct hidraw_bl_spec *sp = arg;
    if (strcmp(name, "backlight_hidraw") == 0) {
        if (!value[0] || strcmp(value, "off") == 0) {
            sp->valid = 0;
            return;
        }
        unsigned int vid = 0, pid = 0, rreg = 0, wreg = 0, mx = 0, sc = 0;
        if (sscanf(value, "%x:%x:%u:%u:%u:%u", &vid, &pid, &rreg, &wreg, &mx, &sc) == 6 &&
            mx > 0 && mx <= 255 && sc > 0 && mx * sc <= 255) {
            *sp = (struct hidraw_bl_spec){ 1, vid, pid, rreg, wreg, mx, sc };
        }
    }
}

static int find_sysfs_backlight(char *dir_out, size_t size, int *max_out)
{
    DIR *d = opendir("/sys/class/backlight");
    struct dirent *e;
    int best_max = 0;
    dir_out[0] = '\0';
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char mpath[320], buf[64];
        snprintf(mpath, sizeof mpath, "/sys/class/backlight/%.255s/max_brightness", e->d_name);
        int fd = open(mpath, O_RDONLY | O_CLOEXEC);
        ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
        if (fd >= 0) close(fd);
        if (n > 0) {
            buf[n] = '\0';
            int mv = atoi(buf);
            if (mv > best_max) {
                best_max = mv;
                snprintf(dir_out, size, "/sys/class/backlight/%.255s", e->d_name);
            }
        }
    }
    if (d) closedir(d);
    if (max_out) *max_out = best_max;
    return best_max > 0 ? 0 : -1;
}

static int open_profile_hidraw(struct hidraw_bl_spec *sp_out)
{
    struct hidraw_bl_spec sp = { 0 };
    rc_read(on_bl_rc, &sp);
    if (!sp.valid) return -1;
    DIR *d = opendir("/sys/class/hidraw");
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "hidraw", 6) != 0) continue;
        char dev[96];
        snprintf(dev, sizeof dev, "/dev/%.64s", e->d_name);
        int fd = open(dev, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct hidraw_devinfo info = { 0 };
        if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0 &&
            (uint16_t)info.vendor == (uint16_t)sp.vid &&
            (uint16_t)info.product == (uint16_t)sp.pid) {
            closedir(d);
            if (sp_out) *sp_out = sp;
            return fd;
        }
        close(fd);
    }
    if (d) closedir(d);
    return -1;
}

int base_brightness_get(int *cur, int *max_val)
{
    char dir[320], bpath[352], buf[64];
    int mv = 0;
    if (find_sysfs_backlight(dir, sizeof dir, &mv) == 0) {
        snprintf(bpath, sizeof bpath, "%s/brightness", dir);
        int fd = open(bpath, O_RDONLY | O_CLOEXEC);
        ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
        if (fd >= 0) close(fd);
        if (n > 0) {
            buf[n] = '\0';
            int cv = atoi(buf);
            if (cv < 0) cv = 0;
            if (cv > mv) cv = mv;
            if (cur) *cur = cv;
            if (max_val) *max_val = mv;
            return 0;
        }
    }

    struct hidraw_bl_spec sp;
    int hfd = open_profile_hidraw(&sp);
    if (hfd >= 0) {
        uint8_t rep[2] = { (uint8_t)sp.read_reg, 0 };
        int r = ioctl(hfd, HIDIOCGFEATURE(2), rep);
        close(hfd);
        if (r >= 0) {
            int lvl = rep[1] > sp.max_lvl ? (int)sp.max_lvl : (int)rep[1];
            if (cur) *cur = lvl;
            if (max_val) *max_val = (int)sp.max_lvl;
            return 0;
        }
    }
    return -1;
}

static void save_brightness_pct(int pct)
{
    mkdir("/var/lib/locachy", 0755);
    FILE *f = fopen(SAVED_BRIGHTNESS ".tmp", "w");
    if (!f) return;
    if (fprintf(f, "%d\n", pct) > 0 && fclose(f) == 0) {
        rename(SAVED_BRIGHTNESS ".tmp", SAVED_BRIGHTNESS);
    } else {
        fclose(f);
        unlink(SAVED_BRIGHTNESS ".tmp");
    }
}

int base_brightness_set(int target, int max_ref)
{
    char dir[320], bpath[352], out[32];
    int mv = 0;
    if (find_sysfs_backlight(dir, sizeof dir, &mv) == 0) {
        int v = max_ref > 0 && max_ref != mv ? (int)((long long)target * mv / max_ref) : target;
        if (v < 1 && mv > 1) v = 1;
        if (v > mv) v = mv;
        snprintf(bpath, sizeof bpath, "%s/brightness", dir);
        int fd = open(bpath, O_WRONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        int len = snprintf(out, sizeof out, "%d\n", v);
        ssize_t w = write(fd, out, (size_t)len);
        close(fd);
        if (w <= 0) return -1;
        save_brightness_pct((v * 100 + mv / 2) / mv);
        return 0;
    }

    struct hidraw_bl_spec sp;
    int hfd = open_profile_hidraw(&sp);
    if (hfd >= 0) {
        int m = (int)sp.max_lvl;
        int lvl = max_ref > 0 && max_ref != m ? (target * m + max_ref / 2) / max_ref : target;
        if (lvl < 0) lvl = 0;
        if (lvl > m) lvl = m;
        uint8_t cmd[2] = { (uint8_t)sp.write_reg, (uint8_t)(lvl * (int)sp.scale) };
        ssize_t w = write(hfd, cmd, sizeof cmd);
        close(hfd);
        if (w != sizeof cmd) return -1;
        save_brightness_pct((lvl * 100 + m / 2) / m);
        return 0;
    }
    return -1;
}

int base_brightness_restore(void)
{
    FILE *f = fopen(SAVED_BRIGHTNESS, "r");
    if (!f) return -1;
    int pct = -1;
    if (fscanf(f, "%d", &pct) != 1) pct = -1;
    fclose(f);
    if (pct < 0 || pct > 100) return -1;
    return base_brightness_set(pct, 100);
}

int base_brightness_key(int action_key, char *toast, size_t size)
{
    int cur = 0, mv = 0;
    if (action_key != K_BRI_UP && action_key != K_BRI_DOWN) return -1;
    if (base_brightness_get(&cur, &mv) < 0 || mv <= 0) return -1;
    int step = mv >= 20 ? (mv + 10) / 20 : 1;
    int next = action_key == K_BRI_UP ? cur + step : cur - step;
    int floor = mv <= 16 ? 0 : 1;
    if (next < floor) next = floor;
    if (next > mv) next = mv;
    if (base_brightness_set(next, mv) < 0) return -1;
    if (toast && size > 0) {
        int pct = (next * 100 + mv / 2) / mv;
        snprintf(toast, size, _(" [Brightness %d%%] "), pct);
    }
    return 0;
}
