/*
 * drives (/usr/local/lib/locachy/drives, service_drives): USB drives and
 * memory cards for the user. Every 2 s it looks at the block devices: each
 * filesystem on a USB disk or an SD card is mounted at ~/Drives/LABEL (the
 * user, uid 1000, owns the files of FAT, exFAT and NTFS ones), and a drive
 * that has been pulled out is unmounted. ~/Drives is a shared mount point:
 * containers have it with slave propagation, so drives come and go there too.
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
#include <unistd.h>

#include "base.h"

#define USER 1000
#define MAX_DRIVES 32

struct drive {
    char dev[32];       /* sdb1, mmcblk0p1 */
    char dir[512];      /* where it is mounted; "" if it is not */
    int seen, ejected;  /* ejected: unmounted by someone else; left alone */
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

static int mounted(const char *dir)
{
    struct stat a, b;
    return stat(dir, &a) == 0 && stat(top, &b) == 0 && a.st_dev != b.st_dev;
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
    for (int i = 2; (mkdir(d->dir, 0755) < 0 && (errno != EEXIST || mounted(d->dir) ||
                                                  rmdir(d->dir) < 0 || mkdir(d->dir, 0755) < 0)); i++) {
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
    if (d->dir[0] && !mounted(d->dir)) {
        say("drives: %s was unmounted (ejected); not mounted again until it is plugged in again", d->dev);
        rmdir(d->dir);
        d->dir[0] = '\0';
        d->ejected = 1;
    }
    if (!d->dir[0] && !d->ejected) {
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
        if (d->dir[0]) {
            say("drives: %s was pulled out; unmounting %s", d->dev, d->dir);
            umount2(d->dir, MNT_DETACH);
            rmdir(d->dir);
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
    /* Left from before: folders of drives (the system stopped with them in),
     * drives mounted by a drives that did not end cleanly. */
    DIR *old = opendir(top);
    struct dirent *e;
    while (old && (e = readdir(old)) != NULL) {
        char path[600];
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", top, e->d_name);
        if (mounted(path)) {
            umount2(path, MNT_DETACH);
        }
        rmdir(path);
    }
    if (old) closedir(old);
    /* A mount point of its own, shared: what is mounted under it appears in
     * the containers that have it (slave). It stays when this ends, so that
     * running containers keep seeing it after a restart of the service. */
    if (mount(NULL, top, NULL, MS_SHARED, NULL) < 0 &&
        (mount(top, top, NULL, MS_BIND, NULL) < 0 || mount(NULL, top, NULL, MS_SHARED, NULL) < 0)) {
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
        if (drives[i].dir[0]) {
            umount2(drives[i].dir, MNT_DETACH);
            rmdir(drives[i].dir);
        }
    }
    return 0;
}
