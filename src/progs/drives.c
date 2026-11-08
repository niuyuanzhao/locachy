/*
 * drives (/usr/local/lib/locachy/drives, service_drives): USB drives, eSATA
 * disks and memory cards for the user. Every 2 s it looks at the block
 * devices: each filesystem on such a drive is mounted at ~/Drives/LABEL (the
 * user, uid 1000, owns the files of FAT, exFAT and NTFS ones), and a drive
 * that has been pulled out, or whose medium was changed, is unmounted.
 * ~/Drives is root's and a shared mount point: containers have it with slave
 * propagation, so drives come and go there too, and nothing the user runs
 * can rename or replace what is mounted in it. Startup adopts a live mount
 * of the same removable device; it never tears down another mount below
 * Drives, and it removes only folders this service created itself.
 *
 * Which disks are the user's follows udisks2 (udiskslinuxblock.c
 * update_hints: USB, FireWire and removable media; 80-udisks2.rules: eMMC is
 * the system's, and so are EFI system, BIOS boot, recovery and the other
 * hidden partitions), and the AHCI ports the kernel calls external (eSATA)
 * are the user's too. A disk with any part mounted elsewhere, held (LUKS,
 * LVM, RAID) or used as swap is left alone.
 *
 * Ejecting is asked by the file manager (files.c, e) at DRIVES_CONTROL, root
 * only. A job of this service, which is never frozen as the file manager is
 * when another program has the screen, does what udisks2's Drive.PowerOff
 * and Drive.Eject do: it names what keeps the drive busy, or unmounts every
 * filesystem of the drive, flushes it, and powers a USB drive off (or ejects
 * a removable medium, or stops an eSATA disk). It tells the file manager how
 * that went, and the shared display as a notification: "don't unplug yet",
 * then "can be safely unplugged". A drive unmounted that way is not mounted
 * again until it is plugged in again.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <scsi/sg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "base.h"

/* Tests point these at fixtures. */
#ifndef SYS_ROOT
#define SYS_ROOT "" /* before /sys */
#endif
#ifndef DEV_ROOT
#define DEV_ROOT "/dev"
#endif
#ifndef PROC_SWAPS
#define PROC_SWAPS "/proc/swaps"
#endif
#ifndef PROC_ROOT
#define PROC_ROOT "/proc" /* the processes that may keep a drive busy */
#endif
#ifndef DRIVES_CONTROL
#define DRIVES_CONTROL "/run/locachy/drives/control" /* files.c asks to eject here */
#endif
#ifndef DRIVES_LOCKED
#define DRIVES_LOCKED "/run/locachy/locked" /* process 1: no new mounts while locked */
#endif
#ifndef HOST_BUS
#define HOST_BUS "/run/user/0/bus" /* root's session bus: locachy-bar's notifications */
#endif
#ifndef DRIVER_SENSE
#define DRIVER_SENSE 0x08
#endif

#define USER 1000
#define MAX_DRIVES 32
#define MAX_DISKS 16
#define MAX_BLOCKS 64

struct drive {
    char dev[32];       /* sdb1, mmcblk0p1 */
    char disk[32];      /* sdb, mmcblk0 */
    char dir[512];      /* where it is mounted; "" if it is not */
    int seen, ejected, owned; /* adopted mounts stay when this service stops */
    dev_t device;            /* block identity, retained after unplug */
    unsigned long long seq;  /* the disk's diskseq then: its medium */
};

/* Whose a disk is; DISK_SYSTEM ones are never mounted here. */
enum { DISK_SYSTEM, DISK_USB, DISK_FIREWIRE, DISK_REMOVABLE, DISK_ESATA, DISK_SD };

struct disk {
    char name[32];
    unsigned long long seq; /* diskseq: a new number for each disk and medium */
    int kind, seen, blocked, said;
    char usb_interface[PATH_MAX], usb_device[PATH_MAX]; /* sysfs, USB disks */
};

static struct drive drives[MAX_DRIVES];
static int ndrives;
static struct disk disks[MAX_DISKS];
static int ndisks;
static char top[256]; /* ~/Drives */
static int topfd = -1; /* ~/Drives, the shared mount's own root */
static pid_t job;      /* the eject job, if one runs */

static int read_line(const char *path, char *out, size_t size)
{
    FILE *in = fopen(path, "re");
    int ok = in && fgets(out, (int)size, in);
    if (in) fclose(in);
    if (ok) out[strcspn(out, "\n")] = '\0';
    return ok ? 0 : -1;
}

/* /sys/block/DISK[/PART]/FILE: a partition's directory is in its disk's. */
static int block_attr(const char *disk, const char *part, const char *file, char *out, size_t size)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/sys/block/%s%s%s/%s", SYS_ROOT, disk, part ? "/" : "", part ? part : "", file);
    return read_line(path, out, size);
}

static unsigned long long disk_seq(const char *disk)
{
    char value[32];
    return block_attr(disk, NULL, "diskseq", value, sizeof value) == 0 ? strtoull(value, NULL, 10) : 0;
}

static int removable_media(const char *disk)
{
    char value[8];
    return block_attr(disk, NULL, "removable", value, sizeof value) == 0 && value[0] == '1';
}

/* The disk's ancestors in sysfs, as udisks2 reads them through GUdev
 * (udiskslinuxdrive.c set_connection_bus: the parent usb_interface, which is
 * also the Drive.SiblingId of a USB device's other LUNs, and its usb_device,
 * which Drive.PowerOff removes; a firewire parent). */
static void ancestry(const char *name, struct disk *k, int *firewire, int *ata, int *host)
{
    char path[PATH_MAX], real[PATH_MAX], devices[PATH_MAX];
    snprintf(path, sizeof path, "%s/sys/block/%s", SYS_ROOT, name);
    snprintf(devices, sizeof devices, "%s/sys/devices", SYS_ROOT);
    if (!realpath(path, real) || !realpath(devices, path)) return;
    size_t n = strlen(path);
    while (strncmp(real, path, n) == 0 && real[n] == '/') {
        char *base = strrchr(real, '/') + 1, link[PATH_MAX], file[PATH_MAX + 16], line[128];
        int end = 0, number;
        if (sscanf(base, "host%d%n", &number, &end) == 1 && !base[end] && *host < 0) *host = number;
        if (strncmp(base, "ata", 3) == 0 && base[3] >= '0' && base[3] <= '9') *ata = 1;
        snprintf(file, sizeof file, "%s/subsystem", real);
        ssize_t len = readlink(file, link, sizeof link - 1);
        link[len > 0 ? len : 0] = '\0';
        const char *subsystem = strrchr(link, '/') ? strrchr(link, '/') + 1 : link;
        *firewire |= strcmp(subsystem, "firewire") == 0;
        if (strcmp(subsystem, "usb") == 0) {
            snprintf(file, sizeof file, "%s/uevent", real);
            FILE *in = fopen(file, "re");
            while (in && fgets(line, sizeof line, in)) {
                if (strcmp(line, "DEVTYPE=usb_interface\n") == 0 && !k->usb_interface[0]) {
                    snprintf(k->usb_interface, sizeof k->usb_interface, "%s", real);
                } else if (strcmp(line, "DEVTYPE=usb_device\n") == 0 && k->usb_interface[0] && !k->usb_device[0]) {
                    snprintf(k->usb_device, sizeof k->usb_device, "%s", real);
                }
            }
            if (in) fclose(in);
        }
        *strrchr(real, '/') = '\0';
    }
}

