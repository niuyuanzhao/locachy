/*
 * drives (/usr/local/lib/locachy/drives, service_drives): USB drives and
 * memory cards for the user. Every 2 s it looks at the block devices: each
 * filesystem on a USB disk or an SD card is mounted at ~/Drives/LABEL (the
 * user, uid 1000, owns the files of FAT, exFAT and NTFS ones), and a drive
 * that has been pulled out is unmounted. ~/Drives is a shared mount point:
 * containers have it with slave propagation, so drives come and go there too.
 * Startup adopts a live mount of the same removable device; it never tears
 * down another mount below Drives, and it removes only folders this service
 * created itself.
 *
 * Ejecting is the file manager's (files.c, e): it unmounts the drive and
 * powers it off; a drive unmounted that way is not mounted again until it is
 * plugged in again.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "base.h"

#define USER 1000
#define MAX_DRIVES 32

struct drive {
    char dev[32];       /* sdb1, mmcblk0p1 */
    char dir[512];      /* where it is mounted; "" if it is not */
    int seen, ejected, owned; /* adopted mounts stay when this service stops */
    dev_t device;            /* block identity, retained after unplug */
};

static struct drive drives[MAX_DRIVES];
static int ndrives;
static char top[256]; /* ~/Drives */

static int read_line(const char *path, char *out, size_t size)
{
    FILE *in = fopen(path, "re");
    int ok = in && fgets(out, (int)size, in);
    if (in) fclose(in);
    if (ok) out[strcspn(out, "\n")] = '\0';
    return ok ? 0 : -1;
}

/* A disk of a USB device or a memory card, not the system's own. */
static int removable_disk(const char *disk)
{
    char path[300], link[512], value[8];
    if (strncmp(disk, "mmcblk", 6) == 0) {
        return strstr(disk, "boot") == NULL && strstr(disk, "rpmb") == NULL;
    }
    if (strncmp(disk, "sd", 2) != 0) {
        return 0;
    }
    snprintf(path, sizeof path, "/sys/block/%s", disk);
    ssize_t n = readlink(path, link, sizeof link - 1);
    link[n > 0 ? n : 0] = '\0';
    snprintf(path, sizeof path, "/sys/block/%s/removable", disk);
    return strstr(link, "/usb") != NULL || (read_line(path, value, sizeof value) == 0 && value[0] == '1');
}

/* TYPE and LABEL of the filesystem on /dev/dev (blkid); -1 if there is none. */
static int probe(const char *dev, char *type, size_t tsize, char *label, size_t lsize)
{
    char cmd[128], line[512];
    snprintf(cmd, sizeof cmd, "blkid -p -o export /dev/%s 2>/dev/null", dev);
    FILE *in = popen(cmd, "re");
    type[0] = label[0] = '\0';
    while (in && fgets(line, sizeof line, in)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "TYPE=", 5) == 0) {
            snprintf(type, tsize, "%.31s", line + 5);
        } else if (strncmp(line, "LABEL=", 6) == 0) {
            snprintf(label, lsize, "%.127s", line + 6);
        }
    }
    if (in) pclose(in);
    return type[0] ? 0 : -1;
}

static void mount_unescape(char *s)
{
    char *to = s;
    while (*s) {
        if (*s == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' && s[2] <= '7' &&
            s[3] >= '0' && s[3] <= '7') {
            *to++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + s[3] - '0');
            s += 4;
        } else {
            *to++ = *s++;
        }
    }
    *to = '\0';
}

/* Match the exact mount, including same-filesystem bind mounts. Never stat
 * a network mount to find out whether it is mounted: that can block. */
static int mounted(const char *dir)
{
    char *line = NULL;
    size_t size = 0;
    int found = 0;
    FILE *in = fopen("/proc/self/mountinfo", "re");
    while (in && getline(&line, &size, in) >= 0) {
        char path[4096];
        if (sscanf(line, "%*s %*s %*s %*s %4095s", path) != 1) continue;
        mount_unescape(path);
        if (strcmp(path, dir) == 0) found = 1;
    }
    free(line);
    if (in) fclose(in);
    return found;
}

static int mounted_device(const struct drive *d)
{
    char source[64], *line = NULL;
    size_t size = 0;
    int match = 0;
    snprintf(source, sizeof source, "/dev/%s", d->dev);
    if (!d->device) return 0;
    FILE *in = fopen("/proc/self/mountinfo", "re");
    while (in && getline(&line, &size, in) >= 0) {
        char path[4096], from[4096];
        unsigned int maj, min;
        const char *dash = strstr(line, " - ");
        if (!dash || sscanf(line, "%*s %*s %u:%u %*s %4095s", &maj, &min, path) != 3 ||
            sscanf(dash + 3, "%*s %4095s", from) != 1) continue;
        mount_unescape(path);
        mount_unescape(from);
        if (strcmp(path, d->dir) == 0) {
            match = strcmp(from, source) == 0 && maj == major(d->device) && min == minor(d->device);
        }
    }
    free(line);
    if (in) fclose(in);
    return match;
}

