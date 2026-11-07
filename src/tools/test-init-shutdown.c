/* Process 1's power off and reboot with a system job running (init.c's
 * shutdown_request, shutdown_look, person_asked, battery_power_off): none
 * runs, it goes at once; one runs, it waits, watching the job cgroups'
 * cgroup.events (inotify, no timer), sees the next queued one start, goes on
 * when none has run for JOBS_QUIET_MS; asked again it goes ahead at once; the
 * critical battery hibernates, or without a resume device waits
 * BATTERY_JOB_WAIT_MS at most. The job cgroups, state files and batteries
 * are files in a scratch directory (test-init-shutdown DIR); the clock,
 * reboot, kill, mount and the sleep state file are doubled. Last, poweroff
 * and reboot (link/) run as root of a user and PID namespace, whose process
 * 1 here says which signal each sent. No device, no real shutdown. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
static int fixture_open(const char *, int, ...);
static ssize_t fixture_write(int, const void *, size_t);
static int fixture_close(int);
static int fixture_clock_gettime(clockid_t, struct timespec *);
static int fixture_klogctl(int, char *, int);
static void fixture_sync(void);
static pid_t fixture_fork(void);
static int fixture_mkdir(const char *, mode_t);
static int fixture_kill(pid_t, int);
static int fixture_reboot(int);
static int fixture_swapoff(const char *);
static int fixture_mount(const char *, const char *, const char *, unsigned long, const void *);
static char root[1024], supply_root[1200], jobs_cgroup[1200], jobs_dir[1200];
#define POWER_SUPPLY_CLASS supply_root
#define JOBS_CGROUP jobs_cgroup
#define JOBS_DIR jobs_dir
#define open fixture_open
#define write fixture_write
#define close fixture_close
#define clock_gettime fixture_clock_gettime
#define klogctl fixture_klogctl
#define sync fixture_sync
#define fork fixture_fork
#define mkdir fixture_mkdir
#define kill fixture_kill
#define reboot fixture_reboot
#define swapoff fixture_swapoff
#define mount fixture_mount
#define main init_main
#include "../init/init.c"
#undef main
#undef mount
#undef swapoff
#undef reboot
#undef kill
#undef mkdir
#undef fork
#undef sync
#undef klogctl
#undef clock_gettime
#undef close
#undef write
#undef open

#define STATE_FD 10001

static double mono_s, boot_s;
static int reboots, last_how, terms, disk_writes, refreshes;
static char log_text[32768], screen_lines[4096];

void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = strlen(log_text);
    vsnprintf(log_text + n, sizeof log_text - n, fmt, ap);
    va_end(ap);
    n = strlen(log_text);
    if (n + 1 < sizeof log_text) {
        log_text[n] = '\n';
        log_text[n + 1] = '\0';
    }
}
/* Process 1's own screen: what is written on it. */
void screen_fill(int col, int row, int ncols, int nrows, uint32_t rgb)
{ (void)col; (void)row; (void)ncols; (void)nrows; (void)rgb; screen_lines[0] = '\0'; }
void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    (void)col; (void)row; (void)fg; (void)bg;
    size_t n = strlen(screen_lines);
    snprintf(screen_lines + n, sizeof screen_lines - n, "%s\n", s);
}
int screen_cols(void) { return 170; }
int screen_rows(void) { return 48; }
void screen_flush(void) {}
/* What arrange() reaches with no program on any screen. */
int screen_refresh(void) { refreshes++; return 1; }
int screen_places(struct screen_output *out, int max) { (void)out; (void)max; return 0; }
int screen_lend(uint32_t *lessee) { (void)lessee; abort(); }
int screen_lend_outputs(const char *const *names, int n, uint32_t *lessee) { (void)names; (void)n; (void)lessee; abort(); }
int screen_take_back(uint32_t lessee) { (void)lessee; return 0; }
int screen_drop_master(uint32_t lessee) { (void)lessee; return 0; }
void keys_leds(int on) { (void)on; }
int keys_locks(void) { return 0; }
void keys_set_locks(int locks) { (void)locks; }
int keys_sync(int *fds, int *n, int max) { (void)fds; (void)max; *n = 0; return 0; }
int keys_lend(int *fds, int max) { (void)fds; (void)max; return 0; }
void keys_take_back(const int *fds, int n) { (void)fds; (void)n; }
int *pointer_lend(size_t *n, char (*paths)[POINTER_PATH], size_t max) { (void)paths; (void)max; *n = 0; return NULL; }
void pointer_take_back(int *fds, size_t n) { (void)fds; (void)n; }
int monitor_internal(const char *name) { return !strncmp(name, "eDP", 3); }
int control_send(int sock, const char *msg, size_t len, const int *fds, int nfds)
{ (void)sock; (void)msg; (void)len; (void)fds; (void)nfds; return 0; }
int control_send_fields(int sock, const char *const *fields) { (void)sock; (void)fields; return 0; }
/* What a program's message would reach (lock_wait's): none comes here. */
int control_recv(int sock, char *msg, size_t size, int *fds, int *nfds)
{ (void)sock; (void)msg; (void)size; (void)fds; *nfds = 0; return 0; }
int control_fields(char *msg, int len, char **fields, int max) { (void)msg; (void)len; (void)fields; (void)max; return 0; }
void apps_list(char *buf, size_t size) { if (size) buf[0] = '\0'; }
void apps_display_ready(void) {}
void apps_tick(void) {}
int apps_open(const char *name, const char *target, char *why, size_t size)
{ (void)name; (void)target; (void)why; (void)size; return -1; }
int apps_action(const char *action, const char *name, char *why, size_t size)
{ (void)action; (void)name; (void)why; (void)size; return -1; }
int apps_window_open(const char *program, const char *target, char *why, size_t size)
{ (void)program; (void)target; (void)why; (void)size; return -1; }