/* An AHCI port the kernel calls external, eSATA: drivers/ata/ahci.c
 * ahci_mark_external_port sets ATA_PFLAG_EXTERNAL for PxCMD.ESP with
 * CAP.SXS, or PxCMD.HPCP. Both registers are in sysfs (ahci_port_cmd,
 * ahci_host_caps: Documentation/ABI/testing/sysfs-class-scsi_host). Reading
 * them resumes a suspended port, so it is done once per disk. */
static int ahci_external(int host)
{
    char path[PATH_MAX], cmd[32], caps[32];
    snprintf(path, sizeof path, "%s/sys/class/scsi_host/host%d/ahci_port_cmd", SYS_ROOT, host);
    if (read_line(path, cmd, sizeof cmd) < 0) return 0;
    snprintf(path, sizeof path, "%s/sys/class/scsi_host/host%d/ahci_host_caps", SYS_ROOT, host);
    if (read_line(path, caps, sizeof caps) < 0) return 0;
    unsigned long port = strtoul(cmd, NULL, 16), host_caps = strtoul(caps, NULL, 16);
    return ((port & 1ul << 21) && (host_caps & 1ul << 5)) || (port & 1ul << 18);
}

static int classify(const char *name, struct disk *k)
{
    char path[PATH_MAX], value[32];
    k->usb_interface[0] = k->usb_device[0] = '\0';
    if (strncmp(name, "mmcblk", 6) == 0) {
        /* 80-udisks2.rules: MMC_TYPE MMC is ID_DRIVE_FLASH_MMC, which
         * udiskslinuxdrive.c set_media makes not removable: an eMMC, the
         * system's (its boot and rpmb areas too). SD, or no type (the rules'
         * fallback), is a card. The type: drivers/mmc/core/bus.c type_show. */
        if (strstr(name, "boot") || strstr(name, "rpmb")) return DISK_SYSTEM;
        snprintf(path, sizeof path, "%s/sys/block/%s/device/type", SYS_ROOT, name);
        return read_line(path, value, sizeof value) == 0 && strcmp(value, "MMC") == 0 ? DISK_SYSTEM : DISK_SD;
    }
    if (strncmp(name, "sd", 2) != 0) return DISK_SYSTEM;
    int firewire = 0, ata = 0, host = -1;
    ancestry(name, k, &firewire, &ata, &host);
    /* udiskslinuxblock.c update_hints: removable media, USB and FireWire
     * are the user's, "as we don't want to automount the world". */
    if (k->usb_interface[0]) return DISK_USB;
    if (firewire) return DISK_FIREWIRE;
    if (removable_media(name)) return DISK_REMOVABLE;
    /* Beyond udisks2 (where only a UDISKS_SYSTEM=0 rule does it): eSATA. */
    return ata && host >= 0 && ahci_external(host) ? DISK_ESATA : DISK_SYSTEM;
}

/* The disk's record, classified again whenever its diskseq changes. */
static struct disk *disk_at(const char *name)
{
    unsigned long long seq = disk_seq(name);
    struct disk *k = NULL;
    for (int i = 0; i < ndisks && !k; i++) {
        if (strcmp(disks[i].name, name) == 0) k = &disks[i];
    }
    if (!k) {
        if (ndisks == MAX_DISKS) return NULL;
        k = &disks[ndisks++];
        memset(k, 0, sizeof *k);
        snprintf(k->name, sizeof k->name, "%s", name);
        k->seq = seq + 1; /* classified below */
    }
    if (k->seq != seq) {
        k->seq = seq;
        k->kind = classify(name, k);
    }
    k->seen = 1;
    return k;
}

static struct disk *disk_named(const char *name)
{
    for (int i = 0; i < ndisks; i++) {
        if (strcmp(disks[i].name, name) == 0) return &disks[i];
    }
    return NULL;
}

/* The partitions of a disk (sdb1, sdb2), as the kernel lists them. */
static int partitions(const char *disk, char names[][32], int max)
{
    char path[PATH_MAX], part[PATH_MAX + 300];
    struct stat st;
    struct dirent *p;
    int n = 0;
    snprintf(path, sizeof path, "%s/sys/block/%s", SYS_ROOT, disk);
    DIR *dir = opendir(path);
    while (dir && (p = readdir(dir)) != NULL && n < max) {
        snprintf(part, sizeof part, "%s/%s/partition", path, p->d_name);
        if (strncmp(p->d_name, disk, strlen(disk)) == 0 && strlen(p->d_name) < 32 && stat(part, &st) == 0) {
            snprintf(names[n++], 32, "%s", p->d_name);
        }
    }
    if (dir) closedir(dir);
    return n;
}

/* TYPE, LABEL and the partition entry (blkid -p prints it: util-linux
 * misc-utils/blkid.c lowprobe_superblocks, BLKID_PARTS_ENTRY_DETAILS);
 * -1 if there is no filesystem. */
struct probe {
    char type[32], label[128], scheme[16], entry[48], number[8];
};

