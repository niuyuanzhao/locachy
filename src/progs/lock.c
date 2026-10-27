/*
 * lock (/usr/local/bin/lock): the lock screen. Process 1 starts it on top of
 * everything (at boot unless rc.conf has login_skip="1", on key_lock, before
 * the machine sleeps with the lid closed) and unlocks when it exits 0; any
 * other end starts it again. It is a PAM client: one transaction for each
 * way in, side by side, and the first to succeed unlocks:
 *   locachy-lock              the user's password, as a local login
 *   locachy-lock-fingerprint  with rc.conf unlock_fingerprint="1" (fprintd)
 *   locachy-lock-face         with rc.conf unlock_face="1" (howdy)
 * What their modules say is shown as they say it. The user is uid 1000's;
 * one without a usable password sets one first (PAM's passwd service): there
 * is always a password. pam_fprintd reaches fprintd through the system bus,
 * which starts with the lock screen at boot: the fingerprint begins once the
 * bus takes connections (before, the module fails at once, "Authentication
 * information cannot be recovered").
 * A device (fingerprint, face) runs only while someone is at the lock: from
 * its start or the last key or pointer input, for two minutes, as GNOME's
 * unlock dialog cancels its verification after IDLE_TIMEOUT. Then it stops
 * and the next input starts it again. Kept going unattended, a reader is busy
 * all the time, and libfprint refuses it after about three minutes ("Device
 * disabled to prevent overheating") until it has been idle for minutes.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <poll.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"
#include "auth.h"

#define FG     0xD0D0D0
#define BG     0x101820
#define ACCENT 0x80C0FF
#define ERROR  0xFF9080

static struct way {
    const char *label, *service, *rc; /* rc: the rc.conf switch; NULL: always */
    int on;
    struct auth a;
    double again_at;                  /* start it again then (ms), after a failure */
    int answered;                     /* the password typed in this transaction */
    double started_at;                /* this transaction (ms) */
    int bus;                          /* its module needs the system bus */
    int idle;                         /* a device stopped: nobody at the lock */
} ways[] = {
    { N_("Password"), "locachy-lock", NULL, 1, { 0 }, 0, 0, 0, 0, 0 },
    { N_("Fingerprint"), "locachy-lock-fingerprint", "unlock_fingerprint", 0, { 0 }, 0, 0, 0, 1, 0 },
    { N_("Face"), "locachy-lock-face", "unlock_face", 0, { 0 }, 0, 0, 0, 0, 0 },
};
static const char *system_bus = "/run/dbus/system_bus_socket"; /* D-Bus's default */
static double idle_ms = 2 * 60 * 1000; /* GNOME's IDLE_TIMEOUT (unlockDialog.js) */
static double active_at;               /* the lock's start or its last input (ms) */
#define NWAYS ((int)(sizeof ways / sizeof ways[0]))

static char user[64];
static int setting;          /* no usable password: setting one first */
static char typed[256];      /* the password as typed */
static size_t ntyped;
static int submitted;        /* Enter pressed: give it when the password is asked */
static char status_line[256];

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void on_rc(const char *name, const char *value, void *arg)
{
    (void)arg;
    for (int i = 0; i < NWAYS; i++) {
        if (ways[i].rc && strcmp(name, ways[i].rc) == 0) {
            ways[i].on = strcmp(value, "1") == 0;
        }
    }
}

/* No password, or one that cannot be used (locked, "!" or "*"). */
static int no_usable_password(void)
{
    struct spwd *sp = getspnam(user);
    return sp == NULL || sp->sp_pwdp == NULL || sp->sp_pwdp[0] == '\0' ||
           sp->sp_pwdp[0] == '!' || sp->sp_pwdp[0] == '*';
}

/* The system bus takes connections: its daemon listens. */
static int system_bus_up(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), up = 0;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", system_bus);
    if (fd >= 0) {
        up = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
        close(fd);
    }
    return up;
}

static void start_way(struct way *w)
{
    if (w->bus && !system_bus_up()) {
        snprintf(w->a.message, sizeof w->a.message, "%s", _("waiting for the system bus"));
        w->again_at = now_ms() + 250;
        return;
    }
    w->again_at = 0;
    w->answered = 0;
    w->started_at = now_ms();
    if (auth_start(&w->a, setting && w == &ways[0] ? "passwd" : w->service, user,
                   setting && w == &ways[0] ? AUTH_CHANGE : AUTH_AUTHENTICATE) < 0) {
        snprintf(w->a.message, sizeof w->a.message, "%s", _("cannot start (PAM)"));
        if (w == &ways[0]) {
            snprintf(status_line, sizeof status_line, "%s", w->a.message);
        }
        w->again_at = now_ms() + 2000;
    }
}

static void forget_typed(void)
{
    explicit_bzero(typed, sizeof typed);
    ntyped = 0;
}

static void centered(int row, const char *text, uint32_t fg)
{
    int col = (screen_cols() - ui_cells(text)) / 2;
    screen_text(col > 0 ? col : 0, row, text, fg, BG);
}

