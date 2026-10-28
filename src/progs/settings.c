/*
 * settings: interactive TUI for hardware & screen layout preferences
 * (TrackPoint sensitivity & speed if hardware present, pointer acceleration,
 * battery hibernate timeout, Tailscale (installed, on, its login), the Clash
 * proxy (installed; off, TUN, or its port only; its subscription and state),
 * full-system Tor (installed; off, through Clash or through bridges; its
 * bridge lines and state),
 * Bluetooth (tools installed, its service, devices through bluetui),
 * each saved network's MAC address (its own random one, a new one each
 * time, or the card's real one) and new networks', the wallpaper, the shared display's terminal key, the time zone
 * (automatic or chosen), network time and the date, the debug cable link (to
 * repair this system from another computer), the user's password (through
 * PAM), whether the screen is locked at boot, the lock key, and
 * display layout / resolution / scale for both built-in and external
 * screens). Saves settings in /etc/locachy/rc.conf and
 * /etc/locachy/monitors.conf (the time zone: /etc/localtime), applying them
 * immediately.
 *
 * Run "settings --apply" to apply saved settings without opening the UI.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/rtc.h>
#include <linux/input-event-codes.h>
#include <linux/videodev2.h>

#include "base.h"
#include "auth.h"
#include "files.h"
#include "monitor-layout.h"

__attribute__((weak)) int base_brightness_get(int *cur, int *max_val);
__attribute__((weak)) int base_brightness_set(int target, int max_ref);
__attribute__((weak)) int base_brightness_restore(void);

#define FG     0xD0D0D0
#define BG     0x101820
#define ACCENT 0x80C0FF
#define DIM    0x808890

#define MAX_MONITORS  8
#define MAX_MODES     8

static int tp_present;
static int tp_sens = 220;       /* 10..255 */
static int tp_speed = 180;      /* 10..255 */
static int tp_pts = 0;          /* 0 or 1 */
static int ptr_accel = 4;       /* -10..+10 => -1.0..+1.0 */
static int hib_after = 30;      /* 10..600 s */
static int tailscale_enabled; /* rc.conf has service_tailscale */
static int bluetooth_on;      /* rc.conf has service_bluetooth */
static int rclone_on;         /* rc.conf has service_rclone */
/* The interface's languages: English (what the programs are written in) and
 * each catalog installed (make.sh, po/LINGUAS), by its own name. */
static const struct { const char *code, *name; } language_names[] = {
    { "zh_CN", "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87" }, /* Simplified Chinese */
};
#define MAX_LANGUAGES 16
static char languages[MAX_LANGUAGES][16]; /* [0] "": English */
static int nlanguages;

#define MAX_MACHINES 32
static char machine_profiles[MAX_MACHINES][64]; /* [0] "": generic PC */
static int nmachines;
static char active_machine[64];

/* Where locachy-pkg takes LOCachy's source updates from: update_repo ("off":
 * none) and update_branch (none: main). */
#define DEFAULT_UPDATE_REPO "https://github.com/niuyuanzhao/locachy.git"
static char update_repo[256], update_branch[128];

/* The boot image's command line (locachy-boot reads them each time it makes
 * it): the kernel's messages shown (boot_verbose) or quiet, and options added
 * (boot_cmdline). */
#define LOCACHY_BOOT "/usr/local/bin/locachy-boot"
#define BOOT_IMAGE "/usr/local/lib/locachy/boot/locachy.efi"
#define BOOT_LOG "/run/locachy/boot.log"
static int boot_verbose;
static char boot_cmdline[256];

static void find_machines(void)
{
    nmachines = 1;
    machine_profiles[0][0] = '\0';
    DIR *d = opendir(MACHINES_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && nmachines < MAX_MACHINES) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len <= 5 || strcmp(e->d_name + len - 5, ".conf") != 0 ||
            len - 5 >= sizeof machine_profiles[0]) {
            continue;
        }
        snprintf(machine_profiles[nmachines++], sizeof machine_profiles[0], "%.*s", (int)(len - 5), e->d_name);
    }
    if (d) closedir(d);
    if (nmachines > 2) {
        for (int i = 1; i < nmachines; i++) {
            for (int j = i + 1; j < nmachines; j++) {
                if (strcmp(machine_profiles[i], machine_profiles[j]) > 0) {
                    char tmp[64];
                    memcpy(tmp, machine_profiles[i], sizeof tmp);
                    memcpy(machine_profiles[i], machine_profiles[j], sizeof tmp);
                    memcpy(machine_profiles[j], tmp, sizeof tmp);
                }
            }
        }
    }
}

static int current_machine_index(void)
{
    for (int i = 1; i < nmachines; i++) {
        if (strcmp(active_machine, machine_profiles[i]) == 0) return i;
    }
    return 0;
}

static void find_languages(void)
{
    nlanguages = 1;
    languages[0][0] = '\0';
    DIR *d = opendir(LOCALE_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && nlanguages < MAX_LANGUAGES) {
        char mo[512];
        snprintf(mo, sizeof mo, "%s/%.64s/LC_MESSAGES/" I18N_DOMAIN ".mo", LOCALE_DIR, e->d_name);
        if (e->d_name[0] != '.' && strlen(e->d_name) < sizeof languages[0] && access(mo, R_OK) == 0) {
            snprintf(languages[nlanguages++], sizeof languages[0], "%s", e->d_name);
        }
    }
    if (d) closedir(d);
}

/* The language's own name, its code if there is none here. */
static const char *language_name(const char *code)
{
    if (!code[0]) return "English";
    for (size_t i = 0; i < sizeof language_names / sizeof language_names[0]; i++) {
        if (strcmp(language_names[i].code, code) == 0) return language_names[i].name;
    }
    return code;
}

/* The index in languages of rc.conf's language (0: English). */
static int current_language(void)
{
    const char *l = base_language();
    for (int i = 1; i < nlanguages; i++) {
        if (strncmp(l, languages[i], strlen(languages[i])) == 0) return i;
    }
    return 0;
}
static char wallpaper_path[512]; /* rc.conf's wallpaper: the shared display's (swaybg) */
/* rc.conf's key_terminal ("": none): labwc's keys for a window of foot. */
static char terminal_key[128] = "Ctrl+Alt+T";
/* rc.conf's key_lock (process 1 locks the screen), login_skip (not at boot)
 * and lock_after (minutes without input; 0: never; 10 if not set). */
static char lock_key[128] = "Super+L";
/* rc.conf's keys for the input method (keys.c): the mode cycle, Chinese,
 * direct input, Chinese mode's punctuation width; "" for none. */
#define NIME_KEYS 4
static const char *const ime_key_names[NIME_KEYS] = { "key_ime", "key_ime_cn", "key_ime_raw", "key_ime_width" };
static char ime_keys[NIME_KEYS][128] = { "Ctrl+Space", "Henkan_Mode", "Muhenkan", "Zenkaku_Hankaku" };
static int login_skip;
static int lock_after = 10;
static const int lock_afters[] = { 0, 1, 2, 5, 10, 15, 30, 60 };
#define NLOCK_AFTERS ((int)(sizeof lock_afters / sizeof lock_afters[0]))
/* rc.conf's unlock_fingerprint="1" and unlock_face="1": the lock screen (and
 * sudo) take a fingerprint (fprintd) or a face (howdy) as well as the
 * password. */
static int fingerprint_on, face_on;
static char face_dlib[8]; /* rc.conf's face_dlib: what computes faces (below) */
static int ntp_on;            /* rc.conf has service_ntp */
static int tz_auto;           /* rc.conf has service_timezone */
static int link_on;           /* rc.conf has service_link */
static int term_net = 1;      /* rc.conf's term_network is not "0": term's shell on the network */
static int circular = -1;     /* rc.conf's touchpad_circular: 1 on, 0 off, -1 (none) as libinput knows the touchpad */
static char link_dev[32];     /* rc.conf's debug_link_interface: the port it reserves */
static int clash_on;          /* rc.conf has service_clash */
static int clash_proxy;       /* rc.conf's clash_mode is "proxy": its port only, no TUN */
static int tor_on;            /* rc.conf has service_tor */
static char tor_front[16];    /* rc.conf's tor_front: "mihomo" or "bridges", Tor's way out */
static pid_t pkg_job;         /* locachy-pkg install|remove PACKAGE..., while it runs */
static char pkg_action[16], pkg_name[128]; /* pkg_name: packages, space-separated */
/* NetworkManager's saved Wi-Fi and wired networks and the MAC address each
 * shows: its own random one, the same each time ("stable", the default of
 * 30-locachy-network.conf), a new one each time it connects ("random"), or
 * the card's real one ("permanent": for a network that knows the machine by
 * it). Read when settings is shown. */
#define MAX_NETS 16
static const char *const mac_modes[] = { "stable", "random", "permanent" };
static const char *const mac_labels[] = {
    N_("its own, the same each time"), N_("a new one each time"), N_("the card's real one"),
};
#define NMAC_MODES ((int)(sizeof mac_modes / sizeof mac_modes[0]))
/* What a network that names none uses (a new one: its profile names none):
 * NM_MAC_CONF's, without it "stable" (30-locachy-network.conf). */
#define NM_MAC_CONF "/etc/NetworkManager/conf.d/31-locachy-mac.conf"
static int mac_default;
static struct net {
    char uuid[40], name[64], property[40];
    int mac;   /* mac_modes[] */
    int named; /* its profile names one: else it follows mac_default */
} nets[MAX_NETS];
static int nnets;

#define TAILSCALE_SERVICE "/usr/local/lib/locachy/tailscale-client"
#define CLASH_SERVICE "/usr/local/lib/locachy/clash-client"
#define BLUETOOTH_SERVICE "/usr/local/lib/locachy/bluetooth-client"
#define BLUETOOTH_PACKAGES "bluez bluez-utils bluetui"
#define RCLONE_SERVICE "/usr/local/lib/locachy/rclone-client"
#define RCLONE_PACKAGES "rclone fuse3"
#define IME_BASE_PACKAGES "librime words"
#define IME_ZH_PACKAGES "rime-frost-git rime-pinyin-simp rime-wanxiang-gram-zh-hans"
#define IME_ALL_PACKAGES IME_BASE_PACKAGES " " IME_ZH_PACKAGES
#define CLASH_STATE "/var/lib/locachy/clash" /* clash-client's: containers do not see it */
#ifndef TOR_SERVICE
#define TOR_SERVICE "/usr/local/lib/locachy/tor-client"
#endif
#ifndef TOR_ENFORCE
#define TOR_ENFORCE "/usr/local/lib/locachy/tor-enforce" /* its firewall */
#endif
#ifndef TOR_STATE
#define TOR_STATE "/var/lib/locachy/tor" /* tor-client's: containers do not see it */
#endif
#define TOR_PACKAGES "tor lyrebird-proxy" /* lyrebird (the AUR's lyrebird-proxy): obfs4, WebTunnel */
#define PKG_LOG "/run/locachy/settings-package.log"
#define NTP_SERVICE "/usr/local/lib/locachy/ntp-client"
#define TIMEZONE_SERVICE "/usr/local/lib/locachy/timezone-client"
#define ZONEINFO "/usr/share/zoneinfo"
#define SETUP "/usr/local/bin/locachy-setup"

static const char *const places[] = { "right", "left", "above", "below", "origin" };
static const char *const place_labels[] = { N_("Right"), N_("Left"), N_("Above"), N_("Below"), N_("Origin") };

struct monitor {
    char name[32];
    char status[32];
    int is_internal;
    int place;                 /* enum monitor_place */
    char modes[MAX_MODES][32];
    int nmodes;
    int mode_sel;
    double scale;              /* one of scales[] */
};

/* The framebuffer programs' text is TEXT_H (18) pixels times the scale, the
 * nearest Terminus: 14 at 75%, 24 at 125%; from 200% one of half the size
 * drawn twice as large. */
static const double scales[] = { 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0 };
#define NSCALES ((int)(sizeof scales / sizeof scales[0]))

static struct monitor monitors[MAX_MONITORS];
static int nmonitors;

static int selected;
/* TRANSLATORS: Up/Down, Enter, Left/Right, Tab, / and q are key names; keep them. */
static const char default_notice[] = N_("Up/Down: choose   Enter: open   Left/Right: change   "
                                        "Tab: categories or settings   /: find   q: back to home");
static char notice[512] = N_("Up/Down: choose   Enter: open   Left/Right: change   "
                             "Tab: categories or settings   /: find   q: back to home");

static int is_internal_output(const char *name)
{
    return name && monitor_internal(name);
}

static int detect_pointing_stick(void)
{
    glob_t g = { 0 };
    int found = 0;
    if (glob("/sys/devices/platform/i8042/serio*/sensitivity", 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        found = 1;
    }
    globfree(&g);
    if (!found) {
        DIR *d = opendir("/sys/class/input");
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "event", 5) != 0) continue;
            char npath[320], name[128];
            snprintf(npath, sizeof npath, "/sys/class/input/%.255s/device/name", e->d_name);
            FILE *f = fopen(npath, "r");
            if (f) {
                if (fgets(name, sizeof name, f)) {
                    if (strcasestr(name, "TrackPoint") || strcasestr(name, "Pointing Stick")) {
                        found = 1;
                    }
                }
                fclose(f);
            }
            if (found) break;
        }
        if (d) closedir(d);
    }
    return found;
}

static int place_index(const char *s)
{
    for (int i = 0; i <= MONITOR_ORIGIN; i++) {
        if (strcmp(s, places[i]) == 0) {
            return i;
        }
    }
    return 0;
}

static struct monitor *find_or_add_monitor(const char *name)
{
    for (int i = 0; i < nmonitors; i++) {
        if (strcmp(monitors[i].name, name) == 0) {
            return &monitors[i];
        }
    }
    if (nmonitors >= MAX_MONITORS) {
        return NULL;
    }
    struct monitor *m = &monitors[nmonitors++];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%.31s", name);
    snprintf(m->status, sizeof m->status, "disconnected");
    m->is_internal = is_internal_output(name);
    m->place = 0;
    m->scale = 1.0;
    return m;
}

static void read_one_line(const char *path, char *out, size_t size)
{
    out[0] = '\0';
    FILE *in = fopen(path, "r");
    if (!in) {
        return;
    }
    if (fgets(out, (int)size, in)) {
        out[strcspn(out, "\r\n")] = '\0';
    }
    fclose(in);
}

static void load_modes_for_connector(const char *card_dir, struct monitor *m)
{
    char mpath[320];
    snprintf(mpath, sizeof mpath, "/sys/class/drm/%.255s/modes", card_dir);
    FILE *f = fopen(mpath, "r");
    char line[32];
    m->nmodes = 0;
    while (f && fgets(line, sizeof line, f) && m->nmodes < MAX_MODES) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0]) {
            snprintf(m->modes[m->nmodes++], sizeof m->modes[0], "%.31s", line);
        }
    }
    if (f) fclose(f);
}

static int monitor_anchor(void)
{
    int a = -1;
    for (int i = 0; i < nmonitors; i++) {
        struct monitor *m = &monitors[i];
        if (strcmp(m->status, "connected") != 0) continue;
        int rank = m->place == MONITOR_ORIGIN ? 2 : m->is_internal;
        int arank = a < 0 ? -1 : monitors[a].place == MONITOR_ORIGIN ? 2 : monitors[a].is_internal;
        if (a < 0 || rank > arank || (rank == arank && strcmp(m->name, monitors[a].name) < 0)) a = i;
    }
    return a;
}

static void load_monitors(void)
{
    nmonitors = 0;
    /* 1. Discover all hardware connectors from /sys/class/drm */
    DIR *dir = opendir("/sys/class/drm");
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL) {
        const char *dash = strchr(e->d_name, '-');
        if (strncmp(e->d_name, "card", 4) != 0 || !dash || dash[1] == '\0') {
            continue;
        }
        const char *conn = dash + 1;
        char spath[320], status[32];
        snprintf(spath, sizeof spath, "/sys/class/drm/%.255s/status", e->d_name);
        read_one_line(spath, status, sizeof status);
        if (!status[0]) {
            continue;
        }
        struct monitor *m = find_or_add_monitor(conn);
        if (m) {
            snprintf(m->status, sizeof m->status, "%.31s", status);
            load_modes_for_connector(e->d_name, m);
        }
    }
    if (dir) {
        closedir(dir);
    }

    /* 2. Overlay remembered configuration from MONITORS_CONF */
    FILE *in = fopen(MONITORS_CONF, "r");
    char line[128];
    while (in && fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') {
            continue;
        }
        *eq++ = '\0';
        struct monitor *m = find_or_add_monitor(line);
        if (m) {
            char *comma1 = strchr(eq, ',');
            if (comma1) *comma1++ = '\0';
            m->place = place_index(eq);
            if (comma1) {
                char *comma2 = strchr(comma1, ',');
                if (comma2) *comma2++ = '\0';
                if (m->nmodes == 0 && strcmp(m->status, "connected") != 0 && comma1[0]) {
                    snprintf(m->modes[m->nmodes++], sizeof m->modes[0], "%.31s", comma1);
                }
                for (int mi = 0; mi < m->nmodes; mi++) {
                    if (strcmp(m->modes[mi], comma1) == 0) {
                        m->mode_sel = mi;
                        break;
                    }
                }
                if (comma2) {
                    double sc = strtod(comma2, NULL);
                    for (int si = 0; si < NSCALES; si++) {
                        if (sc > scales[si] - 0.01 && sc < scales[si] + 0.01) m->scale = scales[si];
                    }
                }
            }
        }
    }
    if (in) {
        fclose(in);
    }
}

static int save_monitors(void)
{
    FILE *out = fopen(MONITORS_CONF ".tmp", "w");
    if (!out) return -1;
    int anchor = monitor_anchor(), failed = 0;
    for (int i = 0; i < nmonitors; i++) {
        struct monitor *m = &monitors[i];
        const char *mstr = m->nmodes > 0 ? m->modes[m->mode_sel] : "preferred";
        int place = i == anchor ? MONITOR_ORIGIN : m->place;
        if (place < MONITOR_RIGHT || place > MONITOR_ORIGIN) place = MONITOR_RIGHT;
        failed |= fprintf(out, "%s=%s,%s,%g\n", m->name, places[place], mstr,
                          m->scale > 0 ? m->scale : 1.0) < 0;
    }
    if (fclose(out) != 0) failed = 1;
    if (!failed && rename(MONITORS_CONF ".tmp", MONITORS_CONF) == 0) return 0;
    unlink(MONITORS_CONF ".tmp");
    return -1;
}

static int monitor_saved(void)
{
    if (save_monitors() < 0) {
        snprintf(notice, sizeof notice, _("Cannot save %s."), MONITORS_CONF);
        return 0;
    }
    snprintf(notice, sizeof notice, _("Saved; the shared display applies the layout when it is shown."));
    return 1;
}

/* The screen touch screens and screens' pens are on: the internal one, or
 * the only one connected; "" when that cannot be told (labwc spans all). */
static const char *touch_output(void)
{
    int internal = -1, only = -1, connected = 0;
    for (int i = 0; i < nmonitors; i++) {
        if (strcmp(monitors[i].status, "connected") != 0) continue;
        connected++;
        only = i;
        if (internal < 0 && monitors[i].is_internal) internal = i;
    }
    return internal >= 0 ? monitors[internal].name : connected == 1 ? monitors[only].name : "";
}

/* A key as rc.conf names it (Ctrl+Alt+T) as labwc does (C-A-t); 0 when
 * labwc's name of it is not known here. */
static int labwc_key(const char *spec, char *out, size_t size)
{
    char buf[64], mods[16] = "", *save = NULL;
    const char *key = NULL;
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *part = strtok_r(buf, "+", &save); part != NULL; part = strtok_r(NULL, "+", &save)) {
        const char *mod = strcasecmp(part, "Ctrl") == 0 || strcasecmp(part, "Control") == 0 ? "C-" :
                          strcasecmp(part, "Alt") == 0 ? "A-" : strcasecmp(part, "Shift") == 0 ? "S-" :
                          strcasecmp(part, "Super") == 0 || strcasecmp(part, "Meta") == 0 ||
                          strcasecmp(part, "Win") == 0 ? "W-" : NULL;
        if (mod != NULL) {
            if (strlen(mods) + 2 < sizeof mods) strcat(mods, mod);
        } else if (key != NULL) {
            return 0;
        } else {
            key = part;
        }
    }
    if (key == NULL) return 0;
    char name[16];
    int fn = (key[0] == 'F' || key[0] == 'f') && isdigit((unsigned char)key[1]) ? atoi(key + 1) : 0;
    if (key[0] != '\0' && key[1] == '\0' && isalnum((unsigned char)key[0])) {
        snprintf(name, sizeof name, "%c", tolower((unsigned char)key[0]));
    } else if (fn >= 1 && fn <= 12) {
        snprintf(name, sizeof name, "F%d", fn);
    } else if (strcasecmp(key, "Space") == 0) {
        snprintf(name, sizeof name, "space");
    } else if (strcasecmp(key, "Enter") == 0 || strcasecmp(key, "Return") == 0) {
        snprintf(name, sizeof name, "Return");
    } else if (strcasecmp(key, "Tab") == 0) {
        snprintf(name, sizeof name, "Tab");
    } else if (strcasecmp(key, "Esc") == 0 || strcasecmp(key, "Escape") == 0) {
        snprintf(name, sizeof name, "Escape");
    } else if (strcasecmp(key, "BackSpace") == 0) {
        snprintf(name, sizeof name, "BackSpace");
    } else {
        return 0;
    }
    snprintf(out, size, "%s%s", mods, name);
    return 1;
}

/* A line of the labwc configuration as written: rc.conf's acceleration, the
 * touch screen's output (the line left out when it is not known), and the
 * terminal keys (a line for each; none, none). */
static void labwc_line(FILE *out, const char *line)
{
    const char *start = strstr(line, "<pointerAcceleration>");
    const char *end = start ? strstr(start, "</pointerAcceleration>") : NULL;
    const char *map = strstr(line, "<mapToOutput></mapToOutput>");
    const char *term = strstr(line, "key=\"TERMINAL-KEY\"");
    if (term) {
        char keys[sizeof terminal_key], name[32], *save = NULL;
        snprintf(keys, sizeof keys, "%s", terminal_key);
        for (char *p = keys; *p; p++) {
            if (*p == ',') *p = ' ';
        }
        for (char *k = strtok_r(keys, " \t", &save); k != NULL; k = strtok_r(NULL, " \t", &save)) {
            if (labwc_key(k, name, sizeof name)) {
                fprintf(out, "%.*skey=\"%s\"%s", (int)(term - line), line, name,
                        term + strlen("key=\"TERMINAL-KEY\""));
            }
        }
    } else if (end) {
        fprintf(out, "%.*s<pointerAcceleration>%.1f%s", (int)(start - line), line, ptr_accel / 10.0, end);
    } else if (map) {
        const char *screen = touch_output();
        if (screen[0]) {
            fprintf(out, "%.*s<mapToOutput>%s</mapToOutput>%s", (int)(map - line), line, screen,
                    map + strlen("<mapToOutput></mapToOutput>"));
        }
    } else {
        fputs(line, out);
    }
}

static void signal_programs(const char *comm_name, int sig);

/* LABWC_RC: /etc/locachy/labwc/rc.xml with this pointer acceleration, touch
 * screen and terminal key; a running labwc rereads it on SIGHUP. */
static void apply_labwc(void)
{
    FILE *in = fopen("/etc/locachy/labwc/rc.xml", "r");
    if (!in) {
        return;
    }
    mkdir("/run/locachy", 0755);
    FILE *out = fopen(LABWC_RC ".tmp", "w");
    char line[512];
    while (out && fgets(line, sizeof line, in)) {
        labwc_line(out, line);
    }
    fclose(in);
    if (!out || fclose(out) != 0 || rename(LABWC_RC ".tmp", LABWC_RC) != 0) {
        return;
    }
    signal_programs("labwc", SIGHUP);
}

static void write_glob(const char *pattern, int val)
{
    glob_t g = { 0 };
    if (glob(pattern, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            int fd = open(g.gl_pathv[i], O_WRONLY | O_CLOEXEC);
            if (fd >= 0) {
                dprintf(fd, "%d\n", val);
                close(fd);
            }
        }
    }
    globfree(&g);
}

static int write_atomic_file(const char *path, const char *tmp, const char *content);

/* libinput's file for local device quirks ("Installing temporary local device
 * quirks" in its documentation); settings writes one section of it. */
#define LOCAL_QUIRKS "/etc/libinput/local-overrides.quirks"
#define CIRCULAR_SECTION "[LOCachy touchpad circular scrolling]"

/* rc.conf's touchpad_circular: whether touchpads scroll as a finger goes round
 * their edge, clockwise down (libinput's circular scrolling, since 1.32),
 * instead of with two fingers. libinput does it on the touchpads it has the
 * ModelCircularTouchpad quirk for, the round ones of Panasonic's CF-SV1, CF-SV8
 * and CF-FV1 (pkgbuilds/libinput), and nothing else on them. Unset, nothing is
 * written: libinput's list decides. "1" or "0" sets the quirk for every
 * touchpad here, in a section of its own; the rest of the file is kept.
 * libinput reads the file when its context is made: a direct program's as it
 * takes the pointer (when it next has the screen), the shared display's as it
 * starts. -1 when the file cannot be written. */
static int apply_circular(void)
{
    char *was = NULL, *now = NULL, line[512];
    size_t was_len = 0, now_len = 0;
    FILE *in = fopen(LOCAL_QUIRKS, "r");
    FILE *old = open_memstream(&was, &was_len), *new = open_memstream(&now, &now_len);
    int ours = 0, blank = 1;
    while (in && old && new && fgets(line, sizeof line, in)) {
        fputs(line, old);
        if (line[0] == '[') ours = strncmp(line, CIRCULAR_SECTION, strlen(CIRCULAR_SECTION)) == 0;
        if (ours) continue;
        fputs(line, new);
        blank = line[strspn(line, " \t\r\n")] == '\0';
    }
    if (in) fclose(in);
    if (new && circular >= 0) {
        fprintf(new, "%s%s\n# settings: Touchpad Circular Scrolling (rc.conf touchpad_circular)\n"
                "MatchUdevType=touchpad\nModelCircularTouchpad=%d\n", blank ? "" : "\n", CIRCULAR_SECTION, circular);
    }
    int ok = old && new;
    if (old) fclose(old);
    if (new) fclose(new);
    int result = ok ? 0 : -1;
    if (ok && strcmp(was, now) != 0) {
        if (access("/etc/libinput", F_OK) != 0) mkdir("/etc/libinput", 0755);
        result = now[strspn(now, " \t\r\n")] == '\0' ? unlink(LOCAL_QUIRKS) :
                 write_atomic_file(LOCAL_QUIRKS, LOCAL_QUIRKS ".tmp", now);
    }
    free(was);
    free(now);
    return result;
}

static void apply_all(void)
{
    apply_circular();
    if (tp_present) {
        write_glob("/sys/devices/platform/i8042/serio*/sensitivity", tp_sens);
        write_glob("/sys/devices/platform/i8042/serio*/serio*/sensitivity", tp_sens);
        write_glob("/sys/devices/platform/i8042/serio*/speed", tp_speed);
        write_glob("/sys/devices/platform/i8042/serio*/serio*/speed", tp_speed);
        write_glob("/sys/devices/platform/i8042/serio*/press_to_select", tp_pts);
        write_glob("/sys/devices/platform/i8042/serio*/serio*/press_to_select", tp_pts);
    }
    apply_labwc();
}

