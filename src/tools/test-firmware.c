/*
 * Drive firmware (the TUI of fwupd, BIOS settings and flashrom) by its keys
 * and pointer over a hermetic helper script double.
 *   test-firmware DIR
 * Build with base/ui.c.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char dir[256];
static char *dpath(const char *rel);

#define FW_HELPER     dpath("helper")
#define PKG_HELPER    dpath("pkg")
#define FWUPD_PATH    dpath("fwupdmgr")
#define FLASHROM_PATH dpath("flashrom")
#define main firmware_main
#include "../progs/firmware.c"
#include "i18n-stub.h"
#undef main

static char *dpath(const char *rel)
{
    static char bufs[8][512];
    static int idx;
    idx = (idx + 1) % 8;
    snprintf(bufs[idx], sizeof bufs[idx], "%s/%s", dir, rel);
    return bufs[idx];
}

#define COLS 120
#define ROWS 32
static char grid[ROWS][COLS + 1], shown[ROWS][COLS + 1];
static const int *script;
static int step, mods;
static struct pointer_event mouse;
static char picked_file[PATH_MAX];

/* A key with modifiers in a script, as the key layer gives it: Ctrl+Q is
 * 17 with KEYS_CTRL, Alt+y is 'y' with KEYS_ALT. */
#define KEY(k, m) (-1000000 - ((m) << 16) - (k))
#define CTRL_Q KEY('q' & 0x1F, KEYS_CTRL)

void say(const char *fmt, ...) { (void)fmt; }

/* The system jobs' queue (jobs.h) as a double: a job runs at once, the
 * helper doubles as the service would run them (its kinds' commands), its
 * output in DIR/job-ID.log; the queue's own behaviour is test-jobs.sh's. */
#define MAX_JOBS 16
static pid_t job_pids[MAX_JOBS + 1];
static int job_status[MAX_JOBS + 1], njobs_run;

int jobs_submit(const char *kind, const char *const args[], int *id, int *position, char *why, size_t size)
{
    static const char *const fixed[][4] = {
        { "fw-refresh", "fwupd-action", "refresh" }, { "fw-update", "fwupd-action", "update" },
        { "fw-verify", "fwupd-action", "verify" }, { "fw-unlock", "fwupd-action", "unlock" },
        { "fw-local-install", "fwupd-action", "local-install" },
        { "fw-install-release", "fwupd-action", "install-release" }, { "spi-probe", "spi-probe" },
        { "spi-backup", "spi-backup" }, { "spi-verify", "spi-verify" }, { "spi-write", "spi-write" },
        { "pkg-install", "install" },
    };
    const char *argv[32];
    int n = 0;
    assert(njobs_run < MAX_JOBS);
    argv[n++] = strcmp(kind, "pkg-install") == 0 ? PKG_HELPER : FW_HELPER;
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        if (strcmp(kind, fixed[i][0]) != 0) continue;
        for (int k = 1; k < 4 && fixed[i][k]; k++) argv[n++] = fixed[i][k];
    }
    assert(n > 1);
    for (int i = 0; args[i]; i++) argv[n++] = args[i];
    argv[n] = NULL;
    int job = ++njobs_run;
    char log[512];
    jobs_log_path(job, log, sizeof log);
    pid_t pid = fork();
    if (pid == 0) {
        FILE *out = freopen(log, "w", stdout);
        if (!out || dup2(STDOUT_FILENO, STDERR_FILENO) < 0) _exit(126);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    assert(pid > 0);
    job_pids[job] = pid;
    job_status[job] = -1;
    *id = job;
    *position = 0;
    snprintf(why, size, "ok");
    return 0;
}

int jobs_status(int id, struct jobs_row *row, char *why, size_t size)
{
    (void)why;
    (void)size;
    assert(id > 0 && id <= njobs_run);
    int st;
    if (job_status[id] < 0 && waitpid(job_pids[id], &st, WNOHANG) == job_pids[id]) {
        job_status[id] = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    memset(row, 0, sizeof *row);
    row->id = id;
    row->status = job_status[id];
    snprintf(row->state, sizeof row->state, "%s", job_status[id] < 0 ? "running" : job_status[id] ? "failed" : "done");
    return 0;
}

int jobs_ended(const struct jobs_row *row) { return strcmp(row->state, "running") != 0; }
void jobs_log_path(int id, char *out, size_t size) { snprintf(out, size, "%s/job-%d.log", dir, id); }
int jobs_mine(struct jobs_row *rows, int max) { (void)rows; (void)max; return 0; }
const char *jobs_state_label(const char *state) { return state; }
int screen_open(const char *font) { (void)font; return 1; }
int keys_open(void) { return 1; }
int screen_cols(void) { return COLS; }
int screen_rows(void) { return ROWS; }
int screen_char_cells(uint32_t cp) { (void)cp; return 1; }
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
const struct pointer_event *pointer_last(void) { return &mouse; }
int keys_modifiers(void) { return mods; }

void screen_fill(int col, int row, int cols, int rows, uint32_t rgb)
{
    (void)rgb;
    for (int r = row; r < row + rows && r < ROWS; r++) {
        if (r < 0) continue;
        for (int c = col; c < col + cols && c < COLS; c++) {
            if (c >= 0) grid[r][c] = ' ';
        }
        grid[r][COLS] = '\0';
    }
}

void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    (void)fg;
    (void)bg;
    if (row < 0 || row >= ROWS || col < 0) return;
    for (int i = 0; s[i] && col + i < COLS; i++) {
        grid[row][col + i] = (unsigned char)s[i] >= 32 && (unsigned char)s[i] < 127 ? s[i] : '?';
    }
}

