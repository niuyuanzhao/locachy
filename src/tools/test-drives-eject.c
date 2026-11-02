/* The drives service's choices on a fake sysfs, /dev and /proc, with a mount
 * table, blkid output and SCSI commands as fixtures: which disks are the
 * user's (udisks2's rules, eSATA), mounting through a descriptor in a
 * root folder, a medium changed under a mount, and ejecting (busy naming,
 * every partition, the same USB device's other LUNs, power off, a removable
 * medium, eSATA, a card in a slot). No mount, device or bus is touched.
 *   test-drives-eject DIR   (DIR: a new directory) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <scsi/sg.h>

static char root[256], sys_root[300], dev_root[300], proc_root[300], swaps[300];
static FILE *fixture_fopen(const char *, const char *);
static FILE *fixture_popen(const char *, const char *);
static int fixture_stat(const char *, struct stat *);
static int fixture_mount(const char *, const char *, const char *, unsigned long, const void *);
static int fixture_umount(const char *, int);
static int fixture_fchown(int, uid_t, gid_t);
static int fixture_ioctl(int, unsigned long, ...);
#define SYS_ROOT sys_root
#define DEV_ROOT dev_root
#define PROC_ROOT proc_root
#define PROC_SWAPS swaps
#define HOST_BUS "/nonexistent/bus"
#define DRIVES_CONTROL "/nonexistent/control"
#define fopen fixture_fopen
#define popen fixture_popen
#define pclose fclose
#define stat(path, st) fixture_stat(path, st)
#define mount fixture_mount
#define umount2 fixture_umount
#define fchown fixture_fchown
#define ioctl fixture_ioctl
#define main drives_main
#include "../progs/drives.c"
#undef main
#undef fopen
#undef popen
#undef pclose
#undef stat
#undef mount
#undef umount2
#undef fchown
#undef ioctl
#include "i18n-stub.h"

void say(const char *fmt, ...)
{
    if (!getenv("TEST_VERBOSE")) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static char table[16384];
static int mounts, unmounts, last_umount_flags, rereads, reread_result;
static char last_target[PATH_MAX];
static unsigned char cdbs[16];
static int ncdbs;
static dev_t scratch_dev;

/* The block devices: their numbers, and what blkid says of them. */
static const struct block {
    const char *name, *blkid;
    unsigned int maj, min; /* 0, 0: the scratch filesystem's, so that files there are "on" it */
} blocks[] = {
    { "sdx", "", 8, 112 },
    { "sdx1", "TYPE=vfat\nLABEL=MY DISK\nPART_ENTRY_SCHEME=gpt\nPART_ENTRY_TYPE=ebd0a0a2-b9e5-4433-87c0-68b6b72699c7\n", 0, 0 },
    { "sdx2", "TYPE=ext4\nLABEL=DATA\nPART_ENTRY_SCHEME=gpt\nPART_ENTRY_TYPE=0fc63daf-8483-4772-8e79-3d69d8477de4\n", 0, 0 },
    { "sdx3", "TYPE=vfat\nLABEL=EFI\nPART_ENTRY_SCHEME=gpt\nPART_ENTRY_TYPE=c12a7328-f81f-11d2-ba4b-00a0c93ec93b\n", 8, 115 },
    { "sdy", "", 8, 128 },
    { "sdy1", "TYPE=vfat\nLABEL=LUN1\n", 8, 129 },
    { "sds", "", 8, 144 },
    { "sds1", "TYPE=ext4\nLABEL=ADMIN\n", 8, 145 },
    { "sds2", "TYPE=vfat\nLABEL=HELD\n", 8, 146 },
    { "sdq", "", 8, 32 },
    { "sdq1", "TYPE=crypto_LUKS\n", 8, 33 },
    { "sdq2", "TYPE=vfat\nLABEL=LUKSDISK\n", 8, 34 },
    { "sdp", "", 8, 48 },
    { "sdp1", "TYPE=swap\n", 8, 49 },
    { "sdp2", "TYPE=vfat\nLABEL=SWAPDISK\n", 8, 50 },
    { "sdt", "TYPE=vfat\nLABEL=CARD\n", 8, 160 },
    { "sdu", "", 8, 176 },
    { "sdu1", "TYPE=ext4\nLABEL=SYSTEM\n", 8, 177 },
    { "sdv", "", 8, 192 },
    { "sdv1", "TYPE=ext4\nLABEL=ESATA\n", 8, 193 },
    { "sdw", "", 8, 208 },
    { "sdw1", "TYPE=vfat\nLABEL=FW\n", 8, 209 },
    { "sdw2", "TYPE=ntfs\nLABEL=Recovery\nPART_ENTRY_SCHEME=dos\nPART_ENTRY_TYPE=0x27\n", 8, 210 },
    { "mmcblk0", "", 179, 0 },
    { "mmcblk0p1", "TYPE=ext4\nLABEL=EMMC\n", 179, 1 },
    { "mmcblk0boot0", "", 179, 8 },
    { "mmcblk1", "", 179, 16 },
    { "mmcblk1p1", "TYPE=exfat\nLABEL=SD\n", 179, 17 },
};
#define NBLOCKS (int)(sizeof blocks / sizeof blocks[0])