static void on_rc(const char *name, const char *value, void *arg)
{
    (void)arg;
    if (strcmp(name, "trackpoint_sensitivity") == 0) {
        tp_sens = atoi(value);
    } else if (strcmp(name, "trackpoint_speed") == 0) {
        tp_speed = atoi(value);
    } else if (strcmp(name, "trackpoint_press_to_select") == 0) {
        tp_pts = atoi(value) ? 1 : 0;
    } else if (strcmp(name, "pointer_accel") == 0) {
        ptr_accel = (int)(atof(value) * 10.0 + (atof(value) >= 0 ? 0.5 : -0.5));
    } else if (strcmp(name, "hibernate_after") == 0) {
        hib_after = atoi(value);
    } else if (strcmp(name, "service_tailscale") == 0) {
        tailscale_enabled = 1;
    } else if (strcmp(name, "service_bluetooth") == 0) {
        bluetooth_on = 1;
    } else if (strcmp(name, "service_rclone") == 0) {
        rclone_on = 1;
    } else if (strcmp(name, "machine") == 0) {
        snprintf(active_machine, sizeof active_machine, "%s", value);
    } else if (strcmp(name, "update_repo") == 0) {
        snprintf(update_repo, sizeof update_repo, "%s", value);
    } else if (strcmp(name, "update_branch") == 0) {
        snprintf(update_branch, sizeof update_branch, "%s", value);
    } else if (strcmp(name, "boot_verbose") == 0) {
        boot_verbose = strcmp(value, "1") == 0;
    } else if (strcmp(name, "boot_cmdline") == 0) {
        snprintf(boot_cmdline, sizeof boot_cmdline, "%s", value);
    } else if (strcmp(name, "service_clash") == 0) {
        clash_on = 1;
    } else if (strcmp(name, "clash_mode") == 0) {
        clash_proxy = strcmp(value, "proxy") == 0;
    } else if (strcmp(name, "service_tor") == 0) {
        tor_on = 1;
    } else if (strcmp(name, "tor_front") == 0) {
        snprintf(tor_front, sizeof tor_front, "%s", value);
    } else if (strcmp(name, "service_ntp") == 0) {
        ntp_on = 1;
    } else if (strcmp(name, "service_timezone") == 0) {
        tz_auto = 1;
    } else if (strcmp(name, "term_network") == 0) {
        term_net = strcmp(value, "0") != 0;
    } else if (strcmp(name, "touchpad_circular") == 0) {
        circular = strcmp(value, "1") == 0 ? 1 : strcmp(value, "0") == 0 ? 0 : -1;
    } else if (strcmp(name, "service_link") == 0) {
        link_on = 1;
    } else if (strcmp(name, "debug_link_interface") == 0) {
        snprintf(link_dev, sizeof link_dev, "%s", value);
    } else if (strcmp(name, "wallpaper") == 0) {
        snprintf(wallpaper_path, sizeof wallpaper_path, "%s", value);
    } else if (strcmp(name, "key_terminal") == 0) {
        snprintf(terminal_key, sizeof terminal_key, "%s", value);
    } else if (strcmp(name, "key_lock") == 0) {
        snprintf(lock_key, sizeof lock_key, "%s", value);
    } else if (strncmp(name, "key_ime", 7) == 0) {
        for (int i = 0; i < NIME_KEYS; i++) {
            if (strcmp(name, ime_key_names[i]) == 0) snprintf(ime_keys[i], sizeof ime_keys[i], "%s", value);
        }
    } else if (strcmp(name, "login_skip") == 0) {
        login_skip = strcmp(value, "1") == 0;
    } else if (strcmp(name, "lock_after") == 0) {
        lock_after = atoi(value) > 0 ? atoi(value) : 0;
    } else if (strcmp(name, "unlock_fingerprint") == 0) {
        fingerprint_on = strcmp(value, "1") == 0;
    } else if (strcmp(name, "unlock_face") == 0) {
        face_on = strcmp(value, "1") == 0;
    } else if (strcmp(name, "face_dlib") == 0) {
        snprintf(face_dlib, sizeof face_dlib, "%s", value);
    }
}

/* rc.conf with these settings; process 1 reads it again (SIGHUP): the
 * Tailscale service starts or ends with its line. */
static void save_rc(void)
{
    char sens[16], speed[16], pts[16], accel[16], hib[16];
    snprintf(sens, sizeof sens, "%d", tp_sens);
    snprintf(speed, sizeof speed, "%d", tp_speed);
    snprintf(pts, sizeof pts, "%d", tp_pts);
    snprintf(accel, sizeof accel, "%.1f", ptr_accel / 10.0);
    snprintf(hib, sizeof hib, "%d", hib_after);
    /* Change these keys only: a growing list of services must not be cut off
     * by a fixed array of lines in the settings editor. */
    const char *values[][2] = {
        { "pointer_accel", accel }, { "hibernate_after", hib },
        { "tailscale_enabled", NULL }, /* superseded by the service line */
        { "service_tailscale", tailscale_enabled ? TAILSCALE_SERVICE : NULL },
        { "trackpoint_sensitivity", sens }, { "trackpoint_speed", speed },
        { "trackpoint_press_to_select", pts },
    };
    size_t count = sizeof values / sizeof values[0] - (tp_present ? 0 : 3);
    for (size_t i = 0; i < count; i++) {
        if (rc_set(values[i][0], values[i][1]) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
    }
    kill(1, SIGHUP);
}

/* Tailscale and Clash: whether the package is installed, and whether it is
 * on, are two rows. The package is installed and removed by locachy-pkg in
 * the background, so leaving settings does not cut it short; on is the
 * service line (which process 1 starts and ends), only when it is installed,
 * and off is the network as it is without it. Tailscale's login runs
 * "tailscale up --qr" and shows its address and code here. */

static int tailscale_installed(void)
{
    return access("/usr/bin/tailscaled", X_OK) == 0 && access("/usr/bin/tailscale", X_OK) == 0;
}

static int clash_installed(void)
{
    return access("/usr/bin/mihomo", X_OK) == 0;
}

/* Full-system Tor (tor-client, tor-enforce): installed (tor, and lyrebird
 * for bridges) and its choice are two rows, as for Clash. Tor never goes out
 * by itself, so the choices are its fronts: off (direct), through Clash
 * (mihomo), through the bridges the user gave (obfs4 or WebTunnel). A choice
 * is offered only when its front is there; a front Tor is using cannot be
 * switched off or removed, nor Tor removed while it is on; nothing goes
 * direct but by choosing off. */
static int tor_installed(void)
{
    return access("/usr/bin/tor", X_OK) == 0;
}

static int lyrebird_installed(void)
{
    return access("/usr/bin/lyrebird", X_OK) == 0;
}

static int tor_job(void)
{
    return strcmp(pkg_name, TOR_PACKAGES) == 0 || strcmp(pkg_name, "lyrebird-proxy") == 0;
}

/* 0 off, 1 through Clash, 2 through bridges. */
static int tor_choice(void)
{
    return !tor_on ? 0 : strcmp(tor_front, "bridges") == 0 ? 2 : 1;
}

static int tor_via_clash(void)
{
    return tor_choice() == 1;
}

/* BlueZ's daemon, bluez-utils' control tool, and the devices UI must all be
 * present. Installing them does not add the service line. */
static int bluetooth_installed(void)
{
    return access("/usr/lib/bluetooth/bluetoothd", X_OK) == 0 &&
           access("/usr/bin/bluetoothctl", X_OK) == 0 && access("/usr/bin/bluetui", X_OK) == 0;
}

static int rclone_installed(void)
{
    return access("/usr/bin/rclone", X_OK) == 0 && access("/usr/bin/fusermount3", X_OK) == 0;
}

static int rime_installed(void)
{
    return access("/usr/lib/librime.so", R_OK) == 0 && access("/usr/bin/rime_deployer", X_OK) == 0;
}

static int rime_zh_installed(void)
{
    return access("/usr/share/rime-data/rime_frost.schema.yaml", R_OK) == 0;
}

static int ime_job(void)
{
    return strcmp(pkg_name, IME_BASE_PACKAGES) == 0 || strcmp(pkg_name, IME_ZH_PACKAGES) == 0 ||
           strcmp(pkg_name, IME_ALL_PACKAGES) == 0;
}

static void command_output(char *const argv[], char *buf, size_t size);

static char tpm_summary[160] = "-";
static int tpm_bound; /* a LUKS2 disk opens with the TPM at power-on */
static int tpm_clear_asked;

static void load_tpm_status(void)
{
    if (access("/usr/local/bin/locachy-tpm", X_OK) != 0) {
        snprintf(tpm_summary, sizeof tpm_summary, "-");
        return;
    }
    char buf[1024] = "";
    char *const argv[] = { "/usr/local/bin/locachy-tpm", "status", "--json", NULL };
    command_output(argv, buf, sizeof buf);
    if (!buf[0]) {
        snprintf(tpm_summary, sizeof tpm_summary, "-");
        return;
    }
    int ver = strstr(buf, "\"tpm_version\": 2") ? 2 : strstr(buf, "\"tpm_version\": 1") ? 1 : 0;
    int has_luks = strstr(buf, "\"luks_devices\": [\"") != NULL;
    int active = strstr(buf, "\"tpm_active\": true") != NULL;
    int bound = strstr(buf, "\"systemd-tpm2\"") != NULL || strstr(buf, "\"locachy-tpm12\"") != NULL;
    tpm_bound = has_luks && bound;
    if (!has_luks) {
        snprintf(tpm_summary, sizeof tpm_summary, "%s", _("no LUKS2 disk"));
    } else if (ver == 0) {
        snprintf(tpm_summary, sizeof tpm_summary, "%s", _("no TPM chip"));
    } else if (!active) {
        snprintf(tpm_summary, sizeof tpm_summary, "%s", _("TPM 1.2 disabled in BIOS"));
    } else if (bound) {
        /* TRANSLATORS: Delete is the Delete key; keep it. */
        snprintf(tpm_summary, sizeof tpm_summary, ver == 2 ?
                 _("TPM 2.0 bound (Delete: unbind)") : _("TPM 1.2 bound (Delete: unbind)"));
    } else {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(tpm_summary, sizeof tpm_summary, ver == 2 ?
                 _("TPM 2.0 (Enter: bind auto-unlock)") : _("TPM 1.2 (Enter: bind auto-unlock)"));
    }
}

/* Fingerprints: fprintd installed, and on (unlock_fingerprint), are two rows
 * as for Tailscale: removing it switches it off first. The user's (uid 1000)
 * fingerprints are fprintd's, listed, enrolled and deleted with its tools
 * (fprintd-list, fprintd-enroll, fprintd-delete); it is started on the
 * system bus when asked. */
#define FPRINTD "/usr/lib/fprintd"

static int fprintd_installed(void)
{
    return access(FPRINTD, X_OK) == 0 && access("/usr/bin/fprintd-enroll", X_OK) == 0;
}

static const char *const fingers[] = {
    "right-index-finger", "left-index-finger", "right-thumb", "left-thumb", "right-middle-finger",
    "left-middle-finger", "right-ring-finger", "left-ring-finger", "right-little-finger", "left-little-finger",
};
#define NFINGERS ((int)(sizeof fingers / sizeof fingers[0]))
static char fp_enrolled[200]; /* the user's fingers as fprintd lists them, or why there are none */
static int fp_next;           /* the finger the add row enrolls (fingers[]) */
static int fp_delete_asked;   /* Delete pressed once on the fingerprints row */

static const char *const finger_labels[] = {
    N_("right index finger"), N_("left index finger"), N_("right thumb"), N_("left thumb"),
    N_("right middle finger"), N_("left middle finger"), N_("right ring finger"), N_("left ring finger"),
    N_("right little finger"), N_("left little finger"),
};

static const char *user_name(void)
{
    struct passwd *pw = getpwuid(1000);
    return pw ? pw->pw_name : NULL;
}

static void command_output(char *const argv[], char *buf, size_t size);

/* fprintd-list: the user's fingers (as "right index finger, ..."). */
static void load_fingers(void)
{
    const char *user = user_name();
    char buf[2048];
    if (!fprintd_installed() || !user) {
        snprintf(fp_enrolled, sizeof fp_enrolled, "%s", user ? "-" : _("no user (uid 1000)"));
        return;
    }
    char *const argv[] = { "/usr/bin/fprintd-list", (char *)user, NULL };
    command_output(argv, buf, sizeof buf);
    if (strstr(buf, "found 0 devices") || strstr(buf, "No devices")) {
        snprintf(fp_enrolled, sizeof fp_enrolled, "%s", _("no fingerprint reader found"));
        return;
    }
    size_t n = 0;
    fp_enrolled[0] = '\0';
    for (char *s = strstr(buf, " - #"); s && n < sizeof fp_enrolled; s = strstr(s + 1, " - #")) {
        char *name = strstr(s, ": ");
        if (!name) {
            break;
        }
        name += 2;
        n += (size_t)snprintf(fp_enrolled + n, sizeof fp_enrolled - n, "%s%.*s", n ? ", " : "",
                              (int)strcspn(name, "\n"), name);
    }
    for (char *c = fp_enrolled; *c; c++) {
        if (*c == '-') {
            *c = ' ';
        }
    }
    if (!fp_enrolled[0]) {
        snprintf(fp_enrolled, sizeof fp_enrolled, "%s", _("none yet"));
    }
}

/* sudo's PAM file: the line of pam_locachy_any.so (the lock screen's other
 * ways beside the typed password) with the ways switched on, before its
 * first auth line; none when they are all off. 0 if written. */
#define SUDO_PAM "/etc/pam.d/sudo"

static int sudo_ways(void)
{
    char lines[64][256];
    int n = 0, put = 0;
    FILE *in = fopen(SUDO_PAM, "r");
    if (!in) {
        return -1;
    }
    while (n < 64 && fgets(lines[n], sizeof lines[n], in)) {
        if (!strstr(lines[n], "pam_locachy_any.so")) {
            n++;
        }
    }
    fclose(in);
    FILE *out = fopen(SUDO_PAM ".tmp", "w");
    if (!out) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (!put && (fingerprint_on || face_on) && strncmp(lines[i] + strspn(lines[i], " \t"), "auth", 4) == 0) {
            fprintf(out, "auth\t\tsufficient\tpam_locachy_any.so%s%s\n",
                    fingerprint_on ? " locachy-lock-fingerprint" : "", face_on ? " locachy-lock-face" : "");
            put = 1;
        }
        fputs(lines[i], out);
    }
    if (fclose(out) != 0 || rename(SUDO_PAM ".tmp", SUDO_PAM) != 0) {
        unlink(SUDO_PAM ".tmp");
        return -1;
    }
    return 0;
}

/* Faces: howdy, with linux-enable-ir-emitter (most laptops' infrared
 * cameras need their emitter switched on by it), both from the AUR, and
 * howdy's dlib. Installed and on are two rows as for fingerprints;
 * installing needs a camera. The user's faces are howdy's (howdy -U USER
 * list, add, clear); adding one and setting the emitter up ask questions, so
 * they run in term. */
#define HOWDY "/usr/bin/howdy"
#define IR_EMITTER "/usr/bin/linux-enable-ir-emitter"
#define FACE_PACKAGES "howdy-git linux-enable-ir-emitter-git"
/* howdy's dlib: the AUR's python-dlib, its CPU build (python-dlib) or its
 * CUDA build (python-dlib-cuda, which provides python-dlib), each the AUR's
 * recipe with LOCachy's sed script leaving the other out (aur-patches/).
 * CUDA needs an NVIDIA GPU with NVIDIA's driver loaded (nouveau has no CUDA).
 * rc.conf's face_dlib: "cpu" or "cuda"; unset, CUDA wherever the driver is
 * loaded, as dlib's own build uses CUDA wherever it finds it. */
#ifndef NVIDIA_DRIVER
#define NVIDIA_DRIVER "/proc/driver/nvidia/version"
#endif
#ifndef PACMAN_LOCAL
#define PACMAN_LOCAL "/var/lib/pacman/local"
#endif

static int cuda_usable(void)
{
    return access(NVIDIA_DRIVER, R_OK) == 0;
}

static int face_cuda(void)
{
    return cuda_usable() && strcmp(face_dlib, "cpu") != 0;
}

static const char *dlib_package(int cuda)
{
    return cuda ? "python-dlib-cuda" : "python-dlib";
}

/* The dlib installed: 2 the CUDA build, 1 the CPU one, 0 none. */
static int dlib_installed(void)
{
    DIR *d = opendir(PACMAN_LOCAL);
    struct dirent *e;
    int found = 0;
    while (d && (e = readdir(d)) != NULL && !found) {
        if (strncmp(e->d_name, "python-dlib-cuda-", 17) == 0) {
            found = 2;
        } else if (strncmp(e->d_name, "python-dlib-", 12) == 0 && isdigit((unsigned char)e->d_name[12])) {
            found = 1;
        }
    }
    if (d) {
        closedir(d);
    }
    return found;
}

/* A job of the faces' packages: installing, removing, or swapping dlib. */
static int face_job(void)
{
    return strstr(pkg_name, "howdy-git") != NULL || strncmp(pkg_name, "python-dlib", 11) == 0;
}

static int howdy_installed(void)
{
    return access(HOWDY, X_OK) == 0;
}

/* A V4L2 video capture device (a camera); its path into out. */
static int camera(char *out, size_t size)
{
    char path[32];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof path, "/dev/video%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        struct v4l2_capability cap;
        int capture = fd >= 0 && ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
                      ((cap.device_caps ? cap.device_caps : cap.capabilities) & V4L2_CAP_VIDEO_CAPTURE);
        if (fd >= 0) {
            close(fd);
        }
        if (capture) {
            snprintf(out, size, "%s", path);
            return 1;
        }
    }
    return 0;
}

/* howdy's camera: device_path in its configuration, a /dev/videoN, as
 * howdy's own setup and the Arch Wiki take it. The capture devices are
 * listed with V4L2's name for each, the infrared one marked: its frames are
 * grey only (the second function of a Windows Hello camera). */
#define HOWDY_CONF "/etc/howdy/config.ini"
static char face_camera[160];
struct face_cam {
    char path[24], name[40];
    int ir;
};

static int face_cameras(struct face_cam *out, int max)
{
    int n = 0;
    for (int i = 0; i < 64 && n < max; i++) {
        char path[24];
        snprintf(path, sizeof path, "/dev/video%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
            ((cap.device_caps ? cap.device_caps : cap.capabilities) & V4L2_CAP_VIDEO_CAPTURE)) {
            struct v4l2_fmtdesc f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
            int grey = 0, colour = 0;
            for (f.index = 0; f.index < 64 && ioctl(fd, VIDIOC_ENUM_FMT, &f) == 0; f.index++) {
                if (f.pixelformat == V4L2_PIX_FMT_GREY || f.pixelformat == V4L2_PIX_FMT_Y10 ||
                    f.pixelformat == V4L2_PIX_FMT_Y12 || f.pixelformat == V4L2_PIX_FMT_Y16) {
                    grey = 1;
                } else {
                    colour = 1;
                }
            }
            snprintf(out[n].path, sizeof out[n].path, "%s", path);
            snprintf(out[n].name, sizeof out[n].name, "%.39s", (const char *)cap.card);
            out[n].ir = grey && !colour;
            n++;
        }
        close(fd);
    }
    return n;
}

static void load_face_camera(void)
{
    char line[256];
    face_camera[0] = '\0';
    FILE *f = fopen(HOWDY_CONF, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *s = line + strspn(line, " \t");
        if (strncmp(s, "device_path", 11) == 0 && (s = strchr(s, '=')) != NULL) {
            s += 1 + strspn(s + 1, " \t");
            snprintf(face_camera, sizeof face_camera, "%.*s", (int)strcspn(s, " \t\r\n"), s);
        }
    }
    if (f) {
        fclose(f);
    }
}

/* Which of cams howdy's device_path is (a /dev/v4l link set before is the
 * node it points to); -1 for none. */
static int face_camera_index(const struct face_cam *cams, int ncams)
{
    char real[PATH_MAX];
    const char *at = face_camera[0] && realpath(face_camera, real) ? real : face_camera;
    for (int i = 0; i < ncams; i++) {
        if (strcmp(cams[i].path, at) == 0) {
            return i;
        }
    }
    return -1;
}

/* What the camera row shows: the node, its name, infrared or not. */
static void face_camera_label(char *out, size_t size)
{
    struct face_cam cams[16];
    int ncams = face_cameras(cams, 16), i = face_camera_index(cams, ncams);
    if (i >= 0) {
        snprintf(out, size, "%s  %s%s", cams[i].path, cams[i].name, cams[i].ir ? _(" (infrared)") : "");
    } else {
        snprintf(out, size, "%s", face_camera[0] ? face_camera : _("none set"));
    }
}

/* The next (dir 1) or previous capture device after the one howdy has, set
 * as its device_path by howdy itself (howdy set); with none set yet, the
 * first infrared one. 0 if set. */
static int next_face_camera(int dir)
{
    struct face_cam cams[16];
    char out[256];
    int ncams = face_cameras(cams, 16);
    if (ncams == 0) {
        return -1;
    }
    int at = face_camera_index(cams, ncams), to = 0;
    if (at >= 0) {
        to = (at + dir + ncams) % ncams;
    } else {
        for (int i = ncams - 1; i >= 0; i--) {
            to = cams[i].ir ? i : to;
        }
    }
    char *const argv[] = { HOWDY, "set", "device_path", cams[to].path, NULL };
    command_output(argv, out, sizeof out);
    load_face_camera();
    return strcmp(face_camera, cams[to].path) == 0 ? 0 : -1;
}

static char face_models[160]; /* howdy -U USER list: how many, or why none */
static int face_delete_asked;

static void load_faces(void)
{
    const char *user = user_name();
    char buf[4096];
    if (!howdy_installed() || !user) {
        snprintf(face_models, sizeof face_models, "-");
        return;
    }
    char *const argv[] = { HOWDY, "-U", (char *)user, "list", NULL };
    command_output(argv, buf, sizeof buf);
    /* A table: a header, then a line for each model ("ID  Date  Label"). */
    int models = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        line += strspn(line, " \t");
        models += isdigit((unsigned char)line[0]) != 0;
    }
    if (models) {
        snprintf(face_models, sizeof face_models, P_("%d recorded", "%d recorded", models), models);
    } else {
        snprintf(face_models, sizeof face_models, "%s", _("none yet"));
    }
    load_face_camera();
}

/* argv in term, on the screen in settings' place until it ends: its status. */
static int in_term(char *const argv[])
{
    char *call[12] = { "/usr/local/bin/term" };
    int n = 1;
    for (; argv[n - 1] && n < 11; n++) {
        call[n] = argv[n - 1];
    }
    call[n] = NULL;
    return base_call(call);
}

struct opt_module {
    const char *label;
    const char *probe;
    const char *packages;
    const char *rc_key, *rc_val;
    const char *rc_key2, *rc_val2;
};

#define AUDIO_SERVICE "/usr/local/lib/locachy/audio-client"
#define MIXER_PROGRAM "/usr/local/bin/term /usr/bin/wiremix"

static const struct opt_module opt_modules[] = {
    { N_("Audio (PipeWire)"), "/usr/bin/wiremix",
      "pipewire pipewire-alsa pipewire-pulse wireplumber wiremix",
      "service_audio", AUDIO_SERVICE, "program_mixer", MIXER_PROGRAM },
    { N_("Wayland display (labwc)"), "/usr/bin/labwc",
      "labwc-locachy swaybg libnotify libinput-tools libinput-gestures lisgd python-pyudev noto-fonts-emoji",
      "service_clipboard", "/usr/local/lib/locachy/clipboard --serve", NULL, NULL },
    { N_("Document viewer (view)"), "/usr/include/mupdf/fitz.h",
      "libmupdf djvulibre libspectre libwebp libheif libjxl imagemagick openexr libarchive",
      NULL, NULL, NULL, NULL },
    { N_("TeX DVI support (texlive)"), "/usr/bin/dvipdfmx",
      "texlive-bin texlive-basic", NULL, NULL, NULL, NULL },
    { N_("Text editor (edit)"), "/usr/include/tree_sitter/api.h",
      "tree-sitter tree-sitter-bash tree-sitter-c tree-sitter-cpp tree-sitter-javascript "
      "tree-sitter-lua tree-sitter-markdown tree-sitter-python tree-sitter-query "
      "tree-sitter-rust tree-sitter-vim tree-sitter-vimdoc pcre2 json-glib",
      NULL, NULL, NULL, NULL },
    { N_("7-Zip (encrypted 7z, RAR)"), "/usr/bin/7z", "7zip", NULL, NULL, NULL, NULL },
    { N_("CJK fonts (noto-cjk)"), "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
      "noto-fonts-cjk", NULL, NULL, NULL, NULL },
    { N_("Firmware updates (fwupd)"), "/usr/bin/fwupdmgr",
      "fwupd", NULL, NULL, NULL, NULL },
    { N_("SPI programmer (flashrom)"), "/usr/bin/flashrom",
      "flashrom", NULL, NULL, NULL, NULL },
};
#define NOPT_MODULES ((int)(sizeof opt_modules / sizeof opt_modules[0]))

static int opt_module_installed(int i)
{
    return access(opt_modules[i].probe, R_OK) == 0;
}

static const char *opt_job_title(void)
{
    for (int i = 0; i < NOPT_MODULES; i++) {
        if (strcmp(pkg_name, opt_modules[i].packages) == 0) {
            return _(opt_modules[i].label);
        }
    }
    return NULL;
}

static const char *pkg_title(void)
{
    const char *opt = opt_job_title();
    if (opt) {
        return opt;
    }
    return strcmp(pkg_name, "mihomo") == 0 ? _("Clash (mihomo)") :
           tor_job() ? _("Tor (tor, lyrebird)") :
           strcmp(pkg_name, BLUETOOTH_PACKAGES) == 0 ? _("Bluetooth tools") :
           strcmp(pkg_name, RCLONE_PACKAGES) == 0 ? _("Cloud drives (rclone)") :
           ime_job() ? _("Input method (Rime)") :
           strcmp(pkg_name, "fprintd") == 0 ? _("Fingerprint reader (fprintd)") :
           face_job() ? _("Face recognition (howdy)") : "Tailscale";
}

/* locachy-pkg ACTION PACKAGE..., in the background, its output in PKG_LOG.
 * ACTION "swap", package "OLD NEW": locachy-pkg remove OLD, then install NEW
 * (one package in place of another it conflicts with). */
static void start_pkg_job(const char *action, const char *package)
{
    mkdir("/run/locachy", 0755);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int log = open(PKG_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int null = open("/dev/null", O_RDONLY);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
        }
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
        }
        char list[512], *args[32] = { "locachy-pkg", (char *)action };
        int n = 2;
        snprintf(list, sizeof list, "%s", package);
        for (char *w = strtok(list, " "); w && n < 31; w = strtok(NULL, " ")) {
            args[n++] = w;
        }
        args[n] = NULL;
        if (strcmp(action, "swap") == 0 && n == 4) {
            char *remove[] = { "locachy-pkg", "remove", args[2], NULL };
            int status;
            pid_t r = fork();
            if (r == 0) {
                execv("/usr/local/bin/locachy-pkg", remove);
                _exit(127);
            }
            if (r < 0 || waitpid(r, &status, 0) != r || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                _exit(1);
            }
            args[1] = "install";
            args[2] = args[3];
            args[3] = NULL;
        }
        execv("/usr/local/bin/locachy-pkg", args);
        _exit(127);
    }
    pkg_job = pid > 0 ? pid : 0;
    snprintf(pkg_action, sizeof pkg_action, "%s", action);
    snprintf(pkg_name, sizeof pkg_name, "%s", package);
}

