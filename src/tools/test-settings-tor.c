/* Settings' Tor rows (test-settings-tor.sh builds and runs this): installed
 * and enabled stay apart, Tor's choice is a front that is there, a front Tor
 * uses is never switched off or removed under it, its firewall is loaded
 * before rc.conf says Tor is on and goes with it, and bridge lines are
 * checked as Tor takes them. tor-enforce and tor-client are scripts of the
 * test in TOR_TEST_DIR (they say what they were asked); rc.conf, process 1,
 * the packages and locachy-pkg are the test's own: nothing else is touched.
 * Build with -ffunction-sections -fdata-sections -Wl,--gc-sections. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TOR_TEST_DIR
#error "TOR_TEST_DIR: the test's directory"
#endif
#define TOR_ENFORCE TOR_TEST_DIR "/tor-enforce"
#define TOR_SERVICE TOR_TEST_DIR "/tor-client"
#define TOR_STATE TOR_TEST_DIR "/state"

static int fixture_access(const char *, int);
static pid_t fixture_fork(void);
static pid_t fixture_waitpid(pid_t, int *, int);
static int fixture_kill(pid_t, int);
#define access fixture_access
#define fork fixture_fork
#define waitpid fixture_waitpid
#define kill fixture_kill
#define main settings_main
#include "../progs/settings.c"
#include "i18n-stub.h"
#undef main
#undef kill
#undef waitpid
#undef fork
#undef access

/* What is installed; whether a fork is locachy-pkg's (not run) or a script
 * of the test's (run); rc.conf's lines as written, in order. */
static int fake_tor, fake_lyrebird, fake_mihomo, fake_pkg_fork, fake_forks, hups, fail_rc;
static char rc_log[2048];
static const char *keys_in;

static int fixture_access(const char *path, int mode)
{
    if (!strcmp(path, "/usr/bin/tor")) return fake_tor ? 0 : -1;
    if (!strcmp(path, "/usr/bin/lyrebird")) return fake_lyrebird ? 0 : -1;
    if (!strcmp(path, "/usr/bin/mihomo")) return fake_mihomo ? 0 : -1;
    return access(path, mode);
}

static pid_t fixture_fork(void)
{
    if (fake_pkg_fork) {
        fake_forks++;
        return 7000 + fake_forks;
    }
    return fork();
}

static pid_t fixture_waitpid(pid_t pid, int *status, int options)
{
    if (pid > 7000 && pid <= 7000 + fake_forks) {
        if (status) *status = 0;
        return pid;
    }
    return waitpid(pid, status, options);
}

/* Process 1 told (SIGHUP); a script of the test's ended (command_output). */
static int fixture_kill(pid_t pid, int sig)
{
    if (pid == 1) {
        assert(sig == SIGHUP);
        hups++;
        return 0;
    }
    assert(pid > 1);
    return kill(pid, sig);
}

/* The lines tor-enforce was asked for, as "load mihomo;remove;". */
static void enforced(char *out, size_t size)
{
    char line[128];
    size_t n = 0;
    out[0] = '\0';
    FILE *f = fopen(TOR_TEST_DIR "/enforce.log", "r");
    while (f && fgets(line, sizeof line, f) && n < size) {
        line[strcspn(line, "\n")] = '\0';
        n += (size_t)snprintf(out + n, size - n, "%s;", line);
    }
    if (f) fclose(f);
}

