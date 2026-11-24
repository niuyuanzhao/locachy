/* Settings' GPU selection, extended display, paired devices, compute node,
 * and Maintenance Windows rows. Includes ../progs/settings.c with doubles,
 * following test-settings-optional.c.
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

static char fake_winmaint_state[256] = "/tmp/nonexistent-winmaint-state.json";
#define WINMAINT_STATE_FILE fake_winmaint_state
#define SETTINGS_COMMAND_OUTPUT
int settings_access(const char *path, int mode);
int settings_kill(pid_t pid, int sig);
#define access settings_access
#define kill settings_kill
#define main settings_main
#include "../progs/settings.c"
#undef main
#undef kill
#undef access
#include "../base/fontmatch.c"
#include "i18n-stub.h"
#include "jobs-double.h"

static int reloads, fail_rc;
static char rc_name[32][64], rc_value[32][256];
static int rc_count;
static int fake_pair_installed, fake_winmaint_installed;
static const char *fake_pair_output = "";
static int term_calls, term_status;
static char term_last[512];
static struct screen_card_info fake_cards[4];
static int nfake_cards;

void key_close_kept(void) {}
int key_get(void) { abort(); }
void say(const char *fmt, ...) { (void)fmt; }

int screen_list_cards(struct screen_card_info *out, int max)
{
    int n = nfake_cards < max ? nfake_cards : max;
    if (n > 0) memcpy(out, fake_cards, (size_t)n * sizeof *out);
    return n;
}

static void command_output(char *const argv[], char *buf, size_t size)
{
    buf[0] = '\0';
    if (argv[0] && strcmp(argv[0], LOCACHY_PAIR) == 0 && argv[1] && strcmp(argv[1], "list") == 0 &&
        argv[2] && strcmp(argv[2], "--machine") == 0 && argv[3] == NULL) {
        snprintf(buf, size, "%s", fake_pair_output);
    }
}

int rc_set(const char *name, const char *value)
{
    if (fail_rc) return -1;
    int i = 0;
    while (i < rc_count && strcmp(rc_name[i], name) != 0) i++;
    if (!value) {
        if (i < rc_count) {
            for (int k = i; k + 1 < rc_count; k++) {
                memcpy(rc_name[k], rc_name[k + 1], sizeof rc_name[k]);
                memcpy(rc_value[k], rc_value[k + 1], sizeof rc_value[k]);
            }
            rc_count--;
        }
        return 0;
    }
    if (i == rc_count) {
        assert(rc_count < 32);
        snprintf(rc_name[rc_count++], sizeof rc_name[0], "%s", name);
    }
    snprintf(rc_value[i], sizeof rc_value[i], "%s", value);
    return 0;
}

static void (*volatile rc_callback)(const char *, const char *, void *);

int rc_read(void (*fn)(const char *, const char *, void *), void *arg)
{
    rc_callback = fn;
    for (int i = 0; i < rc_count; i++) rc_callback(rc_name[i], rc_value[i], arg);
    return 0;
}

static const char *test_rc(const char *name)
{
    for (int i = 0; i < rc_count; i++) {
        if (strcmp(rc_name[i], name) == 0) return rc_value[i];
    }
    return NULL;
}

int settings_access(const char *path, int mode)
{
    if (strcmp(path, LOCACHY_PAIR) == 0) {
        if (fake_pair_installed) return 0;
        errno = ENOENT;
        return -1;
    }
    if (strcmp(path, LOCACHY_WINMAINT) == 0) {
        if (fake_winmaint_installed) return 0;
        errno = ENOENT;
        return -1;
    }
    return access(path, mode);
}

int settings_kill(pid_t pid, int sig)
{
    assert(pid == 1 && sig == SIGHUP);
    reloads++;
    return 0;
}

int base_call(char *const argv[])
{
    assert(argv[0] && strcmp(argv[0], TERM_BIN) == 0);
    term_last[0] = '\0';
    for (int i = 1; argv[i]; i++) {
        size_t len = strlen(term_last);
        snprintf(term_last + len, sizeof term_last - len, "%s%s", len ? " " : "", argv[i]);
    }
    term_calls++;
    return term_status;
}

int monitor_internal(const char *name) { (void)name; return 0; }
int monitor_profiles_connected(struct monitor_identity *out, int max) { (void)out; (void)max; abort(); }
int monitor_profiles_save(const struct monitor_profile_entry *entries, int n) { (void)entries; (void)n; abort(); }
double uptime(void) { static double t; return t += 10; }
int screen_cols(void) { return 100; }
int screen_rows(void) { return 30; }
int ui_cells(const char *s) { return (int)strlen(s); }
const char *ui_pad(char *out, size_t size, const char *s, int cells)
{
    size_t n = strlen(s);
    if (n >= size) n = size - 1;
    memcpy(out, s, n);
    while ((int)n < cells && n + 1 < size) out[n++] = ' ';
    out[n] = '\0';
    return out;
}
void screen_fill(int x, int y, int w, int h, uint32_t color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
void screen_text(int x, int y, const char *text, uint32_t fg, uint32_t bg)
{ (void)x; (void)y; (void)text; (void)fg; (void)bg; }
void screen_flush(void) {}
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; abort(); }
int key_poll_timeout(int fd, int timeout) { (void)fd; (void)timeout; abort(); }
int key_poll_many(struct pollfd *extra, size_t count, int timeout) { (void)extra; (void)count; (void)timeout; abort(); }
int screen_rescale(void) { abort(); }
int open_candidates(const char *type, char names[][OPEN_NAME], int max) { (void)type; (void)names; (void)max; abort(); }
int open_set_rule(const char *type, const char *value) { (void)type; (void)value; abort(); }
int open_containers(char names[][OPEN_NAME], int max) { (void)names; (void)max; abort(); }
int open_host(const char *name) { (void)name; return 0; }
void open_label(const char *name, const char *type, char *label, size_t size)
{ (void)type; snprintf(label, size, "%s", name ? name : ""); }
const char *keys_layout_name(const char *field) { (void)field; return "us"; }

static void write_state(const char *content)
{
    FILE *f = fopen(fake_winmaint_state, "w");
    assert(f && fputs(content, f) >= 0 && fclose(f) == 0);
}

int main(void)
{
    char line[512];
    char tmpdir[] = "/tmp/locachy-settings-feat-XXXXXX";
    assert(mkdtemp(tmpdir) != NULL);
    snprintf(fake_winmaint_state, sizeof fake_winmaint_state, "%s/state.json", tmpdir);

    /* 1. Category coverage: every setting index appears exactly once. */
    int counted[512] = { 0 };
    int total = num_hw_items();
    assert(total < 512);
    for (int c = 0; c < NCATS; c++) {
        nentries = 0;
        category_entries(c);
        for (int k = 0; k < nentries; k++) {
            if (entries[k].idx >= 0 && entries[k].idx < total) counted[entries[k].idx]++;
        }
    }
    for (int i = 0; i < total; i++) assert(counted[i] == 1);
    assert(category_of(display_card_index()) == CAT_INPUT);
    assert(category_of(extd_card_index()) == CAT_INPUT);
    assert(category_of(extd_tb_shared_index()) == CAT_INPUT);
    assert(category_of(extd_paired_only_index()) == CAT_INPUT);
    assert(category_of(paired_devices_index()) == CAT_DEVICES);
    assert(category_of(compute_index()) == CAT_NETWORK);
    assert(category_of(winmaint_index()) == CAT_SYSTEM);
    puts("PASS category placement and unique index chain for all new rows");

    /* 2. Display GPU and Extended display GPU selectors. */
    nfake_cards = 0;
    selected = display_card_index();
    adjust(+1);
    assert(strstr(notice, "no DRM cards found") && reloads == 0);

    nfake_cards = 2;
    snprintf(fake_cards[0].name, sizeof fake_cards[0].name, "card0");
    snprintf(fake_cards[0].driver, sizeof fake_cards[0].driver, "i915");
    snprintf(fake_cards[0].pci, sizeof fake_cards[0].pci, "0000:00:02.0");
    fake_cards[0].connected_outputs = 1;
    snprintf(fake_cards[1].name, sizeof fake_cards[1].name, "card1");
    snprintf(fake_cards[1].driver, sizeof fake_cards[1].driver, "nvidia");
    snprintf(fake_cards[1].pci, sizeof fake_cards[1].pci, "0000:01:00.0");
    fake_cards[1].connected_outputs = 0;

    item_text(display_card_index(), line, sizeof line);
    assert(strstr(line, "Display GPU") && strstr(line, "< auto >"));
    adjust(+1);
    assert(!strcmp(display_card_cfg, "card0") && test_rc("display_card") && !strcmp(test_rc("display_card"), "card0"));
    assert(reloads == 0); /* takes effect on next start */
    item_text(display_card_index(), line, sizeof line);
    assert(strstr(line, "< card0 (i915) >"));
    adjust(+1);
    assert(!strcmp(display_card_cfg, "card1") && !strcmp(test_rc("display_card"), "card1"));
    adjust(+1);
    assert(!strcmp(display_card_cfg, "auto") && !strcmp(test_rc("display_card"), "auto"));

    on_rc("extdisplay_card", "i915", NULL);
    item_text(extd_card_index(), line, sizeof line);
    assert(strstr(line, "Extended display GPU") && strstr(line, "< i915 (card0) >"));
    selected = extd_card_index();
    adjust(+1);
    assert(!strcmp(extdisplay_card_cfg, "card1") && !strcmp(test_rc("extdisplay_card"), "card1"));
    adjust(-1);
    assert(!strcmp(extdisplay_card_cfg, "card0") && !strcmp(test_rc("extdisplay_card"), "card0"));
    puts("PASS display_card and extdisplay_card cycle auto and sysfs DRM cards");

    /* 3. extdisplay_tb_shared (cycles auto/on/off + queues tb-sync) and extdisplay_paired_only. */
    selected = extd_tb_shared_index();
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    adjust(+1);
    assert(extd_tb_shared == 1 && test_rc("extdisplay_tb_shared") && !strcmp(test_rc("extdisplay_tb_shared"), "1"));
    assert(tb_sync_job != 0 && !strcmp(jobs_double_last, "tb-sync"));
    item_text(extd_tb_shared_index(), line, sizeof line);
    assert(strstr(line, "< on >") && strstr(line, "working..."));
    jobs_double_ended = 1;
    tb_sync_check();
    assert(tb_sync_job == 0 && strstr(notice, "Thunderbolt filter updated"));

    adjust(+1);
    assert(extd_tb_shared == 0 && !strcmp(test_rc("extdisplay_tb_shared"), "0") && !strcmp(jobs_double_last, "tb-sync"));
    tb_sync_check();
    adjust(+1);
    assert(extd_tb_shared == -1 && test_rc("extdisplay_tb_shared") == NULL && !strcmp(jobs_double_last, "tb-sync"));
    tb_sync_check();

    selected = extd_paired_only_index();
    assert(!extd_paired_only);
    adjust(+1);
    assert(extd_paired_only == 1 && test_rc("extdisplay_paired_only") && !strcmp(test_rc("extdisplay_paired_only"), "1"));
    adjust(-1);
    assert(extd_paired_only == 0 && !strcmp(test_rc("extdisplay_paired_only"), "0"));
    puts("PASS extdisplay_tb_shared triggers tb-sync and extdisplay_paired_only toggles cleanly");

    /* 4. Paired devices: count parsing + terminal launch (no secrets). */
    fake_pair_installed = 0;
    load_paired_peers();
    assert(paired_peers_count == -1);
    item_text(paired_devices_index(), line, sizeof line);
    assert(strstr(line, "Paired devices") && strstr(line, "Manage"));
    manage_paired_devices();
    assert(term_calls == 0 && strstr(notice, "locachy-pair is not installed"));

    fake_pair_installed = 1;
    fake_pair_output = "0123456789abcdef\tWork Mac\textdisplay,compute\t2026-11-23T00:00:00Z\taabbccdd\n"
                       "fedcba9876543210\tStudio\tcompute\t2026-11-23T01:00:00Z\t11223344\n";
    load_paired_peers();
    assert(paired_peers_count == 2);
    item_text(paired_devices_index(), line, sizeof line);
    assert(strstr(line, "2 paired"));
    assert(enter_row(paired_devices_index()));
    manage_paired_devices();
    assert(term_calls == 1 && strstr(term_last, "/usr/bin/sudo " LOCACHY_PAIR " list"));
    assert(!strstr(term_last, "psk") && !strstr(notice, "psk"));
    puts("PASS paired devices parses list --machine count and opens terminal with sudo locachy-pair list");

    /* 5. Compute node rows: service_compute (SIGHUP), compute_bind_tb (tb-sync), compute_gpu. */
    reloads = 0;
    selected = compute_index();
    adjust(+1);
    assert(compute_on == 1 && test_rc("service_compute") && !strcmp(test_rc("service_compute"), COMPUTE_SERVICE));
    assert(reloads == 1);
    adjust(-1);
    assert(compute_on == 0 && test_rc("service_compute") == NULL && reloads == 2);

    selected = compute_index() + 1;
    jobs_double_last[0] = '\0';
    adjust(+1);
    assert(compute_bind_tb == 1 && !strcmp(test_rc("compute_bind_tb"), "1") && !strcmp(jobs_double_last, "tb-sync"));
    tb_sync_check();

    selected = compute_index() + 2;
    assert(compute_gpu == 1);
    adjust(+1);
    assert(compute_gpu == 0 && !strcmp(test_rc("compute_gpu"), "0"));
    adjust(+1);
    assert(compute_gpu == 1 && !strcmp(test_rc("compute_gpu"), "1"));
    on_rc("compute_gpu", "0", NULL);
    assert(compute_gpu == 0);
    on_rc("compute_gpu", "", NULL);
    assert(compute_gpu == 1);
    puts("PASS compute rows write rc.conf, signal PID 1 on service_compute, and run tb-sync on compute_bind_tb");

    /* 6. Maintenance Windows: size cycling, state.json parsing, create/reclaim, boot-once, heal-boot, vm. */
    selected = winmaint_index();
    assert(winmaint_size_gib == 48);
    adjust(+1);
    assert(winmaint_size_gib == 64 && !strcmp(test_rc("winmaint_size_gib"), "64"));
    adjust(+1);
    assert(winmaint_size_gib == 96);
    adjust(+1);
    assert(winmaint_size_gib == 128);
    adjust(+1);
    assert(winmaint_size_gib == 40);
    adjust(-1);
    assert(winmaint_size_gib == 128);
    on_rc("winmaint_size_gib", "64", NULL);
    assert(winmaint_size_gib == 64);
    on_rc("winmaint_size_gib", "10", NULL); /* out of range: ignored */
    assert(winmaint_size_gib == 64);

    /* Not created -> Enter queues winmaint-create --size-gib 64. */
    unlink(fake_winmaint_state);
    item_text(winmaint_index() + 1, line, sizeof line);
    assert(strstr(line, "Maintenance Windows") && strstr(line, "not created"));
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    winmaint_status_row();
    assert(winmaint_job != 0 && !strcmp(jobs_double_last, "winmaint-create --size-gib 64"));
    item_text(winmaint_index() + 1, line, sizeof line);
    assert(strstr(line, "working..."));

    /* Write realistic state.json (48 GiB = 100663296 sectors) and complete job. */
    write_state("{\n"
                "  \"disk\": \"/dev/nvme0n1\",\n"
                "  \"disk_guid\": \"11111111-2222-3333-4444-555555555555\",\n"
                "  \"partitions\": {\n"
                "    \"c\": {\n"
                "      \"name\": \"LOCACHY_WIN_C\",\n"
                "      \"partn\": 6,\n"
                "      \"sectors\": 100663296\n"
                "    },\n"
                "    \"esp\": { \"sectors\": 262144 },\n"
                "    \"msr\": { \"sectors\": 32768 }\n"
                "  },\n"
                "  \"schema\": 1\n"
                "}\n");
    jobs_double_ended = 1;
    winmaint_check();
    assert(winmaint_job == 0 && winmaint_created == 1 && winmaint_created_gib == 48);
    item_text(winmaint_index() + 1, line, sizeof line);
    assert(strstr(line, "created, 48 GiB") && !strstr(line, "11111111"));

    /* Reclaim requires two consecutive Enters. */
    jobs_double_last[0] = '\0';
    winmaint_status_row();
    assert(winmaint_reclaim_asked == 1 && winmaint_job == 0 && jobs_double_last[0] == '\0');
    assert(!strcmp(notice, "Enter again: reclaim the Windows partition (the second Enter removes it)"));
    jobs_double_ended = 0;
    winmaint_status_row();
    assert(winmaint_reclaim_asked == 0 && winmaint_job != 0 && !strcmp(jobs_double_last, "winmaint-reclaim"));
    jobs_double_ended = 1;
    winmaint_check();
    assert(winmaint_job == 0);

    /* Boot once requires two consecutive Enters (and created state). */
    jobs_double_last[0] = '\0';
    winmaint_boot_once_row();
    assert(winmaint_boot_once_asked == 1 && winmaint_job == 0 && jobs_double_last[0] == '\0');
    jobs_double_ended = 0;
    winmaint_boot_once_row();
    assert(winmaint_boot_once_asked == 0 && winmaint_job != 0 && !strcmp(jobs_double_last, "winmaint-boot-once"));
    jobs_double_ended = 1;
    winmaint_check();
    assert(winmaint_job == 0);

    /* Heal boot queues winmaint-heal-boot immediately. */
    jobs_double_last[0] = '\0';
    jobs_double_ended = 0;
    winmaint_heal_boot_row();
    assert(winmaint_job != 0 && !strcmp(jobs_double_last, "winmaint-heal-boot"));
    jobs_double_ended = 1;
    winmaint_check();
    assert(winmaint_job == 0);

    /* Windows VM opens terminal with sudo /usr/local/bin/locachy-winmaint vm. */
    fake_winmaint_installed = 1;
    term_last[0] = '\0';
    winmaint_vm_row();
    assert(term_calls == 2 && strstr(term_last, "/usr/bin/sudo " LOCACHY_WINMAINT " vm"));
    char hint[256];
    row_hint_text(winmaint_index() + 4, hint, sizeof hint);
    assert(strstr(hint, "graphical session"));
    puts("PASS Maintenance Windows size, state.json, create, two-step reclaim/boot-once, heal-boot, and VM");

    /* 7. Job adoption for winmaint-* and tb-sync. */
    winmaint_job = tb_sync_job = 0;
    winmaint_what[0] = '\0';
    jobs_double_mine[0] = (struct jobs_row){ .id = 7201, .kind = "winmaint-create", .state = "running", .position = 0 };
    jobs_double_mine[1] = (struct jobs_row){ .id = 7202, .kind = "tb-sync", .state = "queued", .position = 1 };
    jobs_double_nmine = 2;
    adopt_jobs();
    assert(winmaint_job == 7201 && !strcmp(winmaint_what, "create") && tb_sync_job == 7202);
    item_text(winmaint_index() + 1, line, sizeof line);
    assert(strstr(line, "working..."));
    item_text(extd_tb_shared_index(), line, sizeof line);
    assert(strstr(line, "working..."));
    puts("PASS adopt_jobs follows winmaint-* and tb-sync jobs");

    unlink(fake_winmaint_state);
    rmdir(tmpdir);
    puts("PASS test-settings-features: all checks passed.");
    return 0;
}
