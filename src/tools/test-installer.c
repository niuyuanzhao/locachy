/*
 * Drive installer (the live medium's full-screen installation) by its keys,
 * with the screen and keyboard as test doubles and locachy-install, term
 * and systemctl as scripts: locachy-install answers --describe-json from a
 * fixed description (the medium's own source: the newest was not fetched),
 * writes down how it was called (and what its secret files hold, and their
 * modes), and prints the progress lines of an installation (the newest
 * source), or a refusal or a failure (the medium's own) when told to, or
 * a stop for a source signing key to trust (then --resume goes on);
 * keytable writes a table of two keys, or fails when told to.
 *   test-installer DIR   (DIR: a new directory for the doubles' files)
 * Build with base/ui.c and json-c.
 */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[256];
#define D(rel) (dir_path(rel))
static char *dir_path(const char *rel);

#define INSTALL_CMD D("locachy-install")
#define TERM_CMD D("term")
#define NMTUI "/usr/bin/nmtui"
#define SYSTEMCTL D("systemctl")
#define RUN_DIR D("run")
#define KEYTABLE D("keytable")
#define KEYMAP_TABLE D("keymap.table")
#define main installer_main
#include "../progs/installer.c"
#include "i18n-stub.h"
#undef main

static char *dir_path(const char *rel)
{
    static char paths[8][512];
    static int i;
    i = (i + 1) % 8;
    snprintf(paths[i], sizeof paths[i], "%s/%s", dir, rel);
    return paths[i];
}

/* The screen: what was last flushed, a text line per row. */
#define COLS 150
#define ROWS 48
static char grid[ROWS][COLS + 1], shown_grid[ROWS][COLS + 1];
static char said[16384];

void say(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(said);
    va_start(ap, fmt);
    vsnprintf(said + n, sizeof said - n, fmt, ap);
    va_end(ap);
    n = strlen(said);
    if (n + 1 < sizeof said) strcat(said, "\n");
}
int screen_open(const char *font) { (void)font; return 1; }
void screen_release(void) {}
int keys_open(void) { return 1; }
void keys_release(void) {}
int pointer_open(void) { return -1; }
int *pointer_lend(size_t *count, char paths[][POINTER_PATH], size_t max) { (void)paths; (void)max; *count = 0; return NULL; }
int pointer_take(int fd, const char *path) { (void)fd; (void)path; return 0; }
void pointer_release(void) {}
/* keys.c's table: what the last one read said (empty: US, without one). */
static char layout_table[4096];
int keys_layout_load(const char *path)
{
    layout_table[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(layout_table, 1, sizeof layout_table - 1, f);
    layout_table[n] = '\0';
    fclose(f);
    return strstr(layout_table, "key ") ? 0 : -1;
}
int screen_cols(void) { return COLS; }
int screen_rows(void) { return ROWS; }
int screen_char_cells(uint32_t cp) { (void)cp; return 1; }
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
void base_status_fields(struct status_fields *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->network, sizeof out->network, "Wired eth0");
    snprintf(out->address, sizeof out->address, "10.0.2.15");
}
void screen_fill(int col, int row, int cols, int rows, uint32_t rgb)
{
    (void)rgb;
    for (int r = row; r < row + rows && r < ROWS; r++) {
        for (int c = col; c < col + cols && c < COLS; c++) grid[r][c] = ' ';
    }
}
void screen_text(int col, int row, const char *s, uint32_t fg, uint32_t bg)
{
    (void)fg;
    (void)bg;
    for (int c = col; *s && c < COLS && row >= 0 && row < ROWS; c++, s++) grid[row][c] = *s;
}
void screen_flush(void)
{
    memcpy(shown_grid, grid, sizeof shown_grid);
}

/* The keys: a script, built with k() and type(); WAIT lets time pass (the
 * installation goes on); SNAP keeps what is on the screen then (snapped
 * looks there). At its end the program is left (it has no quit key). */
