/* Virtual Machine Apps, Maintenance Windows VM Settings, and Windows Installation Media
 * helpers for settings.c (inline sections under Settings > Compute & VMs — zero extra tabs).
 * Included by settings.c after settings-compute.inc. */

#ifndef LOCACHY_WINISO
#define LOCACHY_WINISO "/usr/local/bin/locachy-winiso"
#endif
#ifndef VM_CONF_DIR
#define VM_CONF_DIR CONTAINERS_CONF
#endif
#ifndef VM_DISKS_DIR
#define VM_DISKS_DIR "/var/lib/locachy/vm-disks"
#endif
#ifndef VM_APPS_RUN_DIR
#define VM_APPS_RUN_DIR "/run/locachy/apps"
#endif

#define MAX_VM_APPS 48

struct vm_app_cfg {
    char name[64];
    int is_vm;
    char vm_kind[16];     /* custom | compute | winmaint */
    char vm_name[64];     /* compute only */
    char vm_os[16];       /* linux | windows (custom only) */
    char vm_base[256];    /* read-only base disk (custom only) */
    char vm_disk[256];    /* writable disk (custom only) */
    char vm_iso[256];     /* install ISO (custom / winmaint) */
    char vm_virtio[256];  /* virtio-win ISO (custom / winmaint) */
    char vm_display[16];  /* gtk | sdl | none */
    char vm_freeze[16];   /* qmp | none */
    char vm_disk_bus[16]; /* virtio | nvme | sata */
    char vm_nic[16];      /* virtio | e1000e */
    char vm_gpu[16];      /* 2d | gl */
    int vm_memory;        /* MiB */
    int vm_cpus;          /* vCPU count */
    int vm_uefi;          /* -1 = auto, 0, 1 */
    int vm_tpm;           /* -1 = auto, 0, 1 */
    int vm_share_home;    /* 0, 1 */
    int vm_audio;         /* 0, 1 */
    int network;          /* 0, 1 */
    int running;          /* /run/locachy/apps/NAME.ready present */
};

static struct vm_app_cfg vm_apps[MAX_VM_APPS];
static int vm_apps_remove_asked = -1;

struct winiso_state {
    char win_path[256];
    char win_lang[24];
    unsigned long long win_size;
    char virtio_path[256];
    unsigned long long virtio_size;
};

static struct winiso_state winiso_st;
static int winiso_job;
static char winiso_job_lang[24];
static int vm_remove_job;
static char vm_remove_name[64];

static const char *const winiso_langs[] = {
    "en-US", "zh-CN", "zh-TW", "ja-JP", "de-DE", "fr-FR", "es-ES", "ko-KR", "pt-BR",
};
#define NWINISO_LANGS ((int)(sizeof winiso_langs / sizeof winiso_langs[0]))
static char winiso_lang_sel[24] = "en-US";

static const int vm_disk_sizes_gib[] = { 20, 32, 48, 64, 96, 128, 256 };
#define NVM_DISK_SIZES ((int)(sizeof vm_disk_sizes_gib / sizeof vm_disk_sizes_gib[0]))

static void vm_copy(char *dst, size_t size, const char *src)
{
    if (!size) return;
    snprintf(dst, size, "%.*s", (int)(size - 1), src ? src : "");
}

static int vm_valid_tag(const char *s)
{
    size_t n = strlen(s);
    if (n < 5 || n > 10) return 0;
    const char *dash = strchr(s, '-');
    if (!dash) return 0;
    size_t a = (size_t)(dash - s), b = strlen(dash + 1);
    if (a < 2 || a > 3 || b < 2 || b > 4) return 0;
    for (size_t i = 0; i < a; i++)
        if (s[i] < 'a' || s[i] > 'z') return 0;
    for (size_t i = 0; i < b; i++)
        if (!isalnum((unsigned char)dash[1 + i])) return 0;
    return 1;
}

static int vm_path_safe(const char *p)
{
    if (!p || p[0] != '/') return 0;
    if (strpbrk(p, "\"'\n\r`$") != NULL) return 0;
    if (strstr(p, "/../") != NULL || strcmp(p, "/..") == 0) return 0;
    size_t n = strlen(p);
    if (n >= 3 && strcmp(p + n - 3, "/..") == 0) return 0;
    return 1;
}

static void vm_cfg_defaults(struct vm_app_cfg *c, const char *kind, const char *os_name)
{
    memset(c, 0, sizeof *c);
    c->is_vm = 1;
    vm_copy(c->vm_kind, sizeof c->vm_kind, (kind && kind[0]) ? kind : "custom");
    vm_copy(c->vm_os, sizeof c->vm_os, (os_name && os_name[0]) ? os_name : "windows");
    vm_copy(c->vm_display, sizeof c->vm_display, "gtk");
    vm_copy(c->vm_freeze, sizeof c->vm_freeze, "qmp");
    c->vm_memory = 4096;
    c->vm_cpus = 4;
    c->network = 1;
    if (strcmp(c->vm_kind, "winmaint") == 0) {
        vm_copy(c->name, sizeof c->name, WINMAINT_APP);
        vm_copy(c->vm_os, sizeof c->vm_os, "windows");
        vm_copy(c->vm_disk_bus, sizeof c->vm_disk_bus, "nvme");
        vm_copy(c->vm_nic, sizeof c->vm_nic, "e1000e");
        c->vm_uefi = 1;
        c->vm_tpm = 1;
    } else if (strcmp(c->vm_os, "windows") == 0) {
        vm_copy(c->vm_disk_bus, sizeof c->vm_disk_bus, "nvme");
        vm_copy(c->vm_nic, sizeof c->vm_nic, "e1000e");
        vm_copy(c->vm_gpu, sizeof c->vm_gpu, "2d");
        c->vm_uefi = 1;
        c->vm_tpm = 1;
    } else {
        vm_copy(c->vm_disk_bus, sizeof c->vm_disk_bus, "virtio");
        vm_copy(c->vm_nic, sizeof c->vm_nic, "virtio");
        vm_copy(c->vm_gpu, sizeof c->vm_gpu, "2d");
        c->vm_uefi = 0;
        c->vm_tpm = 0;
    }
}

