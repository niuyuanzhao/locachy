/*
 * firmware: system firmware, device updates, BIOS settings and SPI flash.
 * Three pages, Tab between them:
 *   Devices (fwupd)   LVFS / local .cab updates, release history (upgrade,
 *                     downgrade, reinstall), checksum verification and device
 *                     unlock via fwupdmgr --json (D-Bus activated on demand).
 *   BIOS Settings     Firmware setup attributes (/sys/class/firmware-attributes
 *                     such as ThinkPad think-lmi, or fwupdmgr bios-settings),
 *                     changed directly without entering POST Setup.
 *   SPI Flash         flashrom TUI for internal and external USB programmers
 *                     (CH341A, FT2232, Dediprog, spidev, serprog): probe chip &
 *                     IFD regions, mandatory 3-read SHA-256 verified backup,
 *                     verify, and confirmed write.
 * Long operations run in the root cgroup (never frozen across screen switches)
 * with live output in the lower pane (FW_JOB_LOG).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base.h"
#include "files.h"

#define FG     0xD0D0D0
#define BG     0x101820
#define ACCENT 0x80C0FF
#define DIM    0x808890
#define ERROR  0xFF9090
#define BAR_BG 0x1C2836

#ifndef FW_JOB_LOG
#define FW_JOB_LOG    "/run/locachy/firmware.log"
#define FW_JOB_PID    "/run/locachy/firmware.pid"
#define FW_HELPER     "/usr/local/bin/locachy-firmware"
#define PKG_HELPER    "/usr/local/bin/locachy-pkg"
#define FWUPD_PATH    "/usr/bin/fwupdmgr"
#define FLASHROM_PATH "/usr/bin/flashrom"
#endif

#define MAX_DEVICES   128
#define MAX_RELEASES   64
#define MAX_BIOS_ATTR 256
#define MAX_PROGS      16

#define HIT_ROW    0
#define HIT_TAB    100000
#define HIT_KEY    200000

enum { PAGE_FWUPD, PAGE_BIOS, PAGE_SPI, NPAGES };
enum { SPI_ROW_PROG, SPI_ROW_CHIP, SPI_ROW_REGIONS, SPI_ROW_DETECTED, SPI_ROW_BACKUP_DIR, NSPI_ROWS };
enum {
    CONF_NONE, CONF_UPDATE_ALL, CONF_INSTALL_REL, CONF_LOCAL_CAB,
    CONF_UNLOCK_DEV, CONF_SPI_WRITE
};

struct fw_dev {
    char id[96], name[128], vendor[64], version[64], update_ver[64], flags[96];
};

struct fw_rel {
    char version[64], kind[24], remote[32], uri[256], notes[256];
};

struct bios_attr {
    char name[96], current[128], possible[256], type[32], display[128];
};

struct spi_prog {
    char spec[96], label[96];
    int detected;
};

static struct fw_dev devs[MAX_DEVICES];
static int ndevs, fwupd_missing;

static int in_releases, rel_dev_idx;
static struct fw_rel rels[MAX_RELEASES];
static int nrels, sel_rel, top_rel;

static struct bios_attr battrs[MAX_BIOS_ATTR];
static int nbattrs;

static struct spi_prog progs[MAX_PROGS];
static int nprogs, cur_prog;
static char spi_chip[96];
static int cur_region_preset;
static const char *const region_presets[] = {
    "", "fd,gbe,bios", "bios", "fd", "gbe"
};
static const char *const region_labels[] = {
    N_("full chip"),
    N_("IFD: fd + gbe + bios (skip locked ME)"),
    N_("IFD: bios region only"),
    N_("IFD: flash descriptor (fd) only"),
    N_("IFD: Ethernet NVM (gbe) only"),
};
#define NREGION_PRESETS ((int)(sizeof region_presets / sizeof region_presets[0]))

static char detected_chip[128];
static long long detected_bytes;
static char detected_ifd[64];
static char backup_dir[PATH_MAX];
static int flashrom_missing;

static int page;
static int sel[NPAGES], top_row[NPAGES];
static int show_log = 1, log_scroll;
static pid_t job_pid;
static char job_label[128];
static int confirming;
static char pending_path[PATH_MAX];
static char notice[320];
static int notice_err;

__attribute__((format(printf, 2, 3)))
static void say_notice(int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(notice, sizeof notice, fmt, ap);
    va_end(ap);
    notice_err = err;
}

static int split_tsv(char *line, char **out, int max)
{
    line[strcspn(line, "\r\n")] = '\0';
    int n = 0;
    char *p = line;
    while (n < max) {
        out[n++] = p;
        char *tab = strchr(p, '\t');
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }
    return n;
}

static void fit_cells(char *out, size_t size, const char *s, int cells)
{
    if (cells <= 0 || size == 0) {
        if (size) out[0] = '\0';
        return;
    }
    if (ui_cells(s) <= cells) {
        snprintf(out, size, "%s", s);
        return;
    }
    size_t i = 0;
    int used = 0;
    while (s[i]) {
        unsigned char b = (unsigned char)s[i];
        size_t clen = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        uint32_t cp = b & (0x3Fu >> (clen - 1));
        for (size_t k = 1; k < clen && s[i + k]; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        int w = clen == 1 ? 1 : screen_char_cells(cp);
        if (used + w > cells - 1 || i + clen + 4 >= size) break;
        used += w;
        i += clen;
    }
    snprintf(out, size, "%.*s\xe2\x80\xa6", (int)i, s);
}

static int run_capture(char *const argv[], char *buf, size_t size)
{
    int pfd[2];
    if (size) buf[0] = '\0';
    if (pipe2(pfd, O_CLOEXEC) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDERR_FILENO);
        }
        dup2(pfd[1], STDOUT_FILENO);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t len = 0;
    while (pid > 0 && len + 1 < size) {
        ssize_t n = read(pfd[0], buf + len, size - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(pfd[0]);
    if (size) buf[len] = '\0';
    int st = -1;
    if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

static void load_fwupd(void)
{
    ndevs = 0;
    fwupd_missing = access(FWUPD_PATH, X_OK) != 0;
    if (fwupd_missing) return;
    static char buf[65536];
    char *const argv[] = { FW_HELPER, "fwupd-devices", NULL };
    int rc = run_capture(argv, buf, sizeof buf);
    if (rc == 10) {
        fwupd_missing = 1;
        return;
    }
    for (char *s = buf, *nl; *s && ndevs < MAX_DEVICES; s = nl ? nl + 1 : s + strlen(s)) {
        nl = strchr(s, '\n');
        if (nl) *nl = '\0';
        if (!*s) continue;
        char *f[6];
        if (split_tsv(s, f, 6) < 6) continue;
        struct fw_dev *d = &devs[ndevs++];
        snprintf(d->id, sizeof d->id, "%s", f[0]);
        snprintf(d->name, sizeof d->name, "%s", f[1]);
        snprintf(d->vendor, sizeof d->vendor, "%s", f[2]);
        snprintf(d->version, sizeof d->version, "%s", f[3]);
        snprintf(d->update_ver, sizeof d->update_ver, "%s", f[4]);
        snprintf(d->flags, sizeof d->flags, "%s", f[5]);
    }
    if (sel[PAGE_FWUPD] >= ndevs) sel[PAGE_FWUPD] = ndevs > 0 ? ndevs - 1 : 0;
}

static void load_releases(int idx)
{
    nrels = sel_rel = top_rel = 0;
    if (idx < 0 || idx >= ndevs) return;
    static char buf[32768];
    char *const argv[] = {
        FW_HELPER, "fwupd-releases", devs[idx].id, "--current-version", devs[idx].version, NULL
    };
    if (run_capture(argv, buf, sizeof buf) != 0) return;
    for (char *s = buf, *nl; *s && nrels < MAX_RELEASES; s = nl ? nl + 1 : s + strlen(s)) {
        nl = strchr(s, '\n');
        if (nl) *nl = '\0';
        if (!*s) continue;
        char *f[5];
        int nf = split_tsv(s, f, 5);
        if (nf < 3) continue;
        struct fw_rel *r = &rels[nrels++];
        snprintf(r->version, sizeof r->version, "%s", f[0]);
        snprintf(r->kind, sizeof r->kind, "%s", f[1]);
        snprintf(r->remote, sizeof r->remote, "%s", f[2]);
        snprintf(r->uri, sizeof r->uri, "%s", nf > 3 ? f[3] : "");
        snprintf(r->notes, sizeof r->notes, "%s", nf > 4 ? f[4] : "");
    }
}

static void load_bios(void)
{
    nbattrs = 0;
    static char buf[65536];
    char *const argv[] = { FW_HELPER, "bios-list", NULL };
    if (run_capture(argv, buf, sizeof buf) != 0) return;
    for (char *s = buf, *nl; *s && nbattrs < MAX_BIOS_ATTR; s = nl ? nl + 1 : s + strlen(s)) {
        nl = strchr(s, '\n');
        if (nl) *nl = '\0';
        if (!*s) continue;
        char *f[5];
        int nf = split_tsv(s, f, 5);
        if (nf < 4) continue;
        struct bios_attr *a = &battrs[nbattrs++];
        snprintf(a->name, sizeof a->name, "%s", f[0]);
        snprintf(a->current, sizeof a->current, "%s", f[1]);
        snprintf(a->possible, sizeof a->possible, "%s", f[2]);
        snprintf(a->type, sizeof a->type, "%s", f[3]);
        snprintf(a->display, sizeof a->display, "%s", nf > 4 && f[4][0] ? f[4] : f[0]);
    }
    if (sel[PAGE_BIOS] >= nbattrs) sel[PAGE_BIOS] = nbattrs > 0 ? nbattrs - 1 : 0;
}

static void load_spi(void)
{
    nprogs = 0;
    flashrom_missing = access(FLASHROM_PATH, X_OK) != 0;
    static char buf[8192];
    char *const argv[] = { FW_HELPER, "spi-programmers", NULL };
    if (run_capture(argv, buf, sizeof buf) == 0) {
        for (char *s = buf, *nl; *s && nprogs < MAX_PROGS; s = nl ? nl + 1 : s + strlen(s)) {
            nl = strchr(s, '\n');
            if (nl) *nl = '\0';
            if (!*s) continue;
            char *f[3];
            if (split_tsv(s, f, 3) < 3) continue;
            struct spi_prog *p = &progs[nprogs++];
            snprintf(p->spec, sizeof p->spec, "%s", f[0]);
            p->detected = atoi(f[1]) != 0;
            snprintf(p->label, sizeof p->label, "%s", f[2]);
        }
    }
    if (nprogs == 0) {
        snprintf(progs[0].spec, sizeof progs[0].spec, "internal");
        snprintf(progs[0].label, sizeof progs[0].label, "Internal mainboard SPI controller");
        progs[0].detected = 1;
        nprogs = 1;
    }
    if (cur_prog >= nprogs) cur_prog = 0;
    if (!backup_dir[0]) {
        char hm[256] = "", drs[256] = "";
        open_user_dirs(hm, sizeof hm, drs, sizeof drs);
        snprintf(backup_dir, sizeof backup_dir, "%.240s/Firmware", hm[0] ? hm : "/root");
    }
}

static void load_all(void)
{
    load_fwupd();
    load_bios();
    load_spi();
}

static void start_job(const char *label, char *const argv[])
{
    if (job_pid > 0) {
        say_notice(1, _("Wait: %s is still running."), job_label);
        return;
    }
    mkdir("/run/locachy", 0755);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int cg = open("/sys/fs/cgroup/cgroup.procs", O_WRONLY | O_CLOEXEC);
        if (cg >= 0) {
            if (write(cg, "0", 1) < 0) {}
            close(cg);
        }
        int out = open(FW_JOB_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            dup2(out, STDERR_FILENO);
        }
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) {
        say_notice(1, _("Cannot start %s."), label);
        return;
    }
    job_pid = pid;
    snprintf(job_label, sizeof job_label, "%s", label);
    show_log = 1;
    log_scroll = 0;
    FILE *pf = fopen(FW_JOB_PID, "we");
    if (pf) {
        fprintf(pf, "%d %s\n", (int)pid, label);
        fclose(pf);
    }
    say_notice(0, _("Running: %s..."), label);
}

static void parse_probe_log(void)
{
    FILE *f = fopen(FW_JOB_LOG, "re");
    if (!f) return;
    char line[512];
    detected_chip[0] = detected_ifd[0] = '\0';
    detected_bytes = 0;
    int chip_count = 0;
    char first_chip[96] = "";
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "CHIP\t", 5) == 0) {
            char *cols[4];
            if (split_tsv(line, cols, 4) >= 4) {
                chip_count++;
                if (chip_count == 1) {
                    snprintf(first_chip, sizeof first_chip, "%s", cols[1]);
                    detected_bytes = atoll(cols[2]);
                    snprintf(detected_ifd, sizeof detected_ifd, "%s", cols[3]);
                }
            }
        }
    }
    fclose(f);
    if (chip_count == 1) {
        snprintf(detected_chip, sizeof detected_chip, "%s (%lld KiB)", first_chip, detected_bytes / 1024);
    } else if (chip_count > 1) {
        snprintf(detected_chip, sizeof detected_chip, _("%s (+%d more: set Chip model -c)"),
                 first_chip, chip_count - 1);
        if (!spi_chip[0]) snprintf(spi_chip, sizeof spi_chip, "%s", first_chip);
    }
}

static void check_job(void)
{
    if (job_pid <= 0) return;
    int st = 0;
    if (waitpid(job_pid, &st, WNOHANG) != job_pid) return;
    job_pid = 0;
    unlink(FW_JOB_PID);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (strstr(job_label, "SPI probe") != NULL) {
        parse_probe_log();
    }
    load_all();
    if (in_releases) load_releases(rel_dev_idx);
    if (ok) {
        say_notice(0, _("%s completed."), job_label);
    } else {
        say_notice(1, _("%s failed (status %d); check the output pane below."),
                   job_label, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    }
}

static int prompt_text(const char *question, char *buf, size_t size)
{
    size_t pos = strlen(buf);
    for (;;) {
        int row = screen_rows() - 3, cols = screen_cols();
        char line[512];
        snprintf(line, sizeof line, "%s %.300s_", question, buf);
        screen_fill(0, row, cols, 1, BG);
        screen_text(1, row, line, ACCENT, BG);
        screen_flush();
        int key = key_poll_timeout(-1, -1);
        size_t len = strlen(buf);
        if (pos > len) pos = len;
        if (key == 27) return 0;
        if (key == '\n') return 1;
        if (key == '\b' || key == 8) {
            while (len > 1 && ((unsigned char)buf[len - 1] & 0xC0) == 0x80) len--; /* a whole character */
            if (len > 0) buf[len - 1] = '\0';
        } else if (key == 21) {
            buf[0] = '\0';
        } else if (keys_typed(key) && len + strlen(keys_typed(key)) < size) {
            strcat(buf, keys_typed(key));
        }
    }
}