#define WAIT (-1)
#define SNAP (-2)
#define END (-3)
static int keys[1024], nkeys, at;
static char snaps[16][ROWS][COLS + 1];
static int nsnaps;
static jmp_buf quit;
static struct pointer_event mouse;
const struct pointer_event *pointer_last(void) { return &mouse; }
/* PTR in the keys: the next of these pointer events (ptr() adds one). */
#define PTR (-4)
static struct pointer_event ptrs[16];
static int nptrs, at_ptr;
static void k(int key);
static void ptr(int col, int row, int button, int pressed, int clicks)
{
    assert(nptrs < 16);
    ptrs[nptrs++] = (struct pointer_event){ .col = col, .row = row, .button = button, .pressed = pressed,
                                            .clicks = clicks, .buttons = pressed || !button ? 1 : 0 };
    k(PTR);
}
/* The clipboard: what was copied last, and to which selection. */
static char copied[2][2048];
int clipboard_set(int primary, const void *text, size_t size)
{
    snprintf(copied[primary ? 1 : 0], sizeof copied[0], "%.*s", (int)size, (const char *)text);
    return CLIPBOARD_OK;
}
const char *clipboard_error(int status) { return status == CLIPBOARD_OK ? "ok" : "failed"; }
/* K_TEXT's text: a character beyond ASCII, as keys.c gives it (de: ü). */
const char *keys_text(void) { return "ü"; }

static void k(int key)
{
    assert(nkeys < 1023);
    keys[nkeys++] = key;
    keys[nkeys] = END;
}

static void type(const char *s)
{
    for (; *s; s++) k(*s);
}

int key_poll_timeout(int fd, int ms)
{
    (void)fd;
    (void)ms;
    int key = keys[at];
    if (key == END) longjmp(quit, 1);
    at++;
    if (key == WAIT) {
        usleep(300000);
        return 0;
    }
    if (key == SNAP) {
        assert(nsnaps < 16);
        memcpy(snaps[nsnaps++], shown_grid, sizeof shown_grid);
        return 0;
    }
    if (key == PTR) {
        assert(at_ptr < nptrs);
        mouse = ptrs[at_ptr++];
        return K_POINTER;
    }
    return key;
}

int key_get(void)
{
    return key_poll_timeout(-1, -1);
}

static int snapped(const char *text)
{
    for (int s = 0; s < nsnaps; s++) {
        for (int r = 0; r < ROWS; r++) {
            if (strstr(snaps[s][r], text)) return 1;
        }
    }
    return 0;
}

static char *slurp(const char *path)
{
    static char buf[65536];
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    return buf;
}

static void write_file(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
    chmod(path, mode);
}

static const char describe[] =
    "{\"defaults\": {\"user\": \"user\", \"hostname\": \"locachy\", \"keymap\": \"us\", \"timezone\": \"auto\","
    " \"swap_gib\": 8, \"level\": \"auto\", \"cpu\": \"auto\", \"repos\": [\"core\", \"extra\", \"multilib\"],"
    " \"update_repo\": \"https://github.com/niuyuanzhao/locachy.git\", \"update_branch\": \"main\", \"copy_network\": false},"
    " \"languages\": [\"en_US.UTF-8\", \"zh_CN.UTF-8\"],"
    " \"levels\": {\"supported\": [\"x86-64\", \"x86-64-v2\"], \"auto\": \"x86-64-v2\", \"cpu_model\": \"ivybridge\"},"
    " \"ime\": [{\"name\": \"off\", \"label\": \"OFF (no Rime packages)\"}, {\"name\": \"en\", \"label\": \"English\"},"
    " {\"name\": \"zh\", \"label\": \"Chinese Pinyin\"}],"
    " \"modules\": [{\"name\": \"desktop\", \"description\": \"Shared Wayland compositor\"},"
    " {\"name\": \"cjk-fonts\", \"description\": \"CJK fonts\"}, {\"name\": \"tor\", \"description\": \"Tor\"}],"
    " \"module_defaults\": {\"en_US.UTF-8\": [\"desktop\"], \"zh_CN.UTF-8\": [\"cjk-fonts\", \"desktop\"]},"
    " \"containers\": [{\"name\": \"firefox\", \"host\": false}, {\"name\": \"foot\", \"host\": true}],"
    " \"container_defaults\": [\"foot\"],"
    " \"machines\": [{\"name\": \"generic\", \"description\": \"Generic PC\"}, {\"name\": \"x230-agan\", \"description\": \"X230\"}],"
    " \"repo_sections\": [\"core\", \"extra\", \"multilib\"], \"keymaps\": [\"de\", \"us\"], \"timezones\": [\"UTC\", \"Asia/Shanghai\"],"
    " \"tpm\": {\"usable\": false, \"advice\": \"TPM 1.2 off in the firmware: a passphrase\"},"
    " \"source\": {\"text\": \"this medium's own, 0123456789ab; could not fetch\", \"not_newest\": true},"
    " \"network_profiles\": 0, \"memory_bytes\": 17179869184, \"esp_bytes\": 536870912, \"min_root_bytes\": 17179869184,"
    " \"disks\": [{\"path\": \"/dev/sda\", \"size\": 256060514304, \"model\": \"Samsung SSD\", \"tran\": \"sata\","
    " \"removable\": false, \"in_use\": \"\", \"children\": [{\"path\": \"/dev/sda1\", \"type\": \"part\", \"size\": 536870912,"
    " \"fstype\": \"vfat\", \"label\": \"EFI\", \"parttypename\": \"EFI System\", \"mountpoints\": []},"
    " {\"path\": \"/dev/sda3\", \"type\": \"part\", \"size\": 100000000000, \"fstype\": \"apfs\", \"label\": \"\","
    " \"parttypename\": \"Apple APFS\", \"mountpoints\": []}]},"
    " {\"path\": \"/dev/sdc\", \"size\": 8589934592, \"model\": \"small\", \"tran\": \"usb\", \"removable\": true,"
    " \"in_use\": \"\", \"children\": []}]}\n";