static void on_vm_conf_kv(const char *k, const char *v, void *arg)
{
    struct vm_app_cfg *c = arg;
    if (strcmp(k, "vm") == 0) c->is_vm = strcmp(v, "1") == 0;
    else if (strcmp(k, "vm_kind") == 0) vm_copy(c->vm_kind, sizeof c->vm_kind, v);
    else if (strcmp(k, "vm_name") == 0) vm_copy(c->vm_name, sizeof c->vm_name, v);
    else if (strcmp(k, "vm_os") == 0) vm_copy(c->vm_os, sizeof c->vm_os, v);
    else if (strcmp(k, "vm_base") == 0) vm_copy(c->vm_base, sizeof c->vm_base, v);
    else if (strcmp(k, "vm_disk") == 0) vm_copy(c->vm_disk, sizeof c->vm_disk, v);
    else if (strcmp(k, "vm_iso") == 0) vm_copy(c->vm_iso, sizeof c->vm_iso, v);
    else if (strcmp(k, "vm_virtio_iso") == 0) vm_copy(c->vm_virtio, sizeof c->vm_virtio, v);
    else if (strcmp(k, "vm_display") == 0) vm_copy(c->vm_display, sizeof c->vm_display, v);
    else if (strcmp(k, "vm_freeze") == 0) vm_copy(c->vm_freeze, sizeof c->vm_freeze, v);
    else if (strcmp(k, "vm_disk_bus") == 0) vm_copy(c->vm_disk_bus, sizeof c->vm_disk_bus, v);
    else if (strcmp(k, "vm_nic") == 0) vm_copy(c->vm_nic, sizeof c->vm_nic, v);
    else if (strcmp(k, "vm_gpu") == 0) vm_copy(c->vm_gpu, sizeof c->vm_gpu, v);
    else if (strcmp(k, "vm_memory") == 0) c->vm_memory = atoi(v);
    else if (strcmp(k, "vm_cpus") == 0) c->vm_cpus = atoi(v);
    else if (strcmp(k, "vm_uefi") == 0) c->vm_uefi = strcmp(v, "1") == 0;
    else if (strcmp(k, "vm_tpm") == 0) c->vm_tpm = strcmp(v, "1") == 0;
    else if (strcmp(k, "vm_share_home") == 0) c->vm_share_home = strcmp(v, "1") == 0;
    else if (strcmp(k, "vm_audio") == 0) c->vm_audio = strcmp(v, "1") == 0;
    else if (strcmp(k, "network") == 0) c->network = strcmp(v, "1") == 0;
}

static void vm_read_conf_file(const char *path, struct vm_app_cfg *c)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[512];
    while (fgets(buf, sizeof buf, f)) {
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#' || *p == '\n' || *p == '\r') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = p;
        char *ke = eq;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        size_t vl = strlen(v);
        while (vl > 0 && (v[vl - 1] == '\n' || v[vl - 1] == '\r' || v[vl - 1] == ' ' || v[vl - 1] == '\t'))
            v[--vl] = '\0';
        if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '\'' && v[vl - 1] == '\''))) {
            v[vl - 1] = '\0';
            v++;
        }
        on_vm_conf_kv(k, v, c);
    }
    fclose(f);
}

static int vm_cmp_name(const void *a, const void *b)
{
    return strcmp(((const struct vm_app_cfg *)a)->name, ((const struct vm_app_cfg *)b)->name);
}

static void load_vm_apps(void)
{
    int prev = nvm_apps;
    nvm_apps = nvm_apps_running = 0;
    DIR *d = opendir(VM_CONF_DIR);
    if (!d) {
        if (prev != nvm_apps) row_maps_dirty = entries_dirty = 1;
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nvm_apps < MAX_VM_APPS) {
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 >= sizeof vm_apps[0].name || strcmp(e->d_name + len - 5, ".conf") != 0)
            continue;
        struct vm_app_cfg c;
        memset(&c, 0, sizeof c);
        c.vm_uefi = -1;
        c.vm_tpm = -1;
        snprintf(c.name, sizeof c.name, "%.*s", (int)(len - 5), e->d_name);
        char path[600];
        snprintf(path, sizeof path, "%.320s/%s", VM_CONF_DIR, e->d_name);
        vm_read_conf_file(path, &c);
        if (!c.is_vm) continue;
        if (!c.vm_kind[0]) vm_copy(c.vm_kind, sizeof c.vm_kind, "custom");
        if (strcmp(c.vm_kind, "winmaint") == 0) continue; /* Maintenance Windows: its own settings row */
        if (!c.vm_os[0]) vm_copy(c.vm_os, sizeof c.vm_os,
                                 strcmp(c.vm_kind, "winmaint") == 0 ? "windows" : "linux");
        if (!c.vm_display[0]) vm_copy(c.vm_display, sizeof c.vm_display, "gtk");
        if (!c.vm_freeze[0]) vm_copy(c.vm_freeze, sizeof c.vm_freeze, "qmp");
        if (c.vm_memory <= 0) c.vm_memory = 4096;
        if (c.vm_cpus <= 0) c.vm_cpus = 4;
        if (!c.vm_disk_bus[0]) vm_copy(c.vm_disk_bus, sizeof c.vm_disk_bus,
                                       strcmp(c.vm_os, "windows") == 0 ? "nvme" : "virtio");
        if (!c.vm_nic[0]) vm_copy(c.vm_nic, sizeof c.vm_nic,
                                  strcmp(c.vm_os, "windows") == 0 ? "e1000e" : "virtio");
        if (!c.vm_gpu[0]) vm_copy(c.vm_gpu, sizeof c.vm_gpu, "2d");
        if (c.vm_uefi < 0) c.vm_uefi = strcmp(c.vm_os, "windows") == 0 ? 1 : 0;
        if (c.vm_tpm < 0) c.vm_tpm = strcmp(c.vm_os, "windows") == 0 ? 1 : 0;
        char rpath[512];
        snprintf(rpath, sizeof rpath, "%.320s/%.63s.ready", VM_APPS_RUN_DIR, c.name);
        struct stat st;
        c.running = (stat(rpath, &st) == 0);
        if (c.running) nvm_apps_running++;
        vm_apps[nvm_apps++] = c;
    }
    closedir(d);
    if (nvm_apps > 1) qsort(vm_apps, (size_t)nvm_apps, sizeof vm_apps[0], vm_cmp_name);
    if (prev != nvm_apps) row_maps_dirty = entries_dirty = 1;
}

static void load_winiso_status(void)
{
    memset(&winiso_st, 0, sizeof winiso_st);
    if (access(LOCACHY_WINISO, X_OK) != 0) return;
    char out[512];
    struct stat st;

    out[0] = '\0';
    char *const wargv[] = { LOCACHY_WINISO, "path", "windows", NULL };
    command_output(wargv, out, sizeof out);
    char *nl = strpbrk(out, "\r\n");
    if (nl) *nl = '\0';
    if (out[0] == '/') {
        vm_copy(winiso_st.win_path, sizeof winiso_st.win_path, out);
        if (stat(out, &st) == 0) winiso_st.win_size = (unsigned long long)st.st_size;
        const char *base = strrchr(out, '/');
        base = base ? base + 1 : out;
        if (strncmp(base, "windows-11-", 11) == 0) {
            const char *dot = strstr(base + 11, ".iso");
            if (dot && dot > base + 11 && (size_t)(dot - (base + 11)) < sizeof winiso_st.win_lang) {
                snprintf(winiso_st.win_lang, sizeof winiso_st.win_lang, "%.*s",
                         (int)(dot - (base + 11)), base + 11);
            }
        }
    }

    out[0] = '\0';
    char *const vargv[] = { LOCACHY_WINISO, "path", "virtio-win", NULL };
    command_output(vargv, out, sizeof out);
    nl = strpbrk(out, "\r\n");
    if (nl) *nl = '\0';
    if (out[0] == '/') {
        vm_copy(winiso_st.virtio_path, sizeof winiso_st.virtio_path, out);
        if (stat(out, &st) == 0) winiso_st.virtio_size = (unsigned long long)st.st_size;
    }
}