int rc_set(const char *name, const char *value)
{
    char before[512];
    if (fail_rc) return -1;
    enforced(before, sizeof before);
    size_t n = strlen(rc_log);
    /* Each write with what tor-enforce had done by then. */
    snprintf(rc_log + n, sizeof rc_log - n, "%s=%s [%s]\n", name, value ? value : "(none)", before);
    return 0;
}
int rc_read(void (*fn)(const char *, const char *, void *), void *arg) { (void)fn; (void)arg; return 0; }
int monitor_internal(const char *name) { (void)name; return 0; } /* no screens here */
double uptime(void) { static double t; return t += 10; }
int screen_cols(void) { return 100; }
int screen_rows(void) { return 30; }
int ui_cells(const char *s) { return (int)strlen(s); }
void screen_fill(int x, int y, int w, int h, uint32_t color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
void screen_text(int x, int y, const char *text, uint32_t fg, uint32_t bg)
{ (void)x; (void)y; (void)text; (void)fg; (void)bg; }
void screen_flush(void) {}
int key_get(void)
{
    assert(keys_in && *keys_in);
    return (unsigned char)*keys_in++;
}
void say(const char *fmt, ...) { (void)fmt; }
/* adjust() reaches these for other rows only. */
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; abort(); }
int key_poll_timeout(int fd, int timeout) { (void)fd; (void)timeout; abort(); }
int screen_rescale(void) { abort(); }
int open_candidates(const char *type, char names[][OPEN_NAME], int max) { (void)type; (void)names; (void)max; abort(); }
int open_set_rule(const char *type, const char *value) { (void)type; (void)value; abort(); }
int open_containers(char names[][OPEN_NAME], int max) { (void)names; (void)max; abort(); }

static void script(const char *name, const char *body)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", TOR_TEST_DIR, name);
    FILE *f = fopen(path, "w");
    assert(f && fputs(body, f) >= 0 && fclose(f) == 0 && chmod(path, 0755) == 0);
}

static void reset(void)
{
    rc_log[0] = '\0';
    unlink(TOR_TEST_DIR "/enforce.log");
    unlink(TOR_TEST_DIR "/enforce.fail");
    notice[0] = '\0';
    hups = 0;
}

static void choose(int dir)
{
    selected = tr_index() + 1;
    adjust(dir);
}

static void add_bridge(const char *typed)
{
    static char keys[1200];
    snprintf(keys, sizeof keys, "%s\n", typed);
    keys_in = keys;
    tor_add_bridge();
    assert(*keys_in == '\0');
}