static int fixture_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/sys/power/state")) return STATE_FD;
    if (!strncmp(path, root, strlen(root))) return open(path, flags & ~O_CREAT);
    errno = ENOENT;
    return -1;
}
static ssize_t fixture_write(int fd, const void *bytes, size_t size)
{
    if (fd != STATE_FD) return write(fd, bytes, size);
    assert(size == 4 && !memcmp(bytes, "disk", 4));
    disk_writes++;
    boot_s += 600;
    return (ssize_t)size;
}
static int fixture_close(int fd) { return fd == STATE_FD ? 0 : close(fd); }
static int fixture_clock_gettime(clockid_t id, struct timespec *out)
{
    double t = id == CLOCK_MONOTONIC ? mono_s : boot_s;
    out->tv_sec = (time_t)t;
    out->tv_nsec = (long)((t - (double)out->tv_sec) * 1e9);
    return 0;
}
static int fixture_klogctl(int action, char *buf, int size) { (void)action; (void)buf; (void)size; return -1; }
static void fixture_sync(void) {}
static pid_t fixture_fork(void) { return -1; }
static int fixture_mkdir(const char *path, mode_t mode) { (void)path; (void)mode; errno = EACCES; return -1; }
static int fixture_kill(pid_t pid, int sig) { if (pid == -1 && sig == SIGTERM) terms++; return 0; }
static int fixture_reboot(int how) { reboots++; last_how = how; return 0; }
static int fixture_swapoff(const char *path) { (void)path; errno = EINVAL; return -1; }
static int fixture_mount(const char *a, const char *b, const char *c, unsigned long d, const void *e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }

static int logged(const char *text) { return strstr(log_text, text) != NULL; }
static int shown(const char *text) { return strstr(screen_lines, text) != NULL; }
static void file_text(const char *suffix, const char *text)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", root, suffix);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}
static void directory(const char *suffix)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", root, suffix);
    assert(mkdir(path, 0700) == 0);
}
static void battery(const char *status, int percent)
{
    char text[32];
    file_text("power/BAT0/status", status);
    snprintf(text, sizeof text, "%d\n", percent * 500000);
    file_text("power/BAT0/energy_now", text);
    snprintf(text, sizeof text, "%d\n", percent);
    file_text("power/BAT0/capacity", text);
    file_text("power/AC/online", strcmp(status, "Discharging") == 0 ? "0\n" : "1\n");
}
/* A job's cgroup as the service makes it (mkdir: "populated 0"), and as
 * its processes enter it and end. */
static void job(int id, int populated)
{
    char path[64], full[1300];
    snprintf(path, sizeof path, "cgroup/job-%d", id);
    snprintf(full, sizeof full, "%s/job-%d", jobs_cgroup, id);
    if (access(full, F_OK) != 0) directory(path);
    snprintf(path, sizeof path, "cgroup/job-%d/cgroup.events", id);
    file_text(path, populated ? "populated 1\nfrozen 0\n" : "populated 0\nfrozen 0\n");
}
static void job_gone(int id)
{
    char path[1400];
    snprintf(path, sizeof path, "%s/job-%d/cgroup.events", jobs_cgroup, id);
    unlink(path);
    snprintf(path, sizeof path, "%s/job-%d", jobs_cgroup, id);
    rmdir(path);
}
static void job_state(int id, const char *kind)
{
    char path[64], text[128];
    snprintf(path, sizeof path, "jobs/%d.state", id);
    snprintf(text, sizeof text, "kind=%s\nstate=running\nstatus=-1\nfrom=packages\n", kind);
    file_text(path, text);
}
/* The watch (inotify) has something to say within ms. */
static int watched(int ms)
{
    struct pollfd w = { .fd = jobs_watch, .events = POLLIN };
    return jobs_watch >= 0 && poll(&w, 1, ms) == 1;
}
static void reset_case(void)
{
    for (int id = 1; id < 10; id++) job_gone(id);
    reboots = terms = disk_writes = refreshes = 0;
    last_how = -1;
    log_text[0] = screen_lines[0] = '\0';
    assert(shutdown_how < 0 && jobs_watch < 0 && shutdown_at < 0);
    asked_at = -1;
    mono_s = 100;
    boot_s = 1000;
    resume_dev[0] = '\0';
    battery_act_at = battery_check_at = -1;
    battery("Charging", 80);
    lock_argv[0] = "/nonexistent/lock";
    locked = 0;
}

/* link/NAME [ARG] as root of a user and PID namespace: the signal its
 * process 1 got (0: none), else 100 + the script's exit status. */