/* locachy-install: the description; else how it was called, its secret
 * files (content and mode), and an installation's progress lines. */
static const char fake_install[] =
    "#!/bin/sh\n"
    "d=$(dirname \"$0\")\n"
    "if [ \"$1\" = --describe-json ]; then cat \"$d/describe.json\"; exit 0; fi\n"
    "echo \"$*\" >>\"$d/calls\"\n"
    "for a in \"$@\"; do case $a in --luks-passphrase-file=*|--password-file=*) f=${a#*=};\n"
    "  printf '%s %s %s\\n' \"${a%%=*}\" \"$(stat -c %a \"$f\")\" \"$(cat \"$f\")\" >>\"$d/secrets\";; esac; done\n"
    "if [ \"$1\" = --check ]; then\n"
    "  if [ -e \"$d/check-fail\" ]; then echo 'usage: locachy-install [-h]' >&2;"
    " echo 'locachy-install: error: test refusal' >&2; exit 2; fi\n"
    "  echo 'Would partition'; exit 0\n"
    "fi\n"
    "if [ -e \"$d/keys-wanted\" ] && [ \"$1\" != --resume ]; then\n"
    "  echo '== x86-64-v2: build mounted target'\n"
    "  echo 'Source key to trust: lyrebird-proxy 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C'\n"
    "  echo 'Trust them: locachy-build --trust-key 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C'\n"
    "  echo 'locachy-install: Target build failed; see the log' >&2\n"
    "  echo 'Source key to trust: lyrebird-proxy 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C'\n"
    "  echo 'Trust them: locachy-install --trust-key 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C'\n"
    "  echo 'Continue with: locachy-install --resume'; exit 1\n"
    "fi\n"
    "if [ -e \"$d/install-fail\" ]; then\n"
    "  echo \"Source (not the newest): this medium's own, 0123456789ab; could not fetch\"\n"
    "else echo 'Source: 89abcdef0123 of https://github.com/niuyuanzhao/locachy.git (main), the newest, fetched now'; fi\n"
    "echo 'Root: /dev/sda3 at /run/locachy-install-target'\n"
    "echo 'Ranking the mirrors (reflector, rate-mirrors)...'\n"
    "echo '== x86-64-v2: build mounted target'\n"
    "echo '(1/3) downloading a-1'; echo '(1/3) installing a'; echo '(3/3) installing c'\n"
    "if [ -e \"$d/install-fail\" ]; then echo 'locachy-install: Target build failed; see the log' >&2; exit 1; fi\n"
    "echo '== sources and package helpers'; echo '== our part: make.sh'\n"
    "echo 'make-root: target built and verified'\n"
    "echo 'Installation completed. Packages for x86-64-v2, own builds for ivybridge. No reboot was requested.'\n";