static void draw_log_pane(int top_r, int h)
{
    int cols = screen_cols();
    screen_fill(0, top_r, cols, 1, BAR_BG);
    char hdr[256];
    if (job_pid > 0) {
        snprintf(hdr, sizeof hdr, _(" Output: %.120s (running...) "), job_label);
    } else if (in_releases && nrels > 0 && sel_rel < nrels && rels[sel_rel].notes[0]) {
        snprintf(hdr, sizeof hdr, _(" Release %s notes "), rels[sel_rel].version);
    } else {
        snprintf(hdr, sizeof hdr, _(" Output (%s) "), FW_JOB_LOG);
    }
    screen_text(1, top_r, hdr, ACCENT, BAR_BG);
    if (h <= 1) return;

    if (!job_pid && in_releases && nrels > 0 && sel_rel < nrels && rels[sel_rel].notes[0]) {
        char cut[512];
        fit_cells(cut, sizeof cut, rels[sel_rel].notes, cols - 2);
        screen_text(1, top_r + 1, cut, FG, BG);
        return;
    }

    static char lines[256][256];
    int nlines = 0;
    FILE *f = fopen(FW_JOB_LOG, "re");
    while (f && fgets(lines[nlines % 256], sizeof lines[0], f)) {
        lines[nlines % 256][strcspn(lines[nlines % 256], "\r\n")] = '\0';
        nlines++;
    }
    if (f) fclose(f);
    int total = nlines < 256 ? nlines : 256;
    int base = nlines < 256 ? 0 : nlines % 256;
    int body = h - 1;
    int start = total > body ? total - body - log_scroll : 0;
    if (start < 0) start = 0;
    for (int i = 0; i < body && start + i < total; i++) {
        const char *ln = lines[(base + start + i) % 256];
        char cut[256];
        fit_cells(cut, sizeof cut, ln, cols - 2);
        screen_text(1, top_r + 1 + i, cut, DIM, BG);
    }
}