/* The last line written to a job's log. */
static void log_last_line(const char *path, char *out, size_t size)
{
    char buf[1024];
    out[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    off_t end = lseek(fd, 0, SEEK_END);
    off_t from = end > (off_t)sizeof buf - 1 ? end - (off_t)sizeof buf + 1 : 0;
    ssize_t n = pread(fd, buf, sizeof buf - 1, from);
    close(fd);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';
    char *line = NULL;
    for (char *s = strtok(buf, "\r\n"); s; s = strtok(NULL, "\r\n")) {
        if (*s) {
            line = s;
        }
    }
    if (line) {
        snprintf(out, size, "%s", line);
    }
}

/* The last line locachy-pkg wrote. */
static void pkg_progress(char *out, size_t size)
{
    char line[1024];
    log_last_line(PKG_LOG, line, sizeof line);
    snprintf(out, size, "%.100s", line);
}

/* Has the package job ended? Then say how. */
static void pkg_check(void)
{
    int status;
    if (pkg_job == 0 || waitpid(pkg_job, &status, WNOHANG) != pkg_job) {
        return;
    }
    pkg_job = 0;
    if (strcmp(pkg_name, "fprintd") == 0) {
        load_fingers();
    } else if (face_job()) {
        load_faces();
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && strcmp(pkg_action, "swap") == 0) {
        snprintf(notice, sizeof notice, "%s", dlib_installed() == 2 ?
                 _("Faces are computed on the NVIDIA GPU now.") : _("Faces are computed on the CPU now."));
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        snprintf(notice, sizeof notice,
                 strcmp(pkg_action, "install") == 0 ? _("%s installed; switch it on below.") : _("%s removed."),
                 pkg_title());
    } else {
        snprintf(notice, sizeof notice,
                 strcmp(pkg_action, "install") == 0 ? _("Could not install %s; see %s.") :
                 strcmp(pkg_action, "swap") == 0 ? _("Could not switch %s; see %s.") :
                 _("Could not remove %s; see %s."), pkg_title(), PKG_LOG);
    }
}

/* Software sources (locachy-repos): the repositories' sections and the
 * mirror lists. Switching a section, or ranking the mirrors, runs in the
 * background, its output in REPOS_LOG. A section goes off only when every
 * declared package keeps a repository; else locachy-repos says which ones
 * need it, and rc.conf and pacman.conf stay as they were. */
#define LOCACHY_REPOS "/usr/local/bin/locachy-repos"
#define REPOS_LOG "/run/locachy/settings-repos.log"
static const char *const repo_sections[] = { "core", "extra", "multilib" };
static const char *const repo_labels[] = {
    N_("Core repositories"), N_("Extra repositories"), N_("Multilib (32-bit) repositories"),
};
#define NREPO_SECTIONS ((int)(sizeof repo_sections / sizeof repo_sections[0]))
static int repo_on[NREPO_SECTIONS];
static char repo_level[16], repo_cpu[48], repo_ranked[64], repo_stack[512];
static pid_t repos_job;     /* locachy-repos apply|rank, while it runs */
static char repos_what[24]; /* what it does: "rank", or a section set: "extra=off" */

/* What locachy-repos status says. */
static void load_repos(void)
{
    char out[1024];
    char *const cmd[] = { LOCACHY_REPOS, "status", NULL };
    command_output(cmd, out, sizeof out);
    repo_level[0] = repo_cpu[0] = repo_ranked[0] = repo_stack[0] = '\0';
    for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n")) {
        char *v = strchr(line, '\t');
        if (!v) {
            continue;
        }
        *v++ = '\0';
        for (int i = 0; i < NREPO_SECTIONS; i++) {
            if (strcmp(line, repo_sections[i]) == 0) {
                repo_on[i] = strcmp(v, "on") == 0;
            }
        }
        if (strcmp(line, "level") == 0) {
            snprintf(repo_level, sizeof repo_level, "%s", v);
        } else if (strcmp(line, "cpu") == 0) {
            snprintf(repo_cpu, sizeof repo_cpu, "%s", v);
        } else if (strcmp(line, "stack") == 0) {
            snprintf(repo_stack, sizeof repo_stack, "%s", v);
        } else if (strcmp(line, "ranked") == 0 && *v) {
            char *country = strchr(v, '\t');
            if (country) {
                *country++ = '\0';
            }
            snprintf(repo_ranked, sizeof repo_ranked, country && *country ? "%s, %s" : "%s", v,
                     country ? country : "");
        }
    }
}

/* locachy-repos rank, or apply --set WHAT, in the background. */
static void start_repos_job(const char *what)
{
    mkdir("/run/locachy", 0755);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int log = open(REPOS_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int null = open("/dev/null", O_RDONLY);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
        }
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
        }
        if (strcmp(what, "rank") == 0) {
            execl(LOCACHY_REPOS, "locachy-repos", "rank", (char *)NULL);
        } else {
            execl(LOCACHY_REPOS, "locachy-repos", "apply", "--set", what, (char *)NULL);
        }
        _exit(127);
    }
    repos_job = pid > 0 ? pid : 0;
    snprintf(repos_what, sizeof repos_what, "%s", what);
}

/* Has the repositories' job ended? Then say how: a refusal in its words. */
static void repos_check(void)
{
    int status;
    char line[1024];
    if (repos_job == 0 || waitpid(repos_job, &status, WNOHANG) != repos_job) {
        return;
    }
    repos_job = 0;
    load_repos();
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (strcmp(repos_what, "rank") == 0) {
        snprintf(notice, sizeof notice, ok ? _("Mirrors ranked.") : _("Some mirror lists were not ranked; see %s."),
                 REPOS_LOG);
    } else if (ok) {
        snprintf(notice, sizeof notice, "%s", _("Repositories changed: the next update moves the packages."));
    } else {
        log_last_line(REPOS_LOG, line, sizeof line);
        snprintf(notice, sizeof notice, line[0] ? "%s" : _("Could not change the repositories; see %s."),
                 line[0] ? line : REPOS_LOG);
    }
}

/* What a command (argv[0] a full path) writes on its output within 1.5 s,
 * into buf. */
static void command_output(char *const argv[], char *buf, size_t size)
{
    int p[2];
    size_t len = 0;
    buf[0] = '\0';
    if (pipe2(p, O_CLOEXEC) < 0) {
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(p[1], STDOUT_FILENO);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    struct pollfd wait = { .fd = p[0], .events = POLLIN };
    while (pid > 0 && len < size - 1 && poll(&wait, 1, 1500) > 0) {
        ssize_t n = read(p[0], buf + len, size - 1 - len);
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
    }
    close(p[0]);
    if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    buf[len] = '\0';
}

/* What tailscaled says: BackendState, and the address when connected. */
static void tailscale_state(char *state, size_t ssize, char *ip, size_t isize)
{
    static char *const argv[] = { "/usr/bin/tailscale", "status", "--json", "--peers=false", NULL };
    char buf[16384];
    state[0] = ip[0] = '\0';
    command_output(argv, buf, sizeof buf);
    char *s = strstr(buf, "\"BackendState\": \"");
    if (s) {
        s += 17;
        snprintf(state, ssize, "%.*s", (int)strcspn(s, "\""), s);
    }
    char *self = strstr(buf, "\"Self\":");
    s = self ? strstr(self, "\"TailscaleIPs\": [") : NULL;
    if (s && (s = strchr(s + 17, '"')) != NULL) {
        s++;
        snprintf(ip, isize, "%.*s", (int)strcspn(s, "\""), s);
    }
}

/* The Tailscale account row: refreshed every 2 s at most. */
static const char *tailscale_status(void)
{
    static char line[160];
    static double at = -10;
    if (pkg_job && strcmp(pkg_name, "tailscale") == 0) {
        char progress[112];
        pkg_progress(progress, sizeof progress);
        snprintf(line, sizeof line, strcmp(pkg_action, "install") == 0 ?
                 _("installing: %s") : _("removing: %s"), progress[0] ? progress : "...");
        return line;
    }
    if (!tailscale_installed()) {
        return _("not installed");
    }
    if (!tailscale_enabled) {
        return _("off");
    }
    if (uptime() - at < 2.0) {
        return line;
    }
    at = uptime();
    char state[32], ip[64];
    tailscale_state(state, sizeof state, ip, sizeof ip);
    if (strcmp(state, "Running") == 0) {
        snprintf(line, sizeof line, _("connected, %s"), ip[0] ? ip : _("no address yet"));
    } else if (strcmp(state, "NeedsLogin") == 0) {
        /* TRANSLATORS: Right is the Right arrow key; keep it. */
        snprintf(line, sizeof line, "%s", _("logged out  (Right: log in with a QR code)"));
    } else if (strcmp(state, "Stopped") == 0) {
        /* TRANSLATORS: Right is the Right arrow key; keep it. */
        snprintf(line, sizeof line, "%s", _("stopped  (Right: connect)"));
    } else if (state[0]) {
        snprintf(line, sizeof line, "%s", state);
    } else {
        snprintf(line, sizeof line, "%s", _("starting (tailscaled is not answering yet)"));
    }
    return line;
}

/* A file of clash-client's (CLASH_STATE/name), its first line, into out. */
static void clash_file(const char *name, char *out, size_t size)
{
    char path[128];
    out[0] = '\0';
    snprintf(path, sizeof path, "%s/%s", CLASH_STATE, name);
    FILE *f = fopen(path, "re");
    if (f && fgets(out, (int)size, f)) {
        out[strcspn(out, "\r\n")] = '\0';
    }
    if (f) {
        fclose(f);
    }
}

/* The subscription's host, not its token. */
static void clash_host(char *out, size_t size)
{
    char url[1024];
    clash_file("subscription", url, sizeof url);
    char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    snprintf(out, size, "%.*s", (int)strcspn(s, "/?#"), s);
}

/* The Clash status row: what mihomo's controller says (its unix socket),
 * refreshed every 2 s at most. */
static const char *clash_status(void)
{
    static char line[200];
    static double at = -10;
    char host[128], version[64] = "", secret[64], buf[512];
    if (pkg_job && strcmp(pkg_name, "mihomo") == 0) {
        char progress[112];
        pkg_progress(progress, sizeof progress);
        snprintf(line, sizeof line, strcmp(pkg_action, "install") == 0 ? _("installing: %s") : _("removing: %s"),
                 progress[0] ? progress : "...");
        return line;
    }
    if (!clash_installed()) {
        return _("not installed");
    }
    if (!clash_on) {
        return _("off: the network goes direct");
    }
    clash_host(host, sizeof host);
    if (!host[0]) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        return _("waiting for the subscription (Enter on the line above)");
    }
    if (uptime() - at < 2.0) {
        return line;
    }
    at = uptime();
    static char *const argv[] = { "/usr/bin/curl", "--silent", "--max-time", "1", "--unix-socket",
                                  CLASH_STATE "/controller.sock", "http://localhost/version", NULL };
    command_output(argv, buf, sizeof buf);
    char *s = strstr(buf, "\"version\":\"");
    if (s) {
        s += 11;
        snprintf(version, sizeof version, "%.*s", (int)strcspn(s, "\""), s);
    }
    clash_file("secret", secret, sizeof secret);
    if (version[0]) {
        snprintf(line, sizeof line, _("running %s; panel http://127.0.0.1:9090/ui (Edge), secret %s"), version, secret);
    } else {
        snprintf(line, sizeof line, _("starting (or refused: %s/test.log)"), CLASH_STATE);
    }
    return line;
}

static int prompt(const char *label, char *text, size_t size);

/* The subscription's URL, typed: into CLASH_STATE, which containers do not
 * see; clash-client starts again with it. Nothing typed: as it was. */
static void clash_subscription(void)
{
    char text[1024] = "", path[128], tmp[136];
    if (!prompt(_("Subscription URL (from the provider): "), text, sizeof text) || !text[0]) {
        snprintf(notice, sizeof notice, "%s", _("Subscription unchanged."));
        return;
    }
    if (strncmp(text, "https://", 8) != 0 && strncmp(text, "http://", 7) != 0) {
        snprintf(notice, sizeof notice, "%s", _("A subscription is an http(s) address."));
        return;
    }
    mkdir(CLASH_STATE, 0700);
    snprintf(path, sizeof path, "%s/subscription", CLASH_STATE);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int ok = fd >= 0 && dprintf(fd, "%s\n", text) > 0;
    if (fd >= 0 && close(fd) != 0) {
        ok = 0;
    }
    if (!ok || rename(tmp, path) != 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s."), path);
        return;
    }
    snprintf(notice, sizeof notice, "%s", _("Subscription saved: Clash takes it within seconds."));
}

/* Tor's bridge lines, TOR_STATE/bridges (tor-client makes Tor's Bridge lines
 * of them): how many there are and of which transports, read when settings
 * starts and when they change. */
#define MAX_BRIDGES 32
static int tor_nbridges;
static char tor_transports[32]; /* "obfs4, webtunnel" */
static int tor_bridges_delete_asked;

/* A line as Tor's Bridge directive takes it, "Bridge" or not before it:
 * obfs4 or webtunnel, IPv4:port or [IPv6]:port, a fingerprint or none, then
 * its arguments (KEY=VALUE). Into out without "Bridge"; 0 if it is not one. */
static int tor_bridge_line(const char *in, char *out, size_t size)
{
    char words[16][256];
    int n = 0;
    const char *s = in;
    while (*s && n < 16) {
        s += strspn(s, " \t\r\n");
        size_t len = strcspn(s, " \t\r\n");
        if (len == 0) {
            break;
        }
        if (len >= sizeof words[0]) {
            return 0;
        }
        memcpy(words[n], s, len);
        words[n++][len] = '\0';
        s += len;
    }
    int w = n > 0 && strcasecmp(words[0], "bridge") == 0;
    if (*s || n - w < 2 || (strcmp(words[w], "obfs4") != 0 && strcmp(words[w], "webtunnel") != 0)) {
        return 0;
    }
    const char *addr = words[w + 1], *colon = strrchr(addr, ':');
    if (!colon || !colon[1] || strspn(colon + 1, "0123456789") != strlen(colon + 1) || strlen(colon + 1) > 5) {
        return 0;
    }
    size_t host = (size_t)(colon - addr);
    if (addr[0] == '[') {
        if (host < 3 || addr[host - 1] != ']' || strspn(addr + 1, "0123456789abcdefABCDEF:.") != host - 2) {
            return 0;
        }
    } else {
        unsigned a, b, c, d;
        char end;
        if (sscanf(addr, "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &end) != 5 || end != ':' ||
            strspn(addr, "0123456789.") != host) {
            return 0;
        }
    }
    for (int i = w + 2; i < n; i++) {
        int fingerprint = i == w + 2 && strlen(words[i]) == 40 && strspn(words[i], "0123456789abcdefABCDEF") == 40;
        if (!fingerprint && (strchr(words[i], '=') == NULL || words[i][0] == '=')) {
            return 0;
        }
    }
    size_t len = 0;
    out[0] = '\0';
    for (int i = w; i < n && len < size; i++) {
        len += (size_t)snprintf(out + len, size - len, "%s%s", i > w ? " " : "", words[i]);
    }
    return len < size;
}

static void load_bridges(void)
{
    char line[1024], bridge[1024];
    int obfs4 = 0, webtunnel = 0;
    tor_nbridges = 0;
    FILE *f = fopen(TOR_STATE "/bridges", "re");
    while (f && fgets(line, sizeof line, f)) {
        if (tor_bridge_line(line, bridge, sizeof bridge)) {
            tor_nbridges++;
            obfs4 |= strncmp(bridge, "obfs4 ", 6) == 0;
            webtunnel |= strncmp(bridge, "webtunnel ", 10) == 0;
        }
    }
    if (f) {
        fclose(f);
    }
    snprintf(tor_transports, sizeof tor_transports, "%s%s%s", obfs4 ? "obfs4" : "", obfs4 && webtunnel ? ", " : "",
             webtunnel ? "webtunnel" : "");
}

/* TOR_STATE/bridges as lines (NULL: none, the file goes); 0 if written. */
static int write_bridges(const char *lines)
{
    char path[] = TOR_STATE "/bridges", tmp[] = TOR_STATE "/bridges.new";
    mkdir(TOR_STATE, 0700);
    if (lines == NULL) {
        return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int ok = fd >= 0 && dprintf(fd, "%s", lines) >= 0;
    if (fd >= 0 && close(fd) != 0) {
        ok = 0;
    }
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* A bridge line, typed (or pasted), after those there are; tor-client takes
 * it within seconds when Tor is on through bridges. */
static void tor_add_bridge(void)
{
    char text[1024] = "", bridge[1024], line[1024], kept[1024];
    static char all[MAX_BRIDGES * 1024];
    size_t len = 0;
    if (tor_nbridges >= MAX_BRIDGES) {
        /* TRANSLATORS: Delete is the Delete key; keep it. */
        snprintf(notice, sizeof notice, P_("Already %d bridge line: Delete removes them all.",
                                           "Already %d bridge lines: Delete removes them all.", MAX_BRIDGES),
                 MAX_BRIDGES);
        return;
    }
    if (!prompt(_("Bridge line (obfs4 or webtunnel, from bridges.torproject.org): "), text, sizeof text) || !text[0]) {
        snprintf(notice, sizeof notice, "%s", _("Bridges unchanged."));
        return;
    }
    if (!tor_bridge_line(text, bridge, sizeof bridge)) {
        /* TRANSLATORS: Keep obfs4|webtunnel ADDRESS:PORT [FINGERPRINT] KEY=VALUE... as written. */
        snprintf(notice, sizeof notice, "%s", _("Not an obfs4 or WebTunnel bridge line: obfs4|webtunnel ADDRESS:PORT "
                                                "[FINGERPRINT] KEY=VALUE..."));
        return;
    }
    all[0] = '\0';
    FILE *f = fopen(TOR_STATE "/bridges", "re");
    while (f && fgets(line, sizeof line, f) && len < sizeof all) {
        if (tor_bridge_line(line, kept, sizeof kept)) {
            len += (size_t)snprintf(all + len, sizeof all - len, "%s\n", kept);
        }
    }
    if (f) {
        fclose(f);
    }
    if (len < sizeof all) {
        len += (size_t)snprintf(all + len, sizeof all - len, "%s\n", bridge);
    }
    if (len >= sizeof all || write_bridges(all) < 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s/bridges."), TOR_STATE);
        return;
    }
    load_bridges();
    snprintf(notice, sizeof notice, tor_choice() == 2 ?
             P_("Bridge added (%d now): Tor takes it within seconds.",
                "Bridge added (%d now): Tor takes it within seconds.", tor_nbridges) :
             P_("Bridge added (%d now).", "Bridge added (%d now).", tor_nbridges), tor_nbridges);
}

/* Delete twice: every bridge line goes, unless Tor goes out through them. */
static void tor_remove_bridges(void)
{
    if (tor_choice() == 2) {
        snprintf(notice, sizeof notice, "%s", _("Tor goes out through these bridges: put it through Clash or off first."));
        return;
    }
    if (!tor_bridges_delete_asked) {
        tor_bridges_delete_asked = 1;
        /* TRANSLATORS: Delete is the Delete key; keep it. */
        snprintf(notice, sizeof notice, "%s", _("Delete again: remove every bridge line."));
        return;
    }
    tor_bridges_delete_asked = 0;
    if (write_bridges(NULL) < 0) {
        snprintf(notice, sizeof notice, _("Cannot remove %s/bridges."), TOR_STATE);
        return;
    }
    load_bridges();
    snprintf(notice, sizeof notice, "%s", _("Bridge lines removed."));
}

/* tor-enforce ACTION [FRONT]: 0 if it did it. */
static int tor_enforce(const char *action, const char *front)
{
    char *const argv[] = { TOR_ENFORCE, (char *)action, (char *)front, NULL };
    int status = -1;
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Is the front of choice (1 Clash, 2 bridges) there to go out through? */
static int tor_front_ready(int choice)
{
    return choice == 0 || (choice == 1 && clash_installed() && clash_on) ||
           (choice == 2 && tor_nbridges > 0 && lyrebird_installed());
}

/* Tor off, or on through a front: its firewall first (none: off), then
 * rc.conf, which process 1 and tor-client take from there. */
static void tor_set(int choice)
{
    const char *front = choice == 2 ? "bridges" : "mihomo";
    int was = tor_choice();
    if (choice > 0 && tor_enforce("load", front) < 0) {
        snprintf(notice, sizeof notice, _("Cannot load Tor's firewall (%s): Tor left as it was."), TOR_ENFORCE);
        return;
    }
    if ((choice > 0 && rc_set("tor_front", front) < 0) || rc_set("service_tor", choice ? TOR_SERVICE : NULL) < 0) {
        /* The firewall as it was. */
        if (was == 0) {
            tor_enforce("remove", NULL);
        } else {
            tor_enforce("load", was == 2 ? "bridges" : "mihomo");
        }
        snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
        return;
    }
    tor_on = choice != 0;
    if (choice) {
        snprintf(tor_front, sizeof tor_front, "%s", front);
    }
    kill(1, SIGHUP); /* process 1 starts or ends tor-client */
    if (choice == 0 && tor_enforce("remove", NULL) < 0) {
        snprintf(notice, sizeof notice, _("Tor off, but its firewall is still there: nothing goes out (%s remove)."),
                 TOR_ENFORCE);
        return;
    }
    snprintf(notice, sizeof notice, "%s", choice == 0 ? _("Tor off: the network goes direct.") :
             choice == 1 ? _("Tor through Clash: all the network goes through Tor; Clash carries Tor only.") :
             _("Tor through bridges: all the network goes through Tor, Tor through the bridges."));
}

/* The Tor status row: what tor-client says, refreshed every 2 s at most. */
static const char *tor_status(void)
{
    static char line[200];
    static double at = -10;
    if (pkg_job && tor_job()) {
        char progress[112];
        pkg_progress(progress, sizeof progress);
        snprintf(line, sizeof line, strcmp(pkg_action, "install") == 0 ? _("installing: %s") : _("removing: %s"),
                 progress[0] ? progress : "...");
        return line;
    }
    if (!tor_installed()) {
        return _("not installed");
    }
    if (!tor_on) {
        return _("off: the network goes direct");
    }
    if (uptime() - at < 2.0) {
        return line;
    }
    at = uptime();
    static char *const argv[] = { TOR_SERVICE, "--status", NULL };
    command_output(argv, line, sizeof line);
    line[strcspn(line, "\n")] = '\0';
    if (!line[0]) {
        snprintf(line, sizeof line, _("no answer from %s"), TOR_SERVICE);
    }
    return line;
}

/* "a\:b" in nmcli's terse output is "a:b". */
static void nm_unescape(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
        }
        *d++ = *s;
    }
    *d = '\0';
}

static void load_mac_default(void)
{
    char line[128];
    FILE *in = fopen(NM_MAC_CONF, "re");
    mac_default = 0;
    while (in && fgets(line, sizeof line, in)) {
        for (int m = 1; m < NMAC_MODES; m++) {
            if (strncmp(line, "wifi.cloned-mac-address=", 24) == 0 && strncmp(line + 24, mac_modes[m], strlen(mac_modes[m])) == 0) {
                mac_default = m;
            }
        }
    }
    if (in) {
        fclose(in);
    }
}

static void load_nets(void)
{
    static char *const list[] = { "/usr/bin/nmcli", "-t", "-f", "UUID,TYPE,NAME", "connection", "show", NULL };
    char buf[8192];
    nnets = 0;
    load_mac_default();
    command_output(list, buf, sizeof buf);
    for (char *line = strtok(buf, "\n"); line != NULL && nnets < MAX_NETS; line = strtok(NULL, "\n")) {
        char *type = strchr(line, ':'), *name = type ? strchr(type + 1, ':') : NULL;
        if (name == NULL) {
            continue;
        }
        *type++ = '\0';
        *name++ = '\0';
        const char *property = strcmp(type, "802-11-wireless") == 0 ? "802-11-wireless.cloned-mac-address" :
                               strcmp(type, "802-3-ethernet") == 0 ? "802-3-ethernet.cloned-mac-address" : NULL;
        if (property == NULL) {
            continue;
        }
        struct net *n = &nets[nnets++];
        snprintf(n->uuid, sizeof n->uuid, "%s", line);
        nm_unescape(name);
        snprintf(n->name, sizeof n->name, "%s", name);
        snprintf(n->property, sizeof n->property, "%s", property);
    }
    for (int i = 0; i < nnets; i++) {
        char *const get[] = { "/usr/bin/nmcli", "-g", nets[i].property, "connection", "show", nets[i].uuid, NULL };
        command_output(get, buf, sizeof buf);
        nets[i].named = buf[0] && buf[0] != '\n';
        nets[i].mac = nets[i].named ? 0 : mac_default;
        for (int m = 1; m < NMAC_MODES; m++) {
            if (strncmp(buf, mac_modes[m], strlen(mac_modes[m])) == 0) {
                nets[i].mac = m;
            }
        }
    }
}

/* Network n's MAC address (mac_modes[mac]), from when it next connects. */
static void set_net_mac(struct net *n, int mac)
{
    char *const argv[] = { "/usr/bin/nmcli", "connection", "modify", n->uuid, n->property,
                           (char *)mac_modes[mac], NULL };
    int status = -1;
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(notice, sizeof notice, _("Could not change %.60s (nmcli)."), n->name);
        return;
    }
    n->mac = mac;
    n->named = 1;
    snprintf(notice, sizeof notice, mac == 1 ? _("%.60s: a new MAC address each time, from when it next connects.") :
             mac == 2 ? _("%.60s: the card's real MAC address, from when it next connects.") :
             _("%.60s: its own MAC address, the same each time, from when it next connects."), n->name);
}

/* The MAC address of new networks, and of saved ones that name none
 * (mac_modes[mac]): NM_MAC_CONF for NetworkManager, read again. */
static void set_mac_default(int mac)
{
    if (mac == 0) {
        if (unlink(NM_MAC_CONF) < 0 && errno != ENOENT) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), NM_MAC_CONF);
            return;
        }
    } else {
        FILE *out = fopen(NM_MAC_CONF ".new", "we");
        int ok = out != NULL &&
                 fprintf(out, "# Written by settings (MAC on new networks): the MAC address of a network\n"
                              "# that names none of its own (30-locachy-network.conf: stable).\n[connection]\n"
                              "wifi.cloned-mac-address=%s\nethernet.cloned-mac-address=%s\n",
                         mac_modes[mac], mac_modes[mac]) > 0;
        if (out && fclose(out) != 0) {
            ok = 0;
        }
        if (!ok || rename(NM_MAC_CONF ".new", NM_MAC_CONF) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), NM_MAC_CONF);
            return;
        }
    }
    mac_default = mac;
    for (int i = 0; i < nnets; i++) {
        if (!nets[i].named) {
            nets[i].mac = mac; /* those that name none follow it */
        }
    }
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execl("/usr/bin/nmcli", "nmcli", "general", "reload", "conf", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0); /* not read again: at NetworkManager's next start */
    }
    snprintf(notice, sizeof notice, _("New networks, and saved ones that name none: %s, from when they next connect."),
             _(mac_labels[mac_default]));
}

