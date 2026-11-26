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
 * Build with base/ui.c, base/textsel.c and json-c.
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
#define RESCUE_CMD D("rescue")
#define NMTUI "/usr/bin/nmtui"
#define SYSTEMCTL D("systemctl")
#define RUN_DIR D("run")
#define KEYTABLE D("keytable")
#define KEYMAP_TABLE D("keymap.table")
/* Leaving setup is counted, and the run ends there (installer.c exits). */
static void test_leave(void);
#define LEAVE_SETUP() test_leave()
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
void keys_leds(int on) { (void)on; }
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
 * looks there); KEY(k, m) is key k with modifiers m, as the key layer gives
 * it (Ctrl+A: 1 with KEYS_CTRL). At its end the program is left (it has no
 * quit key). */
#define WAIT (-1)
#define SNAP (-2)
#define END (-3)
#define KEY(k, m) (-1000000 - ((m) << 16) - (k))
static int keys[1024], nkeys, at, mods, kept, left;
int keys_modifiers(void) { return mods; }
void key_close_kept(void) { kept++; }
static char snaps[16][ROWS][COLS + 1];
static int nsnaps;
static jmp_buf quit;
static void test_leave(void)
{
    left++;
    longjmp(quit, 1);
}
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
/* The clipboard's text for a paste (Ctrl+V in a line): a pipe holding it. */
static const char *paste_text = "";
int clipboard_get(int primary, int *fd, size_t *size)
{
    (void)primary;
    int p[2];
    size_t n = strlen(paste_text);
    if (pipe(p) != 0) return -1;
    if (write(p[1], paste_text, n) != (ssize_t)n) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    close(p[1]);
    *fd = p[0];
    *size = n;
    return 0;
}
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
    mods = 0;
    if (key == END) longjmp(quit, 1);
    at++;
    if (key <= KEY(0, 0)) {
        key = KEY(0, 0) - key;
        mods = key >> 16;
        return key & 0xFFFF;
    }
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
    " \"update_repo\": \"https://github.com/niuyuanzhao/locachy.git\", \"update_branch\": \"main\","
    " \"apps_repo\": \"https://github.com/niuyuanzhao/locachy-apps.git\", \"apps_branch\": \"main\", \"copy_network\": false},"
    " \"languages\": [\"en_US.UTF-8\", \"zh_CN.UTF-8\"],"
    " \"levels\": {\"supported\": [\"x86-64\", \"x86-64-v2\"], \"auto\": \"x86-64-v2\", \"cpu_model\": \"ivybridge\"},"
    " \"ime\": [{\"name\": \"off\", \"label\": \"OFF (no Rime packages)\"}, {\"name\": \"en\", \"label\": \"English\"},"
    " {\"name\": \"zh\", \"label\": \"Chinese Pinyin\"}],"
    " \"modules\": [{\"name\": \"desktop\", \"description\": \"Shared Wayland compositor\"},"
    " {\"name\": \"cjk-fonts\", \"description\": \"CJK fonts\"}, {\"name\": \"tor\", \"description\": \"Tor\"}],"
    " \"module_defaults\": {\"en_US.UTF-8\": [\"desktop\"], \"zh_CN.UTF-8\": [\"cjk-fonts\", \"desktop\"]},"
    " \"containers\": [{\"name\": \"firefox\", \"host\": false}, {\"name\": \"foot\", \"host\": true}],"
    " \"container_defaults\": [\"foot\"],"
    " \"machines\": [{\"name\": \"generic\", \"description\": \"Generic PC\"},"
    " {\"name\": \"hp-omen-17-ck1\", \"description\": \"HP OMEN 17-ck1xxx (board 8A1A)\"}],"
    " \"repo_sections\": [\"core\", \"extra\", \"multilib\"], \"keymaps\": [\"de\", \"us\"], \"timezones\": [\"UTC\", \"Asia/Shanghai\"],"
    " \"tpm\": {\"usable\": false, \"advice\": \"TPM 1.2 off in the firmware: a passphrase\"},"
    " \"source\": {\"text\": \"this medium's own, 0123456789ab; could not fetch\", \"not_newest\": true},"
    " \"apps\": {\"text\": \"the medium's own app presets, fedcba987654; could not fetch\", \"not_newest\": true},"
    " \"network_profiles\": 0, \"memory_bytes\": 17179869184, \"esp_bytes\": 536870912, \"min_root_bytes\": 17179869184,"
    " \"disks\": [{\"path\": \"/dev/sda\", \"size\": 256060514304, \"model\": \"Samsung SSD\", \"tran\": \"sata\","
    " \"removable\": false, \"in_use\": \"\", \"children\": [{\"path\": \"/dev/sda1\", \"type\": \"part\", \"size\": 536870912,"
    " \"fstype\": \"vfat\", \"label\": \"EFI\", \"parttypename\": \"EFI System\", \"mountpoints\": []},"
    " {\"path\": \"/dev/sda3\", \"type\": \"part\", \"size\": 100000000000, \"fstype\": \"apfs\", \"label\": \"\","
    " \"parttypename\": \"Apple APFS\", \"mountpoints\": []}]},"
    " {\"path\": \"/dev/sdc\", \"size\": 8589934592, \"model\": \"small\", \"tran\": \"usb\", \"removable\": true,"
    " \"in_use\": \"\", \"children\": []}]}\n";

/* The description with the TPM, the firmware and fwupd's report as tpm_json
 * has them (in place of its "tpm"). */
static void describe_with(const char *tpm_json)
{
    static char text[sizeof describe + 1024];
    const char *a = strstr(describe, "\"tpm\": "), *b = strstr(describe, "\"source\": ");
    snprintf(text, sizeof text, "%.*s%s %s", (int)(a - describe), describe, tpm_json, b);
    write_file(D("describe.json"), text, 0644);
}

/* locachy-install: the description; else how it was called, its secret
 * files (content and mode), and an installation's progress lines. */