static int list_count(void)
{
    if (page == PAGE_FWUPD) return in_releases ? nrels : ndevs;
    if (page == PAGE_BIOS) return nbattrs;
    return NSPI_ROWS;
}

static void draw(void)
{
    int cols = screen_cols(), rows = screen_rows();
    screen_fill(0, 0, cols, rows, BG);
    ui_hits_clear();
    screen_bar(_("firmware"), NULL);

    /* Row 1: Tabs */
    static const char *const tab_names[NPAGES] = {
        N_("Devices (fwupd)"), N_("BIOS Settings"), N_("SPI Flash (flashrom)")
    };
    screen_fill(0, 1, cols, 1, BAR_BG);
    int tc = 1;
    for (int i = 0; i < NPAGES; i++) {
        char tbuf[96];
        snprintf(tbuf, sizeof tbuf, " %s ", _(tab_names[i]));
        int w = ui_cells(tbuf), on = page == i;
        screen_fill(tc, 1, w, 1, on ? ACCENT : BAR_BG);
        screen_text(tc, 1, tbuf, on ? BG : FG, on ? ACCENT : BAR_BG);
        ui_hit(tc, 1, w, 1, HIT_TAB + i);
        tc += w + 1;
    }

    int avail = rows - 5; /* rows 2 .. rows-4 */
    if (avail < 4) avail = 4;
    int log_h = show_log ? avail / 3 + 1 : 0;
    if (log_h > 12) log_h = 12;
    int list_h = avail - log_h - 1; /* 1 row for column header */
    if (list_h < 2) list_h = 2;

    int *cur_sel = (page == PAGE_FWUPD && in_releases) ? &sel_rel : &sel[page];
    int *cur_top = (page == PAGE_FWUPD && in_releases) ? &top_rel : &top_row[page];
    int cnt = list_count();
    if (*cur_sel >= cnt) *cur_sel = cnt > 0 ? cnt - 1 : 0;
    if (*cur_sel < 0) *cur_sel = 0;
    if (*cur_sel < *cur_top) *cur_top = *cur_sel;
    if (*cur_sel >= *cur_top + list_h) *cur_top = *cur_sel - list_h + 1;
    if (*cur_top < 0) *cur_top = 0;

    char line[512], c1[128], c2[128], c3[96], c4[128];
    if (page == PAGE_FWUPD && !in_releases) {
        snprintf(line, sizeof line, " %s %s %s %s",
                 ui_pad(c1, sizeof c1, _("Device"), 30),
                 ui_pad(c2, sizeof c2, _("Vendor"), 16),
                 ui_pad(c3, sizeof c3, _("Version"), 14),
                 _("Status"));
        screen_fill(0, 2, cols, 1, BAR_BG);
        screen_text(0, 2, line, ACCENT, BAR_BG);
        if (fwupd_missing) {
            /* TRANSLATORS: P is the key; keep it. */
            screen_text(2, 4, _("fwupd is not installed. Press P to install fwupd with locachy-pkg."), DIM, BG);
        } else if (ndevs == 0) {
            /* TRANSLATORS: R is the key; keep it. */
            screen_text(2, 4, _("No updatable devices reported yet (press R to refresh LVFS metadata)."), DIM, BG);
        } else {
            for (int r = 0; r < list_h && *cur_top + r < ndevs; r++) {
                int i = *cur_top + r, on = i == *cur_sel;
                const struct fw_dev *d = &devs[i];
                snprintf(line, sizeof line, " %s %s %s %.80s",
                         ui_pad(c1, sizeof c1, d->name, 30),
                         ui_pad(c2, sizeof c2, d->vendor, 16),
                         ui_pad(c3, sizeof c3, d->version, 14),
                         d->flags);
                fit_cells(c4, sizeof c4, line, cols);
                screen_fill(0, 3 + r, cols, 1, on ? ACCENT : BG);
                screen_text(0, 3 + r, c4, on ? BG : FG, on ? ACCENT : BG);
                ui_hit(0, 3 + r, cols, 1, HIT_ROW + i);
            }
        }
    } else if (page == PAGE_FWUPD && in_releases) {
        snprintf(line, sizeof line, _(" Releases for %.50s (current %s) — Esc: back "),
                 devs[rel_dev_idx].name, devs[rel_dev_idx].version);
        screen_fill(0, 2, cols, 1, BAR_BG);
        screen_text(0, 2, line, ACCENT, BAR_BG);
        if (nrels == 0) {
            screen_text(2, 4, _("No LVFS releases listed for this device (refresh metadata with R first)."), DIM, BG);
        } else {
            for (int r = 0; r < list_h && *cur_top + r < nrels; r++) {
                int i = *cur_top + r, on = i == *cur_sel;
                const struct fw_rel *rel = &rels[i];
                snprintf(line, sizeof line, " %s %s %s %.120s",
                         ui_pad(c1, sizeof c1, rel->version, 16),
                         ui_pad(c2, sizeof c2, rel->kind, 12),
                         ui_pad(c3, sizeof c3, rel->remote, 10),
                         rel->notes);
                fit_cells(c4, sizeof c4, line, cols);
                screen_fill(0, 3 + r, cols, 1, on ? ACCENT : BG);
                screen_text(0, 3 + r, c4, on ? BG : FG, on ? ACCENT : BG);
                ui_hit(0, 3 + r, cols, 1, HIT_ROW + i);
            }
        }
    } else if (page == PAGE_BIOS) {
        snprintf(line, sizeof line, " %s %s %s",
                 ui_pad(c1, sizeof c1, _("BIOS Setting"), 34),
                 ui_pad(c2, sizeof c2, _("Current Value"), 22),
                 _("Allowed Values"));
        screen_fill(0, 2, cols, 1, BAR_BG);
        screen_text(0, 2, line, ACCENT, BAR_BG);
        if (nbattrs == 0) {
            screen_text(2, 4, _("No kernel firmware-attributes exposed on this machine."), DIM, BG);
        } else {
            for (int r = 0; r < list_h && *cur_top + r < nbattrs; r++) {
                int i = *cur_top + r, on = i == *cur_sel;
                const struct bios_attr *a = &battrs[i];
                char cur_fmt[96];
                snprintf(cur_fmt, sizeof cur_fmt, "< %.80s >", a->current);
                snprintf(line, sizeof line, " %s %s %.80s",
                         ui_pad(c1, sizeof c1, a->name, 34),
                         ui_pad(c2, sizeof c2, cur_fmt, 22),
                         a->possible);
                fit_cells(c4, sizeof c4, line, cols);
                screen_fill(0, 3 + r, cols, 1, on ? ACCENT : BG);
                screen_text(0, 3 + r, c4, on ? BG : FG, on ? ACCENT : BG);
                ui_hit(0, 3 + r, cols, 1, HIT_ROW + i);
            }
        }
    } else {
        screen_fill(0, 2, cols, 1, BAR_BG);
        screen_text(1, 2, _(" SPI Flash Programmer (flashrom) — always take a 3x verified backup before writing "),
                    ACCENT, BAR_BG);
        if (flashrom_missing) {
            /* TRANSLATORS: P is the key; keep it. */
            screen_text(2, 3 + NSPI_ROWS + 1,
                        _("flashrom is not installed. Press P to install flashrom with locachy-pkg."), ERROR, BG);
        }
        for (int i = 0; i < NSPI_ROWS && i < list_h; i++) {
            int on = i == *cur_sel;
            char val[256] = "";
            const char *lbl = "";
            if (i == SPI_ROW_PROG) {
                lbl = _("Programmer (-p)");
                snprintf(val, sizeof val, "< %s — %s%s >", progs[cur_prog].spec, progs[cur_prog].label,
                         progs[cur_prog].detected && strcmp(progs[cur_prog].spec, "internal") != 0 ?
                         _(" [USB connected]") : "");
            } else if (i == SPI_ROW_CHIP) {
                lbl = _("Chip model (-c)");
                snprintf(val, sizeof val, "%s", spi_chip[0] ? spi_chip : _("auto-detect (Enter to specify)"));
            } else if (i == SPI_ROW_REGIONS) {
                lbl = _("Target region");
                snprintf(val, sizeof val, "< %s >", _(region_labels[cur_region_preset]));
            } else if (i == SPI_ROW_DETECTED) {
                lbl = _("Last probed chip");
                if (detected_chip[0]) {
                    snprintf(val, sizeof val, "%s%s%s", detected_chip,
                             detected_ifd[0] ? _("   IFD readable: ") : "", detected_ifd);
                } else {
                    /* TRANSLATORS: p is the key; keep it. */
                    snprintf(val, sizeof val, "%s", _("not probed yet (press p to probe)"));
                }
            } else if (i == SPI_ROW_BACKUP_DIR) {
                lbl = _("Backup directory");
                snprintf(val, sizeof val, "%.200s", backup_dir);
            }
            snprintf(line, sizeof line, " %s %.240s", ui_pad(c1, sizeof c1, lbl, 24), val);
            fit_cells(c4, sizeof c4, line, cols);
            screen_fill(0, 3 + i, cols, 1, on ? ACCENT : BG);
            screen_text(0, 3 + i, c4, on ? BG : FG, on ? ACCENT : BG);
            ui_hit(0, 3 + i, cols, 1, HIT_ROW + i);
        }
    }

    if (show_log && log_h > 0) {
        draw_log_pane(3 + list_h, log_h);
    }

    /* Notice / confirmation row (rows - 3) and buttons (rows - 2, rows - 1) */
    screen_fill(0, rows - 3, cols, 3, BG);
    screen_text(1, rows - 3, notice, confirming ? ERROR : (notice_err ? ERROR : ACCENT), BG);

    int b2 = rows - 2, b1 = rows - 1, bc = 1;
    if (confirming != CONF_NONE) {
        /* TRANSLATORS: Y and N are the confirmation keys; keep them. */
        bc = ui_button(bc, b2, _("Y: confirm"), HIT_KEY + 'y');
        ui_button(bc, b2, _("N: cancel"), HIT_KEY + 'n');
        screen_text(1, b1, _("Flashing firmware is irreversible; any other key cancels."), FG, BG);
    } else if (page == PAGE_FWUPD && in_releases) {
        bc = ui_button(bc, b2, _("Enter: install release"), HIT_KEY + '\n');
        bc = ui_button(bc, b2, _("l: output"), HIT_KEY + 'l');
        ui_button(bc, b2, _("Esc: back to devices"), HIT_KEY + 27);
        screen_text(1, b1, _("Tab: switch page   Up/Down: select release"), DIM, BG);
    } else if (page == PAGE_FWUPD) {
        if (fwupd_missing) {
            bc = ui_button(bc, b2, _("P: install fwupd"), HIT_KEY + 'P');
        } else {
            bc = ui_button(bc, b2, _("Enter: releases"), HIT_KEY + '\n');
            bc = ui_button(bc, b2, _("r: refresh LVFS"), HIT_KEY + 'r');
            bc = ui_button(bc, b2, _("u: update all"), HIT_KEY + 'u');
            bc = ui_button(bc, b2, _("i: install .cab"), HIT_KEY + 'i');
            bc = ui_button(bc, b2, _("v: verify"), HIT_KEY + 'v');
            bc = ui_button(bc, b2, _("k: unlock"), HIT_KEY + 'k');
        }
        bc = ui_button(bc, b2, _("l: output"), HIT_KEY + 'l');
        ui_button(1, b1, _("q: home"), HIT_KEY + 'q');
        screen_text(14, b1, _("Tab: BIOS Settings / SPI Flash"), DIM, BG);
    } else if (page == PAGE_BIOS) {
        bc = ui_button(bc, b2, _("Enter: choose value"), HIT_KEY + '\n');
        bc = ui_button(bc, b2, _("r: reload"), HIT_KEY + 'r');
        bc = ui_button(bc, b2, _("l: output"), HIT_KEY + 'l');
        ui_button(1, b1, _("q: home"), HIT_KEY + 'q');
        screen_text(14, b1, _("Left/Right: cycle option   Tab: switch page"), DIM, BG);
    } else {
        if (flashrom_missing) {
            bc = ui_button(bc, b2, _("P: install flashrom"), HIT_KEY + 'P');
        } else {
            bc = ui_button(bc, b2, _("p: probe chip"), HIT_KEY + 'p');
            bc = ui_button(bc, b2, _("b: 3x backup"), HIT_KEY + 'b');
            bc = ui_button(bc, b2, _("v: verify file"), HIT_KEY + 'v');
            bc = ui_button(bc, b2, _("w: write image"), HIT_KEY + 'w');
            bc = ui_button(bc, b2, _("c: custom -p"), HIT_KEY + 'c');
        }
        bc = ui_button(bc, b2, _("l: output"), HIT_KEY + 'l');
        ui_button(1, b1, _("q: home"), HIT_KEY + 'q');
        screen_text(14, b1, _("Left/Right/Enter: configure programmer & region   Tab: switch page"), DIM, BG);
    }
    screen_flush();
}