/* Reuse an existing mount of this actual removable block device. No mount
 * below Drives is torn down at startup, and an unrelated mount is not ours. */
static int adopt_mount(struct drive *d)
{
    char source[64], *line = NULL;
    struct stat st;
    size_t size = 0, n = strlen(top);
    snprintf(source, sizeof source, "/dev/%s", d->dev);
    if (stat(source, &st) < 0 || !S_ISBLK(st.st_mode)) return 0;
    FILE *in = fopen("/proc/self/mountinfo", "re");
    while (in && getline(&line, &size, in) >= 0) {
        char path[4096], from[4096], root[4096], fs[64];
        unsigned int maj, min;
        const char *dash = strstr(line, " - ");
        if (!dash || sscanf(line, "%*s %*s %u:%u %4095s %4095s", &maj, &min, root, path) != 4 ||
            sscanf(dash + 3, "%63s %4095s", fs, from) != 2 ||
            maj != major(st.st_rdev) || min != minor(st.st_rdev)) continue;
        mount_unescape(path);
        mount_unescape(from);
        if (strcmp(from, source) != 0 || strcmp(root, "/") != 0 || strncmp(path, top, n) != 0 ||
            path[n] != '/' || !path[n + 1] || strchr(path + n + 1, '/') || strlen(path) >= sizeof d->dir) continue;
        memcpy(d->dir, path, strlen(path) + 1);
        d->device = st.st_rdev;
        d->owned = 0;
    }
    free(line);
    if (in) fclose(in);
    if (d->dir[0] && !mounted_device(d)) d->dir[0] = '\0';
    if (d->dir[0]) say("drives: keeping %s at %s", source, d->dir);
    return d->dir[0] != '\0';
}

static void mount_drive(struct drive *d)
{
    /* File systems without owners or modes: the user's, folders 0755 and
     * files 0644, so that what is copied off them is not executable (on
     * FAT, as udisks2 mounts it: showexec, only .exe, .com and .bat). */
    static const struct { const char *type, *fs, *options; } kinds[] = {
        { "vfat", "vfat", "uid=1000,gid=1000,umask=022,showexec,utf8,shortname=mixed,flush" },
        { "exfat", "exfat", "uid=1000,gid=1000,dmask=022,fmask=133" },
        { "ntfs", "ntfs3", "uid=1000,gid=1000,dmask=022,fmask=133" },
        { "ext4", "ext4", "" }, { "ext3", "ext4", "" }, { "ext2", "ext4", "" },
        { "btrfs", "btrfs", "" }, { "xfs", "xfs", "" }, { "f2fs", "f2fs", "" },
        { "iso9660", "iso9660", "" }, { "udf", "udf", "" },
    };
    char type[32], label[128], name[160], src[64];
    if (probe(d->dev, type, sizeof type, label, sizeof label) < 0) {
        return; /* no filesystem (yet) */
    }
    int k = 0, n = (int)(sizeof kinds / sizeof kinds[0]);
    while (k < n && strcmp(kinds[k].type, type) != 0) {
        k++;
    }
    if (k == n) {
        say("drives: %s: %s is not a filesystem mounted here", d->dev, type);
        d->ejected = 1;
        return;
    }
    snprintf(name, sizeof name, "%s", label[0] ? label : d->dev);
    for (char *p = name; *p; p++) {
        if (*p == '/' || (unsigned char)*p < 32) {
            *p = '_';
        }
    }
    snprintf(d->dir, sizeof d->dir, "%s/%s", top, name);
    for (int i = 2; mkdir(d->dir, 0755) < 0; i++) {
        if (i > 9) {
            say("drives: %s: no place for it in %s", d->dev, top);
            d->dir[0] = '\0';
            d->ejected = 1;
            return;
        }
        snprintf(d->dir, sizeof d->dir, "%s/%s (%d)", top, name, i);
    }
    int ro = strcmp(type, "iso9660") == 0 || strcmp(type, "udf") == 0;
    snprintf(src, sizeof src, "/dev/%s", d->dev);
    if (mount(src, d->dir, kinds[k].fs, MS_NOSUID | MS_NODEV | (ro ? MS_RDONLY : 0),
              kinds[k].options) < 0) {
        say("drives: cannot mount %s (%s) at %s: %m", src, type, d->dir);
        rmdir(d->dir);
        d->dir[0] = '\0';
        d->ejected = 1;
        return;
    }
    struct stat st;
    d->device = stat(src, &st) == 0 && S_ISBLK(st.st_mode) ? st.st_rdev : 0;
    d->owned = 1;
    say("drives: %s (%s) at %s", src, type, d->dir);
}