static void winiso_check(void)
{
    struct jobs_row r;
    char log[96];
    if (winiso_job && job_ended(winiso_job, &r, log, sizeof log)) {
        winiso_job = 0;
        load_winiso_status();
        entries_dirty = 1;
        full_redraw = 1;
        if (strcmp(r.state, "done") == 0) {
            snprintf(notice, sizeof notice, _("Windows 11 ISO (%s) downloaded and verified."),
                     winiso_job_lang[0] ? winiso_job_lang : "en-US");
        } else {
            snprintf(notice, sizeof notice, _("Windows ISO download failed: %.120s"),
                     r.last[0] ? r.last : log);
        }
        winiso_job_lang[0] = '\0';
    }
    if (vm_remove_job && job_ended(vm_remove_job, &r, log, sizeof log)) {
        vm_remove_job = 0;
        load_vm_apps();
        entries_dirty = 1;
        full_redraw = 1;
        if (strcmp(r.state, "done") == 0) {
            snprintf(notice, sizeof notice, _("Removed VM app '%.40s'."), vm_remove_name);
        } else {
            snprintf(notice, sizeof notice, _("Remove VM app failed: %.120s"),
                     r.last[0] ? r.last : log);
        }
        vm_remove_name[0] = '\0';
    }
}

/* Atomically write VM_CONF_DIR/NAME.conf using ONLY the closed VM_KEYS set for c->vm_kind. */
static int vm_write_conf(const struct vm_app_cfg *c, char *why, size_t wsize)
{
    if (!compute_valid_name_input(c->name)) {
        snprintf(why, wsize, "%s", _("invalid VM name"));
        return -1;
    }
    char path[512], tmp[560];
    snprintf(path, sizeof path, "%.320s/%.63s.conf", VM_CONF_DIR, c->name);
    snprintf(tmp, sizeof tmp, "%.320s/.%.63s.conf.tmp.%d", VM_CONF_DIR, c->name, (int)getpid());
    mkdir(VM_CONF_DIR, 0755);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        snprintf(why, wsize, "open: %s", strerror(errno));
        return -1;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        snprintf(why, wsize, "%s", "fdopen failed");
        return -1;
    }
    fprintf(f, "vm=\"1\"\n");
    fprintf(f, "vm_kind=\"%s\"\n", c->vm_kind[0] ? c->vm_kind : "custom");
    fprintf(f, "vm_display=\"%s\"\n", c->vm_display[0] ? c->vm_display : "gtk");
    fprintf(f, "vm_freeze=\"%s\"\n", c->vm_freeze[0] ? c->vm_freeze : "qmp");
    fprintf(f, "network=\"%d\"\n", c->network ? 1 : 0);

    if (strcmp(c->vm_kind, "compute") == 0) {
        if (c->vm_name[0]) fprintf(f, "vm_name=\"%s\"\n", c->vm_name);
    } else if (strcmp(c->vm_kind, "winmaint") == 0) {
        if (c->vm_memory > 0) fprintf(f, "vm_memory=\"%d\"\n", c->vm_memory);
        if (c->vm_cpus > 0) fprintf(f, "vm_cpus=\"%d\"\n", c->vm_cpus);
        if (c->vm_disk_bus[0]) fprintf(f, "vm_disk_bus=\"%s\"\n", c->vm_disk_bus);
        if (c->vm_nic[0]) fprintf(f, "vm_nic=\"%s\"\n", c->vm_nic);
        if (c->vm_iso[0]) fprintf(f, "vm_iso=\"%s\"\n", c->vm_iso);
        if (c->vm_virtio[0]) fprintf(f, "vm_virtio_iso=\"%s\"\n", c->vm_virtio);
    } else {
        fprintf(f, "vm_os=\"%s\"\n", c->vm_os[0] ? c->vm_os : "linux");
        if (c->vm_base[0]) fprintf(f, "vm_base=\"%s\"\n", c->vm_base);
        else if (c->vm_disk[0]) fprintf(f, "vm_disk=\"%s\"\n", c->vm_disk);
        if (c->vm_memory > 0) fprintf(f, "vm_memory=\"%d\"\n", c->vm_memory);
        if (c->vm_cpus > 0) fprintf(f, "vm_cpus=\"%d\"\n", c->vm_cpus);
        fprintf(f, "vm_uefi=\"%d\"\n", c->vm_uefi ? 1 : 0);
        fprintf(f, "vm_tpm=\"%d\"\n", c->vm_tpm ? 1 : 0);
        fprintf(f, "vm_share_home=\"%d\"\n", c->vm_share_home ? 1 : 0);
        fprintf(f, "vm_disk_bus=\"%s\"\n", c->vm_disk_bus[0] ? c->vm_disk_bus : "virtio");
        fprintf(f, "vm_nic=\"%s\"\n", c->vm_nic[0] ? c->vm_nic : "virtio");
        fprintf(f, "vm_gpu=\"%s\"\n",
                (strcmp(c->vm_os, "linux") == 0 && strcmp(c->vm_gpu, "gl") == 0) ? "gl" : "2d");
        fprintf(f, "vm_audio=\"%d\"\n", c->vm_audio ? 1 : 0);
        if (c->vm_iso[0]) fprintf(f, "vm_iso=\"%s\"\n", c->vm_iso);
        if (c->vm_virtio[0]) fprintf(f, "vm_virtio_iso=\"%s\"\n", c->vm_virtio);
    }

    int err = ferror(f) || fflush(f) != 0 || fsync(fileno(f)) != 0;
    if (fclose(f) != 0 || err) {
        unlink(tmp);
        snprintf(why, wsize, "%s", _("write error"));
        return -1;
    }
    if (rename(tmp, path) != 0) {
        snprintf(why, wsize, "rename: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int vm_create_blank_qcow2(const char *name, int size_gib, char *out_path, size_t out_size,
                                 char *why, size_t wsize)
{
    char sz[24], run_out[256] = "";
    mkdir(VM_DISKS_DIR, 0755);
    int pn = snprintf(out_path, out_size, "%.180s/%.63s.qcow2", VM_DISKS_DIR, name);
    if (pn < 0 || (size_t)pn >= out_size) {
        snprintf(why, wsize, "%s", _("disk path is too long"));
        return -1;
    }
    struct stat st;
    if (stat(out_path, &st) == 0) {
        snprintf(why, wsize, _("disk already exists: %.100s"), out_path);
        return -1;
    }
    snprintf(sz, sizeof sz, "%dG", size_gib);
    char *const argv[] = { "/usr/bin/qemu-img", "create", "-f", "qcow2", out_path, sz, NULL };
    int rc = compute_exec(argv, run_out, sizeof run_out);
    if (rc != 0) {
        snprintf(why, wsize, "%s", run_out[0] ? run_out : _("qemu-img create failed"));
        return -1;
    }
    if (geteuid() == 0) {
        if (chown(out_path, 1000, 1000) != 0) { /* best effort */ }
        chmod(out_path, 0644);
    }
    return 0;
}

/* Single interactive form for creating or editing a VM app (custom, winmaint, or compute). */
static int vm_app_form(const struct vm_app_cfg *existing)
{
    int editing = existing != NULL;
    struct vm_app_cfg cfg;
    if (editing) {
        cfg = *existing;
    } else {
        vm_cfg_defaults(&cfg, "custom", "windows");
        vm_copy(cfg.name, sizeof cfg.name, "win11");
    }
    int is_custom = strcmp(cfg.vm_kind, "custom") == 0;
    int is_winmaint = strcmp(cfg.vm_kind, "winmaint") == 0;
    int stor_mode = 0;
    int blank_gib = 64;
    if (editing && is_custom) {
        stor_mode = cfg.vm_base[0] ? 1 : 2;
    }

    int sel = editing ? 1 : 0, top = 0;
    char line[320];
    notice[0] = '\0';

    for (;;) {
        int total = is_custom ? 17 : (is_winmaint ? 10 : 5);
        int cols = screen_cols(), rows = screen_rows();
        int list_y = 6;
        int height = rows - list_y - 4;
        if (height < 1) height = 1;
        if (sel >= total) sel = total - 1;
        if (sel < 0) sel = 0;
        if (sel < top) top = sel;
        if (sel >= top + height) top = sel - height + 1;

        screen_fill(0, 0, cols, rows, BG);
        if (is_winmaint) snprintf(line, sizeof line, "%s", _("Maintenance Windows VM Settings"));
        else if (editing) snprintf(line, sizeof line, _("VM App Settings: %.40s (%s)"), cfg.name, cfg.vm_kind);
        else snprintf(line, sizeof line, "%s", _("New Virtual Machine App"));
        screen_bar(line, NULL);
        ui_hits_clear();

        screen_fill(0, 2, cols, 3, SELECTION);
        if (is_winmaint) {
            screen_text(2, 2, _("Configures /etc/locachy/containers/winmaint.conf for the in-OS Maintenance Windows VM."), FG, SELECTION);
            screen_text(2, 3, _("Leave ISO fields empty to use locachy-winiso automatically when installing onto a blank partition."), DIM, SELECTION);
        } else if (is_custom) {
            screen_text(2, 2, _("Managed KVM virtual machine app registered in Home (runs as uid 1000 in its own cgroup)."), FG, SELECTION);
            screen_text(2, 3, _("Windows VMs automatically use verified Windows 11 & virtio-win ISOs when ISO paths are left blank."), DIM, SELECTION);
        } else {
            screen_text(2, 2, _("Desktop launcher settings for a compute micro-VM exported to Home."), FG, SELECTION);
            screen_text(2, 3, _("Hardware limits (memory/vCPU/disk) belong to Compute Node Manager > Micro-VMs."), DIM, SELECTION);
        }
        screen_text(2, 4, _("Tip: Left/Right toggles options, Enter edits or saves, Esc cancels."), DIM, SELECTION);

        for (int r = 0; r < height && top + r < total; r++) {
            int idx = top + r, y = list_y + r, on = idx == sel;
            line[0] = '\0';
            if (is_custom) {
                switch (idx) {
                case 0: snprintf(line, sizeof line, editing ? _("VM app name:        %.64s  (fixed)") : _("VM app name:        %.64s"), cfg.name); break;
                case 1: snprintf(line, sizeof line, _("Guest OS profile:   < %s >"), strcmp(cfg.vm_os, "windows") == 0 ? _("Windows (UEFI + TPM + Hyper-V)") : _("Linux (virtio)")); break;
                case 2:
                    if (stor_mode == 0) snprintf(line, sizeof line, _("Disk mode:          < Create blank qcow2 (%d GiB) >"), blank_gib);
                    else if (stor_mode == 1) snprintf(line, sizeof line, "%s", _("Disk mode:          < CoW overlay on read-only base (vm_base) >"));
                    else snprintf(line, sizeof line, "%s", _("Disk mode:          < Direct writable disk image (vm_disk) >"));
                    break;
                case 3:
                    if (stor_mode == 0) snprintf(line, sizeof line, _("New disk size:      < %d GiB >  (%.64s/%.32s.qcow2)"), blank_gib, VM_DISKS_DIR, cfg.name);
                    else if (stor_mode == 1) snprintf(line, sizeof line, _("Base disk path:     %.120s"), cfg.vm_base[0] ? cfg.vm_base : _("(Enter: path to read-only qcow2/raw)"));
                    else snprintf(line, sizeof line, _("Writable disk path: %.120s"), cfg.vm_disk[0] ? cfg.vm_disk : _("(Enter: path to qcow2/raw disk)"));
                    break;
                case 4:
                    snprintf(line, sizeof line, _("Install ISO:        %.120s"),
                             cfg.vm_iso[0] ? cfg.vm_iso :
                             (strcmp(cfg.vm_os, "windows") == 0 ? _("(auto from locachy-winiso / Enter: custom)") : _("(none / Enter: path to .iso)")));
                    break;
                case 5:
                    snprintf(line, sizeof line, _("virtio-win ISO:     %.120s"),
                             cfg.vm_virtio[0] ? cfg.vm_virtio :
                             (strcmp(cfg.vm_os, "windows") == 0 ? _("(auto from locachy-winiso / Enter: custom)") : _("(none)")));
                    break;
                case 6: snprintf(line, sizeof line, _("Memory:             < %d MiB >"), cfg.vm_memory); break;
                case 7: snprintf(line, sizeof line, _("vCPUs:              < %d >"), cfg.vm_cpus); break;
                case 8: snprintf(line, sizeof line, _("UEFI firmware:      < %s >"), cfg.vm_uefi ? _("ON (OVMF)") : _("OFF (SeaBIOS)")); break;
                case 9: snprintf(line, sizeof line, _("TPM 2.0 (swtpm):    < %s >"), cfg.vm_tpm ? _("ON") : _("OFF")); break;
                case 10: snprintf(line, sizeof line, _("Disk bus:           < %s >"), cfg.vm_disk_bus); break;
                case 11: snprintf(line, sizeof line, _("Network:            < %s >  (NIC: %s)"), cfg.network ? _("ON (locachy-net)") : _("OFF (isolated)"), cfg.vm_nic); break;
                case 12: snprintf(line, sizeof line, _("NIC model:          < %s >"), cfg.vm_nic); break;
                case 13: snprintf(line, sizeof line, _("Share Home (virtiofs): < %s >"), cfg.vm_share_home ? _("ON (tag: home)") : _("OFF")); break;
                case 14:
                    snprintf(line, sizeof line, _("Graphics / Audio:   < %s > / audio %s  (Enter: toggle audio)"),
                             (strcmp(cfg.vm_os, "linux") == 0 && strcmp(cfg.vm_gpu, "gl") == 0) ? _("OpenGL (virgl)") : _("2D (virtio-vga)"),
                             cfg.vm_audio ? _("ON") : _("OFF"));
                    break;
                case 15: snprintf(line, sizeof line, _("Display / Freeze:   < %s > / freeze %s  (Enter: toggle freeze)"), cfg.vm_display, cfg.vm_freeze); break;
                default: snprintf(line, sizeof line, "%s", editing ? _("[ Save VM App Settings ]") : _("[ Save & Register VM App in Home ]")); break;
                }
            } else if (is_winmaint) {
                switch (idx) {
                case 0: snprintf(line, sizeof line, _("Memory:             < %d MiB >"), cfg.vm_memory); break;
                case 1: snprintf(line, sizeof line, _("vCPUs:              < %d >"), cfg.vm_cpus); break;
                case 2: snprintf(line, sizeof line, _("Disk bus:           < %s >  (nvme recommended for bare-metal parity)"), cfg.vm_disk_bus); break;
                case 3: snprintf(line, sizeof line, _("NIC model:          < %s >"), cfg.vm_nic); break;
                case 4: snprintf(line, sizeof line, _("Network:            < %s >"), cfg.network ? _("ON (locachy-net)") : _("OFF (isolated)")); break;
                case 5: snprintf(line, sizeof line, _("Freeze on switch:   < %s >"), cfg.vm_freeze); break;
                case 6: snprintf(line, sizeof line, _("Display backend:    < %s >"), cfg.vm_display); break;
                case 7: snprintf(line, sizeof line, _("Install ISO:        %.120s"), cfg.vm_iso[0] ? cfg.vm_iso : _("(auto from locachy-winiso / Enter: custom)")); break;
                case 8: snprintf(line, sizeof line, _("virtio-win ISO:     %.120s"), cfg.vm_virtio[0] ? cfg.vm_virtio : _("(auto from locachy-winiso / Enter: custom)")); break;
                default: snprintf(line, sizeof line, "%s", _("[ Save Maintenance Windows VM Settings ]")); break;
                }
            } else {
                switch (idx) {
                case 0: snprintf(line, sizeof line, _("Compute VM app:     %.64s -> %.64s (fixed)"), cfg.name, cfg.vm_name[0] ? cfg.vm_name : cfg.name); break;
                case 1: snprintf(line, sizeof line, _("Display backend:    < %s >"), cfg.vm_display); break;
                case 2: snprintf(line, sizeof line, _("Freeze on switch:   < %s >"), cfg.vm_freeze); break;
                case 3: snprintf(line, sizeof line, _("Network:            < %s >"), cfg.network ? _("ON (locachy-net)") : _("OFF (isolated)")); break;
                default: snprintf(line, sizeof line, "%s", _("[ Save Compute VM App Settings ]")); break;
                }
            }
            screen_fill(0, y, cols, 1, on ? ACCENT : BG);
            screen_text(3, y, line, on ? ON_ACCENT : FG, on ? ACCENT : BG);
            ui_hit(0, y, cols, 1, idx);
        }

        if (notice[0]) screen_text(2, rows - 4, notice, ACCENT_TEXT, BG);
        screen_text(2, rows - 3, _("Up/Down: move   Left/Right: change   Enter: edit/save   Esc: cancel"), ACCENT_TEXT, BG);
        settings_back_button(rows - 2);
        screen_flush();

        int key = key_get(), mods = mods_now(), act = key_list_action(key, mods);
        if (close_read(key)) return 0;
        if (key == K_REPAINT || key == K_SHOWN) continue;
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (settings_back_clicked(e)) return 0;
            if (e->wheel && total > 0) {
                sel -= e->wheel;
                if (sel < 0) sel = 0;
                if (sel >= total) sel = total - 1;
                continue;
            }
            if (e->button == BTN_LEFT && e->pressed) {
                int hit = ui_hit_at(e->col, e->row);
                if (hit >= 0 && hit < total) {
                    sel = hit;
                    if (e->clicks == 2 || sel == total - 1) key = '\n';
                    else continue;
                } else continue;
            } else continue;
        }
        if (act == ACT_HELP) { help_screen(); if (close_due()) return 0; continue; }
        if (act == ACT_CANCEL) return 0;
        if (act != ACT_NONE) continue;

        if (key == K_UP && sel > 0) { sel--; continue; }
        if (key == K_DOWN && sel + 1 < total) { sel++; continue; }
        if (key == K_BOL) { sel = 0; continue; }
        if (key == K_EOL) { sel = total - 1; continue; }

        int dir = (key == K_LEFT) ? -1 : 1;
        int is_toggle = (key == K_LEFT || key == K_RIGHT || key == ' ');

        if (is_custom) {
            if (is_toggle) {
                if (sel == 1) {
                    int to_win = strcmp(cfg.vm_os, "windows") != 0;
                    vm_copy(cfg.vm_os, sizeof cfg.vm_os, to_win ? "windows" : "linux");
                    if (!editing) {
                        cfg.vm_uefi = to_win ? 1 : 0;
                        cfg.vm_tpm = to_win ? 1 : 0;
                        vm_copy(cfg.vm_disk_bus, sizeof cfg.vm_disk_bus, to_win ? "nvme" : "virtio");
                        vm_copy(cfg.vm_nic, sizeof cfg.vm_nic, to_win ? "e1000e" : "virtio");
                        if (to_win) vm_copy(cfg.vm_gpu, sizeof cfg.vm_gpu, "2d");
                    }
                } else if (sel == 2) {
                    int modes = editing ? 2 : 3;
                    int cur = editing ? (stor_mode == 1 ? 0 : 1) : stor_mode;
                    cur = (cur + dir + modes) % modes;
                    stor_mode = editing ? (cur == 0 ? 1 : 2) : cur;
                } else if (sel == 3 && stor_mode == 0) {
                    int pi = 0;
                    for (int i = 0; i < NVM_DISK_SIZES; i++) if (vm_disk_sizes_gib[i] == blank_gib) { pi = i; break; }
                    blank_gib = vm_disk_sizes_gib[(pi + dir + NVM_DISK_SIZES) % NVM_DISK_SIZES];
                } else if (sel == 6) {
                    int pi = 0;
                    for (int i = 0; i < NCOMPUTE_VM_MEM_PRESETS; i++) if (compute_vm_mem_presets[i] == cfg.vm_memory) { pi = i; break; }
                    cfg.vm_memory = compute_vm_mem_presets[(pi + dir + NCOMPUTE_VM_MEM_PRESETS) % NCOMPUTE_VM_MEM_PRESETS];
                } else if (sel == 7) {
                    int pi = 0;
                    for (int i = 0; i < NCOMPUTE_VM_CPU_PRESETS; i++) if (compute_vm_cpu_presets[i] == cfg.vm_cpus) { pi = i; break; }
                    cfg.vm_cpus = compute_vm_cpu_presets[(pi + dir + NCOMPUTE_VM_CPU_PRESETS) % NCOMPUTE_VM_CPU_PRESETS];
                } else if (sel == 8) cfg.vm_uefi = !cfg.vm_uefi;
                else if (sel == 9) cfg.vm_tpm = !cfg.vm_tpm;
                else if (sel == 10) {
                    const char *buses[3] = { "virtio", "nvme", "sata" };
                    int bi = strcmp(cfg.vm_disk_bus, "nvme") == 0 ? 1 : strcmp(cfg.vm_disk_bus, "sata") == 0 ? 2 : 0;
                    vm_copy(cfg.vm_disk_bus, sizeof cfg.vm_disk_bus, buses[(bi + dir + 3) % 3]);
                } else if (sel == 11) cfg.network = !cfg.network;
                else if (sel == 12) vm_copy(cfg.vm_nic, sizeof cfg.vm_nic, strcmp(cfg.vm_nic, "virtio") == 0 ? "e1000e" : "virtio");
                else if (sel == 13) cfg.vm_share_home = !cfg.vm_share_home;
                else if (sel == 14) {
                    if (strcmp(cfg.vm_os, "linux") == 0)
                        vm_copy(cfg.vm_gpu, sizeof cfg.vm_gpu, strcmp(cfg.vm_gpu, "gl") == 0 ? "2d" : "gl");
                    else snprintf(notice, sizeof notice, "%s", _("OpenGL (virgl) is Linux guests only; press Enter to toggle audio."));
                } else if (sel == 15) {
                    vm_copy(cfg.vm_display, sizeof cfg.vm_display, strcmp(cfg.vm_display, "gtk") == 0 ? "sdl" : "gtk");
                }
            } else if (key == '\n') {
                if (sel == 0 && !editing) {
                    char buf[68]; vm_copy(buf, sizeof buf, cfg.name);
                    if (prompt(_("VM app name: "), buf, sizeof buf) && buf[0]) {
                        if (compute_valid_name_input(buf)) vm_copy(cfg.name, sizeof cfg.name, buf);
                        else snprintf(notice, sizeof notice, "%s", _("Invalid name: letters, digits, . _ - only."));
                    }
                } else if (sel == 3 && stor_mode != 0) {
                    char buf[256];
                    vm_copy(buf, sizeof buf, stor_mode == 1 ? cfg.vm_base : cfg.vm_disk);
                    if (prompt(stor_mode == 1 ? _("Base disk path (read-only qcow2/raw): ") : _("Writable disk path (qcow2/raw): "), buf, sizeof buf)) {
                        if (!buf[0] || vm_path_safe(buf)) {
                            if (stor_mode == 1) { vm_copy(cfg.vm_base, sizeof cfg.vm_base, buf); cfg.vm_disk[0] = '\0'; }
                            else { vm_copy(cfg.vm_disk, sizeof cfg.vm_disk, buf); cfg.vm_base[0] = '\0'; }
                        } else snprintf(notice, sizeof notice, "%s", _("Path must be absolute without '..' or quotes."));
                    }
                } else if (sel == 4 || sel == 5) {
                    char buf[256];
                    vm_copy(buf, sizeof buf, sel == 4 ? cfg.vm_iso : cfg.vm_virtio);
                    if (prompt(sel == 4 ? _("Install ISO path (empty = auto/none): ") : _("virtio-win ISO path (empty = auto/none): "), buf, sizeof buf)) {
                        if (!buf[0] || vm_path_safe(buf)) {
                            if (sel == 4) vm_copy(cfg.vm_iso, sizeof cfg.vm_iso, buf);
                            else vm_copy(cfg.vm_virtio, sizeof cfg.vm_virtio, buf);
                        } else snprintf(notice, sizeof notice, "%s", _("Path must be absolute without '..' or quotes."));
                    }
                } else if (sel == 6) {
                    char buf[24]; snprintf(buf, sizeof buf, "%d", cfg.vm_memory);
                    if (prompt(_("Memory in MiB (512..262144): "), buf, sizeof buf) && buf[0]) {
                        int v = atoi(buf);
                        if (v >= 512 && v <= 262144) cfg.vm_memory = v;
                        else snprintf(notice, sizeof notice, "%s", _("Memory must be between 512 and 262144 MiB."));
                    }
                } else if (sel == 7) {
                    char buf[16]; snprintf(buf, sizeof buf, "%d", cfg.vm_cpus);
                    if (prompt(_("vCPU count (1..128): "), buf, sizeof buf) && buf[0]) {
                        int v = atoi(buf);
                        if (v >= 1 && v <= 128) cfg.vm_cpus = v;
                        else snprintf(notice, sizeof notice, "%s", _("vCPUs must be between 1 and 128."));
                    }
                } else if (sel == 8) cfg.vm_uefi = !cfg.vm_uefi;
                else if (sel == 9) cfg.vm_tpm = !cfg.vm_tpm;
                else if (sel == 11) cfg.network = !cfg.network;
                else if (sel == 13) cfg.vm_share_home = !cfg.vm_share_home;
                else if (sel == 14) cfg.vm_audio = !cfg.vm_audio;
                else if (sel == 15) vm_copy(cfg.vm_freeze, sizeof cfg.vm_freeze, strcmp(cfg.vm_freeze, "qmp") == 0 ? "none" : "qmp");
                else if (sel == total - 1) {
                    char why[160] = "";
                    if (!compute_valid_name_input(cfg.name)) {
                        snprintf(notice, sizeof notice, "%s", _("Invalid VM name."));
                        continue;
                    }
                    if (stor_mode == 0) {
                        char new_disk[256] = "";
                        if (vm_create_blank_qcow2(cfg.name, blank_gib, new_disk, sizeof new_disk, why, sizeof why) < 0) {
                            snprintf(notice, sizeof notice, _("Could not create disk: %.120s"), why);
                            continue;
                        }
                        vm_copy(cfg.vm_disk, sizeof cfg.vm_disk, new_disk);
                        cfg.vm_base[0] = '\0';
                    } else if (stor_mode == 1) {
                        if (!vm_path_safe(cfg.vm_base)) {
                            snprintf(notice, sizeof notice, "%s", _("Set a valid absolute base disk path first."));
                            continue;
                        }
                        cfg.vm_disk[0] = '\0';
                    } else {
                        if (!vm_path_safe(cfg.vm_disk)) {
                            snprintf(notice, sizeof notice, "%s", _("Set a valid absolute writable disk path first."));
                            continue;
                        }
                        cfg.vm_base[0] = '\0';
                    }
                    if (vm_write_conf(&cfg, why, sizeof why) < 0) {
                        snprintf(notice, sizeof notice, _("Save VM config failed: %.120s"), why);
                        continue;
                    }
                    load_vm_apps();
                    snprintf(notice, sizeof notice, editing ?
                             _("Saved VM app '%.40s'.") : _("Created VM app '%.40s' (now visible in Home)."), cfg.name);
                    return 1;
                }
            }
        } else if (is_winmaint) {
            if (is_toggle) {
                if (sel == 0) {
                    int pi = 0;
                    for (int i = 0; i < NCOMPUTE_VM_MEM_PRESETS; i++) if (compute_vm_mem_presets[i] == cfg.vm_memory) { pi = i; break; }
                    cfg.vm_memory = compute_vm_mem_presets[(pi + dir + NCOMPUTE_VM_MEM_PRESETS) % NCOMPUTE_VM_MEM_PRESETS];
                } else if (sel == 1) {
                    int pi = 0;
                    for (int i = 0; i < NCOMPUTE_VM_CPU_PRESETS; i++) if (compute_vm_cpu_presets[i] == cfg.vm_cpus) { pi = i; break; }
                    cfg.vm_cpus = compute_vm_cpu_presets[(pi + dir + NCOMPUTE_VM_CPU_PRESETS) % NCOMPUTE_VM_CPU_PRESETS];
                } else if (sel == 2) vm_copy(cfg.vm_disk_bus, sizeof cfg.vm_disk_bus, strcmp(cfg.vm_disk_bus, "nvme") == 0 ? "virtio" : "nvme");
                else if (sel == 3) vm_copy(cfg.vm_nic, sizeof cfg.vm_nic, strcmp(cfg.vm_nic, "e1000e") == 0 ? "virtio" : "e1000e");
                else if (sel == 4) cfg.network = !cfg.network;
                else if (sel == 5) vm_copy(cfg.vm_freeze, sizeof cfg.vm_freeze, strcmp(cfg.vm_freeze, "qmp") == 0 ? "none" : "qmp");
                else if (sel == 6) vm_copy(cfg.vm_display, sizeof cfg.vm_display, strcmp(cfg.vm_display, "gtk") == 0 ? "sdl" : "gtk");
            } else if (key == '\n') {
                if (sel == 0) {
                    char buf[24]; snprintf(buf, sizeof buf, "%d", cfg.vm_memory);
                    if (prompt(_("Memory in MiB (1024..262144): "), buf, sizeof buf) && buf[0]) {
                        int v = atoi(buf);
                        if (v >= 1024 && v <= 262144) cfg.vm_memory = v;
                    }
                } else if (sel == 1) {
                    char buf[16]; snprintf(buf, sizeof buf, "%d", cfg.vm_cpus);
                    if (prompt(_("vCPU count (1..128): "), buf, sizeof buf) && buf[0]) {
                        int v = atoi(buf);
                        if (v >= 1 && v <= 128) cfg.vm_cpus = v;
                    }
                } else if (sel == 4) cfg.network = !cfg.network;
                else if (sel == 5) vm_copy(cfg.vm_freeze, sizeof cfg.vm_freeze, strcmp(cfg.vm_freeze, "qmp") == 0 ? "none" : "qmp");
                else if (sel == 7 || sel == 8) {
                    char buf[256];
                    vm_copy(buf, sizeof buf, sel == 7 ? cfg.vm_iso : cfg.vm_virtio);
                    if (prompt(sel == 7 ? _("Custom Windows ISO path (empty = auto): ") : _("Custom virtio-win ISO path (empty = auto): "), buf, sizeof buf)) {
                        if (!buf[0] || vm_path_safe(buf)) {
                            if (sel == 7) vm_copy(cfg.vm_iso, sizeof cfg.vm_iso, buf);
                            else vm_copy(cfg.vm_virtio, sizeof cfg.vm_virtio, buf);
                        } else snprintf(notice, sizeof notice, "%s", _("Path must be absolute without '..' or quotes."));
                    }
                } else if (sel == total - 1) {
                    char why[160] = "";
                    if (vm_write_conf(&cfg, why, sizeof why) < 0) {
                        snprintf(notice, sizeof notice, _("Save winmaint.conf failed: %.120s"), why);
                        continue;
                    }
                    load_vm_apps();
                    snprintf(notice, sizeof notice, "%s", _("Saved Maintenance Windows VM settings."));
                    return 1;
                }
            }
        } else {
            if (is_toggle) {
                if (sel == 1) vm_copy(cfg.vm_display, sizeof cfg.vm_display, strcmp(cfg.vm_display, "gtk") == 0 ? "sdl" : "gtk");
                else if (sel == 2) vm_copy(cfg.vm_freeze, sizeof cfg.vm_freeze, strcmp(cfg.vm_freeze, "qmp") == 0 ? "none" : "qmp");
                else if (sel == 3) cfg.network = !cfg.network;
            } else if (key == '\n') {
                if (sel == 3) cfg.network = !cfg.network;
                else if (sel == total - 1) {
                    char why[160] = "";
                    if (vm_write_conf(&cfg, why, sizeof why) < 0) {
                        snprintf(notice, sizeof notice, _("Save failed: %.120s"), why);
                        continue;
                    }
                    load_vm_apps();
                    snprintf(notice, sizeof notice, _("Saved VM app '%.40s'."), cfg.name);
                    return 1;
                }
            }
        }
    }
}

