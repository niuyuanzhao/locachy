/* Interactive Compute Node Manager & Client SSH Config viewer for settings.c.
 * Included directly by settings.c after storage, SSH, and clipboard helpers. */

#ifndef LOCACHY_COMPUTE
#define LOCACHY_COMPUTE "/usr/local/bin/locachy-compute"
#endif

#define MAX_COMPUTE_ITEMS 64

struct compute_img_row {
    char name[64], imported_at[32], source[160];
};
struct compute_env_row {
    char name[64], image[64], net[24];
    int gpu;
};
struct compute_job_row {
    char job_id[64], env[64], state[24], exit_code[16], cmd[128];
};
struct compute_vm_row {
    char name[64], ssh_user[64];
    int running, memory_mb, cpus, ssh_port;
};

struct compute_snapshot {
    int loaded;
    int service_enabled;
    int daemon_running;
    int port;
    int bind_tb;
    int gpu_enabled;
    char storage_volume[40];
    int storage_mounted;
    char storage_base[160];
    unsigned long long storage_free;
    unsigned long long storage_total;
    char storage_error[160];
    int gpu_devices;
    int gpu_libs;
    char nvidia_version[48];
    int paired_peers;
    char ssh_host[80];
    char ssh_user[64];
    int ssh_port;
    char ssh_prefix[64];
    struct compute_img_row images[MAX_COMPUTE_ITEMS];
    int nimages;
    struct compute_env_row envs[MAX_COMPUTE_ITEMS];
    int nenvs;
    struct compute_job_row jobs[MAX_COMPUTE_ITEMS];
    int njobs, njobs_running;
    struct compute_vm_row vms[MAX_COMPUTE_ITEMS];
    int nvms, nvms_running;
};

static struct compute_snapshot compute_snap;

static void compute_copy_field(char *dst, size_t size, const char *src)
{
    if (!size) return;
    snprintf(dst, size, "%s", src ? src : "");
}

static void compute_parse_status_tsv(const char *tsv)
{
    memset(&compute_snap, 0, sizeof compute_snap);
    compute_snap.port = 27184;
    compute_snap.gpu_enabled = 1;
    compute_snap.storage_mounted = 1;
    compute_snap.paired_peers = -1;
    compute_snap.ssh_port = 22;
    if (!tsv || !tsv[0]) return;

    char *copy = strdup(tsv);
    if (!copy) return;
    char *line = copy;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        size_t llen = strlen(line);
        if (llen > 0 && line[llen - 1] == '\r') line[--llen] = '\0';

        char *cols[8] = { 0 };
        int ncol = 0;
        char *p = line;
        while (ncol < 8) {
            cols[ncol++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = '\0';
            p = tab + 1;
        }

        if (ncol >= 3 && strcmp(cols[0], "meta") == 0) {
            compute_snap.loaded = 1;
            const char *k = cols[1], *v = cols[2];
            if (strcmp(k, "service_enabled") == 0) compute_snap.service_enabled = atoi(v) != 0;
            else if (strcmp(k, "daemon_running") == 0) compute_snap.daemon_running = atoi(v) != 0;
            else if (strcmp(k, "port") == 0 && atoi(v) > 0) compute_snap.port = atoi(v);
            else if (strcmp(k, "bind_tb") == 0) compute_snap.bind_tb = atoi(v) != 0;
            else if (strcmp(k, "gpu_enabled") == 0) compute_snap.gpu_enabled = atoi(v) != 0;
            else if (strcmp(k, "storage_volume") == 0) compute_copy_field(compute_snap.storage_volume, sizeof compute_snap.storage_volume, v);
            else if (strcmp(k, "storage_mounted") == 0) compute_snap.storage_mounted = atoi(v) != 0;
            else if (strcmp(k, "storage_base") == 0) compute_copy_field(compute_snap.storage_base, sizeof compute_snap.storage_base, v);
            else if (strcmp(k, "storage_free") == 0) compute_snap.storage_free = strtoull(v, NULL, 10);
            else if (strcmp(k, "storage_total") == 0) compute_snap.storage_total = strtoull(v, NULL, 10);
            else if (strcmp(k, "storage_error") == 0) compute_copy_field(compute_snap.storage_error, sizeof compute_snap.storage_error, v);
            else if (strcmp(k, "gpu_devices") == 0) compute_snap.gpu_devices = atoi(v);
            else if (strcmp(k, "gpu_libs") == 0) compute_snap.gpu_libs = atoi(v);
            else if (strcmp(k, "nvidia_version") == 0) compute_copy_field(compute_snap.nvidia_version, sizeof compute_snap.nvidia_version, v);
            else if (strcmp(k, "paired_peers") == 0) compute_snap.paired_peers = atoi(v);
            else if (strcmp(k, "ssh_host") == 0) compute_copy_field(compute_snap.ssh_host, sizeof compute_snap.ssh_host, v);
            else if (strcmp(k, "ssh_user") == 0) compute_copy_field(compute_snap.ssh_user, sizeof compute_snap.ssh_user, v);
            else if (strcmp(k, "ssh_port") == 0 && atoi(v) > 0) compute_snap.ssh_port = atoi(v);
            else if (strcmp(k, "ssh_prefix") == 0) compute_copy_field(compute_snap.ssh_prefix, sizeof compute_snap.ssh_prefix, v);
        } else if (ncol >= 4 && strcmp(cols[0], "image") == 0 && compute_snap.nimages < MAX_COMPUTE_ITEMS) {
            struct compute_img_row *r = &compute_snap.images[compute_snap.nimages++];
            compute_copy_field(r->name, sizeof r->name, cols[1]);
            compute_copy_field(r->imported_at, sizeof r->imported_at, cols[2]);
            compute_copy_field(r->source, sizeof r->source, cols[3]);
        } else if (ncol >= 5 && strcmp(cols[0], "env") == 0 && compute_snap.nenvs < MAX_COMPUTE_ITEMS) {
            struct compute_env_row *r = &compute_snap.envs[compute_snap.nenvs++];
            compute_copy_field(r->name, sizeof r->name, cols[1]);
            compute_copy_field(r->image, sizeof r->image, cols[2]);
            r->gpu = atoi(cols[3]) != 0;
            compute_copy_field(r->net, sizeof r->net, cols[4]);
        } else if (ncol >= 6 && strcmp(cols[0], "job") == 0 && compute_snap.njobs < MAX_COMPUTE_ITEMS) {
            struct compute_job_row *r = &compute_snap.jobs[compute_snap.njobs++];
            compute_copy_field(r->job_id, sizeof r->job_id, cols[1]);
            compute_copy_field(r->env, sizeof r->env, cols[2]);
            compute_copy_field(r->state, sizeof r->state, cols[3]);
            compute_copy_field(r->exit_code, sizeof r->exit_code, cols[4]);
            compute_copy_field(r->cmd, sizeof r->cmd, cols[5]);
            if (strcmp(r->state, "running") == 0) compute_snap.njobs_running++;
        } else if (ncol >= 7 && strcmp(cols[0], "vm") == 0 && compute_snap.nvms < MAX_COMPUTE_ITEMS) {
            struct compute_vm_row *r = &compute_snap.vms[compute_snap.nvms++];
            compute_copy_field(r->name, sizeof r->name, cols[1]);
            r->running = atoi(cols[2]) != 0;
            r->memory_mb = atoi(cols[3]);
            r->cpus = atoi(cols[4]);
            r->ssh_port = atoi(cols[5]);
            compute_copy_field(r->ssh_user, sizeof r->ssh_user, cols[6]);
            if (r->running) compute_snap.nvms_running++;
        }

        line = nl ? nl + 1 : NULL;
    }
    free(copy);
}

