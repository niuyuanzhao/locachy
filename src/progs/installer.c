/*
 * installer: LOCachy's installation on the live medium's screen, in the
 * framebuffer as settings and packages are. It asks; locachy-install decides
 * and does everything: the choices, their defaults and the disks come from
 * its --describe-json, what is chosen goes to it as options (--check first,
 * then the installation), the disk passphrase and the user's password as
 * files only root can read (RUN_DIR), never on a command line. Its output
 * is shown in a pane of its own, its progress as stages above it.
 *   start      install, a terminal (a root shell, for repairs: term), reboot,
 *              power off; the interface's language
 *   steps      network, disk, encryption, user, region, software, system,
 *              summary: a sidebar of them and a page each, as settings'
 *   progress   the stages and the output; then done (reboot) or failed
 * The live system's process 1 is systemd, not ours: the screen and the
 * keyboards are opened directly, and while this runs the console it was
 * started on (tty1) is in graphics mode with its keyboard off, so what is
 * typed here (a passphrase) never reaches the shell under it. installer
 * --text puts the console back; root's .bash_profile runs it after this,
 * however this ended.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <linux/input-event-codes.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "base.h"
#include "internal.h"

#define FG     0xD0D0D0
#define BG     0x101820
#define ACCENT 0x80C0FF
#define DIM    0x808890
#define ERROR  0xFF9090
#define WARN   0xE0C060
#define PANE   0x05080C /* the output pane, as packages' */
#define SIDE_BG 0x304860

#ifndef INSTALL_CMD /* the places: another for each in tools/test-installer.c */
#define INSTALL_CMD "/usr/local/bin/locachy-install"
#define TERM_CMD    "/usr/local/bin/term"
#define NMTUI       "/usr/bin/nmtui"
#define SYSTEMCTL   "/usr/bin/systemctl"
#define RUN_DIR     "/run/locachy-installer"
#define KEYTABLE    "/usr/local/lib/locachy/keytable"
#define KEYMAP_TABLE "/usr/local/share/locachy/keymap.table" /* keys.c's: term reads it too */
#endif

#define MAX_DISKS 16
#define MAX_PARTS 32
#define MAX_ITEMS 64
#define MAX_LIST  1024
#define MAX_ENTRIES 256
#define LABEL_CELLS 30

/* What locachy-install --describe-json says. */
struct part {
    char path[64], fstype[32], label[64], type[64], mounts[128];
    long long size;
};
struct disk {
    char path[64], model[64], tran[16], in_use[200];
    long long size;
    int removable, nparts;
    struct part parts[MAX_PARTS];
};
struct named {
    char name[64], text[200];
    int flag, def[2]; /* def: a module's default in English, in Chinese */
};
static struct disk disks[MAX_DISKS];
static int ndisks;
static struct named modules[MAX_ITEMS], containers[MAX_ITEMS], machines[16], imes[8];
static int nmodules, ncontainers, nmachines, nimes;
static char keymaps[MAX_LIST][48], zones[MAX_LIST][48];
static int nkeymaps, nzones;
static char levels[8][16], level_auto[16], cpu_model[48], repo_names[4][16];
static int nlevels, nrepos;
static int tpm_usable, net_profiles;
static char tpm_advice[512];
static long long memory_bytes, esp_bytes, min_root_bytes;
static struct {
    char user[32], hostname[64], keymap[48], timezone[64], update_repo[256], update_branch[128];
    int swap_gib, copy_network;
    unsigned char repos[4];
} dflt;
static int described; /* the description was read */

/* What is chosen. */
enum { WHOLE, PARTS, MOUNTED };
enum { ENC_OFF, ENC_PASS, ENC_TPM };
static struct {
    int lang; /* 0 English, 1 Chinese: the system's */
    int disk_mode, swap_gib, encrypt, boot_lock, ntp, ime, machine, level, rank, copy_network, hibernate;
    char disk[64], esp[64], root[64], swap[64];
    char passphrase[256], password[256];
    char user[32], full_name[128], hostname[64], keymap[48], timezone[64], cpu[48];
    char update_repo[256], update_branch[128];
    unsigned char module_on[MAX_ITEMS], container_on[MAX_ITEMS], repo_on[4];
    int lang_touched, modules_touched, containers_touched, ime_touched;
} c;
static const char *const languages[2] = { "en_US.UTF-8", "zh_CN.UTF-8" };
static int ui_lang; /* the interface's: 0 English, 1 Chinese */

static char notice[512];
static int notice_error;

