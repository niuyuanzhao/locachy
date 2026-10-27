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
#include <unistd.h>

static char dir[256];
static char *dpath(const char *rel);

#define FW_JOB_LOG    dpath("fw.log")
#define FW_JOB_PID    dpath("fw.pid")
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
static int step;
static struct pointer_event mouse;
static char picked_file[PATH_MAX];

void say(const char *fmt, ...) { (void)fmt; }
int screen_open(const char *font) { (void)font; return 1; }
int keys_open(void) { return 1; }
int screen_cols(void) { return COLS; }
int screen_rows(void) { return ROWS; }
int screen_char_cells(uint32_t cp) { (void)cp; return 1; }
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
const struct pointer_event *pointer_last(void) { return &mouse; }
int keys_modifiers(void) { return 0; }

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

int key_poll_timeout(int fd, int timeout)
{
    (void)fd;
    (void)timeout;
    int k = script[step];
    if (k == 0) return 'q';
    step++;
    if (k == -1) {
        usleep(40000);
        return 0;
    }
    return k;
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

    /* 1. Devices page -> Enter releases -> select downgrade -> N cancel -> Y confirm -> wait -> Esc -> Tab BIOS -> Right cycles -> Tab SPI -> Right selects ch341a_spi -> 'p' probe -> wait */
    script = (const int[]){
        '\n', K_DOWN, '\n', 'n', '\n', 'y', -1, -1, 27,
        '\t', K_RIGHT,
        '\t', K_RIGHT, 'p', -1, -1,
        'q', 0
    };
    step = 0;
    assert(firmware_main() == 0);
    assert(detected_bytes == 4096 && strstr(detected_chip, "W25Q64.V") != NULL);

    /* 2. SPI write guard: wrong-sized 2048B image is refused before confirmation; 4096B image asks Y/N, N cancels, Y writes */
    snprintf(picked_file, sizeof picked_file, "%s", bad_rom);
    script = (const int[]){ 'w', 'q', 0 };
    step = 0;
    assert(firmware_main() == 0);
    assert(screen_contains("Refused: image is 2048 B, but probed chip is 4096 B."));

    snprintf(picked_file, sizeof picked_file, "%s", good_rom);
    script = (const int[]){ 'w', 'n', 'w', 'y', -1, -1, 'q', 0 };
    step = 0;
    assert(firmware_main() == 0);

    char calls[4096] = "";
    FILE *cf = fopen(dpath("calls"), "r");
    assert(cf);
    size_t nr = fread(calls, 1, sizeof calls - 1, cf);
    fclose(cf);
    calls[nr] = '\0';
    assert(strstr(calls, "fwupd-action install-release sys01 https://lvfs/25.cab") != NULL);
    assert(strstr(calls, "bios-set SecurityChip Disable") != NULL);
    assert(strstr(calls, "spi-probe ch341a_spi") != NULL);
    assert(strstr(calls, "spi-write ch341a_spi") != NULL);
    assert(strstr(calls, "--expected-size 4096") != NULL);
    assert(!strstr(calls, "bad.rom"));

    puts("test-firmware C UI: fwupd releases, BIOS settings, SPI probe, size guard and Y/N confirmations PASSED.");
    return 0;
}