static void vm_app_start_row(int idx)
{
    if (idx < 0 || idx >= nvm_apps) return;
    char name[64], why[160] = "";
    vm_copy(name, sizeof name, vm_apps[idx].name);
    vm_apps_remove_asked = -1;
    if (base_app_action && base_app_action("start", name, why, sizeof why) == 0) {
        snprintf(notice, sizeof notice, _("Opening VM app '%.40s' on desktop."), name);
    } else {
        snprintf(notice, sizeof notice, _("Cannot start VM app '%.40s': %.100s"),
                 name, why[0] ? why : _("desktop session unavailable"));
    }
    load_vm_apps();
}

static void vm_app_edit_row(int idx)
{
    if (idx < 0 || idx >= nvm_apps) return;
    struct vm_app_cfg copy = vm_apps[idx];
    vm_apps_remove_asked = -1;
    vm_app_form(&copy);
    entries_dirty = 1;
    full_redraw = 1;
}

static void vm_app_stop_row(int idx)
{
    if (idx < 0 || idx >= nvm_apps) return;
    char name[64], why[160] = "";
    vm_copy(name, sizeof name, vm_apps[idx].name);
    vm_apps_remove_asked = -1;
    if (base_app_action && base_app_action("stop", name, why, sizeof why) == 0) {
        snprintf(notice, sizeof notice, _("Requested graceful shutdown for VM '%.40s'."), name);
    } else {
        snprintf(notice, sizeof notice, _("Cannot stop VM '%.40s': %.100s"),
                 name, why[0] ? why : _("desktop session unavailable"));
    }
    load_vm_apps();
}