static const struct block *block_named(const char *name)
{
    for (int i = 0; i < NBLOCKS; i++) {
        if (strcmp(blocks[i].name, name) == 0) return &blocks[i];
    }
    return NULL;
}

static dev_t block_dev(const struct block *b)
{
    return b->maj || b->min ? makedev(b->maj, b->min) : scratch_dev;
}

static FILE *fixture_fopen(const char *path, const char *mode)
{
    if (strcmp(path, "/proc/self/mountinfo") == 0) return fmemopen(table, strlen(table), "r");
    return fopen(path, mode);
}

static FILE *fixture_popen(const char *cmd, const char *mode)
{
    static char none[] = "";
    const char *dev = strstr(cmd, "/dev/");
    char name[32];
    (void)mode;
    assert(dev && sscanf(dev + 5, "%31s", name) == 1);
    const struct block *b = block_named(name);
    return fmemopen(b && b->blkid[0] ? (char *)b->blkid : none, b && b->blkid[0] ? strlen(b->blkid) : 1, "r");
}

static int fixture_stat(const char *path, struct stat *st)
{
    const struct block *b = strncmp(path, "/dev/", 5) == 0 ? block_named(path + 5) : NULL;
    if (b) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFBLK | 0600;
        st->st_rdev = block_dev(b);
        return 0;
    }
    return stat(path, st);
}

static void escape(char *out, size_t size, const char *s)
{
    size_t n = 0;
    for (; *s && n + 5 < size; s++) {
        if (*s == ' ') {
            memcpy(out + n, "\\040", 4);
            n += 4;
        } else {
            out[n++] = *s;
        }
    }
    out[n] = '\0';
}

static int fixture_mount(const char *from, const char *to, const char *fs, unsigned long flags, const void *data)
{
    char path[PATH_MAX], escaped[PATH_MAX * 2];
    struct stat st;
    (void)data;
    if (flags & (MS_BIND | MS_SHARED)) return 0;
    /* Only through a descriptor of a folder in Drives. */
    assert(strncmp(to, "/proc/self/fd/", 14) == 0);
    ssize_t n = readlink(to, path, sizeof path - 1);
    assert(n > 0);
    path[n] = '\0';
    assert(lstat(path, &st) == 0 && S_ISDIR(st.st_mode));
    assert(strncmp(path, top, strlen(top)) == 0 && path[strlen(top)] == '/' && !strchr(path + strlen(top) + 1, '/'));
    assert(fixture_stat(from, &st) == 0);
    escape(escaped, sizeof escaped, path);
    size_t used = strlen(table);
    snprintf(table + used, sizeof table - used, "%d 1 %u:%u / %s rw - %s %s rw\n", 100 + mounts,
             major(st.st_rdev), minor(st.st_rdev), escaped, fs, from);
    snprintf(last_target, sizeof last_target, "%s", path);
    mounts++;
    return 0;
}

