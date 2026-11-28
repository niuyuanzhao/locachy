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
static char fake_vm_conf_dir[256] = "/tmp/nonexistent-vm-conf";
static char fake_vm_disks_dir[256] = "/tmp/nonexistent-vm-disks";
static char fake_vm_run_dir[256] = "/tmp/nonexistent-vm-run";
#define WINMAINT_STATE_FILE fake_winmaint_state
#define VM_CONF_DIR fake_vm_conf_dir
#define VM_DISKS_DIR fake_vm_disks_dir
#define VM_APPS_RUN_DIR fake_vm_run_dir
#define SETTINGS_COMMAND_OUTPUT
#define SETTINGS_SSH_ADMIN
#define SETTINGS_COMPUTE_RUN
int settings_access(const char *path, int mode);
int settings_kill(pid_t pid, int sig);
struct passwd *settings_getpwuid(uid_t uid);
#define access settings_access
#define kill settings_kill
#define getpwuid settings_getpwuid
#define main settings_main
#include "../progs/settings.c"
#undef main
#undef getpwuid
#undef kill
#undef access
#include "../base/fontmatch.c"
#include "i18n-stub.h"
#include "jobs-double.h"

/* A managed desktop app (base_app_action, process 1's control channel): the test's answer. */
static int fake_winmaint_app;
static char app_last[64];
int base_app_action(const char *action, const char *name, char *why, size_t size)
{
    snprintf(app_last, sizeof app_last, "%s %s", action, name);
    snprintf(why, size, "%s", "no desktop session in tests");
    return fake_winmaint_app ? 0 : -1;
}

static int reloads, fail_rc;
static char rc_name[32][64], rc_value[32][256];
static int rc_count;
static int fake_pair_installed, fake_winmaint_installed, fake_gpu_unlock_installed;
static int fake_nvidia_installed = 1, fake_compute_installed = 1, fake_winiso_installed = 1;
static int fake_ssh_installed = 1;
static const char *fake_winiso_win_path = "";
static const char *fake_winiso_virtio_path = "";
static int ssh_reload_calls, ssh_regen_calls, ssh_keys_add_calls, ssh_keys_remove_calls;
static int ssh_admin_fail_next;
static char ssh_last_user[64], ssh_last_added[256], ssh_last_removed[16];
static char fake_ssh_keys_list[2048];
static char fake_ssh_host_fp[128];
static const char *fake_pair_output = "";
static const char *fake_winmaint_status = "";
static const char *fake_graphics_status = "";
static const char *fake_nvidia_status = "";
static const char *fake_compute_status = "";
static const char *fake_compute_ssh_config = "";
static int fake_compute_exec_rc;
static int compute_exec_calls;
static char compute_exec_last[256];
static int clip_calls;
static char clip_last[256];
static int term_calls, term_status;
static char term_last[512];
static struct screen_card_info fake_cards[4];
static int nfake_cards;
static int fake_keys[256];
static int nfake_keys, fake_key_pos;

static void push_keys_str(const char *s, int trailing_key)
{
    nfake_keys = fake_key_pos = 0;
    for (size_t i = 0; s && s[i] && nfake_keys < 255; i++) {
        fake_keys[nfake_keys++] = (unsigned char)s[i];
    }
    if (trailing_key && nfake_keys < 256) {
        fake_keys[nfake_keys++] = trailing_key;
    }
}

void key_close_kept(void) {}
int key_get(void)
{
    assert(fake_key_pos < nfake_keys);
    return fake_keys[fake_key_pos++];
}
void say(const char *fmt, ...) { (void)fmt; }

struct passwd *settings_getpwuid(uid_t uid)
{
    static struct passwd pw = { .pw_name = "testuser", .pw_uid = 1000, .pw_gid = 1000 };
    return uid == 1000 ? &pw : NULL;
}

