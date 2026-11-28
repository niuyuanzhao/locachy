/* Interactive Compute Node Manager, Image Downloader, Container Settings Form,
 * and Client SSH Config viewer for settings.c.
 * Included directly by settings.c after storage, SSH, and clipboard helpers. */

#ifndef LOCACHY_COMPUTE
#define LOCACHY_COMPUTE "/usr/local/bin/locachy-compute"
#endif

#define MAX_COMPUTE_ITEMS 64
#define MAX_ENV_MOUNTS 12

struct compute_img_row {
    char name[64], imported_at[32], source[160];
};
struct compute_env_row {
    char name[64], image[64], net[24];
    char volumes[240], mounts[240], memory_max[32], cpu_max[32], workdir[120];
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
    snprintf(dst, size, "%.*s", (int)(size - 1), src ? src : "");
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

        char *cols[12] = { 0 };
        int ncol = 0;
        char *p = line;
        while (ncol < 12) {
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
            compute_copy_field(r->volumes, sizeof r->volumes, ncol > 5 ? cols[5] : "");
            compute_copy_field(r->mounts, sizeof r->mounts, ncol > 6 ? cols[6] : "");
            compute_copy_field(r->memory_max, sizeof r->memory_max, ncol > 7 ? cols[7] : "");
            compute_copy_field(r->cpu_max, sizeof r->cpu_max, ncol > 8 ? cols[8] : "");
            compute_copy_field(r->workdir, sizeof r->workdir, (ncol > 9 && cols[9][0]) ? cols[9] : "/");
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

static void compute_open_env_shell(const char *ename)
{
    char *const cmd[] = {
        "/bin/sh", "-c",
        "/usr/bin/sudo " LOCACHY_COMPUTE " shell --kind env \"$1\"; s=$?; "
        "printf '\\nShell exited (%d). Enter: back. ' \"$s\"; read -r _; exit $s",
        "sh", (char *)ename, NULL
    };
    in_term(cmd);
    load_compute_status();
}

static void compute_test_env_gpu(const char *ename)
{
    char *const cmd[] = {
        "/bin/sh", "-c",
        "printf '==> Running nvidia-smi inside compute container %s ...\\n\\n' \"$1\"; "
        "/usr/bin/sudo " LOCACHY_COMPUTE " run \"$1\" -- nvidia-smi; s=$?; "
        "printf '\\nExit %d. Enter: back to Compute Manager. ' \"$s\"; read -r _; exit $s",
        "sh", (char *)ename, NULL
    };
    int st = in_term(cmd);
    load_compute_status();
    snprintf(notice, sizeof notice, st == 0 ?
             _("GPU test (nvidia-smi) succeeded in '%.40s'.") :
             _("GPU test failed in '%.40s' (check driver & GPU passthrough)."), ename);
}

struct compute_img_preset {
    const char *id;
    const char *title;
    const char *detail;
};

static const struct compute_img_preset compute_img_presets[] = {
    { "ubuntu-24.04", "Ubuntu 24.04 LTS Base (~30 MB)", "Official cdimage.ubuntu.com tarball, SHA-256 verified" },
    { "ubuntu-22.04", "Ubuntu 22.04 LTS Base (~30 MB)", "Official cdimage.ubuntu.com tarball, SHA-256 verified" },
    { "arch-linux",   "Arch Linux Bootstrap (~170 MB)", "Official Arch mirror .tar.zst, SHA-256 verified" },
    { "alpine-stable","Alpine Linux Minirootfs (~4 MB)", "Official dl-cdn.alpinelinux.org tarball, SHA-256 verified" },
};
#define NCOMPUTE_IMG_PRESETS ((int)(sizeof compute_img_presets / sizeof compute_img_presets[0]))

/* Interactive Base Rootfs Image Downloader & Importer.
 * Returns 1 when an image was downloaded or imported (storing its name in
 * out_name when non-NULL), 0 if cancelled. */
static int compute_fetch_image_screen(char *out_name, size_t out_size)
{
    if (out_name && out_size) out_name[0] = '\0';
    int sel = 0;
    const int total = NCOMPUTE_IMG_PRESETS + 2; /* presets + custom URL + local path */
    char line[320];

    for (;;) {
        int cols = screen_cols(), rows = screen_rows();
        screen_fill(0, 0, cols, rows, BG);
        screen_bar(_("Download / Import Base Rootfs Image"), NULL);
        ui_hits_clear();

        screen_fill(0, 2, cols, 3, SELECTION);
        screen_text(2, 2, _("Native Linux base rootfs (no Docker daemon; downloads over HTTPS + SHA-256 check)."), FG, SELECTION);
        const char *svol = compute_storage_cfg[0] ? compute_storage_cfg : compute_snap.storage_volume;
        snprintf(line, sizeof line, _("Target storage: %s   (imported images: %d)"),
                 svol[0] ? svol : _("root disk (/var/lib/locachy/compute)"), compute_snap.nimages);
        screen_text(2, 3, line, DIM, SELECTION);
        if (!compute_snap.storage_mounted && svol[0]) {
            screen_text(2, 4, _("Warning: compute storage volume is UNMOUNTED — mount it before downloading."), ACCENT_TEXT, SELECTION);
        } else {
            screen_text(2, 4, _("Pick an official distribution preset below, or import a custom URL / local archive."), DIM, SELECTION);
        }

        int list_y = 6;
        for (int i = 0; i < total; i++) {
            int y = list_y + i, on = i == sel;
            if (i < NCOMPUTE_IMG_PRESETS) {
                snprintf(line, sizeof line, "%-34.34s  %s",
                         compute_img_presets[i].title, compute_img_presets[i].detail);
            } else if (i == NCOMPUTE_IMG_PRESETS) {
                snprintf(line, sizeof line, "%s", _("[ Download from custom HTTPS URL (.tar.gz / .tar.xz / .tar.zst)... ]"));
            } else {
                snprintf(line, sizeof line, "%s", _("[ Import from local tarball or directory path... ]"));
            }
            screen_fill(0, y, cols, 1, on ? ACCENT : BG);
            screen_text(3, y, line, on ? ON_ACCENT : FG, on ? ACCENT : BG);
            ui_hit(0, y, cols, 1, i);
        }

        if (notice[0]) {
            screen_text(2, rows - 4, notice, ACCENT_TEXT, BG);
        }
        screen_text(2, rows - 3, _("Up/Down: choose   Enter: download / import   Esc: back"), ACCENT_TEXT, BG);
        settings_back_button(rows - 2);
        screen_flush();

        int key = key_get(), mods = mods_now(), act = key_list_action(key, mods);
        if (close_read(key)) return 0;
        if (key == K_REPAINT || key == K_SHOWN) continue;
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (settings_back_clicked(e)) return 0;
            if (e->wheel) {
                sel -= e->wheel;
                if (sel < 0) sel = 0;
                if (sel >= total) sel = total - 1;
                continue;
            }
            if (e->button == BTN_LEFT && e->pressed) {
                int hit = ui_hit_at(e->col, e->row);
                if (hit >= 0 && hit < total) {
                    sel = hit;
                    key = '\n';
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (act == ACT_HELP) {
            help_screen();
            if (close_due()) return 0;
            continue;
        }
        if (act == ACT_CANCEL) return 0;
        if (key == K_UP && sel > 0) sel--;
        else if (key == K_DOWN && sel + 1 < total) sel++;
        else if (key == '\n') {
            if (sel < NCOMPUTE_IMG_PRESETS) {
                char name[68];
                snprintf(name, sizeof name, "%s", compute_img_presets[sel].id);
                if (!prompt(_("Save image as (Enter for default): "), name, sizeof name) || !name[0]) {
                    snprintf(notice, sizeof notice, "%s", _("Download cancelled."));
                    continue;
                }
                if (!compute_valid_name_input(name)) {
                    snprintf(notice, sizeof notice, "%s", _("Invalid image name: letters, digits, . _ - only."));
                    continue;
                }
                char *const cmd[] = {
                    "/bin/sh", "-c",
                    "/usr/bin/sudo " LOCACHY_COMPUTE " image fetch \"$1\" --name \"$2\"; s=$?; "
                    "printf '\\nExit %d. Enter: back to Compute Manager. ' \"$s\"; read -r _; exit $s",
                    "sh", (char *)compute_img_presets[sel].id, name, NULL
                };
                int st = in_term(cmd);
                load_compute_status();
                if (st == 0) {
                    if (out_name && out_size) compute_copy_field(out_name, out_size, name);
                    snprintf(notice, sizeof notice, _("Downloaded and verified image '%.40s'."), name);
                    return 1;
                }
                snprintf(notice, sizeof notice, _("Image download failed for '%.40s'."), name);
            } else if (sel == NCOMPUTE_IMG_PRESETS) {
                char name[68] = "", url[256] = "", sha[72] = "";
                if (!prompt(_("Image name (e.g. debian-12): "), name, sizeof name) || !name[0]) {
                    snprintf(notice, sizeof notice, "%s", _("Download cancelled."));
                    continue;
                }
                if (!compute_valid_name_input(name)) {
                    snprintf(notice, sizeof notice, "%s", _("Invalid image name: letters, digits, . _ - only."));
                    continue;
                }
                if (!prompt(_("HTTPS rootfs tarball URL (https://...): "), url, sizeof url) || !url[0]) {
                    snprintf(notice, sizeof notice, "%s", _("Download cancelled."));
                    continue;
                }
                if (strncmp(url, "https://", 8) != 0) {
                    snprintf(notice, sizeof notice, "%s", _("URL must start with https://."));
                    continue;
                }
                prompt(_("Expected SHA-256 (optional, Enter to skip): "), sha, sizeof sha);
                char *const cmd[] = {
                    "/bin/sh", "-c",
                    "if [ -n \"$3\" ]; then "
                    "/usr/bin/sudo " LOCACHY_COMPUTE " image fetch --url \"$1\" --name \"$2\" --sha256 \"$3\"; "
                    "else /usr/bin/sudo " LOCACHY_COMPUTE " image fetch --url \"$1\" --name \"$2\"; fi; "
                    "s=$?; printf '\\nExit %d. Enter: back. ' \"$s\"; read -r _; exit $s",
                    "sh", url, name, sha, NULL
                };
                int st = in_term(cmd);
                load_compute_status();
                if (st == 0) {
                    if (out_name && out_size) compute_copy_field(out_name, out_size, name);
                    snprintf(notice, sizeof notice, _("Downloaded image '%.40s'."), name);
                    return 1;
                }
                snprintf(notice, sizeof notice, _("Image download failed for '%.40s'."), name);
            } else {
                char name[68] = "", src[256] = "";
                if (!prompt(_("Image name (e.g. ubuntu-custom): "), name, sizeof name) || !name[0]) {
                    snprintf(notice, sizeof notice, "%s", _("Import cancelled."));
                    continue;
                }
                if (!compute_valid_name_input(name)) {
                    snprintf(notice, sizeof notice, "%s", _("Invalid image name: letters, digits, . _ - only."));
                    continue;
                }
                if (!prompt(_("Local rootfs path (directory or .tar/.tar.gz/.tar.zst): "), src, sizeof src) || !src[0]) {
                    snprintf(notice, sizeof notice, "%s", _("Import cancelled."));
                    continue;
                }
                if (src[0] != '/') {
                    snprintf(notice, sizeof notice, "%s", _("Source path must be an absolute path."));
                    continue;
                }
                char *const cmd[] = {
                    "/bin/sh", "-c",
                    "/usr/bin/sudo " LOCACHY_COMPUTE " image import \"$1\" \"$2\"; s=$?; "
                    "printf '\\nExit %d. Enter: back to Compute Manager. ' \"$s\"; read -r _; exit $s",
                    "sh", name, src, NULL
                };
                int st = in_term(cmd);
                load_compute_status();
                if (st == 0) {
                    if (out_name && out_size) compute_copy_field(out_name, out_size, name);
                    snprintf(notice, sizeof notice, _("Imported image '%.40s'."), name);
                    return 1;
                }
                snprintf(notice, sizeof notice, _("Image import failed for '%.40s'."), name);
            }
        }
    }
}

static int compute_csv_has_vol(const char *csv, const char *vol_name)
{
    if (!csv || !csv[0] || !vol_name || !vol_name[0]) return 0;
    size_t nlen = strlen(vol_name);
    const char *p = csv;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len >= nlen && strncmp(p, vol_name, nlen) == 0 && (len == nlen || p[nlen] == ':')) {
            return 1;
        }
        p = comma ? comma + 1 : p + len;
    }
    return 0;
}

static int compute_split_csv(const char *csv, char out[][160], int max_items)
{
    int n = 0;
    if (!csv || !csv[0]) return 0;
    const char *p = csv;
    while (*p && n < max_items) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len > 0) {
            if (len >= 160) len = 159;
            memcpy(out[n], p, len);
            out[n][len] = '\0';
            n++;
        }
        p = comma ? comma + 1 : p + len;
    }
    return n;
}