static void load_compute_status(void)
{
    static char buf[32768];
    buf[0] = '\0';
    if (access(LOCACHY_COMPUTE, X_OK) != 0) {
        compute_parse_status_tsv("");
        return;
    }
    char *const argv[] = { LOCACHY_COMPUTE, "status", "--tsv", NULL };
    command_output(argv, buf, sizeof buf);
    compute_parse_status_tsv(buf);
}

/* Direct execution helper for non-interactive locachy-compute mutations;
 * tests may override via SETTINGS_COMPUTE_RUN. */
static int compute_exec(char *const argv[], char *out, size_t out_size);
#ifndef SETTINGS_COMPUTE_RUN
#define SETTINGS_COMPUTE_RUN
static int compute_exec(char *const argv[], char *out, size_t out_size)
{
    int p[2] = { -1, -1 };
    size_t len = 0;
    int status = -1;
    if (out && out_size > 0) out[0] = '\0';
    if (pipe2(p, O_CLOEXEC) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            if (null_fd > 2) close(null_fd);
        }
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    struct pollfd wait = { .fd = p[0], .events = POLLIN };
    int timed_out = 0;
    for (;;) {
        int pr = poll(&wait, 1, 15000);
        if (pr < 0 && errno == EINTR) continue;
        if (pr == 0) {
            timed_out = 1;
            break;
        }
        if (pr < 0) break;
        char discard[256];
        char *dst = (out && out_size > 1 && len < out_size - 1) ? out + len : discard;
        size_t cap = (out && out_size > 1 && len < out_size - 1) ? out_size - 1 - len : sizeof discard;
        ssize_t n = read(p[0], dst, cap);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (dst != discard) len += (size_t)n;
    }
    close(p[0]);
    if (out && out_size > 0) {
        out[len] = '\0';
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
    }
    if (timed_out) kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return (!timed_out && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}
#endif

static int compute_storage_vol_index(void)
{
    const char *want = compute_storage_cfg[0] ? compute_storage_cfg : compute_snap.storage_volume;
    if (!want[0]) return -1;
    for (int i = 0; i < nstorage_vols; i++) {
        if (strcmp(storage_vols[i].name, want) == 0) return i;
    }
    return -1;
}

static void compute_mount_storage_action(void)
{
    load_storage_status();
    int vi = compute_storage_vol_index();
    if (vi < 0) {
        snprintf(notice, sizeof notice, "%s",
                 _("No secondary compute storage volume selected (using root disk /var/lib/locachy/compute)."));
        return;
    }
    storage_toggle_mount(vi);
}

static int compute_valid_name_input(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len > 64 || !isalnum((unsigned char)s[0])) return 0;
    return strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == len;
}

/* Scrollable full-screen text viewer with clipboard copy (used for Client SSH
 * config and Compute Job logs). */
