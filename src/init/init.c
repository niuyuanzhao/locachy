/*
 * /init, process 1 of LOCachy: the switcher, on a distribution's root. The
 * kernel starts it from our initramfs. It turns on the swap labelled
 * locachy-swap, and makes the ext4 file system labelled locachy-root, an
 * Arch-family root, the root of everything: / for itself and all it starts.
 * It loads the drivers for the devices there are from /lib/modules (and keeps
 * doing so for devices that appear), mounts the ESP at /efi, and starts
 * /usr/local/bin/home, which stays at the bottom of everything. It keeps the
 * kernel's log in /var/log/locachy. The system's one startup file is
 * /etc/locachy/rc.conf (base.h).
 *
 * Services, the service_NAME lines of rc.conf (shell commands), run beside
 * all that: never frozen, without the screen or the keyboard, started again
 * 5 s after they exit. SIGHUP reads rc.conf again (settings sends it after
 * saving): a service whose line has gone is ended, a new one is started, the
 * others keep running. SIGTERM and Ctrl+Alt+Del reboot, SIGUSR2 powers off
 * (as busybox's init); both end everything first and leave / read-only.
 *
 * Home opens tasks. A task is a stack of programs: the one on top runs, the
 * ones below wait in base_call for it to return. One program runs at a time
 * and has the screen (a DRM lease) and the keyboard (evdev fds); every other
 * one, home included, is frozen with its memory pushed out to swap.
 * The shared compositor of the containers' windows (labwc) runs under
 * /usr/local/bin/locachy-seat, which implements the seatd protocol on a
 * private socket and translates take/release to seat activation/deactivation.
 *   open ARG...   (from home) home is frozen; the task ARG... is thawed at
 *                 its top, or started.
 *   kill ARG...   (from home) the task ARG... is killed and cleaned up.
 *   call ARG...   (from the top of a task) it is frozen; ARG... starts on top.
 *   a program exits: the one below is thawed and told "returned NAME
 *                 STATUS"; if there is none, the task ends and home is told
 *                 "ended TASK STATUS".
 *   ThinkVantage / Super+Esc, which process 1 always sees on its own fds: the
 *                 task is frozen as it is, and home is told "back TASK".
 *   switch next|prev  (from the program on the screen: a touchpad swipe) the
 *                 next or previous of home and the running tasks, in turn.
 *   lock          (from the program on the screen; key_lock, which process 1
 *                 also sees on its own fds; the lid closing; at boot unless
 *                 rc.conf has login_skip="1"; rc.conf's lock_after minutes
 *                 without input: process 1 reads every input device for
 *                 that, but on the shared display labwc knows, and
 *                 labwc-control asks) /usr/local/bin/lock on top of
 *                 everything, what had the screen waiting as for home; while
 *                 it is there nothing else gets the screen and the home and
 *                 switch requests do nothing. It exiting 0 (someone
 *                 authenticated, through PAM) unlocks; any other end starts it
 *                 again.
 * Taking the screen and keyboard from a program: it is asked to release
 * them, and both are revoked anyway.
 *
 * Process 1 must never exit, so every error is logged and it carries on.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <signal.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"
#include "apps.h"

#define FG    0xD0D0D0
#define BG    0x000000
#define MAX_TASKS 8
#define MAX_DEPTH 6
#define MAX_KBD 8
#define CGROUP "/sys/fs/cgroup"
#define MIB (1024.0 * 1024.0)

struct frame {
    char name[32];          /* the program and its first argument, for people */
    pid_t pid;              /* 0: empty */
    int ctl;                /* our end of its control socket */
    int reaped, status;     /* waited for already (by reap), and how it ended */
    int frozen;
    int shared_display;     /* release display without freezing shared infrastructure */
    uint32_t lessee;        /* its DRM lease while it has the screen */
    int kbd[MAX_KBD], nkbd; /* the keyboard fds lent to it */
    int *pointer;
    size_t npointer;        /* input devices for its libinput (pointer?) */
    int pointer_wanted;
    char pointer_paths[64][POINTER_PATH];
    char cgroup[48];
};

struct task {
    char key[128];          /* the argv that opened it, joined with spaces */
    struct frame f[MAX_DEPTH];
    int depth;              /* 0: no task in this slot */
};

struct service {
    char command[256];
    pid_t pid;              /* 0: not running */
    double restart_at;      /* when to start it again, ms (now_ms) */
};

static char *home_argv[] = { "/usr/local/bin/home", NULL };
static struct frame home;
static char *lock_argv[] = { "/usr/local/bin/lock", NULL };
static struct frame locker;        /* the lock screen, while locked */
static int locked;
static int lock_shown;             /* it has drawn its first picture ("locked") */
static double lock_start_at = -1;  /* start it again then (ms): it ended, not unlocking */
static int login_skip;             /* rc.conf login_skip="1": not locked at boot */
static int lock_after = 600;       /* rc.conf lock_after (minutes), in s: locked after that
                                    * long without input; 0: never */
static double active_at;           /* the last input (ms) */
#define MAX_ACTIVITY 32
static int *afd;                   /* the input devices, read only to see input */
static size_t nafd;
static struct task tasks[MAX_TASKS];
static struct task *front;  /* the task on screen; NULL: home */
static struct frame *fg;    /* the program that has the screen */
static int next_cgroup;
static char *desktop_argv[] = {
    "/usr/local/bin/locachy-seat", "/usr/bin/labwc", NULL
};
static char swap_dev[64], esp_dev[64];
static char resume_dev[16];       /* the swap as MAJOR:MINOR, where hibernation goes */
static int luks_passphrase_entered; /* 1 when passphrase was entered at boot (0 if TPM auto-unlocked) */
static int lid_fd = -1;
static int lid_wakes_from_disk = 1; /* until the closed lid wakes it from hibernation */
static int hibernate_after = 30; /* seconds asleep with the lid closed (config) */
static struct service *services;
static size_t nservices;
static sigset_t handled;    /* blocked here, read from a signalfd */

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static int write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : write(fd, text, strlen(text));

    if (fd >= 0) {
        close(fd);
    }
    return n < 0 ? -1 : 0;
}

/* The number after key in a file of "key value" lines, or the first number
 * in it if key is NULL; -1 if not found. */
static long long read_value(const char *path, const char *key)
{
    char buf[512];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, buf, sizeof buf - 1);

    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    if (key == NULL) {
        return atoll(buf);
    }
    size_t k = strlen(key);
    for (char *line = buf; line != NULL && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (strncmp(line, key, k) == 0 && line[k] == ' ') {
            return atoll(line + k + 1);
        }
    }
    return -1;
}

static double cg_mib(const struct frame *f, const char *file)
{
    char path[96];

    snprintf(path, sizeof path, "%s/%s", f->cgroup, file);
    long long v = read_value(path, NULL);
    return v < 0 ? 0 : (double)v / MIB;
}

static void mount_kernel_fs(void)
{
    static const struct { const char *what, *where, *type; } fs[] = {
        { "devtmpfs", "/dev", "devtmpfs" },
        { "proc", "/proc", "proc" },
        { "sysfs", "/sys", "sysfs" },
        { "cgroup2", CGROUP, "cgroup2" },
    };
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) {
        if (mount(fs[i].what, fs[i].where, fs[i].type, 0, NULL) < 0) {
            say("init: mount %s: %s", fs[i].where, strerror(errno));
        }
    }
    if (write_file(CGROUP "/cgroup.subtree_control", "+memory") < 0) {
        say("init: cannot enable the memory controller: %s", strerror(errno));
    }
}

static int has_signature(const char *dev, off_t offset, const char *sig)
{
    char buf[16] = { 0 };
    size_t len = strlen(sig);
    int fd = open(dev, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        return 0;
    }
    ssize_t n = pread(fd, buf, len, offset);
    close(fd);
    return n == (ssize_t)len && memcmp(buf, sig, len) == 0;
}

/* Whether the 16-byte label field at offset holds label. */
static int has_label(const char *dev, off_t offset, const char *label)
{
    char buf[17] = { 0 };
    int fd = open(dev, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        return 0;
    }
    ssize_t n = pread(fd, buf, 16, offset);
    close(fd);
    return n == 16 && strcmp(buf, label) == 0;
}

/* Call fn("/dev/NAME") for every block device in /sys/class/block (SATA,
 * NVMe, virtio, MMC, and unlocked dm-crypt mapper devices), falling back to
 * /dev/sd[a-h][1-15] when sysfs is empty. */
static void each_block_dev(int (*fn)(const char *dev))
{
    DIR *d = opendir("/sys/class/block");
    struct dirent *e;
    int seen = 0;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char dev[64];
        snprintf(dev, sizeof dev, "/dev/%.56s", e->d_name);
        if (access(dev, F_OK) == 0) {
            seen = 1;
            if (fn(dev)) {
                closedir(d);
                return;
            }
        }
    }
    if (d) closedir(d);
    if (seen) return;
    for (char c = 'a'; c <= 'h'; c++) {
        for (int part = 0; part <= 15; part++) {
            char dev[16];
            snprintf(dev, sizeof dev, part ? "/dev/sd%c%d" : "/dev/sd%c", c, part);
            if (access(dev, F_OK) != 0) {
                if (part == 0) break;
                continue;
            }
            if (fn(dev)) return;
        }
    }
}

static char luks_root_dev[64], luks_swap_dev[64];
/* With an encrypted root, root, swap and hibernation image are only what it
 * unlocked (its mapper devices), never a file system or swap of that label on
 * another disk: an SD card's would be mounted, or resumed, with the real disk
 * already open. */
static int luks_only;