/* tailscale up --qr: its address and code on the screen until it has logged
 * in (or failed), or q / Esc cancels it. The code's lines are "##" for a
 * light module and "  " for a dark one: a 12x24 cell pair is one square. */
static void tailscale_login(void)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0) {
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
        }
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        execl("/usr/bin/tailscale", "tailscale", "up", "--qr", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return;
    }
    static char out[32768];
    size_t len = 0;
    int status = -1, cancelled = 0;
    for (;;) {
        ssize_t n;
        while (len < sizeof out - 1 && (n = read(p[0], out + len, sizeof out - 1 - len)) > 0) {
            len += (size_t)n;
        }
        out[len] = '\0';
        if (waitpid(pid, &status, WNOHANG) == pid) {
            break;
        }
        /* The screen: title, address, code, the other lines, the keys. */
        char copy[sizeof out];
        memcpy(copy, out, len + 1);
        screen_fill(0, 0, screen_cols(), screen_rows(), BG);
        screen_bar(_("Tailscale login"), NULL);
        screen_text(2, 2, _("Scan the code with a phone, or open the address on another device:"), FG, BG);
        int r = 6, qr_rows = 0;
        for (char *s = copy, *end; *s; s = end) {
            end = s + strcspn(s, "\n");
            int last = *end == '\0';
            *end = '\0';
            end = last ? end : end + 1;
            char *t = s + strspn(s, " \t");
            int code = strlen(s) >= 20 && strspn(s, "# ") == strlen(s);
            if (strncmp(t, "https://", 8) == 0) {
                screen_text(4, 3, t, ACCENT, BG);
            } else if (code) {
                if (r + qr_rows < screen_rows() - 4) {
                    for (int c = 0; s[c] && 2 + c < screen_cols(); c++) {
                        screen_fill(2 + c, r + qr_rows, 1, 1, s[c] == '#' ? 0xFFFFFF : 0x000000);
                    }
                }
                qr_rows++;
            } else if (*t && strncmp(t, "To authenticate", 15) != 0) {
                screen_text(2, screen_rows() - 3, t, DIM, BG);
            }
        }
        if (qr_rows && r + qr_rows >= screen_rows() - 4) {
            screen_text(2, r, _("(the screen is too small for the code: use the address)"), DIM, BG);
        }
        /* TRANSLATORS: q and Esc are key names; keep them. */
        screen_text(2, screen_rows() - 2, _("q / Esc: cancel"), ACCENT, BG);
        screen_flush();
        int key = key_poll_timeout(p[0], 500);
        if (key == 'q' || key == 27) {
            kill(-pid, SIGTERM);
            waitpid(pid, &status, 0);
            cancelled = 1;
            break;
        }
    }
    close(p[0]);
    if (cancelled) {
        snprintf(notice, sizeof notice, "%s", _("Tailscale login cancelled."));
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        snprintf(notice, sizeof notice, "%s", _("Tailscale: logged in."));
    } else {
        snprintf(notice, sizeof notice, _("Tailscale login failed (status %d)."),
                 WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    }
}

/* fprintd-enroll -f FINGER USER: what it says, until it ends or q / Esc
 * cancels it. The reader asks for the finger several times. */
static void enroll_finger(const char *finger)
{
    const char *user = user_name();
    int p[2];
    if (!user || pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0) {
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
        }
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        execl("/usr/bin/fprintd-enroll", "fprintd-enroll", "-f", finger, user, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return;
    }
    static char out[8192];
    size_t len = 0;
    int status = -1, cancelled = 0;
    char title[96];
    const char *finger_label = finger;
    for (int i = 0; i < NFINGERS; i++) {
        if (strcmp(fingers[i], finger) == 0) {
            finger_label = _(finger_labels[i]);
        }
    }
    snprintf(title, sizeof title, _("Fingerprint: %s"), finger_label);
    for (;;) {
        ssize_t n;
        while (len < sizeof out - 1 && (n = read(p[0], out + len, sizeof out - 1 - len)) > 0) {
            len += (size_t)n;
        }
        out[len] = '\0';
        int ended = waitpid(pid, &status, WNOHANG) == pid;
        screen_fill(0, 0, screen_cols(), screen_rows(), BG);
        screen_bar(title, NULL);
        screen_text(2, 2, _("Touch or swipe the finger on the reader, again each time it asks."), FG, BG);
        /* Its last lines, from the bottom up. */
        int r = screen_rows() - 4;
        for (char *end = out + len; end > out && r > 3;) {
            char *start = end;
            while (start > out && start[-1] != '\n') {
                start--;
            }
            if (end > start) {
                char line[160];
                snprintf(line, sizeof line, "%.*s", (int)(end - start), start);
                screen_text(4, r--, line, strstr(line, "enroll-completed") ? ACCENT : FG, BG);
            }
            end = start > out ? start - 1 : out;
        }
        screen_text(2, screen_rows() - 2, _("q / Esc: cancel"), ACCENT, BG);
        screen_flush();
        if (ended) {
            break;
        }
        int key = key_poll_timeout(p[0], 500);
        if (key == 'q' || key == 27) {
            kill(-pid, SIGTERM);
            waitpid(pid, &status, 0);
            cancelled = 1;
            break;
        }
    }
    close(p[0]);
    if (cancelled) {
        snprintf(notice, sizeof notice, "%s", _("Fingerprint not added."));
    } else if (strstr(out, "enroll-completed")) {
        snprintf(notice, sizeof notice, _("Fingerprint added: %s."), finger_label);
    } else {
        char *last = strstr(out, "Enroll result: ");
        for (char *s = last; s; s = strstr(s + 1, "Enroll result: ")) {
            last = s;
        }
        snprintf(notice, sizeof notice, _("Fingerprint not added: %.100s"),
                 last ? last + 15 : WIFEXITED(status) ? _("fprintd-enroll failed") : _("fprintd-enroll was killed"));
        notice[strcspn(notice, "\n")] = '\0';
    }
    load_fingers();
}

static void format_bar(char *out, size_t size, int val, int min, int max)
{
    int width = 20;
    int filled = (val - min) * width / (max > min ? max - min : 1);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    size_t p = 0;
    out[p++] = '[';
    for (int i = 0; i < width && p + 2 < size; i++) {
        out[p++] = (i < filled) ? '#' : '-';
    }
    out[p++] = ']';
    out[p] = '\0';
}

/* The rows above the screens: the pointer and battery ones, then Tailscale
 * (installed, on, its account), Clash (installed, its mode, subscription,
 * status), Tor (installed, its front, bridges, status), Bluetooth (tools
 * installed, service, devices), each saved network's
 * MAC address, the wallpaper, the terminal key, the time zone, network time
 * and date, and the debug cable link. */
static int ts_index(void)
{
    return tp_present ? 5 : 2;
}

static int cl_index(void)
{
    return ts_index() + 3;
}

static int tr_index(void)
{
    return cl_index() + 4;
}

static int bt_index(void)
{
    return tr_index() + 4;
}

static int nw_index(void)
{
    return bt_index() + 3;
}

/* After the saved networks, the MAC address of new ones. */
static int wp_index(void)
{
    return nw_index() + nnets + 1;
}

static int tk_index(void)
{
    return wp_index() + 1;
}

static int tz_index(void)
{
    return tk_index() + 1;
}

static int link_index(void)
{
    return tz_index() + 3;
}

/* The password, whether the screen is locked at boot, the lock key, when it
 * locks for want of input. */
static int sec_index(void)
{
    return link_index() + 1;
}

/* Fingerprints: fprintd installed, unlock with one, the user's, add one. */
static int fp_index(void)
{
    return sec_index() + 4;
}

/* Faces: howdy installed, what computes them (dlib on the CPU or CUDA),
 * unlock with one, the user's, the camera, add one, the infrared emitter. */
static int fc_index(void)
{
    return fp_index() + 4;
}

/* Cloud drives (rclone): installed, mounted under ~/Drives, remotes in rclone config. */
static int rclone_index(void)
{
    return fc_index() + 7;
}

static int lang_index(void)
{
    return rclone_index() + 3;
}

static int host_index(void)
{
    return lang_index() + 1;
}

static int ime_index(void)
{
    return host_index() + 1;
}

static int tpm_index(void)
{
    return ime_index() + 2;
}

static int bri_index(void)
{
    return tpm_index() + 1;
}

static int mach_index(void)
{
    return bri_index() + 1;
}

/* Source updates: the repository, its branch. */
static int upd_index(void)
{
    return mach_index() + 1;
}

/* Boot: the kernel's messages, options added, the image (made again). */
static int boot_index(void)
{
    return upd_index() + 2;
}

static int opt_index(void)
{
    return boot_index() + 3;
}

/* Software sources: a row for each section, the mirrors. */
static int repo_index(void)
{
    return opt_index() + NOPT_MODULES;
}

/* The terminal (program_term, user-shell): its shell on the network or not. */
static int tnet_index(void)
{
    return repo_index() + NREPO_SECTIONS + 1;
}

/* Touchpads: scrolling around their edge (rc.conf touchpad_circular). */
static int circ_index(void)
{
    return tnet_index() + 1;
}

/* The input method's keys (rc.conf key_ime, key_ime_cn, key_ime_raw,
 * key_ime_width). */
static int imek_index(void)
{
    return circ_index() + 1;
}

static int num_hw_items(void)
{
    return imek_index() + NIME_KEYS;
}

static void rclone_remotes_summary(char *out, size_t size)
{
    const char *user = user_name();
    char path[256], line[256], names[128] = "";
    int n = 0;
    if (!user) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(out, size, "%s", _("none   Enter: configure (rclone config)"));
        return;
    }
    snprintf(path, sizeof path, "/home/%s/.config/rclone/rclone.conf", user);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        size_t len = strlen(line);
        if (len > 2 && line[0] == '[' && line[len - 1] == ']') {
            line[len - 1] = '\0';
            if (n < 4) {
                size_t cur = strlen(names);
                snprintf(names + cur, sizeof names - cur, "%s%.20s", n ? ", " : "", line + 1);
            }
            n++;
        }
    }
    if (f) fclose(f);
    if (n == 0) {
        snprintf(out, size, "%s", _("none   Enter: configure (rclone config)"));
    } else {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(out, size, P_("%d (%s%s)   Enter: configure (rclone config)",
                               "%d (%s%s)   Enter: configure (rclone config)", n),
                 n, names, n > 4 ? ", ..." : "");
    }
}

static int by_name(const void *a, const void *b)
{
    return strcmp(a, b);
}

/* The wallpaper, chosen in the file manager (files_pick): from the folder of
 * the one there is, or ~/Pictures. */
static void choose_wallpaper(void)
{
    char home[256], dirs[256], dir[512], path[sizeof wallpaper_path];
    const char *keep = NULL;
    open_user_dirs(home, sizeof home, dirs, sizeof dirs);
    snprintf(dir, sizeof dir, "%.250s/Pictures", home);
    if (wallpaper_path[0] && strrchr(wallpaper_path, '/')) {
        snprintf(dir, sizeof dir, "%.*s", (int)(strrchr(wallpaper_path, '/') - wallpaper_path), wallpaper_path);
        keep = strrchr(wallpaper_path, '/') + 1;
    }
    /* TRANSLATORS: Enter is the Enter key; keep it. */
    if (!files_pick(dir, keep, "image/", _("The wallpaper: Enter on a picture."), path, sizeof path)) {
        snprintf(notice, sizeof notice, "%s", _("Wallpaper left as it is."));
        return;
    }
    if (rc_set("wallpaper", path) < 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
        return;
    }
    memcpy(wallpaper_path, path, sizeof wallpaper_path);
    snprintf(notice, sizeof notice, "%s", _("Wallpaper saved; the shared display shows it when it is next shown."));
}

static void draw(void);

/* The labels of the input method's key rows. */
static const char *const ime_key_labels[NIME_KEYS] = {
    N_("Input method key"), N_("Chinese input key"), N_("Direct input key"), N_("Punctuation width key"),
};

/* The action keys.c gives for rc_name's keys (0: none of its own). */
static int key_action(const char *rc_name)
{
    static const struct { const char *name; int action; } actions[] = {
        { "key_lock", K_ACT_LOCK }, { "key_ime", K_CTRL_SPACE }, { "key_ime_cn", K_ACT_IME_CN },
        { "key_ime_raw", K_ACT_IME_RAW }, { "key_ime_width", K_ACT_IME_WIDTH },
    };
    for (size_t i = 0; i < sizeof actions / sizeof actions[0]; i++) {
        if (strcmp(rc_name, actions[i].name) == 0) return actions[i].action;
    }
    return 0;
}

/* name is one of the keys of another of rc.conf's settings than rc_name. */
static int key_taken(const char *rc_name, const char *name)
{
    const char *values[2 + NIME_KEYS] = { terminal_key, lock_key };
    const char *names[2 + NIME_KEYS] = { "key_terminal", "key_lock" };
    for (int i = 0; i < NIME_KEYS; i++) {
        values[2 + i] = ime_keys[i];
        names[2 + i] = ime_key_names[i];
    }
    for (int i = 0; i < 2 + NIME_KEYS; i++) {
        char keys[128], *save = NULL;
        if (strcmp(names[i], rc_name) == 0) continue;
        snprintf(keys, sizeof keys, "%s", values[i]);
        for (char *k = strtok_r(keys, " ", &save); k; k = strtok_r(NULL, " ", &save)) {
            if (strcmp(k, name) == 0) return 1;
        }
    }
    return 0;
}

/* A key of rc.conf (rc_name, into value; label its row's): the next key
 * pressed with Ctrl, Alt or Super, or, where bare, one that types nothing
 * (Henkan_Mode, the JIS key right of the space bar; one bound to nothing
 * comes as K_NAMED while this
 * waits), not another setting's; Esc keeps the one there is. Saved, then
 * applied (applied). */
static void choose_key(const char *label, const char *rc_name, char *value, size_t size, int bare,
                       void (*applied)(void))
{
    const char *now = value[0] ? value : _("none");
    int own = key_action(rc_name);
    /* TRANSLATORS: %s is a row's label (Lock key); Ctrl, Alt, Super and Esc are key names; keep them. */
    snprintf(notice, sizeof notice, bare ? _("%s: press it, alone if it types nothing (Henkan_Mode), "
                                             "else with Ctrl, Alt or Super   Esc: keep %.40s") :
             _("%s: press it, with Ctrl, Alt or Super   Esc: keep %.40s"), label, now);
    draw();
    keys_name_any(1);
    for (;;) {
        int key = key_get(), mods = keys_modifiers();
        char name[128];
        if (key == K_SHOWN) {
            draw();
            continue;
        }
        if (key == 0 || key == K_POINTER) {
            continue;
        }
        if (key == 27 && mods == 0) {
            snprintf(notice, sizeof notice, _("%s left as it is."), label);
            break;
        }
        int known = keys_last_name(name, sizeof name) == 0;
        int typeless = known && strchr(name, '+') == NULL && strlen(name) > 1 && strcmp(name, "Space") != 0 &&
                       strcmp(name, "Enter") != 0 && strcmp(name, "Tab") != 0; /* Henkan_Mode */
        int action = key == K_ACT_HOME || key == K_ACT_LOCK || key == K_CTRL_SPACE || key == K_ACT_IME_CN ||
                     key == K_ACT_IME_RAW || key == K_ACT_IME_WIDTH || (key >= K_VOL_UP && key <= K_BRI_DOWN);
        if ((action && key != own) || (known && key_taken(rc_name, name))) {
            /* TRANSLATORS: Esc is the Esc key; keep it. */
            snprintf(notice, sizeof notice, _("That key is another of rc.conf's; press another   Esc: keep %.40s"), now);
        } else if (!known || (!(mods & (KEYS_CTRL | KEYS_ALT | KEYS_META)) && !(bare && typeless))) {
            /* TRANSLATORS: Ctrl, Alt, Super, Space, Enter, Tab and Esc are key names; keep them. */
            snprintf(notice, sizeof notice, "%s", bare ?
                     _("A key that types nothing (Henkan_Mode), or Ctrl, Alt or Super with a letter, digit, Space, "
                       "Enter or Tab") :
                     _("Ctrl, Alt or Super with a letter, digit, Space, Enter or Tab"));
        } else if (rc_set(rc_name, name) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            break;
        } else {
            snprintf(value, size, "%s", name);
            applied();
            snprintf(notice, sizeof notice, _("%s: %s, saved and applied."), label, value);
            break;
        }
        draw();
    }
    keys_name_any(0);
}

/* Process 1 reads rc.conf's keys again (key_lock). */
static void reread_keys(void)
{
    kill(1, SIGHUP);
}

/* The programs named comm get sig (labwc its configuration, locachy-bar
 * rc.conf's keys). */
static void signal_programs(const char *comm_name, int sig)
{
    DIR *proc = opendir("/proc");
    struct dirent *e;
    while (proc && (e = readdir(proc)) != NULL) {
        char path[300], comm[32];
        if (!isdigit((unsigned char)e->d_name[0])) {
            continue;
        }
        snprintf(path, sizeof path, "/proc/%.255s/comm", e->d_name);
        read_one_line(path, comm, sizeof comm);
        if (strcmp(comm, comm_name) == 0) {
            kill(atoi(e->d_name), sig);
        }
    }
    if (proc) {
        closedir(proc);
    }
}

/* The input method's keys: the shared display's bar reads them again; the
 * direct programs as each next gets the screen (keys.c). */
static void reread_ime_keys(void)
{
    reread_keys();
    signal_programs("locachy-bar", SIGHUP);
}

static int prompt_as(const char *label, char *text, size_t size, int hidden);

/* The user's (uid 1000) password, through PAM's passwd service as root: what
 * its modules ask (the new password, again) is typed hidden at the bottom. */
static void change_password(void)
{
    struct passwd *pw = getpwuid(1000);
    struct auth a;
    char text[256];
    if (pw == NULL || auth_start(&a, "passwd", pw->pw_name, AUTH_CHANGE) < 0) {
        snprintf(notice, sizeof notice, "%s", _("Cannot change the password (no user, or PAM)."));
        return;
    }
    while (!a.done) {
        struct pollfd w = { .fd = a.from, .events = POLLIN };
        if (poll(&w, 1, 5000) <= 0) {
            break;
        }
        if (!auth_read(&a) || !a.asking) {
            continue;
        }
        text[0] = '\0';
        char label[288];
        snprintf(label, sizeof label, "%s ", a.prompt);
        if (!prompt_as(label, text, sizeof text, a.asking == AUTH_ASKS_HIDDEN)) {
            auth_stop(&a);
            explicit_bzero(text, sizeof text);
            snprintf(notice, sizeof notice, "%s", _("Password left as it was."));
            return;
        }
        auth_answer(&a, text);
        explicit_bzero(text, sizeof text);
    }
    if (!a.done) {
        auth_stop(&a);
        snprintf(notice, sizeof notice, "%s", _("Password left as it was (PAM did not answer)."));
    } else if (a.status == 0) {
        snprintf(notice, sizeof notice, _("Password of %.40s changed."), pw->pw_name);
    } else {
        snprintf(notice, sizeof notice, _("Password left as it was: %.100s"), a.message[0] ? a.message : _("refused"));
    }
}

/* Default applications: the rules of open.conf (open.c), which A in an
 * open-with chooser makes; the rows below the screens. Read when settings is
 * shown, so a rule set to "no rule" stays a row until then. */
#define MAX_RULES 32
static char rule_types[MAX_RULES][OPEN_TYPE], rule_values[MAX_RULES][OPEN_NAME];
static int nrules;

static void rule_label(int i, char *out, size_t size)
{
    if (strcmp(rule_values[i], "ask") == 0) {
        snprintf(out, size, "%s", _("ask every time"));
    } else if (!rule_values[i][0]) {
        snprintf(out, size, "%s", _("no rule"));
    } else {
        open_label(rule_values[i], rule_types[i], out, size);
    }
}

/* The next of: each container that opens the type (or each container, if
 * none says so), ask every time, no rule. */
static void cycle_rule(int i, int dir)
{
    char names[24][OPEN_NAME];
    int n = open_candidates(rule_types[i], names, 22), at = -1;
    if (n == 0) {
        n = open_containers(names, 22);
    }
    snprintf(names[n++], OPEN_NAME, "ask");
    names[n++][0] = '\0';
    for (int k = 0; k < n; k++) {
        if (strcmp(names[k], rule_values[i]) == 0) {
            at = k;
        }
    }
    at = at < 0 ? 0 : (at + (dir > 0 ? 1 : n - 1)) % n;
    memcpy(rule_values[i], names[at], OPEN_NAME);
    if (open_set_rule(rule_types[i], names[at][0] ? names[at] : NULL) == 0) {
        snprintf(notice, sizeof notice, _("Saved to %s."), OPEN_RULES);
    } else {
        snprintf(notice, sizeof notice, _("Cannot write %s."), OPEN_RULES);
    }
}

/* The time zone: /etc/localtime, a link to its zoneinfo file, which
 * timezone-client sets while it is automatic (service_timezone). */
static void current_zone(char *out, size_t size)
{
    char target[256];
    ssize_t n = readlink("/etc/localtime", target, sizeof target - 1);
    const char *zone = access("/etc/localtime", F_OK) == 0 ? _("(a file of its own)") : "UTC";
    if (n > 0) {
        target[n] = '\0';
        const char *p = strstr(target, "zoneinfo/");
        zone = p ? p + 9 : target;
    }
    snprintf(out, size, "%.100s", zone);
}

/* Automatic, UTC, and the zones of zone1970.tab (one for each region whose
 * clocks have agreed since 1970), sorted. */
#define MAX_ZONES 512
static char zones[MAX_ZONES][48];
static int nzones;

static void load_zones(void)
{
    char line[512];
    if (nzones > 0) {
        return;
    }
    snprintf(zones[nzones++], sizeof zones[0], "Automatic");
    snprintf(zones[nzones++], sizeof zones[0], "UTC");
    FILE *in = fopen(ZONEINFO "/zone1970.tab", "r");
    while (in && nzones < MAX_ZONES && fgets(line, sizeof line, in)) {
        char *zone = line[0] == '#' ? NULL : strchr(line, '\t'); /* codes, place, ZONE, comment */
        zone = zone ? strchr(zone + 1, '\t') : NULL;
        if (zone) {
            zone++;
            zone[strcspn(zone, "\t\n")] = '\0';
            snprintf(zones[nzones++], sizeof zones[0], "%.47s", zone);
        }
    }
    if (in) {
        fclose(in);
    }
    qsort(zones[2], (size_t)(nzones - 2), sizeof zones[0], by_name);
}

static void choose_zone(const char *zone)
{
    char target[128];
    if (strcmp(zone, "Automatic") == 0) {
        if (rc_set("service_timezone", TIMEZONE_SERVICE) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        tz_auto = 1;
        kill(1, SIGHUP); /* process 1 starts timezone-client */
        snprintf(notice, sizeof notice, "%s", _("Time zone: automatic, from where the network is (asked of ipinfo.io)."));
        return;
    }
    snprintf(target, sizeof target, ZONEINFO "/%s", zone);
    if (access(target, R_OK) != 0 || rc_set("service_timezone", NULL) < 0) {
        snprintf(notice, sizeof notice, _("Cannot set the time zone %s."), zone);
        return;
    }
    tz_auto = 0;
    kill(1, SIGHUP); /* the automatic one ends */
    unlink("/etc/localtime.new");
    if (symlink(target, "/etc/localtime.new") < 0 || rename("/etc/localtime.new", "/etc/localtime") < 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s."), "/etc/localtime");
        return;
    }
    snprintf(notice, sizeof notice, _("Time zone %s: programs started from now on use it."), zone);
}

/* The time zones, filtered by what is typed: Enter chooses, Esc goes back. */
static void pick_zone(void)
{
    static int match[MAX_ZONES];
    char filter[32] = "", current[128], line[256];
    int sel = -1, top = 0;
    load_zones();
    current_zone(current, sizeof current);
    for (;;) {
        int n = 0, list = 5, height = screen_rows() - list - 2;
        for (int i = 0; i < nzones; i++) {
            if (!filter[0] || strcasestr(zones[i], filter)) {
                if (sel < 0 && (tz_auto ? i == 0 : strcmp(zones[i], current) == 0)) {
                    sel = n; /* first shown on the one there is */
                }
                match[n++] = i;
            }
        }
        sel = sel < 0 ? 0 : sel >= n ? (n > 0 ? n - 1 : 0) : sel;
        top = sel < top ? sel : sel >= top + height ? sel - height + 1 : top;
        screen_fill(0, 0, screen_cols(), screen_rows(), BG);
        screen_bar(_("settings"), NULL);
        /* TRANSLATORS: Up/Down/PgUp/PgDn, Enter and Esc are key names; keep them. */
        screen_text(2, 2, _("Time zone: type to filter   Up/Down/PgUp/PgDn   Enter: choose   Esc: back"), ACCENT, BG);
        snprintf(line, sizeof line, _("Filter: %s_"), filter);
        screen_text(2, 3, line, FG, BG);
        if (n == 0) {
            screen_text(4, list, _("No time zone has that in its name."), DIM, BG);
        }
        for (int k = 0; k < height && top + k < n; k++) {
            int i = match[top + k], on = top + k == sel;
            int now = tz_auto ? i == 0 : strcmp(zones[i], current) == 0;
            snprintf(line, sizeof line, "%s%s", i == 0 ? _("Automatic: from where the network is") : zones[i],
                     now ? _("   (now)") : "");
            screen_fill(0, list + k, screen_cols(), 1, on ? ACCENT : BG);
            screen_text(4, list + k, line, on ? BG : FG, on ? ACCENT : BG);
        }
        screen_flush();
        int key = key_get();
        size_t len = strlen(filter);
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (e->wheel) {
                sel = sel - 3 * e->wheel < 0 ? 0 : sel - 3 * e->wheel;
            } else if (e->button == BTN_LEFT && e->pressed && e->row >= list && e->row - list < height &&
                       top + e->row - list < n) {
                sel = top + e->row - list;
                key = e->clicks == 2 ? '\n' : 0;
            }
        }
        if (key == 27) {
            return;
        } else if (key == '\n' && n > 0) {
            choose_zone(zones[match[sel]]);
            return;
        } else if (key == K_UP && sel > 0) {
            sel--;
        } else if (key == K_DOWN && sel < n - 1) {
            sel++;
        } else if (key == K_PGUP) {
            sel = sel > height ? sel - height : 0;
        } else if (key == K_PGDN) {
            sel = sel + height < n ? sel + height : n - 1;
        } else if (key == '\b' && len > 0) {
            while (len > 1 && ((unsigned char)filter[len - 1] & 0xC0) == 0x80) len--; /* a whole character */
            filter[len - 1] = '\0';
            sel = top = 0;
        } else if (keys_typed(key) && len + strlen(keys_typed(key)) < sizeof filter) {
            strcat(filter, keys_typed(key));
            sel = top = 0;
        }
    }
}

