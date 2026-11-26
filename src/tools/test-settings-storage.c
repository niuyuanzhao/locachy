/* Isolated unit tests for Settings > System > Storage Volumes (settings.c).
 * Includes ../progs/settings.c with doubles, following test-settings-features.c.
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

#define WINMAINT_STATE_FILE "/nonexistent-winmaint-state/state.json"
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

int base_app_action(const char *action, const char *name, char *why, size_t size)
{
    (void)action;
    (void)name;
    snprintf(why, size, "%s", "no desktop session in tests");
    return -1;
}

static int reloads;
static char rc_name[32][64], rc_value[32][256];
static int rc_count;
static int fake_storage_installed, fake_winmaint_installed;
static const char *fake_winmaint_json = "";
static const char *fake_storage_json = "";
static int term_calls, term_status;
static char term_last[512];

void key_close_kept(void) {}
int key_get(void) { abort(); }
void say(const char *fmt, ...) { (void)fmt; }

static void command_output(char *const argv[], char *buf, size_t size)
{
    buf[0] = '\0';
    if (argv[0] && strcmp(argv[0], LOCACHY_STORAGE) == 0 && argv[1] && strcmp(argv[1], "status") == 0 &&
        argv[2] && strcmp(argv[2], "--json") == 0 && argv[3] == NULL) {
        snprintf(buf, size, "%s", fake_storage_json);
    } else if (argv[0] && strcmp(argv[0], LOCACHY_WINMAINT) == 0 && argv[1] && strcmp(argv[1], "status") == 0 &&
               argv[2] && strcmp(argv[2], "--json") == 0 && argv[3] == NULL) {
        snprintf(buf, size, "%s", fake_winmaint_json);
    }
}

int rc_set(const char *name, const char *value)
{
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

int settings_access(const char *path, int mode)
{
    if (strcmp(path, LOCACHY_STORAGE) == 0) {
        if (fake_storage_installed) return 0;
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

static void assert_unique_category_coverage(void)
{
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
}

int main(void)
{
    char line[512], hint[256];

    /* 1. Absent locachy-storage: zero volumes, clean index chain, safe refusal. */
    fake_storage_installed = 0;
    load_storage_status();
    assert(nstorage_vols == 0 && nstorage_disks == 0);
    assert_unique_category_coverage();
    assert(category_of(storage_compute_index()) == CAT_SYSTEM);
    assert(category_of(storage_disks_index()) == CAT_SYSTEM);
    assert(enter_row(storage_disks_index()));
    item_text(storage_compute_index(), line, sizeof line);
    assert(strstr(line, "Compute default storage") && strstr(line, "< none >"));
    item_text(storage_disks_index(), line, sizeof line);
    assert(strstr(line, "Unmanaged disks") && strstr(line, "not installed"));
    storage_manage_disks_row();
    assert(term_calls == 0 && strstr(notice, "locachy-storage is not installed"));
    puts("PASS absent locachy-storage keeps index chain intact and reports not installed");

    /* 2. Malformed JSON fails safely without crashing or leaving partial garbage. */
    fake_storage_installed = 1;
    fake_storage_json = "{ \"volumes\": [ { \"name\": \"broken\" ";
    load_storage_status();
    assert(nstorage_vols == 0 && nstorage_disks == 0);
    puts("PASS malformed status JSON fails safely");

    /* 3. Full JSON parsing matching locachy-storage status --json real schema
     * (nested usage, mountpoint, health="healthy", btrfs-array, in_use filtering). */
    fake_storage_json =
        "{\n"
        "  \"schema\": 1,\n"
        "  \"root_encrypted\": true,\n"
        "  \"compute_storage\": \"data\",\n"
        "  \"storage_root\": \"/srv/storage\",\n"
        "  \"volumes\": [\n"
        "    {\n"
        "      \"name\": \"data\",\n"
        "      \"topology\": \"single\",\n"
        "      \"fs\": \"ext4\",\n"
        "      \"raid\": \"\",\n"
        "      \"role\": \"shared\",\n"
        "      \"encrypt\": \"luks\",\n"
        "      \"auto_mount\": true,\n"
        "      \"mountpoint\": \"/srv/storage/data\",\n"
        "      \"mounted\": true,\n"
        "      \"drives_path\": \"/home/user/Drives/data\",\n"
        "      \"drives_projected\": true,\n"
        "      \"health\": \"healthy\",\n"
        "      \"degraded\": false,\n"
        "      \"error\": \"\",\n"
        "      \"usage\": {\"total_bytes\": 2199023255552, \"used_bytes\": 128849018880, \"avail_bytes\": 2070174236672},\n"
        "      \"btrfs_stats\": null,\n"
        "      \"keyfile_on_encrypted_root\": true\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"scratch\",\n"
        "      \"topology\": \"btrfs-array\",\n"
        "      \"fs\": \"btrfs\",\n"
        "      \"raid\": \"raid1\",\n"
        "      \"role\": \"compute\",\n"
        "      \"encrypt\": \"none\",\n"
        "      \"auto_mount\": false,\n"
        "      \"mountpoint\": \"/srv/storage/scratch\",\n"
        "      \"mounted\": false,\n"
        "      \"drives_path\": \"\",\n"
        "      \"health\": \"degraded\",\n"
        "      \"usage\": {\"total_bytes\": 0, \"used_bytes\": 0, \"avail_bytes\": 0}\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"media\",\n"
        "      \"topology\": \"single\",\n"
        "      \"fs\": \"ext4\",\n"
        "      \"raid\": \"\",\n"
        "      \"role\": \"data\",\n"
        "      \"encrypt\": \"none\",\n"
        "      \"auto_mount\": true,\n"
        "      \"mountpoint\": \"/srv/storage/media\",\n"
        "      \"mounted\": true,\n"
        "      \"drives_path\": \"/home/user/Drives/media\",\n"
        "      \"total_bytes\": 1099511627776,\n"
        "      \"used_bytes\": 549755813888,\n"
        "      \"health\": \"ok\"\n"
        "    }\n"
        "  ],\n"
        "  \"available_disks\": [\n"
        "    {\n"
        "      \"path\": \"/dev/nvme1n1\",\n"
        "      \"model\": \"KIOXIA-EXCERIA G2 SSD\",\n"
        "      \"size\": 2000398934016,\n"
        "      \"tran\": \"nvme\",\n"
        "      \"in_use\": false,\n"
        "      \"available_for_format\": true,\n"
        "      \"partitions\": [\n"
        "        {\"path\": \"/dev/nvme1n1p1\", \"size\": 2000397885440, \"fstype\": \"ntfs\", \"label\": \"\", \"attachable\": true}\n"
        "      ]\n"
        "    },\n"
        "    {\n"
        "      \"path\": \"/dev/nvme2n1\",\n"
        "      \"model\": \"Managed Disk\",\n"
        "      \"size\": 1000204886016,\n"
        "      \"tran\": \"nvme\",\n"
        "      \"in_use\": true,\n"
        "      \"available_for_format\": false,\n"
        "      \"partitions\": [\n"
        "        {\"path\": \"/dev/nvme2n1p1\", \"size\": 1000203091968, \"fstype\": \"crypto_LUKS\", \"label\": \"\", \"attachable\": false}\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n";
    load_storage_status();
    assert(nstorage_vols == 3 && nstorage_disks == 1);
    assert(strcmp(compute_storage_cfg, "data") == 0);
    assert(strcmp(storage_vols[0].name, "data") == 0 && storage_vols[0].mounted == 1 && storage_vols[0].auto_mount == 1);
    assert(strcmp(storage_vols[0].mount_path, "/srv/storage/data") == 0);
    assert(strcmp(storage_vols[1].raid, "raid1") == 0 && strcmp(storage_vols[1].health, "degraded") == 0);
    assert(strcmp(storage_disks[0].path, "/dev/nvme1n1") == 0 && storage_disks[0].nparts == 1 &&
           strcmp(storage_disks[0].first_fstype, "ntfs") == 0);
    assert_unique_category_coverage();
    assert(category_of(storage_vols_index()) == CAT_SYSTEM);
    assert(category_of(storage_vols_index() + 2) == CAT_SYSTEM);
    assert(category_of(storage_disks_index()) == CAT_SYSTEM);
    assert(storage_disks_index() == storage_vols_index() + 3);

    /* Compute Node category shows default compute storage summary note. */
    nentries = 0;
    category_entries(CAT_NETWORK);
    int found_note = 0;
    for (int k = 0; k < nentries; k++) {
        if (entries[k].idx == E_NOTE && strstr(entries[k].text, "Default compute storage: data")) found_note = 1;
    }
    assert(found_note);

    item_text(storage_compute_index(), line, sizeof line);
    assert(strstr(line, "Compute default storage") && strstr(line, "< data >"));
    item_text(storage_vols_index(), line, sizeof line);
    assert(strstr(line, "Volume: data") && strstr(line, "< shared >") && strstr(line, "ext4") &&
           strstr(line, "LUKS") && strstr(line, "auto") && strstr(line, "120 GiB / 2.0 TiB") &&
           !strstr(line, "[!"));
    item_text(storage_vols_index() + 1, line, sizeof line);
    assert(strstr(line, "Volume: scratch") && strstr(line, "< compute >") && strstr(line, "btrfs raid1") &&
           strstr(line, "plain") && strstr(line, "manual") && strstr(line, "unmounted") &&
           strstr(line, "[!degraded]"));
    item_text(storage_disks_index(), line, sizeof line);
    assert(strstr(line, "1 unmanaged disk: /dev/nvme1n1 (1.8 TiB)"));
    puts("PASS status JSON parsing, dynamic row indexing, Compute Node note, and row formatting");

    /* 4. Cycle Compute default storage: skips data-only volume "media" (only none, data, scratch). */
    selected = storage_compute_index();
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    adjust(+1);
    assert(storage_job != 0 && strcmp(jobs_double_last, "storage-set-compute scratch") == 0);
    assert(rc_set("compute_storage", "scratch") == 0);
    fake_storage_json =
        "{\"compute_storage\":\"scratch\",\"volumes\":["
        "{\"name\":\"data\",\"fs\":\"ext4\",\"role\":\"shared\",\"encrypt\":\"luks\",\"auto_mount\":true,\"mounted\":true,\"total_bytes\":2199023255552,\"used_bytes\":128849018880,\"health\":\"ok\"},"
        "{\"name\":\"scratch\",\"fs\":\"btrfs\",\"raid\":\"raid1\",\"role\":\"compute\",\"encrypt\":\"none\",\"auto_mount\":false,\"mounted\":false,\"health\":\"degraded\"},"
        "{\"name\":\"media\",\"fs\":\"ext4\",\"role\":\"data\",\"encrypt\":\"none\",\"auto_mount\":true,\"mounted\":true,\"health\":\"ok\"}"
        "],\"available_disks\":[]}";
    jobs_double_ended = 1;
    storage_check();
    assert(storage_job == 0 && strcmp(compute_storage_cfg, "scratch") == 0);

    /* Next +1 wraps around from scratch to none (never picks media whose role is data). */
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    adjust(+1);
    assert(storage_job != 0 && strcmp(jobs_double_last, "storage-set-compute none") == 0);
    assert(rc_set("compute_storage", NULL) == 0);
    fake_storage_json =
        "{\"compute_storage\":\"\",\"volumes\":["
        "{\"name\":\"data\",\"fs\":\"ext4\",\"role\":\"shared\",\"encrypt\":\"luks\",\"auto_mount\":true,\"mounted\":true,\"total_bytes\":2199023255552,\"used_bytes\":128849018880,\"health\":\"ok\"},"
        "{\"name\":\"scratch\",\"fs\":\"btrfs\",\"raid\":\"raid1\",\"role\":\"compute\",\"encrypt\":\"none\",\"auto_mount\":false,\"mounted\":false,\"health\":\"degraded\"}"
        "],\"available_disks\":[]}";
    jobs_double_ended = 1;
    storage_check();
    assert(storage_job == 0 && compute_storage_cfg[0] == '\0' && nstorage_vols == 2);
    puts("PASS compute default storage cycles eligible shared/compute volumes and none");

    /* 5. Role cycling on volume row (shared -> data -> compute). */
    selected = storage_vols_index();
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    adjust(+1);
    assert(storage_job != 0 && strcmp(jobs_double_last, "storage-set-role data data") == 0);
    jobs_double_ended = 1;
    storage_check();
    assert(storage_job == 0);

    selected = storage_vols_index();
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    adjust(-1);
    assert(storage_job != 0 && strcmp(jobs_double_last, "storage-set-role data compute") == 0);
    jobs_double_ended = 1;
    storage_check();
    assert(storage_job == 0);
    puts("PASS volume role cycling submits storage-set-role jobs");

    /* 6. Mount / unmount toggle and auto-mount toggle. */
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    storage_toggle_mount(0); /* data is mounted -> umount */
    assert(storage_job != 0 && strcmp(jobs_double_last, "storage-umount data") == 0);
    jobs_double_ended = 1;
    storage_check();
    assert(storage_job == 0 && strstr(notice, "unmounted"));

    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    storage_toggle_mount(1); /* scratch is unmounted -> mount */
    assert(storage_job != 0 && strcmp(jobs_double_last, "storage-mount scratch") == 0);
    jobs_double_ended = 1;
    storage_check();
    assert(storage_job == 0 && strstr(notice, "mounted"));

    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    storage_toggle_auto(0); /* auto_mount=true -> off */
    assert(storage_job != 0 && strcmp(jobs_double_last, "storage-set-auto data off") == 0);
    jobs_double_ended = 1;
    storage_check();
    assert(storage_job == 0 && strstr(notice, "auto-mount updated"));

    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    storage_toggle_auto(1); /* auto_mount=false -> on */
    assert(storage_job != 0 && strcmp(jobs_double_last, "storage-set-auto scratch on") == 0);
    jobs_double_ended = 1;
    storage_check();
    assert(storage_job == 0);
    puts("PASS mount/unmount and auto-mount toggles submit exact jobs");

    /* 7. Two-press Delete confirmation before storage-remove (safe detach). */
    jobs_double_last[0] = '\0';
    storage_remove_row(0);
    assert(storage_remove_asked == 0 && storage_job == 0 && jobs_double_last[0] == '\0');
    assert(strstr(notice, "Delete again") && strstr(notice, "NOT wiped"));
    /* Pressing Delete on a different volume resets confirmation rather than firing. */
    storage_remove_row(1);
    assert(storage_remove_asked == 1 && storage_job == 0 && jobs_double_last[0] == '\0');
    /* Second Delete on the same volume submits storage-remove. */
    jobs_double_ended = 0;
    storage_remove_row(1);
    assert(storage_remove_asked == -1 && storage_job != 0 && strcmp(jobs_double_last, "storage-remove scratch") == 0);
    jobs_double_ended = 1;
    storage_check();
    assert(storage_job == 0 && strstr(notice, "detached"));
    puts("PASS double-Delete confirmation safely detaches volume via storage-remove");

    /* 8. Unmanaged disks row launches terminal with sudo locachy-storage status,
     * and row hints describe all volume keys clearly. */
    row_hint_text(storage_disks_index(), hint, sizeof hint);
    assert(strstr(hint, "attach, format, or create btrfs array"));
    selected = storage_vols_index();
    row_hint();
    assert(strstr(notice, "m: mount/unmount") && strstr(notice, "a: auto-mount") && strstr(notice, "Delete, twice: detach"));
    term_last[0] = '\0';
    storage_manage_disks_row();
    assert(term_calls == 1 && strstr(term_last, "/usr/bin/sudo " LOCACHY_STORAGE " status"));
    puts("PASS unmanaged disks terminal launch and volume key hints");

    /* 9. adopt_jobs follows storage-* jobs. */
    storage_job = 0;
    storage_what[0] = '\0';
    jobs_double_mine[0] = (struct jobs_row){ .id = 7301, .kind = "storage-mount", .state = "running", .position = 0 };
    jobs_double_nmine = 1;
    adopt_jobs();
    assert(storage_job == 7301 && strcmp(storage_what, "mount") == 0);
    item_text(storage_vols_index(), line, sizeof line);
    assert(strstr(line, "working..."));
    storage_job = 0;
    puts("PASS adopt_jobs follows storage-* jobs");

    /* 10. Maintenance Windows target disk: locachy-winmaint status --json offers
     * candidate_disks (the system disk among them, is_root) and resolved_disk.
     * The row cycles the system disk and the other offered disks; Enter creates
     * with --disk for the chosen one and without it for the system disk. */
    fake_winmaint_installed = 1;
    fake_winmaint_json =
        "{\"configured\": false, \"shrink_plan_armed\": false, \"lock_held\": false,"
        " \"last_shrink\": {}, \"state\": null, \"resolved_disk\": null, \"on_root_disk\": false,"
        " \"candidate_disks\": ["
        " {\"path\": \"/dev/nvme0n1\", \"model\": \"System SSD\", \"size_bytes\": 512110190592, \"is_root\": true,"
        "  \"free_tail_gib\": 1.5, \"disk_guid\": \"aaaa\", \"has_maintenance_windows\": false},"
        " {\"path\": \"/dev/nvme1n1\", \"model\": \"KIOXIA-EXCERIA G2 SSD\", \"size_bytes\": 2000398934016,"
        "  \"is_root\": false, \"free_tail_gib\": 1800.0, \"disk_guid\": \"bbbb\", \"has_maintenance_windows\": false}"
        " ]}";
    winmaint_load_targets();
    assert(nwinmaint_cands == 2 && !strcmp(winmaint_cands[0].path, "/dev/nvme0n1") && winmaint_cands[0].is_root &&
           !strcmp(winmaint_cands[1].path, "/dev/nvme1n1") && !winmaint_cands[1].is_root &&
           winmaint_cands[1].size == 2000398934016ULL && !strcmp(winmaint_cands[1].model, "KIOXIA-EXCERIA G2 SSD") &&
           winmaint_resolved[0] == '\0');
    assert(link_index() == winmaint_index() + 6 && sec_index() == link_index() + 1);
    winmaint_created = 0;
    winmaint_disk_cfg[0] = '\0';
    selected = winmaint_index() + 5;
    item_text(winmaint_index() + 5, line, sizeof line);
    assert(strstr(line, "Windows target disk") && strstr(line, "< System disk (auto) >"));
    adjust(+1);
    assert(!strcmp(winmaint_disk_cfg, "/dev/nvme1n1") && strstr(notice, "goes on /dev/nvme1n1"));
    item_text(winmaint_index() + 5, line, sizeof line);
    assert(strstr(line, "/dev/nvme1n1 (KIOXIA-EXCERIA G2 SSD, 1.8 TiB)"));
    adjust(+1);
    assert(winmaint_disk_cfg[0] == '\0' && strstr(notice, "System disk (auto)"));
    adjust(-1);
    assert(!strcmp(winmaint_disk_cfg, "/dev/nvme1n1"));

    selected = winmaint_index() + 1;
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    winmaint_status_row();
    assert(winmaint_job != 0 && !strcmp(jobs_double_last, "winmaint-create --size-gib 48 --disk /dev/nvme1n1"));
    jobs_double_ended = 1;
    winmaint_check();
    assert(winmaint_job == 0);

    winmaint_disk_cfg[0] = '\0';
    jobs_double_ended = 0;
    jobs_double_last[0] = '\0';
    winmaint_status_row();
    assert(winmaint_job != 0 && !strcmp(jobs_double_last, "winmaint-create --size-gib 48"));
    jobs_double_ended = 1;
    winmaint_check();
    assert(winmaint_job == 0);

    /* Once created, the row names the disk the partition is on, and it cannot change. */
    fake_winmaint_json = "{\"resolved_disk\": \"/dev/nvme1n1\", \"candidate_disks\": []}";
    winmaint_load_targets();
    assert(nwinmaint_cands == 0 && !strcmp(winmaint_resolved, "/dev/nvme1n1"));
    winmaint_created = 1;
    selected = winmaint_index() + 5;
    item_text(winmaint_index() + 5, line, sizeof line);
    assert(strstr(line, "< /dev/nvme1n1 >"));
    adjust(+1);
    assert(strstr(notice, "fixed once Maintenance Windows exists"));
    winmaint_created = 0;

    /* An unreadable answer offers the system disk only, and nothing is chosen from it. */
    fake_winmaint_json = "{\"candidate_disks\": [ {\"path\": ";
    winmaint_load_targets();
    assert(nwinmaint_cands == 0 && winmaint_resolved[0] == '\0');
    fake_winmaint_installed = 0;
    winmaint_load_targets();
    assert(nwinmaint_cands == 0);
    winmaint_disk_cfg[0] = '\0';
    puts("PASS Maintenance Windows target disk: candidates from locachy-winmaint status, the row cycles them, "
         "create passes --disk for the chosen one only, the disk is fixed once created");
    puts("PASS test-settings-storage: all checks passed.");
    return 0;
}