static void vm_app_remove_row(int idx)
{
    if (idx < 0 || idx >= nvm_apps) return;
    if (vm_remove_job) {
        snprintf(notice, sizeof notice, "%s", _("Wait: a VM removal job is already running."));
        return;
    }
    if (vm_apps[idx].running) {
        snprintf(notice, sizeof notice, _("Stop VM '%.40s' (press s) before removing it."), vm_apps[idx].name);
        return;
    }
    if (vm_apps_remove_asked != idx) {
        vm_apps_remove_asked = idx;
        snprintf(notice, sizeof notice,
                 _("Press Delete again to remove VM app '%.40s' (user base/disk files are kept)."),
                 vm_apps[idx].name);
        return;
    }
    vm_apps_remove_asked = -1;
    char name[64], why[160] = "";
    vm_copy(name, sizeof name, vm_apps[idx].name);
    int id = 0, pos = 0;
    const char *args[] = { name, NULL };
    if (jobs_submit("app-remove", args, &id, &pos, why, sizeof why) < 0) {
        snprintf(notice, sizeof notice, _("Could not remove VM app '%.40s': %.100s"), name, why);
        return;
    }
    vm_remove_job = id;
    vm_copy(vm_remove_name, sizeof vm_remove_name, name);
    snprintf(notice, sizeof notice, _("Removing VM app '%.40s'..."), name);
}