__attribute__((weak)) int clipboard_get(int primary, int *fd, size_t *size);
__attribute__((weak)) int keys_modifiers(void);
__attribute__((weak)) const struct pointer_event *pointer_last(void);

static void paste_into(char *text, size_t size, size_t *pos, int primary)
{
    int fd = -1;
    size_t csize = 0;
    if (!clipboard_get || clipboard_get(primary, &fd, &csize) != 0 || fd < 0) {
        return;
    }
    char buf[512];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        size_t len = strlen(text);
        if (c < 0x20 || c == 0x7f || len + 1 >= size) {
            continue;
        }
        memmove(text + *pos + 1, text + *pos, len - *pos + 1);
        text[(*pos)++] = (char)c;
    }
}

static int edit_line_key(int key, char *text, size_t size, size_t *pos)
{
    size_t len = strlen(text);
    const char *typed;
    if (*pos > len) *pos = len;
    if (key == K_LEFT) {
        if (*pos > 0) {
            (*pos)--;
            while (*pos > 0 && ((unsigned char)text[*pos] & 0xC0) == 0x80) (*pos)--;
        }
        return 1;
    } else if (key == K_RIGHT) {
        if (*pos < len) {
            (*pos)++;
            while (*pos < len && ((unsigned char)text[*pos] & 0xC0) == 0x80) (*pos)++;
        }
        return 1;
    } else if (key == K_BOL || key == 1) {
        *pos = 0;
        return 1;
    } else if (key == K_EOL || key == 5) {
        *pos = len;
        return 1;
    } else if (key == '\b' || key == 8) {
        if (*pos > 0) {
            size_t prev = *pos - 1;
            while (prev > 0 && ((unsigned char)text[prev] & 0xC0) == 0x80) prev--;
            memmove(text + prev, text + *pos, len - *pos + 1);
            *pos = prev;
        }
        return 1;
    } else if (key == K_DEL || key == 4) {
        if (*pos < len) {
            size_t next = *pos + 1;
            while (next < len && ((unsigned char)text[next] & 0xC0) == 0x80) next++;
            memmove(text + *pos, text + next, len - next + 1);
        }
        return 1;
    } else if (key == 21) { /* Ctrl+U */
        memmove(text, text + *pos, len - *pos + 1);
        *pos = 0;
        return 1;
    } else if (key == 11) { /* Ctrl+K */
        text[*pos] = '\0';
        return 1;
    } else if (key == 23) { /* Ctrl+W */
        size_t prev = *pos;
        while (prev > 0 && text[prev - 1] == ' ') prev--;
        while (prev > 0 && text[prev - 1] != ' ') prev--;
        memmove(text + prev, text + *pos, len - *pos + 1);
        *pos = prev;
        return 1;
    } else if (key == 22 || (key == K_INS && keys_modifiers && (keys_modifiers() & KEYS_SHIFT))) {
        paste_into(text, size, pos, 0);
        return 1;
    } else if (key == K_POINTER && pointer_last) {
        const struct pointer_event *e = pointer_last();
        if (e && e->pressed && e->button == BTN_MIDDLE) {
            paste_into(text, size, pos, 1);
            return 1;
        }
    } else if (key >= 0x80 && key <= 0xff && len + 1 < size) {
        memmove(text + *pos + 1, text + *pos, len - *pos + 1);
        text[(*pos)++] = (char)key;
        return 1;
    } else if ((typed = keys_typed(key)) != NULL && len + strlen(typed) < size) {
        size_t n = strlen(typed);
        memmove(text + *pos + n, text + *pos, len - *pos + 1);
        memcpy(text + *pos, typed, n);
        *pos += n;
        return 1;
    }
    return 0;
}

/* A line typed at the bottom, starting as text: 1 with Enter, 0 with Esc;
 * hidden: shown as stars. */
static int prompt_as(const char *label, char *text, size_t size, int hidden)
{
    char line[256], stars[65];
    size_t pos = strlen(text);
    for (;;) {
        size_t len = strlen(text);
        if (pos > len) pos = len;
        size_t shown = len < sizeof stars - 1 ? len : sizeof stars - 1;
        memset(stars, '*', shown);
        stars[shown] = '\0';
        /* What fits of a long line is its end, where it is typed (a bridge
         * line is longer than the screen). */
        const char *typed = hidden ? stars : text;
        int room = screen_cols() - 4 - ui_cells(label), typed_len = (int)strlen(typed);
        int cur = hidden && pos > shown ? (int)shown : (int)pos;
        if (room > (int)sizeof line - 2 - (int)strlen(label)) {
            room = (int)sizeof line - 2 - (int)strlen(label);
        }
        room = room < 0 ? 0 : room;
        int start = cur > room ? cur - room : 0;
        int before = cur - start;
        int after = typed_len - cur > room - before ? room - before : typed_len - cur;
        snprintf(line, sizeof line, "%s%.*s_%.*s", label, before, typed + start,
                 after > 0 ? after : 0, typed + cur);
        screen_fill(0, screen_rows() - 2, screen_cols(), 1, BG);
        screen_text(2, screen_rows() - 2, line, ACCENT, BG);
        screen_flush();
        int key = key_get();
        if (key == 27) {
            return 0;
        } else if (key == '\n') {
            return 1;
        } else {
            edit_line_key(key, text, size, &pos);
        }
    }
}

static int prompt(const char *label, char *text, size_t size)
{
    return prompt_as(label, text, size, 0);
}

/* The date and time, typed (network time off): the system's clock, and the
 * hardware clock (UTC) that the next start reads. */
static void set_clock(void)
{
    char text[32];
    time_t t = time(NULL);
    struct tm tm, want = { 0 };
    if (ntp_on) {
        /* TRANSLATORS: Left/Right are the Left and Right arrow keys; keep them. */
        snprintf(notice, sizeof notice, "%s", _("Network time sets the clock: turn it off (Left/Right) to set it here."));
        return;
    }
    localtime_r(&t, &tm);
    strftime(text, sizeof text, "%Y-%m-%d %H:%M", &tm);
    /* TRANSLATORS: Enter and Esc are key names; keep them and YYYY-MM-DD HH:MM. */
    if (!prompt(_("Date and time (YYYY-MM-DD HH:MM), Enter: set, Esc: leave it: "), text, sizeof text)) {
        snprintf(notice, sizeof notice, "%s", _("Date and time left as they are."));
        return;
    }
    const char *end = strptime(text, "%Y-%m-%d %H:%M", &want);
    if (!end || *end) {
        snprintf(notice, sizeof notice, _("Not a date and time as YYYY-MM-DD HH:MM: %.40s"), text);
        return;
    }
    want.tm_isdst = -1;
    struct timespec ts = { .tv_sec = mktime(&want) };
    if (ts.tv_sec == (time_t)-1 || clock_settime(CLOCK_REALTIME, &ts) < 0) {
        snprintf(notice, sizeof notice, _("Cannot set the clock: %s"), strerror(errno));
        return;
    }
    struct tm utc;
    gmtime_r(&ts.tv_sec, &utc);
    struct rtc_time rtc = {
        .tm_sec = utc.tm_sec, .tm_min = utc.tm_min, .tm_hour = utc.tm_hour,
        .tm_mday = utc.tm_mday, .tm_mon = utc.tm_mon, .tm_year = utc.tm_year,
    };
    int fd = open("/dev/rtc0", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || ioctl(fd, RTC_SET_TIME, &rtc) < 0) {
        snprintf(notice, sizeof notice, _("Clock set to %s; not the hardware clock: %s"), text, strerror(errno));
    } else {
        snprintf(notice, sizeof notice, _("Clock set to %s, the hardware clock too."), text);
    }
    if (fd >= 0) {
        close(fd);
    }
}

/* The debug cable link (service_link): a shell on the reserved wired port for
 * another computer at the other end of the cable, to repair this system when
 * it cannot repair itself. locachy-setup moves the port between it and
 * NetworkManager; process 1 starts or ends the service. */
static void switch_link(void)
{
    int status = -1;
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, STDIN_FILENO);
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        execl(SETUP, SETUP, "debug-link", link_on ? "off" : "on", (char *)NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(notice, sizeof notice, _("locachy-setup debug-link %s failed (status %d)."), link_on ? "off" : "on",
                 pid > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return;
    }
    link_on = !link_on;
    kill(1, SIGHUP);
    snprintf(notice, sizeof notice, link_on ? _("Debug cable link on: %.20s is kept for it.") :
             _("Debug cable link off: %.20s is NetworkManager's again (the network restarts)."),
             link_dev[0] ? link_dev : _("the port"));
}

static int m_is_connected(const struct monitor *m)
{
    return strcmp(m->status, "connected") == 0;
}

struct arrangement_hit {
    int x, y, width, height, monitor, button;
};

static struct arrangement_hit arrangement_hits[MAX_MONITORS * 2 + 5];
static int narrangement_hits, arrangement_selected = -1, arrangement_button;
static int arrangement_pointer_col, arrangement_pointer_row, arrangement_pointer_seen;

static int arrangement_rects(struct monitor_rect *rects, int *indices)
{
    int n = 0, anchor = monitor_anchor();
    for (int i = 0; i < nmonitors; i++) {
        struct monitor *m = &monitors[i];
        if (!m_is_connected(m)) continue;
        int width = 1280, height = 720;
        if (m->nmodes > 0) sscanf(m->modes[m->mode_sel], "%dx%d", &width, &height);
        if (width <= 0 || height <= 0) { width = 1280; height = 720; }
        double scale = m->scale > 0 ? m->scale : 1.0;
        rects[n] = (struct monitor_rect){ .name = m->name,
            .width = (int)(width / scale), .height = (int)(height / scale),
            .place = i == anchor ? MONITOR_ORIGIN : m->place };
        indices[n++] = i;
    }
    monitor_layout(rects, (size_t)n);
    return n;
}

static void arrangement_select(int direction)
{
    int anchor = monitor_anchor(), best = -1;
    const char *name = arrangement_selected >= 0 ? monitors[arrangement_selected].name : "";
    for (int i = 0; i < nmonitors; i++) {
        if (i == anchor || !m_is_connected(&monitors[i])) continue;
        int cmp = strcmp(monitors[i].name, name);
        if ((direction > 0 && cmp > 0) || (direction < 0 && cmp < 0)) {
            if (best < 0 || (direction > 0 ? strcmp(monitors[i].name, monitors[best].name) < 0 :
                                             strcmp(monitors[i].name, monitors[best].name) > 0)) best = i;
        }
    }
    if (best < 0) {
        for (int i = 0; i < nmonitors; i++) {
            if (i == anchor || !m_is_connected(&monitors[i])) continue;
            if (best < 0 || (direction > 0 ? strcmp(monitors[i].name, monitors[best].name) < 0 :
                                             strcmp(monitors[i].name, monitors[best].name) > 0)) best = i;
        }
    }
    arrangement_selected = best;
}

static void arrangement_apply(void)
{
    int i = arrangement_selected;
    if (i < 0 || i >= nmonitors || i == monitor_anchor() || !m_is_connected(&monitors[i])) return;
    int previous = monitors[i].place;
    monitors[i].place = arrangement_button;
    if (!monitor_saved()) monitors[i].place = previous;
}

static void arrangement_hit(int x, int y, int width, int height, int monitor, int button)
{
    if (width <= 0 || height <= 0 || narrangement_hits >= (int)(sizeof arrangement_hits / sizeof arrangement_hits[0])) return;
    arrangement_hits[narrangement_hits++] = (struct arrangement_hit){ x, y, width, height, monitor, button };
}

static void arrangement_draw(void)
{
    static const char *const buttons[] = { N_("[Right]"), N_("[Left]"), N_("[Above]"), N_("[Below]") };
    struct monitor_rect rects[MAX_MONITORS];
    int indices[MAX_MONITORS], n = arrangement_rects(rects, indices);
    int cols = screen_cols(), rows = screen_rows();
    int anchor = monitor_anchor();
    narrangement_hits = 0;
    screen_fill(0, 0, cols, rows, BG);
    screen_bar(_("settings: screens"), NULL);
    screen_text(1, 2, _("Arrange screens"), ACCENT, BG);
    char line[160];
    if (arrangement_selected >= 0) {
        struct monitor *m = &monitors[arrangement_selected];
        snprintf(line, sizeof line, _("Selected: %.31s  %s of %.31s"), m->name,
                 _(place_labels[m->place]), anchor >= 0 ? monitors[anchor].name : _("origin"));
    } else {
        snprintf(line, sizeof line, "%s", n ? _("No external screens connected") : _("No screens connected"));
    }
    screen_text(1, 3, line, FG, BG);
    int button_lines = 1, button_x = 1;
    for (int i = 0; i < 4; i++) {
        int len = ui_cells(_(buttons[i]));
        if (button_x + len >= cols) { button_x = 1; button_lines++; }
        button_x += len + 1;
    }
    int button_row = rows - 4 - button_lines;
    if (button_row < 4) button_row = 4;
    int preview_height = button_row - 6, preview_width = cols - 2;
    if (preview_height < 3 && n > 0 && button_row > 4) {
        const char *name = arrangement_selected >= 0 ? monitors[arrangement_selected].name :
                           anchor >= 0 ? monitors[anchor].name : _("none");
        snprintf(line, sizeof line, _("Screen: %.31s"), name);
        screen_text(1, 4, line, FG, BG);
    }
    if (preview_height >= 3 && preview_width >= 8 && n) {
        int width = 1, height = 1;
        for (int i = 0; i < n; i++) {
            if (rects[i].x + rects[i].width > width) width = rects[i].x + rects[i].width;
            if (rects[i].y + rects[i].height > height) height = rects[i].y + rects[i].height;
        }
        double factor = (double)preview_width / width;
        /* Character cells are about twice as tall as they are wide. */
        if ((double)preview_height * 2 / height < factor) factor = (double)preview_height * 2 / height;
        for (int i = 0; i < n; i++) {
            int x = 1 + (int)(rects[i].x * factor), y = 5 + (int)(rects[i].y * factor / 2);
            int w = (int)(rects[i].width * factor), h = (int)(rects[i].height * factor / 2);
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            if (x + w > cols - 1) w = cols - 1 - x;
            if (y + h > 5 + preview_height) h = 5 + preview_height - y;
            if (w <= 0 || h <= 0) continue;
            uint32_t color = indices[i] == arrangement_selected ? ACCENT : 0x304860;
            screen_fill(x, y, w, h, color);
            snprintf(line, sizeof line, "%s%s", rects[i].name,
                     indices[i] == anchor ? _(" (origin)") : "");
            int cells = 0, pos = 0;
            while (line[pos]) {
                unsigned char b = (unsigned char)line[pos];
                int step = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
                char ch[8] = "";
                for (int k = 0; k < step && line[pos + k]; k++) ch[k] = line[pos + k];
                int cw = ui_cells(ch);
                if (cells + cw > w) { line[pos] = '\0'; break; }
                cells += cw;
                pos += (int)strlen(ch);
            }
            screen_text(x, y, line, indices[i] == arrangement_selected ? BG : FG, color);
            if (h > 1) {
                snprintf(line, sizeof line, "%dx%d", rects[i].width, rects[i].height);
                if (w < (int)sizeof line) line[w] = '\0';
                screen_text(x, y + 1, line, indices[i] == arrangement_selected ? BG : FG, color);
            }
            arrangement_hit(x, y, w, h, indices[i], -1);
        }
    }
    button_x = 1;
    for (int i = 0; i < 4; i++) {
        const char *btn = _(buttons[i]);
        int len = ui_cells(btn);
        if (button_x + len >= cols) { button_x = 1; button_row++; }
        uint32_t color = i == arrangement_button ? ACCENT : 0x304860;
        screen_text(button_x, button_row, btn, i == arrangement_button ? BG : FG, color);
        if (arrangement_selected >= 0) arrangement_hit(button_x, button_row, len, 1, -1, i);
        button_x += len + 1;
    }
    /* TRANSLATORS: Enter and Esc are key names; keep them. */
    screen_text(1, rows - 3, cols < 44 ? _("Enter: apply  Esc: back") :
                _("Arrows: select  Enter: apply  Esc: back"), FG, BG);
    screen_text(1, rows - 2, _(notice), ACCENT, BG);
    screen_flush();
}

/* 1 if it changed what is shown. */
static int arrangement_pointer(const struct pointer_event *event)
{
    arrangement_pointer_col = event->col;
    arrangement_pointer_row = event->row;
    arrangement_pointer_seen = 1;
    if (event->button != BTN_LEFT || !event->pressed) return 0;
    for (int i = 0; i < narrangement_hits; i++) {
        struct arrangement_hit *h = &arrangement_hits[i];
        if (arrangement_pointer_col < h->x || arrangement_pointer_col >= h->x + h->width ||
            arrangement_pointer_row < h->y || arrangement_pointer_row >= h->y + h->height) continue;
        if (h->monitor >= 0 && h->monitor != monitor_anchor()) {
            arrangement_selected = h->monitor;
            arrangement_button = monitors[h->monitor].place;
            if (arrangement_button > MONITOR_BELOW) arrangement_button = MONITOR_RIGHT;
        } else if (h->button >= 0) {
            arrangement_button = h->button;
            arrangement_apply();
        }
        return 1;
    }
    return 0;
}

static int arrangement_key(int key)
{
    if (key == 27 || key == 'q') return 0;
    if (key == K_UP || key == K_DOWN) {
        arrangement_select(key == K_DOWN ? 1 : -1);
        if (arrangement_selected >= 0) {
            arrangement_button = monitors[arrangement_selected].place;
            if (arrangement_button > MONITOR_BELOW) arrangement_button = MONITOR_RIGHT;
        }
    } else if (key == K_LEFT) arrangement_button = (arrangement_button + 3) % 4;
    else if (key == K_RIGHT) arrangement_button = (arrangement_button + 1) % 4;
    else if (key == '\n') arrangement_apply();
    return 1;
}

static void arrange_screens(void)
{
    arrangement_pointer_seen = 0;
    arrangement_selected = -1;
    arrangement_select(1);
    arrangement_button = arrangement_selected >= 0 ? monitors[arrangement_selected].place : MONITOR_RIGHT;
    if (arrangement_button > MONITOR_BELOW) arrangement_button = MONITOR_RIGHT;
    snprintf(notice, sizeof notice, "%s", _("Text programs mirror; this layout is for the shared graphical display."));
    arrangement_draw();
    for (;;) {
        int key = key_poll_timeout(-1, 1000);
        if (key == K_POINTER) {
            if (arrangement_pointer(pointer_last())) arrangement_draw();
            continue;
        }
        if (!arrangement_key(key)) return;
        if (key == 0 || key == K_SHOWN) {
            char name[32] = "";
            if (arrangement_selected >= 0) snprintf(name, sizeof name, "%s", monitors[arrangement_selected].name);
            load_monitors();
            arrangement_selected = -1;
            for (int i = 0; i < nmonitors; i++) {
                if (i != monitor_anchor() && m_is_connected(&monitors[i]) && strcmp(monitors[i].name, name) == 0)
                    arrangement_selected = i;
            }
            if (arrangement_selected < 0) arrangement_select(1);
        }
        arrangement_draw();
    }
}

static int bluetooth_settings_key(int key)
{
    if (key != '\n' || selected != bt_index() + 2) return 0;
    if (!bluetooth_installed()) {
        snprintf(notice, sizeof notice, "%s", _("Bluetooth tools are not installed: the Bluetooth tools line installs them."));
    } else if (!bluetooth_on) {
        snprintf(notice, sizeof notice, "%s", _("Bluetooth service is off: switch it on above before opening devices."));
    } else {
        char *const argv[] = { "/usr/bin/bluetui", NULL };
        int status = in_term(argv);
        snprintf(notice, sizeof notice, "%s", status == 0 ?
                 _("Bluetooth devices: bluetui closed.") :
                 _("Bluetooth devices: bluetui did not finish."));
    }
    return 1;
}

static int screen_settings_key(int key)
{
    if (key != '\n' || selected != num_hw_items()) return 0;
    arrange_screens();
    return 1;
}

struct screen_row_map {
    int mon_idx;
    int field; /* 0=status, 1=mode, 2=scale */
};

static struct screen_row_map row_maps[MAX_MONITORS * 3];
static int nrow_maps;

static void build_row_maps(void)
{
    nrow_maps = 0;
    for (int i = 0; i < nmonitors; i++) {
        struct monitor *m = &monitors[i];
        if (!m_is_connected(m)) {
            row_maps[nrow_maps++] = (struct screen_row_map){ .mon_idx = i, .field = 0 };
        } else {
            row_maps[nrow_maps++] = (struct screen_row_map){ .mon_idx = i, .field = 1 };
            row_maps[nrow_maps++] = (struct screen_row_map){ .mon_idx = i, .field = 2 };
        }
    }
}

/* The main page, as KDE's System Settings has it: the categories in a
 * sidebar, the chosen one's settings on the right in titled groups (KDE's
 * pages of that category). Keys go to the sidebar or to the page (in_page);
 * / finds settings in every category. A setting is still its index
 * (selected), as the functions above count them. */
enum { CAT_INPUT, CAT_DEVICES, CAT_NETWORK, CAT_APPEARANCE, CAT_APPS, CAT_SECURITY, CAT_TIME, CAT_SYSTEM, NCATS };
static const char *const cat_names[NCATS] = {
    N_("Input & Output"), N_("Connected Devices"), N_("Networking"), N_("Appearance"),
    N_("Apps & Windows"), N_("Security & Privacy"), N_("Language & Time"), N_("System"),
};
static int category, in_page, page_scroll;
static char search[64]; /* what / found: the page has every category's settings with it */

#define PAGE_TOP 2
#define MAX_ENTRIES 256
#define E_TITLE (-1) /* a group's title */
#define E_NOTE (-2)  /* a line to read, not a setting */
#define E_GAP (-3)   /* the blank line before a title */
struct entry {
    int idx;
    char text[128];
};
static struct entry entries[MAX_ENTRIES];
static int nentries;

static void item_text(int idx, char *line, size_t size);

static void add_entry(int idx, const char *text)
{
    if (nentries < MAX_ENTRIES) {
        entries[nentries].idx = idx;
        snprintf(entries[nentries].text, sizeof entries[0].text, "%s", text ? text : "");
        nentries++;
    }
}

static void add_title(const char *title)
{
    if (nentries > 0) add_entry(E_GAP, NULL);
    add_entry(E_TITLE, title);
}

static void add_items(int first, int count)
{
    for (int i = 0; i < count; i++) add_entry(first + i, NULL);
}

/* Category cat's groups and their settings, after the entries there are. */
static void category_entries(int cat)
{
    int hw = num_hw_items();
    switch (cat) {
    case CAT_INPUT:
        add_title(_("Mouse & Touchpad"));
        add_items(0, tp_present ? 4 : 1);
        add_items(circ_index(), 1);
        add_title(_("Keyboard"));
        add_items(ime_index(), 2);
        add_items(imek_index(), NIME_KEYS);
        add_title(_("Display & Monitor"));
        add_items(bri_index(), 1);
        add_items(hw, 1 + nrow_maps);
        break;
    case CAT_DEVICES:
        add_title(_("Bluetooth"));
        add_items(bt_index(), 3);
        break;
    case CAT_NETWORK:
        add_title(_("Wi-Fi & Internet"));
        add_items(nw_index(), nnets + 1);
        if (nnets == 0) add_entry(E_NOTE, _("No saved network yet."));
        add_title(_("Tailscale"));
        add_items(ts_index(), 3);
        add_title(_("Clash"));
        add_items(cl_index(), 4);
        add_title(_("Tor"));
        add_items(tr_index(), 4);
        add_title(_("Cloud Drives"));
        add_items(rclone_index(), 3);
        add_title(_("Terminal"));
        add_items(tnet_index(), 1);
        break;
    case CAT_APPEARANCE:
        add_title(_("Wallpaper"));
        add_items(wp_index(), 1);
        break;
    case CAT_APPS:
        add_title(_("Default Applications"));
        add_items(hw + 1 + nrow_maps, nrules);
        /* TRANSLATORS: A is the A key; keep it. */
        if (nrules == 0) add_entry(E_NOTE, _("None yet: A in an open-with chooser makes one."));
        add_title(_("Shortcuts"));
        add_items(tk_index(), 1);
        break;
    case CAT_SECURITY:
        add_title(_("Screen Locking"));
        add_items(sec_index() + 1, 3);
        add_title(_("Disk Encryption"));
        add_items(tpm_index(), 1);
        break;
    case CAT_TIME:
        add_title(_("Region & Language"));
        add_items(lang_index(), 1);
        add_title(_("Date & Time"));
        add_items(tz_index(), 3);
        break;
    default:
        add_title(_("About This System"));
        add_items(host_index(), 1);
        add_items(mach_index(), 1);
        add_title(_("Users"));
        add_items(sec_index(), 1);
        add_items(fp_index(), 4);
        add_items(fc_index(), 7);
        add_title(_("Power Management"));
        add_items(tp_present ? 4 : 1, 1);
        add_title(_("Boot"));
        add_items(boot_index(), 3);
        add_title(_("Software Update"));
        add_items(upd_index(), 2);
        add_title(_("Software Sources"));
        add_items(repo_index(), NREPO_SECTIONS + 1);
        if (repo_level[0]) {
            char note[128];
            snprintf(note, sizeof note, _("Packages for %s; own builds for %s. In order:"), repo_level, repo_cpu);
            add_entry(E_NOTE, note);
            /* The stack, a line of names at a time. */
            for (const char *p = repo_stack; *p;) {
                int n = 0;
                for (const char *w = p; *w;) {
                    size_t len = strcspn(w, " ");
                    if (n > 0 && n + (int)len > 100) {
                        break;
                    }
                    n = (int)(w - p + (ptrdiff_t)len);
                    w += len + strspn(w + len, " ");
                }
                snprintf(note, sizeof note, "  %.*s", n, p);
                add_entry(E_NOTE, note);
                p += n + (int)strspn(p + n, " ");
            }
        }
        add_title(_("Optional Components"));
        add_items(opt_index(), NOPT_MODULES);
        add_title(_("Debug Cable"));
        add_items(link_index(), 1);
        break;
    }
}

/* The page: the category's entries; or, after /, every category's settings
 * whose line or group has what was typed, under "Category: Group". */
static void build_entries(void)
{
    static struct entry all[MAX_ENTRIES];
    nentries = 0;
    if (!search[0]) {
        category_entries(category);
        return;
    }
    for (int c = 0; c < NCATS; c++) {
        int start = nentries, shown = 0;
        char group[128] = "", line[512];
        category_entries(c);
        int n = nentries - start;
        memcpy(all, entries + start, sizeof all[0] * (size_t)n);
        nentries = start;
        for (int k = 0; k < n; k++) {
            if (all[k].idx == E_TITLE) {
                snprintf(group, sizeof group, "%.60s: %.60s", _(cat_names[c]), all[k].text);
                shown = 0;
                continue;
            }
            if (all[k].idx < 0) continue;
            item_text(all[k].idx, line, sizeof line);
            if (!strcasestr(line, search) && !strcasestr(group, search)) continue;
            if (!shown) add_title(group);
            shown = 1;
            add_entry(all[k].idx, NULL);
        }
    }
}

static int entry_of(int idx)
{
    for (int k = 0; k < nentries; k++) {
        if (entries[k].idx == idx) return k;
    }
    return -1;
}