static int probe(const char *dev, struct probe *p)
{
    char cmd[128], line[512];
    snprintf(cmd, sizeof cmd, "blkid -p -o export /dev/%s 2>/dev/null", dev);
    FILE *in = popen(cmd, "re");
    memset(p, 0, sizeof *p);
    while (in && fgets(line, sizeof line, in)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "TYPE=", 5) == 0) {
            snprintf(p->type, sizeof p->type, "%.31s", line + 5);
        } else if (strncmp(line, "LABEL=", 6) == 0) {
            snprintf(p->label, sizeof p->label, "%.127s", line + 6);
        } else if (strncmp(line, "PART_ENTRY_SCHEME=", 18) == 0) {
            snprintf(p->scheme, sizeof p->scheme, "%.15s", line + 18);
        } else if (strncmp(line, "PART_ENTRY_TYPE=", 16) == 0) {
            snprintf(p->entry, sizeof p->entry, "%.47s", line + 16);
        } else if (strncmp(line, "PART_ENTRY_NUMBER=", 18) == 0) {
            snprintf(p->number, sizeof p->number, "%.7s", line + 18);
        }
    }
    if (in) pclose(in);
    return p->type[0] ? 0 : -1;
}

static int listed(const char *value, const char *const *list)
{
    for (; *list; list++) {
        if (strcasecmp(value, *list) == 0) return 1;
    }
    return 0;
}

/* 80-udisks2.rules, "Devices which should not be display in the user
 * interface" (UDISKS_IGNORE): Apple boot, special DOS types (EFI, hidden,
 * RAID, LVM), EFI system, BIOS boot, Microsoft reserved and recovery, RAID
 * and LVM GUIDs, XBOOTLDR, and recovery labels; isohybrid images are shown. */