static struct drive *find(const char *dev)
{
    for (int i = 0; i < ndrives; i++) {
        if (strcmp(drives[i].dev, dev) == 0) {
            return &drives[i];
        }
    }
    return NULL;
}

/* A filesystem device: a partition of the disk, or the disk if it has none. */
static void consider(const char *dev)
{
    struct drive *d = find(dev);
    if (!d && ndrives < MAX_DRIVES) {
        d = &drives[ndrives++];
        memset(d, 0, sizeof *d);
        snprintf(d->dev, sizeof d->dev, "%s", dev);
    }
    if (!d) {
        return;
    }
    d->seen = 1;
    if (d->dir[0] && !mounted_device(d)) {
        say("drives: %s was unmounted (ejected); not mounted again until it is plugged in again", d->dev);
        if (d->owned && !mounted(d->dir)) rmdir(d->dir);
        d->dir[0] = '\0';
        d->owned = 0;
        d->ejected = 1;
    }
    if (!d->dir[0] && !d->ejected && !adopt_mount(d)) {
        mount_drive(d);
    }
}

static void scan(void)
{
    DIR *block = opendir("/sys/block");
    struct dirent *e;
    for (int i = 0; i < ndrives; i++) {
        drives[i].seen = 0;
    }
    while (block && (e = readdir(block)) != NULL) {
        if (e->d_name[0] == '.' || !removable_disk(e->d_name)) {
            continue;
        }
        char path[300], size[32];
        snprintf(path, sizeof path, "/sys/block/%s/size", e->d_name);
        if (read_line(path, size, sizeof size) < 0 || atoll(size) == 0) {
            continue; /* a card reader with no card */
        }
        int parts = 0;
        snprintf(path, sizeof path, "/sys/block/%s", e->d_name);
        DIR *disk = opendir(path);
        struct dirent *p;
        while (disk && (p = readdir(disk)) != NULL) {
            char part[600];
            struct stat st;
            snprintf(part, sizeof part, "%s/%s/partition", path, p->d_name);
            if (strncmp(p->d_name, e->d_name, strlen(e->d_name)) == 0 && stat(part, &st) == 0) {
                consider(p->d_name);
                parts++;
            }
        }
        if (disk) closedir(disk);
        if (parts == 0) {
            consider(e->d_name);
        }
    }
    if (block) closedir(block);
    /* Pulled out: unmounted now (lazily: whatever has files open keeps them,
     * with errors), and forgotten. */
    for (int i = 0; i < ndrives;) {
        struct drive *d = &drives[i];
        if (d->seen) {
            i++;
            continue;
        }
        if (d->dir[0] && mounted_device(d)) {
            say("drives: %s was pulled out; unmounting %s", d->dev, d->dir);
            if (umount2(d->dir, MNT_DETACH) == 0 && d->owned) rmdir(d->dir);
        }
        *d = drives[--ndrives];
    }
}

static volatile sig_atomic_t stop;

static void on_term(int sig)
{
    (void)sig;
    stop = 1;
}

int main(void)
{
    struct passwd *pw = getpwuid(USER);
    if (!pw || !pw->pw_dir) {
        say("drives: no user %d", USER);
        return 1;
    }
    snprintf(top, sizeof top, "%s/Drives", pw->pw_dir);
    if (mkdir(top, 0755) == 0 || errno == EEXIST) {
        if (chown(top, USER, USER) < 0) {
            say("drives: cannot give %s to the user: %m", top);
        }
    }
    /* Keep existing mounts and folders. The scan adopts only actual removable
     * block devices; cloud drives and user-owned paths are not ours to clean. */
    /* A mount point of its own, shared: retain existing children when binding
     * it, including a cloud service that started before this one. */
    if ((!mounted(top) && mount(top, top, NULL, MS_BIND | MS_REC, NULL) < 0) ||
        mount(NULL, top, NULL, MS_SHARED, NULL) < 0) {
        say("drives: cannot make %s a shared mount point: %m", top);
        return 1;
    }
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    say("drives: watching for USB drives and memory cards; mounted in %s", top);
    while (!stop) {
        scan();
        sleep(2);
    }
    for (int i = 0; i < ndrives; i++) {
        if (drives[i].owned && drives[i].dir[0] && mounted_device(&drives[i]) &&
            umount2(drives[i].dir, 0) == 0) rmdir(drives[i].dir);
    }
    return 0;
}