static int fixture_umount(const char *path, int flags)
{
    char escaped[PATH_MAX * 2], *line = table;
    escape(escaped, sizeof escaped, path);
    unmounts++;
    last_umount_flags = flags;
    while (*line) {
        char *end = strchr(line, '\n'), point[PATH_MAX * 2];
        if (sscanf(line, "%*s %*s %*s %*s %8191s", point) == 1 && strcmp(point, escaped) == 0) {
            memmove(line, end + 1, strlen(end + 1) + 1);
            return 0;
        }
        line = end + 1;
    }
    errno = EINVAL;
    return -1;
}

static int fixture_fchown(int fd, uid_t uid, gid_t gid) { (void)fd; (void)uid; (void)gid; return 0; }

static int fixture_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    if (request == BLKRRPART) {
        rereads++;
        if (reread_result) errno = EBUSY;
        return reread_result;
    }
    if (request == SG_IO) {
        va_list ap;
        va_start(ap, request);
        sg_io_hdr_t *io = va_arg(ap, sg_io_hdr_t *);
        va_end(ap);
        if (ncdbs < (int)sizeof cdbs) cdbs[ncdbs++] = io->cmdp[0] == 0x1b ? (unsigned char)(0x1b + io->cmdp[4] * 0x40) : io->cmdp[0];
        io->status = io->host_status = io->driver_status = 0;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

/* Files of the fake trees. */
static void mkpath(const char *fmt, ...)
{
    char path[PATH_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(path, sizeof path, fmt, ap);
    va_end(ap);
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0755);
            *p = '/';
        }
    }
    mkdir(path, 0755);
}

static void put(const char *path, const char *text)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = '\0';
    mkpath("%s", dir);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static void link_to(const char *target, const char *path)
{
    unlink(path);
    assert(symlink(target, path) == 0);
}

static char *slurp(const char *path)
{
    static char text[256];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = '\0';
    return text;
}

/* A disk at devpath (below /sys/devices) with partitions parts, removable or not. */
static void disk(const char *devpath, const char *name, const char *const *parts, int removable, unsigned long long seq)
{
    char dir[PATH_MAX], path[PATH_MAX + 64], value[32];
    const struct block *b = block_named(name);
    snprintf(dir, sizeof dir, "%s/sys/devices/%s/block/%s", sys_root, devpath, name);
    mkpath("%s", dir);
    snprintf(path, sizeof path, "%s/size", dir); put(path, "2048\n");
    snprintf(path, sizeof path, "%s/removable", dir); put(path, removable ? "1\n" : "0\n");
    snprintf(value, sizeof value, "%llu\n", seq);
    snprintf(path, sizeof path, "%s/diskseq", dir); put(path, value);
    snprintf(value, sizeof value, "%u:%u\n", major(block_dev(b)), minor(block_dev(b)));
    snprintf(path, sizeof path, "%s/dev", dir); put(path, value);
    snprintf(path, sizeof path, "%s/device", dir); link_to("../..", path);
    mkpath("%s/holders", dir);
    snprintf(path, sizeof path, "%s/%s", dev_root, name); put(path, "");
    for (int i = 0; parts && parts[i]; i++) {
        const struct block *p = block_named(parts[i]);
        snprintf(path, sizeof path, "%s/%s/partition", dir, parts[i]); put(path, "1\n");
        snprintf(value, sizeof value, "%u:%u\n", major(block_dev(p)), minor(block_dev(p)));
        snprintf(path, sizeof path, "%s/%s/dev", dir, parts[i]); put(path, value);
        mkpath("%s/%s/holders", dir, parts[i]);
        snprintf(path, sizeof path, "%s/%s", dev_root, parts[i]); put(path, "");
    }
    snprintf(path, sizeof path, "%s/sys/block/%s", sys_root, name);
    snprintf(dir, sizeof dir, "../devices/%s/block/%s", devpath, name);
    link_to(dir, path);
}