static int script_signal(const char *link, const char *name, const char *arg)
{
    char script[1400];
    snprintf(script, sizeof script, "%s/%s", link, name);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        char map[64];
        uid_t uid = getuid();
        gid_t gid = getgid();
        if (unshare(CLONE_NEWUSER | CLONE_NEWPID) < 0) _exit(90);
        int fd = open("/proc/self/setgroups", O_WRONLY);
        if (fd < 0 || write(fd, "deny", 4) != 4) _exit(91);
        close(fd);
        snprintf(map, sizeof map, "0 %u 1", (unsigned)uid);
        fd = open("/proc/self/uid_map", O_WRONLY);
        if (fd < 0 || write(fd, map, strlen(map)) != (ssize_t)strlen(map)) _exit(92);
        close(fd);
        snprintf(map, sizeof map, "0 %u 1", (unsigned)gid);
        fd = open("/proc/self/gid_map", O_WRONLY);
        if (fd < 0 || write(fd, map, strlen(map)) != (ssize_t)strlen(map)) _exit(93);
        close(fd);
        pid_t init = fork();
        if (init == 0) { /* process 1 of the namespace: every signal blocked, read from a signalfd */
            sigset_t all;
            sigfillset(&all);
            sigprocmask(SIG_BLOCK, &all, NULL);
            int sfd = signalfd(-1, &all, SFD_NONBLOCK);
            pid_t sh = fork();
            if (sh == 0) {
                sigprocmask(SIG_UNBLOCK, &all, NULL);
                execl("/bin/sh", "sh", script, arg, (char *)NULL);
                _exit(127);
            }
            int status = 0, got = 0;
            waitpid(sh, &status, 0);
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo != SIGCHLD) got = (int)si.ssi_signo;
            }
            _exit(got ? got : 100 + (WIFEXITED(status) ? WEXITSTATUS(status) : 99));
        }
        int st = 0;
        waitpid(init, &st, 0);
        _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 98);
    }
    int st = 0;
    assert(waitpid(child, &st, 0) == child);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(root, sizeof root, "%s", argv[1]);
    assert(mkdir(root, 0700) == 0);
    directory("cgroup"); directory("jobs");
    directory("power"); directory("power/BAT0"); directory("power/AC");
    snprintf(jobs_cgroup, sizeof jobs_cgroup, "%s/cgroup", root);
    snprintf(jobs_dir, sizeof jobs_dir, "%s/jobs", root);
    snprintf(supply_root, sizeof supply_root, "%s/power", root);
    file_text("power/BAT0/type", "Battery\n");
    file_text("power/BAT0/energy_full", "50000000\n");
    file_text("power/BAT0/power_now", "10000000\n");
    file_text("power/BAT0/alarm", "2500000\n");
    file_text("power/AC/type", "Mains\n");
    job_state(7, "pkg-install");

    reset_case();
    power_off();
    assert(reboots == 1 && last_how == RB_POWER_OFF && terms == 1 && shutdown_how < 0 && jobs_watch < 0);
    assert(!screen_lines[0] && !logged("waiting"));
    reset_case(); job(3, 0); /* an ended job's cgroup, not yet removed */
    restart();
    assert(reboots == 1 && last_how == RB_AUTOBOOT && terms == 1 && !screen_lines[0]);
    reset_case(); snprintf(jobs_cgroup, sizeof jobs_cgroup, "%s/none", root); /* no service ever ran */
    power_off();
    assert(reboots == 1 && terms == 1);
    snprintf(jobs_cgroup, sizeof jobs_cgroup, "%s/cgroup", root);
    puts("PASS no system job runs (none, an ended one's cgroup still there, no job folder at all): power off "
         "and reboot go at once, as before");

    reset_case(); job(7, 1);
    restart();
    assert(reboots == 0 && terms == 0 && shutdown_how == RB_AUTOBOOT && jobs_watch >= 0 && shutdown_at == 100e3);
    home.pid = 4242;
    assert(showing(0) == NULL);
    home.pid = 0;
    shutdown_look(); /* the main loop: shutdown_at is due */
    assert(refreshes == 1 && reboots == 0 && terms == 0);
    assert(shown("LOCachy: rebooting once the system job ends.") && shown("System job 7 runs: pkg-install."));
    assert(shown("To go ahead now anyway: Ctrl+Alt+Del reboots; as root, poweroff --force or reboot --force."));
    assert(logged("rebooting: waiting for system job 7 (pkg-install) to end; to go ahead now"));
    assert(shutdown_at < 0 && !watched(0)); /* no timer, nothing to read: no busy loop */
    puts("PASS a job runs: the reboot waits, nothing gets the screens, process 1's own and the log name the job "
         "and how to go ahead; its cgroup.events watched, no timer");
    mono_s += 3;
    power_off(); /* poweroff without --force, while it waits */
    assert(reboots == 0 && shutdown_how == RB_POWER_OFF && logged("power off asked for, once no system job runs"));
    shutdown_look();
    assert(shown("LOCachy: powering off once the system job ends.") && shutdown_at < 0);
    puts("PASS asked again without --force: still waiting; the last request says power off or reboot");
    mono_s += 60;
    job(7, 0); /* its processes ended */
    assert(watched(1000));
    shutdown_look();
    assert(reboots == 0 && terms == 0 && shutdown_at == mono_s * 1e3 + JOBS_QUIET_MS);
    assert(shown("No system job runs now: powering off."));
    job_gone(7); /* the service removes it, and starts the next queued one */
    assert(watched(1000));
    shutdown_look();
    job(8, 0);
    assert(watched(1000));
    mono_s += 0.01;
    shutdown_look();
    assert(reboots == 0 && shutdown_at >= 0);
    job(8, 1); /* its runner entered it */
    assert(watched(1000));
    shutdown_look();
    assert(reboots == 0 && terms == 0 && shutdown_at < 0 && jobs_quiet_at < 0);
    assert(shown("System job 8 runs.") && logged("waiting for system job 8 (kind unknown)"));
    puts("PASS the job ended: 1 s more; the next queued one starting in it (its cgroup made, then entered) is "
         "waited for too; a job without a state file is named by its id");
    mono_s += 120;
    job(8, 0);
    assert(watched(1000));
    shutdown_look();
    assert(reboots == 0 && terms == 0);
    mono_s += JOBS_QUIET_MS / 1e3;
    shutdown_look(); /* shutdown_at */
    assert(reboots == 1 && last_how == RB_POWER_OFF && terms == 1 && shutdown_how < 0 && jobs_watch < 0);
    puts("PASS none has run for 1 s: only then is everything ended (SIGTERM) and the machine powered off");

    reset_case(); job(7, 1);
    restart();
    shutdown_look();
    person_asked(RB_AUTOBOOT); /* Ctrl+Alt+Del */
    assert(reboots == 1 && last_how == RB_AUTOBOOT && logged("init: Ctrl+Alt+Del"));
    assert(logged("reboot now, without waiting: system job 7 (pkg-install) is cut"));
    reset_case(); job(7, 1);
    person_asked(RB_AUTOBOOT);
    assert(reboots == 0 && shutdown_how == RB_AUTOBOOT);
    shutdown_look();
    for (int i = 0; i < 40; i++) { /* held: Delete repeats every 33 ms after 0.5 s */
        mono_s += i ? 0.033 : 0.5;
        person_asked(RB_AUTOBOOT);
    }
    assert(reboots == 0);
    mono_s += 1.5;
    person_asked(RB_AUTOBOOT); /* released, pressed again */
    assert(reboots == 1 && last_how == RB_AUTOBOOT);
    reset_case(); job(7, 1);
    restart();
    person_asked(RB_POWER_OFF); /* the power button while the reboot waits */
    assert(reboots == 1 && last_how == RB_POWER_OFF && logged("init: power button"));
    puts("PASS a person asks again while it waits (Ctrl+Alt+Del, the power button): at once, its own action; "
         "not for the same press held or seen twice");
    reset_case(); job(7, 1);
    power_off();
    shutdown_request(RB_POWER_OFF, 1, -1); /* SIG_POWEROFF_NOW: poweroff --force */
    assert(reboots == 1 && last_how == RB_POWER_OFF && logged("power off now, without waiting: system job 7"));
    reset_case(); job(7, 1);
    shutdown_request(RB_AUTOBOOT, 1, -1); /* SIG_REBOOT_NOW, not waiting yet */
    assert(reboots == 1 && last_how == RB_AUTOBOOT && terms == 1);
    puts("PASS poweroff --force, reboot --force: at once, waiting or not, the job logged as cut");

    reset_case(); job(7, 1); job_state(7, "spi-write"); snprintf(resume_dev, sizeof resume_dev, "8:3");
    battery("Discharging", 4);
    battery_check();
    battery_act();
    assert(disk_writes == 1 && reboots == 0 && shutdown_how < 0 && logged("battery critical: hibernating"));
    puts("PASS critical battery with a resume device: hibernates; the job is in the image and goes on after it");
    reset_case(); job(7, 1); battery("Discharging", 4);
    battery_check();
    assert(logged("or the computer powers off in 20 s"));
    battery_act();
    assert(disk_writes == 0 && reboots == 0 && shutdown_how == RB_POWER_OFF);
    assert(shutdown_by == mono_s * 1e3 + BATTERY_JOB_WAIT_MS && logged("battery critical: powering off"));
    shutdown_look();
    assert(shown("LOCachy: battery critical: powering off once the system job ends, in 60 s at most."));
    assert(shown("System job 7 runs: spi-write.") && shutdown_at == shutdown_by);
    mono_s += 30;
    shutdown_look();
    assert(reboots == 0);
    mono_s += 30;
    shutdown_look(); /* shutdown_at: the bound */
    assert(reboots == 1 && last_how == RB_POWER_OFF && terms == 1);
    assert(logged("POWER: the critical battery's power off waited 60 s; system job 7 (spi-write) still runs and "
                  "is cut: it can be left half done"));
    reset_case(); job(7, 1); battery("Discharging", 4);
    battery_act();
    shutdown_look();
    mono_s += 20;
    job(7, 0);
    assert(watched(1000));
    shutdown_look();
    mono_s += 1;
    shutdown_look();
    assert(reboots == 1 && terms == 1 && !logged("is cut"));
    reset_case(); battery("Discharging", 4);
    battery_act();
    assert(reboots == 1 && last_how == RB_POWER_OFF);
    reset_case(); job(7, 1);
    power_off(); /* a person's, without a bound */
    battery_power_off();
    assert(reboots == 0 && shutdown_by == mono_s * 1e3 + BATTERY_JOB_WAIT_MS);
    mono_s += 60;
    shutdown_look();
    assert(reboots == 1);
    puts("PASS critical battery without a resume device: a job gets 60 s at most (said on the screen), then the "
         "machine powers off, logging the cut; ended sooner, no cut; none: at once; a waiting power off gets "
         "the bound");

    char link[1400];
    snprintf(link, sizeof link, "%s", __FILE__);
    char *slash = strrchr(link, '/');
    assert(slash && slash - link >= 6 && !strncmp(slash - 6, "/tools", 6));
    snprintf(slash - 6, sizeof link - (size_t)(slash - 6 - link), "/link");
    assert(script_signal(link, "poweroff", NULL) == SIGUSR2);
    assert(script_signal(link, "reboot", NULL) == SIGTERM);
    assert(script_signal(link, "poweroff", "--force") == SIG_POWEROFF_NOW);
    assert(script_signal(link, "poweroff", "-f") == SIG_POWEROFF_NOW);
    assert(script_signal(link, "reboot", "--force") == SIG_REBOOT_NOW);
    assert(script_signal(link, "reboot", "--now") == 102);
    puts("PASS poweroff, reboot as root (/bin/sh): SIGUSR2, SIGTERM; --force, -f: 48, 49, the numbers process 1 "
         "reads; anything else: usage, exit 2");

    reset_case();
    char path[1400];
    const char *files[] = { "jobs/7.state", "power/BAT0/type", "power/BAT0/energy_full", "power/BAT0/power_now",
                            "power/BAT0/alarm", "power/BAT0/status", "power/BAT0/energy_now",
                            "power/BAT0/capacity", "power/AC/online", "power/AC/type" };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(path, sizeof path, "%s/%s", root, files[i]);
        assert(unlink(path) == 0);
    }
    const char *dirs[] = { "cgroup", "jobs", "power/BAT0", "power/AC", "power" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, sizeof path, "%s/%s", root, dirs[i]);
        assert(rmdir(path) == 0);
    }
    assert(rmdir(root) == 0);
    puts("PASS process 1's power off and reboot wait for a running system job");
    return 0;
}