/* The page's first setting, or -1. */
static int first_setting(void)
{
    for (int k = 0; k < nentries; k++) {
        if (entries[k].idx >= 0) return entries[k].idx;
    }
    return -1;
}

/* The category that has setting idx, or -1 (the entries are built anew). */
static int category_of(int idx)
{
    int found = -1;
    for (int c = 0; c < NCATS && found < 0; c++) {
        nentries = 0;
        category_entries(c);
        found = entry_of(idx) >= 0 ? c : -1;
    }
    build_entries();
    return found;
}

/* step settings down the page (up when negative), as far as there are. */
static void page_move(int step)
{
    int k = entry_of(selected), dir = step > 0 ? 1 : -1;
    if (k < 0) {
        selected = first_setting();
        return;
    }
    for (int n = step > 0 ? step : -step; n > 0; n--) {
        int j = k + dir;
        while (j >= 0 && j < nentries && entries[j].idx < 0) j += dir;
        if (j < 0 || j >= nentries) break;
        k = j;
    }
    selected = entries[k].idx;
}

/* Category c's page, its first setting selected; keys to it (focus_page)
 * or to the sidebar. */
static void show_category(int c, int focus_page)
{
    c = c < 0 ? 0 : c >= NCATS ? NCATS - 1 : c;
    if (c != category || search[0]) {
        category = c;
        search[0] = '\0';
        page_scroll = 0;
        build_entries();
        selected = first_setting();
    }
    in_page = focus_page;
}

/* The sidebar's width: the longest category and room; 0 when the screen is
 * too narrow for it and a page (then each has the screen in turn). */
static int sidebar_width(void)
{
    int w = 0;
    for (int c = 0; c < NCATS; c++) {
        int n = ui_cells(_(cat_names[c]));
        w = n > w ? n : w;
    }
    w += 3;
    return screen_cols() >= w + 48 ? w : 0;
}

/* The column where the page's settings start. */
static int text_x(void)
{
    int sw = sidebar_width();
    return sw ? sw + 3 : 2;
}

static int page_rows(void)
{
    int n = screen_rows() - 2 - PAGE_TOP;
    return n > 1 ? n : 1;
}

/* The selected setting shown, with its group's title when it is the first. */
static void page_scroll_to_selected(void)
{
    int k = entry_of(selected), visible = page_rows();
    if (k >= 0) {
        int top = k;
        while (top > 0 && entries[top - 1].idx < 0 && k - top < 2) top--;
        if (top < page_scroll) page_scroll = top;
        if (k >= page_scroll + visible) page_scroll = k - visible + 1;
    }
    if (page_scroll > nentries - visible) page_scroll = nentries - visible;
    if (page_scroll < 0) page_scroll = 0;
}

/* The setting on screen row row, or -1. */
static int item_at(int row)
{
    int k = page_scroll + row - PAGE_TOP;
    if (row < PAGE_TOP || row >= PAGE_TOP + page_rows() || k < 0 || k >= nentries) return -1;
    return entries[k].idx >= 0 ? entries[k].idx : -1;
}

/* The rows where Enter opens a sub-screen or prompt (on the other rows,
 * Enter and Space change the setting in place, like Right). */
static void current_hostname(char *out, size_t size)
{
    char line[128];
    FILE *f = fopen("/etc/hostname", "re");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *h = strchr(line, '#');
            if (h) *h = '\0';
            char *s = line + strspn(line, " \t\r\n");
            size_t len = strcspn(s, " \t\r\n");
            if (len > 0) {
                snprintf(out, size, "%.*s", (int)len, s);
                fclose(f);
                return;
            }
        }
        fclose(f);
    }
    if (gethostname(out, size) < 0 || !out[0]) {
        snprintf(out, size, "locachy");
    }
    out[size - 1] = '\0';
}

static int valid_hostname(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len > 63 || !isalnum((unsigned char)s[0]) || !isalnum((unsigned char)s[len - 1])) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '-') return 0;
    }
    return 1;
}

static int write_atomic_file(const char *path, const char *tmp, const char *content)
{
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    int ok = fputs(content, f) >= 0;
    if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void change_hostname(void)
{
    char text[68] = "", hosts[256], hline[80];
    current_hostname(text, sizeof text);
    if (!prompt(_("Hostname (letters, digits, hyphens): "), text, sizeof text) || !text[0]) {
        snprintf(notice, sizeof notice, "%s", _("Hostname unchanged."));
        return;
    }
    if (!valid_hostname(text)) {
        snprintf(notice, sizeof notice, "%s", _("Invalid hostname: use 1-63 letters, digits or hyphens."));
        return;
    }
    snprintf(hline, sizeof hline, "%s\n", text);
    snprintf(hosts, sizeof hosts,
             "127.0.0.1\tlocalhost\n::1\t\tlocalhost\n127.0.1.1\t%s.localdomain %s\n", text, text);
    if (write_atomic_file("/etc/hostname", "/etc/hostname.tmp", hline) < 0 ||
        write_atomic_file("/etc/hosts", "/etc/hosts.tmp", hosts) < 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s."), "/etc/hostname");
        return;
    }
    sethostname(text, strlen(text));
    kill(1, SIGHUP);
    snprintf(notice, sizeof notice, _("Hostname set to %s."), text);
}

/* As locachy-pkg takes them: a git URL (https://, ssh:// or git@) with no
 * space, quote, backslash, $ or `; a plain branch name, never an option. */
static int valid_update_repo(const char *s)
{
    return (strncmp(s, "https://", 8) == 0 || strncmp(s, "ssh://", 6) == 0 || strncmp(s, "git@", 4) == 0) &&
           strpbrk(s, " \t\"\\$`") == NULL;
}

static int valid_update_branch(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len > 128 || !isalnum((unsigned char)s[0]) || strstr(s, "..") || strstr(s, "//") ||
        s[len - 1] == '/' || s[len - 1] == '.' || (len >= 5 && strcmp(s + len - 5, ".lock") == 0)) {
        return 0;
    }
    return strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._/-") == len;
}

static void change_update_repo(void)
{
    char text[256];
    snprintf(text, sizeof text, "%s", update_repo[0] ? update_repo : DEFAULT_UPDATE_REPO);
    if (!prompt(_("Update repository (git URL, or off): "), text, sizeof text) || !text[0]) {
        snprintf(notice, sizeof notice, "%s", _("Update repository unchanged."));
        return;
    }
    if (strcmp(text, "off") != 0 && !valid_update_repo(text)) {
        snprintf(notice, sizeof notice, "%s", _("Not an https://, ssh:// or git@ URL (or off)."));
        return;
    }
    if (rc_set("update_repo", text) < 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
        return;
    }
    snprintf(update_repo, sizeof update_repo, "%s", text);
    /* TRANSLATORS: packages is the program's name, c the c key; keep them. */
    snprintf(notice, sizeof notice, strcmp(text, "off") == 0 ? _("Source updates off.") :
             _("Source updates from %.120s: c in packages checks it."), text);
}

static void change_update_branch(void)
{
    char text[128];
    snprintf(text, sizeof text, "%s", update_branch[0] ? update_branch : "main");
    if (!prompt(_("Branch to follow: "), text, sizeof text) || !text[0]) {
        snprintf(notice, sizeof notice, "%s", _("Update branch unchanged."));
        return;
    }
    if (!valid_update_branch(text)) {
        snprintf(notice, sizeof notice, "%s", _("Not a branch name: letters, digits, . _ / - (from a letter or digit)."));
        return;
    }
    if (rc_set("update_branch", strcmp(text, "main") == 0 ? NULL : text) < 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
        return;
    }
    snprintf(update_branch, sizeof update_branch, "%s", strcmp(text, "main") == 0 ? "" : text);
    /* TRANSLATORS: packages is the program's name, c the c key; keep them. */
    snprintf(notice, sizeof notice, _("Following branch %.100s: c in packages checks it."), text);
}

/* locachy-boot now: the boot image with the options as they are, for the next
 * start (its output in BOOT_LOG), as pacman's hooks make it after updates. */
static void make_boot_image(void)
{
    int status = -1;
    snprintf(notice, sizeof notice, "%s", _("Making the boot image (locachy-boot)..."));
    screen_fill(0, screen_rows() - 2, screen_cols(), 1, BG); /* the notice line, while it waits */
    screen_text(2, screen_rows() - 2, notice, ACCENT, BG);
    screen_flush();
    pid_t pid = fork();
    if (pid == 0) {
        mkdir("/run/locachy", 0755);
        int out = open(BOOT_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            dup2(out, STDERR_FILENO);
        }
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
        }
        execl(LOCACHY_BOOT, LOCACHY_BOOT, (char *)NULL);
        _exit(127);
    }
    if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        snprintf(notice, sizeof notice, "%s", _("Boot image made: the next start uses it."));
    } else {
        snprintf(notice, sizeof notice, _("Could not make the boot image; see %s."), BOOT_LOG);
    }
}

/* As locachy-boot takes them: letters, digits, spaces and . , : = / + - _ @. */
static int valid_boot_cmdline(const char *s)
{
    return strlen(s) < sizeof boot_cmdline &&
           strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:=/+-_@ ") == strlen(s);
}

static void change_boot_cmdline(void)
{
    char text[sizeof boot_cmdline];
    snprintf(text, sizeof text, "%s", boot_cmdline);
    if (!prompt(_("Kernel options (added to the command line; empty: none): "), text, sizeof text) ||
        strcmp(text, boot_cmdline) == 0) {
        snprintf(notice, sizeof notice, "%s", _("Kernel options unchanged."));
        return;
    }
    if (!valid_boot_cmdline(text)) {
        snprintf(notice, sizeof notice, "%s", _("Kernel options: letters, digits, spaces and . , : = / + - _ @ only."));
        return;
    }
    if (rc_set("boot_cmdline", text[0] ? text : NULL) < 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
        return;
    }
    snprintf(boot_cmdline, sizeof boot_cmdline, "%s", text);
    make_boot_image();
}

static int enter_row(int idx)
{
    return idx == num_hw_items() || idx == cl_index() + 2 || idx == tr_index() + 2 || idx == bt_index() + 2 ||
           idx == wp_index() || idx == tk_index() || idx == host_index() || idx == upd_index() || idx == upd_index() + 1 ||
           idx == boot_index() + 1 || idx == boot_index() + 2 || idx == repo_index() + NREPO_SECTIONS ||
           idx == sec_index() || idx == sec_index() + 2 || idx == fp_index() + 3 || idx == fc_index() + 5 ||
           idx == fc_index() + 6 || idx == rclone_index() + 2 || idx == tpm_index() ||
           idx == tz_index() || idx == tz_index() + 2 || (idx >= imek_index() && idx < imek_index() + NIME_KEYS);
}

/* What a pointer event does, as a key: on the sidebar a click opens that
 * category's page (the wheel goes through them); on the page a click
 * selects a setting, on its value (from column 33 of its line) changes it:
 * by "<" down, else up; a double click is Enter where Enter does
 * something; the wheel moves. -1: redraw only; 0: nothing. */
static int settings_pointer(void)
{
    const struct pointer_event *e = pointer_last();
    int sw = sidebar_width(), on_sidebar = sw ? e->col < sw : !in_page;
    if (e->wheel) {
        if (on_sidebar) {
            show_category(category - e->wheel, 0);
        } else {
            page_move(-e->wheel);
            in_page = 1;
        }
        return -1;
    }
    if (e->button != BTN_LEFT || !e->pressed) return 0;
    if (on_sidebar) {
        int c = e->row - PAGE_TOP;
        if (c < 0 || c >= NCATS) return 0;
        show_category(c, 1);
        return -1;
    }
    int idx = item_at(e->row), col = e->col - text_x() + 2;
    if (idx < 0) return 0;
    selected = idx;
    in_page = 1;
    if (e->clicks == 2 && enter_row(idx)) return '\n';
    if (col >= 33 && !enter_row(idx)) return col < 37 ? K_LEFT : K_RIGHT;
    return -1;
}

/* A key on the sidebar: Up/Down, Home/End go through the categories (the
 * page shows each); Enter, Space, Right or Tab go into its page. */
static void sidebar_key(int key)
{
    if (key == K_UP || key == K_DOWN) {
        show_category(category + (key == K_UP ? -1 : 1), 0);
    } else if (key == K_BOL || key == K_PGUP || key == K_EOL || key == K_PGDN) {
        show_category(key == K_BOL || key == K_PGUP ? 0 : NCATS - 1, 0);
    } else if (key == '\n' || key == ' ' || key == K_RIGHT || key == '\t') {
        in_page = 1;
    }
}

/* / : the settings of every category whose line has what is typed. */
static void find_setting(void)
{
    char text[sizeof search];
    snprintf(text, sizeof text, "%s", search);
    if (!prompt(_("Find a setting: "), text, sizeof text)) return;
    snprintf(search, sizeof search, "%s", text);
    page_scroll = 0;
    build_entries();
    if (!search[0]) {
        build_entries();
        return;
    }
    if (first_setting() < 0) {
        snprintf(notice, sizeof notice, _("No setting has \"%.40s\"."), search);
        search[0] = '\0';
        build_entries();
        return;
    }
    selected = first_setting();
    in_page = 1;
    /* TRANSLATORS: Esc is the Esc key; keep it. */
    snprintf(notice, sizeof notice, _("Settings with \"%.40s\"   Esc: back to the categories"), search);
}

/* Setting idx's line on the page: its label, padded to the column of the
 * values, then its value. */
static void item_text(int idx, char *line, size_t size)
{
    char bar[32], label[96], vbuf[160];
    int hw = num_hw_items(), i;
    line[0] = '\0';
    /* The pointer's rows (first TrackPoint's three, when there is one), then
     * the battery's. */
    if (idx >= 0 && idx < (tp_present ? 5 : 2)) {
        int k = tp_present ? idx : idx + 3;
        if (k == 0) {
            format_bar(bar, sizeof bar, tp_sens, 10, 255);
            snprintf(line, size, "%.95s %s  %3d / 255", ui_pad(label, sizeof label, _("TrackPoint Sensitivity"), 32), bar, tp_sens);
        } else if (k == 1) {
            format_bar(bar, sizeof bar, tp_speed, 10, 255);
            snprintf(line, size, "%.95s %s  %3d / 255", ui_pad(label, sizeof label, _("TrackPoint Speed"), 32), bar, tp_speed);
        } else if (k == 2) {
            snprintf(line, size, "%.95s [%s]", ui_pad(label, sizeof label, _("TrackPoint Press-to-Select"), 32),
                     tp_pts ? _("ON ") : _("OFF"));
        } else if (k == 3) {
            format_bar(bar, sizeof bar, ptr_accel, -10, 10);
            snprintf(line, size, "%.95s %s  %+4.1f", ui_pad(label, sizeof label, _("Pointer Acceleration"), 32), bar, ptr_accel / 10.0);
        } else {
            format_bar(bar, sizeof bar, hib_after, 10, 300);
            snprintf(line, size, "%.95s %s  %3d s", ui_pad(label, sizeof label, _("Hibernate Timeout"), 32), bar, hib_after);
        }
        return;
    }
    if ((i = idx - ts_index()) >= 0 && i < 3) {
        if (i == 0) {
            snprintf(line, size, "%.95s [%s]", ui_pad(label, sizeof label, _("Tailscale installed"), 32),
                     tailscale_installed() ? _("YES") : _("NO "));
        } else if (i == 1) {
            snprintf(line, size, "  %.95s [%s]%s", ui_pad(label, sizeof label, _("Tailscale VPN"), 30),
                     tailscale_enabled ? _("ON ") : _("OFF"),
                     !tailscale_installed() ? _("  (not installed)") :
                     tor_on ? _("  (Tor is on: no way around it, its UDP is dropped)") : "");
        } else {
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _("Tailscale account"), 30),
                     tailscale_status());
        }
        return;
    }
    if ((i = idx - cl_index()) >= 0 && i < 4) {
        char host[128];
        clash_host(host, sizeof host);
        if (i == 0) {
            snprintf(line, size, "%.95s [%s]", ui_pad(label, sizeof label, _("Clash (mihomo) installed"), 32),
                     clash_installed() ? _("YES") : _("NO "));
        } else if (i == 1) {
            snprintf(line, size, "  %.95s < %s >%s", ui_pad(label, sizeof label, _("Clash proxy"), 30),
                     !clash_on ? (tor_on ? _("off: through Tor alone") : _("off: direct")) :
                     clash_proxy ? _("its port only: 127.0.0.1:7890") : _("TUN: every container on the network"),
                     !clash_installed() ? _("  (not installed)") : !clash_on || !tor_on ? "" :
                     tor_via_clash() ? _("  Tor's front: carries Tor only") :
                     _("  over Tor: no UDP (UDP-only proxies fail)"));
        } else if (i == 2) {
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _("Clash subscription"), 30),
                     host[0] ? host : _("not set (Enter)"));
        } else {
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _("Clash status"), 30), clash_status());
        }
        return;
    }
    if ((i = idx - tr_index()) >= 0 && i < 4) {
        if (i == 0 && pkg_job && tor_job()) {
            char progress[112];
            pkg_progress(progress, sizeof progress);
            snprintf(vbuf, sizeof vbuf, strcmp(pkg_action, "install") == 0 ? _("installing: %s") : _("removing: %s"),
                     progress[0] ? progress : "...");
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Tor installed (tor, lyrebird)"), 32), vbuf);
        } else if (i == 0) {
            snprintf(line, size, "%.95s [%s]%s", ui_pad(label, sizeof label, _("Tor installed (tor, lyrebird)"), 32),
                     tor_installed() ? _("YES") : _("NO "),
                     tor_installed() && !lyrebird_installed() ? _("  lyrebird (for bridges) not installed") : "");
        } else if (i == 1) {
            const char *choices[] = { N_("off: direct"), N_("through Clash (mihomo)"), N_("through bridges") };
            snprintf(line, size, "  %.95s < %s >%s", ui_pad(label, sizeof label, _("Tor for the whole system"), 30),
                     _(choices[tor_choice()]), tor_installed() ? "" : _("  (not installed)"));
        } else if (i == 2) {
            char bridges[96];
            if (tor_nbridges) {
                snprintf(bridges, sizeof bridges, "%d (%s)", tor_nbridges, tor_transports);
            } else {
                snprintf(bridges, sizeof bridges, "%s", _("none"));
            }
            /* TRANSLATORS: Enter and Delete are key names; keep them. */
            snprintf(vbuf, sizeof vbuf, tor_nbridges ?
                     _("%s   Enter: add one   Delete: remove all") : _("%s   Enter: add one"), bridges);
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _("Tor bridges"), 30), vbuf);
        } else {
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _("Tor status"), 30), tor_status());
        }
        return;
    }
    if ((i = idx - bt_index()) >= 0 && i < 3) {
        int installed = bluetooth_installed();
        if (i == 0 && pkg_job && strcmp(pkg_name, BLUETOOTH_PACKAGES) == 0) {
            char progress[112];
            pkg_progress(progress, sizeof progress);
            snprintf(vbuf, sizeof vbuf, strcmp(pkg_action, "install") == 0 ? _("installing: %s") : _("removing: %s"),
                     progress[0] ? progress : "...");
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Bluetooth tools installed"), 32), vbuf);
        } else if (i == 0) {
            snprintf(line, size, "%.95s [%s]", ui_pad(label, sizeof label, _("Bluetooth tools installed"), 32),
                     installed ? _("YES") : _("NO "));
        } else if (i == 1) {
            snprintf(line, size, "  %.95s [%s]%s", ui_pad(label, sizeof label, _("Bluetooth service"), 30),
                     bluetooth_on ? _("ON ") : _("OFF"), installed ? "" : _("  (not installed)"));
        } else {
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _("Bluetooth devices"), 30),
                     !installed ? _("not installed") : !bluetooth_on ? _("service off") : _("Enter: open bluetui"));
        }
        return;
    }
    if (idx == nw_index() + nnets) {
        snprintf(line, size, "%.95s < %s >", ui_pad(label, sizeof label, _("MAC on new networks"), 32),
                 _(mac_labels[mac_default]));
        return;
    }
    if ((i = idx - nw_index()) >= 0 && i < nnets) {
        char raw_label[96];
        snprintf(raw_label, sizeof raw_label, _("MAC on %.24s"), nets[i].name);
        snprintf(line, size, "%.95s < %s >", ui_pad(label, sizeof label, raw_label, 32),
                 _(mac_labels[nets[i].mac]));
        return;
    }
    if (idx == wp_index()) {
        const char *wname = strrchr(wallpaper_path, '/');
        snprintf(line, size, "%.95s %.80s", ui_pad(label, sizeof label, _("Wallpaper"), 32),
                 wallpaper_path[0] ? (wname ? wname + 1 : wallpaper_path) : _("none"));
        return;
    }
    if ((i = idx - imek_index()) >= 0 && i < NIME_KEYS) {
        snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _(ime_key_labels[i]), 32),
                 ime_keys[i][0] ? ime_keys[i] : _("none"));
        return;
    }
    if (idx == tk_index()) {
        snprintf(line, size, "%.95s %s%s", ui_pad(label, sizeof label, _("Terminal key (shared display)"), 32),
                 terminal_key[0] ? terminal_key : _("none"), open_host("foot") ? "" : _("  (foot is not installed)"));
        return;
    }
    if ((i = idx - tz_index()) >= 0 && i < 3) {
        char zone[128], now[48];
        time_t t = time(NULL);
        struct tm tm;
        if (i == 0) {
            current_zone(zone, sizeof zone);
            snprintf(vbuf, sizeof vbuf, tz_auto ? _("Automatic: %.60s") : "%.60s", zone);
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Time zone"), 32), vbuf);
        } else if (i == 1) {
            snprintf(line, size, "%.95s [%s]", ui_pad(label, sizeof label, _("Network time"), 32),
                     ntp_on ? _("ON ") : _("OFF"));
        } else {
            tzset(); /* /etc/localtime may have changed */
            localtime_r(&t, &tm);
            strftime(now, sizeof now, "%Y-%m-%d %H:%M %Z", &tm);
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Date and time"), 32), now);
        }
        return;
    }
    if (idx == link_index()) {
        if (link_on) {
            snprintf(vbuf, sizeof vbuf, _("[%s]  on %.20s"), _("ON "), link_dev[0] ? link_dev : "?");
        } else {
            snprintf(vbuf, sizeof vbuf, "[%s]", _("OFF"));
        }
        snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Debug cable link"), 32), vbuf);
        return;
    }
    if ((i = idx - sec_index()) >= 0 && i < 4) {
        char after[32];
        if (lock_after) {
            snprintf(after, sizeof after, P_("%d min", "%d min", lock_after), lock_after);
        } else {
            snprintf(after, sizeof after, "%s", _("never"));
        }
        if (i == 0) {
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Password"), 32), _("[Enter: change]"));
        } else if (i == 1) {
            snprintf(line, size, "%.95s [%s]", ui_pad(label, sizeof label, _("Password at boot"), 32),
                     login_skip ? _("OFF") : _("ON "));
        } else if (i == 2) {
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Lock key"), 32),
                     lock_key[0] ? lock_key : _("none"));
        } else {
            snprintf(line, size, "%.95s < %s >", ui_pad(label, sizeof label, _("Lock when idle"), 32), after);
        }
        return;
    }
    if ((i = idx - fp_index()) >= 0 && i < 4) {
        int installed = fprintd_installed();
        if (i == 0 && pkg_job && strcmp(pkg_name, "fprintd") == 0) {
            char progress[112];
            pkg_progress(progress, sizeof progress);
            snprintf(vbuf, sizeof vbuf, strcmp(pkg_action, "install") == 0 ? _("installing: %s") : _("removing: %s"),
                     progress[0] ? progress : "...");
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Fingerprint reader (fprintd)"), 32), vbuf);
        } else if (i == 0) {
            snprintf(line, size, "%.95s [%s]", ui_pad(label, sizeof label, _("Fingerprint reader (fprintd)"), 32),
                     installed ? _("YES") : _("NO "));
        } else if (i == 1) {
            snprintf(line, size, "  %.95s [%s]%s", ui_pad(label, sizeof label, _("Unlock with a fingerprint"), 30),
                     fingerprint_on ? _("ON ") : _("OFF"), installed ? "" : _("  (not installed)"));
        } else if (i == 2) {
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _("Fingerprints"), 30),
                     installed ? fp_enrolled : "-");
        } else if (installed) {
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(vbuf, sizeof vbuf, _("< %s >  Enter: add"), _(finger_labels[fp_next]));
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _("Add a fingerprint"), 30), vbuf);
        } else {
            snprintf(line, size, "  %.95s", ui_pad(label, sizeof label, _("Add a fingerprint"), 30));
        }
        return;
    }
    if ((i = idx - fc_index()) >= 0 && i < 7) {
        int installed = howdy_installed();
        const char *labels[] = { N_("Face recognition (howdy)"), N_("Faces computed on"), N_("Unlock with your face"),
                                 N_("Faces"), N_("Camera for faces"), N_("Add a face"), N_("Infrared emitter") };
        if (i == 0 && pkg_job && face_job()) {
            char progress[112];
            pkg_progress(progress, sizeof progress);
            snprintf(vbuf, sizeof vbuf, strcmp(pkg_action, "install") == 0 ? _("installing: %s") :
                     strcmp(pkg_action, "swap") == 0 ? _("switching: %s") : _("removing: %s"),
                     progress[0] ? progress : "...");
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _(labels[0]), 32), vbuf);
        } else if (i == 0) {
            snprintf(line, size, "%.95s [%s]", ui_pad(label, sizeof label, _(labels[0]), 32),
                     installed ? _("YES") : _("NO "));
        } else if (i == 1) {
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _(labels[1]), 30),
                     !cuda_usable() ? _("the CPU (no NVIDIA driver here)") :
                     face_cuda() ? _("< the NVIDIA GPU (CUDA) >") : _("< the CPU >"));
        } else if (!installed) {
            if (i == 2) {
                snprintf(vbuf, sizeof vbuf, "[%s]%s", face_on ? _("ON ") : _("OFF"), _("  (not installed)"));
            } else {
                snprintf(vbuf, sizeof vbuf, "-");
            }
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _(labels[i]), 30), vbuf);
        } else if (i == 2) {
            snprintf(line, size, "  %.95s [%s]", ui_pad(label, sizeof label, _(labels[2]), 30),
                     face_on ? _("ON ") : _("OFF"));
        } else if (i == 3) {
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _(labels[3]), 30), face_models);
        } else if (i == 4) {
            char cam[96];
            face_camera_label(cam, sizeof cam);
            snprintf(line, size, "  %.95s < %.80s >", ui_pad(label, sizeof label, _(labels[4]), 30), cam);
        } else if (i == 5) {
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _(labels[5]), 30),
                     _("Enter: in a terminal (howdy add)"));
        } else {
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _(labels[6]), 30),
                     access(IR_EMITTER, X_OK) == 0 ?
                     _("Enter: set it up (linux-enable-ir-emitter)") : _("linux-enable-ir-emitter not installed"));
        }
        return;
    }
    if ((i = idx - rclone_index()) >= 0 && i < 3) {
        int installed = rclone_installed();
        if (i == 0 && pkg_job && strcmp(pkg_name, RCLONE_PACKAGES) == 0) {
            char progress[112];
            pkg_progress(progress, sizeof progress);
            snprintf(vbuf, sizeof vbuf, strcmp(pkg_action, "install") == 0 ? _("installing: %s") : _("removing: %s"),
                     progress[0] ? progress : "...");
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Cloud drives installed (rclone)"), 32), vbuf);
        } else if (i == 0) {
            snprintf(line, size, "%.95s [%s]", ui_pad(label, sizeof label, _("Cloud drives installed (rclone)"), 32),
                     installed ? _("YES") : _("NO "));
        } else if (i == 1) {
            snprintf(line, size, "  %.95s [%s]%s", ui_pad(label, sizeof label, _("Cloud drives (in ~/Drives)"), 30),
                     rclone_on ? _("ON ") : _("OFF"), installed ? "" : _("  (not installed)"));
        } else if (installed) {
            char rem[128];
            rclone_remotes_summary(rem, sizeof rem);
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _("Cloud remotes (iCloud, Drive)"), 30), rem);
        } else {
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _("Cloud remotes (iCloud, Drive)"), 30),
                     _("not installed"));
        }
        return;
    }
    if (idx == lang_index()) {
        if (!nlanguages) find_languages();
        snprintf(line, size, "%.120s < %.60s >", ui_pad(label, sizeof label, _("Language"), 32),
                 language_name(languages[current_language()]));
        return;
    }
    if (idx == host_index()) {
        char hname[68];
        current_hostname(hname, sizeof hname);
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(line, size, "%.95s %.63s   %s", ui_pad(label, sizeof label, _("Hostname"), 32),
                 hname, _("(Enter: change)"));
        return;
    }
    if ((i = idx - ime_index()) >= 0 && i < 2) {
        int base_on = rime_installed(), zh_on = rime_zh_installed();
        if (i == 0 && pkg_job && (strcmp(pkg_name, IME_BASE_PACKAGES) == 0 || strcmp(pkg_name, IME_ALL_PACKAGES) == 0)) {
            char progress[112];
            pkg_progress(progress, sizeof progress);
            snprintf(vbuf, sizeof vbuf, strcmp(pkg_action, "install") == 0 ? _("installing: %s") : _("removing: %s"),
                     progress[0] ? progress : "...");
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Input method (Rime, English)"), 32), vbuf);
        } else if (i == 0) {
            snprintf(line, size, "%.95s [%s]", ui_pad(label, sizeof label, _("Input method (Rime, English)"), 32),
                     base_on ? _("YES") : _("NO "));
        } else if (pkg_job && strcmp(pkg_name, IME_ZH_PACKAGES) == 0) {
            char progress[112];
            pkg_progress(progress, sizeof progress);
            snprintf(vbuf, sizeof vbuf, strcmp(pkg_action, "install") == 0 ? _("installing: %s") : _("removing: %s"),
                     progress[0] ? progress : "...");
            snprintf(line, size, "  %.95s %s", ui_pad(label, sizeof label, _("Chinese Pinyin (rime-frost)"), 30), vbuf);
        } else {
            snprintf(line, size, "  %.95s [%s]%s", ui_pad(label, sizeof label, _("Chinese Pinyin (rime-frost)"), 30),
                     zh_on ? _("YES") : _("NO "), base_on || zh_on ? "" : _("  (installs Rime too)"));
        }
        return;
    }
    if (idx == tpm_index()) {
        snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("TPM disk auto-unlock"), 32), tpm_summary);
        return;
    }
    if (idx == bri_index()) {
        int bcur = 0, bmax = 0;
        if (base_brightness_get && base_brightness_get(&bcur, &bmax) == 0 && bmax > 0) {
            int pct = (bcur * 100 + bmax / 2) / bmax;
            snprintf(vbuf, sizeof vbuf, "< %d%% >", pct);
        } else {
            snprintf(vbuf, sizeof vbuf, "%s", _("not supported"));
        }
        snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Screen brightness"), 32), vbuf);
        return;
    }
    if (idx == mach_index()) {
        if (!nmachines) find_machines();
        int mi = current_machine_index();
        const char *mval = mi > 0 ? machine_profiles[mi] : (active_machine[0] ? active_machine : _("generic PC"));
        snprintf(line, size, "%.95s < %.60s >", ui_pad(label, sizeof label, _("Machine profile"), 32), mval);
        return;
    }
    if ((i = idx - upd_index()) >= 0 && i < 2) {
        int off = strcmp(update_repo, "off") == 0;
        if (i == 0) {
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(line, size, "%.95s %.120s   %s", ui_pad(label, sizeof label, _("Update repository"), 32),
                     off ? _("off") : update_repo[0] ? update_repo : DEFAULT_UPDATE_REPO, _("(Enter: change)"));
        } else {
            snprintf(line, size, "  %.95s %.100s   %s", ui_pad(label, sizeof label, _("Update branch"), 30),
                     update_branch[0] ? update_branch : "main", off ? _("(updates off)") : _("(Enter: change)"));
        }
        return;
    }
    if ((i = idx - boot_index()) >= 0 && i < 3) {
        if (i == 0) {
            snprintf(line, size, "%.95s < %s >", ui_pad(label, sizeof label, _("Boot messages"), 32),
                     boot_verbose ? _("shown") : _("hidden (quiet)"));
        } else if (i == 1) {
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(line, size, "%.95s %.120s   %s", ui_pad(label, sizeof label, _("Kernel options"), 32),
                     boot_cmdline[0] ? boot_cmdline : _("none"), _("(Enter: change)"));
        } else {
            struct stat st;
            char made[48] = "";
            if (stat(BOOT_IMAGE, &st) == 0) {
                strftime(made, sizeof made, "%Y-%m-%d %H:%M", localtime(&st.st_mtime));
            }
            snprintf(vbuf, sizeof vbuf, made[0] ? _("made %s") : "%s", made[0] ? made : _("not made yet"));
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(line, size, "%.95s %s   %s", ui_pad(label, sizeof label, _("Boot image"), 32), vbuf,
                     _("(Enter: make it again)"));
        }
        return;
    }
    if (idx == tnet_index()) {
        snprintf(line, size, "%.95s < %s >", ui_pad(label, sizeof label, _("Network in the terminal"), 32),
                 term_net ? _("on") : _("off"));
        return;
    }
    if (idx == circ_index()) {
        snprintf(line, size, "%.95s < %s >", ui_pad(label, sizeof label, _("Touchpad Circular Scrolling"), 32),
                 circular > 0 ? _("on") : circular == 0 ? _("off") : _("round touchpads"));
        return;
    }
    if ((i = idx - repo_index()) >= 0 && i <= NREPO_SECTIONS) {
        char progress[112] = "";
        if (repos_job) {
            log_last_line(REPOS_LOG, vbuf, sizeof vbuf);
            snprintf(progress, sizeof progress, "%.100s", vbuf[0] ? vbuf : "...");
        }
        if (i < NREPO_SECTIONS) {
            size_t n = strlen(repo_sections[i]);
            if (repos_job && strncmp(repos_what, repo_sections[i], n) == 0 && repos_what[n] == '=') {
                snprintf(vbuf, sizeof vbuf, _("checking: %s"), progress);
            } else {
                snprintf(vbuf, sizeof vbuf, "< %s >", repo_on[i] ? _("on") : _("off"));
            }
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _(repo_labels[i]), 32), vbuf);
        } else if (repos_job && strcmp(repos_what, "rank") == 0) {
            snprintf(vbuf, sizeof vbuf, _("ranking: %s"), progress);
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _("Mirrors"), 32), vbuf);
        } else {
            snprintf(vbuf, sizeof vbuf, repo_ranked[0] ? _("ranked %s") : "%s",
                     repo_ranked[0] ? repo_ranked : _("not ranked here yet"));
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(line, size, "%.95s %s   %s", ui_pad(label, sizeof label, _("Mirrors"), 32), vbuf,
                     _("(Enter: rank them again)"));
        }
        return;
    }
    if ((i = idx - opt_index()) >= 0 && i < NOPT_MODULES) {
        if (pkg_job && strcmp(pkg_name, opt_modules[i].packages) == 0) {
            char progress[112];
            pkg_progress(progress, sizeof progress);
            snprintf(vbuf, sizeof vbuf, strcmp(pkg_action, "install") == 0 ?
                     _("installing: %s") : _("removing: %s"), progress[0] ? progress : "...");
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _(opt_modules[i].label), 32), vbuf);
        } else {
            snprintf(line, size, "%.95s %s", ui_pad(label, sizeof label, _(opt_modules[i].label), 32),
                     opt_module_installed(i) ? _("installed") : _("not installed"));
        }
        return;
    }
    if (idx == hw) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(line, size, "%s", _("Arrange screens...  [Enter]"));
        return;
    }
    if ((i = idx - hw - 1) >= 0 && i < nrow_maps) {
        struct monitor *m = &monitors[row_maps[i].mon_idx];
        if (!m_is_connected(m)) {
            snprintf(line, size, _("Screen %-12.12s (disconnected)"), m->name);
        } else if (row_maps[i].field == 1) {
            const char *mname = m->nmodes > 0 ? m->modes[m->mode_sel] : _("preferred");
            char mpadded[64];
            snprintf(vbuf, sizeof vbuf, m->is_internal ? _("Laptop %-10.10s Resolution:") :
                     _("Screen %-10.10s Resolution:"), m->name);
            snprintf(line, size, "  %.95s < %s >", ui_pad(label, sizeof label, vbuf, 30),
                     ui_pad(mpadded, sizeof mpadded, mname, 17));
        } else if (row_maps[i].field == 2) {
            snprintf(vbuf, sizeof vbuf, m->is_internal ? _("Laptop %-10.10s Scale:") :
                     _("Screen %-10.10s Scale:"), m->name);
            snprintf(line, size, _("  %.95s < %3d%% >  (text and windows)"),
                     ui_pad(label, sizeof label, vbuf, 30), (int)(m->scale * 100 + 0.5));
        }
        return;
    }
    if ((i = idx - hw - 1 - nrow_maps) >= 0 && i < nrules) {
        char rlabel[160];
        rule_label(i, rlabel, sizeof rlabel);
        snprintf(line, size, "%-32.32s < %.60s >", rule_types[i], rlabel);
    }
}