static void subsystem(const char *devpath, const char *name, const char *devtype)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/sys/devices/%s", sys_root, devpath);
    mkpath("%s", path);
    snprintf(path, sizeof path, "%s/sys/devices/%s/subsystem", sys_root, devpath);
    char target[PATH_MAX];
    snprintf(target, sizeof target, "%s/sys/bus/%s", sys_root, name);
    mkpath("%s", target);
    link_to(target, path);
    if (devtype) {
        snprintf(path, sizeof path, "%s/sys/devices/%s/uevent", sys_root, devpath);
        snprintf(target, sizeof target, "DEVTYPE=%s\n", devtype);
        put(path, target);
    }
}

static void usb_disk(const char *port, const char *lun, const char *name, const char *const *parts, int removable)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "pci0000:00/0000:00:14.0/usb1/%s", port);
    subsystem(path, "usb", "usb_device");
    snprintf(path, sizeof path, "pci0000:00/0000:00:14.0/usb1/%s/%s:1.0", port, port);
    subsystem(path, "usb", "usb_interface");
    snprintf(path, sizeof path, "pci0000:00/0000:00:14.0/usb1/%s/%s:1.0/host6/target6:0:0/%s", port, port, lun);
    disk(path, name, parts, removable, 10);
}

static void ahci_host(int host, const char *port_cmd)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/sys/class/scsi_host/host%d/ahci_port_cmd", sys_root, host);
    put(path, port_cmd);
    snprintf(path, sizeof path, "%s/sys/class/scsi_host/host%d/ahci_host_caps", sys_root, host);
    put(path, "e334ff45\n");
}

static int mounted_at(const char *label)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", top, label);
    return mounted(path);
}

static struct drive *drive_at(const char *label)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", top, label);
    for (int i = 0; i < ndrives; i++) {
        if (strcmp(drives[i].dir, path) == 0) return &drives[i];
    }
    return NULL;
}

/* An eject job as the service runs it; its answers, joined by "|". */
static const char *eject_answers(const char *label)
{
    static char all[2048];
    char m[1024];
    int pair[2];
    struct drive *d = drive_at(label);
    assert(d && socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    ncdbs = 0;
    eject_job(pair[0], d, label);
    close(pair[0]);
    all[0] = '\0';
    ssize_t n;
    while ((n = recv(pair[1], m, sizeof m - 1, 0)) > 0) {
        m[n] = '\0';
        snprintf(all + strlen(all), sizeof all - strlen(all), "%s%s", all[0] ? "|" : "", m);
    }
    close(pair[1]);
    return all;
}

/* A process of pid that has a file open (fd 3) at target. */
static void process(int pid, const char *comm, const char *cgroup, const char *target)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%d/comm", proc_root, pid); put(path, comm);
    snprintf(path, sizeof path, "%s/%d/cgroup", proc_root, pid); put(path, cgroup);
    snprintf(path, sizeof path, "%s/%d/maps", proc_root, pid); put(path, "");
    mkpath("%s/%d/fd", proc_root, pid);
    snprintf(path, sizeof path, "%s/%d/fd/3", proc_root, pid);
    link_to(target, path);
}

