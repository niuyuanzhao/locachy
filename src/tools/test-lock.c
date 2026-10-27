/* progs/lock.c's own loop: the keys typed, what is shown, and when it exits
 * 0 (unlocks), against PAM services of its own (pam_start_confdir, as
 * test-auth): locachy-lock passes "secret" (unless the account is "locked",
 * as pam_faillock refuses one before asking), locachy-lock-fingerprint says
 * something and waits, passwd takes a new password. The system bus is a
 * socket of its own, listening or not. No screen, keyboard or process 1: the
 * screen is a grid of text, keys are typed once what they wait for is shown.
 * Nothing of the system's PAM configuration or passwords.
 * test-lock SCRATCH_DIR (a new directory; as root, for pam_exec).
 * Build with -DAUTH_TEST, progs/auth.c and -lpam. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <shadow.h>
static struct spwd *fixture_getspnam(const char *name);
#define getspnam fixture_getspnam
#define main lock_main
#include "../progs/lock.c"
#include "i18n-stub.h"
#undef main
#undef getspnam
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

extern const char *auth_confdir;
static char dir[PATH_MAX], confdir[PATH_MAX + 8];

/* The account: a password ("x": shadow's hashes are not looked at), or none. */
static char shadow_pw[8] = "x";
static struct spwd *fixture_getspnam(const char *name)
{
    static struct spwd sp;
    sp.sp_namp = (char *)name;
    sp.sp_pwdp = shadow_pw;
    return &sp;
}

static int fingerprint_on, face_on;
int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    fn("unlock_fingerprint", fingerprint_on ? "1" : "0", arg);
    fn("unlock_face", face_on ? "1" : "0", arg);
    return 0;
}

#define COLS 100
#define ROWS 30
static char grid[ROWS][COLS + 1];
int screen_open(const char *font) { (void)font; return 1; }
int keys_open(void) { return 1; }
int control_fd(void) { return -1; }
int control_send(int sock, const char *msg, size_t len, const int *fds, int nfds)
{ (void)sock; (void)msg; (void)len; (void)fds; (void)nfds; abort(); }
int screen_cols(void) { return COLS; }
int screen_rows(void) { return ROWS; }
int ui_cells(const char *s) { return (int)strlen(s); }
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
void screen_flush(void) {}
void screen_fill(int col, int row, int cols, int rows, uint32_t rgb)
{
    (void)rgb;
    for (int r = row; r < row + rows && r < ROWS; r++)
        for (int c = col; c < col + cols && c < COLS; c++) grid[r][c] = ' ';
}
void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    (void)fg; (void)bg;
    for (int i = 0; s[i] && row >= 0 && row < ROWS && col + i < COLS; i++) grid[row][col + i] = s[i];
}

static int shown(const char *text)
{
    for (int r = 0; r < ROWS; r++) {
        grid[r][COLS] = '\0';
        if (strstr(grid[r], text)) return 1;
    }
    return 0;
}

/* What is typed: each group once its text is shown ("" at once) and, with
 * after_ms, that long after the step before; then (if set) is done first. */
static struct { const char *when, *keys; void (*then)(void); int after_ms; } script[4];
static int step_at, key_at;
static double t0, step_since;

int key_poll_many(struct pollfd *extra, size_t count, int timeout_ms)
{
    (void)timeout_ms;
    if (now_ms() - t0 > 10000) {
        fprintf(stderr, "stuck at step %d; the screen:\n", step_at);
        for (int r = 0; r < ROWS; r++) fprintf(stderr, "%s\n", grid[r]);
        abort();
    }
    if (count > 0 && poll(extra, count, 20) > 0) return 0;
    if (count == 0) usleep(20000);
    for (size_t i = 0; i < count; i++) extra[i].revents = 0;
    if (script[step_at].keys == NULL || (key_at == 0 && (!shown(script[step_at].when) ||
        now_ms() - step_since < script[step_at].after_ms))) return 0;
    if (key_at == 0 && script[step_at].then) script[step_at].then();
    if (script[step_at].keys[0] == '\0') {
        step_at++;
        step_since = now_ms();
        return 0;
    }
    int key = (unsigned char)script[step_at].keys[key_at++];
    if (script[step_at].keys[key_at] == '\0') {
        step_at++;
        step_since = now_ms();
        key_at = 0;
    }
    return key;
}