/* Whether dev (/dev/NAME) is the device-mapper device named name. */
static int is_mapper(const char *dev, const char *name)
{
    char sys[128], buf[64];
    snprintf(sys, sizeof sys, "/sys/class/block/%.64s/dm/name", dev + 5);
    int fd = open(sys, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return strcmp(buf, name) == 0;
}

static void load_early_modules(const char *const *mods)
{
    if (access("/sbin/modprobe", X_OK) != 0) return;
    char *argv[16] = { "modprobe", "-q", "-a" };
    int n = 3;
    for (int i = 0; mods[i] && n < 15; i++) {
        argv[n++] = (char *)mods[i];
    }
    argv[n] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        execv("/sbin/modprobe", argv);
        _exit(127);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

static int boot_dev_ready;

/* Whether the 48-byte LUKS2 label field at offset 24 holds label. */
static int has_luks_label(const char *dev, const char *label)
{
    char buf[49] = { 0 };
    int fd = open(dev, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        return 0;
    }
    ssize_t n = pread(fd, buf, 48, 24);
    close(fd);
    return n == 48 && strcmp(buf, label) == 0;
}

static int check_boot_dev(const char *dev)
{
    if ((has_signature(dev, 1080, "\x53\xEF") && has_label(dev, 1144, "locachy-root")) ||
        (has_signature(dev, 0, "LUKS\xba\xbe") && has_luks_label(dev, "locachy-crypt"))) {
        boot_dev_ready = 1;
        return 1;
    }
    return 0;
}

static void prepare_boot_storage(void)
{
    static const char *const storage_mods[] = {
        "vmd", "nvme", "sdhci_pci", "mmc_block", "virtio_scsi", NULL
    };
    load_early_modules(storage_mods);
    for (int i = 0; i < 30; i++) {
        boot_dev_ready = 0;
        each_block_dev(check_boot_dev);
        if (boot_dev_ready) return;
        usleep(50 * 1000);
    }
}

static int check_luks_dev(const char *dev)
{
    if (has_signature(dev, 0, "LUKS\xba\xbe")) {
        if (!luks_root_dev[0] && has_luks_label(dev, "locachy-crypt")) {
            snprintf(luks_root_dev, sizeof luks_root_dev, "%s", dev);
        } else if (!luks_swap_dev[0] && has_luks_label(dev, "locachy-swap-crypt")) {
            snprintf(luks_swap_dev, sizeof luks_swap_dev, "%s", dev);
        }
    }
    return 0;
}

static void ensure_mapper_nodes(const char *mapper)
{
    char mpath[64], sys[128], devno[32];
    snprintf(mpath, sizeof mpath, "/dev/mapper/%s", mapper);
    for (int i = 0; i < 16; i++) {
        char dm_name[64];
        snprintf(sys, sizeof sys, "/sys/class/block/dm-%d/dm/name", i);
        int fd = open(sys, O_RDONLY | O_CLOEXEC);
        ssize_t n = fd >= 0 ? read(fd, dm_name, sizeof dm_name - 1) : -1;
        if (fd >= 0) close(fd);
        if (n > 0) {
            dm_name[n] = '\0';
            dm_name[strcspn(dm_name, "\r\n")] = '\0';
            if (strcmp(dm_name, mapper) == 0) {
                snprintf(sys, sizeof sys, "/sys/class/block/dm-%d/dev", i);
                fd = open(sys, O_RDONLY | O_CLOEXEC);
                n = fd >= 0 ? read(fd, devno, sizeof devno - 1) : -1;
                if (fd >= 0) close(fd);
                unsigned maj = 0, min = 0;
                if (n > 0 && sscanf(devno, "%u:%u", &maj, &min) == 2) {
                    char dm_node[32];
                    snprintf(dm_node, sizeof dm_node, "/dev/dm-%d", i);
                    struct stat st;
                    if (stat(dm_node, &st) != 0) mknod(dm_node, S_IFBLK | 0600, makedev(maj, min));
                    if (stat(mpath, &st) != 0) mknod(mpath, S_IFBLK | 0600, makedev(maj, min));
                }
            }
        }
    }
}

static void ensure_char_node_from_sysfs(const char *dev_path, const char *sys_dev_path)
{
    struct stat st;
    if (stat(dev_path, &st) == 0) return;
    char devno[32];
    int fd = open(sys_dev_path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, devno, sizeof devno - 1) : -1;
    if (fd >= 0) close(fd);
    unsigned maj = 0, min = 0;
    if (n > 0) {
        devno[n] = '\0';
        if (sscanf(devno, "%u:%u", &maj, &min) == 2) {
            mknod(dev_path, S_IFCHR | 0600, makedev(maj, min));
        }
    }
}

static int run_cryptsetup_token(const char *cryptsetup, const char *dev, const char *mapper)
{
    int status = -1;
    pid_t pid = fork();
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            if (null_fd > 2) close(null_fd);
        }
        execl(cryptsetup, "cryptsetup", "open", "--type", "luks", "--token-only", dev, mapper, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        waitpid(pid, &status, 0);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        ensure_mapper_nodes(mapper);
        return 0;
    }
    return -1;
}

static int run_cryptsetup_open(const char *cryptsetup, const char *dev, const char *mapper, const char *pass)
{
    int p[2], status = -1;
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[1]);
        dup2(p[0], STDIN_FILENO);
        close(p[0]);
        execl(cryptsetup, "cryptsetup", "open", "--type", "luks", "--key-file", "-", dev, mapper, (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    if (pid > 0) {
        size_t len = strlen(pass);
        ssize_t w = write(p[1], pass, len);
        (void)w;
        close(p[1]);
        waitpid(pid, &status, 0);
    } else {
        close(p[1]);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        ensure_mapper_nodes(mapper);
        return 0;
    }
    return -1;
}

/* Unlock LUKS2 partitions labelled locachy-crypt (and locachy-swap-crypt)
 * before hibernation resume and root mounting. Tries hardware-bound TPM 2.0 /
 * TPM 1.2 LUKS2 tokens first (leaving the boot lock screen enabled so the user
 * unlocks with fingerprint/face/password), falling back to the passphrase UI. */
static void unlock_luks(void)
{
    luks_root_dev[0] = luks_swap_dev[0] = '\0';
    each_block_dev(check_luks_dev);
    if (!luks_root_dev[0] && !luks_swap_dev[0]) return;
    const char *cs = access("/sbin/cryptsetup", X_OK) == 0 ? "/sbin/cryptsetup" :
                     access("/usr/bin/cryptsetup", X_OK) == 0 ? "/usr/bin/cryptsetup" : NULL;
    if (!cs) {
        say("init: LUKS2 partition %s found, but cryptsetup is not in the initramfs",
            luks_root_dev[0] ? luks_root_dev : luks_swap_dev);
        return;
    }
    static const char *const luks_mods[] = {
        "dm_mod", "dm_crypt", "algif_skcipher", "aesni_intel", NULL
    };
    load_early_modules(luks_mods);
    mkdir("/dev/mapper", 0755);
    mkdir("/run", 0755);
    mkdir("/run/cryptsetup", 0700);
    ensure_char_node_from_sysfs("/dev/mapper/control", "/sys/class/misc/device-mapper/dev");
    ensure_char_node_from_sysfs("/dev/tpm0", "/sys/class/tpm/tpm0/dev");
    ensure_char_node_from_sysfs("/dev/tpmrm0", "/sys/class/tpmrm/tpmrm0/dev");

    int swap_open = !luks_swap_dev[0] || run_cryptsetup_token(cs, luks_swap_dev, "locachy-swap-crypt") == 0;
    int root_open = !luks_root_dev[0] || run_cryptsetup_token(cs, luks_root_dev, "locachy-crypt") == 0;
    if (swap_open && root_open) {
        say("init: auto-unlocked LUKS2 via TPM (%s); boot lock stays active",
            luks_root_dev[0] ? luks_root_dev : luks_swap_dev);
        return;
    }

    screen_open("/font.psf");
    keys_open();
    char pass[256], stars[68], prompt_line[160];
    for (int attempt = 0; attempt < 5; attempt++) {
        size_t len = 0;
        memset(pass, 0, sizeof pass);
        say("init: waiting for LUKS2 passphrase on %s (attempt %d)",
            luks_root_dev[0] ? luks_root_dev : luks_swap_dev, attempt + 1);
        for (;;) {
            screen_fill(0, 0, screen_cols(), screen_rows(), BG);
            screen_text(2, 2, "LOCachy — Full Disk Encryption (LUKS2)", FG, BG);
            snprintf(prompt_line, sizeof prompt_line, "Partition: %s%s%s",
                     luks_root_dev[0] ? luks_root_dev : "",
                     luks_root_dev[0] && luks_swap_dev[0] ? ", " : "",
                     luks_swap_dev[0] ? luks_swap_dev : "");
            screen_text(2, 4, prompt_line, FG, BG);
            if (attempt > 0) {
                screen_text(2, 6, "Wrong passphrase; try again.", 0xCC4444, BG);
            }
            size_t nstars = 0; /* a star for each character */
            for (size_t i = 0; i < len && nstars < sizeof stars - 2; i++) {
                nstars += ((unsigned char)pass[i] & 0xC0) != 0x80;
            }
            memset(stars, '*', nstars);
            stars[nstars] = '_';
            stars[nstars + 1] = '\0';
            snprintf(prompt_line, sizeof prompt_line, "Passphrase: %s", stars);
            screen_text(2, 8, prompt_line, FG, BG);
            screen_flush();
            int k = key_get();
            if (k == '\n') break;
            if (k == 21) { /* Ctrl+U */
                memset(pass, 0, sizeof pass);
                len = 0;
            } else if ((k == '\b' || k == K_DEL) && len > 0) {
                while (len > 1 && ((unsigned char)pass[len - 1] & 0xC0) == 0x80) pass[--len] = '\0';
                pass[--len] = '\0'; /* a whole character */
            } else if (keys_typed(k) && len + strlen(keys_typed(k)) < sizeof pass) {
                memcpy(pass + len, keys_typed(k), strlen(keys_typed(k)));
                len += strlen(keys_typed(k));
                pass[len] = '\0';
            }
        }
        if (!swap_open && run_cryptsetup_open(cs, luks_swap_dev, "locachy-swap-crypt", pass) == 0) swap_open = 1;
        if (!root_open && run_cryptsetup_open(cs, luks_root_dev, "locachy-crypt", pass) == 0) root_open = 1;
        memset(pass, 0, sizeof pass);
        if (swap_open && root_open) {
            luks_passphrase_entered = 1;
            say("init: unlocked LUKS2 %s", luks_root_dev[0] ? luks_root_dev : luks_swap_dev);
            keys_release();
            screen_release();
            return;
        }
    }
    say("init: failed to unlock LUKS2 after 5 attempts");
    keys_release();
    screen_release();
}

/*
 * Coming back from hibernation. The kernel wrote the image to the swap
 * labelled locachy-swap; writing that device to /sys/power/resume makes the
 * kernel load it and continue the hibernated system where it stopped, this
 * process included: the write does not return. Done first, before anything
 * is mounted or any module loaded (the disk drivers are built in), so coming
 * back costs only reading the image. With no image the write returns, and
 * the device stays the one hibernation writes to. The swap's signature, at
 * the end of its first page, is S1SUSPEND while it holds an image (the
 * kernel puts SWAPSPACE2 back when it reads it), SWAPSPACE2 otherwise.
 */
static int check_resume_dev(const char *dev)
{
    struct stat st;
    if (luks_only && !is_mapper(dev, "locachy-swap-crypt")) {
        return 0;
    }
    int image = has_signature(dev, 4096 - 10, "S1SUSPEND");
    if ((image || has_signature(dev, 4096 - 10, "SWAPSPACE2")) && has_label(dev, 1052, "locachy-swap") &&
        stat(dev, &st) == 0) {
        snprintf(resume_dev, sizeof resume_dev, "%u:%u", major(st.st_rdev), minor(st.st_rdev));
        say("init: %s: %s", dev, image ? "resuming from the hibernation image there" : "no hibernation image");
        if (write_file("/sys/power/resume", resume_dev) < 0) {
            say("init: /sys/power/resume %s: %s", resume_dev, strerror(errno));
        }
        return 1;
    }
    return 0;
}

static void resume(void)
{
    each_block_dev(check_resume_dev);
}

/* Our partitions are found by their labels, so nothing else on the machine
 * is touched: the swap labelled locachy-swap (mkswap -L, swaplabel), turned on,
 * and the ext4 file system labelled locachy-root, mounted read-write at /mnt.
 * The first FAT32 file system is noted as the ESP (mounted later, if it has
 * an EFI directory). */
static char root_data_dev[64];

static int check_disk_dev(const char *dev)
{
    /* swap header: label at 1052, signature at the end of the first page;
     * ext4: magic at 1080, label at 1144; FAT32: its type at 82 */
    if (!esp_dev[0] && has_signature(dev, 82, "FAT32   ")) {
        snprintf(esp_dev, sizeof esp_dev, "%s", dev);
        return 0;
    }
    if (!swap_dev[0] && has_signature(dev, 4096 - 10, "SWAPSPACE2") &&
        has_label(dev, 1052, "locachy-swap") && (!luks_only || is_mapper(dev, "locachy-swap-crypt"))) {
        if (swapon(dev, 0) == 0) {
            snprintf(swap_dev, sizeof swap_dev, "%s", dev);
        } else {
            say("init: swapon %s: %s", dev, strerror(errno));
        }
    } else if (!root_data_dev[0] && has_signature(dev, 1080, "\x53\xEF") &&
               has_label(dev, 1144, "locachy-root") && (!luks_only || is_mapper(dev, "locachy-crypt"))) {
        if (mount(dev, "/mnt", "ext4", 0, NULL) == 0) {
            snprintf(root_data_dev, sizeof root_data_dev, "%s", dev);
        } else {
            say("init: mount %s: %s", dev, strerror(errno));
        }
    }
    return 0;
}

static const char *disks(void)
{
    root_data_dev[0] = '\0';
    each_block_dev(check_disk_dev);
    return root_data_dev[0] ? root_data_dev : NULL;
}

/* Make the root mounted at /mnt the root of everything, as switch_root
 * does: /dev, /proc and /sys (the cgroup file system with it) move there,
 * and it becomes / for us and all we start; the initramfs stays underneath,
 * unused. Then what the distribution's programs expect: the file systems
 * systemd would mount, and /dev/fd and friends (devtmpfs has none). */
static int switch_root(void)
{
    static const char *const moved[] = { "/dev", "/proc", "/sys" };
    static const struct {
        const char *what, *where, *type, *options;
        unsigned long flags;
    } fs[] = {
        { "devpts", "/dev/pts", "devpts", "mode=0620,gid=5", MS_NOSUID | MS_NOEXEC },
        { "shm", "/dev/shm", "tmpfs", "mode=1777", MS_NOSUID | MS_NODEV },
        { "run", "/run", "tmpfs", "mode=0755", MS_NOSUID | MS_NODEV },
        { "tmp", "/tmp", "tmpfs", "mode=1777", MS_NOSUID | MS_NODEV },
        { "efivarfs", "/sys/firmware/efi/efivars", "efivarfs", NULL, MS_NOSUID | MS_NODEV | MS_NOEXEC },
    };
    static const char *const links[][2] = {
        { "/proc/self/fd", "/dev/fd" },
        { "/proc/self/fd/0", "/dev/stdin" },
        { "/proc/self/fd/1", "/dev/stdout" },
        { "/proc/self/fd/2", "/dev/stderr" },
    };
    char to[16];

    for (size_t i = 0; i < sizeof moved / sizeof moved[0]; i++) {
        snprintf(to, sizeof to, "/mnt%s", moved[i]);
        if (mount(moved[i], to, NULL, MS_MOVE, NULL) < 0) {
            say("init: move %s to the root: %s", moved[i], strerror(errno));
            return -1;
        }
    }
    if (chdir("/mnt") < 0 || mount(".", "/", NULL, MS_MOVE, NULL) < 0 || chroot(".") < 0 || chdir("/") < 0) {
        say("init: cannot switch to the root: %s", strerror(errno));
        return -1;
    }
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) {
        if (strcmp(fs[i].type, "efivarfs") == 0 && access("/sys/firmware/efi", F_OK) < 0) {
            continue; /* not started by UEFI */
        }
        mkdir(fs[i].where, 0755);
        if (mount(fs[i].what, fs[i].where, fs[i].type, fs[i].flags, fs[i].options) < 0) {
            say("init: mount %s: %s", fs[i].where, strerror(errno));
        }
    }
    for (size_t i = 0; i < sizeof links / sizeof links[0]; i++) {
        symlink(links[i][0], links[i][1]);
    }
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct ifreq ifr = { .ifr_name = "lo", .ifr_flags = IFF_UP };
    if (s >= 0) {
        ioctl(s, SIOCSIFFLAGS, &ifr);
        close(s);
    }
    return 0;
}

/* The ESP at /efi, once the drivers (vfat) are there. */
static void esp_mount(void)
{
    if (esp_dev[0]) {
        mkdir("/efi", 0755);
        if (mount(esp_dev, "/efi", "vfat", MS_NOSUID | MS_NODEV, "umask=0077") < 0 || access("/efi/EFI", F_OK) < 0) {
            say("init: %s is not an ESP", esp_dev);
            umount("/efi");
            esp_dev[0] = '\0';
        }
    }
}

/* Keep the kernel's log (ours included: say writes there) as
 * /var/log/locachy/boot-NNN.txt, one file per boot: there is no serial port to
 * watch it. Saved as soon as the root is there (so a boot that hangs later
 * still leaves everything up to /init behind), whenever something goes
 * wrong, and at power off; each save replaces the last. */
static void save_log(void)
{
    static char path[48];

    if (path[0] == '\0') {
        int n = 1;
        mkdir("/var/log/locachy", 0755);
        do {
            snprintf(path, sizeof path, "/var/log/locachy/boot-%03d.txt", n);
        } while (access(path, F_OK) == 0 && ++n < 1000);
    }
    int size = klogctl(10, NULL, 0); /* SYSLOG_ACTION_SIZE_BUFFER */
    char *buf = size > 0 ? malloc((size_t)size) : NULL;
    int len = buf ? klogctl(3, buf, size) : -1; /* SYSLOG_ACTION_READ_ALL */
    int fd = len > 0 ? open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    if (fd < 0 || write(fd, buf, (size_t)len) != len || fsync(fd) < 0) {
        say("init: could not save the log to %s", path);
    } else {
        say("init: saved the log to %s (%d bytes)", path, len);
    }
    if (fd >= 0) {
        close(fd);
    }
    free(buf);
}

static void join(char *const argv[], char *out, size_t size)
{
    size_t len = 0;

    out[0] = '\0';
    for (int i = 0; argv[i] != NULL && len < size; i++) {
        len += (size_t)snprintf(out + len, size - len, i ? " %s" : "%s", argv[i]);
    }
}

/* Start argv (argv[0] a full path) in frame f, in a cgroup of its own, with
 * a control socket at fd 3. */
static int start(struct frame *f, char *const argv[])
{
    int sv[2];
    const char *base = strrchr(argv[0], '/');

    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        say("init: socketpair: %s", strerror(errno));
        return -1;
    }
    memset(f, 0, sizeof *f);
    f->shared_display = strcmp(argv[0], desktop_argv[0]) == 0 &&
                        argv[1] != NULL && strcmp(argv[1], desktop_argv[1]) == 0;
    snprintf(f->name, sizeof f->name, "%.20s%s%.10s", base ? base + 1 : argv[0],
             argv[1] ? " " : "", argv[1] ? argv[1] : "");
    snprintf(f->cgroup, sizeof f->cgroup, CGROUP "/p%d", next_cgroup++);
    mkdir(f->cgroup, 0755);
    pid_t pid = fork();
    if (pid == 0) {
        char procs[64];
        snprintf(procs, sizeof procs, "%s/cgroup.procs", f->cgroup);
        write_file(procs, "0");
        setsid();
        sigprocmask(SIG_UNBLOCK, &handled, NULL);
        if (sv[1] == 3) {
            fcntl(3, F_SETFD, 0); /* keep it across exec */
        } else {
            dup2(sv[1], 3); /* the copy is not close-on-exec */
        }
        setenv("LOCACHY_CONTROL", "3", 1);
        execv(argv[0], argv);
        say("init: cannot run %s: %s", argv[0], strerror(errno));
        _exit(127);
    }
    close(sv[1]);
    if (pid < 0) {
        close(sv[0]);
        rmdir(f->cgroup);
        say("init: fork: %s", strerror(errno));
        return -1;
    }
    f->pid = pid;
    f->ctl = sv[0];
    say("init: started %s (pid %d)", f->name, (int)pid);
    return 0;
}