static void change_bios_attr(int idx, int step)
{
    if (idx < 0 || idx >= nbattrs) return;
    struct bios_attr *a = &battrs[idx];
    char target_val[128] = "";

    if (a->possible[0] && strchr(a->possible, ';')) {
        char copy[256];
        const char *opts[32];
        char storage[32][64];
        int n = 0, cur_i = -1;
        snprintf(copy, sizeof copy, "%s", a->possible);
        for (char *tok = strtok(copy, ";"); tok && n < 31; tok = strtok(NULL, ";")) {
            snprintf(storage[n], sizeof storage[0], "%s", tok);
            opts[n] = storage[n];
            if (strcmp(tok, a->current) == 0) cur_i = n;
            n++;
        }
        opts[n] = NULL;
        if (n == 0) return;
        if (step != 0) {
            int next = cur_i < 0 ? 0 : (cur_i + (step > 0 ? 1 : n - 1)) % n;
            snprintf(target_val, sizeof target_val, "%.63s", storage[next]);
        } else {
            int ch = ui_menu(36, 4, opts);
            if (ch < 0) return;
            snprintf(target_val, sizeof target_val, "%.63s", storage[ch]);
        }
    } else if (step == 0) {
        snprintf(target_val, sizeof target_val, "%s", strcmp(a->current, "-") == 0 ? "" : a->current);
        char q[160];
        snprintf(q, sizeof q, _("Set %.80s to:"), a->name);
        if (!prompt_text(q, target_val, sizeof target_val)) return;
    } else {
        return;
    }

    char out[512];
    char *const argv[] = { FW_HELPER, "bios-set", a->name, target_val, NULL };
    int rc = run_capture(argv, out, sizeof out);
    out[strcspn(out, "\r\n")] = '\0';
    load_bios();
    if (rc == 0) {
        say_notice(0, "%s", out[0] ? out : _("BIOS setting updated."));
    } else {
        say_notice(1, _("Could not change %s."), a->name);
    }
}