static const char fake_keytable[] =
    "#!/bin/sh\n"
    "d=$(dirname \"$0\")\n"
    "if [ -e \"$d/keytable-fail\" ]; then echo \"keytable: no X keyboard layout for keymap $1\" >&2; exit 1; fi\n"
    "printf 'console %s\\nlayout %s\\nkey 21 y 7a 5a\\nkey 44 z 79 59\\n' \"$1\" \"$1\" >\"$2\"\n";

static const char fake_term[] = "#!/bin/sh\necho \"$*\" >>\"$(dirname \"$0\")/term-calls\"\n";
static const char fake_systemctl[] = "#!/bin/sh\necho \"$*\" >>\"$(dirname \"$0\")/systemctl-calls\"\n";

static void run(const char *what)
{
    printf("== %s\n", what);
    at = nsnaps = 0;
    said[0] = '\0';
    memset(&c, 0, sizeof c);
    described = 0;
    mode = M_START;
    job = 0;
    step = scroll = log_from = 0;
    in_page = 1;
    notice[0] = '\0';
    unsetenv("LOCACHY_LANG");
    for (int i = 0; i < ROWS; i++) {
        memset(grid[i], ' ', COLS);
        grid[i][COLS] = '\0';
    }
    unlink(D("calls"));
    unlink(D("secrets"));
    if (setjmp(quit) == 0) installer_main(1, (char *[]){ "installer", NULL });
    assert(keys[at] == END);
    nkeys = 0;
    keys[0] = END;
    nptrs = at_ptr = 0;
    sel.active = sel.dragging = 0;
}

/* From the start to the summary: the whole disk sda, a passphrase, the
 * user boot-test with a password. */