void screen_flush(void)
{
    memcpy(shown, grid, sizeof grid);
}

void open_user_dirs(char *h, size_t hs, char *ds, size_t dss)
{
    snprintf(h, hs, "%s", dir);
    snprintf(ds, dss, "Documents");
}

int files_pick(const char *start_dir, const char *keep, const char *type,
               const char *what, char *out, size_t size)
{
    (void)start_dir;
    (void)keep;
    (void)type;
    (void)what;
    if (!picked_file[0]) return 0;
    snprintf(out, size, "%s", picked_file);
    return 1;
}

int files_close_asked(void) { return 0; }

int key_poll_timeout(int fd, int timeout)
{
    (void)fd;
    (void)timeout;
    int k = script[step];
    mods = 0;
    if (k == 0) {
        mods = KEYS_CTRL; /* the script has ended: Ctrl+Q */
        return 'q' & 0x1F;
    }
    step++;
    if (k == -1) {
        usleep(40000);
        return 0;
    }
    if (k <= -1000000) {
        k = -1000000 - k;
        mods = k >> 16;
        return k & 0xFFFF;
    }
    return k;
}

static int count_of(const char *haystack, const char *needle)
{
    int n = 0;
    for (const char *p = haystack; (p = strstr(p, needle)) != NULL; p++) n++;
    return n;
}

static void read_calls(char *calls, size_t size)
{
    FILE *cf = fopen(dpath("calls"), "r");
    assert(cf);
    size_t nr = fread(calls, 1, size - 1, cf);
    fclose(cf);
    calls[nr] = '\0';
}