static void compute_view_text_screen(const char *title, const char *text, const char *copy_label)
{
    const char *lines[1024];
    int lens[1024];
    int nlines = 0;
    const char *p = text ? text : "";
    while (*p && nlines < 1024) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len > 0 && p[len - 1] == '\r') len--;
        lines[nlines] = p;
        lens[nlines] = len;
        nlines++;
        p = nl ? nl + 1 : p + strlen(p);
    }
    int top = 0;
    char rowbuf[320];
    for (;;) {
        int cols = screen_cols(), rows = screen_rows();
        int body_y = 3;
        int height = rows - body_y - 4;
        if (height < 1) height = 1;
        int max_top = nlines > height ? nlines - height : 0;
        if (top < 0) top = 0;
        if (top > max_top) top = max_top;

        screen_fill(0, 0, cols, rows, BG);
        screen_bar(title, NULL);
        ui_hits_clear();
        if (nlines == 0) {
            screen_text(3, body_y, _("(empty)"), DIM, BG);
        } else {
            for (int r = 0; r < height && top + r < nlines; r++) {
                int len = lens[top + r];
                if (len >= (int)sizeof rowbuf) len = (int)sizeof rowbuf - 1;
                memcpy(rowbuf, lines[top + r], (size_t)len);
                rowbuf[len] = '\0';
                screen_text(2, body_y + r, rowbuf, FG, BG);
            }
        }
        if (notice[0]) {
            screen_text(2, rows - 4, notice, ACCENT_TEXT, BG);
        }
        /* TRANSLATORS: Up/Down, c and Esc are key names; keep them. */
        screen_text(2, rows - 3, copy_label ?
                    _("Up/Down/PgUp/PgDn: scroll   c: copy to clipboard   Esc: back") :
                    _("Up/Down/PgUp/PgDn: scroll   Esc: back"), ACCENT_TEXT, BG);
        settings_back_button(rows - 2);
        screen_flush();

        int key = key_get(), mods = mods_now(), act = key_list_action(key, mods);
        if (close_read(key)) return;
        if (key == K_REPAINT || key == K_SHOWN) continue;
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (settings_back_clicked(e)) return;
            if (e->wheel) top -= e->wheel * 3;
            continue;
        }
        if (act == ACT_HELP) {
            help_screen();
            if (close_due()) return;
            continue;
        }
        if (act == ACT_CANCEL) return;
        if (key == K_UP) top--;
        else if (key == K_DOWN) top++;
        else if (key == K_PGUP) top -= height;
        else if (key == K_PGDN) top += height;
        else if (key == K_BOL) top = 0;
        else if (key == K_EOL) top = max_top;
        else if (copy_label && (key == 'c' || key == 'C' || key == '\n')) {
            if (clipboard_set && clipboard_set(0, text, strlen(text)) == 0) {
                snprintf(notice, sizeof notice, _("Copied %s to clipboard."), copy_label);
            } else {
                snprintf(notice, sizeof notice, "%s", _("Clipboard service is not running."));
            }
        }
    }
}

static void compute_view_ssh_config(void)
{
    static char cfg_buf[16384];
    cfg_buf[0] = '\0';
    if (access(LOCACHY_COMPUTE, X_OK) != 0) {
        snprintf(notice, sizeof notice, _("%s is not installed."), "locachy-compute");
        return;
    }
    char *const argv[] = { LOCACHY_COMPUTE, "ssh-config", NULL };
    int rc = compute_exec(argv, cfg_buf, sizeof cfg_buf);
    if (rc != 0 || !cfg_buf[0]) {
        /* If compute_storage is unmounted, show a clean base SSH stanza instead of failing blank. */
        load_compute_status();
        const char *prefix = compute_snap.ssh_prefix[0] ? compute_snap.ssh_prefix : "locachy";
        const char *host = compute_snap.ssh_host[0] ? compute_snap.ssh_host : "<node-ip>";
        const char *user = compute_snap.ssh_user[0] ? compute_snap.ssh_user : "user";
        int port = compute_snap.ssh_port > 0 ? compute_snap.ssh_port : 22;
        snprintf(cfg_buf, sizeof cfg_buf,
                 "# Client SSH config (append to ~/.ssh/config or ~/.ssh/config.d/locachy-%s.conf)\n"
                 "# Note: %s\n\n"
                 "Host %s\n"
                 "    HostName %s\n"
                 "    User %s\n"
                 "    Port %d\n",
                 prefix,
                 compute_snap.storage_error[0] ? compute_snap.storage_error : "env/VM entries unavailable",
                 prefix, host, user, port);
    }
    notice[0] = '\0';
    compute_view_text_screen(_("Compute Client SSH Config"), cfg_buf, _("SSH config"));
    if (!notice[0]) {
        snprintf(notice, sizeof notice, "%s", _("Closed client SSH config."));
    }
}