static int ssh_admin_run(char *const argv[], const char *stdin_text, char *out, size_t out_size)
{
    if (out && out_size > 0) out[0] = '\0';
    assert(argv[0] && strcmp(argv[0], SSH_ADMIN_CMD) == 0 && argv[1]);
    if (ssh_admin_fail_next) {
        ssh_admin_fail_next = 0;
        if (out && out_size > 0) snprintf(out, out_size, "simulated ssh admin error");
        return 1;
    }
    if (strcmp(argv[1], "--reload") == 0) {
        ssh_reload_calls++;
        return 0;
    }
    if (strcmp(argv[1], "--keys-list") == 0) {
        assert(argv[2]);
        snprintf(ssh_last_user, sizeof ssh_last_user, "%s", argv[2]);
        if (out && out_size > 0) snprintf(out, out_size, "%s", fake_ssh_keys_list);
        return 0;
    }
    if (strcmp(argv[1], "--keys-add") == 0) {
        assert(argv[2] && stdin_text);
        ssh_keys_add_calls++;
        snprintf(ssh_last_user, sizeof ssh_last_user, "%s", argv[2]);
        snprintf(ssh_last_added, sizeof ssh_last_added, "%s", stdin_text);
        if (strstr(stdin_text, "ssh-ed25519") == NULL && strstr(stdin_text, "ssh-rsa") == NULL) {
            if (out && out_size > 0) snprintf(out, out_size, "expected a valid SSH public key");
            return 1;
        }
        size_t cur = strlen(fake_ssh_keys_list);
        int next_idx = 1;
        for (const char *p = fake_ssh_keys_list; *p; p++) {
            if (*p == '\n') next_idx++;
        }
        snprintf(fake_ssh_keys_list + cur, sizeof fake_ssh_keys_list - cur,
                 "%d\tssh-ed25519\tSHA256:addedkey%d\tuser@host\n", next_idx, next_idx);
        if (out && out_size > 0) snprintf(out, out_size, "SHA256:addedkey%d", next_idx);
        return 0;
    }
    if (strcmp(argv[1], "--keys-remove") == 0) {
        assert(argv[2] && argv[3]);
        ssh_keys_remove_calls++;
        snprintf(ssh_last_user, sizeof ssh_last_user, "%s", argv[2]);
        snprintf(ssh_last_removed, sizeof ssh_last_removed, "%s", argv[3]);
        char kept[sizeof fake_ssh_keys_list] = "";
        int target = atoi(argv[3]);
        char copy[sizeof fake_ssh_keys_list];
        snprintf(copy, sizeof copy, "%s", fake_ssh_keys_list);
        char *save = NULL;
        int new_idx = 1;
        for (char *ln = strtok_r(copy, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            int cur_idx = atoi(ln);
            if (cur_idx == target) continue;
            char *tab = strchr(ln, '\t');
            if (!tab) continue;
            size_t klen = strlen(kept);
            snprintf(kept + klen, sizeof kept - klen, "%d%s\n", new_idx++, tab);
        }
        snprintf(fake_ssh_keys_list, sizeof fake_ssh_keys_list, "%s", kept);
        return 0;
    }
    if (strcmp(argv[1], "--host-key-fingerprint") == 0) {
        if (out && out_size > 0) snprintf(out, out_size, "%s", fake_ssh_host_fp);
        return 0;
    }
    if (strcmp(argv[1], "--host-key-regen") == 0) {
        ssh_regen_calls++;
        snprintf(fake_ssh_host_fp, sizeof fake_ssh_host_fp, "SHA256:regenfp%d", ssh_regen_calls);
        if (out && out_size > 0) snprintf(out, out_size, "%s", fake_ssh_host_fp);
        return 0;
    }
    return 1;
}

static int compute_exec(char *const argv[], char *out, size_t out_size)
{
    if (out && out_size > 0) out[0] = '\0';
    compute_exec_last[0] = '\0';
    for (int i = 0; argv[i]; i++) {
        size_t len = strlen(compute_exec_last);
        snprintf(compute_exec_last + len, sizeof compute_exec_last - len, "%s%s", len ? " " : "", argv[i]);
    }
    compute_exec_calls++;
    if (argv[1] && strcmp(argv[1], "ssh-config") == 0 && fake_compute_exec_rc == 0) {
        if (out && out_size > 0) snprintf(out, out_size, "%s", fake_compute_ssh_config);
    } else if (argv[1] && strcmp(argv[1], "job") == 0 && argv[2] && strcmp(argv[2], "log") == 0) {
        if (out && out_size > 0) snprintf(out, out_size, "epoch 1 loss=0.42\n");
    }
    return fake_compute_exec_rc;
}

int clipboard_set(int primary, const void *text, size_t size)
{
    (void)primary;
    clip_calls++;
    size_t n = size < sizeof clip_last - 1 ? size : sizeof clip_last - 1;
    memcpy(clip_last, text, n);
    clip_last[n] = '\0';
    return 0;
}

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
    if (argv[0] && strcmp(argv[0], LOCACHY_WINMAINT) == 0 && argv[1] && strcmp(argv[1], "status") == 0 &&
        argv[2] && strcmp(argv[2], "--json") == 0 && argv[3] == NULL) {
        snprintf(buf, size, "%s", fake_winmaint_status);
    }
    if (argv[0] && strcmp(argv[0], GPU_GRAPHICS_HELPER) == 0 && argv[1] && strcmp(argv[1], "status") == 0 &&
        argv[2] && strcmp(argv[2], "--tsv") == 0 && argv[3] == NULL) {
        snprintf(buf, size, "%s", fake_graphics_status);
    }
    if (argv[0] && strcmp(argv[0], LOCACHY_NVIDIA) == 0 && argv[1] && strcmp(argv[1], "status") == 0 &&
        argv[2] && strcmp(argv[2], "--tsv") == 0 && argv[3] == NULL) {
        snprintf(buf, size, "%s", fake_nvidia_status);
    }
    if (argv[0] && strcmp(argv[0], LOCACHY_COMPUTE) == 0 && argv[1] && strcmp(argv[1], "status") == 0 &&
        argv[2] && strcmp(argv[2], "--tsv") == 0 && argv[3] == NULL) {
        snprintf(buf, size, "%s", fake_compute_status);
    }
    if (argv[0] && strcmp(argv[0], LOCACHY_WINISO) == 0 && argv[1] && strcmp(argv[1], "path") == 0 && argv[2]) {
        if (strcmp(argv[2], "windows") == 0) snprintf(buf, size, "%s", fake_winiso_win_path);
        else if (strcmp(argv[2], "virtio-win") == 0) snprintf(buf, size, "%s", fake_winiso_virtio_path);
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
    if (strcmp(path, GPU_UNLOCK_DOC) == 0) {
        if (fake_gpu_unlock_installed) return 0;
        errno = ENOENT;
        return -1;
    }
    if (strcmp(path, SSH_ADMIN_CMD) == 0) {
        if (fake_ssh_installed) return 0;
        errno = ENOENT;
        return -1;
    }
    if (strcmp(path, LOCACHY_NVIDIA) == 0) {
        if (fake_nvidia_installed) return 0;
        errno = ENOENT;
        return -1;
    }
    if (strcmp(path, LOCACHY_COMPUTE) == 0) {
        if (fake_compute_installed) return 0;
        errno = ENOENT;
        return -1;
    }
    if (strcmp(path, LOCACHY_WINISO) == 0) {
        if (fake_winiso_installed) return 0;
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
void screen_bar(const char *title, const char *ime) { (void)title; (void)ime; }
void ui_hits_clear(void) {}
void ui_hit(int col, int row, int ncols, int nrows, int id)
{ (void)col; (void)row; (void)ncols; (void)nrows; (void)id; }
int ui_hit_at(int col, int row) { (void)col; (void)row; return -1; }
int ui_button(int col, int row, const char *label, int id)
{ (void)col; (void)row; (void)label; (void)id; return 0; }
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
    snprintf(fake_vm_conf_dir, sizeof fake_vm_conf_dir, "%s/containers", tmpdir);
    snprintf(fake_vm_disks_dir, sizeof fake_vm_disks_dir, "%s/vm-disks", tmpdir);
    snprintf(fake_vm_run_dir, sizeof fake_vm_run_dir, "%s/apps-run", tmpdir);
    mkdir(fake_vm_conf_dir, 0755);
    mkdir(fake_vm_disks_dir, 0755);
    mkdir(fake_vm_run_dir, 0755);

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
    assert(category_of(compute_index()) == CAT_COMPUTE);
    assert(category_of(winmaint_index()) == CAT_COMPUTE);
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

    /* 4b. Dedicated 5-row SSH Server (sshd) section under CAT_NETWORK, cleanly
     * separated from Tailscale (3 rows). */
    assert(cl_index() == ts_index() + 3);
    assert(category_of(ssh_index()) == CAT_NETWORK);
    assert(category_of(ssh_index() + 4) == CAT_NETWORK);
    nentries = 0;
    category_entries(CAT_NETWORK);
    int ts_title_k = -1, ssh_title_k = -1, ts_rows = 0, ssh_rows = 0;
    for (int k = 0; k < nentries; k++) {
        if (entries[k].idx == E_TITLE && strcmp(entries[k].text, "Tailscale") == 0) ts_title_k = k;
        if (entries[k].idx == E_TITLE && strcmp(entries[k].text, "SSH Server (sshd)") == 0) ssh_title_k = k;
        if (entries[k].idx >= ts_index() && entries[k].idx < ts_index() + 3) ts_rows++;
        if (entries[k].idx >= ssh_index() && entries[k].idx < ssh_index() + 5) ssh_rows++;
    }
    assert(ts_title_k >= 0 && entries[ts_title_k + 4].idx == E_GAP &&
           ssh_title_k == ts_title_k + 5 && ts_rows == 3 && ssh_rows == 5);
    assert(!enter_row(ssh_index()) && enter_row(ssh_index() + 1) && !enter_row(ssh_index() + 2) &&
           enter_row(ssh_index() + 3) && enter_row(ssh_index() + 4));

    /* Row 0: SSH server (sshd) mode cycling: Off -> Tailscale only -> LAN & Tailscale -> Off. */
    selected = ssh_index();
    reloads = ssh_reload_calls = 0;
    tailscale_enabled = 0;
    tor_on = 0;
    ssh_service_on = ssh_listen_lan = 0;
    item_text(selected, line, sizeof line);
    assert(strstr(line, "SSH server (sshd)") && strstr(line, "< Off >"));

    fail_rc = 1;
    adjust(+1);
    assert(!ssh_service_on && !test_rc("service_tailnet_ssh") && reloads == 0 && ssh_reload_calls == 0);
    fail_rc = 0;

    adjust(+1); /* Off -> Tailscale only (even while Tailscale is off: fail-closed note shown) */
    assert(ssh_service_on == 1 && ssh_listen_lan == 0 && ssh_mode_value() == 1);
    assert(!strcmp(test_rc("service_tailnet_ssh"), TAILNET_SSH_SERVICE));
    assert(!strcmp(test_rc("ssh_listen"), "tailscale"));
    assert(!test_rc("service_compute") && reloads == 1 && ssh_reload_calls == 1);
    assert(strstr(notice, "Tailscale is off"));
    item_text(selected, line, sizeof line);
    assert(strstr(line, "< Tailscale only >") && strstr(line, "(Tailscale is off)"));

    tor_on = 1;
    tailscale_enabled = 1;
    item_text(selected, line, sizeof line);
    assert(strstr(line, "< Tailscale only >") && !strstr(line, "(Tailscale is off)") &&
           strstr(line, "(blocked while Tor is on)"));

    adjust(+1); /* Tailscale only -> LAN & Tailscale */
    assert(ssh_service_on == 1 && ssh_listen_lan == 1 && ssh_mode_value() == 2);
    assert(!strcmp(test_rc("service_tailnet_ssh"), TAILNET_SSH_SERVICE));
    assert(!strcmp(test_rc("ssh_listen"), "lan"));
    assert(reloads == 2 && ssh_reload_calls == 2 && strstr(notice, "blocked while Tor is on"));
    item_text(selected, line, sizeof line);
    assert(strstr(line, "< LAN & Tailscale >") && strstr(line, "(blocked while Tor is on)"));
    tor_on = 0;
    item_text(selected, line, sizeof line);
    assert(strstr(line, "< LAN & Tailscale >") && !strstr(line, "blocked while Tor"));

    adjust(+1); /* LAN & Tailscale -> Off */
    assert(ssh_service_on == 0 && ssh_mode_value() == 0 && !test_rc("service_tailnet_ssh"));
    assert(reloads == 3 && ssh_reload_calls == 3);
    adjust(-1); /* Off -> LAN & Tailscale via Left */
    assert(ssh_service_on == 1 && ssh_listen_lan == 1 && ssh_mode_value() == 2);
    adjust(-1); /* LAN & Tailscale -> Tailscale only */
    assert(ssh_service_on == 1 && ssh_listen_lan == 0 && ssh_mode_value() == 1);
    adjust(-1); /* Tailscale only -> Off */
    assert(ssh_service_on == 0 && ssh_mode_value() == 0);
    on_rc("service_tailnet_ssh", TAILNET_SSH_SERVICE, NULL);
    on_rc("ssh_listen", "lan", NULL);
    assert(ssh_service_on == 1 && ssh_listen_lan == 1 && ssh_mode_value() == 2);
    on_rc("ssh_listen", "tailscale", NULL);
    assert(ssh_listen_lan == 0 && ssh_mode_value() == 1);
    on_rc("service_tailnet_ssh", "", NULL);
    assert(ssh_service_on == 0 && ssh_mode_value() == 0);

    /* Row 1: sshd port validation and persistence. */
    selected = ssh_index() + 1;
    assert(ssh_port == 22);
    item_text(selected, line, sizeof line);
    assert(strstr(line, "sshd port") && strstr(line, "22   (Enter)"));
    int reloads_before_port = ssh_reload_calls;
    push_keys_str("\x15" "0", '\n'); /* Ctrl+U then "0" -> rejected */
    ssh_edit_port_row();
    assert(ssh_port == 22 && !test_rc("ssh_port") && strstr(notice, "Invalid sshd port") &&
           ssh_reload_calls == reloads_before_port);
    push_keys_str("\x15" "70000", '\n');
    ssh_edit_port_row();
    assert(ssh_port == 22 && !test_rc("ssh_port") && strstr(notice, "Invalid sshd port"));
    push_keys_str("\x15" "abc", '\n');
    ssh_edit_port_row();
    assert(ssh_port == 22 && !test_rc("ssh_port") && strstr(notice, "Invalid sshd port"));
    push_keys_str("\x15" "2222", '\n');
    ssh_edit_port_row();
    assert(ssh_port == 2222 && test_rc("ssh_port") && !strcmp(test_rc("ssh_port"), "2222") &&
           ssh_reload_calls == reloads_before_port + 1);
    item_text(selected, line, sizeof line);
    assert(strstr(line, "2222   (Enter)"));
    on_rc("ssh_port", "22", NULL);
    assert(ssh_port == 22);
    on_rc("ssh_port", "99999", NULL);
    assert(ssh_port == 22);

    /* Row 2: sshd password login toggle. */
    selected = ssh_index() + 2;
    assert(ssh_password == 0);
    item_text(selected, line, sizeof line);
    assert(strstr(line, "sshd password login") && strstr(line, "[OFF]"));
    int reloads_before_pw = ssh_reload_calls;
    adjust(+1);
    assert(ssh_password == 1 && !strcmp(test_rc("ssh_password"), "1") &&
           ssh_reload_calls == reloads_before_pw + 1 && strstr(notice, "root login stays disabled"));
    item_text(selected, line, sizeof line);
    assert(strstr(line, "[ON ]"));
    adjust(+1);
    assert(ssh_password == 0 && !strcmp(test_rc("ssh_password"), "0") &&
           ssh_reload_calls == reloads_before_pw + 2 && strstr(notice, "public key only"));
    on_rc("ssh_password", "1", NULL);
    assert(ssh_password == 1);
    on_rc("ssh_password", "0", NULL);
    assert(ssh_password == 0);

    /* Row 3: sshd AuthorizedKeys count, modal add, and double-Delete remove. */
    fake_ssh_installed = 1;
    fake_ssh_keys_list[0] = '\0';
    nssh_keys = -1;
    item_text(ssh_index() + 3, line, sizeof line);
    assert(nssh_keys == 0 && strstr(line, "sshd AuthorizedKeys") && strstr(line, "0 keys   (Enter)"));
    /* Open modal, move down to "[ + Add public key ]" (row 3), paste an ed25519 key, then Esc back. */
    nfake_keys = fake_key_pos = 0;
    fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = '\n';
    const char *sample_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMockKeyData user@host";
    for (size_t i = 0; sample_key[i]; i++) fake_keys[nfake_keys++] = (unsigned char)sample_key[i];
    fake_keys[nfake_keys++] = '\n';
    fake_keys[nfake_keys++] = 27; /* Esc back */
    manage_ssh_authorized_keys();
    assert(fake_key_pos == nfake_keys);
    assert(ssh_keys_add_calls == 1 && !strcmp(ssh_last_user, "testuser") && strstr(ssh_last_added, "ssh-ed25519"));
    assert(nssh_keys == 1 && !strcmp(ssh_keys[0].type, "ssh-ed25519") &&
           !strcmp(ssh_keys[0].fingerprint, "SHA256:addedkey1"));
    item_text(ssh_index() + 3, line, sizeof line);
    assert(strstr(line, "1 key   (Enter)"));
    /* Open modal, move down to key 1 (row 4), press Delete twice to remove, then q (cancel) back. */
    nfake_keys = fake_key_pos = 0;
    fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = K_DEL;
    fake_keys[nfake_keys++] = K_DEL;
    fake_keys[nfake_keys++] = 'q';
    manage_ssh_authorized_keys();
    assert(fake_key_pos == nfake_keys);
    assert(ssh_keys_remove_calls == 1 && !strcmp(ssh_last_removed, "1") && nssh_keys == 0);
    item_text(ssh_index() + 3, line, sizeof line);
    assert(strstr(line, "0 keys   (Enter)"));

    /* Row 4: sshd host key fingerprint display + two-Enter regen + reset on cursor move. */
    fake_ssh_host_fp[0] = '\0';
    ssh_host_fp_loaded = 0;
    ssh_hostkey_regen_asked = 0;
    item_text(ssh_index() + 4, line, sizeof line);
    assert(strstr(line, "sshd host key") && strstr(line, "not generated yet   (Enter: generate)"));
    snprintf(fake_ssh_host_fp, sizeof fake_ssh_host_fp, "SHA256:initialhostfp");
    load_ssh_host_fp();
    item_text(ssh_index() + 4, line, sizeof line);
    assert(strstr(line, "SHA256:initialhostfp   (Enter: regenerate)"));
    /* First Enter asks confirmation; cursor move clears confirmation. */
    selected = ssh_index() + 4;
    ssh_regen_calls = 0;
    ssh_regen_host_key_row();
    assert(ssh_hostkey_regen_asked == 1 && ssh_regen_calls == 0 && strstr(notice, "Enter again"));
    nentries = 0;
    category_entries(CAT_NETWORK);
    page_move(-1);
    assert(selected == ssh_index() + 3 && ssh_hostkey_regen_asked == 0);
    /* A failed regeneration keeps the fingerprint and clears the confirmation. */
    selected = ssh_index() + 4;
    ssh_regen_host_key_row();
    ssh_admin_fail_next = 1;
    ssh_regen_host_key_row();
    assert(ssh_regen_calls == 0 && ssh_hostkey_regen_asked == 0 && strstr(notice, "Cannot regenerate sshd host key"));
    assert(!strcmp(ssh_host_fp, "SHA256:initialhostfp"));
    /* Two consecutive Enters regenerate and update cached fingerprint. */
    selected = ssh_index() + 4;
    ssh_regen_host_key_row();
    assert(ssh_hostkey_regen_asked == 1 && ssh_regen_calls == 0);
    ssh_regen_host_key_row();
    assert(ssh_hostkey_regen_asked == 0 && ssh_regen_calls == 1 &&
           !strcmp(ssh_host_fp, "SHA256:regenfp1") && strstr(notice, "known_hosts"));
    item_text(ssh_index() + 4, line, sizeof line);
    assert(strstr(line, "SHA256:regenfp1   (Enter: regenerate)"));
    /* Verify sshd row hints are non-empty and match actions. */
    char ssh_hint[256];
    row_hint_text(ssh_index() + 1, ssh_hint, sizeof ssh_hint);
    assert(strstr(ssh_hint, "port"));
    row_hint_text(ssh_index() + 3, ssh_hint, sizeof ssh_hint);
    assert(strstr(ssh_hint, "AuthorizedKeys"));
    row_hint_text(ssh_index() + 4, ssh_hint, sizeof ssh_hint);
    assert(strstr(ssh_hint, "host key"));
    puts("PASS dedicated 5-row SSH Server (sshd) section: mode, port, password, AuthorizedKeys, host key");

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

    /* Compute status TSV parsing, 6-row rendering, SSH config viewer + clipboard copy, and Compute Node Manager TUI. */
    assert(category_of(compute_index() + 3) == CAT_COMPUTE &&
           category_of(compute_index() + 4) == CAT_COMPUTE &&
           category_of(compute_index() + 5) == CAT_COMPUTE);
    assert(enter_row(compute_index() + 4) && enter_row(compute_index() + 5));
    fake_compute_status =
        "meta\tservice_enabled\t1\n"
        "meta\tdaemon_running\t1\n"
        "meta\tport\t27184\n"
        "meta\tbind_tb\t1\n"
        "meta\tgpu_enabled\t1\n"
        "meta\tstorage_volume\tdata\n"
        "meta\tstorage_mounted\t1\n"
        "meta\tstorage_base\t/srv/storage/data/locachy-compute\n"
        "meta\tstorage_free\t536870912000\n"
        "meta\tstorage_total\t1073741824000\n"
        "meta\tstorage_error\t\n"
        "meta\tgpu_devices\t3\n"
        "meta\tgpu_libs\t14\n"
        "meta\tnvidia_version\t595.45.04\n"
        "meta\tpaired_peers\t2\n"
        "meta\tssh_host\t100.64.0.9\n"
        "meta\tssh_user\talice\n"
        "meta\tssh_port\t22222\n"
        "meta\tssh_prefix\tcompute-node\n"
        "image\tpytorch-cu12\t2026-11-26T00:00:00Z\t/tmp/pt.tar.zst\n"
        "env\ttrain1\tpytorch-cu12\t1\tnone\tdata:/volumes/data:rw\t\t16G\t800000 100000\t/root\n"
        "job\tjob-42\ttrain1\trunning\t\tpython train.py\n"
        "vm\tdevvm\t0\t4096\t4\t2222\troot\n";
    compute_on = 1;
    snprintf(compute_storage_cfg, sizeof compute_storage_cfg, "data");
    load_compute_status();
    assert(compute_snap.loaded && compute_snap.daemon_running && compute_snap.nimages == 1 &&
           compute_snap.nenvs == 1 && compute_snap.njobs == 1 && compute_snap.njobs_running == 1 &&
           compute_snap.nvms == 1 && compute_snap.nvms_running == 0 &&
           strcmp(compute_snap.envs[0].volumes, "data:/volumes/data:rw") == 0 &&
           strcmp(compute_snap.envs[0].memory_max, "16G") == 0 &&
           strcmp(compute_snap.envs[0].cpu_max, "800000 100000") == 0 &&
           strcmp(compute_snap.envs[0].workdir, "/root") == 0);
    item_text(compute_index(), line, sizeof line);
    assert(strstr(line, "port 27184, running"));
    item_text(compute_index() + 2, line, sizeof line);
    assert(strstr(line, "NVIDIA 595.45.04"));
    item_text(compute_index() + 3, line, sizeof line);
    assert(strstr(line, "data") && strstr(line, "m: unmount"));
    item_text(compute_index() + 4, line, sizeof line);
    assert(strstr(line, "imgs:1") && strstr(line, "envs:1") && strstr(line, "jobs:1/1") && strstr(line, "vms:0/1"));
    item_text(compute_index() + 5, line, sizeof line);
    assert(strstr(line, "ssh compute-node -p 22222"));

    /* Client SSH config viewer copies to clipboard on 'c' and exits cleanly on Esc. */
    fake_compute_ssh_config = "Host compute-node\n    HostName 100.64.0.9\n    User alice\n    Port 22222\n";
    fake_compute_exec_rc = 0;
    clip_calls = 0;
    clip_last[0] = '\0';
    push_keys_str("c", 27);
    compute_view_ssh_config();
    assert(clip_calls == 1 && strstr(clip_last, "Host compute-node") && strstr(notice, "Copied"));

    /* Single-page sectioned Compute Node Manager TUI (zero tabs):
     * With 1 env, 1 img, 1 vm, 1 job, selectable rows are:
     *   0: default(train1), 1: env(train1), 2: +New env, 3: img(pytorch-cu12),
     *   4: +New img, 5: vm(vm1), 6: +New vm, 7: job(job-001), 8..10: storage/ssh/pair.
     * Navigate to job-001 (7x Down), view log (Enter), exit log (Esc), stop job (s),
     * move up 6 rows to env(train1), confirm reset (x, x), exit manager (Esc). */
    compute_exec_calls = 0;
    nfake_keys = fake_key_pos = 0;
    for (int d = 0; d < 7; d++) fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = '\n';
    fake_keys[nfake_keys++] = 27;
    fake_keys[nfake_keys++] = 's';
    for (int u = 0; u < 6; u++) fake_keys[nfake_keys++] = K_UP;
    fake_keys[nfake_keys++] = 'x';
    fake_keys[nfake_keys++] = 'x';
    fake_keys[nfake_keys++] = 27;
    manage_compute_node();
    assert(compute_exec_calls == 3 && strstr(compute_exec_last, "env reset train1"));

    /* Interactive Container Settings form (Edit mode via 'e' right on default row 0):
     * initial row on edit is 2 (GPU); toggle GPU OFF (Enter), Down to Net (3),
     * toggle Net ON to locachy-net (Enter), Down to Mem (4), Right from 16G -> 24G,
     * Down through CPU(5), Workdir(6), Add-Mount(7) to Save(8) (0 eligible storage_vols here), Enter. */
    compute_exec_calls = 0;
    compute_exec_last[0] = '\0';
    nstorage_vols = 0;
    nfake_keys = fake_key_pos = 0;
    fake_keys[nfake_keys++] = 'e';
    fake_keys[nfake_keys++] = '\n';     /* GPU: ON -> OFF */
    fake_keys[nfake_keys++] = K_DOWN;   /* Net (row 3) */
    fake_keys[nfake_keys++] = '\n';     /* Net: none -> locachy-net */
    fake_keys[nfake_keys++] = K_DOWN;   /* Memory (row 4) */
    fake_keys[nfake_keys++] = K_RIGHT;  /* 16G -> 24G */
    fake_keys[nfake_keys++] = K_DOWN;   /* CPU (5) */
    fake_keys[nfake_keys++] = K_DOWN;   /* Workdir (6) */
    fake_keys[nfake_keys++] = K_DOWN;   /* Add mount (7) */
    fake_keys[nfake_keys++] = K_DOWN;   /* Save (8) */
    fake_keys[nfake_keys++] = '\n';     /* Save! */
    fake_keys[nfake_keys++] = 27;       /* Exit manager */
    manage_compute_node();
    assert(compute_exec_calls == 1
           && strstr(compute_exec_last, "env set train1")
           && strstr(compute_exec_last, "--no-gpu")
           && strstr(compute_exec_last, "--net locachy-net")
           && strstr(compute_exec_last, "--memory 24G")
           && strstr(compute_exec_last, "--clear-volumes"));

    /* Interactive Container Settings form (Create mode via +New container at 2x Down):
     * verify defaults use --gpu and --net locachy-net (NOT --net none). */
    compute_exec_calls = 0;
    compute_exec_last[0] = '\0';
    nfake_keys = fake_key_pos = 0;
    fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = '\n';     /* Open Create form (starts at row 0) */
    for (int d = 0; d < 8; d++) fake_keys[nfake_keys++] = K_DOWN; /* Move 0 -> 8 (Save) */
    fake_keys[nfake_keys++] = '\n';     /* Save & Create */
    fake_keys[nfake_keys++] = 27;       /* Exit manager */
    manage_compute_node();
    assert(compute_exec_calls == 1
           && strstr(compute_exec_last, "env create compute2 --image pytorch-cu12 --gpu --net locachy-net"));

    /* Compute Micro-VM: export to Home ('a' on vm1 at 5x Down) and create form (6x Down). */
    compute_exec_calls = 0;
    compute_exec_last[0] = '\0';
    nfake_keys = fake_key_pos = 0;
    for (int d = 0; d < 5; d++) fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = 'a';
    fake_keys[nfake_keys++] = 27;
    manage_compute_node();
    assert(compute_exec_calls == 1
           && strstr(compute_exec_last, "vm export-app devvm --display gtk --freeze qmp --network 1"));

    compute_exec_calls = 0;
    compute_exec_last[0] = '\0';
    nfake_keys = fake_key_pos = 0;
    for (int d = 0; d < 6; d++) fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = '\n';     /* Open New Compute Micro-VM form */
    fake_keys[nfake_keys++] = K_DOWN;   /* Row 1: Base qcow2 disk */
    fake_keys[nfake_keys++] = '\n';     /* Prompt for path */
    for (const char *s = "/var/lib/locachy/vm-disks/base.qcow2"; *s; s++) fake_keys[nfake_keys++] = (unsigned char)*s;
    fake_keys[nfake_keys++] = '\n';     /* Confirm path */
    for (int d = 0; d < 6; d++) fake_keys[nfake_keys++] = K_DOWN; /* Row 1 -> Row 7 (Create) */
    fake_keys[nfake_keys++] = '\n';     /* Create! */
    fake_keys[nfake_keys++] = 27;       /* Exit manager */
    manage_compute_node();
    assert(compute_exec_calls == 1
           && strstr(compute_exec_last, "vm create vm1 --disk /var/lib/locachy/vm-disks/base.qcow2 --memory-mb 4096 --cpus 4 --net user"));

    /* Default working container switching: Left/Right on default row and 'p' in Containers section. */
    fake_compute_status =
        "meta\tservice_enabled\t1\n"
        "meta\tdaemon_running\t1\n"
        "meta\tport\t27184\n"
        "image\tubuntu-24.04\t2026-11-27\tpreset\n"
        "env\ttrain1\tubuntu-24.04\t1\tlocachy-net\t\t\t16G\t\t/root\n"
        "env\teval2\tubuntu-24.04\t0\tnone\t\t\t8G\t\t/root\n";
    compute_default_env[0] = '\0';
    nfake_keys = fake_key_pos = 0;
    fake_keys[nfake_keys++] = K_RIGHT;  /* Default: train1 -> eval2 */
    fake_keys[nfake_keys++] = 27;
    manage_compute_node();
    assert(!strcmp(compute_default_env, "eval2") && !strcmp(test_rc("compute_default_env"), "eval2"));
    nfake_keys = fake_key_pos = 0;
    fake_keys[nfake_keys++] = K_DOWN;   /* env(train1) */
    fake_keys[nfake_keys++] = 'p';      /* Pin train1 back as default */
    fake_keys[nfake_keys++] = 27;
    manage_compute_node();
    assert(!strcmp(compute_default_env, "train1") && !strcmp(test_rc("compute_default_env"), "train1"));
    compute_default_env[0] = '\0';

    /* Unmounted compute storage shows [UNMOUNTED] in settings row and still renders fallback SSH config. */
    fake_compute_status =
        "meta\tservice_enabled\t1\n"
        "meta\tdaemon_running\t0\n"
        "meta\tport\t27184\n"
        "meta\tstorage_volume\tdata\n"
        "meta\tstorage_mounted\t0\n"
        "meta\tstorage_error\tcompute storage volume 'data' is not mounted\n"
        "meta\tssh_host\t100.64.0.9\n"
        "meta\tssh_user\talice\n"
        "meta\tssh_port\t22222\n"
        "meta\tssh_prefix\tcompute-node\n";
    load_compute_status();
    item_text(compute_index() + 3, line, sizeof line);
    assert(strstr(line, "UNMOUNTED") && strstr(line, "m: mount"));
    fake_compute_exec_rc = 1;
    clip_calls = 0;
    push_keys_str("c", 27);
    compute_view_ssh_config();
    assert(clip_calls == 1 && strstr(clip_last, "Host compute-node") && strstr(clip_last, "not mounted"));
    compute_on = 0;
    compute_storage_cfg[0] = '\0';
    puts("PASS compute rows, live status, Compute Node Manager TUI, and client SSH config viewer");

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

    /* The Windows VM is a managed desktop app (start winmaint), never a terminal. */
    fake_winmaint_installed = 1;
    term_last[0] = '\0';
    int vm_calls_before = term_calls;
    fake_winmaint_app = 1;
    winmaint_vm_row();
    assert(term_calls == vm_calls_before && term_last[0] == '\0' && !strcmp(app_last, "start winmaint") &&
           strstr(notice, "Opening Maintenance Windows on the desktop"));
    fake_winmaint_app = 0;
    winmaint_vm_row();
    assert(strstr(notice, "Cannot open Maintenance Windows on the desktop"));
    char hint[256];
    row_hint_text(winmaint_index() + 4, hint, sizeof hint);
    assert(strstr(hint, "on the desktop") && !strstr(hint, "sudo") && !strstr(hint, "terminal"));
    puts("PASS Maintenance Windows size, state.json, create, two-step reclaim/boot-once, heal-boot, and VM");

    /* 6b. Start in Windows by default (boot-default) and Reboot into Windows now: in the
     * Maintenance Windows group after "Boot Windows once", routed as boot-once is (a
     * winmaint-* job); state.json gives the mode, status --json pending/overridden. */
    static const char created_state[] =
        "{\n  \"disk\": \"/dev/nvme0n1\",\n  \"partitions\": {\n    \"c\": { \"sectors\": 100663296 }\n  },\n"
        "  \"schema\": 1\n}\n";
    static const char windows_state[] =
        "{\n  \"boot_default\": \"windows\",\n  \"boot_default_boot_id\": \"b0\",\n  \"disk\": \"/dev/nvme0n1\",\n"
        "  \"partitions\": {\n    \"c\": { \"sectors\": 100663296 }\n  },\n  \"schema\": 1\n}\n";
    assert(category_of(winmaint_mode_index()) == CAT_COMPUTE && category_of(winmaint_mode_index() + 1) == CAT_COMPUTE);
    nentries = 0;
    category_entries(CAT_COMPUTE);
    int k_once = entry_of(winmaint_index() + 2), k_mode = entry_of(winmaint_mode_index());
    int k_reboot = entry_of(winmaint_mode_index() + 1), k_heal = entry_of(winmaint_index() + 3);
    assert(k_once >= 0 && k_mode == k_once + 1 && k_reboot == k_mode + 1 && k_heal == k_reboot + 1);
    build_entries();
    assert(enter_row(winmaint_mode_index()) && enter_row(winmaint_mode_index() + 1));

    write_state(created_state);
    fake_winmaint_status = "{\"boot_default\": \"locachy\", \"boot_default_overridden\": false, "
                           "\"boot_default_pending_reboot\": false, \"candidate_disks\": [], \"resolved_disk\": null}";
    winmaint_load_targets();
    item_text(winmaint_mode_index(), line, sizeof line);
    assert(strstr(line, "Start in Windows by default (until switched back)") && strstr(line, "[OFF]"));
    item_text(winmaint_mode_index() + 1, line, sizeof line);
    assert(strstr(line, "Reboot into Windows now") && strstr(line, "Enter, twice"));
    row_hint_text(winmaint_mode_index(), hint, sizeof hint);
    assert(strstr(hint, "Enter, twice") && strstr(hint, "until switched back"));
    selected = winmaint_mode_index();
    jobs_double_last[0] = '\0';
    adjust(+1); /* Left/Right only say what Enter does */
    assert(jobs_double_last[0] == '\0' && winmaint_job == 0 && strstr(notice, "Enter, twice"));

    /* Off -> on takes two Enters. */
    jobs_double_ended = 0;
    winmaint_default_row();
    assert(winmaint_default_asked == 1 && winmaint_job == 0 && jobs_double_last[0] == '\0');
    winmaint_default_row();
    assert(winmaint_default_asked == 0 && winmaint_job != 0 && !strcmp(jobs_double_last, "winmaint-boot-default windows"));
    item_text(winmaint_mode_index(), line, sizeof line);
    assert(strstr(line, "working..."));
    write_state(windows_state);
    fake_winmaint_status = "{\"boot_default\": \"windows\", \"boot_default_overridden\": false, "
                           "\"boot_default_pending_reboot\": true, \"candidate_disks\": [], \"resolved_disk\": null}";
    jobs_double_ended = 1;
    winmaint_check();
    assert(winmaint_job == 0 && winmaint_windows_first == 1 && winmaint_first_pending == 1);
    assert(strstr(notice, "Windows starts first until switched back"));
    item_text(winmaint_mode_index(), line, sizeof line);
    assert(strstr(line, "[ON] Windows from the next restart"));

    /* LOCachy picked in the firmware menu: shown, and one Enter switches back. */
    fake_winmaint_status = "{\"boot_default\": \"windows\", \"boot_default_overridden\": true, "
                           "\"boot_default_pending_reboot\": false, \"candidate_disks\": [], \"resolved_disk\": null}";
    winmaint_load_targets();
    item_text(winmaint_mode_index(), line, sizeof line);
    assert(strstr(line, "[ON] LOCachy was picked in the firmware menu") && strstr(line, "switch back"));
    row_hint_text(winmaint_mode_index(), hint, sizeof hint);
    assert(strstr(hint, "LOCachy starts first again"));
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    winmaint_default_row();
    assert(winmaint_job != 0 && !strcmp(jobs_double_last, "winmaint-boot-default locachy"));
    write_state(created_state);
    fake_winmaint_status = "{\"boot_default\": \"locachy\", \"boot_default_overridden\": false, "
                           "\"boot_default_pending_reboot\": false, \"candidate_disks\": [], \"resolved_disk\": null}";
    jobs_double_ended = 1;
    winmaint_check();
    assert(winmaint_job == 0 && winmaint_windows_first == 0 && winmaint_first_overridden == 0);
    assert(!strcmp(notice, "LOCachy starts first again."));

    /* Reboot into Windows now: two Enters, then boot-default windows --reboot. */
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    winmaint_reboot_row();
    assert(winmaint_reboot_asked == 1 && winmaint_job == 0 && jobs_double_last[0] == '\0' &&
           strstr(notice, "restarts now"));
    winmaint_reboot_row();
    assert(winmaint_reboot_asked == 0 && winmaint_job != 0 &&
           !strcmp(jobs_double_last, "winmaint-boot-default windows --reboot"));
    item_text(winmaint_mode_index() + 1, line, sizeof line);
    assert(strstr(line, "working..."));
    jobs_double_ended = 1;
    winmaint_check();
    assert(winmaint_job == 0);

    /* A pending confirmation is dropped by another row's Enter; not created refuses. */
    winmaint_default_row();
    assert(winmaint_default_asked == 1);
    winmaint_reboot_row();
    assert(winmaint_default_asked == 0 && winmaint_reboot_asked == 1);
    winmaint_heal_boot_row();
    assert(winmaint_reboot_asked == 0);
    jobs_double_ended = 1;
    winmaint_check();
    unlink(fake_winmaint_state);
    jobs_double_last[0] = '\0';
    winmaint_default_row();
    winmaint_reboot_row();
    assert(jobs_double_last[0] == '\0' && strstr(notice, "not created"));
    item_text(winmaint_mode_index(), line, sizeof line);
    assert(strstr(line, "not created"));

    /* An adopted winmaint-boot-default job shows working... on the mode rows. */
    winmaint_job = 0;
    jobs_double_mine[0] = (struct jobs_row){ .id = 7301, .kind = "winmaint-boot-default", .state = "running", .position = 0 };
    jobs_double_nmine = 1;
    adopt_jobs();
    assert(winmaint_job == 7301 && !strcmp(winmaint_what, "boot-default"));
    item_text(winmaint_mode_index(), line, sizeof line);
    assert(strstr(line, "working..."));
    winmaint_job = 0;
    winmaint_what[0] = '\0';
    jobs_double_nmine = 0;
    fake_winmaint_status = "";
    puts("PASS Maintenance Windows start mode: two-step on, one-step back, pending/overridden shown, reboot now");

    /* 6c. Inline Virtual Machines, Maintenance VM settings, and Windows Installation Media. */
    assert(category_of(vm_new_index()) == CAT_COMPUTE && category_of(winmaint_conf_index()) == CAT_COMPUTE &&
           category_of(winiso_index()) == CAT_COMPUTE && category_of(winiso_index() + 2) == CAT_COMPUTE);
    assert(enter_row(vm_new_index()) && enter_row(winmaint_conf_index()) &&
           enter_row(winiso_index()) && !enter_row(winiso_index() + 1) && enter_row(winiso_index() + 2));

    /* Create a new custom Windows VM app via vm_new_app_row(): default profile is win11 (Windows),
     * disk mode 0 (create blank 64 GiB qcow2); move from row 0 to Save (row 16) and press Enter. */
    compute_exec_calls = 0;
    compute_exec_last[0] = '\0';
    fake_compute_exec_rc = 0;
    nfake_keys = fake_key_pos = 0;
    for (int d = 0; d < 16; d++) fake_keys[nfake_keys++] = K_DOWN;
    fake_keys[nfake_keys++] = '\n';
    vm_new_app_row();
    assert(compute_exec_calls == 1 && strstr(compute_exec_last, "qemu-img create -f qcow2") &&
           strstr(compute_exec_last, "win11.qcow2 64G"));
    assert(nvm_apps == 1 && !strcmp(vm_apps[0].name, "win11") && !strcmp(vm_apps[0].vm_kind, "custom") &&
           !strcmp(vm_apps[0].vm_os, "windows") && vm_apps[0].vm_uefi == 1 && vm_apps[0].vm_tpm == 1 &&
           !strcmp(vm_apps[0].vm_disk_bus, "nvme") && !strcmp(vm_apps[0].vm_nic, "e1000e") &&
           vm_apps[0].running == 0);
    assert(category_of(vm_apps_index()) == CAT_COMPUTE && enter_row(vm_apps_index()));
    item_text(vm_apps_index(), line, sizeof line);
    assert(strstr(line, "VM: win11") && strstr(line, "stopped") && strstr(line, "windows"));

    /* Start, stop, running-marker check (.ready blocks removal), then two-press remove. */
    fake_winmaint_app = 1;
    vm_app_start_row(0);
    assert(!strcmp(app_last, "start win11"));
    vm_app_stop_row(0);
    assert(!strcmp(app_last, "stop win11"));
    char ready_file[320];
    snprintf(ready_file, sizeof ready_file, "%s/win11.ready", fake_vm_run_dir);
    FILE *rf = fopen(ready_file, "w");
    assert(rf != NULL);
    fclose(rf);
    load_vm_apps();
    assert(vm_apps[0].running == 1);
    jobs_double_last[0] = '\0';
    vm_app_remove_row(0);
    assert(strstr(notice, "Stop VM") && !jobs_double_last[0]);
    unlink(ready_file);
    load_vm_apps();
    assert(vm_apps[0].running == 0);
    vm_app_remove_row(0);
    assert(vm_apps_remove_asked == 0 && !jobs_double_last[0]);
    vm_app_remove_row(0);
    assert(vm_remove_job != 0 && !strcmp(jobs_double_last, "app-remove win11"));
    jobs_double_ended = 1;
    winiso_check();
    assert(vm_remove_job == 0);

    /* Maintenance Windows VM settings form writes winmaint.conf with ONLY closed winmaint keys,
     * and shrink_plan_armed / lock_held render live status and QMP stop. */
    nfake_keys = fake_key_pos = 0;
    fake_keys[nfake_keys++] = K_UP;     /* Edit starts at row 1 (vCPUs): go to row 0 (Memory) */
    fake_keys[nfake_keys++] = K_RIGHT;  /* 4096 -> 8192 MiB */
    for (int d = 0; d < 9; d++) fake_keys[nfake_keys++] = K_DOWN; /* Move 0 -> 9 (Save) */
    fake_keys[nfake_keys++] = '\n';
    winmaint_conf_row();
    char wm_conf[320], wm_text[1024] = "";
    snprintf(wm_conf, sizeof wm_conf, "%s/winmaint.conf", fake_vm_conf_dir);
    FILE *wf = fopen(wm_conf, "r");
    assert(wf != NULL);
    size_t wn = fread(wm_text, 1, sizeof wm_text - 1, wf);
    wm_text[wn] = '\0';
    fclose(wf);
    assert(strstr(wm_text, "vm=\"1\"") && strstr(wm_text, "vm_kind=\"winmaint\"") &&
           strstr(wm_text, "vm_memory=\"8192\"") && !strstr(wm_text, "vm_os=") &&
           !strstr(wm_text, "vm_disk=\"") && !strstr(wm_text, "vm_uefi="));
    item_text(winmaint_conf_index(), line, sizeof line);
    assert(strstr(line, "8192 MiB"));
    fake_winmaint_status = "{\"shrink_plan_armed\": true, \"lock_held\": true}";
    winmaint_load_targets();
    assert(winmaint_shrink_armed == 1 && winmaint_vm_locked == 1);
    unlink(fake_winmaint_state);
    item_text(winmaint_index() + 1, line, sizeof line);
    assert(strstr(line, "shrink plan armed"));
    write_state("{\"disk\":\"/dev/nvme0n1\",\"partitions\":{\"c\":{\"size_sectors\":100663296}}}\n");
    item_text(winmaint_index() + 4, line, sizeof line);
    assert(strstr(line, "RUNNING") && strstr(line, "QMP stop"));
    winmaint_stop_vm_row();
    assert(!strcmp(app_last, "stop winmaint"));
    unlink(fake_winmaint_state);
    fake_winmaint_status = "";
    winmaint_load_targets();
    load_winmaint_status();

    /* Windows Installation Media: language cycle, winiso-fetch submit, adopt_jobs, winiso_check. */
    fake_winiso_installed = 1;
    fake_winiso_win_path = "";
    fake_winiso_virtio_path = "";
    load_winiso_status();
    selected = winiso_index() + 1;
    assert(!strcmp(winiso_lang_sel, "en-US"));
    adjust(+1);
    assert(!strcmp(winiso_lang_sel, "zh-CN"));
    jobs_double_last[0] = '\0';
    winiso_fetch_row();
    assert(winiso_job != 0 && !strcmp(jobs_double_last, "winiso-fetch --lang zh-CN"));
    item_text(winiso_index(), line, sizeof line);
    assert(strstr(line, "downloading") && strstr(line, "zh-CN"));
    char fake_iso[320];
    snprintf(fake_iso, sizeof fake_iso, "%s/windows-11-zh-CN.iso", tmpdir);
    FILE *isf = fopen(fake_iso, "w");
    assert(isf != NULL);
    fputs("iso-placeholder", isf);
    fclose(isf);
    fake_winiso_win_path = fake_iso;
    jobs_double_ended = 1;
    winiso_check();
    assert(winiso_job == 0 && !strcmp(winiso_st.win_lang, "zh-CN"));
    item_text(winiso_index(), line, sizeof line);
    assert(strstr(line, "READY") && strstr(line, "zh-CN"));
    /* Verify adopt_jobs picks up a running winiso-fetch job after restart. */
    jobs_double_mine[0] = (struct jobs_row){ .id = 7401, .kind = "winiso-fetch", .args = "--lang ja-JP", .state = "running", .position = 0 };
    jobs_double_nmine = 1;
    adopt_jobs();
    assert(winiso_job == 7401 && !strcmp(winiso_job_lang, "ja-JP"));
    winiso_job = 0;
    winiso_job_lang[0] = '\0';
    jobs_double_nmine = 0;
    unlink(fake_iso);
    unlink(wm_conf);
    char w11_conf[320];
    snprintf(w11_conf, sizeof w11_conf, "%s/win11.conf", fake_vm_conf_dir);
    unlink(w11_conf);
    load_vm_apps();
    assert(nvm_apps == 0);
    puts("PASS Virtual Machines apps, Maintenance VM settings, and Windows Installation Media");

    /* 7. Job adoption for winmaint-*, tb-sync, gpu-unlock-*, and gpu-power*. */
    winmaint_job = tb_sync_job = gpu_unlock_job = gpu_power_job = 0;
    winmaint_what[0] = '\0';
    jobs_double_mine[0] = (struct jobs_row){ .id = 7201, .kind = "winmaint-create", .state = "running", .position = 0 };
    jobs_double_mine[1] = (struct jobs_row){ .id = 7202, .kind = "tb-sync", .state = "queued", .position = 1 };
    jobs_double_mine[2] = (struct jobs_row){ .id = 7203, .kind = "gpu-unlock-install", .state = "queued", .position = 2 };
    jobs_double_mine[3] = (struct jobs_row){ .id = 7204, .kind = "gpu-power-apply", .state = "queued", .position = 3 };
    jobs_double_mine[4] = (struct jobs_row){ .id = 7205, .kind = "gpu-driver-set", .state = "queued", .position = 4 };
    jobs_double_nmine = 5;
    adopt_jobs();
    assert(winmaint_job == 7201 && !strcmp(winmaint_what, "create") && tb_sync_job == 7202 &&
           gpu_unlock_job == 7203 && gpu_power_job == 7204 && gpu_driver_job == 7205);
    item_text(winmaint_index() + 1, line, sizeof line);
    assert(strstr(line, "working..."));
    item_text(extd_tb_shared_index(), line, sizeof line);
    assert(strstr(line, "working..."));
    item_text(gpu_driver_index(), line, sizeof line);
    assert(strstr(line, "working..."));
    item_text(gpu_unlock_index(), line, sizeof line);
    assert(strstr(line, "working..."));
    item_text(gpu_power_index(), line, sizeof line);
    assert(strstr(line, "working..."));
    gpu_unlock_job = gpu_power_job = gpu_driver_job = 0;
    puts("PASS adopt_jobs follows winmaint-*, tb-sync, gpu-driver-*, gpu-unlock-*, and gpu-power* jobs");

    /* 7b. NVIDIA DKMS driver (auto -> open -> 580xx -> off) and GSP firmware rows. */
    assert(category_of(gpu_driver_index()) == CAT_INPUT && category_of(gpu_gsp_index()) == CAT_INPUT);
    assert(picker_row(gpu_driver_index()) && enter_row(gpu_driver_index()));
    fake_nvidia_status = "configured\toff\ninstalled\toff\nrecommended\toff\ngpu_family\tnone\ndkms_version\t\ngsp_policy\tauto\ngsp_disabled\t0\n";
    gpu_driver_cfg = NVD_OFF;
    gpu_driver_choice = NVD_AUTO;
    load_gpu_driver_status();
    item_text(gpu_driver_index(), line, sizeof line);
    assert(strstr(line, "Auto (by detected GPU)") && strstr(line, "not installed"));

    /* Detected GPU family + recommendation shown while not installed; auto+rec=open marks GSP mandatory. */
    fake_nvidia_status = "configured\tauto\ninstalled\toff\nrecommended\topen\ngpu_family\tampere\ndkms_version\t\ngsp_policy\tauto\ngsp_disabled\t0\n";
    gpu_driver_cfg = NVD_AUTO;
    load_gpu_driver_status();
    assert(gpu_driver_choice == NVD_AUTO);
    item_text(gpu_driver_index(), line, sizeof line);
    assert(strstr(line, "Auto (by detected GPU)") && strstr(line, "detected ampere, rec open (Enter: apply)"));
    item_text(gpu_gsp_index(), line, sizeof line);
    assert(strstr(line, "open requires GSP"));

    /* Picker cycles auto -> open -> 580xx -> off without submitting until Enter (gpu_driver_apply_row). */
    selected = gpu_driver_index();
    jobs_double_last[0] = '\0';
    adjust(+1);
    assert(gpu_driver_choice == NVD_OPEN && gpu_driver_job == 0 && !jobs_double_last[0]);
    adjust(+1);
    assert(gpu_driver_choice == NVD_580XX && gpu_driver_job == 0);
    adjust(+1);
    assert(gpu_driver_choice == NVD_OFF && gpu_driver_job == 0);
    adjust(+1);
    assert(gpu_driver_choice == NVD_AUTO && gpu_driver_job == 0);
    adjust(+1);
    assert(gpu_driver_choice == NVD_OPEN && gpu_driver_job == 0);
    jobs_double_ended = 0;
    gpu_driver_apply_row();
    assert(gpu_driver_job != 0 && !strcmp(jobs_double_last, "gpu-driver-set open"));
    fake_nvidia_status = "configured\topen\ninstalled\topen\nrecommended\topen\ngpu_family\tampere\ndkms_version\t595.45.04-1\ngsp_policy\tauto\ngsp_disabled\t0\n";
    assert(rc_set("nvidia_driver", "open") == 0);
    jobs_double_ended = 1;
    gpu_driver_check();
    assert(gpu_driver_job == 0 && gpu_driver_installed == NVD_OPEN && !strcmp(gpu_driver_ver, "595.45.04-1"));
    item_text(gpu_driver_index(), line, sizeof line);
    assert(strstr(line, "installed open 595.45.04-1"));
    item_text(gpu_gsp_index(), line, sizeof line);
    assert(strstr(line, "open requires GSP"));

    /* Esc cancels unapplied picker change; applying 580xx updates installed state and GSP indicator. */
    selected = gpu_driver_index();
    adjust(+1);
    assert(gpu_driver_choice == NVD_580XX);
    picker_cancel();
    assert(gpu_driver_choice == NVD_OPEN);
    adjust(+1);
    assert(gpu_driver_choice == NVD_580XX);
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    gpu_driver_apply_row();
    assert(gpu_driver_job != 0 && !strcmp(jobs_double_last, "gpu-driver-set 580xx"));
    fake_nvidia_status = "configured\t580xx\ninstalled\t580xx\nrecommended\topen\ngpu_family\tampere\ndkms_version\t580.126.25-1\ngsp_policy\tauto\ngsp_disabled\t1\n";
    assert(rc_set("nvidia_driver", "580xx") == 0);
    jobs_double_ended = 1;
    gpu_driver_check();
    assert(gpu_driver_job == 0 && gpu_driver_installed == NVD_580XX && gpu_gsp_disabled == 1);
    item_text(gpu_gsp_index(), line, sizeof line);
    assert(strstr(line, "NVreg_EnableGpuFirmware=0 active"));

    /* GSP row cycles auto -> off -> on and queues gpu-driver-gsp. */
    selected = gpu_gsp_index();
    gpu_gsp_mode = NVGSP_AUTO;
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    adjust(+1);
    assert(gpu_driver_job != 0 && gpu_gsp_mode == NVGSP_OFF && !strcmp(jobs_double_last, "gpu-driver-gsp off"));
    assert(rc_set("nvidia_gsp", "off") == 0);
    jobs_double_ended = 1;
    gpu_driver_check();
    assert(gpu_driver_job == 0 && gpu_gsp_mode == NVGSP_OFF);
    puts("PASS NVIDIA DKMS driver (auto/open/580xx/off) picker and GSP firmware settings rows");

    /* 8. NVIDIA GPU unlock (one-keypress GitHub fetch/toggle/double-Delete remove)
     * and machine GPU power mode + Dynamic Boost (nvidia-powerd). */
    fake_gpu_unlock_installed = 0;
    snprintf(gpu_unlock_cfg, sizeof gpu_unlock_cfg, "off");
    item_text(gpu_unlock_index(), line, sizeof line);
    assert(strstr(line, "NVIDIA GPU unlock") && strstr(line, "not installed"));
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    gpu_unlock_action();
    assert(gpu_unlock_job != 0 && !strcmp(jobs_double_last, "gpu-unlock-install all"));
    fake_gpu_unlock_installed = 1;
    assert(rc_set("gpu_unlock_targets", "all") == 0);
    jobs_double_ended = 1;
    gpu_unlock_check();
    assert(gpu_unlock_job == 0 && !strcmp(gpu_unlock_cfg, "all"));
    item_text(gpu_unlock_index(), line, sizeof line);
    assert(strstr(line, "[ON]"));
    /* Toggle off once installed. */
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    gpu_unlock_action();
    assert(gpu_unlock_job != 0 && !strcmp(jobs_double_last, "gpu-unlock-set off"));
    assert(rc_set("gpu_unlock_targets", NULL) == 0);
    jobs_double_ended = 1;
    gpu_unlock_check();
    assert(gpu_unlock_job == 0 && !strcmp(gpu_unlock_cfg, "off"));
    item_text(gpu_unlock_index(), line, sizeof line);
    assert(strstr(line, "[OFF]"));
    /* Custom PCI BDF displays explicitly. */
    on_rc("gpu_unlock_targets", "0000:01:00.0", NULL);
    item_text(gpu_unlock_index(), line, sizeof line);
    assert(strstr(line, "ON: 0000:01:00.0"));
    /* Double-Delete confirmation before gpu-unlock-remove. */
    jobs_double_last[0] = '\0';
    gpu_unlock_remove_row();
    assert(gpu_unlock_remove_asked == 1 && gpu_unlock_job == 0 && jobs_double_last[0] == '\0');
    jobs_double_ended = 0;
    gpu_unlock_remove_row();
    assert(gpu_unlock_remove_asked == 0 && gpu_unlock_job != 0 && !strcmp(jobs_double_last, "gpu-unlock-remove"));
    fake_gpu_unlock_installed = 0;
    jobs_double_ended = 1;
    gpu_unlock_check();
    assert(gpu_unlock_job == 0);

    /* GPU power mode: disabled when machine has no method, cycles when method is present. */
    gpu_power_method[0] = '\0';
    gpu_power_mode = GPUPWR_AUTO;
    selected = gpu_power_index();
    item_text(gpu_power_index(), line, sizeof line);
    assert(strstr(line, "not on this machine"));
    jobs_double_last[0] = '\0';
    adjust(+1);
    assert(gpu_power_job == 0 && jobs_double_last[0] == '\0');

    on_rc("gpu_power_method", "hp-omen-wmi", NULL);
    on_rc("gpu_power_mode", "auto", NULL);
    item_text(gpu_power_index(), line, sizeof line);
    assert(strstr(line, "Auto (follow power profile)"));
    jobs_double_ended = 0;
    adjust(+1);
    assert(gpu_power_job != 0 && !strcmp(jobs_double_last, "gpu-power-apply full"));
    assert(rc_set("gpu_power_mode", "full") == 0);
    jobs_double_ended = 1;
    gpu_power_check();
    assert(gpu_power_job == 0 && gpu_power_mode == GPUPWR_FULL);
    item_text(gpu_power_index(), line, sizeof line);
    assert(strstr(line, "Full (cTGP + Dynamic Boost)"));

    /* nvidia-powerd toggle queues gpu-powerd on/off. */
    selected = nvidia_powerd_index();
    nvidia_powerd_on = 0;
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    adjust(+1);
    assert(gpu_power_job != 0 && !strcmp(jobs_double_last, "gpu-powerd on"));
    assert(rc_set("nvidia_powerd", "1") == 0);
    jobs_double_ended = 1;
    gpu_power_check();
    assert(gpu_power_job == 0 && nvidia_powerd_on == 1);
    item_text(nvidia_powerd_index(), line, sizeof line);
    assert(strstr(line, "[ON ]"));

    /* Changing power_profile when gpu_power_mode is auto triggers gpu-power-apply. */
    assert(rc_set("gpu_power_mode", "auto") == 0);
    gpu_power_mode = GPUPWR_AUTO;
    selected = power_profile_index();
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    adjust(+1);
    assert(gpu_power_job != 0 && !strcmp(jobs_double_last, "gpu-power-apply"));
    jobs_double_ended = 1;
    gpu_power_check();
    assert(gpu_power_job == 0);
    puts("PASS NVIDIA GPU unlock, machine GPU power mode, and nvidia-powerd settings rows");

    int graphics = gpu_graphics_index();
    assert(category_of(graphics) == CAT_INPUT && enter_row(graphics + 1) && enter_row(graphics + 2));
    selected = graphics;
    gpu_graphics_auto = 1;
    fail_rc = 1;
    adjust(1);
    assert(gpu_graphics_auto);
    fail_rc = 0;
    adjust(1);
    assert(!gpu_graphics_auto);
    item_text(graphics, line, sizeof line);
    assert(strstr(line, "leave unchanged"));

    jobs_double_last[0] = '\0';
    fake_graphics_status = "mux\tunsupported\ndgpu\tunsupported\nreboot\tunknown\n";
    gpu_graphics_action(graphics + 1);
    assert(gpu_graphics_asked == -1 && !jobs_double_last[0]);
    item_text(graphics + 1, line, sizeof line);
    assert(strstr(line, "not supported"));

    fake_graphics_status = "mux\thybrid\ndgpu\ton\nreboot\t0\n";
    jobs_double_ended = 0;
    gpu_graphics_action(graphics + 1);
    assert(gpu_graphics_asked == graphics + 1 && !jobs_double_last[0]);
    gpu_graphics_action(graphics + 1);
    assert(gpu_graphics_job && !strcmp(jobs_double_last, "gpu-graphics-mux discrete"));
    fake_graphics_status = "mux\tdiscrete\ndgpu\ton\nreboot\t1\n";
    jobs_double_ended = 1;
    gpu_graphics_check();
    item_text(graphics + 1, line, sizeof line);
    assert(!gpu_graphics_job && strstr(line, "direct NVIDIA") && strstr(line, "pending restart"));
    jobs_double_last[0] = '\0';
    gpu_graphics_action(graphics + 2);
    assert(gpu_graphics_asked == -1 && !jobs_double_last[0]);

    fake_graphics_status = "mux\thybrid\ndgpu\ton\nreboot\t0\n";
    gpu_graphics_action(graphics + 2);
    assert(gpu_graphics_asked == graphics + 2 && !jobs_double_last[0] && strstr(notice, "CUDA"));
    gpu_graphics_action(graphics + 2);
    assert(gpu_graphics_job && !strcmp(jobs_double_last, "gpu-graphics-dgpu off"));
    puts("PASS hybrid policy and separate firmware MUX/dGPU choices with confirmation and reboot state");

    unlink(fake_winmaint_state);
    rmdir(fake_vm_conf_dir);
    rmdir(fake_vm_disks_dir);
    rmdir(fake_vm_run_dir);
    rmdir(tmpdir);
    puts("PASS test-settings-features: all checks passed.");
    return 0;
}