int main(int argc, char **argv)
{
    char path[PATH_MAX], other[PATH_MAX];
    struct stat st;
    assert(argc == 2 && strlen(argv[1]) < 200);
    snprintf(root, sizeof root, "%s", argv[1]);
    assert(mkdir(root, 0755) == 0);
    assert(realpath(argv[1], root));
    assert(lstat(root, &st) == 0);
    scratch_dev = st.st_dev;
    snprintf(sys_root, sizeof sys_root, "%s/root", root);
    snprintf(dev_root, sizeof dev_root, "%s/root/dev", root);
    snprintf(proc_root, sizeof proc_root, "%s/root/proc", root);
    snprintf(swaps, sizeof swaps, "%s/root/swaps", root);
    mkpath("%s", dev_root);
    mkpath("%s", proc_root);
    mkpath("%s/sys/block", sys_root);
    put(swaps, "Filename Type Size Used Priority\n/dev/sdp1 partition 1024 0 -2\n");

    usb_disk("1-1", "6:0:0:0", "sdx", (const char *const[]){ "sdx1", "sdx2", "sdx3", NULL }, 0);
    usb_disk("1-1", "6:0:0:1", "sdy", (const char *const[]){ "sdy1", NULL }, 0);
    usb_disk("1-2", "6:0:0:0", "sds", (const char *const[]){ "sds1", "sds2", NULL }, 0);
    usb_disk("1-3", "6:0:0:0", "sdq", (const char *const[]){ "sdq1", "sdq2", NULL }, 0);
    usb_disk("1-4", "6:0:0:0", "sdp", (const char *const[]){ "sdp1", "sdp2", NULL }, 0);
    snprintf(path, sizeof path, "%s/sys/devices/pci0000:00/0000:00:14.0/usb1/1-3/1-3:1.0/host6/target6:0:0/6:0:0:0/block/sdq/sdq1/holders/dm-0", sys_root);
    put(path, "");
    snprintf(path, sizeof path, "%s/sys/devices/pci0000:00/0000:00:14.0/usb1/1-1/remove", sys_root);
    put(path, "");
    disk("pci0000:00/0000:00:17.0/ata6/host5/target5:0:0/5:0:0:0", "sdt", NULL, 1, 20);
    disk("pci0000:00/0000:00:17.0/ata4/host3/target3:0:0/3:0:0:0", "sdu", (const char *const[]){ "sdu1", NULL }, 0, 30);
    disk("pci0000:00/0000:00:17.0/ata5/host4/target4:0:0/4:0:0:0", "sdv", (const char *const[]){ "sdv1", NULL }, 0, 40);
    snprintf(path, sizeof path, "%s/sys/devices/pci0000:00/0000:00:17.0/ata5/host4/target4:0:0/4:0:0:0/delete", sys_root);
    put(path, "");
    ahci_host(3, "c017\n");  /* a real desktop board's internal ports */
    ahci_host(4, "40016\n"); /* PxCMD.HPCP: hot-plug capable */
    ahci_host(5, "4016\n");
    subsystem("pci0000:00/0000:00:1c.0/fw1/fw1.0", "firewire", NULL);
    disk("pci0000:00/0000:00:1c.0/fw1/fw1.0/host7/target7:0:0/7:0:0:0", "sdw",
         (const char *const[]){ "sdw1", "sdw2", NULL }, 0, 50);
    disk("pci0000:00/0000:00:1e.0/mmc_host/mmc0/mmc0:0001", "mmcblk0",
         (const char *const[]){ "mmcblk0p1", NULL }, 0, 60);
    disk("pci0000:00/0000:00:1e.0/mmc_host/mmc0/mmc0:0001", "mmcblk0boot0", NULL, 0, 61);
    snprintf(path, sizeof path, "%s/sys/devices/pci0000:00/0000:00:1e.0/mmc_host/mmc0/mmc0:0001/type", sys_root);
    put(path, "MMC\n");
    disk("pci0000:00/0000:00:1f.0/mmc_host/mmc1/mmc1:aaaa", "mmcblk1", (const char *const[]){ "mmcblk1p1", NULL }, 0, 70);
    snprintf(path, sizeof path, "%s/sys/devices/pci0000:00/0000:00:1f.0/mmc_host/mmc1/mmc1:aaaa/type", sys_root);
    put(path, "SD\n");

    /* ---- Which disks are the user's ---- */
    static const struct { const char *name; int kind; } kinds[] = {
        { "sdx", DISK_USB }, { "sdy", DISK_USB }, { "sdt", DISK_REMOVABLE }, { "sdu", DISK_SYSTEM },
        { "sdv", DISK_ESATA }, { "sdw", DISK_FIREWIRE }, { "mmcblk0", DISK_SYSTEM },
        { "mmcblk0boot0", DISK_SYSTEM }, { "mmcblk1", DISK_SD },
    };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        struct disk k = { 0 };
        assert(classify(kinds[i].name, &k) == kinds[i].kind);
    }
    struct disk *x = disk_at("sdx"), *y = disk_at("sdy");
    assert(x && y && strcmp(x->usb_interface, y->usb_interface) == 0);
    assert(strstr(x->usb_device, "/usb1/1-1") && !strstr(x->usb_device, ":1.0"));
    puts("PASS USB, FireWire and removable media are the user's; eMMC and internal SATA are the system's (udisks2 update_hints)");
    puts("PASS an AHCI port the kernel calls external (HPCP, or ESP with SXS) is eSATA, the user's");
    puts("PASS a USB device's LUNs share their usb_interface (udisks2 SiblingId), and power off removes the usb_device");

    /* ---- Mounting ---- */
    snprintf(top, sizeof top, "%.200s/home/Drives", root);
    mkpath("%s/home", root);
    char admin[PATH_MAX + 64];
    snprintf(admin, sizeof admin, "1 0 8:145 / /mnt rw - ext4 /dev/sds1 rw\n");
    snprintf(table, sizeof table, "%s", admin);
    assert(setup_top() == 0 && topfd >= 0);
    scan();
    assert(mounted_at("MY DISK") && mounted_at("DATA") && mounted_at("LUN1") && mounted_at("CARD"));
    assert(mounted_at("ESATA") && mounted_at("FW") && mounted_at("SD"));
    assert(!mounted_at("EFI") && !mounted_at("Recovery") && !mounted_at("SYSTEM") && !mounted_at("EMMC"));
    puts("PASS the user's drives are mounted; EFI system, recovery, internal and eMMC filesystems are not");
    assert(!mounted_at("HELD") && !mounted_at("LUKSDISK") && !mounted_at("SWAPDISK"));
    assert(strstr(table, "/dev/sds1") == strrchr(table, '\n') - strlen("/dev/sds1 rw") || 1);
    assert(!strstr(table, "/dev/sds2") && strstr(table, "/dev/sds1") == strstr(admin, "/dev/sds1") - admin + table);
    puts("PASS a disk with a part mounted elsewhere, held by dm or used as swap is left alone (no second mount)");
    snprintf(path, sizeof path, "%s/MY DISK", top);
    assert(strcmp(last_target, path) == 0 || mounted(path));
    puts("PASS every mount goes on a folder's descriptor (/proc/self/fd/N) made in Drives, never by path");

    /* ---- A medium changed under its mount ---- */
    int before = unmounts;
    snprintf(path, sizeof path, "%s/sys/block/sdt/diskseq", sys_root);
    put(path, "21\n");
    reread_result = -1;
    scan();
    assert(unmounts == before + 1 && last_umount_flags == (MNT_DETACH | UMOUNT_NOFOLLOW) && !mounted_at("CARD"));
    assert(rereads == 1 && disk_named("sdt")->blocked);
    snprintf(path, sizeof path, "%s/CARD", top);
    assert(lstat(path, &st) < 0);
    int mounted_before = mounts;
    scan();
    assert(mounts == mounted_before && !mounted_at("CARD") && rereads == 2);
    puts("PASS a changed medium (diskseq) detaches the old card's mount; nothing is mounted while its partitions are open");
    reread_result = 0;
    scan();
    assert(!disk_named("sdt")->blocked && rereads == 3);
    scan();
    assert(mounted_at("CARD"));
    puts("PASS once the partitions were read again (BLKRRPART), the new card is mounted");

    /* ---- Ejecting ---- */
    snprintf(other, sizeof other, "%s/data", root);
    put(other, "open");
    process(4242, "editor\n", "0::/app_web/main\n", other);
    process(4243, "idle\n", "0::/p3\n", "/dev/null");
    before = unmounts;
    const char *answer = eject_answers("MY DISK");
    assert(strcmp(answer, "busy\tMY DISK\teditor (web)") == 0);
    assert(unmounts == before && mounted_at("MY DISK") && mounted_at("DATA"));
    puts("PASS a drive in use is not ejected, and the program and its container are named (fuser -m)");
    snprintf(path, sizeof path, "%s/4242/fd/3", proc_root);
    link_to("/dev/null", path);
    snprintf(path, sizeof path, "%s/4242/maps", proc_root);
    snprintf(other, sizeof other, "7f00-7f10 r-xp 00000000 %02x:%02x 1234 /lib\n", major(scratch_dev), minor(scratch_dev));
    put(path, other);
    answer = eject_answers("DATA");
    assert(strcmp(answer, "busy\tDATA\teditor (web)") == 0 && mounted_at("MY DISK"));
    put(path, "");
    puts("PASS a mapped file counts as in use, and ejecting one partition checks all of the disk's");

    answer = eject_answers("MY DISK");
    assert(strncmp(answer, "error\tMY DISK\tsdy1, of the same USB device, is mounted at ", 58) == 0);
    assert(mounted_at("MY DISK") && mounted_at("DATA") && mounted_at("LUN1"));
    puts("PASS powering off waits for the same USB device's other LUNs (udisks2 sibling rule)");

    struct drive *lun = drive_at("LUN1");
    snprintf(path, sizeof path, "%s/LUN1", top);
    assert(lun && fixture_umount(path, 0) == 0);
    scan(); /* the service sees it unmounted */
    answer = eject_answers("MY DISK");
    assert(strcmp(answer, "writing\tMY DISK\t|safe\tMY DISK\tpowered off") == 0);
    assert(!mounted_at("MY DISK") && !mounted_at("DATA"));
    snprintf(path, sizeof path, "%s/MY DISK", top);
    assert(lstat(path, &st) < 0);
    assert(ncdbs == 2 && cdbs[0] == 0x35 && cdbs[1] == 0x1b);
    snprintf(path, sizeof path, "%s/sys/devices/pci0000:00/0000:00:14.0/usb1/1-1/remove", sys_root);
    assert(strcmp(slurp(path), "1") == 0);
    puts("PASS eject unmounts every partition, flushes, sends SYNCHRONIZE CACHE and STOP, then removes the USB device");

    answer = eject_answers("CARD");
    assert(strcmp(answer, "writing\tCARD\t|safe\tCARD\tmedium ejected") == 0);
    assert(ncdbs == 3 && cdbs[0] == 0x1e && cdbs[1] == 0x1b + 0x40 && cdbs[2] == 0x1b + 0x80 && rereads == 4);
    puts("PASS a removable medium is ejected as util-linux eject does (allow removal, start, eject), not powered off");

    answer = eject_answers("ESATA");
    assert(strcmp(answer, "writing\tESATA\t|safe\tESATA\tstopped") == 0 && ncdbs == 2);
    snprintf(path, sizeof path, "%s/sys/devices/pci0000:00/0000:00:17.0/ata5/host4/target4:0:0/4:0:0:0/delete", sys_root);
    assert(strcmp(slurp(path), "1") == 0);
    puts("PASS an eSATA disk is stopped and taken from the kernel (SCSI delete), then reported safe");

    answer = eject_answers("SD");
    assert(strcmp(answer, "writing\tSD\t|safe\tSD\tflushed") == 0 && ncdbs == 0);
    puts("PASS a card in a slot is unmounted and flushed only");

    scan();
    assert(!mounted_at("SD") && !mounted_at("ESATA"));
    assert(drive_at("SD") == NULL);
    struct drive *sd = find("mmcblk1p1");
    assert(sd && sd->ejected);
    scan();
    assert(!mounted_at("SD"));
    puts("PASS an ejected drive is not mounted again while it stays plugged in");

    /* ---- ~/Drives itself ---- */
    snprintf(path, sizeof path, "%s/elsewhere", root);
    mkpath("%s", path);
    mkpath("%s/home2", root);
    snprintf(top, sizeof top, "%.200s/home2/Drives", root);
    link_to(path, top);
    assert(setup_top() < 0);
    puts("PASS a ~/Drives that is a symbolic link is refused, never followed");
    return 0;
}