static const char fake_install[] =
    "#!/bin/sh\n"
    "d=$(dirname \"$0\")\n"
    "if [ \"$1\" = --describe-json ]; then\n"
    "  for f in /proc/$$/fd/*; do case $(readlink \"$f\") in *locachy-installer-session*) touch \"$d/state-leaked\";; esac; done\n"
    "  cat \"$d/describe.json\"; exit 0\n"
    "fi\n"
    "if [ \"$1\" = --refresh-live ]; then\n"
    "  echo refresh >>\"$d/refresh-calls\"\n"
    "  if [ -e \"$d/refresh-fail\" ]; then exit 1; fi\n"
    "  printf '{\"frontend\":\"%s/installer-next\",\"revision\":\"89abcdef0123\",\"input_hash\":\"fixture\",\"source\":\"%s/src\",\"snapshot\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}\\n' \"$d\" \"$d\"; exit 0\n"
    "fi\n"
    "if [ \"$1\" = --resume-status ]; then\n"
    "  if [ -e \"$d/resume-status.json\" ]; then cat \"$d/resume-status.json\"; else echo null; fi; exit 0\n"
    "fi\n"
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
    "  echo 'Source key to trust: odd ;reboot'\n"
    "  echo 'locachy-install: Target build failed; see the log' >&2\n"
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
static const char fake_rescue[] = "#!/bin/sh\necho rescue >>\"$(dirname \"$0\")/rescue-calls\"\n";
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
    step = scroll = log_from = log_all = 0;
    in_page = 1;
    notice[0] = '\0';
    unsetenv("LOCACHY_LANG");
    setenv("LOCACHY_INSTALLER_INPUT_HASH", "fixture", 1); /* this binary already matches the refreshed one */
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
    textsel_clear();
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
    k('\n'); k(KEY('a' & 0x1F, KEYS_CTRL)); type("boot-test"); k('\n'); /* Ctrl+A: typing replaces it */
    k(K_DOWN); k(K_DOWN); k('\n'); type("Locachy123"); k('\n'); type("Locachy123"); k('\n');
    for (int s = S_USER; s < S_SUMMARY; s++) { k(K_EOL); k('\n'); }
    k(SNAP);
}