static void compute_import_image_interactive(void)
{
    char name[68] = "", src[256] = "";
    if (!prompt(_("Image name (e.g. pytorch-cu12): "), name, sizeof name) || !name[0]) {
        snprintf(notice, sizeof notice, "%s", _("Image import cancelled."));
        return;
    }
    if (!compute_valid_name_input(name)) {
        snprintf(notice, sizeof notice, "%s", _("Invalid image name: letters, digits, . _ - only."));
        return;
    }
    if (!prompt(_("Source rootfs path (directory or .tar/.tar.zst): "), src, sizeof src) || !src[0]) {
        snprintf(notice, sizeof notice, "%s", _("Image import cancelled."));
        return;
    }
    if (src[0] != '/') {
        snprintf(notice, sizeof notice, "%s", _("Source path must be an absolute path."));
        return;
    }
    char *const cmd[] = {
        "/bin/sh", "-c",
        "/usr/bin/sudo " LOCACHY_COMPUTE " image import \"$1\" \"$2\"; s=$?; "
        "printf '\\nExit %d. Enter: back to Compute Manager. ' \"$s\"; read -r _; exit $s",
        "sh", name, src, NULL
    };
    int st = in_term(cmd);
    load_compute_status();
    snprintf(notice, sizeof notice, st == 0 ? _("Imported image %.40s.") : _("Image import failed for %.40s."), name);
}

static void compute_create_env_interactive(void)
{
    if (compute_snap.nimages == 0) {
        snprintf(notice, sizeof notice, "%s", _("Import a base rootfs image first (Images tab or Overview)."));
        return;
    }
    char name[68] = "", img[68] = "";
    snprintf(img, sizeof img, "%s", compute_snap.images[0].name);
    if (!prompt(_("Environment name (e.g. train-1): "), name, sizeof name) || !name[0]) {
        snprintf(notice, sizeof notice, "%s", _("Environment creation cancelled."));
        return;
    }
    if (!compute_valid_name_input(name)) {
        snprintf(notice, sizeof notice, "%s", _("Invalid environment name: letters, digits, . _ - only."));
        return;
    }
    if (!prompt(_("Base image name: "), img, sizeof img) || !img[0]) {
        snprintf(notice, sizeof notice, "%s", _("Environment creation cancelled."));
        return;
    }
    if (!compute_valid_name_input(img)) {
        snprintf(notice, sizeof notice, "%s", _("Invalid image name."));
        return;
    }
    char out[256] = "";
    char *const argv[] = {
        LOCACHY_COMPUTE, "env", "create", name, "--image", img,
        compute_gpu ? "--gpu" : "--no-gpu", "--net", "none", NULL
    };
    int rc = compute_exec(argv, out, sizeof out);
    load_compute_status();
    if (rc == 0) {
        snprintf(notice, sizeof notice, _("Created environment %.40s from %.40s."), name, img);
    } else {
        snprintf(notice, sizeof notice, _("Create env failed: %.120s"), out[0] ? out : _("check storage and image name"));
    }
}

static void compute_pair_client_interactive(void)
{
    if (access(LOCACHY_PAIR, X_OK) != 0) {
        snprintf(notice, sizeof notice, _("%s is not installed."), "locachy-pair");
        return;
    }
    char name[68] = "";
    if (!prompt(_("Client peer name (e.g. macbook): "), name, sizeof name) || !name[0]) {
        snprintf(notice, sizeof notice, "%s", _("Pairing cancelled."));
        return;
    }
    if (!compute_valid_name_input(name)) {
        snprintf(notice, sizeof notice, "%s", _("Invalid peer name: letters, digits, . _ - only."));
        return;
    }
    char *const cmd[] = {
        "/bin/sh", "-c",
        "/usr/bin/sudo " LOCACHY_PAIR " add --scope compute --name \"$1\"; s=$?; "
        "printf '\\nCopy the token/bundle above onto your client. Enter: back. '; read -r _; exit $s",
        "sh", name, NULL
    };
    int st = in_term(cmd);
    load_paired_peers();
    load_compute_status();
    snprintf(notice, sizeof notice, st == 0 ? _("Paired compute client %.40s.") : _("Client pairing did not finish."), name);
}

enum { CTAB_OVERVIEW, CTAB_ENVS, CTAB_JOBS, CTAB_VMS, CTAB_IMAGES, NCTABS };

static int compute_tab_count(int tab)
{
    if (tab == CTAB_OVERVIEW) return 6;
    if (tab == CTAB_ENVS) return 1 + compute_snap.nenvs;
    if (tab == CTAB_JOBS) return compute_snap.njobs;
    if (tab == CTAB_VMS) return compute_snap.nvms;
    return 1 + compute_snap.nimages;
}