static void confirm_action(int key)
{
    int which = confirming;
    confirming = CONF_NONE;
    if (key != 'y' && key != 'Y') {
        say_notice(0, _("Cancelled; nothing written."));
        return;
    }
    if (which == CONF_UPDATE_ALL) {
        char *const argv[] = { FW_HELPER, "fwupd-action", "update", NULL };
        start_job(_("fwupd update all"), argv);
    } else if (which == CONF_INSTALL_REL && rel_dev_idx < ndevs && sel_rel < nrels) {
        const struct fw_rel *r = &rels[sel_rel];
        const char *extra = r->uri[0] ? r->uri : r->kind;
        char label[128];
        snprintf(label, sizeof label, "fwupd %s %s", r->kind, r->version);
        char *const argv[] = { FW_HELPER, "fwupd-action", "install-release", devs[rel_dev_idx].id, (char *)extra, NULL };
        start_job(label, argv);
    } else if (which == CONF_LOCAL_CAB && pending_path[0]) {
        char *const argv[] = { FW_HELPER, "fwupd-action", "local-install", pending_path, NULL };
        start_job(_("fwupd local-install"), argv);
    } else if (which == CONF_UNLOCK_DEV && sel[PAGE_FWUPD] < ndevs) {
        char *const argv[] = { FW_HELPER, "fwupd-action", "unlock", devs[sel[PAGE_FWUPD]].id, NULL };
        start_job(_("fwupd unlock"), argv);
    } else if (which == CONF_SPI_WRITE && pending_path[0]) {
        char sz_arg[32] = "0";
        const char *regs = region_presets[cur_region_preset];
        if (!regs[0] && detected_bytes > 0) {
            snprintf(sz_arg, sizeof sz_arg, "%lld", detected_bytes);
        }
        char *argv[12];
        int n = 0;
        argv[n++] = FW_HELPER;
        argv[n++] = "spi-write";
        argv[n++] = progs[cur_prog].spec;
        argv[n++] = pending_path;
        if (spi_chip[0]) { argv[n++] = "--chip"; argv[n++] = spi_chip; }
        if (regs[0])     { argv[n++] = "--regions"; argv[n++] = (char *)regs; }
        if (strcmp(sz_arg, "0") != 0) { argv[n++] = "--expected-size"; argv[n++] = sz_arg; }
        argv[n] = NULL;
        start_job(_("SPI write & verify"), argv);
    }
}