static void put(const char *name, const char *text, int mode, int in_confdir)
{
    char path[2 * PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", in_confdir ? confdir : dir, name);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
    assert(chmod(path, (mode_t)mode) == 0);
}

/* The system bus: a socket listening at bus_path (it starts with the lock
 * screen at boot). */
static char bus_path[sizeof ((struct sockaddr_un *)0)->sun_path], finger_pid[PATH_MAX + 16];
static int bus_fd = -1;
static void start_bus(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    assert(access(finger_pid, F_OK) != 0); /* its module has not run yet */
    memcpy(a.sun_path, bus_path, sizeof a.sun_path);
    bus_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(bus_fd >= 0 && bind(bus_fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(bus_fd, 8) == 0);
}

static double unlocked_at;
static void unlock_account(void)
{
    char path[2 * PATH_MAX];
    snprintf(path, sizeof path, "%s/locked", dir);
    assert(unlink(path) == 0);
    unlocked_at = now_ms();
}

static int lines_in(const char *name)
{
    char path[2 * PATH_MAX], line[64];
    int n = 0;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) n++;
    if (f) fclose(f);
    return n;
}

/* The device of a case (ways[1], the fingerprint; ways[2], the face), its
 * script's files (NAME.pid, NAME.runs) and its runs before the case. */
static int dev;
static const char *dev_name;
static int dev_runs;

static int runs_of_dev(void)
{
    char name[32];
    snprintf(name, sizeof name, "%s.runs", dev_name);
    return lines_in(name);
}

/* The device's last run: its module's PAM process must have ended; its
 * command (a session of its own, pam_exec) is ended here. */
static void end_dev(void)
{
    char path[2 * PATH_MAX];
    int pid = 0, pam = 0;
    snprintf(path, sizeof path, "%s/%s.pid", dir, dev_name);
    FILE *f = fopen(path, "r");
    assert(f && fscanf(f, "%d %d", &pid, &pam) == 2 && pid > 0 && pam > 0);
    fclose(f);
    assert(kill(pam, 0) < 0 && errno == ESRCH);
    kill(-pid, SIGKILL);
}

static void stopped(void)
{
    assert(ways[dev].idle && ways[dev].a.pid == 0 && runs_of_dev() == dev_runs + 1);
    end_dev();
}
static void not_again(void) { assert(ways[dev].idle && runs_of_dev() == dev_runs + 1); }
static void again(void) { assert(!ways[dev].idle && runs_of_dev() == dev_runs + 2); }

/* Run the lock from the start with this script; it must exit 0. */
static void run(void)
{
    memset(grid, ' ', sizeof grid);
    for (int i = 0; i < NWAYS; i++) {
        memset(&ways[i].a, 0, sizeof ways[i].a);
        ways[i].on = ways[i].rc == NULL;
        ways[i].again_at = 0;
        ways[i].idle = 0;
    }
    setting = 0;
    submitted = 0;
    status_line[0] = '\0';
    forget_typed();
    step_at = key_at = 0;
    t0 = step_since = now_ms();
    assert(lock_main() == 0);
    for (int i = 0; i < NWAYS; i++) assert(ways[i].a.pid == 0);
    assert(ntyped == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    setvbuf(stdout, NULL, _IOLBF, 0);
    assert(mkdir(argv[1], 0700) == 0 && realpath(argv[1], dir));
    snprintf(confdir, sizeof confdir, "%s/pam.d", dir);
    assert(mkdir(confdir, 0700) == 0);
    auth_confdir = confdir;
    assert(snprintf(bus_path, sizeof bus_path, "%s/bus", dir) < (int)sizeof bus_path);
    snprintf(finger_pid, sizeof finger_pid, "%s/finger.pid", dir);
    system_bus = bus_path;
    char text[2 * PATH_MAX + 300];

    put("password.sh", "#!/bin/bash\nIFS= read -r -d '' pw\n[ \"$pw\" = secret ]\n", 0755, 0);
    snprintf(text, sizeof text, "#!/bin/bash\necho run >> %s/runs\n[ -e %s/locked ] || exit 0\n"
             "echo 'The account is locked due to 3 failed logins.'\necho '(10 minutes left to unlock)'\nexit 1\n", dir, dir);
    put("locked.sh", text, 0755, 0);
    snprintf(text, sizeof text, "#!/bin/bash\necho $$ $PPID > %s/finger.pid\necho $$ >> %s/finger.runs\n"
             "echo 'Place your finger on the reader'\nsleep 30\nexit 1\n", dir, dir);
    put("finger.sh", text, 0755, 0);
    snprintf(text, sizeof text, "#!/bin/bash\necho $$ $PPID > %s/face.pid\necho $$ >> %s/face.runs\n"
             "echo 'Look at the camera'\nsleep 30\nexit 1\n", dir, dir);
    put("face.sh", text, 0755, 0);
    snprintf(text, sizeof text, "#!/bin/bash\nIFS= read -r -d '' pw\nprintf '%%s' \"$pw\" > %s/changed.to\n", dir);
    put("change.sh", text, 0755, 0);
    /* As Arch's system-auth: a wrong password is "Authentication failure". */
    snprintf(text, sizeof text, "auth requisite pam_exec.so stdout quiet %s/locked.sh\n"
             "auth [success=1 default=ignore] pam_exec.so expose_authtok quiet %s/password.sh\n"
             "auth requisite pam_deny.so\nauth required pam_permit.so\naccount required pam_permit.so\n", dir, dir);
    put("locachy-lock", text, 0644, 1);
    snprintf(text, sizeof text, "auth required pam_exec.so stdout quiet %s/finger.sh\naccount required pam_permit.so\n", dir);
    put("locachy-lock-fingerprint", text, 0644, 1);
    snprintf(text, sizeof text, "auth required pam_exec.so stdout quiet %s/face.sh\naccount required pam_permit.so\n", dir);
    put("locachy-lock-face", text, 0644, 1);
    snprintf(text, sizeof text, "password required pam_exec.so expose_authtok quiet %s/change.sh\n", dir);
    put("passwd", text, 0644, 1);

    /* Typed before PAM asks, then a wrong one, then the right one. */
    script[0].when = "";
    script[0].keys = "wrong\n";
    script[1].when = "Wrong password.";
    script[1].keys = "secre\bet\n";
    script[2].keys = NULL;
    run();
    assert(shown("Password:") && shown("Checking..."));
    puts("PASS typed before PAM asks, a wrong password refused and shown, Backspace, the right one unlocks");

    /* The fingerprint at boot: the system bus is not up yet, so it waits
     * (its module would fail at once and be tried only 30 s later), and
     * starts as soon as the bus listens. Its words shown; the password
     * unlocks and the waiting fingerprint's PAM process is ended. (pam_exec
     * runs its command in a session of its own, which process 1 ends with
     * the lock's cgroup.) */
    fingerprint_on = 1;
    char path[2 * PATH_MAX];
    snprintf(path, sizeof path, "%s", finger_pid);
    script[0].when = "Fingerprint: waiting for the system bus";
    script[0].keys = "";
    script[0].then = start_bus;
    script[1].when = "Fingerprint: Place your finger on the reader";
    script[1].keys = "secret\n";
    script[2].keys = NULL;
    run();
    script[0].then = NULL;
    FILE *f = fopen(path, "r");
    int pid = 0, pam = 0;
    assert(f && fscanf(f, "%d %d", &pid, &pam) == 2 && pid > 0 && pam > 0);
    fclose(f);
    assert(kill(pam, 0) < 0 && errno == ESRCH);
    kill(-pid, SIGKILL);
    puts("PASS the fingerprint waits for the system bus at boot and starts as soon as it listens");
    puts("PASS with the fingerprint on, its module's words are shown; the password unlocks and ends it");

    /* With the bus up from the start (a lock after boot), it starts at once. */
    unlink(path);
    script[0].when = "Fingerprint: Place your finger on the reader";
    script[0].keys = "secret\n";
    script[1].keys = NULL;
    run();
    assert(!shown("waiting for the system bus"));
    f = fopen(path, "r");
    assert(f && fscanf(f, "%d %d", &pid, &pam) == 2);
    fclose(f);
    kill(-pid, SIGKILL);
    puts("PASS with the system bus up, the fingerprint starts at once");

    /* Nobody at the lock: some time after its start or the last key (here
     * one second; two minutes on the machine, GNOME's) a device (the
     * fingerprint, then the face) stops, its PAM process ended, and it is
     * not tried again while nobody is there. A key starts it again. */
    idle_ms = 1000;
    for (dev = 1; dev <= 2; dev++) {
        char idle_text[64], start_text[64];
        dev_name = dev == 1 ? "finger" : "face";
        fingerprint_on = dev == 1;
        face_on = dev == 2;
        snprintf(idle_text, sizeof idle_text, "%s: Click or press a key", ways[dev].label);
        snprintf(start_text, sizeof start_text, "%s: %s", ways[dev].label,
                 dev == 1 ? "Place your finger on the reader" : "Look at the camera");
        dev_runs = runs_of_dev();
        script[0].when = idle_text;
        script[0].keys = "";
        script[0].then = stopped;
        script[1].when = idle_text;
        script[1].after_ms = 2500; /* longer than the 2 s before a way is tried again */
        script[1].keys = "\033";
        script[1].then = not_again;
        script[2].when = start_text;
        script[2].keys = "secret\n";
        script[2].then = again;
        script[3].keys = NULL;
        run();
        end_dev();
        for (int i = 0; i < 3; i++) {
            script[i].then = NULL;
            script[i].after_ms = 0;
        }
        printf("PASS nobody at the lock: the %s stops after the idle time and is not tried again; "
               "a key starts it again\n", dev == 1 ? "fingerprint" : "face");
    }
    idle_ms = 2 * 60 * 1000;
    fingerprint_on = face_on = 0;

    /* Esc clears what is typed. */
    script[0].when = "Password:";
    script[0].keys = "nope\033secret\n";
    script[1].keys = NULL;
    run();
    puts("PASS Esc clears what is typed");

    /* A locked account (pam_faillock): refused before asking, its words
     * shown, tried again after 2 s, not at once over and over. */
    put("locked", "", 0644, 0);
    int runs = lines_in("runs");
    script[0].when = "(10 minutes left to unlock)";
    script[0].keys = "";
    script[0].then = unlock_account;
    script[1].when = "";
    script[1].keys = "secret\n";
    script[2].keys = NULL;
    run();
    script[0].then = NULL;
    assert(lines_in("runs") == runs + 2 && now_ms() - unlocked_at > 1000);
    puts("PASS a locked account: its module's words shown, tried again after 2 s; then the password unlocks");

    /* No usable password: one is set first (passwd), then it unlocks. */
    strcpy(shadow_pw, "!");
    fingerprint_on = 1; /* not while setting one */
    script[0].when = "There is no password yet";
    script[0].keys = "n3w pass\n";
    script[1].keys = NULL;
    run();
    assert(!ways[1].on);
    snprintf(path, sizeof path, "%s/changed.to", dir);
    f = fopen(path, "r");
    assert(f && fgets(text, sizeof text, f) && strcmp(text, "n3w pass") == 0);
    fclose(f);
    puts("PASS an account without a usable password sets one first (PAM passwd), then it unlocks");
    puts("ALL PASSED");
    return 0;
}