static void vm_new_app_row(void)
{
    vm_apps_remove_asked = -1;
    vm_app_form(NULL);
    entries_dirty = 1;
    full_redraw = 1;
}

static void vm_load_winmaint_conf(struct vm_app_cfg *wcfg)
{
    vm_cfg_defaults(wcfg, "winmaint", "windows");
    char path[512];
    snprintf(path, sizeof path, "%.320s/%s.conf", VM_CONF_DIR, WINMAINT_APP);
    vm_read_conf_file(path, wcfg);
    vm_copy(wcfg->name, sizeof wcfg->name, WINMAINT_APP);
    vm_copy(wcfg->vm_kind, sizeof wcfg->vm_kind, "winmaint");
    if (wcfg->vm_memory <= 0) wcfg->vm_memory = 4096;
    if (wcfg->vm_cpus <= 0) wcfg->vm_cpus = 4;
    if (!wcfg->vm_disk_bus[0]) vm_copy(wcfg->vm_disk_bus, sizeof wcfg->vm_disk_bus, "nvme");
    if (!wcfg->vm_nic[0]) vm_copy(wcfg->vm_nic, sizeof wcfg->vm_nic, "e1000e");
    if (!wcfg->vm_display[0]) vm_copy(wcfg->vm_display, sizeof wcfg->vm_display, "gtk");
    if (!wcfg->vm_freeze[0]) vm_copy(wcfg->vm_freeze, sizeof wcfg->vm_freeze, "qmp");
}

