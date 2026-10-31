/* The real drives lifecycle with a mount table and block metadata fixtures.
 * No mount, unmount, device access or ownership change reaches the host. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dirent.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
static FILE *fixture_fopen(const char *, const char *);
static DIR *fixture_opendir(const char *);
static struct passwd *fixture_user(uid_t);
static int fixture_stat(const char *, struct stat *);
static int fixture_mount(const char *, const char *, const char *, unsigned long, const void *);
static int fixture_umount(const char *, int);
static int fixture_mkdir(const char *, mode_t);
static int fixture_rmdir(const char *);
static int fixture_chown(const char *, uid_t, gid_t);
static unsigned int fixture_sleep(unsigned int);
#define fopen fixture_fopen
#define opendir fixture_opendir
#define getpwuid fixture_user
#define stat(path, st) fixture_stat(path, st)
#define mount fixture_mount
#define umount2 fixture_umount
#define mkdir fixture_mkdir
#define rmdir fixture_rmdir
#define chown fixture_chown
#define sleep fixture_sleep
#define main drives_main
#include "../progs/drives.c"
#undef main
#undef fopen
#undef opendir
#undef getpwuid
#undef stat
#undef mount
#undef umount2
#undef mkdir
#undef rmdir
#undef chown
#undef sleep

static char scratch[200], mount_table[16384];
static int unmounts, removals, mounts;
static unsigned long bind_flags;
static struct passwd user;
void say(const char *fmt, ...) { (void)fmt; }
static FILE *fixture_fopen(const char *path, const char *mode)
{
    if (strcmp(path, "/proc/self/mountinfo") == 0) return fmemopen(mount_table, strlen(mount_table), "r");
    return fopen(path, mode);
}
static DIR *fixture_opendir(const char *path)
{
    if (strcmp(path, "/sys/block") == 0) return NULL;
    return opendir(path);
}
static struct passwd *fixture_user(uid_t uid)
{
    assert(uid == USER);
    user.pw_dir = scratch;
    return &user;
}
static int fixture_stat(const char *path, struct stat *st)
{
    if (strcmp(path, "/dev/sdb1") == 0) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFBLK | 0600;
        st->st_rdev = makedev(8, 17);
        return 0;
    }
    if (strstr(path, "/Cloud")) {
        memset(st, 0, sizeof *st);
        st->st_mode = S_IFDIR | 0755;
        st->st_dev = makedev(0, 77);
        return 0;
    }
    return stat(path, st);
}
static int fixture_mount(const char *from, const char *to, const char *fs, unsigned long flags, const void *data)
{
    (void)from; (void)to; (void)fs; (void)data;
    mounts++;
    if (flags & MS_BIND) bind_flags = flags;
    return 0;
}
static int fixture_umount(const char *path, int flags)
{
    (void)path; (void)flags;
    unmounts++;
    return 0;
}
static int fixture_mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
static int fixture_rmdir(const char *path) { (void)path; removals++; return 0; }
static int fixture_chown(const char *path, uid_t uid, gid_t gid)
{ (void)path; (void)uid; (void)gid; return 0; }
static unsigned int fixture_sleep(unsigned int seconds) { (void)seconds; stop = 1; return 0; }
static void table(const char *text) { snprintf(mount_table, sizeof mount_table, "%s", text); }
static void reset(void)
{
    ndrives = stop = unmounts = removals = mounts = 0;
    bind_flags = 0;
    memset(drives, 0, sizeof drives);
}
int main(int argc, char **argv)
{
    assert(argc == 2 && strlen(argv[1]) + 32 < sizeof scratch);
    snprintf(scratch, sizeof scratch, "%s", argv[1]);
    assert(mkdir(scratch, 0700) == 0);
    char path[1024];
    snprintf(path, sizeof path, "%s/Drives", scratch); assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof path, "%s/Drives/Cloud", scratch); assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof path, "%s/Drives/Empty", scratch); assert(mkdir(path, 0700) == 0);
    snprintf(mount_table, sizeof mount_table,
             "1 0 0:1 / / rw - tmpfs root rw\n2 1 0:77 / %s/Drives/Cloud rw - fuse.rclone cloud: rw\n", scratch);
    assert(drives_main() == 0 && unmounts == 0 && removals == 0);
    assert(mounts == 2 && bind_flags == (MS_BIND | MS_REC));
    puts("PASS startup preserves cloud mounts and empty user folders; recursive bind retains child mounts");

    snprintf(path, sizeof path, "%s/Drives/Cloud", scratch);
    assert(mounted(path));
    assert(!mounted("/not/a/mount"));
    puts("PASS mount membership uses the exact mount table rather than probing remote directories");

    reset();
    snprintf(top, sizeof top, "/home/user/Drives");
    struct drive d = { .dev = "sdb1" };
    table("20 1 8:17 / /home/user/Drives/USB\\040drive rw - ext4 /dev/sdb1 rw\n");
    assert(adopt_mount(&d) && !d.owned && strcmp(d.dir, "/home/user/Drives/USB drive") == 0);
    assert(d.device == makedev(8, 17) && mounted_device(&d));
    puts("PASS an existing removable block mount is adopted without unmounting or renaming it");

    table("20 1 8:17 / /home/user/Drives/USB\\040drive rw - ext4 /dev/sdb1 rw\n"
          "21 20 0:77 / /home/user/Drives/USB\\040drive rw - fuse.rclone cloud: rw\n");
    assert(!mounted_device(&d));
    d.dir[0] = '\0';
    assert(!adopt_mount(&d));
    puts("PASS an overmounted cloud path is not adopted as the hidden block mount");

    static const char *const unrelated[] = {
        "20 1 0:77 / /home/user/Drives/Cloud rw - fuse.rclone cloud: rw\n",
        "20 1 8:18 / /home/user/Drives/USB rw - ext4 /dev/sdb1 rw\n",
        "20 1 8:17 / /home/user/Drives/USB rw - ext4 /dev/sdc1 rw\n",
        "20 1 8:17 /sub /home/user/Drives/USB rw - ext4 /dev/sdb1 rw\n",
        "20 1 8:17 / /home/user/Drives-other/USB rw - ext4 /dev/sdb1 rw\n",
        "20 1 8:17 / /home/user/Drives/USB/nested rw - ext4 /dev/sdb1 rw\n",
    };
    for (size_t i = 0; i < sizeof unrelated / sizeof unrelated[0]; i++) {
        d.dir[0] = '\0';
        table(unrelated[i]);
        assert(!adopt_mount(&d));
    }
    puts("PASS cloud, wrong device identity, bind subtree and paths outside direct drive roots stay unrelated");

    reset();
    drives[0] = (struct drive){ .dev = "sdb1", .device = makedev(8, 17), .owned = 1 };
    snprintf(drives[0].dir, sizeof drives[0].dir, "/home/user/Drives/USB");
    ndrives = 1;
    table("20 1 0:77 / /home/user/Drives/USB rw - fuse.rclone cloud: rw\n");
    scan();
    assert(ndrives == 0 && unmounts == 0 && removals == 0);
    puts("PASS unplug cleanup does not unmount a replacement cloud filesystem");
    return 0;
}