static int screen_contains(const char *needle)
{
    for (int r = 0; r < ROWS; r++) {
        if (strstr(shown[r], needle)) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0); /* a line at once: the job double forks, and a child flushes what is buffered */
    assert(argc == 2);
    snprintf(dir, sizeof dir, "%s", argv[1]);
    assert(mkdir(dir, 0755) == 0);

    /* Executable markers for fwupdmgr and flashrom */
    const char *bins[] = { "fwupdmgr", "flashrom", "pkg" };
    for (int i = 0; i < 3; i++) {
        FILE *f = fopen(dpath(bins[i]), "w");
        assert(f);
        fprintf(f, "#!/bin/sh\necho '%s' \"$@\" >> %s/calls\n", bins[i], dir);
        fclose(f);
        assert(chmod(dpath(bins[i]), 0755) == 0);
    }

    FILE *hf = fopen(FW_HELPER, "w");
    assert(hf);
    fprintf(hf,
            "#!/bin/sh\n"
            "echo \"$@\" >> %s/calls\n"
            "case \"$1\" in\n"
            "  fwupd-devices) printf 'sys01\\tSystem Firmware\\tLENOVO\\t0.1.27\\t0.1.29\\tupdate:0.1.29\\n' ;;\n"
            "  fwupd-releases) printf '0.1.29\\tupgrade\\tlvfs\\thttps://lvfs/29.cab\\tFixes S3\\n"
            "0.1.25\\tdowngrade\\tlvfs\\thttps://lvfs/25.cab\\tOlder release\\n' ;;\n"
            "  bios-list) printf 'SecurityChip\\tInactive\\tActive;Inactive;Disable\\tenumeration\\tTPM\\n' ;;\n"
            "  bios-set) printf '%%s = %%s\\n' \"$2\" \"$3\" ;;\n"
            "  spi-programmers) printf 'internal\\t1\\tInternal SPI\\nch341a_spi\\t1\\tWCH CH341A\\n' ;;\n"
            "  spi-probe) printf 'CHIP\\tW25Q64.V\\t4096\\tfd,gbe,bios\\nProbed OK\\n' ;;\n"
            "  *) echo \"done $*\" ;;\n"
            "esac\n", dir);
    fclose(hf);
    assert(chmod(FW_HELPER, 0755) == 0);

    /* Create one 2048-byte wrong-sized ROM and one 4096-byte matching ROM */
    char bad_rom[512], good_rom[512];
    snprintf(bad_rom, sizeof bad_rom, "%s/bad.rom", dir);
    snprintf(good_rom, sizeof good_rom, "%s/good.rom", dir);
    FILE *rf = fopen(bad_rom, "wb");
    assert(rf);
    char zeros[4096] = { 0 };
    assert(fwrite(zeros, 1, 2048, rf) == 2048);
    fclose(rf);
    rf = fopen(good_rom, "wb");
    assert(rf);
    assert(fwrite(zeros, 1, 4096, rf) == 4096);
    fclose(rf);

    /* 0. Esc, q and Alt+q at the top do not quit, nor does home's key; F1
     * shows the keys (Esc closes them); Ctrl+Q quits, as the window's close
     * does. */
    script = (const int[]){ 27, 'q', KEY('q', KEYS_ALT), K_ACT_HOME, K_F1, 27, KEY('q' & 0x1F, KEYS_CTRL), 0 };
    step = 0;
    assert(firmware_main() == 0 && step == 7);
    assert(screen_contains("Ctrl+Q: quit") && !screen_contains("q: home"));
    script = (const int[]){ K_F1, CTRL_Q, 0 }; /* Ctrl+Q in the help quits too */
    step = 0;
    assert(firmware_main() == 0 && step == 2);
    script = (const int[]){ K_CLOSE, 0 };
    step = 0;
    assert(firmware_main() == 0 && step == 1);
    puts("PASS firmware: Esc, q, Alt+q and home's key at the top stay; F1 helps; Ctrl+Q and the window's close quit");

    /* 1. Devices page -> Enter releases -> select downgrade -> N cancel -> Y confirm -> wait -> Esc -> Tab BIOS -> Right cycles -> Tab SPI -> Right selects ch341a_spi -> 'p' probe -> wait;
     * q in the releases goes back as Esc does; Shift+Tab goes back a page. */
    script = (const int[]){
        '\n', K_DOWN, '\n', 'n', '\n', 'y', -1, -1, 27,
        '\n', 'q', KEY('\t', KEYS_SHIFT), KEY('\t', KEYS_SHIFT), K_RIGHT,
        '\t', K_RIGHT, 'p', -1, -1,
        CTRL_Q, 0
    };
    step = 0;
    assert(firmware_main() == 0);
    assert(detected_bytes == 4096 && strstr(detected_chip, "W25Q64.V") != NULL);
    assert(page == PAGE_SPI && !in_releases);

    /* 2. SPI write guard: wrong-sized 2048B image is refused before confirmation; 4096B image asks Y/N, N cancels, Y writes */
    snprintf(picked_file, sizeof picked_file, "%s", bad_rom);
    script = (const int[]){ 'w', CTRL_Q, 0 };
    step = 0;
    assert(firmware_main() == 0);
    assert(screen_contains("Refused: image is 2048 B, but probed chip is 4096 B."));

    /* The question: Alt+y, home's key, q and other keys are no answer; it
     * waits (Ctrl+Q quits with it unanswered, nothing written); Enter is no. */
    char calls[4096] = "";
    snprintf(picked_file, sizeof picked_file, "%s", good_rom);
    script = (const int[]){ 'w', KEY('y', KEYS_ALT), K_ACT_HOME, 'q', 'x', K_SHOWN, CTRL_Q, 0 };
    step = 0;
    assert(firmware_main() == 0 && confirming == CONF_SPI_WRITE);
    confirming = CONF_NONE;
    script = (const int[]){ 'w', '\n', CTRL_Q, 0 };
    step = 0;
    assert(firmware_main() == 0 && confirming == CONF_NONE && screen_contains("Cancelled; nothing written."));
    read_calls(calls, sizeof calls);
    assert(count_of(calls, "spi-write") == 0);
    puts("PASS firmware: a question waits through Alt+y, home's key, q and a return to the screen; Enter is no");

    script = (const int[]){ 'w', 'n', 'w', 'y', -1, -1, CTRL_Q, 0 };
    step = 0;
    assert(firmware_main() == 0);

    read_calls(calls, sizeof calls);
    assert(strstr(calls, "fwupd-action install-release sys01 https://lvfs/25.cab") != NULL);
    assert(strstr(calls, "bios-set SecurityChip Disable") != NULL);
    assert(strstr(calls, "spi-probe ch341a_spi") != NULL);
    assert(strstr(calls, "spi-write ch341a_spi") != NULL);
    assert(strstr(calls, "--expected-size 4096") != NULL);
    assert(!strstr(calls, "bad.rom"));
    assert(count_of(calls, "spi-write") == 1);

    puts("test-firmware C UI: fwupd releases, BIOS settings, SPI probe, size guard and Y/N confirmations PASSED.");
    return 0;
}