static const char *const compute_mem_presets[] = { "", "8G", "16G", "24G", "32G", "48G", "64G" };
#define NCOMPUTE_MEM_PRESETS ((int)(sizeof compute_mem_presets / sizeof compute_mem_presets[0]))

static const char *const compute_cpu_presets[] = {
    "", "400000 100000", "800000 100000", "1200000 100000", "1600000 100000"
};
static const char *const compute_cpu_labels[] = {
    "unlimited", "4 cores (400000 100000)", "8 cores (800000 100000)",
    "12 cores (1200000 100000)", "16 cores (1600000 100000)"
};
#define NCOMPUTE_CPU_PRESETS ((int)(sizeof compute_cpu_presets / sizeof compute_cpu_presets[0]))

static void compute_format_cpu(const char *val, char *buf, size_t size)
{
    for (int i = 0; i < NCOMPUTE_CPU_PRESETS; i++) {
        if (strcmp(val, compute_cpu_presets[i]) == 0) {
            snprintf(buf, size, "%s", compute_cpu_labels[i]);
            return;
        }
    }
    snprintf(buf, size, "%s", val[0] ? val : "unlimited");
}

/* Full interactive form for creating or editing a compute container's settings:
 * image picker (with download), GPU toggle, network (locachy-net/none), memory
 * and CPU limits, workdir, storage volume toggles, and extra host mounts. */