/* Give f the screen and the keyboard. */
static int display_pending;
static double display_refresh_at = -1;
static double display_retry_until = -1;

static int lend(struct frame *f)
{
    int fds[1 + MAX_KBD];

    fds[0] = screen_lend(&f->lessee);
    if (fds[0] < 0) {
        f->lessee = 0;
        display_pending = 1;
        display_refresh_at = now_ms() + 200;
        display_retry_until = now_ms() + 2000;
        return -1;
    }
    f->nkbd = keys_lend(f->kbd, MAX_KBD);
    memcpy(fds + 1, f->kbd, sizeof(int) * (size_t)f->nkbd);
    if (control_send(f->ctl, "take", 4, fds, 1 + f->nkbd) < 0) {
        say("init: cannot send the screen to %s", f->name);
        close(fds[0]);
        keys_take_back(f->kbd, f->nkbd);
        f->nkbd = 0;
        screen_take_back(f->lessee);
        f->lessee = 0;
        return -1;
    }
    close(fds[0]); /* the program holds the lease now */
    fg = f;
    display_pending = 0;
    display_refresh_at = -1;
    display_retry_until = -1;
    return 0;
}

static void tell_tasks(void);
static void display_changed(void)
{
    display_pending = 1;
    display_refresh_at = now_ms() + 100;
    display_retry_until = now_ms() + 2000;
}

/* Take the screen and the keyboard back from f: ask, then revoke anyway. */
static void take_back(struct frame *f)
{
    char msg[CONTROL_MAX];
    int fds[CONTROL_MAX_FDS], n = 0;
    double t0 = now_ms();

    control_send(f->ctl, "release", 7, NULL, 0);
    for (;;) {
        struct pollfd w = { .fd = f->ctl, .events = POLLIN };
        int left = 500 - (int)(now_ms() - t0);
        if (left <= 0 || poll(&w, 1, left) <= 0 || control_recv(f->ctl, msg, sizeof msg, fds, &n) <= 0) {
            say("init: %s did not release the screen in time; revoking", f->name);
            break;
        }
        for (int i = 0; i < n; i++) {
            close(fds[i]);
        }
        if (strcmp(msg, "released") == 0) {
            break;
        }
        if (f == &home && strcmp(msg, "apps?") == 0) {
            char rows[CONTROL_MAX - 8];
            apps_list(rows, sizeof rows);
            const char *reply[] = { "apps", rows, NULL };
            control_send_fields(home.ctl, reply);
        } else if (f == &home && strcmp(msg, "tasks?") == 0) {
            tell_tasks();
        }
    }
    keys_take_back(f->kbd, f->nkbd);
    f->nkbd = 0;
    pointer_take_back(f->pointer, f->npointer);
    f->pointer = NULL;
    f->npointer = 0;
    f->pointer_wanted = 0;
    if (screen_take_back(f->lessee) < 0) {
        say("init: cannot reclaim DRM master from %s", f->name);
    }
    f->lessee = 0;
    fg = NULL;
}

/* Topology changes never freeze/restart a direct task. The shared display
 * owns the raw DRM master while active, so leave its hotplug to labwc and
 * rescan only once control returns to a direct frontend. */