#define SIDE_BG 0x304860 /* the category or setting where the keys are not */

static void draw_sidebar(int width)
{
    for (int c = 0; c < NCATS && PAGE_TOP + c < screen_rows() - 2; c++) {
        int on = c == category && !search[0];
        uint32_t bg = on ? (in_page ? SIDE_BG : ACCENT) : BG;
        screen_fill(0, PAGE_TOP + c, width, 1, bg);
        screen_text(1, PAGE_TOP + c, _(cat_names[c]), on && !in_page ? BG : FG, bg);
    }
}

/* The page from column px: its groups' titles, their settings, notes. */
static void draw_page(int px)
{
    char line[512];
    int x = px + 2;
    page_scroll_to_selected();
    for (int r = 0; r < page_rows() && page_scroll + r < nentries; r++) {
        const struct entry *e = &entries[page_scroll + r];
        int row = PAGE_TOP + r;
        if (e->idx == E_TITLE) {
            screen_text(px + 1, row, e->text, ACCENT, BG);
        } else if (e->idx == E_NOTE) {
            screen_text(x + 2, row, e->text, DIM, BG);
        } else if (e->idx >= 0) {
            int on = e->idx == selected;
            uint32_t bg = on ? (in_page ? ACCENT : SIDE_BG) : BG;
            item_text(e->idx, line, sizeof line);
            screen_fill(px, row, screen_cols() - px, 1, bg);
            screen_text(x, row, line, on && in_page ? BG : FG, bg);
        }
    }
}

static void draw(void)
{
    int sw = sidebar_width(), cols = screen_cols(), rows = screen_rows();
    build_row_maps();
    build_entries();
    if (!search[0] && selected >= 0 && entry_of(selected) < 0) {
        int c = category_of(selected); /* chosen elsewhere: the page of its category */
        if (c >= 0) {
            category = c;
            build_entries();
        }
    }
    if (entry_of(selected) < 0) selected = first_setting();
    screen_fill(0, 0, cols, rows, BG);
    screen_bar(_("settings"), NULL);
    if (sw) {
        draw_sidebar(sw);
        screen_fill(sw, PAGE_TOP, 1, rows - 2 - PAGE_TOP, SIDE_BG);
        draw_page(sw + 1);
    } else if (in_page) {
        draw_page(0);
    } else {
        draw_sidebar(cols);
    }
    screen_text(2, rows - 2, _(notice), ACCENT, BG);
    screen_flush();
}