static void tell(int error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tell(int error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(notice, sizeof notice, fmt, ap);
    va_end(ap);
    notice_error = error;
}

/* s into out, cut to fit. */
static void copy(char *out, size_t size, const char *s)
{
    size_t n = 0;
    for (; s && s[n] && n + 1 < size; n++) out[n] = s[n];
    if (size) out[n] = '\0';
}

/* The console the program was started on: graphics mode, its keyboard
 * off (keys reach this program through evdev only), and back. */
static int console = -1, console_kb = K_UNICODE;

static void console_restore(void)
{
    if (console < 0) return;
    ioctl(console, KDSKBMODE, console_kb);
    ioctl(console, KDSETMODE, KD_TEXT);
    tcflush(console, TCIFLUSH); /* nothing typed here is left for the shell */
}

static void console_take(void)
{
    int mode;
    if (ioctl(STDIN_FILENO, KDGETMODE, &mode) < 0) return; /* not a virtual console */
    console = STDIN_FILENO;
    if (ioctl(console, KDGKBMODE, &console_kb) < 0 || console_kb == K_OFF) console_kb = K_UNICODE;
    ioctl(console, KDSETMODE, KD_GRAPHICS);
    ioctl(console, KDSKBMODE, K_OFF);
}

static void secret_path(const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", RUN_DIR, name);
}

static void remove_secrets(void)
{
    char path[512];
    secret_path("passphrase", path, sizeof path);
    unlink(path);
    secret_path("password", path, sizeof path);
    unlink(path);
}

static void on_fatal(int sig)
{
    console_restore();
    remove_secrets();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void at_exit(void)
{
    remove_secrets();
    console_restore();
}

/* A file only root can read, with text and no newline: 0, or -1. */
static int write_secret(const char *name, const char *text, char *path, size_t size)
{
    mkdir(RUN_DIR, 0700);
    secret_path(name, path, size);
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t len = strlen(text);
    int ok = write(fd, text, len) == (ssize_t)len;
    return close(fd) == 0 && ok ? 0 : -1;
}

/* Run argv and keep what it prints (and its errors, with both), at most
 * size - 1 bytes: its exit status, or -1. */
static int capture(char *const argv[], char *out, size_t size, int both)
{
    int fds[2];
    out[0] = '\0';
    if (pipe2(fds, O_CLOEXEC) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        dup2(null, STDIN_FILENO);
        dup2(fds[1], STDOUT_FILENO);
        dup2(both ? fds[1] : null, STDERR_FILENO);
        execv(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return -1;
    }
    size_t got = 0;
    for (;;) {
        char sink[4096];
        ssize_t n = got + 1 < size ? read(fds[0], out + got, size - 1 - got) : read(fds[0], sink, sizeof sink);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (got + 1 < size) got += (size_t)n;
    }
    out[got] = '\0';
    close(fds[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static struct json_object *jget(struct json_object *o, const char *key)
{
    struct json_object *v = NULL;
    return o && json_object_object_get_ex(o, key, &v) ? v : NULL;
}

static const char *jstr(struct json_object *o, const char *key)
{
    struct json_object *v = jget(o, key);
    return v && json_object_is_type(v, json_type_string) ? json_object_get_string(v) : "";
}

static long long jint(struct json_object *o, const char *key)
{
    struct json_object *v = jget(o, key);
    return v ? json_object_get_int64(v) : 0;
}

static int jlen(struct json_object *a)
{
    return a && json_object_is_type(a, json_type_array) ? (int)json_object_array_length(a) : 0;
}

static struct json_object *jat(struct json_object *a, int i)
{
    return json_object_array_get_idx(a, (size_t)i);
}

static const char *jtext(struct json_object *a, int i)
{
    const char *s = json_object_get_string(jat(a, i));
    return s ? s : "";
}

static int jhas(struct json_object *a, const char *s)
{
    for (int i = 0; i < jlen(a); i++) {
        if (strcmp(jtext(a, i), s) == 0) return 1;
    }
    return 0;
}

static void load_disks(struct json_object *a)
{
    for (ndisks = 0; ndisks < jlen(a) && ndisks < MAX_DISKS; ndisks++) {
        struct json_object *j = jat(a, ndisks), *kids = jget(j, "children");
        struct disk *k = &disks[ndisks];
        copy(k->path, sizeof k->path, jstr(j, "path"));
        copy(k->model, sizeof k->model, jstr(j, "model"));
        copy(k->tran, sizeof k->tran, jstr(j, "tran"));
        copy(k->in_use, sizeof k->in_use, jstr(j, "in_use"));
        k->size = jint(j, "size");
        k->removable = json_object_get_boolean(jget(j, "removable"));
        for (k->nparts = 0; k->nparts < jlen(kids) && k->nparts < MAX_PARTS; k->nparts++) {
            struct json_object *p = jat(kids, k->nparts), *m = jget(p, "mountpoints");
            struct part *q = &k->parts[k->nparts];
            copy(q->path, sizeof q->path, jstr(p, "path"));
            copy(q->fstype, sizeof q->fstype, jstr(p, "fstype"));
            copy(q->label, sizeof q->label, jstr(p, "label"));
            copy(q->type, sizeof q->type, jstr(p, "parttypename"));
            q->size = jint(p, "size");
            q->mounts[0] = '\0';
            for (int i = 0; i < jlen(m); i++) {
                size_t n = strlen(q->mounts);
                snprintf(q->mounts + n, sizeof q->mounts - n, "%s%s", n ? "," : "", jtext(m, i));
            }
        }
    }
}

/* The description: what there is to choose, the defaults, the disks. */
static int load_description(void)
{
    static char buf[1 << 20];
    char *const argv[] = { INSTALL_CMD, "--describe-json", NULL };
    if (capture(argv, buf, sizeof buf, 0) != 0) return -1;
    struct json_object *d = json_tokener_parse(buf);
    if (!d) return -1;
    struct json_object *def = jget(d, "defaults"), *a, *o;
    copy(dflt.user, sizeof dflt.user, jstr(def, "user"));
    copy(dflt.hostname, sizeof dflt.hostname, jstr(def, "hostname"));
    copy(dflt.keymap, sizeof dflt.keymap, jstr(def, "keymap"));
    copy(dflt.timezone, sizeof dflt.timezone, jstr(def, "timezone"));
    copy(dflt.update_repo, sizeof dflt.update_repo, jstr(def, "update_repo"));
    copy(dflt.update_branch, sizeof dflt.update_branch, jstr(def, "update_branch"));
    dflt.swap_gib = (int)jint(def, "swap_gib");
    dflt.copy_network = json_object_get_boolean(jget(def, "copy_network"));
    a = jget(d, "repo_sections");
    for (nrepos = 0; nrepos < jlen(a) && nrepos < 4; nrepos++) {
        copy(repo_names[nrepos], sizeof repo_names[0], jtext(a, nrepos));
        dflt.repos[nrepos] = (unsigned char)jhas(jget(def, "repos"), repo_names[nrepos]);
    }
    o = jget(d, "levels");
    a = jget(o, "supported");
    for (nlevels = 0; nlevels < jlen(a) && nlevels < 8; nlevels++) {
        copy(levels[nlevels], sizeof levels[0], jtext(a, nlevels));
    }
    copy(level_auto, sizeof level_auto, jstr(o, "auto"));
    copy(cpu_model, sizeof cpu_model, jstr(o, "cpu_model"));
    a = jget(d, "ime");
    for (nimes = 0; nimes < jlen(a) && nimes < 8; nimes++) {
        copy(imes[nimes].name, sizeof imes[0].name, jstr(jat(a, nimes), "name"));
        copy(imes[nimes].text, sizeof imes[0].text, jstr(jat(a, nimes), "label"));
    }
    a = jget(d, "modules");
    for (nmodules = 0; nmodules < jlen(a) && nmodules < MAX_ITEMS; nmodules++) {
        struct named *m = &modules[nmodules];
        copy(m->name, sizeof m->name, jstr(jat(a, nmodules), "name"));
        copy(m->text, sizeof m->text, jstr(jat(a, nmodules), "description"));
        for (int l = 0; l < 2; l++) {
            m->def[l] = jhas(jget(jget(d, "module_defaults"), languages[l]), m->name);
        }
    }
    a = jget(d, "containers");
    for (ncontainers = 0; ncontainers < jlen(a) && ncontainers < MAX_ITEMS; ncontainers++) {
        struct named *k = &containers[ncontainers];
        copy(k->name, sizeof k->name, jstr(jat(a, ncontainers), "name"));
        k->flag = json_object_get_boolean(jget(jat(a, ncontainers), "host"));
        k->def[0] = k->def[1] = jhas(jget(d, "container_defaults"), k->name);
    }
    a = jget(d, "machines");
    for (nmachines = 0; nmachines < jlen(a) && nmachines < 16; nmachines++) {
        copy(machines[nmachines].name, sizeof machines[0].name, jstr(jat(a, nmachines), "name"));
        copy(machines[nmachines].text, sizeof machines[0].text, jstr(jat(a, nmachines), "description"));
    }
    a = jget(d, "keymaps");
    for (nkeymaps = 0; nkeymaps < jlen(a) && nkeymaps < MAX_LIST; nkeymaps++) {
        copy(keymaps[nkeymaps], sizeof keymaps[0], jtext(a, nkeymaps));
    }
    a = jget(d, "timezones");
    copy(zones[0], sizeof zones[0], "auto"); /* first: from the network */
    for (nzones = 1; nzones - 1 < jlen(a) && nzones < MAX_LIST; nzones++) {
        copy(zones[nzones], sizeof zones[0], jtext(a, nzones - 1));
    }
    o = jget(d, "tpm");
    tpm_usable = json_object_get_boolean(jget(o, "usable"));
    copy(tpm_advice, sizeof tpm_advice, jstr(o, "advice"));
    net_profiles = (int)jint(d, "network_profiles");
    memory_bytes = jint(d, "memory_bytes");
    esp_bytes = jint(d, "esp_bytes");
    min_root_bytes = jint(d, "min_root_bytes");
    load_disks(jget(d, "disks"));
    json_object_put(d);
    described = 1;
    return 0;
}

/* The defaults that follow the system's language (modules: CJK fonts;
 * programs: with the desktop; the input method), unless chosen by hand. */
static int module_on(const char *name)
{
    for (int i = 0; i < nmodules; i++) {
        if (strcmp(modules[i].name, name) == 0) return c.module_on[i];
    }
    return 0;
}

static void follow_modules(void)
{
    if (!c.containers_touched) {
        for (int i = 0; i < ncontainers; i++) {
            c.container_on[i] = (unsigned char)(containers[i].def[0] && module_on("desktop"));
        }
    }
}

static void follow_language(void)
{
    if (!c.modules_touched) {
        for (int i = 0; i < nmodules; i++) c.module_on[i] = (unsigned char)modules[i].def[c.lang];
    }
    follow_modules();
    if (!c.ime_touched) {
        c.ime = 0;
        for (int i = 0; i < nimes; i++) {
            if (strcmp(imes[i].name, c.lang ? "zh" : "off") == 0) c.ime = i;
        }
    }
}

static void set_defaults(void)
{
    c.lang = ui_lang;
    c.disk_mode = WHOLE;
    c.swap_gib = dflt.swap_gib;
    c.encrypt = ENC_PASS;
    c.boot_lock = c.ntp = c.rank = 1;
    c.copy_network = dflt.copy_network;
    c.hibernate = -1;
    copy(c.user, sizeof c.user, dflt.user);
    copy(c.hostname, sizeof c.hostname, dflt.hostname);
    copy(c.keymap, sizeof c.keymap, dflt.keymap);
    copy(c.timezone, sizeof c.timezone, dflt.timezone);
    copy(c.cpu, sizeof c.cpu, "auto");
    copy(c.update_repo, sizeof c.update_repo, dflt.update_repo);
    copy(c.update_branch, sizeof c.update_branch, dflt.update_branch);
    memcpy(c.repo_on, dflt.repos, sizeof c.repo_on);
    follow_language();
}

static int disk_index(const char *path)
{
    for (int i = 0; i < ndisks; i++) {
        if (path[0] && strcmp(disks[i].path, path) == 0) return i;
    }
    return -1;
}

static const struct part *find_part(const char *path)
{
    for (int i = 0; i < ndisks; i++) {
        for (int k = 0; k < disks[i].nparts; k++) {
            if (path[0] && strcmp(disks[i].parts[k].path, path) == 0) return &disks[i].parts[k];
        }
    }
    return NULL;
}

static void human_size(long long bytes, char *out, size_t size)
{
    if (bytes >= 1LL << 30) {
        snprintf(out, size, "%.1f GiB", (double)bytes / (1 << 30));
    } else {
        snprintf(out, size, "%.0f MiB", (double)bytes / (1 << 20));
    }
}

static void disk_text(const struct disk *k, char *out, size_t size)
{
    char sz[32];
    human_size(k->size, sz, sizeof sz);
    snprintf(out, size, "%-14s %10s  %s%s%s%s", k->path, sz, k->model[0] ? k->model : _("(no model name)"),
             k->tran[0] ? " (" : "", k->tran, k->tran[0] ? ")" : "");
}

static void part_text(const struct part *p, char *out, size_t size)
{
    char sz[32];
    human_size(p->size, sz, sizeof sz);
    snprintf(out, size, "%-16s %10s  %-12s %s%s%s%s", p->path, sz, p->fstype[0] ? p->fstype : "-", p->type,
             p->label[0] ? "  \"" : "", p->label, p->label[0] ? "\"" : "");
    if (p->mounts[0]) {
        size_t n = strlen(out);
        snprintf(out + n, size - n, _("  in use: %s"), p->mounts);
    }
}

/* The steps, and what still keeps each from being right (locachy-install
 * --check has the last word: these are the ones to show before it). */
enum { S_NETWORK, S_DISK, S_ENCRYPTION, S_USER, S_REGION, S_SOFTWARE, S_SYSTEM, S_SUMMARY, NSTEPS };
static const char *const step_names[NSTEPS] = {
    N_("Network"), N_("Disk"), N_("Encryption"), N_("User"), N_("Region"), N_("Software"), N_("System"),
    N_("Summary"),
};

static int valid_user(const char *s)
{
    if (!((*s >= 'a' && *s <= 'z') || *s == '_') || strlen(s) > 31 || strcmp(s, "root") == 0 ||
        strcmp(s, "locachy-builder") == 0) {
        return 0;
    }
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return 0;
    }
    return 1;
}

static int valid_hostname(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > 63 || s[0] == '-' || s[n - 1] == '-') return 0;
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-')) {
            return 0;
        }
    }
    return 1;
}

static long long disk_needs(void)
{
    return esp_bytes + (long long)c.swap_gib * (1LL << 30) + min_root_bytes;
}

static int problem(int step, char *out, size_t size)
{
    out[0] = '\0';
    if (step == S_DISK && c.disk_mode == WHOLE) {
        int i = disk_index(c.disk);
        char need[32];
        human_size(disk_needs(), need, sizeof need);
        if (i < 0) {
            snprintf(out, size, "%s", _("Choose the disk to install on."));
        } else if (disks[i].in_use[0]) {
            snprintf(out, size, "%s", disks[i].in_use);
        } else if (disks[i].size < disk_needs()) {
            snprintf(out, size, _("%s is too small: %s are needed."), disks[i].path, need);
        }
    } else if (step == S_DISK && c.disk_mode == PARTS) {
        if (!c.esp[0] || !c.root[0]) {
            snprintf(out, size, "%s", _("Choose the EFI system partition and the root partition."));
        } else if (strcmp(c.esp, c.root) == 0 || (c.swap[0] && (strcmp(c.swap, c.root) == 0 || strcmp(c.swap, c.esp) == 0))) {
            snprintf(out, size, "%s", _("The partitions must be different ones."));
        }
    } else if (step == S_ENCRYPTION && c.encrypt != ENC_OFF && !c.passphrase[0]) {
        snprintf(out, size, "%s", _("Set the disk passphrase."));
    } else if (step == S_USER) {
        if (!valid_user(c.user)) {
            snprintf(out, size, "%s", _("The user name: small letters, digits, - and _, starting with a letter."));
        } else if (!c.password[0]) {
            snprintf(out, size, "%s", _("Set the user's password."));
        } else if (!valid_hostname(c.hostname)) {
            snprintf(out, size, "%s", _("The computer's name: letters, digits and -, up to 63."));
        }
    }
    return out[0] != '\0';
}

static int first_problem(void)
{
    char why[256];
    for (int s = 0; s < NSTEPS; s++) {
        if (problem(s, why, sizeof why)) return s;
    }
    return -1;
}

/* A page: lines of titles, notes and items (what can be selected: an id
 * from the R_ ones), built from the choices each time it is drawn. */
enum { E_TITLE = -1, E_NOTE = -2, E_WARN = -3, E_GAP = -4 };
enum {
    R_START_LANG = 1, R_START_KEYMAP, R_START_INSTALL, R_START_TERM, R_START_REBOOT, R_START_OFF,
    R_NET_SETUP, R_NET_COPY, R_NEXT,
    R_DISK_MODE, R_SWAP, R_ESP, R_ROOT, R_SWAP_PART, R_TERMINAL, R_RESCAN,
    R_ENC, R_PASS,
    R_USER, R_FULL_NAME, R_PASSWORD, R_HOST, R_LOCK,
    R_LANG, R_TZ, R_NTP, R_IME,
    R_MACHINE, R_LEVEL, R_CPU, R_RANK, R_UPD_REPO, R_UPD_BRANCH, R_HIBERNATE,
    R_INSTALL,
    R_DISK = 1000, R_MODULE = 2000, R_CONTAINER = 3000, R_REPO = 4000, R_PROBLEM = 5000,
};
struct entry {
    int id;
    char text[512];
};
static struct entry entries[MAX_ENTRIES];
static int nentries;

static void add(int id, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(int id, const char *fmt, ...)
{
    if (nentries == MAX_ENTRIES) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(entries[nentries].text, sizeof entries[0].text, fmt, ap);
    va_end(ap);
    entries[nentries++].id = id;
}

/* An item: its label, padded to the column of the values, and its value. */
static void add_item(int id, const char *label, const char *value)
{
    char pad[160];
    add(id, "%s %s", ui_pad(pad, sizeof pad, label, LABEL_CELLS), value);
}

/* Text over as many lines as it takes, width cells each, broken at spaces
 * where there are some (Chinese has none: anywhere). */
static void add_wrapped(int id, const char *text, int width)
{
    char line[512];
    const char *s = text;
    while (*s) {
        int cells = 0;
        const char *p = s, *space = NULL;
        while (*p) {
            const char *q = p + 1;
            while ((*q & 0xC0) == 0x80) q++;
            char ch[8] = "";
            memcpy(ch, p, (size_t)(q - p) < sizeof ch - 1 ? (size_t)(q - p) : sizeof ch - 1);
            int w = ui_cells(ch);
            if (cells + w > width) break;
            if (*p == ' ') space = p;
            cells += w;
            p = q;
        }
        const char *end = *p && space && space > s ? space : p;
        if (end == s) end = p > s ? p : s + 1;
        snprintf(line, sizeof line, "%.*s", (int)(end - s), s);
        add(id, "%s", line);
        s = end;
        while (*s == ' ') s++;
    }
}

static const char *yes_no(int on)
{
    return on ? _("yes") : _("no");
}

/* "< value >": what Left and Right change. */
static const char *choice(const char *value)
{
    static char buf[4][600];
    static int i;
    i = (i + 1) % 4;
    snprintf(buf[i], sizeof buf[i], "< %s >", value);
    return buf[i];
}

static const char *enc_label(int enc)
{
    return enc == ENC_OFF ? _("off") : enc == ENC_PASS ? _("with a passphrase") : _("passphrase and TPM auto-unlock");
}

static const char *level_label(int i)
{
    static char buf[64];
    if (i == 0) {
        snprintf(buf, sizeof buf, _("auto (%s)"), level_auto);
        return buf;
    }
    return levels[i - 1];
}

static const char *cpu_label(void)
{
    static char buf[96];
    if (strcmp(c.cpu, "auto") == 0) {
        snprintf(buf, sizeof buf, _("auto (%s)"), cpu_model);
        return buf;
    }
    return c.cpu;
}

static const char *zone_label(const char *zone)
{
    return strcmp(zone, "auto") == 0 ? _("automatic (from the network)") : zone;
}

static const char *secret_label(const char *s)
{
    static char buf[64];
    if (!s[0]) return _("not set");
    snprintf(buf, sizeof buf, P_("set (%d character)", "set (%d characters)", (unsigned long)strlen(s)), (int)strlen(s));
    return buf;
}

static void add_next(int step)
{
    char line[160];
    add(E_GAP, " ");
    if (step + 1 < NSTEPS) {
        snprintf(line, sizeof line, _("Next: %s"), _(step_names[step + 1]));
        add(R_NEXT, "%s", line);
    }
}

static void build_network(int width)
{
    struct status_fields st;
    char line[256];
    base_status_fields(&st);
    add(E_TITLE, "%s", _("Network"));
    add_wrapped(E_NOTE, _("The installation downloads its packages: it needs the network."), width);
    snprintf(line, sizeof line, "%s %s", st.network[0] ? st.network : _("not connected"), st.address);
    add_item(E_NOTE, _("Now"), line);
    add_item(R_NET_SETUP, _("Set up Wi-Fi or a wire"), _("Enter: nmtui"));
    snprintf(line, sizeof line, P_("%s (%d network saved here)", "%s (%d networks saved here)", (unsigned long)net_profiles),
             yes_no(c.copy_network), net_profiles);
    add_item(R_NET_COPY, _("Saved networks for the system"), choice(line));
    add_next(S_NETWORK);
}

static void build_disk(int width)
{
    static const char *const modes[] = { N_("the whole of a disk"), N_("partitions chosen here"),
                                         N_("filesystems mounted at /mnt") };
    char line[512], size[32];
    add(E_TITLE, "%s", _("Disk"));
    add_item(R_DISK_MODE, _("Install on"), choice(_(modes[c.disk_mode])));
    if (c.disk_mode == WHOLE) {
        add_wrapped(E_NOTE, _("Everything on the chosen disk is erased; it gets an EFI system partition, swap and the root."),
                    width);
        for (int i = 0; i < ndisks; i++) {
            int on = strcmp(disks[i].path, c.disk) == 0;
            disk_text(&disks[i], line, sizeof line);
            add(R_DISK + i, "(%c) %s%s%s", on ? '*' : ' ', line, disks[i].removable ? _("  removable") : "",
                disks[i].in_use[0] ? _("  in use") : "");
            for (int k = 0; on && k < disks[i].nparts; k++) {
                char part[400];
                part_text(&disks[i].parts[k], part, sizeof part);
                add(E_WARN, _("      erased: %s"), part);
            }
            if (on && disks[i].nparts == 0) add(E_NOTE, "      %s", _("no partitions on it"));
        }
        if (ndisks == 0) add(E_WARN, "%s", _("No disk to install on was found (the live medium is not one)."));
        snprintf(line, sizeof line, P_("%d GiB", "%d GiB", (unsigned long)c.swap_gib), c.swap_gib);
        add_item(R_SWAP, _("Swap"), choice(line));
        human_size(memory_bytes, size, sizeof size);
        snprintf(line, sizeof line, _("Hibernation writes the memory into the swap: this computer has %s."), size);
        add_wrapped(E_NOTE, line, width);
    } else if (c.disk_mode == PARTS) {
        add_item(R_ESP, _("EFI system partition"), c.esp[0] ? c.esp : _("Enter: choose"));
        add_item(R_ROOT, _("Root partition"), c.root[0] ? c.root : _("Enter: choose"));
        add_item(R_SWAP_PART, _("Swap partition"), c.swap[0] ? c.swap : _("none"));
        add_wrapped(E_WARN, _("Each of them is formatted: what is on it is lost."), width);
    } else {
        add_wrapped(E_NOTE, _("Prepared in the terminal: an empty ext4 labelled locachy-root at /mnt, an empty FAT32 at /mnt/efi."),
                    width);
        add_item(R_TERMINAL, _("Terminal"), _("Enter: open it"));
    }
    add_item(R_RESCAN, _("Disks changed"), _("Enter: look again"));
    add_next(S_DISK);
}

static void build_encryption(int width)
{
    add(E_TITLE, "%s", _("Encryption"));
    add_item(R_ENC, _("Encrypt the disk"), choice(enc_label(c.encrypt)));
    if (c.encrypt != ENC_OFF) {
        add_item(R_PASS, _("Passphrase"), secret_label(c.passphrase));
        add_wrapped(E_NOTE, _("Typed at every power-on, with the keyboard as here (the US layout). Ten characters or more."),
                    width);
    }
    add_wrapped(E_NOTE, tpm_advice, width);
    add_next(S_ENCRYPTION);
}

static void build_user(void)
{
    add(E_TITLE, "%s", _("User"));
    add_item(R_USER, _("User name"), c.user);
    add_item(R_FULL_NAME, _("Full name"), c.full_name[0] ? c.full_name : "-");
    add_item(R_PASSWORD, _("Password"), secret_label(c.password));
    add_item(R_HOST, _("Computer name"), c.hostname);
    add_item(R_LOCK, _("Lock the screen at power-on"),
             c.encrypt == ENC_TPM ? _("yes (the TPM opens the disk)") : choice(yes_no(c.boot_lock)));
    add_next(S_USER);
}

static void build_region(void)
{
    add(E_TITLE, "%s", _("Region"));
    add_item(R_LANG, _("Language"), choice(c.lang ? "中文" : "English"));
    add_item(R_TZ, _("Time zone"), zone_label(c.timezone));
    add_item(R_NTP, _("Network time"), choice(yes_no(c.ntp)));
    add_item(R_IME, _("Input method"), choice(nimes ? imes[c.ime].text : "-"));
    add_next(S_REGION);
}

static void build_software(void)
{
    char pad[96];
    add(E_TITLE, "%s", _("Modules"));
    for (int i = 0; i < nmodules; i++) {
        add(R_MODULE + i, "[%c] %s %s", c.module_on[i] ? 'x' : ' ', ui_pad(pad, sizeof pad, modules[i].name, 12),
            modules[i].text);
    }
    add(E_GAP, " ");
    add(E_TITLE, "%s", _("Programs"));
    for (int i = 0; i < ncontainers; i++) {
        add(R_CONTAINER + i, "[%c] %s %s", c.container_on[i] ? 'x' : ' ', ui_pad(pad, sizeof pad, containers[i].name, 12),
            containers[i].flag ? _("a host program") : _("in a container"));
    }
    add_next(S_SOFTWARE);
}

static void build_system(void)
{
    char line[160];
    add(E_TITLE, "%s", _("System"));
    snprintf(line, sizeof line, "%s (%s)", machines[c.machine].name, machines[c.machine].text);
    add_item(R_MACHINE, _("Machine profile"), choice(nmachines ? line : "-"));
    add_item(R_LEVEL, _("Packages' CPU level"), choice(level_label(c.level)));
    add_item(R_CPU, _("Own builds for the CPU"), cpu_label());
    for (int i = 0; i < nrepos; i++) {
        snprintf(line, sizeof line, _("Repository section %s"), repo_names[i]);
        add_item(R_REPO + i, line, choice(yes_no(c.repo_on[i])));
    }
    add_item(R_RANK, _("Rank the mirrors first"), choice(yes_no(c.rank)));
    add_item(R_UPD_REPO, _("Updates from"), c.update_repo);
    add_item(R_UPD_BRANCH, _("Branch"), c.update_branch);
    snprintf(line, sizeof line, P_("%d second", "%d seconds", (unsigned long)c.hibernate), c.hibernate);
    add_item(R_HIBERNATE, _("Hibernate after"), c.hibernate < 0 ? _("as the system has it") : line);
    add_next(S_SYSTEM);
}

static void list_chosen(char *out, size_t size, const struct named *items, const unsigned char *on, int n)
{
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t len = strlen(out);
        if (on[i]) snprintf(out + len, size - len, "%s%s", len ? ", " : "", items[i].name);
    }
    if (!out[0]) snprintf(out, size, "%s", _("none"));
}

/* What will be erased, a line each (for the summary and the confirmation). */
static void add_erased(void)
{
    char line[512];
    if (c.disk_mode == WHOLE) {
        int i = disk_index(c.disk);
        for (int k = 0; i >= 0 && k < disks[i].nparts; k++) {
            part_text(&disks[i].parts[k], line, sizeof line);
            add(E_WARN, _("    erased: %s"), line);
        }
    } else if (c.disk_mode == PARTS) {
        const char *const parts[] = { c.esp, c.root, c.swap };
        for (int k = 0; k < 3; k++) {
            const struct part *p = find_part(parts[k]);
            if (!p) continue;
            part_text(p, line, sizeof line);
            add(E_WARN, _("    formatted: %s"), line);
        }
    }
}

static void build_summary(int width)
{
    char line[1024], list[512], why[256];
    add(E_TITLE, "%s", _("Summary"));
    int problems = 0;
    for (int s = 0; s < NSTEPS; s++) {
        if (problem(s, why, sizeof why)) {
            add(R_PROBLEM + s, "! %s: %s", _(step_names[s]), why);
            problems++;
        }
    }
    if (!problems) add(E_NOTE, "%s", _("Nothing is missing. Install asks once more before it writes anything."));
    add(E_GAP, " ");
    if (c.disk_mode == WHOLE) {
        int i = disk_index(c.disk);
        if (i >= 0) {
            disk_text(&disks[i], list, sizeof list);
            snprintf(line, sizeof line, P_("all of %s; swap %d GiB", "all of %s; swap %d GiB", (unsigned long)c.swap_gib),
                     list, c.swap_gib);
        } else {
            snprintf(line, sizeof line, "%s", _("not chosen"));
        }
    } else if (c.disk_mode == PARTS) {
        snprintf(line, sizeof line, _("EFI %s, root %s, swap %s"), c.esp[0] ? c.esp : "?", c.root[0] ? c.root : "?",
                 c.swap[0] ? c.swap : _("none"));
    } else {
        snprintf(line, sizeof line, "%s", _("the filesystems mounted at /mnt and /mnt/efi"));
    }
    add_item(E_NOTE, _("Disk"), line);
    add_erased();
    add_item(E_NOTE, _("Encryption"), enc_label(c.encrypt));
    snprintf(line, sizeof line, _("%s%s%s%s on %s; lock at power-on: %s"), c.user, c.full_name[0] ? " (" : "",
             c.full_name, c.full_name[0] ? ")" : "", c.hostname, yes_no(c.boot_lock || c.encrypt == ENC_TPM));
    add_item(E_NOTE, _("User"), line);
    snprintf(line, sizeof line, _("%s; keyboard %s; time zone %s; network time %s"), c.lang ? "中文" : "English",
             c.keymap, zone_label(c.timezone), yes_no(c.ntp));
    add_item(E_NOTE, _("Region"), line);
    add_item(E_NOTE, _("Input method"), nimes ? imes[c.ime].text : "-");
    list_chosen(list, sizeof list, modules, c.module_on, nmodules);
    add_item(E_NOTE, _("Modules"), list);
    list_chosen(list, sizeof list, containers, c.container_on, ncontainers);
    add_item(E_NOTE, _("Programs"), list);
    snprintf(line, sizeof line, _("%s; packages for %s; own builds for %s"), nmachines ? machines[c.machine].name : "-",
             level_label(c.level), cpu_label());
    add_item(E_NOTE, _("System"), line);
    list[0] = '\0';
    for (int i = 0; i < nrepos; i++) {
        size_t n = strlen(list);
        if (c.repo_on[i]) snprintf(list + n, sizeof list - n, "%s%s", n ? ", " : "", repo_names[i]);
    }
    snprintf(line, sizeof line, _("%s; mirrors %s"), list[0] ? list : _("none"),
             c.rank ? _("ranked first") : _("as they are"));
    add_item(E_NOTE, _("Repositories"), line);
    snprintf(line, sizeof line, "%s (%s)", c.update_repo, c.update_branch);
    add_item(E_NOTE, _("Updates from"), line);
    add(E_GAP, " ");
    (void)width;
    add(R_INSTALL, "%s", _("Install"));
}

static void build_step(int step, int width)
{
    nentries = 0;
    switch (step) {
    case S_NETWORK: build_network(width); break;
    case S_DISK: build_disk(width); break;
    case S_ENCRYPTION: build_encryption(width); break;
    case S_USER: build_user(); break;
    case S_REGION: build_region(); break;
    case S_SOFTWARE: build_software(); break;
    case S_SYSTEM: build_system(); break;
    default: build_summary(width); break;
    }
}

static void build_start(int width)
{
    nentries = 0;
    add(E_TITLE, "%s", _("Install LOCachy on this computer"));
    add(E_GAP, " ");
    add_item(R_START_LANG, _("Language"), choice(ui_lang ? "中文" : "English"));
    add_item(R_START_KEYMAP, _("Keyboard layout"), c.keymap);
    add(E_GAP, " ");
    add(R_START_INSTALL, "%s", _("Install LOCachy"));
    add(R_START_TERM, "%s", _("Terminal (a root shell, for repairs)"));
    add(R_START_REBOOT, "%s", _("Reboot"));
    add(R_START_OFF, "%s", _("Power off"));
    add(E_GAP, " ");
    add_wrapped(E_NOTE, _("The terminal is a root shell on this live system: nmtui, cryptsetup, arch-chroot, "
                          "locachy-install --help. exit comes back here."), width);
}

/* The pointer: no process 1 lends the devices here, so they are opened
 * as it would lend them. */
static void take_pointer(void)
{
    char paths[32][POINTER_PATH];
    size_t n = 0;
    pointer_open();
    int *fds = pointer_lend(&n, paths, 32);
    for (size_t i = 0; i < n; i++) pointer_take(fds[i], paths[i]);
    free(fds);
}

/* Another program on the screen (term, nmtui in term): this one gives the
 * screen and the input up, waits for it, and takes them back. */
static int run_on_screen(char *const argv[])
{
    pointer_release();
    keys_release();
    screen_release();
    const char *what = argv[1] ? argv[1] : argv[0];
    say("installer: running %s", what);
    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    int status = -1;
    while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    screen_open(BASE_FONT);
    keys_open();
    take_pointer();
    int code = pid > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    say("installer: %s ended %d", what, code);
    return code;
}

static void terminal(void)
{
    char *const argv[] = { TERM_CMD, NULL };
    run_on_screen(argv);
}

/* The keys that edit a line of text (as packages' fields): Left/Right,
 * Home/End (^A/^E), Backspace, Delete (^D), ^U, ^K, ^W; what keys type
 * (keys_typed), as the layout chosen on the start page types it. */
static int edit_line_key(int key, char *text, size_t size, size_t *pos)
{
    size_t len = strlen(text);
    const char *typed;
    if (*pos > len) *pos = len;
    if (key == K_LEFT) {
        while (*pos > 0 && (((unsigned char)text[--*pos]) & 0xC0) == 0x80) {
        }
    } else if (key == K_RIGHT) {
        if (*pos < len) {
            (*pos)++;
            while (*pos < len && ((unsigned char)text[*pos] & 0xC0) == 0x80) (*pos)++;
        }
    } else if (key == K_BOL || key == 1) {
        *pos = 0;
    } else if (key == K_EOL || key == 5) {
        *pos = len;
    } else if (key == '\b' || key == 127) {
        if (*pos > 0) {
            size_t prev = *pos - 1;
            while (prev > 0 && ((unsigned char)text[prev] & 0xC0) == 0x80) prev--;
            memmove(text + prev, text + *pos, len - *pos + 1);
            *pos = prev;
        }
    } else if (key == K_DEL || key == 4) {
        if (*pos < len) {
            size_t next = *pos + 1;
            while (next < len && ((unsigned char)text[next] & 0xC0) == 0x80) next++;
            memmove(text + *pos, text + next, len - next + 1);
        }
    } else if (key == 21) { /* Ctrl+U */
        memmove(text, text + *pos, len - *pos + 1);
        *pos = 0;
    } else if (key == 11) { /* Ctrl+K */
        text[*pos] = '\0';
    } else if (key == 23) { /* Ctrl+W */
        size_t prev = *pos;
        while (prev > 0 && text[prev - 1] == ' ') prev--;
        while (prev > 0 && text[prev - 1] != ' ') prev--;
        memmove(text + prev, text + *pos, len - *pos + 1);
        *pos = prev;
    } else if ((typed = keys_typed(key)) != NULL && len + strlen(typed) < size) {
        size_t n = strlen(typed);
        memmove(text + *pos + n, text + *pos, len - *pos + 1);
        memcpy(text + *pos, typed, n);
        *pos += n;
    } else {
        return 0;
    }
    return 1;
}

/* A line typed at the bottom, starting as text (hidden: shown as stars):
 * 1 with Enter, 0 with Esc (text as it was). */
static int prompt(const char *label, char *text, size_t size, int hidden)
{
    char line[1024], was[512], shown[512];
    size_t pos = strlen(text);
    copy(was, sizeof was, text);
    for (;;) {
        size_t len = strlen(text);
        if (hidden) {
            memset(shown, '*', len < sizeof shown - 1 ? len : sizeof shown - 1);
            shown[len < sizeof shown - 1 ? len : sizeof shown - 1] = '\0';
        } else {
            copy(shown, sizeof shown, text);
        }
        snprintf(line, sizeof line, "%s %.*s_%s", label, (int)pos, shown, shown + pos);
        screen_fill(0, screen_rows() - 1, screen_cols(), 1, BG);
        screen_text(1, screen_rows() - 1, line, ACCENT, BG);
        screen_flush();
        int key = key_get();
        if (key == 27) {
            copy(text, size, was);
            explicit_bzero(was, sizeof was);
            return 0;
        }
        if (key == '\n') {
            explicit_bzero(was, sizeof was);
            return 1;
        }
        edit_line_key(key, text, size, &pos);
    }
}

/* A secret typed twice: 1 and it in out when both are the same. */
static int prompt_secret(const char *label, const char *again, char *out, size_t size)
{
    char one[256] = "", two[256] = "";
    int ok = prompt(label, one, sizeof one, 1) && one[0] && prompt(again, two, sizeof two, 1);
    if (ok && strcmp(one, two) != 0) {
        tell(1, "%s", _("The two were not the same; nothing changed."));
        ok = 0;
    } else if (ok) {
        copy(out, size, one);
        tell(0, "%s", "");
    }
    explicit_bzero(one, sizeof one);
    explicit_bzero(two, sizeof two);
    return ok;
}

static void screen_title(const char *title)
{
    screen_fill(0, 0, screen_cols(), screen_rows(), BG);
    screen_bar(_("installer"), NULL);
    screen_text(2, 2, title, ACCENT, BG);
}

/* One of n items, filtered by what is typed (as settings' time zones):
 * its index, or -1 (Esc). */
static int pick(const char *title, const char *const *items, int n, int current)
{
    static int match[MAX_LIST];
    char filter[48] = "", line[512];
    int sel = -1, top = 0;
    for (;;) {
        int m = 0, list = 5, height = screen_rows() - list - 2;
        for (int i = 0; i < n && m < MAX_LIST; i++) {
            if (!filter[0] || strcasestr(items[i], filter)) {
                if (sel < 0 && i == current) sel = m;
                match[m++] = i;
            }
        }
        sel = sel < 0 ? 0 : sel >= m ? (m > 0 ? m - 1 : 0) : sel;
        top = sel < top ? sel : sel >= top + height ? sel - height + 1 : top;
        screen_title(title);
        snprintf(line, sizeof line, _("Filter: %s_   (type to filter; Enter: choose; Esc: back)"), filter);
        screen_text(2, 3, line, FG, BG);
        if (m == 0) screen_text(4, list, _("Nothing has that in its name."), DIM, BG);
        for (int k = 0; k < height && top + k < m; k++) {
            int on = top + k == sel;
            screen_fill(2, list + k, screen_cols() - 4, 1, on ? ACCENT : BG);
            screen_text(4, list + k, items[match[top + k]], on ? BG : FG, on ? ACCENT : BG);
        }
        screen_flush();
        int key = key_get();
        size_t len = strlen(filter);
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (e->wheel) {
                sel -= e->wheel;
            } else if (e->button == BTN_LEFT && e->pressed && e->row >= list && e->row - list < height &&
                       top + e->row - list < m) {
                sel = top + e->row - list;
                if (e->clicks == 2) return match[sel];
            }
        } else if (key == 27) {
            return -1;
        } else if (key == '\n') {
            if (m > 0) return match[sel];
        } else if (key == K_UP) {
            sel--;
        } else if (key == K_DOWN) {
            sel++;
        } else if (key == K_PGUP) {
            sel -= height;
        } else if (key == K_PGDN) {
            sel += height;
        } else if ((key == '\b' || key == 127) && len > 0) {
            while (len > 1 && ((unsigned char)filter[len - 1] & 0xC0) == 0x80) len--; /* a whole character */
            filter[len - 1] = '\0';
        } else if (keys_typed(key) && len + strlen(keys_typed(key)) < sizeof filter) {
            strcat(filter, keys_typed(key));
            sel = 0;
        }
        if (sel < 0) sel = 0;
    }
}

/* A partition, from every disk's: its path into out ("" with none). */
static void pick_part(const char *title, char *out, size_t size, int none)
{
    static char texts[MAX_DISKS * MAX_PARTS + 1][400];
    static const char *items[MAX_DISKS * MAX_PARTS + 1];
    static const struct part *parts[MAX_DISKS * MAX_PARTS + 1];
    int n = 0, current = 0;
    if (none) {
        copy(texts[n], sizeof texts[0], _("none"));
        parts[n++] = NULL;
    }
    for (int i = 0; i < ndisks; i++) {
        for (int k = 0; k < disks[i].nparts; k++) {
            part_text(&disks[i].parts[k], texts[n], sizeof texts[0]);
            if (strcmp(disks[i].parts[k].path, out) == 0) current = n;
            parts[n++] = &disks[i].parts[k];
        }
    }
    for (int i = 0; i < n; i++) items[i] = texts[i];
    if (n == 0) {
        tell(1, "%s", _("No partitions were found: make them in the terminal, then look again."));
        return;
    }
    int got = pick(title, items, n, current);
    if (got >= 0) copy(out, size, parts[got] ? parts[got]->path : "");
}

static int yes(const char *question)
{
    char line[512];
    int row = screen_rows() - 1;
    snprintf(line, sizeof line, "%s [y/N] ", question);
    screen_fill(0, row, screen_cols(), 1, BG);
    screen_text(1, row, line, ACCENT, BG);
    /* TRANSLATORS: Y and N are the keys; keep "Y: " and "N: ". */
    int col = ui_button(2 + ui_cells(line), row, _("Y: yes"), 'y');
    ui_button(col, row, _("N: no"), 'n');
    screen_flush();
    for (;;) {
        int key = key_get();
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (!e->pressed) continue;
            key = ui_hit_at(e->col, e->row);
        }
        if (key == 0) continue;
        return key == 'y' || key == 'Y';
    }
}