static int refresh_display(void)
{
    if (!display_pending || display_refresh_at < 0 || now_ms() < display_refresh_at) return 0;
    if (fg && fg->shared_display) {
        display_refresh_at = -1;
        return 0;
    }
    struct frame *active = fg;
    if (!active) {
        if (locked) active = locker.pid > 0 ? &locker : NULL;
        else if (front && front->depth > 0) active = &front->f[front->depth - 1];
        else if (home.pid > 0) active = &home;
    }
    if (!active) { display_refresh_at = -1; return 0; }
    if (fg) take_back(fg);
    int available = screen_refresh();
    if (available > 0 && lend(active) == 0) {
        say("init: refreshed outputs for %s", active->name);
    } else {
        /* Keep the task and its shadow. A subsequent DRM event retries even
         * when the same connector and mode return after a disconnect. */
        display_pending = 1;
        display_refresh_at = available < 0 && now_ms() < display_retry_until ? now_ms() + 200 : -1;
        say("init: outputs unavailable for %s; waiting for a display change", active->name);
    }
    return 1;
}

static void freeze(struct frame *f)
{
    char path[96], amount[32];
    double t0 = now_ms();

    snprintf(path, sizeof path, "%s/cgroup.freeze", f->cgroup);
    write_file(path, "1");
    snprintf(path, sizeof path, "%s/cgroup.events", f->cgroup);
    while (read_value(path, "frozen") != 1 && now_ms() - t0 < 2000) {
        usleep(1000);
    }
    double frozen_ms = now_ms() - t0, before = cg_mib(f, "memory.current");
    snprintf(path, sizeof path, "%s/memory.reclaim", f->cgroup);
    snprintf(amount, sizeof amount, "%lld", (long long)(before * MIB));
    write_file(path, amount); /* EAGAIN: it reclaimed less than asked */
    f->frozen = 1;
    say("init: froze %s in %.1f ms; reclaim %.1f -> %.1f MiB in memory, %.1f MiB in swap, %.1f ms",
        f->name, frozen_ms, before, cg_mib(f, "memory.current"), cg_mib(f, "memory.swap.current"),
        now_ms() - t0 - frozen_ms);
}

static void thaw(struct frame *f)
{
    char path[96];
    double t0 = now_ms();

    snprintf(path, sizeof path, "%s/cgroup.freeze", f->cgroup);
    write_file(path, "0");
    f->frozen = 0;
    say("init: thawed %s in %.1f ms", f->name, now_ms() - t0);
}

/* Hand the screen from the program that has it to `to`, freezing the first. */
static void switch_to(struct frame *to)
{
    struct frame *from = fg;

    if (from != NULL) {
        take_back(from);
        if (!from->shared_display) {
            freeze(from);
        }
    }
    if (to->frozen) {
        thaw(to);
    }
    lend(to);
}

static void send3(struct frame *f, const char *a, const char *b, const char *c)
{
    const char *fields[] = { a, b, c, NULL };

    control_send_fields(f->ctl, fields);
}

static void do_open(char *const argv[])
{
    char key[128];
    struct task *t = NULL, *free_slot = NULL;

    join(argv, key, sizeof key);
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].depth > 0 && strcmp(tasks[i].key, key) == 0) {
            t = &tasks[i];
        } else if (tasks[i].depth == 0 && free_slot == NULL) {
            free_slot = &tasks[i];
        }
    }
    if (t == NULL && strcmp(argv[0], desktop_argv[0]) == 0 && argv[1] != NULL &&
        strcmp(argv[1], desktop_argv[1]) == 0) {
        for (int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].depth && tasks[i].f[0].shared_display) {
                t = &tasks[i];
                break;
            }
        }
    }
    if (t != NULL) {
        front = t;
        switch_to(&t->f[t->depth - 1]);
        return;
    }
    if (free_slot == NULL) {
        send3(&home, "ended", key, "-1");
        return;
    }
    take_back(&home);
    freeze(&home);
    if (start(&free_slot->f[0], argv) < 0) {
        thaw(&home);
        lend(&home);
        send3(&home, "ended", key, "-1");
        return;
    }
    snprintf(free_slot->key, sizeof free_slot->key, "%s", key);
    free_slot->depth = 1;
    front = free_slot;
    lend(&free_slot->f[0]);
}

static void do_call(struct frame *caller, char *const argv[])
{
    struct task *t = front;

    if (t == NULL || caller != &t->f[t->depth - 1] || t->depth == MAX_DEPTH) {
        send3(caller, "returned", argv[0], "-1");
        return;
    }
    take_back(caller);
    freeze(caller);
    if (start(&t->f[t->depth], argv) < 0) {
        thaw(caller);
        lend(caller);
        send3(caller, "returned", argv[0], "-1");
        return;
    }
    t->depth++;
    lend(&t->f[t->depth - 1]);
}

static void do_kill(char *const argv[])
{
    char key[128];

    join(argv, key, sizeof key);
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->depth == 0 || strcmp(t->key, key) != 0) {
            continue;
        }
        say("init: killing task %s", t->key);
        for (int d = t->depth - 1; d >= 0; d--) {
            struct frame *f = &t->f[d];
            if (f->frozen) {
                thaw(f);
            }
            char kill_path[96];
            snprintf(kill_path, sizeof kill_path, "%s/cgroup.kill", f->cgroup);
            write_file(kill_path, "1");
            if (f->pid > 0) {
                kill(-f->pid, SIGKILL);
                kill(f->pid, SIGKILL);
                if (!f->reaped) {
                    waitpid(f->pid, NULL, 0);
                }
            }
            pointer_take_back(f->pointer, f->npointer);
            f->pointer = NULL;
            f->npointer = 0;
            close(f->ctl);
            rmdir(f->cgroup);
            f->pid = 0;
        }
        t->depth = 0;
        send3(&home, "ended", key, "137");
        return;
    }
    send3(&home, "ended", key, "0");
}

static int stay_on_display;

static void go_home(void)
{
    stay_on_display = 0;
    if (front == NULL) {
        return;
    }
    const char *key = front->key;
    front = NULL;
    if (!locked) { /* locked: home gets the screen when it unlocks */
        switch_to(&home);
    }
    send3(&home, "back", key, NULL);
}

/* The next (dir 1) or previous (-1) of home and the running tasks, in the
 * order of their slots, round; the one left is frozen as it is. */
static void do_switch(int dir)
{
    struct task *ring[MAX_TASKS + 1];
    int n = 0, at = 0;
    ring[n++] = NULL; /* home */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].depth == 0) continue;
        if (&tasks[i] == front) at = n;
        ring[n++] = &tasks[i];
    }
    struct task *t = ring[(at + dir + n) % n];
    if (n < 2 || dir == 0 || t == front) return;
    if (t == NULL) {
        go_home();
        return;
    }
    front = t;
    if (t->f[0].shared_display) stay_on_display = 1; /* as when home opens its view */
    switch_to(&t->f[t->depth - 1]);
}

/* The lock screen on top of everything (lock_argv): what had the screen waits
 * as it would for home, frozen (the shared display only released). Without
 * the lock program there is no locking. */
static void lock_start(void)
{
    lock_start_at = -1;
    lock_shown = 0;
    if (start(&locker, lock_argv) == 0) {
        lend(&locker);
    } else {
        lock_start_at = now_ms() + 1000;
    }
}

static void lock(void)
{
    if (locked) {
        return;
    }
    if (access(lock_argv[0], X_OK) != 0) {
        say("init: %s is not installed; not locking", lock_argv[0]);
        return;
    }
    say("init: locking");
    locked = 1;
    stay_on_display = 0;
    if (fg != NULL) {
        struct frame *from = fg;
        take_back(from);
        if (!from->shared_display) {
            freeze(from);
        }
    }
    lock_start();
}

/* The lock exited 0: back to the task that was on the screen, or home. */
static void unlock(void)
{
    if (!locked) {
        return;
    }
    locked = 0;
    lock_start_at = -1;
    active_at = now_ms();
    say("init: unlocked");
    struct frame *to = front != NULL && front->depth > 0 ? &front->f[front->depth - 1] : &home;
    if (to == &home) {
        front = NULL;
    }
    if (to->pid == 0) {
        return; /* home starting again: gone() lends it */
    }
    if (to->frozen) {
        thaw(to);
    }
    lend(to);
}

/* Every keyboard, mouse, touchpad, touch screen and pen (pointer_lend's
 * devices), read only to see that there was input: the program on the screen
 * reads copies of its own, and nothing is grabbed. Opened again when devices
 * come or go. */
static void activity_open(void)
{
    char paths[MAX_ACTIVITY][POINTER_PATH];
    if (afd != NULL) {
        pointer_take_back(afd, nafd);
    }
    afd = pointer_lend(&nafd, paths, MAX_ACTIVITY);
}

/* Reconcile pointer leases without reopening devices the frontend is already
 * reading. A removed/reused event path first loses its old revoked lease. */
static void pointer_refresh(struct frame *f)
{
    char paths[64][POINTER_PATH];
    size_t n;
    int *fds = pointer_lend(&n, paths, 64);
    for (size_t i = 0; i < f->npointer;) {
        int found = 0;
        for (size_t k = 0; k < n; k++) found |= input_same(f->pointer[i], fds[k]);
        if (found) { i++; continue; }
        char msg[15 + POINTER_PATH];
        int len = snprintf(msg, sizeof msg, "pointer-remove%c%s", 0, f->pointer_paths[i]);
        control_send(f->ctl, msg, (size_t)len, NULL, 0);
        ioctl(f->pointer[i], EVIOCREVOKE, NULL);
        close(f->pointer[i]);
        f->npointer--;
        memmove(f->pointer + i, f->pointer + i + 1, (f->npointer - i) * sizeof *f->pointer);
        memmove(f->pointer_paths + i, f->pointer_paths + i + 1,
                (f->npointer - i) * sizeof *f->pointer_paths);
    }
    for (size_t k = 0; k < n; k++) {
        int found = 0;
        for (size_t i = 0; i < f->npointer; i++) found |= input_same(f->pointer[i], fds[k]);
        if (found) { close(fds[k]); continue; }
        int *p = realloc(f->pointer, (f->npointer + 1) * sizeof *p);
        if (!p) { close(fds[k]); continue; }
        f->pointer = p;
        char msg[8 + POINTER_PATH];
        int len = snprintf(msg, sizeof msg, "pointer%c%s", 0, paths[k]);
        if (control_send(f->ctl, msg, (size_t)len, &fds[k], 1) < 0) { close(fds[k]); continue; }
        memcpy(f->pointer_paths[f->npointer], paths[k], POINTER_PATH);
        f->pointer[f->npointer++] = fds[k];
    }
    free(fds); /* surviving descriptors belong to f, not a temporary lease */
    control_send(f->ctl, "pointer-retry", 13, NULL, 0);
}

/* Called after existing module events and dead input descriptors. Only the
 * current framebuffer owner receives updates (including the lock); labwc
 * discovers its own devices. No screen handover, input grab or synthetic key. */
static void input_refresh(void)
{
    keys_refresh();
    activity_open();
    if (!fg || fg->shared_display) return;
    if (keys_sync(fg->kbd, &fg->nkbd, MAX_KBD)) {
        control_send(fg->ctl, "keys", 4, fg->kbd, fg->nkbd);
    }
    if (fg->pointer_wanted) pointer_refresh(fg);
}