static void winmaint_conf_summary(char *out, size_t size)
{
    struct vm_app_cfg wcfg;
    vm_load_winmaint_conf(&wcfg);
    snprintf(out, size, _("%d MiB, %d vCPU, %s, %s, freeze:%s  (Enter)"),
             wcfg.vm_memory, wcfg.vm_cpus, wcfg.vm_disk_bus, wcfg.vm_nic, wcfg.vm_freeze);
}

static void winmaint_conf_row(void)
{
    struct vm_app_cfg wcfg;
    vm_load_winmaint_conf(&wcfg);
    vm_app_form(&wcfg);
    entries_dirty = 1;
    full_redraw = 1;
}

static void winmaint_stop_vm_row(void)
{
    char why[160] = "";
    if (base_app_action && base_app_action("stop", WINMAINT_APP, why, sizeof why) == 0) {
        snprintf(notice, sizeof notice, "%s", _("Requested graceful QMP shutdown for Maintenance Windows VM."));
    } else {
        snprintf(notice, sizeof notice, _("Cannot stop Maintenance Windows VM: %.100s"),
                 why[0] ? why : _("desktop session unavailable"));
    }
    winmaint_load_targets();
    load_vm_apps();
}

static void winiso_cycle_lang(int dir)
{
    int li = 0;
    for (int i = 0; i < NWINISO_LANGS; i++) {
        if (strcmp(winiso_langs[i], winiso_lang_sel) == 0) {
            li = i;
            break;
        }
    }
    vm_copy(winiso_lang_sel, sizeof winiso_lang_sel,
            winiso_langs[(li + (dir < 0 ? NWINISO_LANGS - 1 : 1)) % NWINISO_LANGS]);
    snprintf(notice, sizeof notice, _("Windows 11 ISO download language set to %s."), winiso_lang_sel);
}