static void draw(void)
{
    char line[512], when[64];
    int rows = screen_rows(), r = rows / 3;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    screen_fill(0, 0, screen_cols(), rows, BG);
    screen_bar(_("locked"), NULL);
    strftime(when, sizeof when, "%H:%M", &tm);
    centered(r++, when, FG);
    strftime(when, sizeof when, "%A %Y-%m-%d", &tm);
    centered(r++, when, FG);
    r++;
    centered(r++, user, ACCENT);
    r++;
    struct auth *p = &ways[0].a;
    const char *ask = p->asking ? p->prompt : setting ? _("New password:") : _("Password:");
    char stars[33];
    size_t n = 0;
    for (size_t i = 0; i < ntyped && n < 32; i++) n += ((unsigned char)typed[i] & 0xC0) != 0x80;
    memset(stars, '*', n);
    stars[n] = '\0';
    snprintf(line, sizeof line, "%s %s_", ask, stars);
    centered(r++, line, FG);
    for (int i = 1; i < NWAYS; i++) { /* stopped: GNOME's lock screen hint */
        if (ways[i].on) {
            snprintf(line, sizeof line, "%s: %s", _(ways[i].label),
                     ways[i].idle ? _("Click or press a key") :
                     ways[i].a.message[0] ? ways[i].a.message :
                     ways[i].a.pid ? _("waiting") : _("starting again"));
            centered(r++, line, FG);
        }
    }
    r++;
    centered(r, status_line, ERROR);
    /* TRANSLATORS: Enter and Esc are key names; keep them. */
    centered(rows - 2, setting ? _("There is no password yet: type one, then again.   Esc: clear") :
             _("Enter: unlock   Esc: clear"), FG);
    screen_flush();
}

/* A way has ended: unlocked (1), or what to show and when to try again: the
 * password at once when it was typed; otherwise (pam_faillock refusing a
 * locked account before asking, a fingerprint not matched) after 2 s, or
 * after 30 s for a fingerprint or face that failed at once (no reader, none
 * enrolled: it says so). */
static int way_ended(struct way *w)
{
    if (w->a.status == 0) {
        return 1;
    }
    if (w == &ways[0]) {
        snprintf(status_line, sizeof status_line, "%s", w->a.message[0] && strcmp(w->a.message,
                 "Authentication failure") != 0 ? w->a.message :
                 setting ? _("Not set; again.") : _("Wrong password."));
    }
    if (w == &ways[0] && w->answered) {
        start_way(w);
    } else {
        w->again_at = now_ms() + (w != &ways[0] && now_ms() - w->started_at < 2000 ? 30000 : 2000);
    }
    return 0;
}

static void type_key(int key)
{
    struct auth *p = &ways[0].a;
    if (key == '\n') {
        if (p->asking) {
            auth_answer(p, typed);
            ways[0].answered = 1;
            forget_typed();
            snprintf(status_line, sizeof status_line, "%s", _("Checking..."));
        } else {
            submitted = 1;
        }
    } else if (key == '\b' || key == 0x7F) {
        while (ntyped > 1 && ((unsigned char)typed[ntyped - 1] & 0xC0) == 0x80) ntyped--; /* a whole character */
        if (ntyped > 0) {
            typed[--ntyped] = '\0';
        }
    } else if (key == 27 || key == 0x15) { /* Esc, Ctrl+U */
        forget_typed();
        submitted = 0;
    } else if (keys_typed(key) && ntyped + strlen(keys_typed(key)) < sizeof typed) {
        memcpy(typed + ntyped, keys_typed(key), strlen(keys_typed(key)));
        ntyped += strlen(keys_typed(key));
        typed[ntyped] = '\0';
        status_line[0] = '\0';
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    struct passwd *pw = getpwuid(1000);
    snprintf(user, sizeof user, "%s", pw ? pw->pw_name : "");
    rc_read(on_rc, NULL);
    setting = user[0] && no_usable_password();
    if (setting) {
        for (int i = 1; i < NWAYS; i++) {
            ways[i].on = 0;
        }
    }
    if (!screen_open(BASE_FONT)) {
        return 1;
    }
    keys_open();
    if (!user[0]) {
        snprintf(status_line, sizeof status_line, "%s", _("There is no user (uid 1000) to unlock for."));
    }
    active_at = now_ms();
    for (int i = 0; i < NWAYS && user[0]; i++) {
        if (ways[i].on) {
            start_way(&ways[i]);
        }
    }
    draw();
    int ctl = control_fd();
    if (ctl >= 0) {
        control_send(ctl, "locked", 6, NULL, 0); /* on the screen: process 1 may let the machine sleep */
    }
    for (;;) {
        struct pollfd extra[NWAYS];
        struct way *of[NWAYS];
        size_t n = 0;
        int wait = 1000;
        for (int i = 0; i < NWAYS; i++) {
            if (ways[i].a.pid) {
                of[n] = &ways[i];
                extra[n++] = (struct pollfd){ .fd = ways[i].a.from, .events = POLLIN };
            } else if (ways[i].on && ways[i].again_at > 0) {
                int left = (int)(ways[i].again_at - now_ms());
                wait = left < wait ? (left > 0 ? left : 0) : wait;
            }
        }
        int key = key_poll_many(extra, n, wait);
        if (key != 0 && key != K_SHOWN) {
            active_at = now_ms();
        }
        for (size_t i = 0; i < n; i++) {
            struct way *w = of[i];
            if (extra[i].revents && auth_read(&w->a) && w->a.done && way_ended(w)) {
                for (int k = 0; k < NWAYS; k++) {
                    auth_stop(&ways[k].a);
                }
                forget_typed();
                return 0;
            }
        }
        for (int i = 0; i < NWAYS; i++) {
            struct way *w = &ways[i];
            if (!w->on || !user[0]) {
                continue;
            }
            if (w != &ways[0] && now_ms() - active_at >= idle_ms) {
                if (!w->idle) {
                    auth_stop(&w->a);
                    w->again_at = 0;
                    w->idle = 1;
                }
            } else if (w->idle) {
                w->idle = 0;
                start_way(w);
            } else if (!w->a.pid && w->again_at > 0 && now_ms() >= w->again_at) {
                start_way(w);
            }
        }
        if (submitted && ways[0].a.asking) {
            submitted = 0;
            type_key('\n');
        }
        if (key != 0 && key != K_SHOWN && key != K_POINTER) {
            type_key(key);
        }
        draw();
    }
}