int main(void)
{
    char done[512], out[1024];
    assert(mkdir(TOR_TEST_DIR, 0700) == 0 || errno == EEXIST);
    script("tor-enforce", "#!/bin/sh\necho \"$*\" >>" TOR_TEST_DIR "/enforce.log\n"
                          "[ ! -e " TOR_TEST_DIR "/enforce.fail ] || exit 1\n");
    script("tor-client", "#!/bin/sh\n[ \"$1\" = --status ] && echo 'connecting through bridges: 10% conn_pt'\n");
    unlink(TOR_STATE "/bridges");

    /* Bridge lines: obfs4 and WebTunnel as Tor's Bridge takes them; plain
     * bridges (they do not hide Tor), meek, snowflake and junk are not. */
    assert(tor_bridge_line("obfs4 146.57.248.225:22 10A6CD36A537FCE513A322361547444B393989F0 "
                           "cert=K1gDtDAIcUfeLqbstggjIw2rtgIKqdIhUlHp82XRqNSq/mtAjp1BIC9vHKJ2FAEpGssTPw iat-mode=0",
                           out, sizeof out));
    assert(!strncmp(out, "obfs4 146.57.248.225:22 10A6", 28));
    assert(tor_bridge_line("  Bridge   webtunnel [2001:db8:3d87:6a0e:b0b5:bd1a:d0b8:43e]:443 "
                           "D0A51BE9E5E9C0A0D4B7F2D7E9D6B8D3B4F4F7A1 url=https://example.net/path ver=0.0.1\n",
                           out, sizeof out));
    assert(!strcmp(out, "webtunnel [2001:db8:3d87:6a0e:b0b5:bd1a:d0b8:43e]:443 "
                        "D0A51BE9E5E9C0A0D4B7F2D7E9D6B8D3B4F4F7A1 url=https://example.net/path ver=0.0.1"));
    assert(tor_bridge_line("obfs4 192.0.2.10:443 cert=abc iat-mode=0", out, sizeof out)); /* no fingerprint */
    assert(!tor_bridge_line("192.0.2.10:443 0123456789ABCDEF0123456789ABCDEF01234567", out, sizeof out));
    assert(!tor_bridge_line("bridge 192.0.2.10:443", out, sizeof out));
    assert(!tor_bridge_line("meek_lite 192.0.2.20:80 url=https://x front=y", out, sizeof out));
    assert(!tor_bridge_line("snowflake 192.0.2.3:80 2B280B23E1107BB62ABFC40DDCC8824814F80A72", out, sizeof out));
    assert(!tor_bridge_line("obfs4 192.0.2.10 cert=abc", out, sizeof out));       /* no port */
    assert(!tor_bridge_line("obfs4 192.0..10:443 cert=abc", out, sizeof out));    /* not an address */
    assert(!tor_bridge_line("obfs4 192.0.2.10:443 cert", out, sizeof out));       /* not KEY=VALUE */
    assert(!tor_bridge_line("obfs4 example.net:443 cert=abc", out, sizeof out));  /* a name: Tor takes addresses */
    assert(!tor_bridge_line("", out, sizeof out) && !tor_bridge_line("obfs4", out, sizeof out));
    puts("PASS bridge lines: obfs4 and WebTunnel only, as Tor's Bridge takes them");

    /* Not installed: the choice is refused, nothing written. */
    reset();
    choose(+1);
    assert(!tor_on && !rc_log[0] && !hups && strstr(notice, "Tor is not installed"));
    puts("PASS enabling without the package is refused");

    /* Installing is a package job only: it does not switch Tor on. */
    reset();
    fake_pkg_fork = 1;
    selected = tr_index();
    adjust(+1);
    fake_pkg_fork = 0;
    assert(pkg_job && !strcmp(pkg_action, "install") && !strcmp(pkg_name, TOR_PACKAGES) && !tor_on && !rc_log[0]);
    assert(!strcmp(pkg_title(), "Tor (tor, lyrebird)") && strstr(tor_status(), "installing"));
    choose(+1);
    assert(!tor_on && strstr(notice, "still being installed"));
    pkg_check();
    assert(!pkg_job && strstr(notice, "Tor (tor, lyrebird) installed"));
    fake_tor = 1;
    assert(!strcmp(tor_status(), "off: the network goes direct"));
    puts("PASS installing is apart from switching on");

    /* No front at all: refused, never direct; no firewall, no rc.conf. */
    reset();
    choose(+1);
    assert(!tor_on && !rc_log[0] && strstr(notice, "Tor never goes out directly"));
    enforced(done, sizeof done);
    assert(!done[0]);
    /* A bridge line but no lyrebird: still nothing to go out through. */
    add_bridge("obfs4 192.0.2.10:443 0123456789ABCDEF0123456789ABCDEF01234567 cert=AAAA iat-mode=0");
    assert(tor_nbridges == 1 && !strcmp(tor_transports, "obfs4") && strstr(notice, "Bridge added (1 now)"));
    reset();
    choose(+1);
    assert(!tor_on && !rc_log[0] && strstr(notice, "Tor never goes out directly"));
    puts("PASS without a front (Clash off; bridges without lyrebird) Tor stays off");

    /* Clash on: through Clash, its firewall loaded before rc.conf says so. */
    fake_mihomo = 1;
    clash_on = 1;
    reset();
    choose(+1);
    assert(tor_on && tor_choice() == 1 && hups == 1 && strstr(notice, "Tor through Clash"));
    assert(!strcmp(rc_log, "tor_front=mihomo [load mihomo;]\nservice_tor=" TOR_SERVICE " [load mihomo;]\n"));
    puts("PASS through Clash: the firewall first, then tor_front and service_tor");

    /* Clash is its front: off and removal refused; TUN to port only allowed. */
    reset();
    selected = cl_index() + 1;
    clash_proxy = 0;
    adjust(+1); /* TUN to its port only */
    assert(clash_on && clash_proxy && strstr(rc_log, "clash_mode=proxy"));
    reset();
    adjust(+1); /* its port only to off */
    assert(clash_on && clash_proxy && !rc_log[0] && strstr(notice, "Clash is Tor's front"));
    reset();
    fake_pkg_fork = 1;
    selected = cl_index();
    adjust(+1);
    fake_pkg_fork = 0;
    assert(clash_on && !pkg_job && !rc_log[0] && strstr(notice, "Clash is Tor's front"));
    /* Nor is Tor removed while it is on. */
    selected = tr_index();
    fake_pkg_fork = 1;
    adjust(+1);
    fake_pkg_fork = 0;
    assert(!pkg_job && strstr(notice, "Tor is on: switch it off"));
    puts("PASS a front in use: Clash off, Clash removed, Tor removed are refused");

    /* Bridges without lyrebird are passed over: Right from Clash is off. */
    reset();
    choose(+1);
    assert(!tor_on && tor_choice() == 0 && strstr(notice, "Tor off: the network goes direct"));
    assert(!strcmp(rc_log, "service_tor=(none) []\n"));
    enforced(done, sizeof done);
    assert(!strcmp(done, "remove;") && hups == 1);
    puts("PASS off: rc.conf first, then the firewall goes (bridges without lyrebird passed over)");

    /* lyrebird in: through bridges, then Left back through Clash. */
    fake_lyrebird = 1;
    reset();
    choose(-1); /* off, Left: through bridges */
    assert(tor_on && tor_choice() == 2);
    assert(!strcmp(rc_log, "tor_front=bridges [load bridges;]\nservice_tor=" TOR_SERVICE " [load bridges;]\n"));
    assert(!strcmp(tor_status(), "connecting through bridges: 10% conn_pt"));
    /* Clash may go now; the bridge lines may not. */
    reset();
    selected = cl_index() + 1;
    adjust(+1); /* its port only to off */
    assert(!clash_on && strstr(rc_log, "service_clash=(none)") && strstr(notice, "through Tor alone"));
    tor_remove_bridges();
    assert(tor_nbridges == 1 && strstr(notice, "Tor goes out through these bridges"));
    /* Clash off: through Clash is passed over too. */
    reset();
    choose(-1); /* bridges, Left: Clash (not there), then off */
    assert(!tor_on && strstr(rc_log, "service_tor=(none)"));
    puts("PASS through bridges; their lines kept while Tor uses them; Clash off passed over");

    /* A failed firewall leaves Tor as it was; so does a failed rc.conf. */
    reset();
    FILE *fail = fopen(TOR_TEST_DIR "/enforce.fail", "w");
    assert(fail && fclose(fail) == 0);
    choose(+1);
    assert(!tor_on && !rc_log[0] && strstr(notice, "Cannot load Tor's firewall"));
    reset();
    fail_rc = 1;
    choose(+1);
    fail_rc = 0;
    enforced(done, sizeof done);
    assert(!tor_on && !strcmp(done, "load bridges;remove;") && strstr(notice, "Cannot write"));
    puts("PASS a firewall or rc.conf that fails leaves Tor as it was");

    /* Off, the bridge lines go (Delete twice); a bad line is refused. */
    reset();
    tor_remove_bridges();
    assert(tor_nbridges == 1 && strstr(notice, "Delete again"));
    tor_remove_bridges();
    assert(tor_nbridges == 0 && access(TOR_STATE "/bridges", F_OK) != 0);
    add_bridge("bridge 192.0.2.10:443");
    assert(tor_nbridges == 0 && strstr(notice, "Not an obfs4 or WebTunnel bridge line"));
    add_bridge("webtunnel [2001:db8::1]:443 D0A51BE9E5E9C0A0D4B7F2D7E9D6B8D3B4F4F7A1 url=https://example.net/p ver=0.0.1");
    add_bridge("Bridge obfs4 192.0.2.11:443 cert=BBBB iat-mode=0");
    assert(tor_nbridges == 2 && !strcmp(tor_transports, "obfs4, webtunnel"));
    struct stat st;
    assert(stat(TOR_STATE "/bridges", &st) == 0 && (st.st_mode & 0777) == 0600);
    puts("PASS bridge lines removed with Tor off, refused when not a bridge, kept private (0600)");

    /* Removing: tor and lyrebird together, with Tor off. */
    fake_pkg_fork = 1;
    selected = tr_index();
    adjust(+1);
    fake_pkg_fork = 0;
    assert(pkg_job && !strcmp(pkg_action, "remove") && !strcmp(pkg_name, TOR_PACKAGES));
    pkg_check();
    puts("PASS removing with Tor off");
    puts("settings: Tor's rows passed.");
    return 0;
}