/* locachy-install's options for what is chosen. */
struct args {
    char *v[96];
    int n;
    char buf[8192];
    size_t used;
};

static void arg(struct args *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void arg(struct args *a, const char *fmt, ...)
{
    va_list ap;
    if (a->n + 1 >= (int)(sizeof a->v / sizeof a->v[0])) return;
    va_start(ap, fmt);
    int len = vsnprintf(a->buf + a->used, sizeof a->buf - a->used, fmt, ap);
    va_end(ap);
    if (len < 0 || a->used + (size_t)len + 1 > sizeof a->buf) return;
    a->v[a->n++] = a->buf + a->used;
    a->v[a->n] = NULL;
    a->used += (size_t)len + 1;
}

static void list_arg(struct args *a, const char *option, const struct named *items, const unsigned char *on, int n)
{
    char list[1024] = "";
    for (int i = 0; i < n; i++) {
        size_t len = strlen(list);
        if (on[i]) snprintf(list + len, sizeof list - len, "%s%s", len ? "," : "", items[i].name);
    }
    arg(a, "%s=%s", option, list);
}

/* The options, the secrets written for them: 0, or -1. */
static int install_args(struct args *a, int check)
{
    char path[512];
    a->n = 0;
    a->used = 0;
    arg(a, "%s", INSTALL_CMD);
    arg(a, "%s", check ? "--check" : "--yes");
    if (c.disk_mode == WHOLE) {
        arg(a, "--disk=%s", c.disk);
        arg(a, "--swap-gib=%d", c.swap_gib);
    } else if (c.disk_mode == PARTS) {
        arg(a, "--format-esp=%s", c.esp);
        arg(a, "--format-root=%s", c.root);
        if (c.swap[0]) arg(a, "--format-swap=%s", c.swap);
    } else {
        arg(a, "--root=/mnt");
        arg(a, "--esp=/mnt/efi");
    }
    if (c.encrypt != ENC_OFF) {
        if (write_secret("passphrase", c.passphrase, path, sizeof path) < 0) return -1;
        arg(a, "--encrypt");
        if (c.encrypt == ENC_TPM) arg(a, "--tpm");
        arg(a, "--luks-passphrase-file=%s", path);
    }
    if (write_secret("password", c.password, path, sizeof path) < 0) return -1;
    arg(a, "--password-file=%s", path);
    arg(a, "--user=%s", c.user);
    if (c.full_name[0]) arg(a, "--full-name=%s", c.full_name);
    arg(a, "--hostname=%s", c.hostname);
    if (!c.boot_lock && c.encrypt != ENC_TPM) arg(a, "--login-skip");
    arg(a, "--language=%s", languages[c.lang]);
    arg(a, "--keymap=%s", c.keymap);
    arg(a, "--timezone=%s", c.timezone);
    if (!c.ntp) arg(a, "--no-ntp");
    if (nimes) arg(a, "--ime=%s", imes[c.ime].name);
    arg(a, "%s", c.copy_network ? "--copy-network" : "--no-copy-network");
    list_arg(a, "--features", modules, c.module_on, nmodules);
    list_arg(a, "--containers", containers, c.container_on, ncontainers);
    if (c.machine > 0) arg(a, "--machine=%s", machines[c.machine].name);
    arg(a, "--level=%s", c.level == 0 ? "auto" : levels[c.level - 1]);
    arg(a, "--cpu=%s", c.cpu);
    char repos[64] = "";
    for (int i = 0; i < nrepos; i++) {
        size_t len = strlen(repos);
        if (c.repo_on[i]) snprintf(repos + len, sizeof repos - len, "%s%s", len ? "," : "", repo_names[i]);
    }
    arg(a, "--repos=%s", repos);
    if (!c.rank) arg(a, "--no-rank-mirrors");
    arg(a, "--update-repo=%s", c.update_repo);
    arg(a, "--update-branch=%s", c.update_branch);
    if (c.hibernate >= 0) arg(a, "--hibernate-after=%d", c.hibernate);
    return 0;
}

/* The installation, a job of its own (its session, its output in the log):
 * it goes on while the terminal is open. Its stages, from what it prints. */
enum { G_DISK, G_MIRRORS, G_PACKAGES, G_HELPERS, G_OWN, G_FINISH, NSTAGES };
static const char *const stage_names[NSTAGES] = {
    N_("Partition and format"), N_("Rank the mirrors"), N_("Download and install the packages"),
    N_("Package helpers and the AUR"), N_("Build LOCachy's own programs"), N_("User, settings and boot files"),
};
static pid_t job;
static int stage = -1, completed, pkg_done, pkg_fetched, pkg_total;
static double started, stage_at[NSTAGES], ended;
static char last_line[256], error_line[512];
static off_t log_read;
static char log_part[512];

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void log_path(char *out, size_t size)
{
    snprintf(out, size, "%s/install.log", RUN_DIR);
}

static void stage_go(int s)
{
    if (s > stage) {
        stage = s;
        stage_at[s] = now();
    }
}

static void progress_line(const char *s)
{
    int n, m;
    if (strncmp(s, "Ranking the mirrors", 19) == 0) {
        stage_go(G_MIRRORS);
    } else if (strncmp(s, "== sources and package helpers", 30) == 0) {
        stage_go(G_HELPERS);
    } else if (strncmp(s, "== our part", 11) == 0) {
        stage_go(G_OWN);
    } else if (strncmp(s, "== ", 3) == 0) {
        stage_go(G_PACKAGES); /* make-root.sh's first steps: the keys, the packages */
    } else if (strncmp(s, "make-root: target built", 23) == 0) {
        stage_go(G_FINISH);
    } else if (strncmp(s, "Installation completed.", 23) == 0) {
        completed = 1;
    } else if (sscanf(s, "(%d/%d) ", &n, &m) == 2 && strstr(s, ") installing ")) {
        pkg_done = n;
        pkg_total = m;
    } else if (sscanf(s, "(%d/%d) ", &n, &m) == 2 && strstr(s, ") downloading ")) {
        pkg_fetched = n;
        pkg_total = m;
    }
    if (strncmp(s, "locachy-install: ", 17) == 0 || strncmp(s, "make-root: ", 11) == 0) {
        copy(error_line, sizeof error_line, s);
    }
    if (s[0]) copy(last_line, sizeof last_line, s);
}

/* What the installation printed since the last look. */
static void read_progress(void)
{
    char path[512], buf[16384];
    log_path(path, sizeof path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t got;
    while ((got = pread(fd, buf, sizeof buf, log_read)) > 0) {
        log_read += got;
        for (ssize_t i = 0; i < got; i++) {
            size_t len = strlen(log_part);
            if (buf[i] == '\n' || buf[i] == '\r') {
                progress_line(log_part);
                log_part[0] = '\0';
            } else if (len + 1 < sizeof log_part) {
                log_part[len] = buf[i];
                log_part[len + 1] = '\0';
            }
        }
    }
    close(fd);
}

static void start_install(char *const argv[])
{
    char path[512];
    log_path(path, sizeof path);
    mkdir(RUN_DIR, 0700);
    unlink(path);
    int out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) {
        tell(1, _("Cannot write %s."), path);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
        dup2(null, STDIN_FILENO); /* nothing waits for an answer: a question would end it */
        dup2(out, STDOUT_FILENO);
        dup2(out, STDERR_FILENO);
        setenv("PYTHONUNBUFFERED", "1", 1); /* its lines as they come, through make-root.sh and the chroot */
        execv(argv[0], argv);
        _exit(127);
    }
    close(out);
    if (pid < 0) {
        tell(1, "%s", _("Cannot start locachy-install."));
        return;
    }
    job = pid;
    stage = c.disk_mode == MOUNTED ? -1 : G_DISK;
    memset(stage_at, 0, sizeof stage_at);
    started = now();
    if (stage == G_DISK) stage_at[G_DISK] = started;
    completed = pkg_done = pkg_fetched = pkg_total = 0;
    last_line[0] = error_line[0] = log_part[0] = '\0';
    log_read = 0;
    tell(0, "%s", "");
    say("installer: installing");
}

/* The installation ended: 0 done, 1 failed; -1 still going. */
static int check_install(void)
{
    int status;
    read_progress();
    if (!job || waitpid(job, &status, WNOHANG) != job) return -1;
    job = 0;
    ended = now();
    read_progress();
    remove_secrets();
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && completed;
    if (ok) {
        say("installer: installation completed");
    } else {
        say("installer: installation failed (%d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    return ok ? 0 : 1;
}

/* --check with what is chosen: 1 if locachy-install takes it, else 0 and
 * why in the notice. */
static int check_choices(void)
{
    static struct args a;
    char out[8192];
    if (install_args(&a, 1) < 0) {
        tell(1, _("Cannot write %s."), RUN_DIR);
        return 0;
    }
    screen_fill(0, screen_rows() - 1, screen_cols(), 1, BG);
    screen_text(1, screen_rows() - 1, _("Checking the choices with locachy-install --check..."), ACCENT, BG);
    screen_flush();
    int status = capture(a.v, out, sizeof out, 1);
    if (status == 0) return 1;
    remove_secrets();
    /* Its last line says why (argparse: "locachy-install: error: ..."). */
    char *end = out + strlen(out);
    while (end > out && (end[-1] == '\n' || end[-1] == ' ')) *--end = '\0';
    char *line = strrchr(out, '\n');
    tell(1, _("locachy-install does not take it: %s"), line ? line + 1 : out[0] ? out : "?");
    return 0;
}

/* The last word before anything is written, on a screen of its own. */
static int confirm_install(void)
{
    char line[512];
    int width = screen_cols() - 6;
    nentries = 0;
    if (c.disk_mode == WHOLE) {
        int i = disk_index(c.disk);
        disk_text(&disks[i], line, sizeof line);
        add(E_TITLE, _("Erase %s and install LOCachy on it?"), c.disk);
        add(E_NOTE, "%s", line);
    } else if (c.disk_mode == PARTS) {
        add(E_TITLE, "%s", _("Format these partitions and install LOCachy on them?"));
    } else {
        add(E_TITLE, "%s", _("Install LOCachy into the filesystems mounted at /mnt?"));
    }
    add_erased();
    add(E_GAP, " ");
    add_wrapped(E_NOTE, _("This cannot be undone. Y erases and installs; N goes back to the summary."), width);
    screen_title("");
    for (int i = 0; i < nentries && 3 + i < screen_rows() - 2; i++) {
        uint32_t fg = entries[i].id == E_TITLE ? ACCENT : entries[i].id == E_WARN ? ERROR : FG;
        screen_text(3, 3 + i, entries[i].text, fg, BG);
    }
    return yes(_("Erase and install?"));
}

static void power(const char *how)
{
    char *const argv[] = { SYSTEMCTL, (char *)how, NULL };
    char out[512];
    remove_secrets();
    say("installer: %s", how);
    if (capture(argv, out, sizeof out, 1) != 0) tell(1, "systemctl %s: %s", how, out);
}

/* Where the user is: the start, a step (its page, or the sidebar), the
 * installation running, done or failed. */
enum { M_START, M_STEPS, M_RUN, M_DONE, M_FAILED };
static int mode = M_START, step, in_page = 1, selected, scroll, log_from;
#define PAGE_TOP 2
#define HIT_STEP   10000 /* + the step */
#define HIT_KEY    20000 /* + the key a button stands for */
#define HIT_PANE   30000

static int sidebar_width(void)
{
    if (mode != M_STEPS) return 0;
    int w = 0;
    for (int s = 0; s < NSTEPS; s++) {
        int n = ui_cells(_(step_names[s]));
        w = n > w ? n : w;
    }
    return w + 4;
}

static int page_x(void)
{
    int sw = sidebar_width();
    return sw ? sw + 2 : 2;
}

static int page_rows(void)
{
    return screen_rows() - 3 - PAGE_TOP;
}

static void build(void)
{
    int width = screen_cols() - page_x() - 4;
    if (mode == M_START) {
        build_start(width);
    } else {
        build_step(step, width);
    }
}

static int selectable(int k)
{
    return k >= 0 && k < nentries && entries[k].id > 0;
}

/* The entry of the selected item: kept on an item (the first one when it is
 * no longer there), and in view. */
static int entry_of_selected(void)
{
    for (int k = 0; k < nentries; k++) {
        if (selectable(k) && entries[k].id == selected) return k;
    }
    for (int k = 0; k < nentries; k++) {
        if (selectable(k)) {
            selected = entries[k].id;
            return k;
        }
    }
    return -1;
}

static void keep_in_view(void)
{
    int k = entry_of_selected(), rows = page_rows();
    if (k >= 0) {
        int top = k;
        while (top > 0 && !selectable(top - 1) && k - top < 2) top--; /* its title too */
        if (top < scroll) scroll = top;
        if (k >= scroll + rows) scroll = k - rows + 1;
    }
    if (scroll > nentries - rows) scroll = nentries - rows;
    if (scroll < 0) scroll = 0;
}

static void draw_entries(int px)
{
    int width = screen_cols() - px;
    keep_in_view();
    for (int r = 0; r < page_rows() && scroll + r < nentries; r++) {
        const struct entry *e = &entries[scroll + r];
        int row = PAGE_TOP + r;
        if (e->id == E_TITLE) {
            screen_text(px + 1, row, e->text, ACCENT, BG);
        } else if (e->id == E_NOTE || e->id == E_GAP) {
            screen_text(px + 2, row, e->text, DIM, BG);
        } else if (e->id == E_WARN) {
            screen_text(px + 2, row, e->text, ERROR, BG);
        } else {
            int on = e->id == selected && (in_page || mode == M_START);
            uint32_t fg = e->id >= R_PROBLEM ? ERROR : FG, bg = on ? ACCENT : BG;
            screen_fill(px, row, width, 1, bg);
            screen_text(px + 2, row, e->text, on ? BG : fg, bg);
            ui_hit(px, row, width, 1, e->id);
        }
    }
}

static void draw_sidebar(int width)
{
    char why[256], line[160];
    for (int s = 0; s < NSTEPS && PAGE_TOP + s < screen_rows() - 2; s++) {
        int on = s == step, bad = problem(s, why, sizeof why);
        uint32_t bg = on ? (in_page ? SIDE_BG : ACCENT) : BG;
        snprintf(line, sizeof line, "%c %s", bad ? '!' : ' ', _(step_names[s]));
        screen_fill(0, PAGE_TOP + s, width, 1, bg);
        screen_text(1, PAGE_TOP + s, line, on && !in_page ? BG : bad ? ERROR : FG, bg);
        ui_hit(0, PAGE_TOP + s, width, 1, HIT_STEP + s);
    }
    screen_fill(width, PAGE_TOP, 1, screen_rows() - 2 - PAGE_TOP, SIDE_BG);
}

static void elapsed(double seconds, char *out, size_t size)
{
    int s = (int)seconds;
    if (s >= 3600) {
        snprintf(out, size, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    } else {
        snprintf(out, size, "%d:%02d", s / 60, s % 60);
    }
}

/* The last lines of the log (a line rewritten with \r: its last form), n
 * at most, skipping from from its end; how many there are (of its last
 * 64 KiB) into total. */
static int log_lines(char lines[][256], int n, int from, int *total)
{
    static char buf[65536];
    char path[512];
    log_path(path, sizeof path);
    *total = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    off_t end = lseek(fd, 0, SEEK_END);
    off_t start = end > (off_t)sizeof buf - 1 ? end - (off_t)sizeof buf + 1 : 0;
    ssize_t got = pread(fd, buf, sizeof buf - 1, start);
    close(fd);
    if (got <= 0) return 0;
    buf[got] = '\0';
    static char *all[4096];
    int count = 0;
    for (char *s = strtok(buf, "\n"); s && count < 4096; s = strtok(NULL, "\n")) {
        char *cr = strrchr(s, '\r');
        while (cr && cr[1] == '\0' && cr > s) {
            *cr = '\0';
            cr = strrchr(s, '\r');
        }
        all[count++] = cr ? cr + 1 : s;
    }
    *total = count;
    int last = count - from, first = last - n < 0 ? 0 : last - n, k = 0;
    for (int i = first; i < last && i >= 0; i++) snprintf(lines[k++], 256, "%s", all[i]);
    return k;
}

static uint32_t log_color(const char *s)
{
    if (strncmp(s, "==", 2) == 0 || strncmp(s, "::", 2) == 0) return ACCENT;
    if (strcasestr(s, "error") || strcasestr(s, "failed") || strcasestr(s, "fatal")) return ERROR;
    return strcasestr(s, "warning") ? WARN : FG;
}

/* The installation: what it is on, its stages, then its output in a pane
 * of its own. */
static void draw_progress(void)
{
    char line[1024], t[32];
    int cols = screen_cols(), rows = screen_rows(), r = PAGE_TOP;
    double until = job ? now() : ended;
    elapsed(until - started, t, sizeof t);
    if (mode == M_RUN) {
        snprintf(line, sizeof line, _("Installing LOCachy   %s"), t);
        screen_text(2, r, line, ACCENT, BG);
    } else if (mode == M_DONE) {
        snprintf(line, sizeof line, _("LOCachy is installed (%s). Take the installation medium out, then reboot."), t);
        screen_text(2, r, line, ACCENT, BG);
    } else {
        screen_text(2, r, _("The installation failed. Nothing more is written; the output below says why."), ERROR, BG);
        if (error_line[0]) screen_text(4, ++r, error_line, ERROR, BG);
    }
    r += 2;
    for (int s = 0; s < NSTAGES; s++, r++) {
        int done = stage > s || (mode == M_DONE && stage_at[s] > 0), now_on = stage == s && mode == M_RUN;
        int skipped = stage_at[s] == 0 && (stage > s || mode == M_DONE);
        double from = stage_at[s], to = 0;
        for (int k = s + 1; k < NSTAGES && !to; k++) to = stage_at[k];
        if (!to) to = until;
        t[0] = '\0';
        if (from > 0 && !skipped) elapsed(to - from, t, sizeof t);
        snprintf(line, sizeof line, "[%s] %s", skipped ? "-" : done ? "x" : now_on ? ">" : " ", _(stage_names[s]));
        char pad[160];
        ui_pad(pad, sizeof pad, line, 44);
        if (s == G_PACKAGES && pkg_total) {
            snprintf(line, sizeof line, _("%s %8s   fetched %d, installed %d of %d"), pad, t, pkg_fetched, pkg_done, pkg_total);
        } else {
            snprintf(line, sizeof line, "%s %8s", pad, t);
        }
        screen_text(4, r, line, skipped ? DIM : now_on ? ACCENT : stage == s && mode == M_FAILED ? ERROR : FG, BG);
    }
    r++;
    int top = r, height = rows - 2 - top;
    if (height < 3) return;
    char lines[128][256];
    int total, n = log_lines(lines, height - 1 < 128 ? height - 1 : 128, log_from, &total);
    char where[64] = "";
    if (n) snprintf(where, sizeof where, _("lines %d-%d of %d"), total - log_from - n + 1, total - log_from, total);
    screen_fill(0, top, cols, height, PANE);
    screen_fill(0, top, cols, 1, DIM);
    char logp[512];
    log_path(logp, sizeof logp);
    /* TRANSLATORS: PgUp/PgDn are key names; keep them. */
    snprintf(line, sizeof line, _("Output: %s   %s   PgUp/PgDn: scroll"), logp, where);
    screen_text(1, top, line, BG, DIM);
    ui_hit(0, top, cols, height, HIT_PANE);
    for (int i = 0; i < n; i++) screen_text(2, top + 1 + i, lines[i], log_color(lines[i]), PANE);
    if (log_from > total) log_from = total;
}

/* The buttons at the bottom: label, the key it stands for. */
static void draw_buttons(void)
{
    struct button {
        const char *label;
        int key;
    };
    /* TRANSLATORS: the part before the colon is a key; keep it and ": ". */
    static const struct button start[] = { { N_("Enter: choose"), '\n' }, { NULL, 0 } };
    static const struct button steps[] = { { N_("Tab: steps"), '\t' }, { N_("PgUp: back"), K_PGUP },
                                           { N_("PgDn: next"), K_PGDN }, { N_("Esc: start"), 27 }, { NULL, 0 } };
    static const struct button run[] = { { N_("t: terminal"), 't' }, { NULL, 0 } };
    static const struct button done[] = { { N_("r: reboot"), 'r' }, { N_("p: power off"), 'p' },
                                          { N_("t: terminal"), 't' }, { NULL, 0 } };
    static const struct button failed[] = { { N_("t: terminal"), 't' }, { N_("b: back to the summary"), 'b' },
                                            { N_("r: reboot"), 'r' }, { N_("p: power off"), 'p' }, { NULL, 0 } };
    const struct button *b = mode == M_START ? start : mode == M_STEPS ? steps : mode == M_RUN ? run :
                             mode == M_DONE ? done : failed;
    int x = 1;
    for (; b->label; b++) x = ui_button(x, screen_rows() - 2, _(b->label), HIT_KEY + b->key);
}

static void draw(void)
{
    int sw = sidebar_width();
    ui_hits_clear();
    screen_fill(0, 0, screen_cols(), screen_rows(), BG);
    screen_bar(_("installer"), NULL);
    if (mode == M_START || mode == M_STEPS) {
        build();
        if (sw) draw_sidebar(sw);
        draw_entries(page_x());
    } else {
        draw_progress();
    }
    draw_buttons();
    screen_text(1, screen_rows() - 1, notice, notice_error ? ERROR : ACCENT, BG);
    screen_flush();
}

static void set_ui_language(void)
{
    setenv("LOCACHY_LANG", languages[ui_lang], 1);
    base_language_reload();
}

static void go_step(int s)
{
    step = s < 0 ? 0 : s >= NSTEPS ? NSTEPS - 1 : s;
    in_page = 1;
    selected = scroll = 0;
}

/* The description again: the disks or the networks may have changed (in
 * the terminal, in nmtui). What was chosen and is gone is chosen no more. */
static void reread(void)
{
    screen_title(_("Looking at this computer: its disks, its TPM, its CPU..."));
    screen_flush();
    int first = !described;
    if (load_description() < 0) {
        tell(1, "%s", _("locachy-install --describe-json failed; the terminal shows why."));
        return;
    }
    if (first) set_defaults();
    if (disk_index(c.disk) < 0) c.disk[0] = '\0';
    if (!find_part(c.esp)) c.esp[0] = '\0';
    if (!find_part(c.root)) c.root[0] = '\0';
    if (!find_part(c.swap)) c.swap[0] = '\0';
}

/* The layout chosen: the installer types as it does from now on, as the
 * installed system will (its disk passphrase prompt too), with the key table
 * keytable writes for it; the live system's, so the rescue terminal does too.
 * A passphrase or password typed with another layout is set again. */
static void choose_keymap(void)
{
    static const char *items[MAX_LIST];
    int current = 0;
    char next[512], out[512];
    if (!described) reread();
    for (int i = 0; i < nkeymaps; i++) {
        items[i] = keymaps[i];
        if (strcmp(keymaps[i], c.keymap) == 0) current = i;
    }
    int got = pick(_("Keyboard layout: this installer, the disk passphrase at boot and the system type with it"),
                   items, nkeymaps, current);
    if (got < 0 || strcmp(keymaps[got], c.keymap) == 0) return;
    snprintf(next, sizeof next, "%s.next", KEYMAP_TABLE);
    char *const argv[] = { KEYTABLE, keymaps[got], next, NULL };
    if (capture(argv, out, sizeof out, 1) != 0 || keys_layout_load(next) < 0) {
        tell(1, _("No key table for %s: %s"), keymaps[got], out);
        remove(next);
        keys_layout_load(KEYMAP_TABLE); /* the layout before (US, without its table) */
        return;
    }
    rename(next, KEYMAP_TABLE);
    copy(c.keymap, sizeof c.keymap, keymaps[got]);
    say("installer: keymap %s", c.keymap);
    if (c.passphrase[0] || c.password[0]) {
        memset(c.passphrase, 0, sizeof c.passphrase);
        memset(c.password, 0, sizeof c.password);
        tell(0, "%s", _("The passphrase and the password were typed with the other layout: set them again."));
    }
}

static void begin_install(void)
{
    static struct args a;
    char why[256];
    int s = first_problem();
    if (s >= 0) {
        problem(s, why, sizeof why);
        tell(1, "%s: %s", _(step_names[s]), why);
        go_step(s);
        return;
    }
    if (!check_choices()) return;
    if (!confirm_install()) {
        remove_secrets();
        tell(0, "%s", _("Nothing was written."));
        return;
    }
    if (install_args(&a, 0) < 0) {
        tell(1, _("Cannot write %s."), RUN_DIR);
        return;
    }
    start_install(a.v);
    if (job) {
        mode = M_RUN;
        log_from = 0;
    }
}

/* The rows Left and Right change in place (elsewhere Left goes to the
 * steps). */
static int is_choice(int id)
{
    return id == R_START_LANG || id == R_NET_COPY || id == R_DISK_MODE || id == R_SWAP || id == R_ENC ||
           id == R_LOCK || id == R_LANG || id == R_NTP || id == R_IME || id == R_MACHINE || id == R_LEVEL ||
           id == R_RANK || (id >= R_REPO && id < R_REPO + 4);
}

static void act(int id, int key)
{
    int dir = key == K_LEFT ? -1 : 1;
    char text[256];
    switch (id) {
    case R_START_LANG:
        ui_lang = !ui_lang;
        set_ui_language();
        if (!c.lang_touched) {
            c.lang = ui_lang;
            follow_language();
        }
        break;
    case R_START_KEYMAP:
        choose_keymap();
        break;
    case R_START_INSTALL:
        if (!described) reread();
        if (described) {
            mode = M_STEPS;
            go_step(S_NETWORK);
        }
        break;
    case R_START_TERM:
    case R_TERMINAL:
        terminal();
        reread();
        break;
    case R_START_REBOOT:
        if (yes(_("Reboot now?"))) power("reboot");
        break;
    case R_START_OFF:
        if (yes(_("Power off now?"))) power("poweroff");
        break;
    case R_NEXT:
        go_step(step + 1);
        break;
    case R_NET_SETUP: {
        char *const argv[] = { TERM_CMD, NMTUI, NULL };
        run_on_screen(argv);
        reread();
        break;
    }
    case R_NET_COPY:
        c.copy_network = !c.copy_network;
        break;
    case R_DISK_MODE:
        c.disk_mode = (c.disk_mode + 3 + dir) % 3;
        break;
    case R_SWAP:
        if (key == K_LEFT || key == K_RIGHT) {
            c.swap_gib += dir;
        } else {
            snprintf(text, sizeof text, "%d", c.swap_gib);
            if (prompt(_("Swap in GiB (0: none):"), text, sizeof text, 0)) c.swap_gib = atoi(text);
        }
        c.swap_gib = c.swap_gib < 0 ? 0 : c.swap_gib > 1024 ? 1024 : c.swap_gib;
        break;
    case R_ESP:
        pick_part(_("The EFI system partition (formatted FAT32)"), c.esp, sizeof c.esp, 0);
        break;
    case R_ROOT:
        pick_part(_("The root partition (formatted ext4)"), c.root, sizeof c.root, 0);
        break;
    case R_SWAP_PART:
        pick_part(_("The swap partition (formatted)"), c.swap, sizeof c.swap, 1);
        break;
    case R_RESCAN:
        reread();
        break;
    case R_ENC: {
        int n = tpm_usable ? 3 : 2;
        c.encrypt = (c.encrypt + n + dir) % n;
        break;
    }
    case R_PASS:
        prompt_secret(_("Disk passphrase:"), _("The same again:"), c.passphrase, sizeof c.passphrase);
        break;
    case R_USER:
        prompt(_("User name:"), c.user, sizeof c.user, 0);
        break;
    case R_FULL_NAME:
        prompt(_("Full name:"), c.full_name, sizeof c.full_name, 0);
        break;
    case R_PASSWORD:
        prompt_secret(_("The user's password:"), _("The same again:"), c.password, sizeof c.password);
        break;
    case R_HOST:
        prompt(_("Computer name:"), c.hostname, sizeof c.hostname, 0);
        break;
    case R_LOCK:
        if (c.encrypt != ENC_TPM) c.boot_lock = !c.boot_lock;
        break;
    case R_LANG:
        c.lang = !c.lang;
        c.lang_touched = 1;
        follow_language();
        break;
    case R_TZ: {
        static const char *items[MAX_LIST];
        int current = 0;
        for (int i = 0; i < nzones; i++) {
            items[i] = zone_label(zones[i]);
            if (strcmp(zones[i], c.timezone) == 0) current = i;
        }
        int got = pick(_("Time zone"), items, nzones, current);
        if (got >= 0) copy(c.timezone, sizeof c.timezone, zones[got]);
        break;
    }
    case R_NTP:
        c.ntp = !c.ntp;
        break;
    case R_IME:
        if (nimes) c.ime = (c.ime + nimes + dir) % nimes;
        c.ime_touched = 1;
        break;
    case R_MACHINE:
        if (nmachines) c.machine = (c.machine + nmachines + dir) % nmachines;
        break;
    case R_LEVEL:
        c.level = (c.level + nlevels + 1 + dir) % (nlevels + 1);
        break;
    case R_CPU:
        copy(text, sizeof text, c.cpu);
        if (prompt(_("Own builds for the CPU (gcc -march; auto: this one):"), text, sizeof text, 0)) {
            copy(c.cpu, sizeof c.cpu, text[0] ? text : "auto");
        }
        break;
    case R_RANK:
        c.rank = !c.rank;
        break;
    case R_UPD_REPO:
        prompt(_("Updates from (a git URL, or off):"), c.update_repo, sizeof c.update_repo, 0);
        break;
    case R_UPD_BRANCH:
        prompt(_("Branch:"), c.update_branch, sizeof c.update_branch, 0);
        break;
    case R_HIBERNATE:
        if (c.hibernate >= 0) {
            snprintf(text, sizeof text, "%d", c.hibernate);
        } else {
            text[0] = '\0';
        }
        if (prompt(_("Hibernate after (seconds asleep; empty: as the system has it):"), text, sizeof text, 0)) {
            c.hibernate = text[0] ? atoi(text) : -1;
        }
        break;
    case R_INSTALL:
        begin_install();
        break;
    default:
        if (id >= R_PROBLEM && id < R_PROBLEM + NSTEPS) {
            go_step(id - R_PROBLEM);
        } else if (id >= R_REPO && id < R_REPO + nrepos) {
            c.repo_on[id - R_REPO] ^= 1;
        } else if (id >= R_CONTAINER && id < R_CONTAINER + ncontainers) {
            c.container_on[id - R_CONTAINER] ^= 1;
            c.containers_touched = 1;
        } else if (id >= R_MODULE && id < R_MODULE + nmodules) {
            c.module_on[id - R_MODULE] ^= 1;
            c.modules_touched = 1;
            follow_modules();
        } else if (id >= R_DISK && id < R_DISK + ndisks) {
            copy(c.disk, sizeof c.disk, disks[id - R_DISK].path);
        }
    }
}

/* Up/Down to the next item, Home/End to the first or last. */
static void move(int key)
{
    build();
    int k = entry_of_selected(), to = -1;
    if (k < 0) return;
    if (key == K_UP || key == K_DOWN) {
        int d = key == K_UP ? -1 : 1;
        for (int i = k + d; i >= 0 && i < nentries && to < 0; i += d) {
            if (selectable(i)) to = i;
        }
        if (to < 0 && key == K_UP) scroll = 0; /* the title above the first */
    } else {
        int d = key == K_BOL ? 1 : -1;
        for (int i = key == K_BOL ? 0 : nentries - 1; i >= 0 && i < nentries && to < 0; i += d) {
            if (selectable(i)) to = i;
        }
    }
    if (to >= 0) selected = entries[to].id;
}

static void page_key(int key)
{
    if (key == K_UP || key == K_DOWN || key == K_BOL || key == K_EOL) {
        move(key);
    } else if ((key == K_LEFT || key == K_RIGHT) && is_choice(selected)) {
        act(selected, key);
    } else if (key == '\n' || key == ' ') {
        act(selected, key);
    } else if (mode == M_STEPS && (key == K_LEFT || key == '\t' || key == 27)) {
        in_page = 0;
    } else if (mode == M_STEPS && (key == K_PGUP || key == K_PGDN)) {
        go_step(step + (key == K_PGUP ? -1 : 1));
    }
}

static void sidebar_key(int key)
{
    if (key == K_UP || key == K_DOWN || key == K_PGUP || key == K_PGDN) {
        step += key == K_UP || key == K_PGUP ? -1 : 1;
        step = step < 0 ? 0 : step >= NSTEPS ? NSTEPS - 1 : step;
        selected = scroll = 0;
    } else if (key == '\n' || key == ' ' || key == K_RIGHT || key == '\t') {
        in_page = 1;
    } else if (key == 27) {
        mode = M_START;
        selected = R_START_INSTALL;
    }
}

static void after_key(int key)
{
    if (key == 't') {
        terminal();
    } else if (key == K_PGUP || key == K_PGDN) {
        log_from += key == K_PGUP ? 10 : -10;
        log_from = log_from < 0 ? 0 : log_from;
    } else if (mode != M_RUN && key == 'r' && yes(_("Reboot now?"))) {
        power("reboot");
    } else if (mode != M_RUN && key == 'p' && yes(_("Power off now?"))) {
        power("poweroff");
    } else if (mode == M_FAILED && key == 'b') {
        mode = M_STEPS;
        go_step(S_SUMMARY);
        reread(); /* the disk was partitioned: what is on it now */
    }
}

/* A click: what it stands for as a key (0: dealt with here). A click on
 * an item selects and does it, as Enter. */
static int pointer_key(void)
{
    const struct pointer_event *e = pointer_last();
    int hit = ui_hit_at(e->col, e->row);
    if (e->wheel) {
        if (hit == HIT_PANE) {
            log_from += e->wheel * 3;
            log_from = log_from < 0 ? 0 : log_from;
        } else if (mode == M_START || mode == M_STEPS) {
            for (int i = 0; i < (e->wheel < 0 ? -e->wheel : e->wheel); i++) move(e->wheel > 0 ? K_UP : K_DOWN);
            in_page = 1;
        }
        return 0;
    }
    if (e->button != BTN_LEFT || !e->pressed) return 0;
    if (hit >= HIT_KEY && hit < HIT_PANE) {
        int key = hit - HIT_KEY;
        if (key == 27 && mode == M_STEPS) { /* the button goes to the start from anywhere */
            mode = M_START;
            selected = R_START_INSTALL;
            return 0;
        }
        if (key == '\t' && mode == M_STEPS) in_page = !in_page;
        return key == '\t' ? 0 : key;
    }
    if (hit >= HIT_STEP && hit < HIT_STEP + NSTEPS) {
        step = hit - HIT_STEP;
        in_page = 0;
        selected = scroll = 0;
        return 0;
    }
    if (hit > 0 && hit < HIT_STEP) {
        selected = hit;
        in_page = 1;
        return '\n';
    }
    return 0;
}

static void handle(int key)
{
    if (mode == M_START) {
        page_key(key);
    } else if (mode == M_STEPS) {
        if (in_page) {
            page_key(key);
        } else {
            sidebar_key(key);
        }
    } else {
        after_key(key);
    }
}

int main(int argc, char **argv)
{
    int vt;
    if (argc > 1 && strcmp(argv[1], "--text") == 0) { /* after a crash: the console as it was */
        if (ioctl(STDIN_FILENO, KDGETMODE, &vt) == 0) {
            console = STDIN_FILENO;
            console_restore();
        }
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    const char *lang = getenv("LOCACHY_LANG");
    ui_lang = lang && strncmp(lang, "zh", 2) == 0;
    set_ui_language();
    console_take();
    atexit(at_exit);
    static const int fatal[] = { SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof fatal / sizeof fatal[0]; i++) signal(fatal[i], on_fatal);
    if (!screen_open(BASE_FONT)) {
        console_restore();
        fprintf(stderr, "installer: no screen to use; locachy-install --help is the installer on the console\n");
        return 1;
    }
    keys_open();
    take_pointer();
    say("installer: started");
    reread();
    tell(0, "%s", "");
    if (!described) tell(1, "%s", _("locachy-install --describe-json failed; the terminal shows why."));
    selected = R_START_INSTALL;
    for (;;) {
        draw();
        int key = key_poll_timeout(-1, job ? 500 : 1000);
        if (job) {
            int result = check_install();
            if (result >= 0) mode = result == 0 ? M_DONE : M_FAILED;
        }
        if (key == K_POINTER) key = pointer_key();
        if (key == 0 || key == K_SHOWN) continue;
        handle(key);
    }
}