static int compute_env_form(const struct compute_env_row *existing)
{
    int editing = existing != NULL;
    if (!editing && compute_snap.nimages == 0) {
        char fetched[68] = "";
        if (!compute_fetch_image_screen(fetched, sizeof fetched) || compute_snap.nimages == 0) {
            if (!notice[0]) snprintf(notice, sizeof notice, "%s", _("Download or import a base image first."));
            return 0;
        }
    }

    load_storage_status();
    char ename[68] = "";
    char img[68] = "";
    int img_idx = 0;
    int want_gpu = compute_gpu ? 1 : 0;
    int want_net = 1; /* default: locachy-net (Internet & Tailscale) */
    char mem_max[32] = "";
    char cpu_max[32] = "";
    char workdir[120] = "/root";
    int elig_vols[MAX_STORAGE_VOLS];
    int vol_on[MAX_STORAGE_VOLS];
    int nelig = 0;
    char extra_mounts[MAX_ENV_MOUNTS][160];
    int nmounts = 0;

    for (int i = 0; i < nstorage_vols && nelig < MAX_STORAGE_VOLS; i++) {
        if (strcmp(storage_vols[i].role, "shared") == 0 || strcmp(storage_vols[i].role, "compute") == 0) {
            elig_vols[nelig] = i;
            vol_on[nelig] = 0;
            nelig++;
        }
    }

    if (editing) {
        snprintf(ename, sizeof ename, "%s", existing->name);
        snprintf(img, sizeof img, "%s", existing->image);
        want_gpu = existing->gpu ? 1 : 0;
        want_net = strcmp(existing->net, "none") != 0;
        snprintf(mem_max, sizeof mem_max, "%s", existing->memory_max);
        snprintf(cpu_max, sizeof cpu_max, "%s", existing->cpu_max);
        snprintf(workdir, sizeof workdir, "%s", existing->workdir[0] ? existing->workdir : "/");
        for (int i = 0; i < nelig; i++) {
            vol_on[i] = compute_csv_has_vol(existing->volumes, storage_vols[elig_vols[i]].name);
        }
        nmounts = compute_split_csv(existing->mounts, extra_mounts, MAX_ENV_MOUNTS);
    } else {
        if (compute_snap.nenvs == 0) snprintf(ename, sizeof ename, "compute");
        else snprintf(ename, sizeof ename, "compute%d", compute_snap.nenvs + 1);
        if (compute_snap.nimages > 0) snprintf(img, sizeof img, "%s", compute_snap.images[0].name);
        const char *cvol = compute_storage_cfg[0] ? compute_storage_cfg : compute_snap.storage_volume;
        for (int i = 0; i < nelig; i++) {
            const struct storage_vol *v = &storage_vols[elig_vols[i]];
            if (v->mounted && ((cvol[0] && strcmp(v->name, cvol) == 0) || strcmp(v->role, "compute") == 0)) {
                vol_on[i] = 1;
            }
        }
    }

    /* Row layout:
     *   0: Name
     *   1: Base image
     *   2: GPU passthrough
     *   3: Network
     *   4: Memory limit
     *   5: CPU limit
     *   6: Working directory
     *   7 .. 7+nelig-1: Storage volume toggles
     *   7+nelig .. 7+nelig+nmounts-1: Existing extra mounts (Delete removes)
     *   7+nelig+nmounts: [ + Add host directory mount... ]
     *   7+nelig+nmounts+1: [ Save & Create / Save Changes ]
     */
    int sel = editing ? 2 : 0;
    int top = 0;
    char line[320], cpubuf[64];

    for (;;) {
        int vol_start = 7;
        int mnt_start = vol_start + nelig;
        int add_mnt_row = mnt_start + nmounts;
        int save_row = add_mnt_row + 1;
        int total = save_row + 1;

        int cols = screen_cols(), rows = screen_rows();
        int list_y = 6;
        int height = rows - list_y - 4;
        if (height < 1) height = 1;
        if (sel >= total) sel = total - 1;
        if (sel < 0) sel = 0;
        if (sel < top) top = sel;
        if (sel >= top + height) top = sel - height + 1;

        screen_fill(0, 0, cols, rows, BG);
        snprintf(line, sizeof line, editing ? _("Container Settings: %.40s") : _("New Compute Container"), ename);
        screen_bar(line, NULL);
        ui_hits_clear();

        screen_fill(0, 2, cols, 3, SELECTION);
        snprintf(line, sizeof line, _("Host GPU: %s   NVIDIA: %s   (devs=%d, libs=%d)"),
                 compute_gpu ? _("enabled") : _("disabled"),
                 compute_snap.nvidia_version[0] ? compute_snap.nvidia_version : _("none"),
                 compute_snap.gpu_devices, compute_snap.gpu_libs);
        screen_text(2, 2, line, FG, SELECTION);
        snprintf(line, sizeof line, editing ?
                 _("Editing container settings keeps all installed packages and files in its OS layer intact.") :
                 _("Configure one container below: pick image, GPU, network, and mounted storage volumes, then Save."));
        screen_text(2, 3, line, DIM, SELECTION);
        snprintf(line, sizeof line, _("Tip: Left/Right toggles options, Enter edits or runs action, d downloads another image."));
        screen_text(2, 4, line, DIM, SELECTION);

        for (int r = 0; r < height && top + r < total; r++) {
            int idx = top + r, y = list_y + r, on = idx == sel;
            line[0] = '\0';
            if (idx == 0) {
                snprintf(line, sizeof line, editing ? _("Name:               %.64s  (fixed)") : _("Name:               %.64s"), ename);
            } else if (idx == 1) {
                if (editing) {
                    snprintf(line, sizeof line, _("Base rootfs image:  %.64s  (fixed)"), img);
                } else {
                    snprintf(line, sizeof line, _("Base rootfs image:  < %.48s >  (%d available, d: download)"), img, compute_snap.nimages);
                }
            } else if (idx == 2) {
                snprintf(line, sizeof line, _("GPU passthrough:    < %s >"), want_gpu ? _("ON (NVIDIA devices + libs + nvidia-smi)") : _("OFF (CPU only)"));
            } else if (idx == 3) {
                snprintf(line, sizeof line, _("Network access:     < %s >"), want_net ? _("locachy-net (Internet & Tailscale)") : _("none (offline / isolated)"));
            } else if (idx == 4) {
                snprintf(line, sizeof line, _("Memory limit:       < %s >"), mem_max[0] ? mem_max : _("unlimited"));
            } else if (idx == 5) {
                compute_format_cpu(cpu_max, cpubuf, sizeof cpubuf);
                snprintf(line, sizeof line, _("CPU limit:          < %s >"), cpubuf);
            } else if (idx == 6) {
                snprintf(line, sizeof line, _("Working directory:  %.100s"), workdir);
            } else if (idx >= vol_start && idx < mnt_start) {
                int vi = idx - vol_start;
                const struct storage_vol *v = &storage_vols[elig_vols[vi]];
                snprintf(line, sizeof line, _("Storage volume:     [%c] %-18.18s -> /volumes/%-18.18s (%s, %s)"),
                         vol_on[vi] ? 'x' : ' ', v->name, v->name, v->role,
                         v->mounted ? _("MOUNTED") : _("UNMOUNTED - m: mount"));
            } else if (idx >= mnt_start && idx < add_mnt_row) {
                snprintf(line, sizeof line, _("Host mount:         %.140s  (Delete: remove)"), extra_mounts[idx - mnt_start]);
            } else if (idx == add_mnt_row) {
                snprintf(line, sizeof line, "%s", _("[ + Add extra host directory mount (host:container:rw)... ]"));
            } else {
                snprintf(line, sizeof line, "%s", editing ? _("[ Save Container Settings ]") : _("[ Save & Create Container ]"));
            }
            screen_fill(0, y, cols, 1, on ? ACCENT : BG);
            screen_text(3, y, line, on ? ON_ACCENT : FG, on ? ACCENT : BG);
            ui_hit(0, y, cols, 1, idx);
        }

        if (notice[0]) {
            screen_text(2, rows - 4, notice, ACCENT_TEXT, BG);
        }
        screen_text(2, rows - 3, _("Up/Down: move   Left/Right: change   Enter: edit/save   d: download image   Esc: cancel"), ACCENT_TEXT, BG);
        settings_back_button(rows - 2);
        screen_flush();

        int key = key_get(), mods = mods_now(), act = key_list_action(key, mods);
        if (close_read(key)) return 0;
        if (key == K_REPAINT || key == K_SHOWN) continue;
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (settings_back_clicked(e)) return 0;
            if (e->wheel) {
                sel -= e->wheel;
                continue;
            }
            if (e->button == BTN_LEFT && e->pressed) {
                int hit = ui_hit_at(e->col, e->row);
                if (hit >= 0 && hit < total) {
                    sel = hit;
                    key = '\n';
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (act == ACT_HELP) {
            help_screen();
            if (close_due()) return 0;
            continue;
        }
        if (act == ACT_CANCEL) return 0;
        if (act == ACT_DELETE || act == ACT_DELETE_PERMANENT) {
            key = K_DEL;
            act = ACT_NONE;
        }

        if (key == K_UP && sel > 0) {
            sel--;
        } else if (key == K_DOWN && sel + 1 < total) {
            sel++;
        } else if (key == 'd' || key == 'D') {
            char fetched[68] = "";
            if (compute_fetch_image_screen(fetched, sizeof fetched) && !editing && fetched[0]) {
                snprintf(img, sizeof img, "%s", fetched);
                for (int i = 0; i < compute_snap.nimages; i++) {
                    if (strcmp(compute_snap.images[i].name, fetched) == 0) {
                        img_idx = i;
                        break;
                    }
                }
            }
        } else if ((key == 'm' || key == 'M') && sel >= vol_start && sel < mnt_start) {
            storage_toggle_mount(elig_vols[sel - vol_start]);
            load_storage_status();
            load_compute_status();
        } else if (key == K_DEL && sel >= mnt_start && sel < add_mnt_row) {
            int mi = sel - mnt_start;
            for (int j = mi; j + 1 < nmounts; j++) {
                memcpy(extra_mounts[j], extra_mounts[j + 1], sizeof extra_mounts[j]);
            }
            nmounts--;
            snprintf(notice, sizeof notice, "%s", _("Removed extra mount (press Save to apply)."));
        } else if (key == K_LEFT || key == K_RIGHT || key == ' ' || key == '\n') {
            int delta = (key == K_LEFT) ? -1 : 1;
            if (sel == 0 && !editing && key == '\n') {
                char buf[68];
                snprintf(buf, sizeof buf, "%s", ename);
                if (prompt(_("Container name: "), buf, sizeof buf) && buf[0]) {
                    if (compute_valid_name_input(buf)) snprintf(ename, sizeof ename, "%s", buf);
                    else snprintf(notice, sizeof notice, "%s", _("Invalid name: letters, digits, . _ - only."));
                }
            } else if (sel == 1 && !editing) {
                if (key == '\n' && compute_snap.nimages <= 1) {
                    char fetched[68] = "";
                    if (compute_fetch_image_screen(fetched, sizeof fetched) && fetched[0]) {
                        snprintf(img, sizeof img, "%s", fetched);
                    }
                } else if (compute_snap.nimages > 0) {
                    img_idx = (img_idx + delta + compute_snap.nimages) % compute_snap.nimages;
                    snprintf(img, sizeof img, "%s", compute_snap.images[img_idx].name);
                }
            } else if (sel == 2) {
                want_gpu = !want_gpu;
            } else if (sel == 3) {
                want_net = !want_net;
            } else if (sel == 4) {
                if (key == '\n') {
                    char buf[32];
                    snprintf(buf, sizeof buf, "%s", mem_max);
                    if (prompt(_("Memory limit (e.g. 16G, empty = unlimited): "), buf, sizeof buf)) {
                        snprintf(mem_max, sizeof mem_max, "%s", buf);
                    }
                } else {
                    int mi = 0;
                    for (int i = 0; i < NCOMPUTE_MEM_PRESETS; i++) {
                        if (strcmp(mem_max, compute_mem_presets[i]) == 0) { mi = i; break; }
                    }
                    mi = (mi + delta + NCOMPUTE_MEM_PRESETS) % NCOMPUTE_MEM_PRESETS;
                    snprintf(mem_max, sizeof mem_max, "%s", compute_mem_presets[mi]);
                }
            } else if (sel == 5) {
                if (key == '\n') {
                    char buf[32];
                    snprintf(buf, sizeof buf, "%s", cpu_max);
                    if (prompt(_("CPU quota (e.g. '800000 100000', empty = unlimited): "), buf, sizeof buf)) {
                        snprintf(cpu_max, sizeof cpu_max, "%s", buf);
                    }
                } else {
                    int ci = 0;
                    for (int i = 0; i < NCOMPUTE_CPU_PRESETS; i++) {
                        if (strcmp(cpu_max, compute_cpu_presets[i]) == 0) { ci = i; break; }
                    }
                    ci = (ci + delta + NCOMPUTE_CPU_PRESETS) % NCOMPUTE_CPU_PRESETS;
                    snprintf(cpu_max, sizeof cpu_max, "%s", compute_cpu_presets[ci]);
                }
            } else if (sel == 6 && key == '\n') {
                char buf[120];
                snprintf(buf, sizeof buf, "%s", workdir);
                if (prompt(_("Working directory inside container (absolute path): "), buf, sizeof buf) && buf[0]) {
                    if (buf[0] == '/') snprintf(workdir, sizeof workdir, "%s", buf);
                    else snprintf(notice, sizeof notice, "%s", _("Working directory must start with '/'."));
                }
            } else if (sel >= vol_start && sel < mnt_start) {
                int vi = sel - vol_start;
                vol_on[vi] = !vol_on[vi];
                if (vol_on[vi] && !storage_vols[elig_vols[vi]].mounted) {
                    snprintf(notice, sizeof notice, _("Volume '%.32s' is not mounted yet — press m to mount it before saving."),
                             storage_vols[elig_vols[vi]].name);
                }
            } else if (sel == add_mnt_row && key == '\n') {
                if (nmounts >= MAX_ENV_MOUNTS) {
                    snprintf(notice, sizeof notice, "%s", _("Maximum extra mounts reached."));
                } else {
                    char spec[160] = "";
                    if (prompt(_("Mount spec (/host/path:/container/path[:ro|:rw]): "), spec, sizeof spec) && spec[0]) {
                        if (spec[0] == '/' && strchr(spec, ':')) {
                            snprintf(extra_mounts[nmounts++], sizeof extra_mounts[0], "%s", spec);
                        } else {
                            snprintf(notice, sizeof notice, "%s", _("Expected /host/path:/container/path[:ro|:rw]."));
                        }
                    }
                }
            } else if (sel == save_row && key == '\n') {
                if (!compute_valid_name_input(ename)) {
                    snprintf(notice, sizeof notice, "%s", _("Invalid container name."));
                    continue;
                }
                if (!editing && !compute_valid_name_input(img)) {
                    snprintf(notice, sizeof notice, "%s", _("Select or download a base rootfs image first."));
                    continue;
                }
                char *argv[96];
                char vol_specs[MAX_STORAGE_VOLS][64];
                int n = 0;
                argv[n++] = LOCACHY_COMPUTE;
                argv[n++] = "env";
                argv[n++] = editing ? "set" : "create";
                argv[n++] = ename;
                if (!editing) {
                    argv[n++] = "--image";
                    argv[n++] = img;
                }
                argv[n++] = want_gpu ? "--gpu" : "--no-gpu";
                argv[n++] = "--net";
                argv[n++] = want_net ? "locachy-net" : "none";
                argv[n++] = "--memory";
                argv[n++] = mem_max;
                argv[n++] = "--cpu";
                argv[n++] = cpu_max;
                argv[n++] = "--workdir";
                argv[n++] = workdir;
                if (editing) {
                    argv[n++] = "--clear-volumes";
                    argv[n++] = "--clear-mounts";
                }
                for (int i = 0; i < nelig && n + 2 < 95; i++) {
                    if (!vol_on[i]) continue;
                    snprintf(vol_specs[i], sizeof vol_specs[i], "%s:/volumes/%s:rw",
                             storage_vols[elig_vols[i]].name, storage_vols[elig_vols[i]].name);
                    argv[n++] = "--volume";
                    argv[n++] = vol_specs[i];
                }
                for (int i = 0; i < nmounts && n + 2 < 95; i++) {
                    argv[n++] = "--mount";
                    argv[n++] = extra_mounts[i];
                }
                argv[n] = NULL;
                char out[256] = "";
                int rc = compute_exec(argv, out, sizeof out);
                load_compute_status();
                if (rc == 0) {
                    snprintf(notice, sizeof notice, editing ?
                             _("Saved settings for container '%.40s'.") :
                             _("Created compute container '%.40s'."), ename);
                    return 1;
                }
                snprintf(notice, sizeof notice, _("Save failed: %.140s"), out[0] ? out : _("check storage mount and inputs"));
            }
        }
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

static int compute_primary_env_index(void)
{
    if (compute_snap.nenvs <= 0) return -1;
    if (compute_default_env[0]) {
        for (int i = 0; i < compute_snap.nenvs; i++) {
            if (strcmp(compute_snap.envs[i].name, compute_default_env) == 0) return i;
        }
    }
    return 0;
}

static void compute_set_default_env(const char *ename)
{
    if (!ename || !ename[0]) return;
    if (rc_set("compute_default_env", ename) == 0) {
        compute_copy_field(compute_default_env, sizeof compute_default_env, ename);
        snprintf(notice, sizeof notice, _("Default working container set to '%.40s'."), ename);
    } else {
        snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
    }
}

static const int compute_vm_mem_presets[] = { 1024, 2048, 4096, 8192, 16384, 32768 };
#define NCOMPUTE_VM_MEM_PRESETS ((int)(sizeof compute_vm_mem_presets / sizeof compute_vm_mem_presets[0]))
static const int compute_vm_cpu_presets[] = { 1, 2, 4, 6, 8, 12, 16 };
#define NCOMPUTE_VM_CPU_PRESETS ((int)(sizeof compute_vm_cpu_presets / sizeof compute_vm_cpu_presets[0]))

/* Full interactive form for creating a compute micro-VM (locachy-compute vm create). */
static int compute_vm_create_form(void)
{
    char name[68] = "vm1";
    char disk[256] = "";
    int mem_mb = 4096;
    int cpus = 4;
    int net_user = 1;
    char ssh_user[64] = "";
    int vol_idx[MAX_STORAGE_VOLS];
    int vol_on[MAX_STORAGE_VOLS];
    int nvols = 0;
    char shares[MAX_ENV_MOUNTS][160];
    int nshares = 0;

    load_storage_status();
    for (int i = 0; i < nstorage_vols && nvols < MAX_STORAGE_VOLS; i++) {
        if (strcmp(storage_vols[i].role, "shared") == 0 || strcmp(storage_vols[i].role, "compute") == 0) {
            vol_idx[nvols] = i;
            vol_on[nvols] = 0;
            nvols++;
        }
    }

    int sel = 0, top = 0;
    char line[320];
    notice[0] = '\0';

    for (;;) {
        int base_rows = 6; /* name, disk, mem, cpus, net, ssh_user */
        int row_vols = base_rows;
        int row_shares = row_vols + nvols;
        int row_add_share = row_shares + nshares;
        int row_save = row_add_share + 1;
        int total = row_save + 1;

        int cols = screen_cols(), rows = screen_rows();
        int list_y = 6;
        int height = rows - list_y - 4;
        if (height < 1) height = 1;
        if (sel >= total) sel = total - 1;
        if (sel < 0) sel = 0;
        if (sel < top) top = sel;
        if (sel >= top + height) top = sel - height + 1;

        screen_fill(0, 0, cols, rows, BG);
        screen_bar(_("New Compute Micro-VM"), NULL);
        ui_hits_clear();

        screen_fill(0, 2, cols, 3, SELECTION);
        screen_text(2, 2, _("KVM micro-VM with CoW qcow2 overlay over a read-only base disk (virtio-blk + virtiofs)."), FG, SELECTION);
        screen_text(2, 3, _("Supports loopback SSH forwarding (--net user) and optional export to Home desktop."), DIM, SELECTION);
        screen_text(2, 4, _("Tip: Left/Right changes values, Enter edits or toggles, Delete removes a share."), DIM, SELECTION);

        for (int r = 0; r < height && top + r < total; r++) {
            int idx = top + r, y = list_y + r, on = idx == sel;
            line[0] = '\0';
            if (idx == 0) {
                snprintf(line, sizeof line, _("VM name:            %.64s"), name);
            } else if (idx == 1) {
                snprintf(line, sizeof line, _("Base qcow2 disk:    %.120s"), disk[0] ? disk : _("(Enter: path to read-only .qcow2)"));
            } else if (idx == 2) {
                snprintf(line, sizeof line, _("Memory:             < %d MiB >"), mem_mb);
            } else if (idx == 3) {
                snprintf(line, sizeof line, _("vCPUs:              < %d >"), cpus);
            } else if (idx == 4) {
                snprintf(line, sizeof line, _("Network:            < %s >"),
                         net_user ? _("user (loopback SSH forwarded)") : _("none (offline / isolated)"));
            } else if (idx == 5) {
                snprintf(line, sizeof line, _("SSH user (opt.):    %.64s"), ssh_user[0] ? ssh_user : _("(auto)"));
            } else if (idx >= row_vols && idx < row_shares) {
                int vi = vol_idx[idx - row_vols];
                snprintf(line, sizeof line, _("Storage volume:     [%c] %-18.18s (virtiofs tag: vol-%-14.14s, %s)"),
                         vol_on[idx - row_vols] ? 'x' : ' ',
                         storage_vols[vi].name, storage_vols[vi].name,
                         storage_vols[vi].mounted ? _("MOUNTED") : _("UNMOUNTED - m: mount"));
            } else if (idx >= row_shares && idx < row_add_share) {
                snprintf(line, sizeof line, _("Host share:         %.140s  (Delete: remove)"), shares[idx - row_shares]);
            } else if (idx == row_add_share) {
                snprintf(line, sizeof line, "%s", _("[ + Add virtiofs directory share (/host/dir:tag[:ro|:rw])... ]"));
            } else {
                snprintf(line, sizeof line, "%s", _("[ Create Compute Micro-VM ]"));
            }
            screen_fill(0, y, cols, 1, on ? ACCENT : BG);
            screen_text(3, y, line, on ? ON_ACCENT : FG, on ? ACCENT : BG);
            ui_hit(0, y, cols, 1, idx);
        }

        if (notice[0]) screen_text(2, rows - 4, notice, ACCENT_TEXT, BG);
        screen_text(2, rows - 3, _("Up/Down: move   Left/Right: change   Enter: edit/create   Esc: cancel"), ACCENT_TEXT, BG);
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
                    if (e->clicks == 2 || sel >= row_add_share) key = '\n';
                    else continue;
                } else continue;
            } else continue;
        }
        if (act == ACT_HELP) {
            help_screen();
            if (close_due()) return 0;
            continue;
        }
        if (act == ACT_CANCEL) return 0;
        if (act == ACT_DELETE || act == ACT_DELETE_PERMANENT) { key = K_DEL; act = ACT_NONE; }
        if (act != ACT_NONE) continue;

        if (key == K_UP && sel > 0) sel--;
        else if (key == K_DOWN && sel + 1 < total) sel++;
        else if (key == K_BOL) sel = 0;
        else if (key == K_EOL) sel = total - 1;
        else if ((key == 'm' || key == 'M') && sel >= row_vols && sel < row_shares) {
            storage_toggle_mount(vol_idx[sel - row_vols]);
            load_storage_status();
        } else if (key == K_DEL && sel >= row_shares && sel < row_add_share) {
            int si = sel - row_shares;
            for (int k = si; k + 1 < nshares; k++) compute_copy_field(shares[k], sizeof shares[k], shares[k + 1]);
            nshares--;
            snprintf(notice, sizeof notice, "%s", _("Removed share."));
        } else if (key == K_LEFT || key == K_RIGHT || key == ' ') {
            int dir = (key == K_LEFT) ? -1 : 1;
            if (sel == 2) {
                int pi = 0;
                for (int i = 0; i < NCOMPUTE_VM_MEM_PRESETS; i++)
                    if (compute_vm_mem_presets[i] == mem_mb) { pi = i; break; }
                pi = (pi + dir + NCOMPUTE_VM_MEM_PRESETS) % NCOMPUTE_VM_MEM_PRESETS;
                mem_mb = compute_vm_mem_presets[pi];
            } else if (sel == 3) {
                int pi = 0;
                for (int i = 0; i < NCOMPUTE_VM_CPU_PRESETS; i++)
                    if (compute_vm_cpu_presets[i] == cpus) { pi = i; break; }
                pi = (pi + dir + NCOMPUTE_VM_CPU_PRESETS) % NCOMPUTE_VM_CPU_PRESETS;
                cpus = compute_vm_cpu_presets[pi];
            } else if (sel == 4) {
                net_user = !net_user;
            } else if (sel >= row_vols && sel < row_shares) {
                vol_on[sel - row_vols] = !vol_on[sel - row_vols];
            }
        } else if (key == '\n') {
            if (sel == 0) {
                char buf[68];
                compute_copy_field(buf, sizeof buf, name);
                if (prompt(_("VM name: "), buf, sizeof buf) && buf[0]) {
                    if (compute_valid_name_input(buf)) compute_copy_field(name, sizeof name, buf);
                    else snprintf(notice, sizeof notice, "%s", _("Invalid name: letters, digits, . _ - only."));
                }
            } else if (sel == 1) {
                char buf[256];
                compute_copy_field(buf, sizeof buf, disk);
                if (prompt(_("Absolute path to base qcow2 image: "), buf, sizeof buf)) {
                    if (!buf[0] || buf[0] == '/') compute_copy_field(disk, sizeof disk, buf);
                    else snprintf(notice, sizeof notice, "%s", _("Base disk path must be an absolute path starting with '/'."));
                }
            } else if (sel == 2) {
                char buf[24];
                snprintf(buf, sizeof buf, "%d", mem_mb);
                if (prompt(_("Memory in MiB (256..262144): "), buf, sizeof buf) && buf[0]) {
                    int v = atoi(buf);
                    if (v >= 256 && v <= 262144) mem_mb = v;
                    else snprintf(notice, sizeof notice, "%s", _("Memory must be between 256 and 262144 MiB."));
                }
            } else if (sel == 3) {
                char buf[16];
                snprintf(buf, sizeof buf, "%d", cpus);
                if (prompt(_("vCPU count (1..128): "), buf, sizeof buf) && buf[0]) {
                    int v = atoi(buf);
                    if (v >= 1 && v <= 128) cpus = v;
                    else snprintf(notice, sizeof notice, "%s", _("vCPUs must be between 1 and 128."));
                }
            } else if (sel == 4) {
                net_user = !net_user;
            } else if (sel == 5) {
                char buf[64];
                compute_copy_field(buf, sizeof buf, ssh_user);
                if (prompt(_("SSH login username (empty = auto): "), buf, sizeof buf))
                    compute_copy_field(ssh_user, sizeof ssh_user, buf);
            } else if (sel >= row_vols && sel < row_shares) {
                vol_on[sel - row_vols] = !vol_on[sel - row_vols];
            } else if (sel == row_add_share) {
                if (nshares >= MAX_ENV_MOUNTS) {
                    snprintf(notice, sizeof notice, "%s", _("Maximum shares reached."));
                } else {
                    char buf[160] = "";
                    if (prompt(_("Share spec (/host/dir:tag[:ro|:rw]): "), buf, sizeof buf) && buf[0]) {
                        if (buf[0] == '/' && strchr(buf, ':')) compute_copy_field(shares[nshares++], sizeof shares[0], buf);
                        else snprintf(notice, sizeof notice, "%s", _("Expected /host/dir:tag[:ro|:rw]."));
                    }
                }
            } else if (sel == row_save) {
                if (!compute_valid_name_input(name)) {
                    snprintf(notice, sizeof notice, "%s", _("Invalid VM name."));
                    continue;
                }
                if (disk[0] != '/') {
                    snprintf(notice, sizeof notice, "%s", _("Specify an absolute path to a base qcow2 disk first."));
                    continue;
                }
                char mem_s[24], cpu_s[16];
                snprintf(mem_s, sizeof mem_s, "%d", mem_mb);
                snprintf(cpu_s, sizeof cpu_s, "%d", cpus);
                char *argv[96];
                int ac = 0;
                argv[ac++] = LOCACHY_COMPUTE;
                argv[ac++] = "vm";
                argv[ac++] = "create";
                argv[ac++] = name;
                argv[ac++] = "--disk";
                argv[ac++] = disk;
                argv[ac++] = "--memory-mb";
                argv[ac++] = mem_s;
                argv[ac++] = "--cpus";
                argv[ac++] = cpu_s;
                argv[ac++] = "--net";
                argv[ac++] = net_user ? "user" : "none";
                if (ssh_user[0]) {
                    argv[ac++] = "--ssh-user";
                    argv[ac++] = ssh_user;
                }
                for (int i = 0; i < nvols && ac + 2 < 95; i++) {
                    if (vol_on[i]) {
                        argv[ac++] = "--volume";
                        argv[ac++] = storage_vols[vol_idx[i]].name;
                    }
                }
                for (int i = 0; i < nshares && ac + 2 < 95; i++) {
                    argv[ac++] = "--share";
                    argv[ac++] = shares[i];
                }
                argv[ac] = NULL;
                char out[256] = "";
                int rc = compute_exec(argv, out, sizeof out);
                load_compute_status();
                if (rc == 0) {
                    snprintf(notice, sizeof notice, _("Created compute micro-VM '%.40s'."), name);
                    return 1;
                }
                snprintf(notice, sizeof notice, _("Create VM failed: %.140s"), out[0] ? out : _("check base disk path and storage"));
            }
        }
    }
}

/* Compute Node Manager: one page with section headers and no tabs. The list
 * holds the default working container (opened first), the containers, the
 * images, the micro-VMs, the jobs, and storage and access. Settings forms,
 * the image download, the micro-VM form, the SSH config and pairing open on top. */
enum compute_mgr_kind {
    CMR_HEADER, CMR_DEFAULT, CMR_DEFAULT_SETUP, CMR_ENV, CMR_NEW_ENV,
    CMR_IMAGE, CMR_NEW_IMAGE, CMR_VM, CMR_NEW_VM, CMR_JOB, CMR_NO_JOBS,
    CMR_STORAGE, CMR_SSH_CONFIG, CMR_PAIR
};

struct compute_mgr_row {
    int kind;
    int arg;
    const char *header;
};

#define MAX_COMPUTE_MGR_ROWS (4 * MAX_COMPUTE_ITEMS + 32)

static int compute_mgr_selectable(int kind)
{
    return kind != CMR_HEADER && kind != CMR_NO_JOBS;
}

/* The next selectable row from `from` in direction dir (+1 or -1), or `from`. */
static int compute_mgr_next(const struct compute_mgr_row *rows, int nrows, int from, int dir)
{
    for (int k = from + dir; k >= 0 && k < nrows; k += dir) {
        if (compute_mgr_selectable(rows[k].kind)) return k;
    }
    return from;
}

static int compute_mgr_build(struct compute_mgr_row *out, int max, int penv)
{
    int n = 0;
#define CMR_ADD(k, a, h) do { if (n < max) { out[n].kind = (k); out[n].arg = (a); out[n].header = (h); n++; } } while (0)
    CMR_ADD(CMR_HEADER, 0, _("Default working container"));
    if (penv >= 0) CMR_ADD(CMR_DEFAULT, penv, NULL);
    else CMR_ADD(CMR_DEFAULT_SETUP, 0, NULL);
    CMR_ADD(CMR_HEADER, 0, _("Containers"));
    for (int i = 0; i < compute_snap.nenvs; i++) CMR_ADD(CMR_ENV, i, NULL);
    CMR_ADD(CMR_NEW_ENV, 0, NULL);
    CMR_ADD(CMR_HEADER, 0, _("Images"));
    for (int i = 0; i < compute_snap.nimages; i++) CMR_ADD(CMR_IMAGE, i, NULL);
    CMR_ADD(CMR_NEW_IMAGE, 0, NULL);
    CMR_ADD(CMR_HEADER, 0, _("Micro-VMs"));
    for (int i = 0; i < compute_snap.nvms; i++) CMR_ADD(CMR_VM, i, NULL);
    CMR_ADD(CMR_NEW_VM, 0, NULL);
    CMR_ADD(CMR_HEADER, 0, _("Jobs"));
    if (compute_snap.njobs == 0) CMR_ADD(CMR_NO_JOBS, 0, NULL);
    for (int i = 0; i < compute_snap.njobs; i++) CMR_ADD(CMR_JOB, i, NULL);
    CMR_ADD(CMR_HEADER, 0, _("Storage and access"));
    CMR_ADD(CMR_STORAGE, 0, NULL);
    CMR_ADD(CMR_SSH_CONFIG, 0, NULL);
    CMR_ADD(CMR_PAIR, 0, NULL);
#undef CMR_ADD
    return n;
}

static void compute_mgr_row_text(const struct compute_mgr_row *mr, int penv, const char *svol,
                                 int storage_mounted, char *line, size_t size)
{
    line[0] = '\0';
    switch (mr->kind) {
    case CMR_DEFAULT: {
        const struct compute_env_row *e = &compute_snap.envs[mr->arg];
        snprintf(line, size, compute_snap.nenvs > 1 ?
                 _("> %-18.18s  img=%-18.18s  GPU:%-3s  net=%-11.11s  (Left/Right: switch default)") :
                 _("> %-18.18s  img=%-18.18s  GPU:%-3s  net=%-11.11s"),
                 e->name, e->image, e->gpu ? _("ON") : _("OFF"), e->net);
        break;
    }
    case CMR_DEFAULT_SETUP:
        snprintf(line, size, "%s", _("+ Set up the default working container..."));
        break;
    case CMR_ENV: {
        const struct compute_env_row *e = &compute_snap.envs[mr->arg];
        snprintf(line, size, "%c %-18.18s  img=%-18.18s  GPU:%-3s  net=%-11.11s  mem=%-6.6s",
                 mr->arg == penv ? '*' : ' ', e->name, e->image,
                 e->gpu ? _("ON") : _("OFF"), e->net, e->memory_max[0] ? e->memory_max : "-");
        break;
    }
    case CMR_NEW_ENV:
        snprintf(line, size, "%s", _("+ New container..."));
        break;
    case CMR_IMAGE: {
        const struct compute_img_row *im = &compute_snap.images[mr->arg];
        snprintf(line, size, "%-20.20s  %-20.20s  %.70s", im->name, im->imported_at, im->source);
        break;
    }
    case CMR_NEW_IMAGE:
        snprintf(line, size, "%s", _("+ Download or import a rootfs image..."));
        break;
    case CMR_VM: {
        const struct compute_vm_row *v = &compute_snap.vms[mr->arg];
        if (v->ssh_port > 0) {
            snprintf(line, size, "%-18.18s  %-7s  %5d MiB  %2d vCPU  ssh=127.0.0.1:%d",
                     v->name, v->running ? _("RUNNING") : _("stopped"), v->memory_mb, v->cpus, v->ssh_port);
        } else {
            snprintf(line, size, "%-18.18s  %-7s  %5d MiB  %2d vCPU",
                     v->name, v->running ? _("RUNNING") : _("stopped"), v->memory_mb, v->cpus);
        }
        break;
    }
    case CMR_NEW_VM:
        snprintf(line, size, "%s", _("+ Create compute micro-VM..."));
        break;
    case CMR_JOB: {
        const struct compute_job_row *j = &compute_snap.jobs[mr->arg];
        snprintf(line, size, "%-16.16s  %-14.14s  %-9.9s  exit=%-4.4s  %.60s",
                 j->job_id, j->env, j->state, j->exit_code[0] ? j->exit_code : "-", j->cmd);
        break;
    }
    case CMR_STORAGE:
        if (!svol[0]) {
            snprintf(line, size, "%s", _("Storage: root disk (configure under Storage > Storage Volumes)"));
        } else {
            snprintf(line, size, storage_mounted ?
                     _("Storage volume '%.32s' [MOUNTED]  (Enter: unmount)") :
                     _("Storage volume '%.32s' [UNMOUNTED]  (Enter: mount)"), svol);
        }
        break;
    case CMR_SSH_CONFIG:
        snprintf(line, size, "%s", _("View / copy client SSH config"));
        break;
    case CMR_PAIR:
        snprintf(line, size, "%s", _("Pair a compute client (locachy-pair)..."));
        break;
    default:
        break;
    }
}

static void manage_compute_node(void)
{
    if (access(LOCACHY_COMPUTE, X_OK) != 0) {
        snprintf(notice, sizeof notice, _("%s is not installed."), "locachy-compute");
        return;
    }
    load_storage_status();
    load_compute_status();

    struct compute_mgr_row mrows[MAX_COMPUTE_MGR_ROWS];
    int sel = 0, top = 0;
    int confirm_key = 0, confirm_row = -1;
    char line[320], sz1[24], sz2[24];
    notice[0] = '\0';

    for (;;) {
        int cols = screen_cols(), scr_rows = screen_rows();
        int penv = compute_primary_env_index();
        int nrows = compute_mgr_build(mrows, MAX_COMPUTE_MGR_ROWS, penv);
        if (sel < 0 || sel >= nrows) sel = 0;
        if (nrows > 0 && !compute_mgr_selectable(mrows[sel].kind)) {
            int next = compute_mgr_next(mrows, nrows, sel, 1);
            sel = next != sel ? next : compute_mgr_next(mrows, nrows, sel, -1);
        }
        int kind = nrows > 0 ? mrows[sel].kind : CMR_HEADER;
        int show_detail = (kind == CMR_DEFAULT || kind == CMR_ENV) && scr_rows >= 20;
        int list_y = 7;
        int height = scr_rows - list_y - (show_detail ? 6 : 4);
        if (height < 1) height = 1;
        if (sel < top) top = sel;
        if (sel >= top + height) top = sel - height + 1;
        if (top < 0) top = 0;
        const char *svol = compute_storage_cfg[0] ? compute_storage_cfg : compute_snap.storage_volume;

        screen_fill(0, 0, cols, scr_rows, BG);
        screen_bar(_("Compute Node Manager"), NULL);
        ui_hits_clear();

        /* Summary box (rows 2-5): API, storage, GPU, default container and inventory. */
        screen_fill(0, 2, cols, 4, SELECTION);
        snprintf(line, sizeof line, _("API: %s (port %d, %s)   Paired: %d   SSH: ssh %.32s -p %d"),
                 compute_on ? (compute_snap.daemon_running ? _("ON / running") : _("ON / starting")) : _("OFF"),
                 compute_snap.port,
                 compute_bind_tb ? _("Tailscale+TB") : _("Tailscale"),
                 compute_snap.paired_peers >= 0 ? compute_snap.paired_peers : 0,
                 compute_snap.ssh_prefix[0] ? compute_snap.ssh_prefix : "locachy",
                 compute_snap.ssh_port);
        screen_text(2, 2, line, FG, SELECTION);

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

        if (penv >= 0) {
            snprintf(line, sizeof line, _("Default: %.24s   Inventory: %d images, %d envs, %d/%d jobs running, %d/%d VMs running"),
                     compute_snap.envs[penv].name, compute_snap.nimages, compute_snap.nenvs,
                     compute_snap.njobs_running, compute_snap.njobs,
                     compute_snap.nvms_running, compute_snap.nvms);
        } else {
            snprintf(line, sizeof line, _("No default container yet.   Inventory: %d images, %d envs, %d/%d jobs running, %d/%d VMs running"),
                     compute_snap.nimages, compute_snap.nenvs,
                     compute_snap.njobs_running, compute_snap.njobs,
                     compute_snap.nvms_running, compute_snap.nvms);
        }
        screen_text(2, 5, line, DIM, SELECTION);

        for (int r = 0; r < height && top + r < nrows; r++) {
            int idx = top + r, y = list_y + r, on = idx == sel;
            const struct compute_mgr_row *mr = &mrows[idx];
            if (mr->kind == CMR_HEADER) {
                screen_text(2, y, mr->header, ACCENT_TEXT, BG);
                continue;
            }
            if (mr->kind == CMR_NO_JOBS) {
                screen_text(4, y, _("No compute jobs recorded yet."), DIM, BG);
                continue;
            }
            compute_mgr_row_text(mr, penv, svol, compute_snap.storage_mounted, line, sizeof line);
            screen_fill(0, y, cols, 1, on ? ACCENT : BG);
            screen_text(3, y, line, on ? ON_ACCENT : FG, on ? ACCENT : BG);
            ui_hit(0, y, cols, 1, idx);
        }

        if (show_detail) {
            const struct compute_env_row *e = &compute_snap.envs[mrows[sel].arg];
            char cpubuf[64];
            compute_format_cpu(e->cpu_max, cpubuf, sizeof cpubuf);
            screen_fill(0, scr_rows - 6, cols, 2, SELECTION);
            snprintf(line, sizeof line, _("Container: %.24s%s   image=%.24s   memory=%s   cpu=%s   workdir=%.40s"),
                     e->name, mrows[sel].arg == penv ? _("  [default]") : "",
                     e->image, e->memory_max[0] ? e->memory_max : _("unlimited"), cpubuf,
                     e->workdir[0] ? e->workdir : "/");
            screen_text(2, scr_rows - 6, line, FG, SELECTION);
            snprintf(line, sizeof line, _("Volumes: %.80s   Extra mounts: %.80s"),
                     e->volumes[0] ? e->volumes : _("none"), e->mounts[0] ? e->mounts : _("none"));
            screen_text(2, scr_rows - 5, line, DIM, SELECTION);
        }

        if (notice[0]) screen_text(2, scr_rows - 4, notice, ACCENT_TEXT, BG);
        const char *hint;
        switch (kind) {
        case CMR_DEFAULT:
            hint = _("Left/Right: switch default   Enter: open shell   e: settings   t: GPU test   r: refresh   Esc: back");
            break;
        case CMR_ENV:
            hint = _("Enter: open shell   e: settings   p: make default   t: GPU test   x: reset   Del: remove   Esc: back");
            break;
        case CMR_VM:
            hint = _("Enter: start or shell   s: stop   a: export to Home   x: reset   Del: remove   Esc: back");
            break;
        case CMR_JOB:
            hint = _("Enter: view log   s: stop job   r: refresh   Esc: back");
            break;
        case CMR_IMAGE:
            hint = _("Enter: details   Del: remove unused image   Esc: back");
            break;
        default:
            hint = _("Up/Down: move   Enter: run action   r: refresh   Esc: back");
            break;
        }
        screen_text(2, scr_rows - 3, hint, ACCENT_TEXT, BG);
        settings_back_button(scr_rows - 2);
        screen_flush();

        int key = key_get(), mods = mods_now(), act = key_list_action(key, mods);
        if (close_read(key)) return;
        if (key == K_REPAINT || key == K_SHOWN) continue;
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (settings_back_clicked(e)) return;
            if (e->wheel && nrows > 0) {
                int dir = e->wheel > 0 ? -1 : 1;
                int steps = abs(e->wheel);
                for (int s = 0; s < steps; s++) sel = compute_mgr_next(mrows, nrows, sel, dir);
                confirm_key = 0;
                continue;
            }
            if (e->button == BTN_LEFT && e->pressed) {
                int hit = ui_hit_at(e->col, e->row);
                if (hit >= 0 && hit < nrows && compute_mgr_selectable(mrows[hit].kind)) {
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
        if (act != ACT_NONE || (key >= ' ' && key < 0x7F && (mods & (KEYS_ALT | KEYS_META)))) continue;

        int ckey = (key >= 'A' && key <= 'Z') ? key + ('a' - 'A') : key;
        int confirming = confirm_key != 0 && ckey == confirm_key && sel == confirm_row;
        if (!confirming) {
            confirm_key = 0;
            confirm_row = -1;
        }
        const struct compute_mgr_row *cur = nrows > 0 ? &mrows[sel] : NULL;

        if (key == K_UP) {
            sel = compute_mgr_next(mrows, nrows, sel, -1);
        } else if (key == K_DOWN) {
            sel = compute_mgr_next(mrows, nrows, sel, 1);
        } else if (key == K_PGUP) {
            for (int s = 0; s < height; s++) sel = compute_mgr_next(mrows, nrows, sel, -1);
        } else if (key == K_PGDN) {
            for (int s = 0; s < height; s++) sel = compute_mgr_next(mrows, nrows, sel, 1);
        } else if (key == K_BOL) {
            int first = compute_mgr_next(mrows, nrows, -1, 1);
            if (first >= 0) sel = first;
        } else if (key == K_EOL) {
            int last = compute_mgr_next(mrows, nrows, nrows, -1);
            if (last >= 0 && last < nrows) sel = last;
        } else if (ckey == 'r') {
            load_storage_status();
            load_compute_status();
            snprintf(notice, sizeof notice, "%s", _("Refreshed Compute Node status."));
        } else if (ckey == 'm') {
            compute_mount_storage_action();
            load_compute_status();
        } else if (ckey == 'c') {
            compute_view_ssh_config();
            if (close_due()) return;
        } else if ((key == K_LEFT || key == K_RIGHT) && kind == CMR_DEFAULT && compute_snap.nenvs > 1) {
            int next = (penv + (key == K_RIGHT ? 1 : compute_snap.nenvs - 1)) % compute_snap.nenvs;
            compute_set_default_env(compute_snap.envs[next].name);
        } else if (key == '\n' && cur) {
            if (kind == CMR_DEFAULT || kind == CMR_ENV) {
                char name[64];
                compute_copy_field(name, sizeof name, compute_snap.envs[cur->arg].name);
                compute_open_env_shell(name);
            } else if (kind == CMR_DEFAULT_SETUP || kind == CMR_NEW_ENV) {
                compute_env_form(NULL);
                if (close_due()) return;
            } else if (kind == CMR_IMAGE) {
                const struct compute_img_row *im = &compute_snap.images[cur->arg];
                snprintf(notice, sizeof notice, _("Image '%.32s' (imported %.20s, from %.60s)."),
                         im->name, im->imported_at, im->source);
            } else if (kind == CMR_NEW_IMAGE) {
                compute_fetch_image_screen(NULL, 0);
                if (close_due()) return;
                load_compute_status();
            } else if (kind == CMR_VM) {
                struct compute_vm_row v = compute_snap.vms[cur->arg];
                if (!v.running) {
                    char out[256] = "";
                    char *const argv[] = { LOCACHY_COMPUTE, "vm", "start", v.name, NULL };
                    int rc = compute_exec(argv, out, sizeof out);
                    load_compute_status();
                    snprintf(notice, sizeof notice, rc == 0 ? _("Started VM '%.32s'.") : _("Start VM failed: %.120s"),
                             rc == 0 ? v.name : out);
                } else {
                    char *const shell[] = {
                        "/bin/sh", "-c",
                        "/usr/bin/sudo " LOCACHY_COMPUTE " shell --kind vm \"$1\"; s=$?; "
                        "printf '\\nVM shell exited (%d). Enter: back. ' \"$s\"; read -r _; exit $s",
                        "sh", v.name, NULL
                    };
                    in_term(shell);
                    load_compute_status();
                }
            } else if (kind == CMR_NEW_VM) {
                compute_vm_create_form();
                if (close_due()) return;
            } else if (kind == CMR_JOB) {
                static char logbuf[32768];
                struct compute_job_row j = compute_snap.jobs[cur->arg];
                char title[96];
                logbuf[0] = '\0';
                char *const argv[] = { LOCACHY_COMPUTE, "job", "log", j.job_id, NULL };
                int rc = compute_exec(argv, logbuf, sizeof logbuf);
                snprintf(title, sizeof title, _("Compute Job Log: %.48s"), j.job_id);
                compute_view_text_screen(title, rc == 0 && logbuf[0] ? logbuf : _("(no log output)"), _("job log"));
                if (close_due()) return;
            } else if (kind == CMR_STORAGE) {
                compute_mount_storage_action();
                load_compute_status();
            } else if (kind == CMR_SSH_CONFIG) {
                compute_view_ssh_config();
                if (close_due()) return;
            } else if (kind == CMR_PAIR) {
                compute_pair_client_interactive();
            }
        } else if (ckey == 'e' && cur && (kind == CMR_DEFAULT || kind == CMR_ENV)) {
            struct compute_env_row env = compute_snap.envs[cur->arg];
            compute_env_form(&env);
            if (close_due()) return;
        } else if (ckey == 'p' && cur && kind == CMR_ENV) {
            compute_set_default_env(compute_snap.envs[cur->arg].name);
        } else if (ckey == 't' && cur && (kind == CMR_DEFAULT || kind == CMR_ENV)) {
            char name[64];
            compute_copy_field(name, sizeof name, compute_snap.envs[cur->arg].name);
            compute_test_env_gpu(name);
        } else if (ckey == 'x' && cur && (kind == CMR_DEFAULT || kind == CMR_ENV || kind == CMR_VM)) {
            int is_vm = kind == CMR_VM;
            char name[64];
            compute_copy_field(name, sizeof name,
                               is_vm ? compute_snap.vms[cur->arg].name : compute_snap.envs[cur->arg].name);
            if (!confirming) {
                confirm_key = 'x';
                confirm_row = sel;
                snprintf(notice, sizeof notice,
                         _("Press x again to reset '%.32s' (discards its writable changes; base image and volumes stay)."),
                         name);
            } else {
                char out[256] = "";
                char *const argv[] = { LOCACHY_COMPUTE, is_vm ? "vm" : "env", "reset", name, NULL };
                int rc = compute_exec(argv, out, sizeof out);
                confirm_key = 0;
                confirm_row = -1;
                load_compute_status();
                snprintf(notice, sizeof notice, rc == 0 ? _("Reset '%.32s'.") : _("Reset failed: %.120s"),
                         rc == 0 ? name : out);
            }
        } else if (key == K_DEL && cur && (kind == CMR_ENV || kind == CMR_IMAGE || kind == CMR_VM)) {
            char name[64];
            const char *what = kind == CMR_ENV ? "env" : kind == CMR_IMAGE ? "image" : "vm";
            if (kind == CMR_ENV) compute_copy_field(name, sizeof name, compute_snap.envs[cur->arg].name);
            else if (kind == CMR_IMAGE) compute_copy_field(name, sizeof name, compute_snap.images[cur->arg].name);
            else compute_copy_field(name, sizeof name, compute_snap.vms[cur->arg].name);
            if (kind == CMR_VM && compute_snap.vms[cur->arg].running) {
                snprintf(notice, sizeof notice, _("Stop VM '%.32s' (s) before removing it."), name);
            } else if (!confirming) {
                confirm_key = K_DEL;
                confirm_row = sel;
                snprintf(notice, sizeof notice, _("Press Delete again to remove '%.32s'."), name);
            } else {
                char out[256] = "";
                char *const argv[] = { LOCACHY_COMPUTE, (char *)what, "remove", name, NULL };
                int rc = compute_exec(argv, out, sizeof out);
                confirm_key = 0;
                confirm_row = -1;
                load_compute_status();
                snprintf(notice, sizeof notice, rc == 0 ? _("Removed '%.32s'.") : _("Remove failed: %.120s"),
                         rc == 0 ? name : out);
            }
        } else if (ckey == 's' && cur && kind == CMR_VM) {
            char name[64];
            compute_copy_field(name, sizeof name, compute_snap.vms[cur->arg].name);
            char out[256] = "";
            char *const argv[] = { LOCACHY_COMPUTE, "vm", "stop", name, NULL };
            int rc = compute_exec(argv, out, sizeof out);
            load_compute_status();
            snprintf(notice, sizeof notice, rc == 0 ? _("Stopped VM '%.32s'.") : _("Stop VM failed: %.120s"),
                     rc == 0 ? name : out);
        } else if (ckey == 's' && cur && kind == CMR_JOB) {
            char jid[64];
            compute_copy_field(jid, sizeof jid, compute_snap.jobs[cur->arg].job_id);
            char out[256] = "";
            char *const argv[] = { LOCACHY_COMPUTE, "job", "stop", jid, NULL };
            int rc = compute_exec(argv, out, sizeof out);
            load_compute_status();
            snprintf(notice, sizeof notice, rc == 0 ? _("Stopped compute job %.32s.") : _("Stop failed: %.120s"),
                     rc == 0 ? jid : out);
        } else if (ckey == 'a' && cur && kind == CMR_VM) {
            char name[64];
            compute_copy_field(name, sizeof name, compute_snap.vms[cur->arg].name);
            char out[256] = "";
            char *const argv[] = { LOCACHY_COMPUTE, "vm", "export-app", name,
                                   "--display", "gtk", "--freeze", "qmp", "--network", "1", NULL };
            int rc = compute_exec(argv, out, sizeof out);
            load_compute_status();
            snprintf(notice, sizeof notice, rc == 0 ?
                     _("Exported VM '%.32s' to Home (see Compute & VMs > Virtual Machines).") :
                     _("Export failed: %.120s"),
                     rc == 0 ? name : out);
        }
    }
}