static void manage_compute_node(void)
{
    if (access(LOCACHY_COMPUTE, X_OK) != 0) {
        snprintf(notice, sizeof notice, _("%s is not installed."), "locachy-compute");
        return;
    }
    load_storage_status();
    load_compute_status();

    int tab = CTAB_OVERVIEW;
    int sel = 0, top = 0;
    int confirm_key = 0, confirm_tab = -1, confirm_sel = -1;
    char line[320], sz1[24], sz2[24];
    notice[0] = '\0';

    for (;;) {
        int cols = screen_cols(), rows = screen_rows();
        int total = compute_tab_count(tab);
        int list_y = 8;
        int height = rows - list_y - 4;
        if (height < 1) height = 1;
        if (total == 0) {
            sel = top = 0;
        } else {
            if (sel >= total) sel = total - 1;
            if (sel < 0) sel = 0;
            if (sel < top) top = sel;
            if (sel >= top + height) top = sel - height + 1;
        }

        screen_fill(0, 0, cols, rows, BG);
        screen_bar(_("Compute Node Manager"), NULL);
        ui_hits_clear();

        /* Top summary box (separate region: API, storage, GPU). */
        screen_fill(0, 2, cols, 4, SELECTION);
        snprintf(line, sizeof line, _("API: %s (port %d, %s)   Paired: %d   SSH: ssh %.32s -p %d"),
                 compute_on ? (compute_snap.daemon_running ? _("ON / running") : _("ON / starting")) : _("OFF"),
                 compute_snap.port,
                 compute_bind_tb ? _("Tailscale+TB") : _("Tailscale"),
                 compute_snap.paired_peers >= 0 ? compute_snap.paired_peers : 0,
                 compute_snap.ssh_prefix[0] ? compute_snap.ssh_prefix : "locachy",
                 compute_snap.ssh_port);
        screen_text(2, 2, line, FG, SELECTION);

        const char *svol = compute_storage_cfg[0] ? compute_storage_cfg : compute_snap.storage_volume;
        if (!svol[0]) {
            if (compute_snap.storage_total > 0) {
                format_bytes_iec(compute_snap.storage_free, sz1, sizeof sz1);
                format_bytes_iec(compute_snap.storage_total, sz2, sizeof sz2);
                snprintf(line, sizeof line, _("Storage: root disk (%s free / %s)   base: %.80s"),
                         sz1, sz2, compute_snap.storage_base);
            } else {
                snprintf(line, sizeof line, _("Storage: root disk (/var/lib/locachy/compute)"));
            }
        } else if (compute_snap.storage_mounted) {
            format_bytes_iec(compute_snap.storage_free, sz1, sizeof sz1);
            format_bytes_iec(compute_snap.storage_total, sz2, sizeof sz2);
            snprintf(line, sizeof line, _("Storage: %.32s [MOUNTED]  %s free / %s  (%.70s)"),
                     svol, sz1, sz2, compute_snap.storage_base);
        } else {
            snprintf(line, sizeof line, _("Storage: %.32s [UNMOUNTED]  press m to mount"), svol);
        }
        screen_text(2, 3, line, compute_snap.storage_mounted ? FG : ACCENT_TEXT, SELECTION);

        if (compute_snap.nvidia_version[0]) {
            snprintf(line, sizeof line, _("GPU: passthrough %s   NVIDIA %s   (%d devs, %d CUDA libs)"),
                     compute_gpu ? _("ON") : _("OFF"), compute_snap.nvidia_version,
                     compute_snap.gpu_devices, compute_snap.gpu_libs);
        } else if (compute_snap.gpu_devices > 0) {
            snprintf(line, sizeof line, _("GPU: passthrough %s   %d GPU device(s), %d NVIDIA libs"),
                     compute_gpu ? _("ON") : _("OFF"), compute_snap.gpu_devices, compute_snap.gpu_libs);
        } else {
            snprintf(line, sizeof line, _("GPU: passthrough %s   no NVIDIA driver loaded (see Input & Output > NVIDIA GPU)"),
                     compute_gpu ? _("ON") : _("OFF"));
        }
        screen_text(2, 4, line, FG, SELECTION);

        snprintf(line, sizeof line, _("Inventory: %d images   %d envs   %d/%d jobs running   %d/%d VMs running"),
                 compute_snap.nimages, compute_snap.nenvs,
                 compute_snap.njobs_running, compute_snap.njobs,
                 compute_snap.nvms_running, compute_snap.nvms);
        screen_text(2, 5, line, DIM, SELECTION);

        /* Tab strip (row 6). */
        char tlabels[NCTABS][32];
        snprintf(tlabels[CTAB_OVERVIEW], sizeof tlabels[0], " 1:%s ", _("Overview"));
        snprintf(tlabels[CTAB_ENVS], sizeof tlabels[0], " 2:%s(%d) ", _("Envs"), compute_snap.nenvs);
        snprintf(tlabels[CTAB_JOBS], sizeof tlabels[0], " 3:%s(%d/%d) ", _("Jobs"),
                 compute_snap.njobs_running, compute_snap.njobs);
        snprintf(tlabels[CTAB_VMS], sizeof tlabels[0], " 4:%s(%d/%d) ", _("VMs"),
                 compute_snap.nvms_running, compute_snap.nvms);
        snprintf(tlabels[CTAB_IMAGES], sizeof tlabels[0], " 5:%s(%d) ", _("Images"), compute_snap.nimages);
        int tx = 2;
        for (int t = 0; t < NCTABS; t++) {
            int tw = ui_cells(tlabels[t]);
            int on = t == tab;
            screen_fill(tx, 6, tw, 1, on ? ACCENT : BG);
            screen_text(tx, 6, tlabels[t], on ? ON_ACCENT : FG, on ? ACCENT : BG);
            ui_hit(tx, 6, tw, 1, 1000 + t);
            tx += tw + 1;
        }

        /* List content (rows 8 .. rows-5). */
        if (total == 0) {
            const char *empty = tab == CTAB_JOBS ? _("No compute jobs recorded yet.") :
                                tab == CTAB_VMS ? _("No compute micro-VMs created yet (locachy-compute vm create).") :
                                _("Nothing here yet.");
            screen_text(4, list_y, empty, DIM, BG);
        } else {
            for (int r = 0; r < height && top + r < total; r++) {
                int idx = top + r, y = list_y + r, on = idx == sel;
                line[0] = '\0';
                if (tab == CTAB_OVERVIEW) {
                    if (idx == 0) {
                        if (svol[0]) {
                            snprintf(line, sizeof line, compute_snap.storage_mounted ?
                                     _("[ Unmount compute storage volume '%.32s' ]") :
                                     _("[ Mount compute storage volume '%.32s' ]"), svol);
                        } else {
                            snprintf(line, sizeof line, "%s", _("[ Storage: using root disk (configure under System > Storage Volumes) ]"));
                        }
                    } else if (idx == 1) {
                        snprintf(line, sizeof line, "%s", _("[ + Import rootfs image (tarball or directory) ]"));
                    } else if (idx == 2) {
                        snprintf(line, sizeof line, "%s", _("[ + Create compute environment ]"));
                    } else if (idx == 3) {
                        snprintf(line, sizeof line, "%s", _("[ + Pair remote client (mTLS token) ]"));
                    } else if (idx == 4) {
                        snprintf(line, sizeof line, "%s", _("[ View / copy client SSH config ]"));
                    } else {
                        snprintf(line, sizeof line, "%s", _("[ Manage paired devices (locachy-pair) ]"));
                    }
                } else if (tab == CTAB_ENVS) {
                    if (idx == 0) {
                        snprintf(line, sizeof line, "%s", _("[ + Create compute environment ]"));
                    } else {
                        const struct compute_env_row *e = &compute_snap.envs[idx - 1];
                        snprintf(line, sizeof line, "%-20.20s  img=%-20.20s  GPU:%-3s  net=%-12.12s",
                                 e->name, e->image, e->gpu ? _("ON") : _("OFF"), e->net);
                    }
                } else if (tab == CTAB_JOBS) {
                    const struct compute_job_row *j = &compute_snap.jobs[idx];
                    snprintf(line, sizeof line, "%-16.16s  %-14.14s  %-9.9s  exit=%-4.4s  %.60s",
                             j->job_id, j->env, j->state, j->exit_code[0] ? j->exit_code : "-", j->cmd);
                } else if (tab == CTAB_VMS) {
                    const struct compute_vm_row *v = &compute_snap.vms[idx];
                    if (v->ssh_port > 0) {
                        snprintf(line, sizeof line, "%-18.18s  %-7s  %5d MiB  %2d vCPU  ssh=127.0.0.1:%d",
                                 v->name, v->running ? _("RUNNING") : _("stopped"),
                                 v->memory_mb, v->cpus, v->ssh_port);
                    } else {
                        snprintf(line, sizeof line, "%-18.18s  %-7s  %5d MiB  %2d vCPU",
                                 v->name, v->running ? _("RUNNING") : _("stopped"),
                                 v->memory_mb, v->cpus);
                    }
                } else {
                    if (idx == 0) {
                        snprintf(line, sizeof line, "%s", _("[ + Import rootfs image (tarball or directory) ]"));
                    } else {
                        const struct compute_img_row *im = &compute_snap.images[idx - 1];
                        snprintf(line, sizeof line, "%-20.20s  %-20.20s  %.70s",
                                 im->name, im->imported_at, im->source);
                    }
                }
                screen_fill(0, y, cols, 1, on ? ACCENT : BG);
                screen_text(3, y, line, on ? ON_ACCENT : FG, on ? ACCENT : BG);
                ui_hit(0, y, cols, 1, idx);
            }
        }

        if (notice[0]) {
            screen_text(2, rows - 4, notice, ACCENT_TEXT, BG);
        }
        const char *hint =
            tab == CTAB_ENVS ? _("1-5/Tab: tabs   Enter: shell/create   x: reset   Delete: remove   r: refresh   Esc: back") :
            tab == CTAB_JOBS ? _("1-5/Tab: tabs   Enter: view log   s: stop job   r: refresh   Esc: back") :
            tab == CTAB_VMS ? _("1-5/Tab: tabs   Enter: start/shell   s: stop   x: reset   Delete: remove   r: refresh   Esc: back") :
            tab == CTAB_IMAGES ? _("1-5/Tab: tabs   Enter: import   Delete: remove   r: refresh   Esc: back") :
            _("1-5/Tab: tabs   Enter: run action   m: mount storage   c: SSH config   r: refresh   Esc: back");
        screen_text(2, rows - 3, hint, ACCENT_TEXT, BG);
        settings_back_button(rows - 2);
        screen_flush();

        int key = key_get(), mods = mods_now(), act = key_list_action(key, mods);
        if (close_read(key)) return;
        if (key == K_REPAINT || key == K_SHOWN) continue;
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (settings_back_clicked(e)) return;
            if (e->wheel && total > 0) {
                sel -= e->wheel;
                if (sel < 0) sel = 0;
                if (sel >= total) sel = total - 1;
                confirm_key = 0;
                continue;
            }
            if (e->button == BTN_LEFT && e->pressed) {
                int hit = ui_hit_at(e->col, e->row);
                if (hit >= 1000 && hit < 1000 + NCTABS) {
                    tab = hit - 1000;
                    sel = top = 0;
                    confirm_key = 0;
                    continue;
                }
                if (hit >= 0 && hit < total) {
                    if (hit != sel) confirm_key = 0;
                    sel = hit;
                    if (e->clicks == 2) key = '\n';
                    else continue;
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (act == ACT_HELP) {
            help_screen();
            if (close_due()) return;
            continue;
        }
        if (act == ACT_CANCEL) {
            snprintf(notice, sizeof notice, _("Compute Node: %d images, %d envs, %d jobs, %d VMs."),
                     compute_snap.nimages, compute_snap.nenvs, compute_snap.njobs, compute_snap.nvms);
            return;
        }
        if (act == ACT_DELETE || act == ACT_DELETE_PERMANENT) {
            key = K_DEL;
            act = ACT_NONE;
        }
        if (act != ACT_NONE || (key >= ' ' && key < 0x7F && (mods & (KEYS_ALT | KEYS_META)))) {
            continue;
        }

        int ckey = (key == 'X') ? 'x' : key;
        if (ckey != confirm_key || tab != confirm_tab || sel != confirm_sel) {
            confirm_key = 0;
            confirm_tab = confirm_sel = -1;
        }

        if (key == '\t' || key == K_RIGHT) {
            tab = (tab + 1) % NCTABS;
            sel = top = 0;
            confirm_key = 0;
        } else if (key == K_LEFT) {
            tab = (tab + NCTABS - 1) % NCTABS;
            sel = top = 0;
            confirm_key = 0;
        } else if (key >= '1' && key <= '5') {
            tab = key - '1';
            sel = top = 0;
            confirm_key = 0;
        } else if (key == K_UP && sel > 0) {
            sel--;
        } else if (key == K_DOWN && sel + 1 < total) {
            sel++;
        } else if (key == K_PGUP && total > 0) {
            sel = sel > height ? sel - height : 0;
        } else if (key == K_PGDN && total > 0) {
            sel = sel + height < total ? sel + height : total - 1;
        } else if (key == K_BOL) {
            sel = 0;
        } else if (key == K_EOL && total > 0) {
            sel = total - 1;
        } else if (key == 'r' || key == 'R') {
            load_storage_status();
            load_compute_status();
            snprintf(notice, sizeof notice, "%s", _("Refreshed Compute Node status."));
        } else if (key == 'm' || key == 'M') {
            compute_mount_storage_action();
            load_compute_status();
        } else if (key == 'c' || key == 'C') {
            compute_view_ssh_config();
            if (close_due()) return;
        } else if (tab == CTAB_OVERVIEW && key == '\n') {
            if (sel == 0) {
                compute_mount_storage_action();
                load_compute_status();
            } else if (sel == 1) {
                compute_import_image_interactive();
            } else if (sel == 2) {
                compute_create_env_interactive();
            } else if (sel == 3) {
                compute_pair_client_interactive();
            } else if (sel == 4) {
                compute_view_ssh_config();
            } else if (sel == 5) {
                manage_paired_devices();
                load_compute_status();
            }
            if (close_due()) return;
        } else if (tab == CTAB_ENVS) {
            if (key == '\n' && sel == 0) {
                compute_create_env_interactive();
            } else if (sel > 0 && sel - 1 < compute_snap.nenvs) {
                const char *ename = compute_snap.envs[sel - 1].name;
                if (key == '\n') {
                    char *const cmd[] = {
                        "/bin/sh", "-c",
                        "/usr/bin/sudo " LOCACHY_COMPUTE " shell --kind env \"$1\"; s=$?; "
                        "printf '\\nShell exited (%d). Enter: back. ' \"$s\"; read -r _; exit $s",
                        "sh", (char *)ename, NULL
                    };
                    in_term(cmd);
                    load_compute_status();
                } else if (key == 'x' || key == 'X') {
                    if (confirm_key != 'x') {
                        confirm_key = 'x'; confirm_tab = tab; confirm_sel = sel;
                        snprintf(notice, sizeof notice, _("Press x again to wipe upperdir and reset env '%.40s'."), ename);
                    } else {
                        char out[256] = "";
                        char *const argv[] = { LOCACHY_COMPUTE, "env", "reset", (char *)ename, NULL };
                        int rc = compute_exec(argv, out, sizeof out);
                        confirm_key = 0;
                        load_compute_status();
                        snprintf(notice, sizeof notice, rc == 0 ? _("Reset environment '%.40s'.") :
                                 _("Reset failed: %.120s"), rc == 0 ? ename : (out[0] ? out : _("error")));
                    }
                } else if (key == K_DEL) {
                    if (confirm_key != K_DEL) {
                        confirm_key = K_DEL; confirm_tab = tab; confirm_sel = sel;
                        snprintf(notice, sizeof notice, _("Press Delete again to remove env '%.40s'."), ename);
                    } else {
                        char out[256] = "";
                        char *const argv[] = { LOCACHY_COMPUTE, "env", "remove", (char *)ename, NULL };
                        int rc = compute_exec(argv, out, sizeof out);
                        confirm_key = 0;
                        load_compute_status();
                        snprintf(notice, sizeof notice, rc == 0 ? _("Removed environment '%.40s'.") :
                                 _("Remove failed: %.120s"), rc == 0 ? ename : (out[0] ? out : _("error")));
                    }
                }
            }
            if (close_due()) return;
        } else if (tab == CTAB_JOBS && sel < compute_snap.njobs) {
            const char *jid = compute_snap.jobs[sel].job_id;
            if (key == '\n') {
                static char log_buf[32768];
                log_buf[0] = '\0';
                char *const argv[] = { LOCACHY_COMPUTE, "job", "log", (char *)jid, NULL };
                int rc = compute_exec(argv, log_buf, sizeof log_buf);
                char jtitle[128];
                snprintf(jtitle, sizeof jtitle, _("Job Log: %.64s"), jid);
                if (rc != 0 && !log_buf[0]) snprintf(log_buf, sizeof log_buf, "%s", _("(no log output)"));
                notice[0] = '\0';
                compute_view_text_screen(jtitle, log_buf, _("job log"));
                if (close_due()) return;
                load_compute_status();
            } else if (key == 's' || key == 'S') {
                char out[256] = "";
                char *const argv[] = { LOCACHY_COMPUTE, "job", "stop", (char *)jid, NULL };
                int rc = compute_exec(argv, out, sizeof out);
                load_compute_status();
                snprintf(notice, sizeof notice, rc == 0 ? _("Stopped job '%.48s'.") :
                         _("Stop job failed: %.120s"), rc == 0 ? jid : (out[0] ? out : _("error")));
            }
        } else if (tab == CTAB_VMS && sel < compute_snap.nvms) {
            const struct compute_vm_row *vm = &compute_snap.vms[sel];
            char vname[64];
            snprintf(vname, sizeof vname, "%s", vm->name);
            int was_running = vm->running;
            if (key == '\n') {
                if (!was_running) {
                    char out[256] = "";
                    char *const argv[] = { LOCACHY_COMPUTE, "vm", "start", vname, NULL };
                    int rc = compute_exec(argv, out, sizeof out);
                    load_compute_status();
                    snprintf(notice, sizeof notice, rc == 0 ? _("Started VM '%.40s'.") :
                             _("Start VM failed: %.120s"), rc == 0 ? vname : (out[0] ? out : _("error")));
                } else {
                    char *const cmd[] = {
                        "/bin/sh", "-c",
                        "/usr/bin/sudo " LOCACHY_COMPUTE " shell --kind vm \"$1\"; s=$?; "
                        "printf '\\nVM shell exited (%d). Enter: back. ' \"$s\"; read -r _; exit $s",
                        "sh", vname, NULL
                    };
                    in_term(cmd);
                    load_compute_status();
                }
            } else if (key == 's' || key == 'S') {
                char out[256] = "";
                char *const argv[] = { LOCACHY_COMPUTE, "vm", "stop", vname, NULL };
                int rc = compute_exec(argv, out, sizeof out);
                load_compute_status();
                snprintf(notice, sizeof notice, rc == 0 ? _("Stopped VM '%.40s'.") :
                         _("Stop VM failed: %.120s"), rc == 0 ? vname : (out[0] ? out : _("error")));
            } else if (key == 'x' || key == 'X') {
                if (confirm_key != 'x') {
                    confirm_key = 'x'; confirm_tab = tab; confirm_sel = sel;
                    snprintf(notice, sizeof notice, _("Press x again to reset CoW disk of VM '%.40s'."), vname);
                } else {
                    char out[256] = "";
                    char *const argv[] = { LOCACHY_COMPUTE, "vm", "reset", vname, NULL };
                    int rc = compute_exec(argv, out, sizeof out);
                    confirm_key = 0;
                    load_compute_status();
                    snprintf(notice, sizeof notice, rc == 0 ? _("Reset VM '%.40s'.") :
                             _("Reset VM failed: %.120s"), rc == 0 ? vname : (out[0] ? out : _("error")));
                }
            } else if (key == K_DEL) {
                if (confirm_key != K_DEL) {
                    confirm_key = K_DEL; confirm_tab = tab; confirm_sel = sel;
                    snprintf(notice, sizeof notice, _("Press Delete again to remove VM '%.40s'."), vname);
                } else {
                    char out[256] = "";
                    char *const argv[] = { LOCACHY_COMPUTE, "vm", "remove", vname, NULL };
                    int rc = compute_exec(argv, out, sizeof out);
                    confirm_key = 0;
                    load_compute_status();
                    snprintf(notice, sizeof notice, rc == 0 ? _("Removed VM '%.40s'.") :
                             _("Remove VM failed: %.120s"), rc == 0 ? vname : (out[0] ? out : _("error")));
                }
            }
            if (close_due()) return;
        } else if (tab == CTAB_IMAGES) {
            if (key == '\n' && sel == 0) {
                compute_import_image_interactive();
            } else if (key == K_DEL && sel > 0 && sel - 1 < compute_snap.nimages) {
                const char *iname = compute_snap.images[sel - 1].name;
                if (confirm_key != K_DEL) {
                    confirm_key = K_DEL; confirm_tab = tab; confirm_sel = sel;
                    snprintf(notice, sizeof notice, _("Press Delete again to remove image '%.40s'."), iname);
                } else {
                    char out[256] = "";
                    char *const argv[] = { LOCACHY_COMPUTE, "image", "remove", (char *)iname, NULL };
                    int rc = compute_exec(argv, out, sizeof out);
                    confirm_key = 0;
                    load_compute_status();
                    snprintf(notice, sizeof notice, rc == 0 ? _("Removed image '%.40s'.") :
                             _("Remove image failed: %.120s"), rc == 0 ? iname : (out[0] ? out : _("error")));
                }
            }
            if (close_due()) return;
        }
    }
}