static void adjust(int dir)
{
    int hw = num_hw_items();
    if (tp_present) {
        if (selected == 0) {
            tp_sens += dir * 10;
            if (tp_sens < 10) tp_sens = 10;
            if (tp_sens > 255) tp_sens = 255;
        } else if (selected == 1) {
            tp_speed += dir * 10;
            if (tp_speed < 10) tp_speed = 10;
            if (tp_speed > 255) tp_speed = 255;
        } else if (selected == 2) {
            tp_pts = !tp_pts;
        } else if (selected == 3) {
            ptr_accel += dir;
            if (ptr_accel < -10) ptr_accel = -10;
            if (ptr_accel > 10) ptr_accel = 10;
        } else if (selected == 4) {
            hib_after += dir * 10;
            if (hib_after < 10) hib_after = 10;
            if (hib_after > 600) hib_after = 600;
        }
    } else {
        if (selected == 0) {
            ptr_accel += dir;
            if (ptr_accel < -10) ptr_accel = -10;
            if (ptr_accel > 10) ptr_accel = 10;
        } else if (selected == 1) {
            hib_after += dir * 10;
            if (hib_after < 10) hib_after = 10;
            if (hib_after > 600) hib_after = 600;
        }
    }

    int ts_idx = ts_index();
    if ((selected == ts_idx || selected == ts_idx + 1 || selected == cl_index() || selected == cl_index() + 1 ||
         selected == tr_index() || selected == tr_index() + 1 || selected == bt_index() ||
         selected == bt_index() + 1) && pkg_job) {
        snprintf(notice, sizeof notice, strcmp(pkg_action, "install") == 0 ?
                 _("Wait: %s is still being installed.") : _("Wait: %s is still being removed."), pkg_title());
        return;
    }
    if (selected == ts_idx) {
        if (tailscale_installed()) {
            if (tailscale_enabled) {
                tailscale_enabled = 0;
                save_rc(); /* off before it goes */
            }
            start_pkg_job("remove", "tailscale");
        } else {
            start_pkg_job("install", "tailscale");
        }
        snprintf(notice, sizeof notice, "%s", pkg_job ? (strcmp(pkg_action, "install") == 0 ?
                 _("Tailscale: installing it; switch it on below when it is in.") :
                 _("Tailscale: switched off; removing it.")) :
                 _("Tailscale: cannot run locachy-pkg."));
        return;
    }
    if (selected == ts_idx + 1) {
        if (!tailscale_installed()) {
            snprintf(notice, sizeof notice, "%s", _("Tailscale is not installed: the line above installs it."));
            return;
        }
        tailscale_enabled = !tailscale_enabled;
        save_rc(); /* process 1 starts or ends the service */
        snprintf(notice, sizeof notice, "%s", tailscale_enabled ? _("Tailscale on.") : _("Tailscale off."));
        return;
    }
    if (selected == ts_idx + 2) {
        const char *state = tailscale_status();
        if (dir > 0 && (strcmp(state, _("logged out  (Right: log in with a QR code)")) == 0 ||
                        strcmp(state, _("stopped  (Right: connect)")) == 0)) {
            tailscale_login();
        } else {
            snprintf(notice, sizeof notice, _("Tailscale: %.140s."), state);
        }
        return;
    }

    if (selected == cl_index()) {
        if (clash_installed() && tor_via_clash()) {
            snprintf(notice, sizeof notice, "%s", _("Clash is Tor's front: put Tor through bridges or off first."));
            return;
        }
        if (clash_installed()) {
            if (clash_on) {
                if (rc_set("service_clash", NULL) < 0) {
                    snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
                    return;
                }
                clash_on = 0;
                kill(1, SIGHUP); /* off before it goes */
            }
            start_pkg_job("remove", "mihomo");
        } else {
            start_pkg_job("install", "mihomo");
        }
        snprintf(notice, sizeof notice, "%s", pkg_job ? (strcmp(pkg_action, "install") == 0 ?
                 _("Clash: installing it; choose its mode below when it is in.") :
                 _("Clash: switched off; removing it.")) :
                 _("Clash: cannot run locachy-pkg."));
        return;
    }
    /* Clash: off, TUN, its port only, in turn. Process 1 starts or ends the
     * service; clash-client starts again in the other mode. */
    if (selected == cl_index() + 1) {
        if (!clash_installed()) {
            snprintf(notice, sizeof notice, "%s", _("Clash is not installed: the line above installs it."));
            return;
        }
        int now = !clash_on ? 0 : clash_proxy ? 2 : 1, next = (now + (dir > 0 ? 1 : 2)) % 3;
        if (next == 0 && tor_via_clash()) {
            snprintf(notice, sizeof notice, "%s", _("Clash is Tor's front: put Tor through bridges or off first."));
            return;
        }
        if (rc_set("clash_mode", next == 2 ? "proxy" : "tun") < 0 ||
            rc_set("service_clash", next ? CLASH_SERVICE : NULL) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        clash_on = next != 0;
        clash_proxy = next == 2;
        kill(1, SIGHUP);
        snprintf(notice, sizeof notice, "%s", next == 0 ? (tor_on ? _("Clash off: the network goes through Tor alone.") :
                 _("Clash off: the network goes direct.")) :
                 next == 1 ? _("Clash TUN: every container on the network goes through it.") :
                 _("Clash, its port only: containers started from now on are told of it (start running ones again)."));
        return;
    }
    if (selected == cl_index() + 2 || selected == cl_index() + 3) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(notice, sizeof notice, "%s", selected == cl_index() + 2 ? _("Enter: type the subscription's URL") :
                 _("The panel: open its address in Edge (on the network), with the secret shown."));
        return;
    }
    /* Tor: installed (tor and lyrebird, or what of them is missing), then
     * its choice in turn among those whose front is there. */
    if (selected == tr_index()) {
        if (tor_installed() && tor_on) {
            snprintf(notice, sizeof notice, "%s", _("Tor is on: switch it off on the line below before removing it."));
            return;
        }
        if (tor_installed() && !lyrebird_installed() && dir > 0) {
            start_pkg_job("install", "lyrebird-proxy");
        } else if (tor_installed()) {
            start_pkg_job("remove", lyrebird_installed() ? TOR_PACKAGES : "tor");
        } else {
            start_pkg_job("install", TOR_PACKAGES);
        }
        snprintf(notice, sizeof notice, "%s", !pkg_job ? _("Tor: cannot run locachy-pkg.") :
                 strcmp(pkg_action, "install") == 0 ? _("Tor: installing it (lyrebird is built from the AUR: a while); "
                                                        "choose its way out below when it is in.") :
                 _("Tor: removing it."));
        return;
    }
    if (selected == tr_index() + 1) {
        if (!tor_installed()) {
            snprintf(notice, sizeof notice, "%s", _("Tor is not installed: the line above installs it."));
            return;
        }
        int now = tor_choice(), next = now;
        do {
            next = (next + (dir > 0 ? 1 : 2)) % 3;
        } while (next != now && !tor_front_ready(next));
        if (next == now) { /* from off, no front there; off is always there */
            snprintf(notice, sizeof notice, "%s", _("Tor never goes out directly: switch Clash on, or add a bridge line "
                                                    "(obfs4 or WebTunnel) with lyrebird installed."));
            return;
        }
        tor_set(next);
        return;
    }
    if (selected == tr_index() + 2 || selected == tr_index() + 3) {
        /* TRANSLATORS: Enter and Delete are key names; keep them. */
        snprintf(notice, sizeof notice, "%s", selected == tr_index() + 2 ?
                 _("Enter: add a bridge line (bridges.torproject.org)   Delete: remove them all") :
                 _("Tor's state: tor-client --status"));
        return;
    }
    if (selected == bt_index()) {
        if (bluetooth_installed()) {
            if (rc_set("service_bluetooth", NULL) < 0) {
                snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
                return;
            }
            bluetooth_on = 0;
            kill(1, SIGHUP); /* off before the packages go, even if already off here */
            start_pkg_job("remove", BLUETOOTH_PACKAGES);
        } else {
            start_pkg_job("install", BLUETOOTH_PACKAGES);
        }
        snprintf(notice, sizeof notice, "%s", pkg_job ? (strcmp(pkg_action, "install") == 0 ?
                 _("Bluetooth tools: installing them; switch the service on below when they are in.") :
                 _("Bluetooth tools: service off; removing them.")) :
                 _("Bluetooth tools: cannot run locachy-pkg."));
        return;
    }
    if (selected == bt_index() + 1) {
        if (!bluetooth_installed() && !bluetooth_on) {
            snprintf(notice, sizeof notice, "%s", _("Bluetooth tools are not installed: the line above installs them."));
            return;
        }
        if (rc_set("service_bluetooth", bluetooth_on ? NULL : BLUETOOTH_SERVICE) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        bluetooth_on = !bluetooth_on;
        kill(1, SIGHUP);
        snprintf(notice, sizeof notice, "%s", bluetooth_on ? _("Bluetooth service on.") : _("Bluetooth service off."));
        return;
    }
    if (selected == bt_index() + 2) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(notice, sizeof notice, "%s", _("Enter: open Bluetooth devices in bluetui (the service must be on)."));
        return;
    }
    if (selected >= nw_index() && selected < nw_index() + nnets) {
        struct net *n = &nets[selected - nw_index()];
        set_net_mac(n, (n->mac + (dir < 0 ? NMAC_MODES - 1 : 1)) % NMAC_MODES);
        return;
    }
    if (selected == nw_index() + nnets) {
        set_mac_default((mac_default + (dir < 0 ? NMAC_MODES - 1 : 1)) % NMAC_MODES);
        return;
    }
    if (selected == wp_index()) {
        /* TRANSLATORS: Enter and Delete are key names; keep them. */
        snprintf(notice, sizeof notice, "%s", _("Enter: choose a picture in the file manager   Delete: no wallpaper"));
        return;
    }
    if (selected == tk_index()) {
        /* TRANSLATORS: Enter and Delete are key names; keep them. */
        snprintf(notice, sizeof notice, "%s", _("Enter: press a new key (a window of foot, the terminal)   Delete: none"));
        return;
    }
    if (selected == tz_index() + 1) {
        if (rc_set("service_ntp", ntp_on ? NULL : NTP_SERVICE) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        ntp_on = !ntp_on;
        kill(1, SIGHUP); /* process 1 starts or ends ntp-client */
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(notice, sizeof notice, "%s", ntp_on ? _("Network time on: the clock is set from the network.") :
                 _("Network time off: Enter on Date and time sets the clock."));
        return;
    }
    if (selected == link_index()) {
        switch_link();
        return;
    }
    if (selected == sec_index()) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(notice, sizeof notice, "%s", _("Enter: a new password for the user (it unlocks the screen, and sudo)."));
        return;
    }
    if (selected == sec_index() + 1) {
        /* With the TPM opening the disk at power-on, the lock at boot is all
         * there is between turning it on and the disk. */
        if (!login_skip && tpm_bound) {
            snprintf(notice, sizeof notice, "%s",
                     _("Not changed: the TPM opens the disk at power-on, so the lock at boot must stay. Unbind the TPM first."));
            return;
        }
        if (rc_set("login_skip", login_skip ? NULL : "1") < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        login_skip = !login_skip;
        kill(1, SIGHUP);
        snprintf(notice, sizeof notice, "%s", login_skip ? _("At boot: home at once (the lock key and the lid still lock).") :
                 _("At boot: the lock screen first, the password to go on."));
        return;
    }
    if (selected == sec_index() + 2) {
        /* TRANSLATORS: Enter and Delete are key names; keep them. */
        snprintf(notice, sizeof notice, "%s", _("Enter: press a new lock key   Delete: none (the lid still locks)"));
        return;
    }
    if ((selected == fc_index() || selected == fc_index() + 1 || selected == fc_index() + 2) && pkg_job) {
        snprintf(notice, sizeof notice, strcmp(pkg_action, "install") == 0 ?
                 _("Wait: %s is still being installed.") :
                 strcmp(pkg_action, "swap") == 0 ? _("Wait: %s is still being switched.") :
                 _("Wait: %s is still being removed."), pkg_title());
        return;
    }
    if (selected == fc_index()) {
        char cam[32];
        if (howdy_installed()) {
            if (face_on) {
                if (rc_set("unlock_face", NULL) < 0) {
                    snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
                    return;
                }
                face_on = 0; /* off before it goes */
                sudo_ways();
            }
            start_pkg_job("remove", "python-dlib python-dlib-cuda " FACE_PACKAGES);
        } else if (!camera(cam, sizeof cam)) {
            snprintf(notice, sizeof notice, "%s", _("No camera: face recognition needs one (an infrared one, as for "
                                                    "Windows Hello)."));
            return;
        } else {
            char packages[96];
            snprintf(packages, sizeof packages, "%s %s", dlib_package(face_cuda()), FACE_PACKAGES);
            start_pkg_job("install", packages);
        }
        snprintf(notice, sizeof notice, "%s", pkg_job ? (strcmp(pkg_action, "install") == 0 ?
                 _("Face recognition: building dlib, howdy and linux-enable-ir-emitter (it takes long); then add a face.") :
                 _("Face recognition: switched off; removing them.")) :
                 _("Face recognition: cannot run locachy-pkg."));
        return;
    }
    if (selected == fc_index() + 1) {
        if (!cuda_usable()) {
            snprintf(notice, sizeof notice, "%s", _("CUDA needs an NVIDIA GPU with NVIDIA's driver loaded (nvidia or "
                                                    "nvidia-open); here faces are computed on the CPU."));
            return;
        }
        int cuda = !face_cuda();
        if (rc_set("face_dlib", cuda ? "cuda" : "cpu") < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        snprintf(face_dlib, sizeof face_dlib, "%s", cuda ? "cuda" : "cpu");
        if (dlib_installed() && dlib_installed() != cuda + 1) {
            char swap[64];
            snprintf(swap, sizeof swap, "%s %s", dlib_package(!cuda), dlib_package(cuda));
            start_pkg_job("swap", swap);
            snprintf(notice, sizeof notice, cuda ?
                     _("Faces on the NVIDIA GPU: building %s (with cuda and cudnn: about 2.9 GiB to download).") :
                     _("Faces on the CPU: building %s."), dlib_package(cuda));
        } else {
            snprintf(notice, sizeof notice, "%s", cuda ?
                     _("Faces will be computed on the NVIDIA GPU (cuda and cudnn: about 2.9 GiB to download when installed).") :
                     _("Faces will be computed on the CPU."));
        }
        return;
    }
    if (selected == fc_index() + 2) {
        if (!howdy_installed()) {
            snprintf(notice, sizeof notice, "%s", _("howdy is not installed: the Face recognition line installs it."));
            return;
        }
        if (rc_set("unlock_face", face_on ? NULL : "1") < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        face_on = !face_on;
        if (sudo_ways() < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), SUDO_PAM);
            return;
        }
        snprintf(notice, sizeof notice, "%s", face_on ? _("The lock screen and sudo take your face as well as the "
                                                          "password.") : _("Your face no longer unlocks."));
        return;
    }
    if (selected == fc_index() + 3) {
        /* TRANSLATORS: Delete is the Delete key; keep it. */
        snprintf(notice, sizeof notice, "%s", _("Delete: forget these faces (asks once more)."));
        return;
    }
    if (selected == fc_index() + 4) {
        if (!howdy_installed()) {
            snprintf(notice, sizeof notice, "%s", _("howdy is not installed."));
        } else if (next_face_camera(dir) < 0) {
            snprintf(notice, sizeof notice, "%s", _("No camera (/dev/video*), or howdy could not set it."));
        } else {
            char cam[96];
            face_camera_label(cam, sizeof cam);
            snprintf(notice, sizeof notice, _("howdy uses %.100s. Face recognition wants the infrared one."), cam);
        }
        return;
    }
    if (selected == fc_index() + 5 || selected == fc_index() + 6) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(notice, sizeof notice, "%s", selected == fc_index() + 5 ?
                 _("Enter: record your face (howdy add).") :
                 _("Enter: find and switch on the camera's infrared emitter (linux-enable-ir-emitter configure)."));
        return;
    }
    if ((selected == rclone_index() || selected == rclone_index() + 1) && pkg_job) {
        snprintf(notice, sizeof notice, strcmp(pkg_action, "install") == 0 ?
                 _("Wait: %s is still being installed.") : _("Wait: %s is still being removed."), pkg_title());
        return;
    }
    if (selected == rclone_index()) {
        if (rclone_installed()) {
            if (rc_set("service_rclone", NULL) < 0) {
                snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
                return;
            }
            rclone_on = 0;
            kill(1, SIGHUP);
            start_pkg_job("remove", RCLONE_PACKAGES);
        } else {
            start_pkg_job("install", RCLONE_PACKAGES);
        }
        snprintf(notice, sizeof notice, "%s", pkg_job ? (strcmp(pkg_action, "install") == 0 ?
                 _("Cloud drives (rclone): installing rclone and fuse3; configure remotes below when they are in.") :
                 _("Cloud drives (rclone): unmounted; removing rclone.")) :
                 _("Cloud drives (rclone): cannot run locachy-pkg."));
        return;
    }
    if (selected == rclone_index() + 1) {
        if (!rclone_installed() && !rclone_on) {
            snprintf(notice, sizeof notice, "%s", _("rclone is not installed: the line above installs it."));
            return;
        }
        if (rc_set("service_rclone", rclone_on ? NULL : RCLONE_SERVICE) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        rclone_on = !rclone_on;
        kill(1, SIGHUP);
        snprintf(notice, sizeof notice, "%s", rclone_on ?
                 _("Cloud drives mounted under ~/Drives/<remote>.") : _("Cloud drives unmounted."));
        return;
    }
    if (selected == rclone_index() + 2) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(notice, sizeof notice, "%s", _("Enter: open rclone config in a terminal (iCloud, Google Drive, OneDrive, WebDAV)."));
        return;
    }
    if (selected == lang_index()) {
        if (!nlanguages) find_languages();
        int next = (current_language() + (dir < 0 ? nlanguages - 1 : 1)) % nlanguages;
        char value[32];
        snprintf(value, sizeof value, "%s.UTF-8", languages[next]);
        if (rc_set("language", next ? value : NULL) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        {
            char lconf[64];
            snprintf(lconf, sizeof lconf, "LANG=%s\n", next ? value : "en_US.UTF-8");
            write_atomic_file("/etc/locale.conf", "/etc/locale.conf.tmp", lconf);
        }
        base_language_reload(); /* this program at once; the others on process 1's SIGHUP */
        kill(1, SIGHUP);
        snprintf(notice, sizeof notice, _("Language: %s."), language_name(languages[next]));
        return;
    }
    if (selected == host_index()) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(notice, sizeof notice, "%s", _("Enter: set this computer's hostname (/etc/hostname)."));
        return;
    }
    if ((selected == ime_index() || selected == ime_index() + 1) && pkg_job) {
        snprintf(notice, sizeof notice, strcmp(pkg_action, "install") == 0 ?
                 _("Wait: %s is still being installed.") : _("Wait: %s is still being removed."), pkg_title());
        return;
    }
    if (selected == ime_index()) {
        if (rime_installed()) {
            start_pkg_job("remove", IME_ALL_PACKAGES);
        } else {
            start_pkg_job("install", IME_BASE_PACKAGES);
        }
        snprintf(notice, sizeof notice, "%s", pkg_job ? (strcmp(pkg_action, "install") == 0 ?
                 _("Input method: installing librime and English words, then rebuilding.") :
                 _("Input method: removing Rime and its dictionaries, then rebuilding.")) :
                 _("Input method: cannot run locachy-pkg."));
        return;
    }
    if (selected == ime_index() + 1) {
        if (rime_zh_installed()) {
            start_pkg_job("remove", IME_ZH_PACKAGES);
        } else {
            start_pkg_job("install", rime_installed() ? IME_ZH_PACKAGES : IME_ALL_PACKAGES);
        }
        snprintf(notice, sizeof notice, "%s", pkg_job ? (strcmp(pkg_action, "install") == 0 ?
                 _("Chinese Pinyin: installing rime-frost and grammar model, then rebuilding.") :
                 _("Chinese Pinyin: removing rime-frost (English completion kept).")) :
                 _("Chinese Pinyin: cannot run locachy-pkg."));
        return;
    }
    if (selected == tpm_index()) {
        /* TRANSLATORS: Enter and Delete are key names; keep them. */
        snprintf(notice, sizeof notice, "%s", _("Enter: bind LUKS2 auto-unlock to TPM (1.2 or 2.0)   Delete: unbind"));
        return;
    }
    if (selected == bri_index()) {
        int bcur = 0, bmax = 0;
        if (!base_brightness_get || !base_brightness_set || base_brightness_get(&bcur, &bmax) < 0 || bmax <= 0) {
            snprintf(notice, sizeof notice, "%s", _("No controllable backlight on this display."));
            return;
        }
        int step = bmax >= 20 ? (bmax + 10) / 20 : 1;
        int floor = bmax <= 16 ? 0 : 1;
        int next = dir > 0 ? bcur + step : bcur - step;
        if (next < floor) next = floor;
        if (next > bmax) next = bmax;
        if (base_brightness_set(next, bmax) == 0) {
            snprintf(notice, sizeof notice, _("Screen brightness: %d%%."), (next * 100 + bmax / 2) / bmax);
        } else {
            snprintf(notice, sizeof notice, "%s", _("Cannot set screen brightness."));
        }
        return;
    }
    if (selected == mach_index()) {
        if (!nmachines) find_machines();
        int next = (current_machine_index() + (dir < 0 ? nmachines - 1 : 1)) % nmachines;
        const char *val = next ? machine_profiles[next] : NULL;
        if (rc_set("machine", val) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        snprintf(active_machine, sizeof active_machine, "%s", val ? val : "");
        rc_read(on_rc, NULL);
        load_monitors();
        apply_all();
        kill(1, SIGHUP);
        snprintf(notice, sizeof notice, _("Machine profile: %s."), val ? val : _("generic PC"));
        return;
    }
    if (selected == upd_index() || selected == upd_index() + 1) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(notice, sizeof notice, "%s", selected == upd_index() ?
                 _("Enter: the git repository LOCachy's source updates come from (or off).") :
                 _("Enter: the branch of that repository to follow (main by default)."));
        return;
    }
    if (selected == boot_index()) {
        if (rc_set("boot_verbose", boot_verbose ? NULL : "1") < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        boot_verbose = !boot_verbose;
        make_boot_image();
        return;
    }
    if (selected == boot_index() + 1 || selected == boot_index() + 2) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(notice, sizeof notice, "%s", selected == boot_index() + 1 ?
                 _("Enter: options added to the kernel's command line, made into the boot image.") :
                 _("Enter: make the boot image again (locachy-boot), as pacman's hooks do after updates."));
        return;
    }
    if (selected == tnet_index()) {
        /* term's shell is the user's (user-shell); admin, root's, never has it. */
        if (rc_set("term_network", term_net ? "0" : NULL) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        term_net = !term_net;
        snprintf(notice, sizeof notice, "%s", term_net ? _("The terminal's shell is on the network from its next start.") :
                 _("The terminal's shell has no network from its next start."));
        return;
    }
    if (selected == circ_index()) {
        /* Round touchpads (libinput's list), on, off, round again. */
        int next = dir < 0 ? (circular == -1 ? 0 : circular == 0 ? 1 : -1) :
                             (circular == -1 ? 1 : circular == 1 ? 0 : -1);
        if (rc_set("touchpad_circular", next < 0 ? NULL : next ? "1" : "0") < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        circular = next;
        if (apply_circular() < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), LOCAL_QUIRKS);
            return;
        }
        /* libinput reads its quirks once per context: see apply_circular. */
        snprintf(notice, sizeof notice, "%s", circular > 0 ?
                 _("Every touchpad scrolls around its edge: programs after home, the shared display from its next start.") :
                 circular == 0 ?
                 _("Every touchpad scrolls with two fingers: programs after home, the shared display from its next start.") :
                 _("Round touchpads scroll around their edge: programs after home, the shared display from its next start."));
        return;
    }
    if (selected >= repo_index() && selected <= repo_index() + NREPO_SECTIONS) {
        int ri = selected - repo_index();
        char what[24];
        if (repos_job || pkg_job) {
            snprintf(notice, sizeof notice, "%s", repos_job ? _("Wait: the repositories' job is still running.") :
                     _("Wait: a package job is still running."));
            return;
        }
        if (ri == NREPO_SECTIONS) {
            /* TRANSLATORS: Enter is the Enter key; keep it. */
            snprintf(notice, sizeof notice, "%s", _("Enter: rank the mirrors from here again (reflector, rate-mirrors)."));
            return;
        }
        snprintf(what, sizeof what, "%s=%s", repo_sections[ri], repo_on[ri] ? "off" : "on");
        start_repos_job(what);
        snprintf(notice, sizeof notice, repos_job ? _("Checking the declared packages against the repositories...") :
                 _("Cannot run %s."), "locachy-repos");
        return;
    }
    if (selected >= opt_index() && selected < opt_index() + NOPT_MODULES) {
        int mi = selected - opt_index();
        const struct opt_module *m = &opt_modules[mi];
        if (pkg_job) {
            snprintf(notice, sizeof notice, strcmp(pkg_action, "install") == 0 ?
                     _("Wait: %s is still being installed.") : _("Wait: %s is still being removed."), pkg_title());
            return;
        }
        if (opt_module_installed(mi)) {
            if ((m->rc_key && rc_set(m->rc_key, NULL) < 0) ||
                (m->rc_key2 && rc_set(m->rc_key2, NULL) < 0)) {
                snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
                return;
            }
            if (m->rc_key || m->rc_key2) {
                kill(1, SIGHUP);
            }
            start_pkg_job("remove", m->packages);
        } else {
            if ((m->rc_key && rc_set(m->rc_key, m->rc_val) < 0) ||
                (m->rc_key2 && rc_set(m->rc_key2, m->rc_val2) < 0)) {
                snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
                return;
            }
            start_pkg_job("install", m->packages);
            if (m->rc_key || m->rc_key2) {
                kill(1, SIGHUP);
            }
        }
        snprintf(notice, sizeof notice, pkg_job ? (strcmp(pkg_action, "install") == 0 ?
                 _("%s: installing packages in background.") : _("%s: removing packages in background.")) :
                 _("%s: cannot run locachy-pkg."), _(m->label));
        return;
    }
    if ((selected == fp_index() || selected == fp_index() + 1) && pkg_job) {
        snprintf(notice, sizeof notice, strcmp(pkg_action, "install") == 0 ?
                 _("Wait: %s is still being installed.") : _("Wait: %s is still being removed."), pkg_title());
        return;
    }
    if (selected == fp_index()) {
        if (fprintd_installed()) {
            if (fingerprint_on) {
                if (rc_set("unlock_fingerprint", NULL) < 0) {
                    snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
                    return;
                }
                fingerprint_on = 0; /* off before it goes */
                sudo_ways();
            }
            start_pkg_job("remove", "fprintd");
        } else {
            start_pkg_job("install", "fprintd");
        }
        snprintf(notice, sizeof notice, "%s", pkg_job ? (strcmp(pkg_action, "install") == 0 ?
                 _("Fingerprint reader: installing fprintd; switch it on below when it is in.") :
                 _("Fingerprint reader: switched off; removing fprintd.")) :
                 _("Fingerprint reader: cannot run locachy-pkg."));
        return;
    }
    if (selected == fp_index() + 1) {
        if (!fprintd_installed()) {
            snprintf(notice, sizeof notice, "%s", _("fprintd is not installed: the line above installs it."));
            return;
        }
        if (rc_set("unlock_fingerprint", fingerprint_on ? NULL : "1") < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        fingerprint_on = !fingerprint_on;
        if (sudo_ways() < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), SUDO_PAM);
            return;
        }
        snprintf(notice, sizeof notice, "%s", fingerprint_on ? _("The lock screen and sudo take a fingerprint as well "
                                                                 "as the password.") :
                 _("Only the password unlocks, and sudo asks only for it."));
        return;
    }
    if (selected == fp_index() + 2) {
        /* TRANSLATORS: Delete is the Delete key; keep it. */
        snprintf(notice, sizeof notice, "%s", _("Delete: delete these fingerprints (asks once more)."));
        return;
    }
    if (selected == fp_index() + 3) {
        fp_next = (fp_next + dir + NFINGERS) % NFINGERS;
        return;
    }
    if (selected == sec_index() + 3) {
        int i = 0;
        while (i < NLOCK_AFTERS - 1 && lock_afters[i] < lock_after) {
            i++;
        }
        i = (i + dir + NLOCK_AFTERS) % NLOCK_AFTERS;
        char value[8];
        snprintf(value, sizeof value, "%d", lock_afters[i]);
        if (rc_set("lock_after", value) < 0) {
            snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
            return;
        }
        lock_after = lock_afters[i];
        kill(1, SIGHUP); /* labwc-control sees rc.conf change itself */
        if (lock_after) {
            snprintf(notice, sizeof notice, P_("Locked after %d min without input (on the shared display, not "
                                               "while a video plays).",
                                               "Locked after %d min without input (on the shared display, not "
                                               "while a video plays).", lock_after), lock_after);
        } else {
            snprintf(notice, sizeof notice, "%s", _("Never locked for want of input (the lock key and the lid still lock)."));
        }
        return;
    }
    if (selected == tz_index() || selected == tz_index() + 2) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(notice, sizeof notice, "%s", selected == tz_index() ? _("Enter: choose the time zone.") :
                 _("Enter: set the date and time."));
        return;
    }
    if (selected == hw) {
        /* TRANSLATORS: Enter is the Enter key; keep it. */
        snprintf(notice, sizeof notice, "%s", _("Enter: arrange the screens with the direction buttons."));
        return;
    }
    if (selected > hw && selected < hw + 1 + nrow_maps) {
        int sidx = selected - hw - 1;
        struct monitor *m = &monitors[row_maps[sidx].mon_idx];
        if (!m_is_connected(m)) return;
        struct monitor previous = *m;
        if (row_maps[sidx].field == 1 && m->nmodes > 1) {
            m->mode_sel = (m->mode_sel + (dir > 0 ? 1 : m->nmodes - 1)) % m->nmodes;
        } else if (row_maps[sidx].field == 2) {
            int si = 0;
            while (si < NSCALES - 1 && scales[si] < m->scale - 0.01) {
                si++;
            }
            si = (si + (dir > 0 ? 1 : NSCALES - 1)) % NSCALES;
            m->scale = scales[si];
        }
        if (!monitor_saved()) *m = previous;
        else screen_rescale(); /* this screen's text follows its scale at once */
        return;
    }
    if (selected >= hw + 1 + nrow_maps) {
        cycle_rule(selected - hw - 1 - nrow_maps, dir);
        return;
    }
    apply_all();
    save_rc();
    snprintf(notice, sizeof notice, _("Saved to %s and applied."), RC_CONF);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN); /* an answer to a PAM child that has ended (auth.c) */
    int applying = argc > 1 && strcmp(argv[1], "--apply") == 0;
    double t0 = applying ? uptime() : 0;
    if (applying) say("settings: applying at %.3f s", t0);
    tp_present = detect_pointing_stick();
    rc_read(on_rc, NULL);
    double ready = applying ? uptime() : 0;
    load_monitors();
    double monitors_ready = applying ? uptime() : 0;
    apply_all();
    if (applying && base_brightness_restore) {
        base_brightness_restore();
    }
    if (applying) {
        say("settings: applied at %.3f s: input/config %.0f ms, monitors %.0f ms, devices %.0f ms",
            uptime(), (ready - t0) * 1000, (monitors_ready - ready) * 1000,
            (uptime() - monitors_ready) * 1000);
        return 0;
    }

    screen_open(BASE_FONT);
    keys_open();
    nrules = open_rules(rule_types, rule_values, MAX_RULES);
    load_nets();
    load_fingers();
    load_faces();
    load_bridges();
    load_tpm_status();
    load_repos();
    draw();

    for (;;) {
        int key = key_poll_timeout(-1, pkg_job || repos_job ? 500 : 2000);
        if (key != 0 && key != K_DEL) {
            fp_delete_asked = face_delete_asked = tor_bridges_delete_asked = tpm_clear_asked = 0; /* Delete twice in a row deletes them */
        }
        build_row_maps();
        pkg_check();
        repos_check();
        if (key == K_POINTER) {
            key = settings_pointer();
            if (key == 0) continue;
            if (key == -1) {
                draw();
                continue;
            }
        }
        if (key == 0 || key == K_SHOWN) {
            load_monitors();
            if (key == K_SHOWN) {
                nrules = open_rules(rule_types, rule_values, MAX_RULES);
                load_nets();
            }
            build_row_maps();
            build_entries();
            if (entry_of(selected) < 0) {
                selected = first_setting();
            }
            draw();
        } else if (key == 'q' || (key == 27 && !in_page && !search[0])) {
            return 0;
        } else if (key == '/') {
            find_setting();
            draw();
        } else if (key == 27 && search[0]) { /* back to the categories, at the setting found */
            search[0] = '\0';
            in_page = 0;
            snprintf(notice, sizeof notice, "%s", _(default_notice));
            draw();
        } else if (!in_page) {
            sidebar_key(key);
            draw();
        } else if (key == 27 || key == '\t') {
            in_page = 0;
            draw();
        } else if (key == K_UP || key == K_DOWN) {
            page_move(key == K_UP ? -1 : 1);
            draw();
        } else if (key == K_PGUP || key == K_PGDN) {
            page_move(key == K_PGUP ? -page_rows() : page_rows());
            draw();
        } else if (key == K_BOL || key == K_EOL) {
            page_move(key == K_BOL ? -MAX_ENTRIES : MAX_ENTRIES);
            draw();
        } else if (key == K_LEFT || key == '-') {
            adjust(-1);
            draw();
        } else if (key == K_RIGHT || key == '+' || key == '=') {
            adjust(+1);
            draw();
        } else if (bluetooth_settings_key(key)) {
            draw();
        } else if (screen_settings_key(key)) {
            draw();
        } else if (key == '\n' && selected == cl_index() + 2) {
            clash_subscription();
            draw();
        } else if (key == '\n' && selected == tr_index() + 2) {
            tor_add_bridge();
            draw();
        } else if (key == K_DEL && selected == tr_index() + 2) {
            tor_remove_bridges();
            draw();
        } else if (key == '\n' && selected == wp_index()) {
            choose_wallpaper();
            draw();
        } else if (key == K_DEL && selected == wp_index()) {
            if (rc_set("wallpaper", NULL) == 0) {
                wallpaper_path[0] = '\0';
                snprintf(notice, sizeof notice, "%s", _("No wallpaper; the shared display shows that when it is next shown."));
            }
            draw();
        } else if (key == '\n' && selected == tk_index()) {
            choose_key(_("Terminal key (shared display)"), "key_terminal", terminal_key, sizeof terminal_key, 0,
                       apply_labwc);
            draw();
        } else if (key == K_DEL && selected == tk_index()) {
            if (rc_set("key_terminal", "") == 0) {
                terminal_key[0] = '\0';
                apply_labwc();
                snprintf(notice, sizeof notice, "%s", _("No terminal key: saved and applied."));
            }
            draw();
        } else if (key == '\n' && selected >= imek_index() && selected < imek_index() + NIME_KEYS) {
            int k = selected - imek_index();
            choose_key(_(ime_key_labels[k]), ime_key_names[k], ime_keys[k], sizeof ime_keys[k], 1, reread_ime_keys);
            draw();
        } else if (key == K_DEL && selected >= imek_index() && selected < imek_index() + NIME_KEYS) {
            int k = selected - imek_index();
            if (rc_set(ime_key_names[k], "") == 0) {
                ime_keys[k][0] = '\0';
                reread_ime_keys();
                snprintf(notice, sizeof notice, _("%s: none, saved and applied."), _(ime_key_labels[k]));
            }
            draw();
        } else if (key == '\n' && selected == sec_index()) {
            change_password();
            draw();
        } else if (key == '\n' && selected == host_index()) {
            change_hostname();
            draw();
        } else if (key == '\n' && selected == upd_index()) {
            change_update_repo();
            draw();
        } else if (key == '\n' && selected == upd_index() + 1) {
            change_update_branch();
            draw();
        } else if (key == '\n' && selected == boot_index() + 1) {
            change_boot_cmdline();
            draw();
        } else if (key == '\n' && selected == boot_index() + 2) {
            make_boot_image();
            draw();
        } else if (key == '\n' && selected == repo_index() + NREPO_SECTIONS) {
            if (repos_job || pkg_job) {
                snprintf(notice, sizeof notice, "%s", repos_job ? _("Wait: the repositories' job is still running.") :
                         _("Wait: a package job is still running."));
            } else {
                start_repos_job("rank");
                snprintf(notice, sizeof notice, repos_job ? _("Ranking the mirrors (reflector, rate-mirrors)...") :
                         _("Cannot run %s."), "locachy-repos");
            }
            draw();
        } else if (key == '\n' && selected == tpm_index()) {
            if (access("/usr/local/bin/locachy-tpm", X_OK) != 0) {
                snprintf(notice, sizeof notice, _("%s is not installed."), "locachy-tpm");
            } else if (login_skip) {
                snprintf(notice, sizeof notice, "%s",
                         _("Not bound: with no lock at boot, anyone who turns it on would be in. Turn the boot lock on first."));
            } else {
                char *const cmd[] = { "/usr/local/bin/locachy-tpm", "enroll", NULL };
                int st = in_term(cmd);
                load_tpm_status();
                snprintf(notice, sizeof notice, st == 0 ? _("%s done.") : _("%s did not finish."), "locachy-tpm enroll");
            }
            draw();
        } else if (key == K_DEL && selected == tpm_index() && access("/usr/local/bin/locachy-tpm", X_OK) == 0) {
            if (!tpm_clear_asked) {
                tpm_clear_asked = 1;
                /* TRANSLATORS: Delete is the Delete key; keep it. */
                snprintf(notice, sizeof notice, "%s", _("Delete again: unbind TPM disk auto-unlock (boot will ask passphrase)."));
            } else {
                char buf[512];
                char *const cmd[] = { "/usr/local/bin/locachy-tpm", "clear", NULL };
                tpm_clear_asked = 0;
                command_output(cmd, buf, sizeof buf);
                load_tpm_status();
                snprintf(notice, sizeof notice, "%s", _("TPM disk auto-unlock unbound."));
            }
            draw();
        } else if (key == '\n' && selected == fp_index() + 3) {
            if (fprintd_installed()) {
                enroll_finger(fingers[fp_next]);
            } else {
                snprintf(notice, sizeof notice, "%s", _("fprintd is not installed: the Fingerprint reader line installs it."));
            }
            draw();
        } else if (key == '\n' && (selected == fc_index() + 5 || selected == fc_index() + 6)) {
            const char *user = user_name();
            int add = selected == fc_index() + 5;
            if (!howdy_installed() || !user || (!add && access(IR_EMITTER, X_OK) != 0)) {
                snprintf(notice, sizeof notice, _("%s is not installed."), add ? "howdy" : "linux-enable-ir-emitter");
            } else {
                char *const howdy_add[] = { HOWDY, "-U", (char *)user, "add", NULL };
                char *const emitter[] = { IR_EMITTER, "configure", NULL };
                int status = in_term(add ? howdy_add : emitter);
                snprintf(notice, sizeof notice, status == 0 ? _("%s done.") : _("%s did not finish."),
                         add ? "howdy add" : "linux-enable-ir-emitter configure");
                load_faces();
            }
            draw();
        } else if (key == K_DEL && selected == fc_index() + 3 && howdy_installed()) {
            if (!face_delete_asked) {
                face_delete_asked = 1;
                /* TRANSLATORS: Delete is the Delete key; keep it. */
                snprintf(notice, sizeof notice, _("Delete again: forget every face of %.40s."),
                         user_name() ? user_name() : _("the user"));
            } else {
                char *const argv[] = { HOWDY, "-U", (char *)user_name(), "-y", "clear", NULL };
                char buf[1024];
                face_delete_asked = 0;
                if (argv[2]) {
                    command_output(argv, buf, sizeof buf);
                }
                load_faces();
                snprintf(notice, sizeof notice, _("Faces: %.100s."), face_models);
            }
            draw();
        } else if (key == K_DEL && selected == fp_index() + 2 && fprintd_installed()) {
            if (!fp_delete_asked) {
                fp_delete_asked = 1;
                /* TRANSLATORS: Delete is the Delete key; keep it. */
                snprintf(notice, sizeof notice, _("Delete again: delete every fingerprint of %.40s."),
                         user_name() ? user_name() : _("the user"));
            } else {
                char *const argv[] = { "/usr/bin/fprintd-delete", (char *)user_name(), NULL };
                char buf[1024];
                fp_delete_asked = 0;
                if (argv[1]) {
                    command_output(argv, buf, sizeof buf);
                }
                load_fingers();
                snprintf(notice, sizeof notice, _("Fingerprints: %.100s."), fp_enrolled);
            }
            draw();
        } else if (key == '\n' && selected == sec_index() + 2) {
            choose_key(_("Lock key"), "key_lock", lock_key, sizeof lock_key, 0, reread_keys);
            draw();
        } else if (key == K_DEL && selected == sec_index() + 2) {
            if (rc_set("key_lock", "") == 0) {
                lock_key[0] = '\0';
                reread_keys();
                snprintf(notice, sizeof notice, "%s", _("No lock key: the lid still locks."));
            }
            draw();
        } else if (key == '\n' && selected == tz_index()) {
            pick_zone();
            draw();
        } else if (key == '\n' && selected == tz_index() + 2) {
            set_clock();
            draw();
        } else if (key == '\n' && selected == rclone_index() + 2) {
            if (!rclone_installed()) {
                snprintf(notice, sizeof notice, "%s", _("rclone is not installed: the Cloud drives installed line installs it."));
            } else {
                char *const argv[] = { RCLONE_SERVICE, "--config", NULL };
                int status = in_term(argv);
                snprintf(notice, sizeof notice, "%s", status == 0 ?
                         _("rclone config done.") : _("rclone config did not finish."));
            }
            draw();
        } else if (key == '\n' || key == ' ') {
            adjust(+1);
            draw();
        }
    }
}