static int pointer_to_key(void)
{
    const struct pointer_event *e = pointer_last();
    int *cur_sel = (page == PAGE_FWUPD && in_releases) ? &sel_rel : &sel[page];
    int cnt = list_count();
    if (e->wheel && confirming == CONF_NONE) {
        *cur_sel = *cur_sel - e->wheel;
        if (*cur_sel < 0) *cur_sel = 0;
        if (*cur_sel >= cnt) *cur_sel = cnt > 0 ? cnt - 1 : 0;
        return -1;
    }
    if (!e->pressed || e->button != BTN_LEFT) return 0;
    int id = ui_hit_at(e->col, e->row);
    if (id >= HIT_KEY) return id - HIT_KEY;
    if (confirming != CONF_NONE) return 0;
    if (id >= HIT_TAB && id - HIT_TAB < NPAGES) {
        page = id - HIT_TAB;
        in_releases = 0;
        return -1;
    }
    if (id >= HIT_ROW && id - HIT_ROW < cnt) {
        *cur_sel = id - HIT_ROW;
        return e->clicks == 2 ? '\n' : -1;
    }
    return 0;
}

int main(void)
{
    load_all();
    screen_open(BASE_FONT);
    keys_open();
    say_notice(0, "%s", _("Tab switches between fwupd Devices, BIOS Settings and SPI Flash (flashrom)."));
    draw();

    for (;;) {
        int timeout = job_pid > 0 ? 300 : -1;
        int key = key_poll_timeout(-1, timeout);
        int mods = keys_modifiers();
        check_job();

        if (key == K_SHOWN) {
            confirming = CONF_NONE;
            load_all();
            draw();
            continue;
        }
        if (key == K_POINTER) {
            key = pointer_to_key();
            if (key <= 0) {
                if (key < 0 || job_pid > 0) draw();
                continue;
            }
        }
        if (key == 0) {
            if (job_pid > 0) draw();
            continue;
        }

        if (confirming != CONF_NONE) {
            confirm_action(key);
            draw();
            continue;
        }

        if (key == '\t') {
            in_releases = 0;
            page = (page + ((mods & KEYS_SHIFT) ? NPAGES - 1 : 1)) % NPAGES;
            draw();
            continue;
        }
        if (key == 'l') {
            show_log = !show_log;
            draw();
            continue;
        }
        if (key == 'q' || key == 27 || key == '\b') {
            if (page == PAGE_FWUPD && in_releases) {
                in_releases = 0;
                draw();
                continue;
            }
            if (key == 'q' || key == 27) {
                return 0;
            }
        }

        int *cur_sel = (page == PAGE_FWUPD && in_releases) ? &sel_rel : &sel[page];
        int cnt = list_count();
        if (key == K_UP || key == K_DOWN || key == K_PGUP || key == K_PGDN || key == K_BOL || key == K_EOL) {
            int step = (key == K_PGUP || key == K_PGDN) ? 8 : 1;
            if (key == K_BOL) *cur_sel = 0;
            else if (key == K_EOL) *cur_sel = cnt > 0 ? cnt - 1 : 0;
            else *cur_sel += (key == K_UP || key == K_PGUP) ? -step : step;
            if (*cur_sel < 0) *cur_sel = 0;
            if (*cur_sel >= cnt) *cur_sel = cnt > 0 ? cnt - 1 : 0;
            draw();
            continue;
        }

        if (page == PAGE_FWUPD) {
            if (key == 'P' && fwupd_missing) {
                char *const argv[] = { PKG_HELPER, "install", "fwupd", NULL };
                start_job(_("Install fwupd"), argv);
            } else if (!fwupd_missing && key == 'r') {
                char *const argv[] = { FW_HELPER, "fwupd-action", "refresh", NULL };
                start_job(_("Refresh LVFS metadata"), argv);
            } else if (!fwupd_missing && !in_releases && key == 'u') {
                confirming = CONF_UPDATE_ALL;
                say_notice(1, "%s", _("Y/N: Apply all pending LVFS firmware updates?"));
            } else if (!fwupd_missing && !in_releases && key == 'i') {
                char hm[256] = "", drs[256] = "", chosen[PATH_MAX] = "";
                open_user_dirs(hm, sizeof hm, drs, sizeof drs);
                if (files_pick(hm[0] ? hm : "/root", NULL, "", _("Choose a .cab firmware archive (Enter):"),
                               chosen, sizeof chosen)) {
                    snprintf(pending_path, sizeof pending_path, "%s", chosen);
                    confirming = CONF_LOCAL_CAB;
                    say_notice(1, _("Y/N: Install firmware archive %.180s?"), chosen);
                }
            } else if (!fwupd_missing && !in_releases && key == 'v' && ndevs > 0) {
                char *const argv[] = { FW_HELPER, "fwupd-action", "verify", devs[*cur_sel].id, NULL };
                start_job(_("fwupd verify"), argv);
            } else if (!fwupd_missing && !in_releases && key == 'k' && ndevs > 0) {
                confirming = CONF_UNLOCK_DEV;
                say_notice(1, _("Y/N: Unlock device %.80s for firmware updates?"), devs[*cur_sel].name);
            } else if (!fwupd_missing && !in_releases && key == '\n' && ndevs > 0) {
                rel_dev_idx = *cur_sel;
                load_releases(rel_dev_idx);
                in_releases = 1;
            } else if (!fwupd_missing && in_releases && key == '\n' && nrels > 0) {
                confirming = CONF_INSTALL_REL;
                say_notice(1, _("Y/N: %s %.60s to version %s?"),
                           rels[sel_rel].kind, devs[rel_dev_idx].name, rels[sel_rel].version);
            }
        } else if (page == PAGE_BIOS) {
            if (key == 'r') {
                load_bios();
                say_notice(0, "%s", _("BIOS settings reloaded."));
            } else if (nbattrs > 0 && (key == '\n' || key == K_LEFT || key == K_RIGHT)) {
                change_bios_attr(*cur_sel, key == K_LEFT ? -1 : key == K_RIGHT ? 1 : 0);
            }
        } else if (page == PAGE_SPI) {
            if (key == 'P' && flashrom_missing) {
                char *const argv[] = { PKG_HELPER, "install", "flashrom", NULL };
                start_job(_("Install flashrom"), argv);
            } else if ((key == K_LEFT || key == K_RIGHT || key == '\n') && *cur_sel == SPI_ROW_PROG) {
                int step = key == K_LEFT ? nprogs - 1 : 1;
                cur_prog = (cur_prog + step) % nprogs;
                detected_chip[0] = detected_ifd[0] = '\0';
                detected_bytes = 0;
            } else if (key == 'c' || (key == '\n' && *cur_sel == SPI_ROW_PROG && mods & KEYS_CTRL)) {
                char spec[96];
                snprintf(spec, sizeof spec, "%s", progs[cur_prog].spec);
                if (prompt_text(_("Custom flashrom -p programmer:"), spec, sizeof spec) && spec[0]) {
                    snprintf(progs[cur_prog].spec, sizeof progs[cur_prog].spec, "%s", spec);
                    snprintf(progs[cur_prog].label, sizeof progs[cur_prog].label, "custom");
                }
            } else if (key == '\n' && *cur_sel == SPI_ROW_CHIP) {
                prompt_text(_("Chip name (-c, empty for auto):"), spi_chip, sizeof spi_chip);
            } else if ((key == K_LEFT || key == K_RIGHT || key == '\n') && *cur_sel == SPI_ROW_REGIONS) {
                int step = key == K_LEFT ? NREGION_PRESETS - 1 : 1;
                cur_region_preset = (cur_region_preset + step) % NREGION_PRESETS;
            } else if (key == '\n' && *cur_sel == SPI_ROW_BACKUP_DIR) {
                char chosen[PATH_MAX] = "";
                if (files_pick(backup_dir, NULL, "inode/directory",
                               _("Choose backup folder (o on folder):"), chosen, sizeof chosen)) {
                    snprintf(backup_dir, sizeof backup_dir, "%s", chosen);
                }
            } else if (!flashrom_missing && key == 'p') {
                char *argv[8];
                int n = 0;
                argv[n++] = FW_HELPER;
                argv[n++] = "spi-probe";
                argv[n++] = progs[cur_prog].spec;
                if (spi_chip[0]) { argv[n++] = "--chip"; argv[n++] = spi_chip; }
                argv[n] = NULL;
                start_job(_("SPI probe"), argv);
            } else if (!flashrom_missing && key == 'b') {
                const char *regs = region_presets[cur_region_preset];
                char *argv[10];
                int n = 0;
                argv[n++] = FW_HELPER;
                argv[n++] = "spi-backup";
                argv[n++] = progs[cur_prog].spec;
                argv[n++] = backup_dir;
                if (spi_chip[0]) { argv[n++] = "--chip"; argv[n++] = spi_chip; }
                if (regs[0])     { argv[n++] = "--regions"; argv[n++] = (char *)regs; }
                argv[n] = NULL;
                start_job(_("SPI 3x verified backup"), argv);
            } else if (!flashrom_missing && (key == 'v' || key == 'w')) {
                char chosen[PATH_MAX] = "";
                if (files_pick(backup_dir, NULL, "", _("Choose ROM/BIN image (Enter):"), chosen, sizeof chosen)) {
                    if (key == 'v') {
                        const char *regs = region_presets[cur_region_preset];
                        char *argv[10];
                        int n = 0;
                        argv[n++] = FW_HELPER;
                        argv[n++] = "spi-verify";
                        argv[n++] = progs[cur_prog].spec;
                        argv[n++] = chosen;
                        if (spi_chip[0]) { argv[n++] = "--chip"; argv[n++] = spi_chip; }
                        if (regs[0])     { argv[n++] = "--regions"; argv[n++] = (char *)regs; }
                        argv[n] = NULL;
                        start_job(_("SPI verify"), argv);
                    } else {
                        struct stat st;
                        if (stat(chosen, &st) < 0 || st.st_size <= 0) {
                            say_notice(1, _("Cannot read %s."), chosen);
                        } else if (!region_presets[cur_region_preset][0] && detected_bytes > 0 &&
                                   (long long)st.st_size != detected_bytes) {
                            say_notice(1, _("Refused: image is %lld B, but probed chip is %lld B."),
                                       (long long)st.st_size, detected_bytes);
                        } else {
                            snprintf(pending_path, sizeof pending_path, "%s", chosen);
                            confirming = CONF_SPI_WRITE;
                            say_notice(1, _("Y/N: Write %.100s (%lld KiB) to %s [%s]?"),
                                       chosen, (long long)st.st_size / 1024, progs[cur_prog].spec,
                                       _(region_labels[cur_region_preset]));
                        }
                    }
                }
            }
        }
        draw();
    }
}