/* When the screen locks for want of input (ms), or -1: never with lock_after
 * 0, nor while locked, nor while the shared display has the screen: labwc
 * knows its idleness, and what holds it off (a video playing), and
 * labwc-control asks for the lock. */
static double idle_lock_at(void)
{
    if (lock_after <= 0 || locked || (fg != NULL && fg->shared_display)) {
        return -1;
    }
    return active_at + lock_after * 1e3;
}

static struct task *shared_display_task(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].depth > 0 && tasks[i].f[0].shared_display) {
            return &tasks[i];
        }
    }
    return NULL;
}

/* Start, thaw, freeze or stop the shared display (labwc) following container
 * state. Returns 1 if it did any of it: the programs' sockets may then have
 * been read (take_back) or closed since the last poll. */
static int sync_display(void)
{
    int state = apps_display_state();
    struct task *t = shared_display_task();

    if (state == APPS_DISPLAY_ACTIVE) {
        if (t == NULL) {
            if (access(desktop_argv[1], X_OK) == 0 && front == NULL && fg == &home) {
                do_open(desktop_argv);
                return 1;
            }
        } else if (t->f[0].frozen) {
            thaw(&t->f[0]);
            return 1;
        }
        return 0;
    }
    if (t == NULL) {
        return 0;
    }
    int did = front == t;
    if (did) {
        go_home();
    }
    struct frame *f = &t->f[0];
    if (state == APPS_DISPLAY_FROZEN) {
        if (!f->frozen && fg != f) {
            freeze(f);
            did = 1;
        }
        return did;
    }
    say("init: stopping shared display (all containers closed)");
    if (f->frozen) {
        thaw(f);
    }
    char kill_path[96], ev_path[96];
    snprintf(kill_path, sizeof kill_path, "%s/cgroup.kill", f->cgroup);
    write_file(kill_path, "1");
    snprintf(ev_path, sizeof ev_path, "%s/cgroup.events", f->cgroup);
    double t0 = now_ms();
    while (read_value(ev_path, "populated") != 0 && now_ms() - t0 < 300) {
        usleep(1000);
    }
    if (!f->reaped && f->pid > 0) {
        waitpid(f->pid, NULL, WNOHANG);
    }
    close(f->ctl);
    rmdir(f->cgroup);
    f->pid = 0;
    t->depth = 0;
    unlink("/run/user/0/wayland-0");
    unlink("/run/user/0/wayland-0.lock");
    return 1;
}

static void tell_tasks(void)
{
    char lines[MAX_TASKS][256];
    const char *fields[MAX_TASKS + 2] = { "tasks" };
    int n = 1;

    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->depth == 0) {
            continue;
        }
        size_t len = (size_t)snprintf(lines[i], sizeof lines[i], "%s:", t->key);
        for (int d = 0; d < t->depth && len < sizeof lines[i]; d++) {
            len += (size_t)snprintf(lines[i] + len, sizeof lines[i] - len, "%s%s", d ? " > " : " ",
                                    t->f[d].name);
        }
        if (len < sizeof lines[i]) {
            snprintf(lines[i] + len, sizeof lines[i] - len, "; %.1f MiB in swap",
                     cg_mib(&t->f[t->depth - 1], "memory.swap.current"));
        }
        fields[n++] = lines[i];
    }
    fields[n] = NULL;
    control_send_fields(home.ctl, fields);
}

/* f has exited (its control socket closed). */
static void gone(struct frame *f)
{
    int status = 0;

    if (f->frozen) {
        thaw(f); /* a frozen process cannot finish exiting */
    }
    int known = 1;
    if (f->reaped) {
        status = f->status;
    } else if (waitpid(f->pid, &status, 0) != f->pid) {
        known = 0; /* reaped where its status was not kept: never read as 0 (for the lock, unlocked) */
    }
    status = !known ? 255 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    say("init: %s (pid %d) exited with status %d", f->name, (int)f->pid, status);
    char kill_path[96], ev_path[96];
    snprintf(kill_path, sizeof kill_path, "%s/cgroup.kill", f->cgroup);
    write_file(kill_path, "1");
    snprintf(ev_path, sizeof ev_path, "%s/cgroup.events", f->cgroup);
    double t0 = now_ms();
    while (read_value(ev_path, "populated") != 0 && now_ms() - t0 < 200) {
        usleep(1000);
    }
    if (fg == f) {
        keys_take_back(f->kbd, f->nkbd);
        pointer_take_back(f->pointer, f->npointer);
        f->pointer = NULL;
        f->npointer = 0;
        if (screen_take_back(f->lessee) < 0) {
            say("init: exited %s while DRM master reclaim failed", f->name);
        }
        f->lessee = 0;
        fg = NULL;
    }
    close(f->ctl);
    rmdir(f->cgroup);
    char name[32];
    snprintf(name, sizeof name, "%s", f->name);
    f->pid = 0;

    if (f == &locker) {
        if (status == 0) {
            unlock();
        } else if (locked) {
            say("init: the lock ended (status %d); starting it again", status);
            lock_start_at = now_ms() + 500;
        }
        return;
    }
    if (f == &home) {
        say("init: starting home again");
        if (start(&home, home_argv) == 0 && front == NULL && !locked) {
            lend(&home);
        }
        return;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->depth == 0 || f < t->f || f >= t->f + MAX_DEPTH) {
            continue;
        }
        if (f != &t->f[t->depth - 1]) {
            say("init: %s left the middle of task %s", name, t->key);
            return;
        }
        t->depth--;
        char code[12];
        snprintf(code, sizeof code, "%d", status);
        if (t->depth > 0) {
            struct frame *caller = &t->f[t->depth - 1];
            if (front == t && !locked) {
                thaw(caller);
                lend(caller);
            }
            send3(caller, "returned", name, code);
        } else {
            if (front == t) {
                front = NULL;
                if (!locked) {
                    thaw(&home);
                    lend(&home);
                }
            }
            send3(&home, "ended", t->key, code);
        }
        return;
    }
}

/* Wait up to ms for every child to end, taking whatever ends. */
static void reap_all(int ms)
{
    for (int i = 0; i < ms / 50; i++) {
        pid_t pid;
        while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        }
        if (pid < 0) {
            return; /* none left */
        }
        usleep(50000);
    }
}

/* Power off or reboot (how: RB_POWER_OFF, RB_AUTOBOOT) cleanly: everything
 * else is asked to end (SIGTERM), and killed 2 s later (SIGKILL reaches
 * frozen programs too); the ESP is unmounted, the log saved and the root made
 * read-only, so neither needs repairing at the next boot. */
static void shut_down(int how)
{
    say(how == RB_POWER_OFF ? "init: power off" : "init: reboot");
    kill(-1, SIGTERM);
    reap_all(2000);
    kill(-1, SIGKILL);
    reap_all(2000);
    if (esp_dev[0] && umount("/efi") < 0) {
        say("init: umount /efi: %s", strerror(errno));
    }
    save_log();
    sync();
    if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) < 0) {
        say("init: cannot make / read-only: %s", strerror(errno));
    }
    reboot(how);
}

static void power_off(void)
{
    shut_down(RB_POWER_OFF);
}

static void restart(void)
{
    shut_down(RB_AUTOBOOT);
}

struct settings_read {
    struct service *services;
    size_t count;
    int hibernate_after, login_skip, lock_after, failed;
};

/* One setting of rc.conf (base.h) for process 1; the other settings belong
 * to the programs that read them (as in BSD's rc.conf):
 *   hibernate_after="SECONDS"   asleep with the lid closed, then hibernate
 *                               (default 30), on mains power as on battery
 *   login_skip="1"              not locked at boot (key_lock and the lid lock)
 *   lock_after="MINUTES"        locked after that long without input
 *                               (default 10; 0: never)
 *   service_NAME="COMMAND"      a service (shell command line) */
static void setting(const char *name, const char *value, void *arg)
{
    struct settings_read *next = arg;
    if (strcmp(name, "hibernate_after") == 0 && atoi(value) > 0) {
        next->hibernate_after = atoi(value);
    } else if (strcmp(name, "login_skip") == 0) {
        next->login_skip = strcmp(value, "1") == 0;
    } else if (strcmp(name, "lock_after") == 0) {
        next->lock_after = atoi(value) > 0 ? atoi(value) * 60 : 0;
    } else if (strncmp(name, "service_", 8) == 0 && !next->failed) {
        if (strlen(value) >= sizeof services[0].command) {
            say("init: rc.conf: %s is too long; left out", name);
            return;
        }
        for (size_t i = 0; i < next->count; i++) {
            if (strcmp(next->services[i].command, value) == 0) {
                return;
            }
        }
        struct service *p = next->count >= SIZE_MAX / sizeof *p ? NULL :
                            realloc(next->services, (next->count + 1) * sizeof *p);
        if (p == NULL) {
            next->failed = 1;
            return;
        }
        next->services = p;
        struct service *s = &p[next->count++];
        memset(s, 0, sizeof *s);
        snprintf(s->command, sizeof s->command, "%s", value);
    }
}

static void apply_hostname(void)
{
    char name[65] = "locachy", raw[128];
    FILE *f = fopen("/etc/hostname", "re");
    if (f) {
        while (fgets(raw, sizeof raw, f)) {
            char *hash = strchr(raw, '#');
            if (hash) *hash = '\0';
            char *s = raw + strspn(raw, " \t\r\n");
            size_t len = strcspn(s, " \t\r\n");
            if (len > 0 && len < sizeof name) {
                int ok = isalnum((unsigned char)s[0]) && isalnum((unsigned char)s[len - 1]);
                for (size_t i = 0; i < len && ok; i++) {
                    unsigned char c = (unsigned char)s[i];
                    ok = isalnum(c) || c == '-' || c == '.';
                }
                if (ok) {
                    memcpy(name, s, len);
                    name[len] = '\0';
                }
                break;
            }
        }
        fclose(f);
    }
    if (sethostname(name, strlen(name)) < 0) {
        say("init: sethostname %s: %s", name, strerror(errno));
    }
}

/* Read a new list before changing anything: a failed allocation must not
 * stop the services already running. Unchanged commands keep their pid and
 * restart time; removed ones are ended, new ones start in the main loop. */
static void read_settings(void)
{
    apply_hostname();
    struct settings_read next = { .hibernate_after = hibernate_after, .lock_after = 600 };
    int read = rc_read(setting, &next);
    if (read < 0 || next.failed) {
        say("init: cannot %s %s; services unchanged", read < 0 ? "read" : "allocate services for", RC_CONF);
        free(next.services);
        return;
    }
    for (size_t i = 0; i < nservices; i++) {
        size_t k;
        for (k = 0; k < next.count; k++) {
            if (strcmp(next.services[k].command, services[i].command) == 0) {
                next.services[k] = services[i];
                break;
            }
        }
        if (k < next.count) {
            continue;
        }
        say("init: service no longer in %s: %s", RC_CONF, services[i].command);
        if (services[i].pid > 0) {
            /* Its session, and itself: just started, it may not have one yet. */
            kill(-services[i].pid, SIGTERM);
            kill(services[i].pid, SIGTERM);
        }
    }
    free(services);
    services = next.services;
    nservices = next.count;
    hibernate_after = next.hibernate_after;
    login_skip = next.login_skip;
    lock_after = next.lock_after;
    keys_reload(); /* key_home, key_lock */
    say("init: settings: hibernate_after %d s, login_skip %d, lock_after %d s, %zu services", hibernate_after,
        login_skip, lock_after, nservices);
}