static int ignored(const struct probe *p)
{
    static const char *const dos[] = {
        "0x0", "0x11", "0x12", "0x14", "0x16", "0x17", "0x1b", "0x1c", "0x1e", "0x27", "0x3d", "0x84", "0x8d",
        "0x8e", "0x90", "0x91", "0x92", "0x93", "0x97", "0x98", "0x9a", "0x9b", "0xab", "0xbb", "0xc2", "0xc3",
        "0xdd", "0xef", "0xfd", NULL,
    };
    static const char *const gpt[] = {
        "426f6f74-0000-11aa-aa11-00306543ecac", "69646961-6700-11aa-aa11-00306543ecac",
        "52637672-7900-11aa-aa11-00306543ecac", "c12a7328-f81f-11d2-ba4b-00a0c93ec93b",
        "21686148-6449-6e6f-744e-656564454649", "a19d880f-05fc-4d3b-a006-743f0f84911e",
        "e6d6d379-f507-44c2-a23c-238f2a3df928", "e3c9e316-0b5c-4db8-817d-f92df00215ae",
        "de94bba4-06d1-4d40-a16a-bfd50179d6ac", "bc13c2ff-59e6-4262-a352-b275fd6f7172", NULL,
    };
    static const char *const recovery[] = {
        "Recovery", "RECOVERY", "Lenovo_Recovery", "HP_RECOVERY", "Recovery_Partition", "DellUtility",
        "DellRestore", "IBM_SERVICE", "SERVICEV001", "SERVICEV002", "SYSTEM_RESERVED", "System_Reserved",
        "WINRE_DRV", "DIAGS", "IntelRST", NULL,
    };
    if (strcmp(p->scheme, "dos") == 0 && (!strcmp(p->entry, "0x0") || !strcmp(p->entry, "0x17")) &&
        (!strcmp(p->number, "1") || !strcmp(p->number, "2")) &&
        (!strcmp(p->type, "iso9660") || !strcmp(p->type, "udf"))) {
        return 0;
    }
    if ((!strcmp(p->type, "ntfs") || !strcmp(p->type, "vfat"))) {
        for (int i = 0; recovery[i]; i++) {
            if (strcmp(p->label, recovery[i]) == 0) return 1;
        }
    }
    return (strcmp(p->scheme, "mac") == 0 && strcmp(p->entry, "Apple_Bootstrap") == 0) ||
           (strcmp(p->scheme, "dos") == 0 && listed(p->entry, dos)) ||
           (strcmp(p->scheme, "gpt") == 0 && listed(p->entry, gpt));
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

/* A folder directly in Drives: where this service mounts. */
static const char *drive_leaf(const char *path)
{
    size_t n = strlen(top);
    return strncmp(path, top, n) == 0 && path[n] == '/' && path[n + 1] && !strchr(path + n + 1, '/') ?
           path + n + 1 : NULL;
}

/* Whether a block device (sdb, sdb1) is the system's now, why says so: what
 * udisks2 refuses to power off (udiskslinuxdriveobject.c
 * udisks_linux_drive_object_is_not_in_use: mounted, or unlocked: a holder
 * such as dm-crypt, LVM or RAID), and swap. Mounts in Drives are this
 * service's. */
static int held(const char *disk, const char *part, char *why, size_t size)
{
    const char *dev = part ? part : disk;
    char path[PATH_MAX], number[32], source[64], *line = NULL;
    unsigned int want_maj = 0, want_min = 0;
    size_t lsize = 0;
    struct dirent *e;
    int found = 0;
    snprintf(path, sizeof path, "%s/sys/block/%s%s%s/holders", SYS_ROOT, disk, part ? "/" : "", part ? part : "");
    DIR *holders = opendir(path);
    while (holders && !found && (e = readdir(holders)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(why, size, "%s is used by %s", dev, e->d_name);
        found = 1;
    }
    if (holders) closedir(holders);
    if (block_attr(disk, part, "dev", number, sizeof number) == 0) sscanf(number, "%u:%u", &want_maj, &want_min);
    snprintf(source, sizeof source, "/dev/%s", dev);
    FILE *in = found ? NULL : fopen("/proc/self/mountinfo", "re");
    while (in && !found && getline(&line, &lsize, in) >= 0) {
        char point[4096], from[4096];
        unsigned int maj, min;
        const char *dash = strstr(line, " - ");
        if (!dash || sscanf(line, "%*s %*s %u:%u %*s %4095s", &maj, &min, point) != 3 ||
            sscanf(dash + 3, "%*s %4095s", from) != 1) continue;
        mount_unescape(point);
        mount_unescape(from);
        if (((maj == want_maj && min == want_min && (maj || min)) || strcmp(from, source) == 0) && !drive_leaf(point)) {
            snprintf(why, size, "%s is mounted at %.400s", dev, point);
            found = 1;
        }
    }
    if (in) fclose(in);
    in = found ? NULL : fopen(PROC_SWAPS, "re");
    while (in && !found && getline(&line, &lsize, in) >= 0) {
        char name[4096];
        if (sscanf(line, "%4095s", name) == 1 && strcmp(name, source) == 0) {
            snprintf(why, size, "%s is swap space", dev);
            found = 1;
        }
    }
    if (in) fclose(in);
    free(line);
    return found;
}

/* Any part of the disk held: then all of it is the system's. */
static int disk_held(const char *disk, char *why, size_t size)
{
    char parts[MAX_BLOCKS][32];
    int n = partitions(disk, parts, MAX_BLOCKS);
    if (held(disk, NULL, why, size)) return 1;
    for (int i = 0; i < n; i++) {
        if (held(disk, parts[i], why, size)) return 1;
    }
    return 0;
}

/* Reuse an existing mount of this actual removable block device. No mount
 * below Drives is torn down at startup, and an unrelated mount is not ours. */
static int adopt_mount(struct drive *d)
{
    char source[64], *line = NULL;
    struct stat st;
    size_t size = 0;
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
        if (strcmp(from, source) != 0 || strcmp(root, "/") != 0 || !drive_leaf(path) ||
            strlen(path) >= sizeof d->dir) continue;
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

/* A folder this service made in Drives, removed (by its name in Drives:
 * nothing below a root folder can be renamed by the user). */
static void remove_dir(const char *dir)
{
    const char *leaf = drive_leaf(dir);
    if (leaf && topfd >= 0) unlinkat(topfd, leaf, AT_REMOVEDIR);
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
    struct probe p;
    char name[160], leaf[176], src[64], target[64];
    if (probe(d->dev, &p) < 0) {
        return; /* no filesystem (yet) */
    }
    if (ignored(&p)) {
        say("drives: %s: a system partition (%s %s), not mounted", d->dev, p.scheme, p.entry);
        d->ejected = 1;
        return;
    }
    int k = 0, n = (int)(sizeof kinds / sizeof kinds[0]);
    while (k < n && strcmp(kinds[k].type, p.type) != 0) {
        k++;
    }
    if (k == n) {
        say("drives: %s: %s is not a filesystem mounted here", d->dev, p.type);
        d->ejected = 1;
        return;
    }
    snprintf(name, sizeof name, "%s", p.label[0] ? p.label : d->dev);
    for (char *c = name; *c; c++) {
        if (*c == '/' || (unsigned char)*c < 32) {
            *c = '_';
        }
    }
    /* Made in Drives by its descriptor and mounted on the folder's own
     * descriptor, never by a path that could lead elsewhere (systemd
     * src/basic/mountpoint-util.c mount_nofollow: /proc/self/fd/N). */
    snprintf(leaf, sizeof leaf, "%s", name);
    for (int i = 2; mkdirat(topfd, leaf, 0755) < 0; i++) {
        if (i > 9) {
            say("drives: %s: no place for it in %s", d->dev, top);
            d->ejected = 1;
            return;
        }
        snprintf(leaf, sizeof leaf, "%s (%d)", name, i);
    }
    snprintf(d->dir, sizeof d->dir, "%s/%s", top, leaf);
    int at = openat(topfd, leaf, O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
    int ro = strcmp(p.type, "iso9660") == 0 || strcmp(p.type, "udf") == 0;
    snprintf(src, sizeof src, "/dev/%s", d->dev);
    snprintf(target, sizeof target, "/proc/self/fd/%d", at);
    if (at < 0 || mount(src, target, kinds[k].fs, MS_NOSUID | MS_NODEV | (ro ? MS_RDONLY : 0),
                        kinds[k].options) < 0) {
        say("drives: cannot mount %s (%s) at %s: %m", src, p.type, d->dir);
        if (at >= 0) close(at);
        unlinkat(topfd, leaf, AT_REMOVEDIR);
        d->dir[0] = '\0';
        d->ejected = 1;
        return;
    }
    close(at);
    struct stat st;
    d->device = stat(src, &st) == 0 && S_ISBLK(st.st_mode) ? st.st_rdev : 0;
    d->owned = 1;
    say("drives: %s (%s) at %s", src, p.type, d->dir);
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

/* A filesystem device: a partition of the disk, or the disk if it has none.
 * Of a disk the system holds, what is mounted already stays, and nothing
 * new is mounted. */
static void consider(const char *dev, const struct disk *k, int may_mount)
{
    struct drive *d = find(dev);
    if (!d && may_mount && ndrives < MAX_DRIVES) {
        d = &drives[ndrives++];
        memset(d, 0, sizeof *d);
        snprintf(d->dev, sizeof d->dev, "%s", dev);
        snprintf(d->disk, sizeof d->disk, "%s", k->name);
        d->seq = k->seq;
    }
    if (!d) {
        return;
    }
    d->seen = 1;
    if (d->dir[0] && !mounted_device(d)) {
        say("drives: %s was unmounted (ejected); not mounted again until it is plugged in again", d->dev);
        if (d->owned && !mounted(d->dir)) remove_dir(d->dir);
        d->dir[0] = '\0';
        d->owned = 0;
        d->ejected = 1;
    }
    if (may_mount && !d->dir[0] && !d->ejected && !adopt_mount(d)) {
        mount_drive(d);
    }
}

/* A medium changed under its mounts (a card swapped in a reader). The kernel
 * counts each new medium in diskseq (block/disk-events.c disk_check_events:
 * inc_diskseq with DISK_EVENT_MEDIA_CHANGE), but partitions mounted from the
 * old one stay, with its offsets, while they are open (block/partitions/
 * core.c bdev_disk_changed: -EBUSY), and once the media poll has cleared
 * sdev->changed (drivers/scsi/sd.c sd_check_events) writes through them
 * reach the new card. udisks2 cleans up only when the size is 0
 * (udisksstate.c); here such a mount is detached at once, as one pulled
 * out, and nothing on the disk is mounted until its partitions were read
 * again (BLKRRPART, as util-linux eject does), which the kernel does once
 * none of them is open. A disk without mounts of ours just starts afresh. */
static int medium_changed(struct disk *k)
{
    int changed = 0;
    for (int i = 0; i < ndrives;) {
        struct drive *d = &drives[i];
        if (strcmp(d->disk, k->name) != 0 || d->seq == k->seq) {
            i++;
            continue;
        }
        if (d->dir[0] && mounted_device(d)) {
            say("drives: the medium in %s changed; unmounting %s", k->name, d->dir);
            if (umount2(d->dir, MNT_DETACH | UMOUNT_NOFOLLOW) == 0 && d->owned) remove_dir(d->dir);
            changed = 1;
        }
        *d = drives[--ndrives];
    }
    if (changed) {
        k->blocked = 1;
        k->said = 0;
    }
    if (k->blocked) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", DEV_ROOT, k->name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0 && ioctl(fd, BLKRRPART) == 0) {
            say("drives: read the partitions of %s again", k->name);
            k->blocked = 0;
            k->seq = disk_seq(k->name);
        } else if (!k->said) {
            say("drives: %s: the old medium's partitions are still open; nothing is mounted from it", k->name);
            k->said = 1;
        }
        if (fd >= 0) close(fd);
    }
    return k->blocked;
}

static void scan(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/sys/block", SYS_ROOT);
    DIR *block = opendir(path);
    struct dirent *e;
    for (int i = 0; i < ndrives; i++) {
        drives[i].seen = 0;
    }
    for (int i = 0; i < ndisks; i++) {
        disks[i].seen = 0;
    }
    while (block && (e = readdir(block)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof disks[0].name) {
            continue;
        }
        struct disk *k = disk_at(e->d_name);
        char size[32], why[512], parts[MAX_BLOCKS][32];
        if (!k || k->kind == DISK_SYSTEM) {
            continue;
        }
        if (block_attr(k->name, NULL, "size", size, sizeof size) < 0 || atoll(size) == 0) {
            continue; /* a card reader with no card */
        }
        if (medium_changed(k)) {
            continue;
        }
        int held = disk_held(k->name, why, sizeof why);
        if (held && !k->said) say("drives: %s is the system's; not mounted here: %s", k->name, why);
        k->said = held;
        int may_mount = !held && access(DRIVES_LOCKED, F_OK) != 0;
        int n = partitions(k->name, parts, MAX_BLOCKS);
        for (int i = 0; i < n; i++) {
            consider(parts[i], k, may_mount);
        }
        if (n == 0) {
            consider(k->name, k, may_mount);
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
            if (umount2(d->dir, MNT_DETACH | UMOUNT_NOFOLLOW) == 0 && d->owned) remove_dir(d->dir);
            /* A card pulled from a reader that stays: files still open on
             * the detached mount keep its partitions, which the next card
             * must not be mounted through (medium_changed). */
            struct disk *k = disk_named(d->disk);
            if (k && k->seen) {
                k->blocked = 1;
                k->said = 0;
            }
        }
        *d = drives[--ndrives];
    }
    for (int i = 0; i < ndisks;) {
        if (disks[i].seen) {
            i++;
        } else {
            disks[i] = disks[--ndisks];
        }
    }
}

/* ---- Ejecting ---- */

/* A string in GVariant's text format, for gdbus. */
static void gvariant_string(char *out, size_t size, const char *s)
{
    size_t n = 0;
    if (size < 3) return;
    out[n++] = '\'';
    for (; *s && n + 4 < size; s++) {
        if (*s == '\'' || *s == '\\') out[n++] = '\\';
        if (*s == '\n') {
            out[n++] = '\\';
            out[n++] = 'n';
            continue;
        }
        out[n++] = *s;
    }
    out[n++] = '\'';
    out[n] = '\0';
}

/* A notification on the shared display: locachy-bar's
 * org.freedesktop.Notifications on root's session bus, sent with gdbus as
 * rclone-client sends its own. It replaces the one numbered replaces (0:
 * none); its number, 0 if there is no display. */
static unsigned int notify(unsigned int replaces, int timeout, const char *summary, const char *body)
{
    char address[PATH_MAX + 16], id[16], ms[16], s[512], b[1024], out[128] = "";
    int pipefd[2];
    if (access(HOST_BUS, F_OK) != 0 || pipe2(pipefd, O_CLOEXEC) < 0) return 0;
    snprintf(address, sizeof address, "unix:path=%s", HOST_BUS);
    snprintf(id, sizeof id, "%u", replaces);
    snprintf(ms, sizeof ms, "%d", timeout);
    gvariant_string(s, sizeof s, summary);
    gvariant_string(b, sizeof b, body);
    char *argv[] = {
        "gdbus", "call", "--address", address, "--dest", "org.freedesktop.Notifications",
        "--object-path", "/org/freedesktop/Notifications", "--method", "org.freedesktop.Notifications.Notify",
        "'Drives'", id, "''", s, b, "[]", "{}", ms, NULL,
    };
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null < 0 || dup2(null, 0) < 0 || dup2(pipefd[1], 1) < 0 || dup2(null, 2) < 0) _exit(127);
        execv("/usr/bin/gdbus", argv);
        _exit(127);
    }
    close(pipefd[1]);
    size_t got = 0;
    struct pollfd p = { .fd = pipefd[0], .events = POLLIN };
    while (pid > 0 && got + 1 < sizeof out && poll(&p, 1, 2000) > 0) {
        ssize_t n = read(pipefd[0], out + got, sizeof out - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    out[got] = '\0';
    close(pipefd[0]);
    if (pid > 0) {
        kill(pid, SIGKILL); /* done, or stuck on a bus that does not answer */
        waitpid(pid, NULL, 0);
    }
    unsigned int number = 0;
    return sscanf(out, "(uint32 %u,)", &number) == 1 ? number : 0;
}

static void reply(int fd, const char *kind, const char *name, const char *detail)
{
    char line[PATH_MAX + 128];
    int n = snprintf(line, sizeof line, "%s\t%s\t%s", kind, name, detail);
    if (n > 0) send(fd, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1, MSG_NOSIGNAL);
}

static int on_device(const char *path, dev_t dev)
{
    struct statx sx;
    /* AT_STATX_DONT_SYNC: a FUSE file answers from its cache (fs/fuse/dir.c
     * fuse_update_get_attr), so a stuck cloud drive cannot stall this. */
    return statx(AT_FDCWD, path, AT_STATX_DONT_SYNC, STATX_INO, &sx) == 0 &&
           makedev(sx.stx_dev_major, sx.stx_dev_minor) == dev;
}

/* What keeps a filesystem busy, named as fuser -m names it (psmisc
 * src/fuser.c scan_procs: each process's working folder, root, program,
 * open files and mapped files on the device), with the container each runs
 * in (its cgroup, /app_NAME: link/locachy-app). */
static int users(dev_t dev, char *out, size_t size)
{
    DIR *proc = opendir(PROC_ROOT);
    struct dirent *e;
    int found = 0;
    while (proc && (e = readdir(proc)) != NULL) {
        char path[PATH_MAX], name[64] = "", line[512], who[160];
        static const char *const links[] = { "cwd", "root", "exe" };
        int hit = 0;
        if (e->d_name[0] < '1' || e->d_name[0] > '9' || atoi(e->d_name) == getpid()) continue;
        for (int i = 0; i < 3 && !hit; i++) {
            snprintf(path, sizeof path, "%s/%s/%s", PROC_ROOT, e->d_name, links[i]);
            hit = on_device(path, dev);
        }
        snprintf(path, sizeof path, "%s/%s/fd", PROC_ROOT, e->d_name);
        DIR *fds = hit ? NULL : opendir(path);
        struct dirent *f;
        while (fds && !hit && (f = readdir(fds)) != NULL) {
            if (f->d_name[0] == '.') continue;
            snprintf(path, sizeof path, "%s/%s/fd/%s", PROC_ROOT, e->d_name, f->d_name);
            hit = on_device(path, dev);
        }
        if (fds) closedir(fds);
        snprintf(path, sizeof path, "%s/%s/maps", PROC_ROOT, e->d_name);
        FILE *maps = hit ? NULL : fopen(path, "re");
        while (maps && !hit && fgets(line, sizeof line, maps)) {
            unsigned int maj, min;
            unsigned long inode;
            hit = sscanf(line, "%*s %*s %*s %x:%x %lu", &maj, &min, &inode) == 3 && inode && makedev(maj, min) == dev;
        }
        if (maps) fclose(maps);
        if (!hit) continue;
        snprintf(path, sizeof path, "%s/%s/comm", PROC_ROOT, e->d_name);
        if (read_line(path, name, sizeof name) < 0) snprintf(name, sizeof name, "%.63s", e->d_name);
        snprintf(who, sizeof who, "%s", name);
        snprintf(path, sizeof path, "%s/%s/cgroup", PROC_ROOT, e->d_name);
        if (read_line(path, line, sizeof line) == 0 && strncmp(line, "0::/app_", 8) == 0) {
            line[8 + strcspn(line + 8, "/")] = '\0';
            snprintf(who, sizeof who, "%.63s (%.80s)", name, line + 8);
        }
        size_t used = strlen(out);
        if (!strstr(out, who) && used + strlen(who) + 3 < size) {
            snprintf(out + used, size - used, "%s%s", used ? ", " : "", who);
        }
        found = 1;
    }
    if (proc) closedir(proc);
    return found;
}

/* One SCSI command without data, through SG_IO's v3 interface, which a
 * block device takes (drivers/scsi/scsi_ioctl.c sg_io); 30 s as udisks2's
 * send_scsi_command_sync (udiskslinuxdrive.c). 0 when GOOD, 1 otherwise,
 * with the sense data; -1 when it could not be sent. */
static int scsi_command(int fd, const unsigned char *cdb, unsigned char len, unsigned char *sense, unsigned char slen)
{
    sg_io_hdr_t io;
    memset(&io, 0, sizeof io);
    memset(sense, 0, slen);
    io.interface_id = 'S';
    io.cmd_len = len;
    io.cmdp = (unsigned char *)cdb;
    io.dxfer_direction = SG_DXFER_NONE;
    io.sbp = sense;
    io.mx_sb_len = slen;
    io.timeout = 30000;
    if (ioctl(fd, SG_IO, &io) < 0 || io.host_status) return -1;
    return io.status == 0 && io.driver_status == 0 ? 0 : io.driver_status == DRIVER_SENSE ? 1 : -1;
}

/* udisks2 handle_power_off (udiskslinuxdrive.c): SYNCHRONIZE CACHE (10),
 * then START STOP UNIT to stop; failures do not count ("some USB-attached
 * hard-disks fail with one or both of these commands"). */
static void stop_unit(int fd, const char *disk)
{
    static const unsigned char sync_cache[10] = { 0x35 }, stop[6] = { 0x1b };
    unsigned char sense[32];
    int synced = scsi_command(fd, sync_cache, sizeof sync_cache, sense, sizeof sense) == 0;
    int stopped = scsi_command(fd, stop, sizeof stop, sense, sizeof sense) == 0;
    say("drives: %s: SYNCHRONIZE CACHE %s, START STOP UNIT %s", disk,
        synced ? "done" : "failed (ignored)", stopped ? "done" : "failed (ignored)");
}

/* util-linux sys-utils/eject.c eject_scsi: PREVENT ALLOW MEDIUM REMOVAL
 * (allow), START STOP UNIT to start, then to eject (LoEj); ILLEGAL REQUEST
 * to the first and MEDIUM NOT PRESENT to the second are not failures. Then
 * the partitions are read again (BLKRRPART), as eject does. */
static int eject_medium(int fd)
{
    static const unsigned char allow[6] = { 0x1e }, start[6] = { 0x1b, 0, 0, 0, 1, 0 },
                               eject[6] = { 0x1b, 0, 0, 0, 2, 0 };
    unsigned char sense[32];
    int r = scsi_command(fd, allow, sizeof allow, sense, sizeof sense);
    if (r < 0 || (r > 0 && (sense[2] & 0x0f) != 0x05)) return -1;
    r = scsi_command(fd, start, sizeof start, sense, sizeof sense);
    if (r < 0 || (r > 0 && sense[12] != 0x3a)) return -1;
    if (scsi_command(fd, eject, sizeof eject, sense, sizeof sense) != 0) return -1;
    ioctl(fd, BLKRRPART);
    return 0;
}

static int write_sysfs(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    int ok = fd >= 0 && write(fd, value, strlen(value)) == (ssize_t)strlen(value);
    if (fd >= 0 && close(fd) < 0) ok = 0;
    return ok ? 0 : -1;
}

struct mount_of {
    char path[4096];
    dev_t dev;  /* the filesystem's, as stat gives it */
    int block;  /* of blocks[] */
};

/* The mounts in Drives of the block devices. */
static int mounts_of(char blocks[][32], int nblocks, struct mount_of *out, int max)
{
    char *line = NULL;
    size_t size = 0;
    int n = 0;
    FILE *in = fopen("/proc/self/mountinfo", "re");
    while (in && n < max && getline(&line, &size, in) >= 0) {
        char point[4096], from[4096];
        unsigned int maj, min;
        const char *dash = strstr(line, " - ");
        if (!dash || sscanf(line, "%*s %*s %u:%u %*s %4095s", &maj, &min, point) != 3 ||
            sscanf(dash + 3, "%*s %4095s", from) != 1) continue;
        mount_unescape(point);
        mount_unescape(from);
        for (int i = 0; i < nblocks; i++) {
            if (strncmp(from, "/dev/", 5) == 0 && strcmp(from + 5, blocks[i]) == 0 && drive_leaf(point)) {
                snprintf(out[n].path, sizeof out[n].path, "%s", point);
                out[n].block = i;
                out[n++].dev = makedev(maj, min);
                break;
            }
        }
    }
    free(line);
    if (in) fclose(in);
    return n;
}

enum { EJECT_FLUSH, EJECT_MEDIUM, EJECT_POWER_OFF, EJECT_STOP };

/* The eject job: what udisks2 Drive.PowerOff (udiskslinuxdrive.c
 * handle_power_off) and Drive.Eject do after GVfs and KDE's Solid have
 * unmounted (gvfsudisks2drive.c gvfs_udisks2_drive_stop; Solid's
 * udisksstorageaccess.cpp slotDBusReply: Eject when the medium is
 * removable, else PowerOff when the drive can be powered off). c is the
 * file manager's connection; name is the folder's. 0 when it can be
 * unplugged. */
static int eject_job(int c, const struct drive *d, const char *name)
{
    struct disk *k = disk_named(d->disk);
    char blocks[MAX_BLOCKS][32], owner[MAX_BLOCKS][32], why[512] = "", who[512] = "", path[PATH_MAX], summary[256];
    static struct mount_of mounts[MAX_BLOCKS];
    int nblocks = 0, method;
    if (!k) {
        reply(c, "error", name, "not-drive");
        return 1;
    }
    if (k->kind == DISK_SD || (k->kind == DISK_FIREWIRE && !removable_media(k->name))) {
        method = EJECT_FLUSH; /* udisks2: only USB drives CanPowerOff */
    } else if (removable_media(k->name)) {
        method = EJECT_MEDIUM;
    } else {
        method = k->kind == DISK_USB ? EJECT_POWER_OFF : EJECT_STOP;
    }
    /* Powering a USB device off takes all its LUNs (udisks2 handle_power_off:
     * "A drive that is part of the same device is in use"). */
    for (int i = 0; i < ndisks; i++) {
        struct disk *o = &disks[i];
        if (o != k && !(method == EJECT_POWER_OFF && o->kind == DISK_USB &&
                        strcmp(o->usb_interface, k->usb_interface) == 0)) continue;
        int first = nblocks;
        if (nblocks < MAX_BLOCKS) snprintf(blocks[nblocks++], 32, "%.31s", o->name);
        nblocks += partitions(o->name, blocks + nblocks, MAX_BLOCKS - nblocks);
        for (int j = first; j < nblocks; j++) snprintf(owner[j], 32, "%.31s", o->name);
    }
    for (int i = 0; i < nblocks; i++) {
        int whole = strcmp(blocks[i], owner[i]) == 0;
        if (held(owner[i], whole ? NULL : blocks[i], why, sizeof why)) {
            say("drives: not ejecting %s: %s", name, why);
            reply(c, "error", name, why);
            snprintf(summary, sizeof summary, _("%s was not ejected"), name);
            notify(0, 10000, summary, why);
            return 1;
        }
    }
    int nmounts = mounts_of(blocks, nblocks, mounts, MAX_BLOCKS);
    for (int i = 0; i < nmounts; i++) {
        if (strcmp(owner[mounts[i].block], k->name) == 0) users(mounts[i].dev, who, sizeof who);
    }
    if (who[0]) {
        say("drives: not ejecting %s: in use by %s", name, who);
        reply(c, "busy", name, who);
        snprintf(summary, sizeof summary, _("%s is in use"), name);
        snprintf(why, sizeof why, _("Files on it are open in %s."), who);
        notify(0, 10000, summary, why);
        return 1;
    }
    for (int i = 0; i < nmounts; i++) {
        if (strcmp(owner[mounts[i].block], k->name) != 0) {
            snprintf(why, sizeof why, "%s, of the same USB device, is mounted at %s", blocks[mounts[i].block], mounts[i].path);
            say("drives: not ejecting %s: %s", name, why);
            reply(c, "error", name, why);
            snprintf(summary, sizeof summary, _("%s was not ejected"), name);
            notify(0, 10000, summary, why);
            return 1;
        }
    }
    reply(c, "writing", name, "");
    snprintf(summary, sizeof summary, _("Writing data to %s"), name);
    unsigned int note = notify(0, 0, summary, _("Don't unplug it yet."));
    for (int i = 0; i < nmounts; i++) {
        if (umount2(mounts[i].path, UMOUNT_NOFOLLOW) < 0) {
            /* Busy after all: a container's mount below it, or a file
             * opened since; what was unmounted stays so (as with GVfs). */
            say("drives: cannot unmount %s: %m", mounts[i].path);
            users(mounts[i].dev, who, sizeof who);
            reply(c, "busy", name, who);
            snprintf(summary, sizeof summary, _("%s is in use"), name);
            if (who[0]) snprintf(why, sizeof why, _("Files on it are open in %s."), who);
            notify(note, 10000, summary, who[0] ? why : _("One or more applications are keeping it busy."));
            return 1;
        }
        remove_dir(mounts[i].path);
        say("drives: unmounted %s", mounts[i].path);
    }
    /* udisks2 handle_power_off: each block device opened exclusively and
     * flushed, which also proves that nothing has it open any more (a
     * lazily detached mount, a program reading the device). */
    for (int i = 0; i < nblocks; i++) {
        snprintf(path, sizeof path, "%s/%s", DEV_ROOT, blocks[i]);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_EXCL | O_CLOEXEC);
        if (fd < 0 || fsync(fd) < 0) {
            say("drives: %s: %s: %m", name, path);
            snprintf(why, sizeof why, "%s: %s", blocks[i], strerror(errno));
            if (fd >= 0) close(fd);
            reply(c, "error", name, errno == EBUSY ? "held" : why);
            snprintf(summary, sizeof summary, _("%s was not ejected"), name);
            notify(note, 10000, summary, errno == EBUSY ? _("A program still has it open.") : why);
            return 1;
        }
        close(fd);
    }
    const char *how = "flushed";
    if (method != EJECT_FLUSH) {
        snprintf(path, sizeof path, "%s/%s", DEV_ROOT, k->name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_EXCL | O_CLOEXEC);
        if (fd >= 0 && method == EJECT_MEDIUM) {
            how = eject_medium(fd) == 0 ? "medium ejected" : "flushed (the medium was not ejected)";
        } else if (fd >= 0) {
            stop_unit(fd, k->name);
        }
        if (fd >= 0) close(fd);
        if (method == EJECT_POWER_OFF) {
            /* drivers/usb/core/sysfs.c remove_store: unconfigured, then its
             * port disabled (hub.c usb_remove_device). */
            char remove[PATH_MAX + 16];
            snprintf(remove, sizeof remove, "%s/remove", k->usb_device);
            how = k->usb_device[0] && write_sysfs(remove, "1") == 0 ? "powered off" : "stopped (not powered off)";
        } else if (method == EJECT_STOP) {
            /* No port to power off: the disk is taken away from the kernel
             * (drivers/scsi/scsi_sysfs.c sdev_store_delete); sd_remove's
             * sd_shutdown flushes it and, for libata's disks
             * (manage_runtime_start_stop), stops it. */
            snprintf(path, sizeof path, "%s/sys/block/%s/device/delete", SYS_ROOT, k->name);
            how = write_sysfs(path, "1") == 0 ? "stopped" : "flushed (not stopped)";
        }
    }
    say("drives: ejected %s (%s): %s", name, k->name, how);
    reply(c, "safe", name, how);
    snprintf(summary, sizeof summary, _("%s can be safely unplugged"), name);
    notify(note, 10000, summary, _("The device can be removed now."));
    return 0;
}

/* The control socket: root's only (its folder is 0700, and the peer must
 * be root). */
static int control_socket(void)
{
    char dir[PATH_MAX], parent[PATH_MAX];
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(dir, sizeof dir, "%s", DRIVES_CONTROL);
    if (!strrchr(dir, '/') || strlen(DRIVES_CONTROL) >= sizeof a.sun_path) return -1;
    *strrchr(dir, '/') = '\0';
    snprintf(parent, sizeof parent, "%s", dir);
    if (strrchr(parent, '/')) *strrchr(parent, '/') = '\0';
    mkdir(parent, 0755);
    if (mkdir(dir, 0700) < 0 && errno != EEXIST) return -1;
    if (chown(dir, 0, 0) < 0 || chmod(dir, 0700) < 0) return -1;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", DRIVES_CONTROL);
    unlink(a.sun_path);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || chmod(a.sun_path, 0600) < 0 || listen(fd, 4) < 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

/* "eject\tPATH" from the file manager: a drive's folder in Drives. */
static void take_request(int control)
{
    int c = accept4(control, NULL, NULL, SOCK_CLOEXEC);
    struct ucred cred;
    socklen_t len = sizeof cred;
    struct timeval wait = { .tv_sec = 1 };
    char request[PATH_MAX + 16];
    ssize_t n = -1;
    if (c < 0) return;
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || cred.uid != 0 ||
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait) < 0 ||
        (n = recv(c, request, sizeof request - 1, 0)) <= 6 || strncmp(request, "eject\t", 6) != 0) {
        close(c);
        return;
    }
    request[n] = '\0';
    const char *path = request + 6, *leaf = drive_leaf(path);
    struct drive *d = NULL;
    for (int i = 0; i < ndrives && leaf; i++) {
        if (strcmp(drives[i].dir, path) == 0 && mounted_device(&drives[i])) d = &drives[i];
    }
    pid_t pid = -1;
    if (!d) {
        reply(c, "error", leaf ? leaf : path, "not-drive");
    } else if (job > 0) {
        reply(c, "error", leaf, "another");
    } else if ((pid = fork()) == 0) {
        close(control);
        signal(SIGTERM, SIG_DFL);
        _exit(eject_job(c, d, leaf));
    } else if (pid < 0) {
        reply(c, "error", leaf, "fork");
    } else {
        job = pid;
    }
    close(c);
}

static volatile sig_atomic_t stop;

static void on_term(int sig)
{
    (void)sig;
    stop = 1;
}

/* ~/Drives: root's, 0755, and not a symbolic link; then a shared mount point
 * of its own (a bind of itself), keeping existing children when binding it,
 * a cloud service's that started before this one too. udisks2 mounts only
 * below root's /run/media/$USER (udiskslinuxfilesystem.c
 * calculate_mount_point), never in a folder the user can write. */
static int setup_top(void)
{
    struct stat before, after;
    char at[64];
    if (mkdir(top, 0755) < 0 && errno != EEXIST) {
        say("drives: cannot make %s: %m", top);
        return -1;
    }
    int fd = open(top, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &before) < 0 || fchown(fd, 0, 0) < 0 || fchmod(fd, 0755) < 0) {
        say("drives: %s is not a folder this service can own: %m", top);
        if (fd >= 0) close(fd);
        return -1;
    }
    snprintf(at, sizeof at, "/proc/self/fd/%d", fd);
    if (!mounted(top) && mount(at, at, NULL, MS_BIND | MS_REC, NULL) < 0) {
        say("drives: cannot make %s a mount point: %m", top);
        close(fd);
        return -1;
    }
    close(fd);
    /* The bind mount's own root now, which must be the same folder. */
    topfd = open(top, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    snprintf(at, sizeof at, "/proc/self/fd/%d", topfd);
    if (topfd < 0 || fstat(topfd, &after) < 0 || after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        mount(NULL, at, NULL, MS_SHARED, NULL) < 0) {
        say("drives: cannot make %s a shared mount point: %m", top);
        return -1;
    }
    return 0;
}

int main(void)
{
    struct passwd *pw = getpwuid(USER);
    if (!pw || !pw->pw_dir) {
        say("drives: no user %d", USER);
        return 1;
    }
    snprintf(top, sizeof top, "%s/Drives", pw->pw_dir);
    /* Keep existing mounts and folders. The scan adopts only actual removable
     * block devices; cloud drives and user-owned paths are not ours to clean. */
    if (setup_top() < 0) {
        return 1;
    }
    int control = control_socket();
    if (control < 0) say("drives: no control socket %s: %m; drives cannot be ejected", DRIVES_CONTROL);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);
    say("drives: watching for USB drives, eSATA disks and memory cards; mounted in %s", top);
    while (!stop) {
        scan();
        struct pollfd p = { .fd = control, .events = POLLIN };
        int ready = poll(&p, 1, 2000) > 0 && (p.revents & POLLIN);
        /* A job that has ended does not hold up the next request. */
        if (job > 0 && waitpid(job, NULL, WNOHANG) == job) {
            job = 0;
        }
        if (ready) {
            take_request(control);
        }
    }
    for (int i = 0; i < ndrives; i++) {
        if (drives[i].owned && drives[i].dir[0] && mounted_device(&drives[i]) &&
            umount2(drives[i].dir, UMOUNT_NOFOLLOW) == 0) remove_dir(drives[i].dir);
    }
    return 0;
}