static void winiso_prompt_lang(void)
{
    char buf[24];
    vm_copy(buf, sizeof buf, winiso_lang_sel);
    if (prompt(_("Language tag (e.g. en-US, zh-CN): "), buf, sizeof buf) && buf[0]) {
        if (vm_valid_tag(buf)) {
            vm_copy(winiso_lang_sel, sizeof winiso_lang_sel, buf);
            snprintf(notice, sizeof notice, _("Windows 11 ISO download language set to %s."), winiso_lang_sel);
        } else {
            snprintf(notice, sizeof notice, "%s", _("Invalid tag: expected e.g. en-US or zh-CN."));
        }
    }
}

static void winiso_fetch_row(void)
{
    if (access(LOCACHY_WINISO, X_OK) != 0) {
        snprintf(notice, sizeof notice, _("%s is not installed."), "locachy-winiso");
        return;
    }
    if (winiso_job) {
        snprintf(notice, sizeof notice, _("Windows 11 ISO download is already running (job #%d)."), winiso_job);
        return;
    }
    char why[160] = "";
    int id = 0, pos = 0;
    const char *args[] = { "--lang", winiso_lang_sel, NULL };
    if (jobs_submit("winiso-fetch", args, &id, &pos, why, sizeof why) < 0) {
        snprintf(notice, sizeof notice, _("Could not start Windows ISO download: %.120s"), why);
        return;
    }
    winiso_job = id;
    vm_copy(winiso_job_lang, sizeof winiso_job_lang, winiso_lang_sel);
    snprintf(notice, sizeof notice,
             _("Started Windows 11 ISO download (%s, job #%d). Closing Settings keeps it running."),
             winiso_lang_sel, id);
}

static void winiso_check_service_row(void)
{
    if (access(LOCACHY_WINISO, X_OK) != 0) {
        snprintf(notice, sizeof notice, _("%s is not installed."), "locachy-winiso");
        return;
    }
    char *const cmd[] = {
        "/bin/sh", "-c",
        LOCACHY_WINISO " check --lang \"$1\"; s=$?; "
        "printf '\\nExit %d. Enter: back. ' \"$s\"; read -r _; exit $s",
        "sh", winiso_lang_sel, NULL
    };
    int st = in_term(cmd);
    load_winiso_status();
    snprintf(notice, sizeof notice, st == 0 ?
             _("Microsoft download check succeeded for %s.") :
             _("Microsoft download check failed for %s (exit %d)."),
             winiso_lang_sel, st);
}