static void service_start(struct service *s)
{
    pid_t pid = fork();

    if (pid == 0) {
        setsid();
        sigprocmask(SIG_UNBLOCK, &handled, NULL);
        execl("/bin/sh", "sh", "-c", s->command, (char *)NULL);
        _exit(127);
    }
    s->pid = pid < 0 ? 0 : pid;
    s->restart_at = pid < 0 ? now_ms() + 5000 : 0;
    say("init: service started (pid %d): %s", (int)pid, s->command);
}

/* Wait for every child that has ended: a program's status is kept for gone()
 * (which runs when its control socket closes); a service is started again 5 s
 * later; anything else (orphans, which become ours) is just let go. */
static void reap(void)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (apps_reap(pid, status)) {
            continue;
        }
        if (home.pid == pid) {
            home.reaped = 1;
            home.status = status;
        }
        if (locker.pid == pid) {
            locker.reaped = 1;
            locker.status = status;
        }
        for (int i = 0; i < MAX_TASKS; i++) {
            for (int d = 0; d < tasks[i].depth; d++) {
                if (tasks[i].f[d].pid == pid) {
                    tasks[i].f[d].reaped = 1;
                    tasks[i].f[d].status = status;
                    char kill_path[96];
                    snprintf(kill_path, sizeof kill_path, "%s/cgroup.kill", tasks[i].f[d].cgroup);
                    write_file(kill_path, "1");
                }
            }
        }
        for (size_t i = 0; i < nservices; i++) {
            if (services[i].pid == pid) {
                say("init: service ended (pid %d, status %d): %s", (int)pid,
                    WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), services[i].command);
                services[i].pid = 0;
                services[i].restart_at = now_ms() + 5000;
            }
        }
    }
}

/* A message from f (its control socket is readable). */
static void message(struct frame *f)
{
    char msg[CONTROL_MAX], *fields[16];
    int fds[CONTROL_MAX_FDS], nfds = 0;
    int len = control_recv(f->ctl, msg, sizeof msg, fds, &nfds);

    for (int i = 0; i < nfds; i++) {
        close(fds[i]);
    }
    if (len <= 0) {
        gone(f);
        return;
    }
    int n = control_fields(msg, len, fields, 15);
    fields[n] = NULL;
    if (locked && f != &locker && (strcmp(fields[0], "call") == 0 || strcmp(fields[0], "open") == 0 ||
                                   strcmp(fields[0], "app") == 0)) {
        /* Sent just before it was frozen: not now. */
        char key[128];
        join(fields + 1, key, sizeof key);
        if (strcmp(fields[0], "call") == 0) {
            send3(f, "returned", n > 1 ? fields[1] : "", "-1");
        } else if (strcmp(fields[0], "open") == 0) {
            send3(&home, "ended", key, "-1");
        } else {
            const char *reply[] = { "app-result", "error", n > 2 ? fields[2] : "", "the screen is locked", NULL };
            control_send_fields(home.ctl, reply);
        }
    } else if (strcmp(fields[0], "call") == 0 && n > 1) {
        do_call(f, fields + 1);
    } else if (strcmp(fields[0], "open") == 0 && n > 1 && f == &home) {
        do_open(fields + 1);
    } else if (strcmp(fields[0], "kill") == 0 && n > 1 && f == &home) {
        do_kill(fields + 1);
    } else if (strcmp(fields[0], "tasks?") == 0 && f == &home) {
        tell_tasks();
    } else if (strcmp(fields[0], "apps?") == 0 && f == &home) {
        char rows[CONTROL_MAX - 8];
        apps_list(rows, sizeof rows);
        const char *reply[] = { "apps", rows, NULL };
        control_send_fields(home.ctl, reply);
    } else if (strcmp(fields[0], "app") == 0 && (n == 3 || (n == 4 && strcmp(fields[1], "open") == 0)) &&
               f == &home) {
        /* app ACTION NAME, or app open NAME TARGET (a file or URL to open in it) */
        int open_view = strcmp(fields[1], "open") == 0 || strcmp(fields[1], "show") == 0;
        if (open_view || strcmp(fields[1], "start") == 0 || strcmp(fields[1], "resume") == 0) {
            struct task *dt = shared_display_task();
            if (dt != NULL && dt->f[0].frozen) {
                thaw(&dt->f[0]);
            }
        }
        char why[192];
        int result = n == 4 ? apps_open(fields[2], fields[3], why, sizeof why)
                            : apps_action(fields[1], fields[2], why, sizeof why);
        if (result == 0 && open_view) {
            stay_on_display = 1;
            do_open(desktop_argv);
        }
        const char *reply[] = { "app-result", result == 0 ? "ok" : "error", fields[2], why, NULL };
        control_send_fields(home.ctl, reply);
    } else if (strcmp(fields[0], "pointer?") == 0 && f == fg && !f->shared_display) {
        f->pointer_wanted = 1;
        pointer_refresh(f);
    } else if (strcmp(fields[0], "master") == 0 && f == fg) {
        int result = screen_drop_master(f->lessee);
        f->lessee = 0;
        control_send(f->ctl, result == 0 ? "mastered" : "master-error", result == 0 ? 8 : 12, NULL, 0);
    } else if (strcmp(fields[0], "display-ready") == 0 && f == fg && f->shared_display) {
        if (stay_on_display) {
            stay_on_display = 0;
            apps_tick();
        } else {
            go_home();
        }
    } else if (strcmp(fields[0], "home") == 0 && f == fg && !locked) {
        go_home();
    } else if (strcmp(fields[0], "lock") == 0 && f == fg) {
        lock();
    } else if (strcmp(fields[0], "locked") == 0 && f == &locker) {
        lock_shown = 1;
    } else if (strcmp(fields[0], "switch") == 0 && n == 2 && f == fg && !locked) {
        do_switch(strcmp(fields[1], "next") == 0 ? 1 : strcmp(fields[1], "prev") == 0 ? -1 : 0);
    } else if (strcmp(fields[0], "reboot") == 0) {
        restart();
    } else if (strcmp(fields[0], "poweroff") == 0) {
        power_off();
    } /* "released" after the deadline: nothing to do */
}

/* Lock, and wait (up to 1 s) for the lock to be on the screen: before the
 * machine sleeps, so that it wakes up locked. */
static void lock_wait(void)
{
    lock();
    double t0 = now_ms();
    while (locked && !lock_shown && locker.pid > 0 && now_ms() - t0 < 1000) {
        struct pollfd w = { .fd = locker.ctl, .events = POLLIN };
        if (poll(&w, 1, 1000 - (int)(now_ms() - t0)) <= 0) {
            break;
        }
        message(&locker);
    }
}

/* The ACPI button driver's input devices, which process 1 always watches,
 * whatever is on the screen or stuck: the power button (KEY_POWER, no
 * letters) saves the log and powers off; the lid (SW_LID) puts the machine
 * to sleep when it closes, on mains power as on battery, as logind does
 * (HandleLidSwitchExternalPower); docked, another screen connected, it stays
 * up, locked, and sleeps when that screen goes with the lid closed
 * (HandleLidSwitchDocked). */
static int system_inputs(int *fds, int max)
{
    int n = 0;

    for (int i = 0; i < 32 && n < max; i++) {
        unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
        unsigned long sw[SW_MAX / (8 * sizeof(long)) + 1] = { 0 };
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        size_t w = 8 * sizeof(long);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        ioctl(fd, EVIOCGBIT(EV_SW, sizeof sw), sw);
        int power = ((keys[KEY_POWER / w] >> (KEY_POWER % w)) & 1) && !((keys[KEY_A / w] >> (KEY_A % w)) & 1);
        int vantage = ((keys[KEY_VENDOR / w] >> (KEY_VENDOR % w)) & 1) ||
                      ((keys[KEY_PROG1 / w] >> (KEY_PROG1 % w)) & 1);
        int lid = (sw[SW_LID / w] >> (SW_LID % w)) & 1;
        if (power || vantage || lid) {
            fds[n++] = fd;
            if (lid) {
                lid_fd = fd;
            }
        } else {
            close(fd);
        }
    }
    return n;
}

/* Whether the lid is closed. The ACPI button driver's state file asks the
 * firmware (_LID) at every read; the input device only knows the last event,
 * which after a resume from disk is the closing before it. */