static void to_summary(const char *pass_again)
{
    k('\n');                    /* Install LOCachy */
    k(K_EOL); k('\n');          /* Network: Next */
    k(K_DOWN); k('\n');         /* Disk: the first disk */
    k(SNAP);
    k(K_EOL); k('\n');          /* Next */
    k(K_DOWN); k('\n'); type("correct horse"); k('\n'); type(pass_again); k('\n');
    k(K_EOL); k('\n');          /* Encryption: Next */
    k('\n'); k(21); type("boot-test"); k('\n');
    k(K_DOWN); k(K_DOWN); k('\n'); type("Locachy123"); k('\n'); type("Locachy123"); k('\n');
    for (int s = S_USER; s < S_SUMMARY; s++) { k(K_EOL); k('\n'); }
    k(SNAP);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(dir, sizeof dir, "%s", argv[1]);
    assert(mkdir(dir, 0755) == 0);
    write_file(D("describe.json"), describe, 0644);
    write_file(D("locachy-install"), fake_install, 0755);
    write_file(D("term"), fake_term, 0755);
    write_file(D("systemctl"), fake_systemctl, 0755);
    write_file(D("keytable"), fake_keytable, 0755);

    /* The start: the terminal comes and goes; the language. */
    k(K_DOWN); k('\n'); k(SNAP);
    k(K_BOL); k(K_RIGHT); k(SNAP);
    run("start: terminal and language");
    assert(access(D("term-calls"), F_OK) == 0);
    assert(strstr(said, "installer: started"));
    assert(strstr(said, "installer: running ") && strstr(said, " ended 0"));
    assert(snapped("Install LOCachy") && snapped("Terminal (a root shell, for repairs)"));
    assert(snapped("< 中文 >"));

    /* The layout, on the start page: no table for it, the layout kept. */
    write_file(D("keytable-fail"), "", 0644);
    k(K_BOL); k(K_DOWN); k('\n'); type("de"); k('\n'); k(SNAP);
    run("start: a layout without a table");
    assert(snapped("No key table for de: keytable: no X keyboard layout for keymap de"));
    assert(!strcmp(c.keymap, "us") && !layout_table[0] && access(D("keymap.table"), F_OK) != 0);
    assert(access(D("keymap.table.next"), F_OK) != 0);
    unlink(D("keytable-fail"));

    /* Its table: the installer (and the rescue terminal) type with it. */
    k(K_BOL); k(K_DOWN); k('\n'); type("de"); k('\n'); k(SNAP);
    run("start: the layout");
    assert(snapped("Keyboard layout") && snapped(" de"));
    assert(!strcmp(c.keymap, "de") && strstr(said, "installer: keymap de"));
    assert(strstr(layout_table, "layout de\n") && strstr(slurp(D("keymap.table")), "key 21 y 7a 5a"));

    /* The whole way, N at the last question, then Y and the installation. */
    to_summary("correct horse");
    k(K_EOL); k('\n'); k(SNAP); k('n');     /* Install: the confirmation, no */
    k(SNAP);
    k(K_EOL); k('\n'); k('y');              /* again: yes */
    for (int i = 0; i < 10; i++) k(WAIT);
    k(SNAP);
    run("install: summary, check, confirmation, progress, done");
    assert(snapped("erased: /dev/sda1"));
    assert(snapped("erased: /dev/sda3"));
    assert(snapped("Nothing is missing."));
    assert(snapped("LOCachy source") && snapped("the newest from the repository above, fetched when it installs"));
    assert(snapped("Now: this medium's own, 0123456789ab; could not fetch"));
    assert(snapped("Erase /dev/sda and install LOCachy on it?"));
    assert(snapped("Nothing was written."));
    assert(snapped("LOCachy is installed"));
    assert(snapped("[x] Download and install the packages") && snapped("fetched 1, installed 3 of 3"));
    assert(snapped("LOCachy source: 89abcdef0123 of https://github.com/niuyuanzhao/locachy.git (main), the newest"));
    assert(strstr(said, "installer: installation completed"));
    const char *calls = slurp(D("calls"));
    printf("%s", calls);
    assert(strstr(calls, "--check --disk=/dev/sda --swap-gib=8 --encrypt --luks-passphrase-file="));
    assert(strstr(calls, "--yes --disk=/dev/sda --swap-gib=8 --encrypt --luks-passphrase-file="));
    assert(strstr(calls, "--user=boot-test --hostname=locachy --language=en_US.UTF-8 --keymap=us --timezone=auto"));
    assert(strstr(calls, "--ime=off --no-copy-network --features=desktop --containers=foot --level=auto --cpu=auto"));
    assert(strstr(calls, "--repos=core,extra,multilib --update-repo=https://github.com/niuyuanzhao/locachy.git --update-branch=main"));
    assert(!strstr(calls, "correct horse") && !strstr(calls, "Locachy123"));
    const char *secrets = slurp(D("secrets"));
    printf("%s", secrets);
    assert(strstr(secrets, "--luks-passphrase-file 600 correct horse\n"));
    assert(strstr(secrets, "--password-file 600 Locachy123\n"));
    assert(access(D("run/passphrase"), F_OK) != 0 && access(D("run/password"), F_OK) != 0);

    /* A full name beyond ASCII (K_TEXT, keys.c's ü of a de table): typed as
     * it is; Backspace takes the whole character. */
    k('\n');                    /* Install LOCachy */
    k(K_EOL); k('\n');          /* Network: Next */
    k(K_DOWN); k('\n');         /* Disk: the first disk */
    k(K_EOL); k('\n');
    k(K_DOWN); k('\n'); type("correct horse"); k('\n'); type("correct horse"); k('\n');
    k(K_EOL); k('\n');          /* Encryption: Next */
    k('\n'); k(21); type("boot-test"); k('\n');
    k(K_DOWN); k('\n'); type("J"); k(K_TEXT); k(K_TEXT); k('\b'); type("rgen"); k('\n');
    k(K_DOWN); k('\n'); type("Locachy123"); k('\n'); type("Locachy123"); k('\n');
    for (int s = S_USER; s < S_SUMMARY; s++) { k(K_EOL); k('\n'); }
    k(K_EOL); k('\n'); k('n');  /* Install: --check, then no */
    run("a full name beyond ASCII");
    assert(strstr(slurp(D("calls")), "--user=boot-test --full-name=Jürgen --hostname=locachy"));

    /* Another layout after the passphrase and the password: both again. */
    to_summary("correct horse");
    k(27); k(27); k(K_BOL); k(K_DOWN); k('\n'); type("de"); k('\n'); k(SNAP);
    run("layout changed after the secrets");
    assert(snapped("The passphrase and the password were typed with the other layout: set them again."));
    assert(!strcmp(c.keymap, "de") && !c.passphrase[0] && !c.password[0]);

    /* The two passphrases differ: not set, and the summary says so. */
    to_summary("correct hose");
    run("passphrase typed differently");
    assert(snapped("! Encryption: Set the disk passphrase."));

    /* No usable TPM: off and a passphrase only. */
    k('\n'); k(K_PGDN); k(K_PGDN); k(K_RIGHT); k(SNAP); k(K_RIGHT); k(SNAP); k(K_RIGHT); k(SNAP);
    run("encryption without a usable TPM");
    assert(snapped("< off >") && snapped("< with a passphrase >") && !snapped("TPM auto-unlock >"));

    /* locachy-install refuses at --check: why, and nothing more. */
    write_file(D("check-fail"), "", 0644);
    to_summary("correct horse");
    k(K_EOL); k('\n'); k(SNAP);
    run("check refused");
    assert(snapped("locachy-install does not take it: locachy-install: error: test refusal"));
    assert(!strstr(slurp(D("calls")), "--yes"));
    unlink(D("check-fail"));

    /* The installation fails: the failure, its reason, the way back. */
    write_file(D("install-fail"), "", 0644);
    to_summary("correct horse");
    k(K_EOL); k('\n'); k('y');
    for (int i = 0; i < 6; i++) k(WAIT);
    k(SNAP); k('b'); k(SNAP);
    run("installation failed");
    assert(snapped("The installation failed."));
    assert(snapped("locachy-install: Target build failed; see the log"));
    assert(snapped("LOCachy source: this medium's own, 0123456789ab; could not fetch"));
    assert(snapped("Summary"));
    assert(strstr(said, "installer: installation failed (1)"));
    unlink(D("install-fail"));

    /* A source signing key to trust: the installation waits, says which and
     * the command (y copies it); text is selected with the pointer and
     * copied; c goes on where it stopped (--resume), the secrets again. */
    write_file(D("keys-wanted"), "", 0644);
    to_summary("correct horse");
    k(K_EOL); k('\n'); k('y');
    for (int i = 0; i < 6; i++) k(WAIT);
    k(SNAP); k('y'); k(SNAP);
    ptr(2, PAGE_TOP, BTN_LEFT, 1, 1); ptr(12, PAGE_TOP + 1, 0, 0, 0); ptr(12, PAGE_TOP + 1, BTN_LEFT, 0, 0);
    k('y');
    ptr(10, PAGE_TOP + 1, BTN_LEFT, 1, 3); ptr(10, PAGE_TOP + 1, BTN_LEFT, 0, 0);
    k('c');
    for (int i = 0; i < 10; i++) k(WAIT);
    k(SNAP);
    run("keys to trust, copying, continuing");
    assert(snapped("Waiting for you: source signing keys to trust. None is trusted by itself."));
    assert(snapped("lyrebird-proxy") && snapped("40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C"));
    assert(snapped("locachy-install --trust-key 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C"));
    assert(snapped("c: continue") && !snapped("b: back to the summary"));
    assert(snapped("Copied: locachy-install --trust-key 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C"));
    printf("copied: [%s] [%s]\n", copied[0], copied[1]);
    fflush(stdout);
    assert(strncmp(copied[0], "Waiting for you: source signing keys", 36) == 0);
    assert(strstr(copied[0], "None is trusted by itself.\n    lyrebird-") && !strstr(copied[0], "lyrebird-p"));
    assert(strncmp(copied[1], "lyrebird-proxy ", 15) == 0);
    assert(strcmp(copied[1] + strlen(copied[1]) - 41, " 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C") == 0);
    assert(snapped("LOCachy is installed"));
    assert(strstr(said, "installer: continuing") && strstr(said, "installer: installation completed"));
    calls = slurp(D("calls"));
    printf("%s", calls);
    assert(strstr(calls, "--resume --yes --luks-passphrase-file="));
    assert(strstr(calls, "--update-repo=https://github.com/niuyuanzhao/locachy.git --update-branch=main"));
    secrets = slurp(D("secrets"));
    assert(strstr(strstr(secrets, "--password-file 600 Locachy123\n") + 1, "--password-file 600 Locachy123\n"));
    const char *log = slurp(D("run/install.log"));
    assert(strstr(log, "Continue with: locachy-install --resume") && strstr(log, "Installation completed."));
    unlink(D("keys-wanted"));

    printf("PASS installer\n");
    return 0;
}