static int fixture_session(struct json_object *state, int sealed)
{
    const char *text = json_object_to_json_string_ext(state, JSON_C_TO_STRING_PLAIN);
    int fd = memfd_create("locachy-installer-session", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    if (sealed) assert(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0);
    return fd;
}

static void test_session_handoff(void)
{
    printf("== sealed choices: atomic restore, rejected input and one-exec descriptor\n");
    assert(load_description() == 0);
    memset(&c, 0, sizeof c);
    set_defaults();
    copy(c.password, sizeof c.password, "fixture-user-secret");
    copy(c.passphrase, sizeof c.passphrase, "fixture-disk-secret");
    copy(c.disk, sizeof c.disk, "/dev/sda");
    c.nextra = 2;
    copy(c.extra[0].disk, sizeof c.extra[0].disk, "/dev/nvme1n1");
    copy(c.extra[0].part, sizeof c.extra[0].part, "/dev/nvme1n1p1");
    copy(c.extra[0].name, sizeof c.extra[0].name, "archive");
    c.extra[0].action = EXTRA_ATTACH;
    c.extra[0].role = role_index("data");
    c.extra[0].encrypt = VENC_NONE;
    copy(c.extra[1].disk, sizeof c.extra[1].disk, "/dev/nvme2n1");
    copy(c.extra[1].name, sizeof c.extra[1].name, "data");
    c.extra[1].action = EXTRA_FMT_BTRFS;
    c.extra[1].role = role_index("shared");
    c.extra[1].encrypt = VENC_LUKS;
    c.extra[1].compute_storage = 1;
    copy(c.array_name, sizeof c.array_name, "fastpool");
    c.array_raid = raid_index("raid0");
    c.array_role = role_index("compute");
    c.array_encrypt = VENC_NONE;
    copy(c.winmaint_disk, sizeof c.winmaint_disk, "/dev/sda");
    c.winmaint_gib = 64;
    c.lang = ui_lang = 1;
    recovered_installation = 1;
    struct choices expected = c;
    int fd = session_fd("begin");
    assert(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    char text[65537], action[16] = "unchanged";
    ssize_t size = pread(fd, text, sizeof text - 1, 0);
    assert(size > 0);
    text[size] = '\0';
    struct json_object *state = json_tokener_parse(text);
    assert(state);
    memset(&c, 0, sizeof c);
    assert(restore_session(fd, action, sizeof action) == 0);
    assert(!memcmp(&c, &expected, sizeof c) && !strcmp(action, "begin") && ui_lang == 1 && recovered_installation);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);

    /* Missing fields, wrong types, unknown names and overlong secrets must
     * preserve every prior choice, the action and the non-secret UI flags. */
    for (int test = 0; test < 8; test++) {
        struct json_object *bad = json_tokener_parse(text);
        if (test == 0) json_object_object_del(bad, "disk_mode");
        if (test == 1) json_object_object_add(bad, "encrypt", json_object_new_string("2"));
        if (test == 2) json_object_array_add(jget(bad, "modules"), json_object_new_string("removed-module"));
        if (test == 3) json_object_object_add(bad, "password", json_object_new_string_len(text, 300));
        if (test == 4) json_object_object_add(bad, "action", json_object_new_string("other"));
        if (test == 6) json_object_object_add(bad, "array_raid_name", json_object_new_string("raid99"));
        if (test == 7) json_object_object_add(bad, "winmaint_gib", json_object_new_int(5));
        fd = fixture_session(bad, test != 5);
        copy(action, sizeof action, "unchanged");
        assert(restore_session(fd, action, sizeof action) == -1);
        assert(!memcmp(&c, &expected, sizeof c) && !strcmp(action, "unchanged") && ui_lang == 1 && recovered_installation);
        assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
        clear_session_json(bad);
        json_object_put(bad);
    }
    fd = fixture_session(state, 1);
    assert(fcntl(fd, F_SETFD, 0) == 0); /* what the previous executable passes */
    char state_arg[48];
    snprintf(state_arg, sizeof state_arg, "--state-fd=%d", fd);
    at = nkeys = 0;
    keys[0] = END;
    described = recovered_installation = 0;
    stopped_installation = NULL;
    if (setjmp(quit) == 0) installer_main(2, (char *[]){ "installer", state_arg, NULL });
    assert(access(D("state-leaked"), F_OK) < 0);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    clear_session_json(state);
    json_object_put(state);
    explicit_bzero(text, sizeof text);
    explicit_bzero(&expected, sizeof expected);
    printf("PASS installer session: sealed roundtrip, atomic rejection, no helper inheritance\n");
}

static void test_refresh_exec_failure(void)
{
    printf("== exec failure: the running binary never claims the new input hash\n");
    setenv("LOCACHY_INSTALLER_INPUT_HASH", "old-fixture", 1);
    setenv("LOCACHY_INSTALLER_REVISION", "old-revision", 1);
    setenv("LOCACHY_INSTALL_SNAPSHOT_ROOT", "old-root", 1);
    setenv("LOCACHY_INSTALL_SNAPSHOT", "old-snapshot", 1);
    unsetenv("LOCACHY_INSTALL_SNAPSHOT_COMMIT");
    unlink(D("refresh-calls"));
    for (int attempt = 0; attempt < 2; attempt++) {
        assert(refresh_installer("begin") == -1); /* installer-next does not exist */
        assert(!strcmp(getenv("LOCACHY_INSTALLER_INPUT_HASH"), "old-fixture"));
        assert(!strcmp(getenv("LOCACHY_INSTALLER_REVISION"), "old-revision"));
        assert(!strcmp(getenv("LOCACHY_INSTALL_SNAPSHOT_ROOT"), "old-root"));
        assert(!getenv("LOCACHY_INSTALL_SNAPSHOT_COMMIT"));
        assert(!strcmp(getenv("LOCACHY_INSTALL_SNAPSHOT"), "old-snapshot"));
    }
    assert(!strcmp(slurp(D("refresh-calls")), "refresh\nrefresh\n"));
    setenv("LOCACHY_INSTALLER_INPUT_HASH", "fixture", 1);
    assert(refresh_installer("begin") == 0);
    assert(!strcmp(getenv("LOCACHY_INSTALL_SNAPSHOT"),
                   "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    printf("PASS installer refresh: failed exec retains the old snapshot; matching binary adopts the new one\n");
}

static void test_recovered_preflight(void)
{
    printf("== recovered preflight: both secrets are prompted; TPM wins in either order\n");
    const char *records[] = {
        "{\"phase\":\"preflight\",\"argv\":[\"--tpm\",\"--encrypt\",\"--user=boot-test\"]}",
        "{\"phase\":\"preflight\",\"argv\":[\"--encrypt\",\"--tpm\",\"--user=boot-test\"]}",
        "{\"phase\":\"preflight\",\"argv\":[\"--user=boot-test\"]}"
    };
    for (int test = 0; test < 3; test++) {
        stopped_installation = json_tokener_parse(records[test]);
        assert(recover_installation() == 1 && recovered_installation);
        assert(c.encrypt == (test == 2 ? ENC_OFF : ENC_TPM) && !c.password[0] && !c.passphrase[0]);
        at = nkeys = 0;
        if (test != 2) { type("recovered-disk"); k('\n'); }
        type("recovered-user"); k('\n');
        continue_install_current();
        assert(at == nkeys && !strcmp(c.password, "recovered-user"));
        if (test != 2) assert(!strcmp(c.passphrase, "recovered-disk"));
        assert(job);
        int result;
        do { usleep(10000); result = check_install(); } while (result < 0);
        assert(result == 0);
        json_object_put(stopped_installation);
        stopped_installation = NULL;
    }
    at = nkeys = recovered_installation = 0;
    keys[0] = END;
    printf("PASS installer recovery: preflight secrets prompted, encryption order independent\n");
}

static const char multi_disk_describe[] =
    "{\"defaults\": {\"user\": \"user\", \"hostname\": \"locachy\", \"keymap\": \"us\", \"timezone\": \"auto\","
    " \"swap_gib\": 8, \"level\": \"auto\", \"cpu\": \"auto\", \"repos\": [\"core\", \"extra\", \"multilib\"],"
    " \"update_repo\": \"https://github.com/niuyuanzhao/locachy.git\", \"update_branch\": \"main\","
    " \"apps_repo\": \"https://github.com/niuyuanzhao/locachy-apps.git\", \"apps_branch\": \"main\", \"copy_network\": false},"
    " \"languages\": [\"en_US.UTF-8\", \"zh_CN.UTF-8\"],"
    " \"levels\": {\"supported\": [\"x86-64\", \"x86-64-v2\"], \"auto\": \"x86-64-v2\", \"cpu_model\": \"ivybridge\"},"
    " \"ime\": [{\"name\": \"off\", \"label\": \"OFF (no Rime packages)\"}],"
    " \"modules\": [{\"name\": \"desktop\", \"description\": \"Shared Wayland compositor\"}],"
    " \"module_defaults\": {\"en_US.UTF-8\": [\"desktop\"], \"zh_CN.UTF-8\": [\"desktop\"]},"
    " \"containers\": [{\"name\": \"foot\", \"host\": true}],"
    " \"container_defaults\": [\"foot\"],"
    " \"machines\": [{\"name\": \"generic\", \"description\": \"Generic PC\"}],"
    " \"repo_sections\": [\"core\", \"extra\", \"multilib\"],"
    " \"storage_roles\": [\"shared\", \"data\", \"compute\"],"
    " \"storage_raids\": [\"single\", \"raid0\", \"raid1\", \"raid10\"],"
    " \"winmaint\": {\"min_gib\": 40, \"default_gib\": 48, \"max_gib\": 4096},"
    " \"keymaps\": [\"us\"], \"timezones\": [\"UTC\"],"
    " \"tpm\": {\"usable\": false, \"advice\": \"no TPM: a passphrase\"},"
    " \"source\": {\"text\": \"medium\", \"not_newest\": false},"
    " \"apps\": {\"text\": \"medium\", \"not_newest\": false},"
    " \"network_profiles\": 0, \"memory_bytes\": 17179869184, \"esp_bytes\": 536870912, \"min_root_bytes\": 17179869184,"
    " \"disks\": ["
    "{\"path\": \"/dev/nvme0n1\", \"size\": 549755813888, \"model\": \"Samsung 990\", \"serial\": \"S990-A\", \"tran\": \"nvme\","
    " \"removable\": false, \"in_use\": \"\", \"children\": ["
    "  {\"path\": \"/dev/nvme0n1p1\", \"type\": \"part\", \"size\": 536870912, \"fstype\": \"vfat\", \"label\": \"EFI\", \"parttypename\": \"EFI System\", \"mountpoints\": []}]},"
    "{\"path\": \"/dev/nvme1n1\", \"size\": 1073741824000, \"model\": \"WD Black\", \"serial\": \"WD-1\", \"tran\": \"nvme\","
    " \"removable\": false, \"in_use\": \"\", \"pttype\": \"gpt\", \"log_sec\": 512, \"free_tail\": 536332975104, \"children\": ["
    "  {\"path\": \"/dev/nvme1n1p1\", \"type\": \"part\", \"size\": 536870912000, \"fstype\": \"ntfs\", \"label\": \"Archive\", \"parttypename\": \"Microsoft basic data\", \"mountpoints\": []},"
    "  {\"path\": \"/dev/nvme1n1p2\", \"type\": \"part\", \"size\": 536870912, \"fstype\": \"vfat\", \"label\": \"ESP\", \"parttypename\": \"EFI System\", \"mountpoints\": []}]},"
    "{\"path\": \"/dev/nvme2n1\", \"size\": 1073741824000, \"model\": \"WD Black\", \"serial\": \"WD-2\", \"tran\": \"nvme\","
    " \"removable\": false, \"in_use\": \"\", \"log_sec\": 512, \"free_tail\": 1073740758528, \"children\": []},"
    "{\"path\": \"/dev/nvme3n1\", \"size\": 1073741824000, \"model\": \"WD Black\", \"serial\": \"WD-3\", \"tran\": \"nvme\","
    " \"removable\": false, \"in_use\": \"\", \"log_sec\": 512, \"free_tail\": 1073740758528, \"children\": []}]}\n";

static void test_multi_disk_storage_and_winmaint(void)
{
    write_file(D("describe.json"), multi_disk_describe, 0644);

    /* 1. Primary disk is NEVER auto-selected; serials are shown; attach + format + winmaint. */
    k('\n');                    /* Install LOCachy */
    k(K_EOL); k('\n');          /* Network: Next -> Disk */
    k(SNAP);
    /* Select primary disk nvme0n1 (row 1 after disk mode). */
    k(K_DOWN); k('\n');
    /* Navigate past nvme1n1, nvme2n1, nvme3n1, Swap -> Additional disk nvme1n1. */
    k(K_DOWN); k(K_DOWN); k(K_DOWN); k(K_DOWN); k(K_DOWN);
    /* Configure nvme1n1: Attach existing partition (action 1), part 0 (ntfs), name "archive", role 1 ("data"). */
    k('\n'); k(K_DOWN); k('\n'); k('\n');
    k(KEY('a' & 0x1F, KEYS_CTRL)); type("archive"); k('\n');
    k(K_DOWN); k('\n');
    /* Next row: Additional disk nvme2n1 -> Format whole disk (ext4) (action 2), accept "data", role 0 (shared),
     * enc 0 (auto), default compute storage: y. */
    k(K_DOWN); k('\n'); k(K_DOWN); k(K_DOWN); k('\n'); k('\n'); k('\n'); k('\n'); k('y');
    /* Next rows: nvme3n1 (leave untouched), then Maintenance Windows -> Right (on system disk /dev/nvme0n1),
     * then Down -> Windows C: size -> Right (48 -> 64 GiB). */
    k(K_DOWN); k(K_DOWN); k(K_RIGHT); k(K_DOWN); k(K_RIGHT);
    k(SNAP);
    k(K_EOL); k('\n');          /* Disk: Next -> Encryption */
    k(K_DOWN); k('\n'); type("correct horse"); k('\n'); type("correct horse"); k('\n');
    k(K_EOL); k('\n');          /* Encryption: Next -> User */
    k(K_DOWN); k(K_DOWN); k('\n'); type("Locachy123"); k('\n'); type("Locachy123"); k('\n');
    for (int s = S_USER; s < S_SUMMARY; s++) { k(K_EOL); k('\n'); }
    k(SNAP);
    k(K_EOL); k('\n'); k('n');  /* Install -> --check, then N at confirm */
    run("multi-disk: no auto-select, attach, format, Maintenance Windows");

    /* First snap (before picking primary): disk list shows serials and no primary selected yet. */
    assert(snapped("[S990-A]") && snapped("[WD-1]") && snapped("[WD-2]"));
    assert(snapped("( ) /dev/nvme0n1"));
    assert(snapped("Additional disks & arrays"));
    assert(snapped("attach /dev/nvme1n1p1 (ntfs) as archive [data]"));
    assert(snapped("format (ext4, auto) as data [shared] *compute"));
    assert(snapped("attached (keep data, volume archive): /dev/nvme1n1p1"));
    assert(snapped("erased (volume data): /dev/nvme2n1"));
    assert(snapped("on system disk (/dev/nvme0n1), Windows C: 64 GiB"));

    /* Verify E_NOTE vs E_WARN classification in build_summary(). */
    build_step(S_SUMMARY, 120);
    int saw_attach_note = 0, saw_format_warn = 0;
    for (int i = 0; i < nentries; i++) {
        if (strstr(entries[i].text, "attached (keep data, volume archive)")) {
            assert(entries[i].id == E_NOTE);
            saw_attach_note = 1;
        }
        if (strstr(entries[i].text, "erased (volume data): /dev/nvme2n1")) {
            assert(entries[i].id == E_WARN);
            saw_format_warn = 1;
        }
    }
    assert(saw_attach_note && saw_format_warn);

    const char *calls = slurp(D("calls"));
    printf("multi-disk calls: %s", calls);
    assert(strstr(calls, "--check --disk=/dev/nvme0n1 --swap-gib=8 "));
    assert(strstr(calls, "--extra-volume=name=archive,mode=attach,dev=/dev/nvme1n1p1,role=data"));
    assert(strstr(calls, "--extra-volume=name=data,mode=format,dev=/dev/nvme2n1,fs=ext4,role=shared,encrypt=auto,compute_storage=1"));
    assert(strstr(calls, "--winmaint-disk=/dev/nvme0n1 --winmaint-gib=64"));

    /* 2. Btrfs array: 1 disk warns, 2 disks valid, raid10 with 2 disks warns, 2-disk raid1 emits mode=array;
     *    changing primary clears conflict; winmaint + format on same secondary disk warns. */
    k('\n');                    /* Install LOCachy */
    k(K_EOL); k('\n');          /* Network -> Disk */
    /* Before choosing primary, c.disk must still be empty. */
    k(K_DOWN); k('\n');         /* Choose primary /dev/nvme0n1 */
    /* Jump to nvme2n1 in Additional disks (past nvme1n1, nvme2n1, nvme3n1, Swap, extra nvme1n1). */
    k(K_DOWN); k(K_DOWN); k(K_DOWN); k(K_DOWN); k(K_DOWN); k(K_DOWN);
    /* Mark nvme2n1 as Add to Btrfs array (action 4). */
    k('\n'); k(K_DOWN); k(K_DOWN); k(K_DOWN); k(K_DOWN); k('\n');
    k(SNAP);                    /* 1 array disk -> warning visible */
    /* Mark nvme3n1 as Add to Btrfs array (action 4). */
    k(K_DOWN); k('\n'); k(K_DOWN); k(K_DOWN); k(K_DOWN); k(K_DOWN); k('\n');
    /* Now array rows appear below nvme3n1: name, RAID (raid1), role (shared -> compute), enc, compute (no -> yes). */
    k(K_DOWN); k(K_DOWN); k(K_DOWN); k(K_LEFT); /* role: shared -> compute */
    k(K_DOWN); k(K_DOWN); k(K_RIGHT);           /* Array as Compute storage: yes */
    k(SNAP);
    k(K_EOL); k('\n');          /* Disk -> Encryption */
    k(K_DOWN); k('\n'); type("correct horse"); k('\n'); type("correct horse"); k('\n');
    k(K_EOL); k('\n');          /* Encryption -> User */
    k(K_DOWN); k(K_DOWN); k('\n'); type("Locachy123"); k('\n'); type("Locachy123"); k('\n');
    for (int s = S_USER; s < S_SUMMARY; s++) { k(K_EOL); k('\n'); }
    k(SNAP);
    k(K_EOL); k('\n'); k('n');  /* Install -> --check, then N */
    run("multi-disk: Btrfs raid1 array");
    assert(snapped("Btrfs array needs at least 2 disks."));
    assert(snapped("Array RAID mode") && snapped("< raid1 >"));
    assert(snapped("erased (volume pool): /dev/nvme2n1"));
    assert(snapped("erased (volume pool): /dev/nvme3n1"));
    calls = slurp(D("calls"));
    printf("array calls: %s", calls);
    assert(strstr(calls, "--extra-volume=name=pool,mode=array,devs=/dev/nvme2n1+/dev/nvme3n1,raid=raid1,role=compute,encrypt=auto,compute_storage=1"));

    /* Direct unit checks on problem(S_DISK) edge cases:
     * (a) raid10 with 2 disks rejected; (b) primary change clears extra assignment;
     * (c) winmaint on same disk as Btrfs array rejected;
     * (d) winmaint on same disk as EXTRA_FMT_EXT4/EXTRA_FMT_BTRFS allowed when disk fits
     *     1 GiB + tail + 4 MiB (ignoring old partitions), rejected when undersized;
     * (e) empty c.disk at defaults. */
    char why[256];
    c.array_raid = raid_index("raid10");
    assert(problem(S_DISK, why, sizeof why) && strstr(why, "at least 4 disks"));
    c.array_raid = raid_index("raid1");
    assert(!problem(S_DISK, why, sizeof why));
    /* Maintenance Windows on a secondary disk with free tail space (nvme1n1: 500.5 GiB used of 1000 GiB). */
    copy(c.winmaint_disk, sizeof c.winmaint_disk, "/dev/nvme1n1");
    c.winmaint_gib = 48;
    assert(!problem(S_DISK, why, sizeof why));
    {
        /* Only the unallocated space after the last partition counts (locachy-winmaint create
         * appends there): 220 GiB unallocated in all but 20 GiB at the end is refused; exactly
         * C: + 256 MiB there is enough, a sector less is not. */
        c.disk_mode = WHOLE;
        copy(c.disk, sizeof c.disk, "/dev/nvme0n1");
        struct disk *w = &disks[disk_index("/dev/nvme1n1")], saved = *w;
        w->free_tail = 20LL << 30;
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "unallocated after its last partition"));
        w->free_tail = (48LL << 30) + (256LL << 20);
        assert(!problem(S_DISK, why, sizeof why));
        w->free_tail -= 512;
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "unallocated after its last partition"));
        w->free_tail = -1;
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "cannot be measured"));
        *w = saved;
        copy(w->pttype, sizeof w->pttype, "dos");
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "has a partition table of type MBR (dos); Maintenance Windows needs GPT"));
        *w = saved;
        w->log_sec = 4096;
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "uses 4096-byte sectors"));
        *w = saved;
        w->winmaint = 1;
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "already holds Maintenance Windows partitions"));
        *w = saved;
        w->nparts = 0;
        w->pttype[0] = '\0';
        copy(w->fstype, sizeof w->fstype, "ext4");
        w->free_tail = -1;
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "has a whole-disk filesystem (ext4); Maintenance Windows would overwrite it"));
        *w = saved;
        c.winmaint_disk[0] = '\0';
        assert(!problem(S_DISK, why, sizeof why));

        /* A disk or partition of Maintenance Windows is never formatted or attached, and never installed on. */
        struct disk *a = &disks[disk_index("/dev/nvme2n1")]; /* a Btrfs array member here */
        a->winmaint = 1;
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "/dev/nvme2n1 holds Maintenance Windows partitions and cannot be formatted"));
        a->winmaint = 0;
        struct part p = saved.parts[0];
        assert(part_attachable(&p));
        p.winmaint = 1;
        assert(!part_attachable(&p));
        struct disk *s = &disks[disk_index("/dev/nvme0n1")];
        s->winmaint = 1;
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "installing here would erase them"));
        s->winmaint = 0;
        c.disk_mode = PARTS;
        copy(c.esp, sizeof c.esp, "/dev/nvme1n1p2");
        copy(c.root, sizeof c.root, "/dev/nvme1n1p1");
        w->parts[1].winmaint = 1;
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "/dev/nvme1n1p2 is a Maintenance Windows partition"));
        w->parts[1].winmaint = 0;
        c.disk_mode = WHOLE;
        c.esp[0] = c.root[0] = '\0';

        /* The system disk's least size has the 1 MiB before the first partition and the 2 MiB kept at the end. */
        long long least = (3LL << 20) + esp_bytes + (long long)c.swap_gib * (1LL << 30) + min_root_bytes;
        assert(disk_needs() == least);
        long long size = s->size;
        s->size = least - 1;
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "is too small"));
        s->size = least;
        assert(!problem(S_DISK, why, sizeof why));
        copy(c.winmaint_disk, sizeof c.winmaint_disk, "/dev/nvme0n1");
        assert(disk_needs() == least + (48LL << 30) + (256LL << 20));
        assert(problem(S_DISK, why, sizeof why) && strstr(why, "is too small"));
        s->size = size;
        assert(!problem(S_DISK, why, sizeof why));
    }
    c.winmaint_disk[0] = '\0';
    c.winmaint_gib = 48;
    copy(c.winmaint_disk, sizeof c.winmaint_disk, "/dev/nvme2n1");
    assert(problem(S_DISK, why, sizeof why) && strstr(why, "cannot host both Maintenance Windows and a Btrfs array"));
    c.winmaint_disk[0] = '\0';

    /* Formatted secondary disk + Maintenance Windows on /dev/nvme1n1 (1000 GiB with 500.5 GiB old partitions). */
    int ei = c.nextra++;
    memset(&c.extra[ei], 0, sizeof c.extra[ei]);
    copy(c.extra[ei].disk, sizeof c.extra[ei].disk, "/dev/nvme1n1");
    copy(c.extra[ei].name, sizeof c.extra[ei].name, "data");
    c.extra[ei].action = EXTRA_FMT_EXT4;
    c.extra[ei].role = role_index("shared");
    c.extra[ei].encrypt = VENC_AUTO;
    copy(c.winmaint_disk, sizeof c.winmaint_disk, "/dev/nvme1n1");
    c.winmaint_gib = 48;
    assert(!problem(S_DISK, why, sizeof why));
    /* Old partitions on nvme1n1 are wiped by format, so 900 GiB C: fits on the 1000 GiB disk. */
    c.winmaint_gib = 900;
    assert(!problem(S_DISK, why, sizeof why));
    /* Undersized formatted disk + winmaint (999 GiB C: + 256 MiB + 1 GiB + 4 MiB > 1000 GiB) is rejected. */
    c.winmaint_gib = 999;
    assert(problem(S_DISK, why, sizeof why) && strstr(why, "for the formatted volume and Maintenance Windows"));
    /* Same check holds for EXTRA_FMT_BTRFS. */
    c.extra[ei].action = EXTRA_FMT_BTRFS;
    c.winmaint_gib = 48;
    assert(!problem(S_DISK, why, sizeof why));
    /* Disk page and Summary page reflect shared reserved-tail hosting clearly. */
    build_step(S_DISK, 120);
    int saw_disk_tail = 0, saw_disk_note = 0;
    for (int i = 0; i < nentries; i++) {
        if (strstr(entries[i].text, "on /dev/nvme1n1 (reserved tail)")) saw_disk_tail = 1;
        if (strstr(entries[i].text, "Formatting /dev/nvme1n1 reserves the tail for Maintenance Windows.")) saw_disk_note = 1;
    }
    assert(saw_disk_tail && saw_disk_note);
    build_step(S_SUMMARY, 120);
    int saw_sum_tail = 0, saw_sum_erased = 0;
    for (int i = 0; i < nentries; i++) {
        if (strstr(entries[i].text, "on /dev/nvme1n1 (reserved tail), Windows C: 48 GiB")) saw_sum_tail = 1;
        if (strstr(entries[i].text, "erased (volume data, Maintenance Windows tail): /dev/nvme1n1")) {
            assert(entries[i].id == E_WARN);
            saw_sum_erased = 1;
        }
    }
    assert(saw_sum_tail && saw_sum_erased);
    extra_clear_disk("/dev/nvme1n1");
    c.winmaint_disk[0] = '\0';

    act(R_DISK + 2, '\n'); /* switch primary to /dev/nvme2n1 -> clears its array membership */
    assert(extra_for_disk("/dev/nvme2n1") < 0);
    assert(problem(S_DISK, why, sizeof why) && strstr(why, "at least 2 disks"));
    memset(&c, 0, sizeof c);
    set_defaults();
    assert(c.disk[0] == '\0' && c.winmaint_gib == 48);
    assert(problem(S_DISK, why, sizeof why) && strstr(why, "Choose the disk"));

    /* 3. Interactive flow: format secondary /dev/nvme2n1 AND place Maintenance Windows on /dev/nvme2n1. */
    k('\n');                    /* Install LOCachy */
    k(K_EOL); k('\n');          /* Network -> Disk */
    k(K_DOWN); k('\n');         /* Choose primary /dev/nvme0n1 */
    /* Jump to nvme2n1 in Additional disks (past nvme1n1, nvme2n1, nvme3n1, Swap, extra nvme1n1). */
    k(K_DOWN); k(K_DOWN); k(K_DOWN); k(K_DOWN); k(K_DOWN); k(K_DOWN);
    /* Configure nvme2n1: Format whole disk (ext4) (action 2), accept "data", role 0 (shared),
     * enc 0 (auto), default compute storage: n. */
    k('\n'); k(K_DOWN); k(K_DOWN); k('\n'); k('\n'); k('\n'); k('\n'); k('n');
    /* Next rows: nvme3n1 (leave untouched), then Maintenance Windows -> Right x3
     * (off -> nvme0n1 -> nvme1n1 -> nvme2n1). */
    k(K_DOWN); k(K_DOWN); k(K_RIGHT); k(K_RIGHT); k(K_RIGHT);
    k(SNAP);
    k(K_EOL); k('\n');          /* Disk -> Encryption */
    k(K_DOWN); k('\n'); type("correct horse"); k('\n'); type("correct horse"); k('\n');
    k(K_EOL); k('\n');          /* Encryption -> User */
    k(K_DOWN); k(K_DOWN); k('\n'); type("Locachy123"); k('\n'); type("Locachy123"); k('\n');
    for (int s = S_USER; s < S_SUMMARY; s++) { k(K_EOL); k('\n'); }
    k(SNAP);
    k(K_EOL); k('\n'); k('n');  /* Install -> --check, then N at confirm */
    run("multi-disk: format secondary disk + Maintenance Windows on same disk");
    assert(snapped("on /dev/nvme2n1 (reserved tail)"));
    assert(snapped("Formatting /dev/nvme2n1 reserves the tail for Maintenance Windows."));
    assert(snapped("erased (volume data, Maintenance Windows tail): /dev/nvme2n1"));
    assert(snapped("on /dev/nvme2n1 (reserved tail), Windows C: 48 GiB"));
    assert(snapped("Nothing is missing."));
    calls = slurp(D("calls"));
    printf("shared-tail calls: %s", calls);
    assert(strstr(calls, "--extra-volume=name=data,mode=format,dev=/dev/nvme2n1,fs=ext4,role=shared,encrypt=auto"));
    assert(strstr(calls, "--winmaint-disk=/dev/nvme2n1 --winmaint-gib=48"));

    write_file(D("describe.json"), describe, 0644);
    printf("PASS installer multi-disk: attach, format, btrfs array, winmaint, validation\n");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(dir, sizeof dir, "%s", argv[1]);
    assert(mkdir(dir, 0755) == 0);
    write_file(D("describe.json"), describe, 0644);
    write_file(D("locachy-install"), fake_install, 0755);
    write_file(D("term"), fake_term, 0755);
    write_file(D("rescue"), fake_rescue, 0755);
    write_file(D("systemctl"), fake_systemctl, 0755);
    write_file(D("keytable"), fake_keytable, 0755);

    /* The start: the terminal and the independent rescue app each come and go. */
    k(K_DOWN); k(K_DOWN); k('\n'); k(SNAP);
    run("start: rescue is independent of terminal");
    assert(access(D("rescue-calls"), F_OK) == 0);
    assert(access(D("term-calls"), F_OK) != 0);
    assert(snapped("Rescue mode") && snapped("Terminal (a root shell, for repairs)"));

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

    /* F1's keys, Esc closes them; on the start page Ctrl+Q leaves setup at
     * once, and so does the window's close. */
    left = 0;
    k(K_F1); k(SNAP); k(27); k(SNAP); k(KEY('q' & 0x1F, KEYS_CTRL));
    run("start: F1's keys, Ctrl+Q leaves");
    assert(left == 1 && snapped("Installer keys") && snapped("Quit setup (the console comes back)"));
    assert(!snapped("Between the steps and the step's page") && !snapped("Copy the whole output")); /* the page's own */
    assert(snapped("Install LOCachy") && snapped("[F1: help]") && snapped("[Ctrl+Q: quit]"));
    left = 0;
    k(K_CLOSE);
    run("start: the window's close leaves");
    assert(left == 1);
    /* The button sends Ctrl+Q itself (its modifier with it): it leaves. */
    left = 0;
    ptr(30, ROWS - 2, BTN_LEFT, 1, 1);
    run("start: a click on [Ctrl+Q: quit] leaves");
    assert(left == 1);
    /* On the steps it asks (the choices would be lost): n stays, and so do
     * Ctrl+Q or the close given to the question; y leaves. Alt+F4 quits as
     * Ctrl+Q does; F1 there, then Ctrl+Q, asks the same. */
    left = kept = 0;
    k('\n'); k(KEY('q' & 0x1F, KEYS_CTRL)); k(SNAP); k('n'); k(K_CLOSE); k(K_CLOSE);
    k(KEY(K_F1 + 3, KEYS_ALT)); k(27); k(K_F1); k(KEY('q' & 0x1F, KEYS_CTRL)); k('y');
    run("steps: Ctrl+Q asks, n and the close stay, y leaves");
    assert(left == 1 && mode == M_STEPS && kept >= 3 && snapped("Leave setup? The choices made here are lost."));
    /* Tab and Shift+Tab: between the steps and the page; q, as Esc, from
     * the steps to the start. */
    k('\n'); k(KEY('\t', KEYS_SHIFT));
    run("steps: Shift+Tab to the steps");
    assert(mode == M_STEPS && !in_page);
    k('\n'); k(KEY('\t', KEYS_SHIFT)); k(KEY('\t', KEYS_SHIFT)); k(KEY('\t', KEYS_SHIFT)); k('q');
    run("steps: Shift+Tab back and forth, q to the start");
    assert(mode == M_START);
    /* F1 on the steps: their keys, the lines' and the questions', not the
     * output's; the steps have the Ctrl+Q button too. */
    k('\n'); k(K_F1); k(SNAP); k(27); k(SNAP);
    run("steps: F1 lists the steps' keys");
    assert(mode == M_STEPS && snapped("The step before or after") && snapped("In a line you type: paste"));
    assert(snapped("Quit setup: asks first") && !snapped("Copy the whole output") && snapped("[Ctrl+Q: quit]"));

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
    assert(strstr(calls, "--repos=core,extra,multilib --update-repo=https://github.com/niuyuanzhao/locachy.git --update-branch=main"
                         " --apps-repo=https://github.com/niuyuanzhao/locachy-apps.git --apps-branch=main"));
    assert(!strstr(calls, "correct horse") && !strstr(calls, "Locachy123"));
    /* The app presets in the summary as the source: where from, then what
     * locachy-install has now, warned as the source's when not the newest. */
    assert(snapped("App presets from") && snapped("https://github.com/niuyuanzhao/locachy-apps.git (main)"));
    assert(snapped("Now: the medium's own app presets, fedcba987654; could not fetch"));
    build_step(S_SUMMARY, 100);
    int apps_now = 0, source_now = 0, apps_where = 0;
    for (int i = 0; i < nentries; i++) {
        if (strstr(entries[i].text, "Now: the medium's own app presets")) apps_now = entries[i].id;
        if (strstr(entries[i].text, "Now: this medium's own, 0123456789ab")) source_now = entries[i].id;
        if (strstr(entries[i].text, "App presets ") && strstr(entries[i].text, "the newest from the repository above")) {
            apps_where = i;
        }
    }
    assert(apps_now == E_WARN && source_now == E_WARN && apps_where > 0);
    apps_not_newest = 0;
    build_step(S_SUMMARY, 100);
    for (int i = 0; i < nentries; i++) {
        if (strstr(entries[i].text, "Now: the medium's own app presets")) apps_now = entries[i].id;
    }
    assert(apps_now == E_NOTE);
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
    paste_text = "boot-test";
    k('\n'); k(21); k(KEY('v' & 0x1F, KEYS_CTRL)); k('\n'); /* Ctrl+U, then Ctrl+V: the user's name is pasted */
    k(K_DOWN); k('\n'); type("J"); k(K_TEXT); k(K_TEXT); k('\b'); type("rgen"); k('\n');
    k(K_DOWN); k('\n'); type("Locachy123"); k('\n'); type("Locachy123"); k('\n');
    for (int s = S_USER; s < S_SUMMARY; s++) { k(K_EOL); k('\n'); }
    k(K_EOL); k('\n');          /* Install: --check, then the question: */
    k(K_ACT_HOME); k(KEY('y', KEYS_ALT)); k('x'); k(SNAP); k('\n'); /* no answers, then Enter: no */
    run("a full name beyond ASCII");
    assert(strstr(slurp(D("calls")), "--user=boot-test --full-name=Jürgen --hostname=locachy"));
    assert(snapped("Erase and install? [y/N]") && !strstr(slurp(D("calls")), "--yes"));

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

    /* A backend can reject reconstructing an old target command before it emits
     * Continue with (the archived ISO did this for an incompatible CATALOG).
     * The public resume status still keeps c available; it must never fall back
     * to the formatting summary. */
    write_file(D("resume-status.json"),
               "{\"schema\":2,\"phase\":\"target\",\"install_id\":\"0123456789abcdef0123456789abcdef\","
               "\"root\":\"/run/locachy-install-target\",\"argv\":[\"--user\",\"boot-test\"]}\n", 0600);
    write_file(D("install-fail"), "", 0644);
    to_summary("correct horse");
    k(K_EOL); k('\n'); k('y');
    for (int i = 0; i < 6; i++) k(WAIT);
    k(SNAP);
    run("backend rejects resume command: preserve c from resume status");
    assert(snapped("The installation failed."));
    assert(snapped("c: continue"));
    assert(!snapped("b: back to the summary"));
    unlink(D("resume-status.json"));
    unlink(D("install-fail"));

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

    /* Done: F1 lists the output's keys (reboot, power off; not the steps');
     * Ctrl+A selects all of the output and Ctrl+C copies the output itself,
     * from its first line; the Ctrl+Q button is there (not while it runs). */
    to_summary("correct horse");
    k(K_EOL); k('\n'); k('y');
    for (int i = 0; i < 10; i++) k(WAIT);
    k(K_F1); k(SNAP); k(27);
    k(KEY('a' & 0x1F, KEYS_CTRL)); k(SNAP); k(KEY('c' & 0x1F, KEYS_CTRL));
    run("done: the output's F1; Ctrl+A, then Ctrl+C copies all of the output");
    assert(mode == M_DONE && snapped("LOCachy is installed"));
    assert(snapped("Copy the whole output") && snapped("Reboot (asks first)") && !snapped("The step before or after"));
    assert(snapped("The whole output is selected: Ctrl+C copies it.") && snapped("Ctrl+A, Ctrl+C: copy all"));
    assert(snapped("[Ctrl+Q: quit]"));
    const char *output = slurp(D("run/install.log"));
    printf("copied: [%.80s...]\n", copied[0]);
    assert(copied[0][0] && strchr(copied[0], '\n') && strncmp(copied[0], output, strlen(copied[0])) == 0);

    /* A source signing key to trust: the installation waits, says which and
     * the command (Ctrl+C copies it): this medium's locachy-install by its path
     * with the full fingerprints, whatever "Trust them:" the output had
     * (here only locachy-build's, a program of the target) and never a
     * "key" that is not one; text is selected with the pointer and copied;
     * c goes on where it stopped (--resume), the secrets again. */
    write_file(D("keys-wanted"), "", 0644);
    to_summary("correct horse");
    k(K_EOL); k('\n'); k('y');
    for (int i = 0; i < 6; i++) k(WAIT);
    k(SNAP); k(KEY('c' & 0x1F, KEYS_CTRL)); k(SNAP);
    ptr(2, PAGE_TOP, BTN_LEFT, 1, 1); ptr(12, PAGE_TOP + 1, 0, 0, 0); ptr(12, PAGE_TOP + 1, BTN_LEFT, 0, 0);
    k(KEY('c' & 0x1F, KEYS_CTRL));
    ptr(10, PAGE_TOP + 1, BTN_LEFT, 1, 3); ptr(10, PAGE_TOP + 1, BTN_LEFT, 0, 0);
    k('c');
    for (int i = 0; i < 10; i++) k(WAIT);
    k(SNAP);
    run("keys to trust, copying, continuing");
    assert(snapped("Waiting for you: source signing keys to trust. None is trusted by itself."));
    assert(snapped("lyrebird-proxy") && snapped("40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C"));
    char trust[640];
    /* The command row: that and nothing after it (the rows are blank-padded). */
    snprintf(trust, sizeof trust, "    %s --trust-key 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C   ", D("locachy-install"));
    assert(snapped(trust));
    assert(snapped("Source key to trust: odd ;reboot") && !snapped("    odd    ")); /* in the output only */
    assert(snapped("c: continue") && !snapped("b: back to the summary"));
    snprintf(trust, sizeof trust, "Copied: %s --trust-key 40BBCBED223F5EB2A03EF657D7D7A110ABC79A6C", D("locachy-install"));
    assert(snapped(trust));
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
    assert(strstr(calls, "--update-repo=https://github.com/niuyuanzhao/locachy.git --update-branch=main"
                         " --apps-repo=https://github.com/niuyuanzhao/locachy-apps.git --apps-branch=main"));
    secrets = slurp(D("secrets"));
    assert(strstr(strstr(secrets, "--password-file 600 Locachy123\n") + 1, "--password-file 600 Locachy123\n"));
    const char *log = slurp(D("run/install.log"));
    assert(strstr(log, "Continue with: locachy-install --resume") && strstr(log, "Installation completed."));
    unlink(D("keys-wanted"));

    /* The firmware in setup mode: Secure Boot with this computer's own keys,
     * on unless turned off, Microsoft's too when asked; fwupd's HSI and the
     * checks it found failing. */
    describe_with("\"tpm\": {\"usable\": true, \"advice\": \"TPM 2.0, the firmware in setup mode\", \"version\": 2,"
                  " \"setup_mode\": true}, \"host_security\": {\"id\": \"HSI:0 (v2.0.16)\", \"checks\":"
                  " [\"\u2714 TPM v2.0: Found\", \"\u2718 Secure Boot: Disabled\"]},");
    k('\n'); k(K_EOL); k('\n'); k(K_DOWN); k('\n'); k(K_EOL); k('\n');      /* to Encryption */
    k(K_DOWN); k('\n'); type("correct horse"); k('\n'); type("correct horse"); k('\n');
    k(SNAP);
    k(K_DOWN); k(K_DOWN); k(K_RIGHT); k(SNAP);                               /* Microsoft's: yes */
    k(K_EOL); k('\n');
    /* A line: Ctrl+A then Ctrl+X cuts what is typed to the clipboard, Ctrl+V
     * pastes (the double's boot-test); a password, after Ctrl+A, is neither
     * copied nor cut: the clipboard still has "copy me" at the end. */
    k('\n'); k(21); type("copy me"); k(KEY('a' & 0x1F, KEYS_CTRL)); k(KEY('x' & 0x1F, KEYS_CTRL));
    k(KEY('v' & 0x1F, KEYS_CTRL)); k('\n');
    k(K_DOWN); k(K_DOWN); k('\n'); type("Locachy123");
    k(KEY('a' & 0x1F, KEYS_CTRL)); k(KEY('c' & 0x1F, KEYS_CTRL)); k(KEY('x' & 0x1F, KEYS_CTRL));
    k('\n'); type("Locachy123"); k('\n');
    for (int s = S_USER; s < S_SUMMARY; s++) { k(K_EOL); k('\n'); }
    k(SNAP);
    k(K_EOL); k('\n'); k('n');
    run("secure boot in setup mode, fwupd's host security");
    assert(snapped("Secure Boot with this computer's own keys") && snapped("Microsoft's keys too"));
    assert(snapped("Host security (fwupd): HSI:0 (v2.0.16)") && snapped("\u2718 Secure Boot: Disabled"));
    assert(!snapped("\u2714 TPM v2.0: Found"));
    assert(snapped("this computer's own keys and Microsoft's"));
    calls = slurp(D("calls"));
    assert(strstr(calls, "--check ") && strstr(calls, " --secure-boot --secure-boot-microsoft"));
    assert(strstr(calls, "--user=boot-test ")); /* what Ctrl+V pasted */
    printf("copied: [%s]\n", copied[0]);
    assert(strcmp(copied[0], "copy me") == 0); /* cut from the line; the password never */
    write_file(D("describe.json"), describe, 0644);
    k('\n'); k(K_EOL); k('\n'); k(K_DOWN); k('\n'); k(K_EOL); k('\n'); k(SNAP);
    run("no setup mode: no Secure Boot rows");
    assert(snapped("Encrypt the disk") && !snapped("Secure Boot with this computer's own keys"));

    /* The System page and summary show the machine model and app settings;
     * locachy-install still receives the stable machine ID. */
    k('\n');                    /* Install LOCachy */
    k(K_EOL); k('\n');          /* Network: Next */
    k(K_DOWN); k('\n');         /* Disk: the first disk */
    k(K_EOL); k('\n');
    k(K_DOWN); k('\n'); type("correct horse"); k('\n'); type("correct horse"); k('\n');
    k(K_EOL); k('\n');          /* Encryption: Next */
    k('\n'); k(21); type("boot-test"); k('\n');
    k(K_DOWN); k(K_DOWN); k('\n'); type("Locachy123"); k('\n'); type("Locachy123"); k('\n');
    for (int s = S_USER; s < S_SYSTEM; s++) { k(K_EOL); k('\n'); }
    k(K_BOL); k(K_RIGHT); k(SNAP); /* choose the OMEN profile by model */
    k(K_EOL); k(K_UP); k(K_UP); k(K_UP);                     /* above Next and Hibernate after */
    k('\n'); k(21); type("off"); k('\n');
    k(K_DOWN); k('\n'); k(21); type("next"); k('\n');
    k(SNAP);
    k(K_EOL); k('\n');          /* System: Next */
    k(SNAP);
    k(K_EOL); k('\n'); k('n');  /* Install: --check, then no */
    run("OMEN model and app presets on the System page and summary");
    assert(snapped("< HP OMEN 17-ck1xxx (board 8A1A) >"));
    assert(snapped("HP OMEN 17-ck1xxx (board 8A1A); packages for"));
    char row[160];
    snprintf(row, sizeof row, "%-30s %s", "Branch", "main");
    assert(snapped(row));
    snprintf(row, sizeof row, "%-30s %s", "App presets from", "off");
    assert(snapped(row));
    snprintf(row, sizeof row, "%-30s %s", "App presets branch", "next");
    assert(snapped(row));
    snprintf(row, sizeof row, "%-30s %s", "App presets from", "off (next)");
    assert(snapped(row));
    snprintf(row, sizeof row, "%-30s %s", "App presets", "this medium's own (updates are off)");
    assert(snapped(row));
    snprintf(row, sizeof row, "%-30s %s", "LOCachy source", "the newest from the repository above");
    assert(snapped(row));
    calls = slurp(D("calls"));
    assert(strstr(calls, "--check ") && strstr(calls, " --update-branch=main --apps-repo=off --apps-branch=next"));
    assert(strstr(calls, " --machine=hp-omen-17-ck1 "));

    test_multi_disk_storage_and_winmaint();
    test_session_handoff();
    test_refresh_exec_failure();
    test_recovered_preflight();
    printf("PASS installer\n");
    return 0;
}