static int lid_closed(void)
{
    DIR *d = opendir("/proc/acpi/button/lid");
    struct dirent *e;
    char path[300], state[64];

    while (d != NULL && (e = readdir(d)) != NULL) {
        snprintf(path, sizeof path, "/proc/acpi/button/lid/%s/state", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        ssize_t n = fd < 0 ? -1 : read(fd, state, sizeof state - 1);
        if (fd >= 0) {
            close(fd);
        }
        if (n > 0) {
            state[n] = '\0';
            closedir(d);
            return strstr(state, "closed") != NULL;
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    unsigned long sw[SW_MAX / (8 * sizeof(long)) + 1] = { 0 };
    size_t w = 8 * sizeof(long);
    return lid_fd >= 0 && ioctl(lid_fd, EVIOCGSW(sizeof sw), sw) >= 0 && ((sw[SW_LID / w] >> (SW_LID % w)) & 1);
}

/* Let the lid wake the machine, or not: the ACPI lid devices (PNP0C0D) as
 * wakeup sources. Opening the lid wakes it from S3, and from hibernation
 * where the firmware allows. */
static void lid_wake(int on)
{
    DIR *d = opendir("/sys/bus/acpi/devices");
    struct dirent *e;
    char path[320];

    while (d != NULL && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "PNP0C0D:", 8) == 0) {
            snprintf(path, sizeof path, "/sys/bus/acpi/devices/%s/physical_node/power/wakeup", e->d_name);
            write_file(path, on ? "enabled" : "disabled");
        }
    }
    if (d != NULL) {
        closedir(d);
    }
}

/* Docked: a screen other than the laptop's own (monitor_internal) is
 * connected, so with the lid closed the machine is still in use. */
static int docked(void)
{
    DIR *d = opendir("/sys/class/drm");
    struct dirent *e;
    int found = 0;

    while (d != NULL && !found && (e = readdir(d)) != NULL) {
        const char *dash = strchr(e->d_name, '-');
        char path[300], status[16] = "";
        if (strncmp(e->d_name, "card", 4) != 0 || dash == NULL || monitor_internal(dash + 1)) {
            continue;
        }
        snprintf(path, sizeof path, "/sys/class/drm/%s/status", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        ssize_t n = fd < 0 ? -1 : read(fd, status, sizeof status - 1);
        if (fd >= 0) {
            close(fd);
        }
        status[n > 0 ? n : 0] = '\0';
        found = strncmp(status, "connected", 9) == 0;
    }
    if (d != NULL) {
        closedir(d);
    }
    return found;
}

/* Hibernate: the kernel writes everything in memory to the swap (resume_dev)
 * and powers the machine off. At the next start resume() brings it all back
 * and this returns, as if the machine had only slept. */
static void hibernate(void)
{
    say("init: hibernating to %s", swap_dev);
    save_log();
    sync();
    if (write_file("/sys/power/state", "disk") < 0) {
        say("init: cannot hibernate: %s", strerror(errno));
        return;
    }
    say("init: back from hibernation");
}

/* Suspend to RAM (ACPI S3) until something wakes the machine: opening the
 * lid, the power button, or the RTC alarm. Everything stays in memory as it
 * was, frozen programs and the screen included; the kernel suspends and
 * resumes the devices. When the lid closed it (lid), the machine hibernates
 * if the lid is still closed hibernate_after seconds later (the RTC alarm
 * wakes it for that; hibernate_after in rc.conf), on mains power as on
 * battery: only memory written to the encrypted swap is as safe as the disk.
 * Docked meanwhile (another screen connected), it stays up. Returns how long
 * the machine slept, in seconds. */
static double suspend(int lid)
{
    struct timespec a, b;
    char when[24];

    save_log();
    sync();
    write_file("/sys/power/mem_sleep", "deep");
    clock_gettime(CLOCK_REALTIME, &a); /* the boot-time clock stops in S3 */
    time_t hibernate_at = a.tv_sec + hibernate_after;
    int try_hibernate = lid && resume_dev[0];
    for (;;) {
        if (try_hibernate) {
            clock_gettime(CLOCK_REALTIME, &b);
            snprintf(when, sizeof when, "+%lld", (long long)(hibernate_at > b.tv_sec ? hibernate_at - b.tv_sec : 1));
            write_file("/sys/class/rtc/rtc0/wakealarm", "0");
            write_file("/sys/class/rtc/rtc0/wakealarm", when);
        }
        say("init: going to sleep");
        if (write_file("/sys/power/state", "mem") < 0) {
            say("init: cannot sleep: %s", strerror(errno));
            return 0;
        }
        clock_gettime(CLOCK_REALTIME, &b);
        if (!lid || !lid_closed()) {
            break;
        }
        if (docked()) {
            say("init: woke up with the lid closed, another screen connected: staying up");
            break;
        }
        if (try_hibernate && b.tv_sec >= hibernate_at) {
            /* Once: it returns back from disk, or having failed (the
             * machine must not stay up with the lid closed either way).
             * The alarm that just woke the machine goes first: still
             * pending, it would wake it again at once from hibernation. */
            write_file("/sys/class/rtc/rtc0/wakealarm", "0");
            lid_wake(lid_wakes_from_disk);
            hibernate();
            if (lid_wakes_from_disk && lid_closed()) {
                /* Back with the lid still closed: the firmware woke the
                 * machine because of the closed lid (the X230's does, some
                 * 10 s after powering off). Again without the lid as a wakeup
                 * source, and so from now on: the power button brings it
                 * back. */
                say("init: the closed lid woke the machine from disk; again without lid wakeup");
                lid_wakes_from_disk = 0;
                lid_wake(0);
                hibernate();
            }
            lid_wake(1); /* opening the lid wakes it from S3 */
            try_hibernate = 0;
            if (!lid_closed()) {
                break;
            }
        }
        say("init: woke up with the lid still closed; back to sleep");
    }
    if (lid) {
        write_file("/sys/class/rtc/rtc0/wakealarm", "0");
    }
    clock_gettime(CLOCK_REALTIME, &b);
    double slept = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
    say("init: woke up after %.1f s asleep", slept);
    /* Restore TrackPoint speed (psmouse resets on resume) and restart networking. */
    if (fork() == 0) {
        execl("/usr/local/bin/settings", "settings", "--apply", (char *)NULL);
        _exit(0);
    }
    for (size_t i = 0; i < nservices; i++) {
        if (services[i].pid > 0 && strstr(services[i].command, "network-client") != NULL) {
            kill(services[i].pid, SIGTERM);
            /* reap() schedules the restart after the old service has exited. */
        }
    }
    screen_flush();
    return slept;
}

/*
 * Idle power, measured in debug boots (locachy.reboot): from MEASURE_FROM
 * seconds after boot, when everything has settled at home, until the
 * reboot. The CPU's own counters (model-specific registers, /dev/cpu/0/msr)
 * give the package's energy (RAPL: cores, graphics and memory controller,
 * not the screen or the chipset) and how long the package and core 0 spent
 * in each C-state; the battery, when it is discharging, gives the whole
 * machine; /proc/interrupts gives how often something woke the CPUs.
 */
#define MEASURE_FROM 20

struct sample {
    double t;
    uint64_t tsc, energy, pkg[4], core[3]; /* package C2 C3 C6 C7, core C3 C6 C7 */
    long long interrupts, battery_uwh;
};

static uint64_t msr(int fd, uint32_t reg)
{
    uint64_t v = 0;

    return fd >= 0 && pread(fd, &v, sizeof v, reg) == sizeof v ? v : 0;
}

static long long interrupt_count(void)
{
    FILE *f = fopen("/proc/interrupts", "re");
    char line[1024];
    long long sum = 0;

    while (f != NULL && fgets(line, sizeof line, f) != NULL) {
        char *p = strchr(line, ':');
        if (p == NULL || strstr(line, "CPU0") != NULL) {
            continue;
        }
        for (char *end;; p = end) {
            long long v = strtoll(p + 1, &end, 10);
            if (end == p + 1) {
                break;
            }
            sum += v;
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    return sum;
}

static void sample(struct sample *s)
{
    static const uint32_t pkg[4] = { 0x60D, 0x3F8, 0x3F9, 0x3FA }, core[3] = { 0x3FC, 0x3FD, 0x3FE };
    int fd = open("/dev/cpu/0/msr", O_RDONLY | O_CLOEXEC);

    s->t = uptime();
    s->tsc = msr(fd, 0x10);
    s->energy = msr(fd, 0x611) & 0xFFFFFFFF; /* MSR_PKG_ENERGY_STATUS */
    for (int i = 0; i < 4; i++) {
        s->pkg[i] = msr(fd, pkg[i]);
    }
    for (int i = 0; i < 3; i++) {
        s->core[i] = msr(fd, core[i]);
    }
    if (fd >= 0) {
        close(fd);
    }
    s->interrupts = interrupt_count();
    s->battery_uwh = read_value("/sys/class/power_supply/BAT0/energy_now", NULL);
}

/* BAT0's status ("Charging", "Discharging", "Not charging", "Full"), or
 * "no battery driver". */
static const char *battery_status(void)
{
    static char status[32];
    int fd = open("/sys/class/power_supply/BAT0/status", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, status, sizeof status - 1);

    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return "no battery driver";
    }
    status[n] = '\0';
    status[strcspn(status, "\n")] = '\0';
    return status;
}

static void report(const struct sample *a, const struct sample *b)
{
    int fd = open("/dev/cpu/0/msr", O_RDONLY | O_CLOEXEC);
    uint64_t units = msr(fd, 0x606); /* MSR_RAPL_POWER_UNIT */
    double dt = b->t - a->t, ticks = (double)(b->tsc - a->tsc);
    const char *status = battery_status();

    if (fd < 0) {
        say("init: power: no /dev/cpu/0/msr");
        return;
    }
    close(fd);
    double joule = (double)((b->energy - a->energy) & 0xFFFFFFFF) / (double)(1u << ((units >> 8) & 0x1F));
    say("init: power: idle %.0f s: package %.2f W; package C2 %.1f%% C3 %.1f%% C6 %.1f%% C7 %.1f%%; "
        "core0 C3 %.1f%% C6 %.1f%% C7 %.1f%%; %.0f interrupts/s",
        dt, joule / dt, 100 * (double)(b->pkg[0] - a->pkg[0]) / ticks,
        100 * (double)(b->pkg[1] - a->pkg[1]) / ticks, 100 * (double)(b->pkg[2] - a->pkg[2]) / ticks,
        100 * (double)(b->pkg[3] - a->pkg[3]) / ticks, 100 * (double)(b->core[0] - a->core[0]) / ticks,
        100 * (double)(b->core[1] - a->core[1]) / ticks, 100 * (double)(b->core[2] - a->core[2]) / ticks,
        (double)(b->interrupts - a->interrupts) / dt);
    if (strcmp(status, "Discharging") == 0 && a->battery_uwh > 0 && b->battery_uwh > 0) {
        say("init: power: battery %.2f W (whole machine)",
            (double)(a->battery_uwh - b->battery_uwh) * 3600 / 1e6 / dt);
    } else {
        say("init: power: battery: %s, AC online %lld", status,
            read_value("/sys/class/power_supply/AC/online", NULL));
    }
}

/* The number in NAME=NUMBER on the kernel command line, or 0. Debug boots
 * (unattended) use these:
 *   locachy.reboot=SECONDS     after that long since boot, save the log and
 *                           reboot (the idle power is measured until then)
 *   locachy.sleeptest=SECONDS  20 s after boot, sleep (S3) that long, woken by
 *                           the RTC alarm, and measure the battery meanwhile;
 *                           the time asleep counts towards locachy.reboot
 * The kernel leaves parameters with a dot to modules, so they reach us only
 * through /proc/cmdline. */
static int cmdline_int(const char *name)
{
    char buf[1024];
    size_t len = strlen(name);
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, buf, sizeof buf - 1);

    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    for (char *w = strtok(buf, " \n"); w != NULL; w = strtok(NULL, " \n")) {
        if (strncmp(w, name, len) == 0 && w[len] == '=') {
            return atoi(w + len + 1);
        }
    }
    return 0;
}

/* locachy.sleeptest: sleep for seconds, woken by the RTC alarm; then measure the
 * idle power from now on (in *after). */
static void sleep_test(int seconds, struct sample *after)
{
    struct sample before;
    char when[16];

    sample(&before);
    say("init: sleep test: before: AC online %lld, battery %s", read_value("/sys/class/power_supply/AC/online", NULL),
        battery_status());
    snprintf(when, sizeof when, "+%d", seconds);
    if (write_file("/sys/class/rtc/rtc0/wakealarm", "0") < 0 ||
        write_file("/sys/class/rtc/rtc0/wakealarm", when) < 0) {
        say("init: sleep test: cannot set the RTC alarm: %s", strerror(errno));
        sample(after);
        return;
    }
    double slept = suspend(0);
    sample(after);
    say("init: sleep test: after: AC online %lld, battery %s", read_value("/sys/class/power_supply/AC/online", NULL),
        battery_status());
    if (slept > 0 && before.battery_uwh > 0 && after->battery_uwh > 0) {
        say("init: sleep test: %.0f s asleep, battery %.0f -> %.0f mWh, %.2f W (whole machine)", slept,
            before.battery_uwh / 1e3, after->battery_uwh / 1e3,
            (double)(before.battery_uwh - after->battery_uwh) * 3600 / 1e6 / slept);
    }
}

int main(void)
{
    double started = uptime();
    int kfd[MAX_KBD], pfd[8];
    int reboot_s = 0, sleeptest_s = 0;
    struct sample measured = { 0 }; /* the start of the idle measurement */

    /* Signals, read from sfd: children ending, rc.conf changed (SIGHUP), and
     * the requests to reboot (SIGTERM; SIGINT is Ctrl+Alt+Del, which the
     * kernel would otherwise act on at once, without syncing) and power off
     * (SIGUSR2). */
    sigemptyset(&handled);
    sigaddset(&handled, SIGCHLD);
    sigaddset(&handled, SIGHUP);
    sigaddset(&handled, SIGTERM);
    sigaddset(&handled, SIGINT);
    sigaddset(&handled, SIGUSR2);
    sigprocmask(SIG_BLOCK, &handled, NULL);
    int sfd = signalfd(-1, &handled, SFD_CLOEXEC);
    reboot(RB_DISABLE_CAD);
    /* The host's environment, which everything started here inherits
     * (programs, services, containers' locachy-app): they run as root, so
     * root's home; root's runtime directory (made below, once the root is
     * mounted); the one UTF-8 locale there is. */
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/bin", 1);
    setenv("HOME", "/root", 1);
    setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);
    setenv("LANG", "C.UTF-8", 1);

    mount_kernel_fs();
    say("init: started at %.3f s", started);
    prepare_boot_storage();
    unlock_luks();
    luks_only = luks_root_dev[0] != '\0';
    resume(); /* does not return when there is a hibernated system to go back to */
    /* The disks first (their drivers and ext4 are built into the kernel):
     * the other drivers are modules in the root. */
    const char *data = disks();
    int rooted = data != NULL && switch_root() == 0;
    if (rooted) {
        /* XDG_RUNTIME_DIR, made here only: 0711, so that a container reaches
         * the sockets it is given (its display, sound) and nothing else. */
        mkdir("/run/user", 0755);
        mkdir("/run/user/0", 0711);
        chmod("/run/user/0", 0711);
        save_log();
        read_settings();
        modules_start("");
        say("init: drivers loaded at %.3f s", uptime());
        if (access("/usr/local/bin/settings", X_OK) == 0) {
            double t0 = uptime();
            say("init: applying settings at %.3f s", t0);
            pid_t spid = fork();
            if (spid == 0) {
                execl("/usr/local/bin/settings", "settings", "--apply", (char *)NULL);
                _exit(0);
            }
            if (spid > 0) {
                waitpid(spid, NULL, 0);
            }
            say("init: settings applied at %.3f s (%.0f ms)", uptime(), (uptime() - t0) * 1000);
        }
        double t0 = uptime();
        esp_mount();
        say("init: ESP checked at %.3f s (%.0f ms)", uptime(), (uptime() - t0) * 1000);
        t0 = uptime();
        apps_init();
        say("init: applications discovered at %.3f s (%.0f ms)", uptime(), (uptime() - t0) * 1000);
    }
    say("init: opening screen at %.3f s", uptime());
    if (screen_open(rooted ? BASE_FONT : "/font.psf")) {
        screen_describe();
        screen_text(1, 0, rooted ? "LOCachy: starting home" : "LOCachy: no root (locachy-root)", FG, BG);
        screen_flush();
    }
    int nk = keys_open(), npfd = system_inputs(pfd, 8);
    activity_open();
    active_at = now_ms();
    say("init: keyboards: %d, power button and lid: %d, swap: %s, root: %s, esp: %s", nk, npfd,
        swap_dev[0] ? swap_dev : "none", rooted ? data : "none", esp_dev[0] ? esp_dev : "none");
    reboot_s = cmdline_int("locachy.reboot");
    sleeptest_s = cmdline_int("locachy.sleeptest");
    if (reboot_s > 0) {
        say("init: locachy.reboot: rebooting %d s after boot", reboot_s);
    }
    if (rooted && start(&home, home_argv) == 0) {
        if (!login_skip && !luks_passphrase_entered) {
            lock(); /* home waits under it (including when TPM auto-unlocked the disk) */
        }
        if (!locked) {
            lend(&home);
        }
    }
    if (rooted) {
        for (size_t i = 0; i < nservices; i++) {
            service_start(&services[i]);
        }
    }

    for (;;) {
        /* signals, system inputs, keyboards, home, the tasks, uevents */
        struct pollfd p[1 + 8 + MAX_KBD + MAX_ACTIVITY + 2 + MAX_TASKS * MAX_DEPTH + 1];
        struct frame *who[1 + 8 + MAX_KBD + MAX_ACTIVITY + 2 + MAX_TASKS * MAX_DEPTH + 1];
        int n = 0;
        int signal_j = n; /* SIGCHLD, SIGHUP, SIGTERM, SIGINT, SIGUSR2 */
        who[n] = NULL;
        p[n++] = (struct pollfd){ .fd = sfd, .events = POLLIN };
        for (int i = 0; i < npfd; i++) {
            who[n] = NULL;
            p[n++] = (struct pollfd){ .fd = pfd[i], .events = POLLIN };
        }
        int nkfd = keys_fds(kfd, MAX_KBD);
        for (int i = 0; i < nkfd; i++) {
            who[n] = NULL;
            p[n++] = (struct pollfd){ .fd = kfd[i], .events = POLLIN };
        }
        int activity_j = n; /* [activity_j, activity_end): input, to see that there is some */
        for (size_t i = 0; i < nafd; i++) {
            who[n] = NULL;
            p[n++] = (struct pollfd){ .fd = afd[i], .events = POLLIN };
        }
        int activity_end = n;
        if (home.pid != 0) {
            who[n] = &home;
            p[n++] = (struct pollfd){ .fd = home.ctl, .events = POLLIN };
        }
        if (locker.pid != 0) {
            who[n] = &locker;
            p[n++] = (struct pollfd){ .fd = locker.ctl, .events = POLLIN };
        }
        for (int i = 0; i < MAX_TASKS; i++) {
            for (int d = 0; d < tasks[i].depth; d++) {
                who[n] = &tasks[i].f[d];
                p[n++] = (struct pollfd){ .fd = tasks[i].f[d].ctl, .events = POLLIN };
            }
        }
        int uevent_j = -1; /* devices that appear, to load their drivers */
        if (modules_fd() >= 0) {
            uevent_j = n;
            who[n] = NULL;
            p[n++] = (struct pollfd){ .fd = modules_fd(), .events = POLLIN };
        }
        /* Wake up for the next timed thing: a service to start again, the
         * idle measurement, the debug reboot. */
        double next = -1;
        for (size_t i = 0; i < nservices; i++) {
            if (services[i].pid == 0 && (next < 0 || services[i].restart_at < next)) {
                next = services[i].restart_at;
            }
        }
        if (display_pending && display_refresh_at >= 0 && (next < 0 || display_refresh_at < next)) {
            next = display_refresh_at;
        }
        if (lock_start_at >= 0 && (next < 0 || lock_start_at < next)) {
            next = lock_start_at;
        }
        double idle_at = idle_lock_at();
        if (idle_at >= 0 && (next < 0 || idle_at < next)) {
            next = idle_at;
        }
        if (reboot_s > 0) {
            double at = now_ms() + ((measured.t == 0 ? MEASURE_FROM : reboot_s) - uptime()) * 1e3;
            if (next < 0 || at < next) {
                next = at;
            }
        }
        int timeout = next < 0 ? -1 : next > now_ms() ? (int)(next - now_ms()) + 1 : 0;
        if (apps_display_state() == APPS_DISPLAY_ACTIVE && shared_display_task() == NULL &&
            (timeout < 0 || timeout > 20)) {
            timeout = 20;
        }
        if (apps_busy() && (timeout < 0 || timeout > 100)) {
            timeout = 100;
        }
        int ready = poll(p, (nfds_t)n, timeout);
        if (refresh_display()) {
            continue; /* take_back consumed control messages: rebuild poll */
        }
        apps_tick();
        /* What poll found ready may be stale after it (home's message read by
         * take_back, then waited for again while home is frozen): poll again. */
        if ((ready == 0 || !apps_busy()) && sync_display()) {
            continue;
        }
        for (size_t i = 0; i < nservices; i++) {
            if (services[i].pid == 0 && services[i].restart_at <= now_ms()) {
                service_start(&services[i]);
            }
        }
        if (locked && locker.pid == 0 && lock_start_at >= 0 && lock_start_at <= now_ms()) {
            lock_start();
            continue;
        }
        if (idle_lock_at() >= 0 && idle_lock_at() <= now_ms()) {
            say("init: no input for %d s: locking", lock_after);
            active_at = now_ms(); /* without the lock program: not again at once */
            lock();
            continue;
        }
        if (reboot_s > 0 && measured.t == 0 && uptime() >= MEASURE_FROM) {
            if (sleeptest_s > 0) {
                sleep_test(sleeptest_s, &measured);
            } else {
                sample(&measured);
            }
            continue;
        }
        if (reboot_s > 0 && measured.t != 0 && uptime() >= reboot_s) {
            struct sample end;
            sample(&end);
            report(&measured, &end);
            say("init: locachy.reboot: rebooting now");
            restart();
        }
        if (ready <= 0) {
            continue;
        }
        /* Handle one ready fd, then poll again: handling it can change
         * which fds there are. */
        for (int j = 0; j < n; j++) {
            if (j == signal_j) {
                struct signalfd_siginfo si;
                if ((p[j].revents & POLLIN) && read(sfd, &si, sizeof si) == sizeof si) {
                    if (si.ssi_signo == SIGCHLD) {
                        reap();
                    } else if (si.ssi_signo == SIGHUP) {
                        if (rooted) {
                            read_settings();
                        }
                    } else if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                        restart();
                    } else if (si.ssi_signo == SIGUSR2) {
                        power_off();
                    }
                    break;
                }
                continue;
            }
            if (j == uevent_j) {
                if (p[j].revents & POLLIN) {
                    if (modules_event()) display_changed();
                    input_refresh(); /* an input device may have come or gone */
                    /* Up with the lid closed (docked), and the other screen gone. */
                    if (lid_closed() && !docked()) {
                        say("init: the lid closed, no other screen");
                        lock_wait();
                        suspend(1);
                    }
                    break;
                }
                continue;
            }
            if (who[j] != NULL && (p[j].revents & (POLLIN | POLLHUP | POLLERR))) {
                message(who[j]);
                break;
            }
            if (who[j] == NULL && j <= npfd && (p[j].revents & POLLIN)) { /* 1..npfd: system inputs */
                struct input_event ev;
                if (read(p[j].fd, &ev, sizeof ev) != sizeof ev) {
                    break;
                }
                if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1) {
                    say("init: power button");
                    power_off();
                } else if (ev.type == EV_KEY && (ev.code == KEY_VENDOR || ev.code == KEY_PROG1) && ev.value == 1) {
                    if (!locked) {
                        say("init: ThinkVantage button -> home");
                        go_home();
                    }
                } else if (ev.type == EV_SW && ev.code == SW_LID && ev.value == 1) {
                    lock_wait(); /* closed: locked, whether it sleeps or not */
                    if (docked()) {
                        say("init: lid closed, another screen connected: not sleeping");
                    } else {
                        say("init: lid closed");
                        suspend(1);
                    }
                }
                break;
            }
            if (j > npfd && j < activity_j && (p[j].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                input_refresh();
                break;
            }
            if (j >= activity_j && j < activity_end && p[j].revents) {
                if (p[j].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    input_refresh(); /* the device has gone */
                } else {
                    char events[64 * sizeof(struct input_event)];
                    while (read(p[j].fd, events, sizeof events) > 0) {
                    }
                    active_at = now_ms();
                }
                break;
            }
            if (who[j] == NULL && (p[j].revents & POLLIN)) {
                int key = keys_event(p[j].fd);
                if (key == K_HOME && !locked) {
                    go_home();
                } else if (key == K_ACT_LOCK) {
                    lock();
                }
                break;
            }
        }
    }
}
