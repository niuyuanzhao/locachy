/* System status: NetworkManager's active connection, battery, volume and
 * clock, as one line (base_status); as fields for a bar with two rows, with
 * also the microphone, memory and CPU (base_status_fields). */
#ifdef CONFIG_NETWORKMANAGER
#include <NetworkManager.h>
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "base.h"

static char cached_network[128] = N_("Network: --");
static char cached_bt[64];
static char cached_bat[64] = N_("Bat: --");
static char cached_audio[48] = N_("Vol: --");
static struct status_fields fields = {
    .volume = N_("Vol --"), .network = N_("Network"), .address = "--", .battery = N_("Bat --"),
    .memory = N_("RAM --"), .cpu = N_("CPU --"),
};
static int want_fields; /* the microphone, memory and CPU are read only for fields */
static double last_poll = -10.0;
static char last_lang[32];

static int read_first_line(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) {
        *nl = '\0';
    }
    return 0;
}

static void update_network(void)
{
#ifdef CONFIG_NETWORKMANAGER
    static NMClient *client;
    if (client == NULL) {
        client = nm_client_new(NULL, NULL);
    }
    while (g_main_context_pending(NULL)) {
        g_main_context_iteration(NULL, FALSE);
    }
    NMActiveConnection *active = client ? nm_client_get_primary_connection(client) : NULL;
    if (active == NULL) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(cached_network, sizeof cached_network, "%s", _("Network: disconnected"));
        snprintf(fields.network, sizeof fields.network, "%s", _("Network"));
        snprintf(fields.address, sizeof fields.address, "%s", _("disconnected"));
        return;
    }
    const char *type = nm_active_connection_get_connection_type(active);
    /* TRANSLATORS: status bar: keep short. */
    const char *label = type && strcmp(type, NM_SETTING_WIRED_SETTING_NAME) == 0 ? _("Ethernet") :
                        type && strcmp(type, NM_SETTING_WIRELESS_SETTING_NAME) == 0 ? _("Wi-Fi") : _("Network");
    NMIPConfig *config = nm_active_connection_get_ip4_config(active);
    const GPtrArray *addresses = config ? nm_ip_config_get_addresses(config) : NULL;
    if (addresses == NULL || addresses->len == 0) {
        config = nm_active_connection_get_ip6_config(active);
        addresses = config ? nm_ip_config_get_addresses(config) : NULL;
    }
    const char *address = addresses && addresses->len ?
        nm_ip_address_get_address(g_ptr_array_index(addresses, 0)) : NULL;
    const char *id = nm_active_connection_get_id(active);
    if (address != NULL) {
        snprintf(cached_network, sizeof cached_network, "%s: %.48s (%.45s)", label, id, address);
    } else {
        snprintf(cached_network, sizeof cached_network, "%s: %.48s", label, id);
    }
    snprintf(fields.network, sizeof fields.network, "%s %.48s", label, id);
    /* TRANSLATORS: status bar: keep short. */
    snprintf(fields.address, sizeof fields.address, "%.48s", address ? address : _("no address"));
#else
    /* TRANSLATORS: status bar: keep short. */
    snprintf(cached_network, sizeof cached_network, "%s", _("Network: --"));
#endif
}

static void update_battery(void)
{
    char cap_s[16], ac_s[16], pwr_s[32], eng_s[32], state[32] = "";
    int cap = -1, ac = 0;
    double w = 0;
    long mins = 0;

    if (read_first_line("/sys/class/power_supply/BAT0/capacity", cap_s, sizeof cap_s) == 0) {
        cap = atoi(cap_s);
    }
    if (read_first_line("/sys/class/power_supply/AC/online", ac_s, sizeof ac_s) == 0) {
        ac = atoi(ac_s);
    }
    if (cap < 0) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(cached_bat, sizeof cached_bat, "%s", ac ? _("AC: on") : _("AC: off"));
        snprintf(fields.battery, sizeof fields.battery, "%s", ac ? _("AC on") : _("AC off"));
        fields.power[0] = '\0';
        return;
    }
    read_first_line("/sys/class/power_supply/BAT0/status", state, sizeof state);
    if (read_first_line("/sys/class/power_supply/BAT0/power_now", pwr_s, sizeof pwr_s) == 0) {
        long long pwr_uw = atoll(pwr_s);
        w = pwr_uw / 1e6;
        if (!ac && w > 0.1 &&
            read_first_line("/sys/class/power_supply/BAT0/energy_now", eng_s, sizeof eng_s) == 0) {
            mins = (long)((atoll(eng_s) * 60.0) / (double)pwr_uw + 0.5);
        }
    }
    /* TRANSLATORS: status bar: keep short. */
    snprintf(fields.battery, sizeof fields.battery, _("Bat %d%%"), cap);
    if (ac) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(cached_bat, sizeof cached_bat, _("Bat: %d%% (AC)"), cap);
        if (strcmp(state, "Charging") == 0) {
            /* TRANSLATORS: status bar: keep short. */
            snprintf(fields.power, sizeof fields.power, w > 0.1 ? _("charging, %.1f W") : _("charging"), w);
        } else {
            /* TRANSLATORS: status bar: keep short. */
            snprintf(fields.power, sizeof fields.power, "%s",
                     strcmp(state, "Full") == 0 ? _("AC, full") : _("AC"));
        }
    } else if (w > 0.1 && mins > 0) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(cached_bat, sizeof cached_bat, _("Bat: %d%% (%.1fW, %ldh%02ldm)"),
                 cap, w, mins / 60, mins % 60);
        snprintf(fields.power, sizeof fields.power, _("%.1f W, %ldh%02ldm"), w, mins / 60, mins % 60);
    } else if (w > 0.1) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(cached_bat, sizeof cached_bat, _("Bat: %d%% (%.1fW)"), cap, w);
        snprintf(fields.power, sizeof fields.power, "%.1f W", w);
    } else {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(cached_bat, sizeof cached_bat, _("Bat: %d%%"), cap);
        fields.power[0] = '\0';
    }
}

/* wpctl get-volume NODE: 0, the volume in percent and whether it is muted. */
static int wpctl_volume(const char *node, int *pct, int *muted)
{
    char out[64] = "";
    int p[2], got = -1;
    if (pipe(p) < 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        close(p[1]);
        execl("/usr/bin/wpctl", "wpctl", "get-volume", node, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid > 0) {
        ssize_t n = read(p[0], out, sizeof out - 1);
        double v = 0;
        if (n > 0) {
            out[n] = '\0';
            if (sscanf(out, "Volume: %lf", &v) == 1) {
                *pct = (int)(v * 100.0 + 0.5);
                *muted = strstr(out, "[MUTED]") != NULL;
                got = 0;
            }
        }
        waitpid(pid, NULL, 0);
    }
    close(p[0]);
    return got;
}

static void update_audio(void)
{
    int pct, muted;
    if (access("/run/user/0/pipewire-0", F_OK) != 0) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(cached_audio, sizeof cached_audio, "%s", _("Vol: --"));
        snprintf(fields.volume, sizeof fields.volume, "%s", _("Vol --"));
        fields.mic[0] = '\0';
        return;
    }
    if (wpctl_volume("@DEFAULT_AUDIO_SINK@", &pct, &muted) == 0) {
        if (muted) {
            /* TRANSLATORS: status bar: keep short. */
            snprintf(cached_audio, sizeof cached_audio, _("Vol: MUTE(%d%%)"), pct);
            snprintf(fields.volume, sizeof fields.volume, "%s", _("Vol muted"));
        } else {
            /* TRANSLATORS: status bar: keep short. */
            snprintf(cached_audio, sizeof cached_audio, _("Vol: %d%%"), pct);
            snprintf(fields.volume, sizeof fields.volume, _("Vol %d%%"), pct);
        }
    }
    if (want_fields && wpctl_volume("@DEFAULT_AUDIO_SOURCE@", &pct, &muted) == 0) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(fields.mic, sizeof fields.mic, muted ? _("Mic muted") : _("Mic %d%%"), pct);
    }
}

static void update_memory(void)
{
    FILE *in = fopen("/proc/meminfo", "re");
    char line[128];
    long long total = -1, avail = -1, swap = -1, swap_free = -1, v;
    while (in && fgets(line, sizeof line, in)) {
        if (sscanf(line, "MemTotal: %lld", &v) == 1) {
            total = v;
        } else if (sscanf(line, "MemAvailable: %lld", &v) == 1) {
            avail = v;
        } else if (sscanf(line, "SwapTotal: %lld", &v) == 1) {
            swap = v;
        } else if (sscanf(line, "SwapFree: %lld", &v) == 1) {
            swap_free = v;
        }
    }
    if (in) {
        fclose(in);
    }
    const double gib = 1024.0 * 1024.0; /* the file is in KiB */
    if (total > 0 && avail >= 0) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(fields.memory, sizeof fields.memory, _("RAM %.1f/%.1fG"), (total - avail) / gib, total / gib);
    } else {
        snprintf(fields.memory, sizeof fields.memory, "%s", _("RAM --"));
    }
    if (swap > 0 && swap_free >= 0) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(fields.swap, sizeof fields.swap, _("Swap %.1f/%.1fG"), (swap - swap_free) / gib, swap / gib);
    } else if (swap == 0) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(fields.swap, sizeof fields.swap, "%s", _("Swap none"));
    }
}

/* CPU use since the last poll, and the package temperature. */
static void update_cpu(void)
{
    static unsigned long long last_total, last_idle;
    static char zone[64];
    unsigned long long v[8];
    FILE *in = fopen("/proc/stat", "re");
    if (in && fscanf(in, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                     &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) {
        unsigned long long idle = v[3] + v[4], total = 0;
        for (int i = 0; i < 8; i++) {
            total += v[i];
        }
        if (last_total && total > last_total) {
            double busy = 1.0 - (double)(idle - last_idle) / (double)(total - last_total);
            /* TRANSLATORS: status bar: keep short. */
            snprintf(fields.cpu, sizeof fields.cpu, _("CPU %d%%"), (int)(busy * 100 + 0.5));
        } else {
            snprintf(fields.cpu, sizeof fields.cpu, "%s", _("CPU --"));
        }
        last_total = total;
        last_idle = idle;
    }
    if (in) {
        fclose(in);
    }
    for (int i = 0; !zone[0] && i < 16; i++) {
        char path[64], type[32];
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", i);
        if (read_first_line(path, type, sizeof type) < 0) {
            break;
        }
        if (strcmp(type, "x86_pkg_temp") == 0) {
            snprintf(zone, sizeof zone, "/sys/class/thermal/thermal_zone%d/temp", i);
        }
    }
    char temp[16];
    if (zone[0] && read_first_line(zone, temp, sizeof temp) == 0) {
        snprintf(fields.temp, sizeof fields.temp, "%d°C", atoi(temp) / 1000);
    }
}

static void bt_rc(const char *name, const char *value, void *arg)
{
    if (strcmp(name, "service_bluetooth") == 0 && value[0]) {
        *(int *)arg = 1;
    }
}

static void update_bluetooth(void)
{
    int enabled = 0;
    rc_read(bt_rc, &enabled);
    if (!enabled || access("/usr/lib/bluetooth/bluetoothd", X_OK) != 0) {
        cached_bt[0] = fields.bluetooth[0] = fields.bt_device[0] = '\0';
        return;
    }
#ifdef CONFIG_NETWORKMANAGER
    static GDBusConnection *sys_bus;
    if (!sys_bus) {
        sys_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    }
    GVariant *reply = sys_bus ? g_dbus_connection_call_sync(sys_bus, "org.bluez", "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 150, NULL, NULL) : NULL;
    if (!reply) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(fields.bluetooth, sizeof fields.bluetooth, "%s", _("BT off"));
        snprintf(fields.bt_device, sizeof fields.bt_device, "%s", _("starting"));
        snprintf(cached_bt, sizeof cached_bt, "%s", _("BT: off"));
        return;
    }
    int has_adapter = 0, powered = 0, nconnected = 0, dev_pct = -1;
    char dev_name[48] = "";
    GVariantIter *objs = NULL;
    const gchar *obj_path;
    GVariant *ifaces;
    g_variant_get(reply, "(a{oa{sa{sv}}})", &objs);
    while (g_variant_iter_next(objs, "{&o@a{sa{sv}}}", &obj_path, &ifaces)) {
        GVariant *adapter = g_variant_lookup_value(ifaces, "org.bluez.Adapter1", G_VARIANT_TYPE_VARDICT);
        if (adapter) {
            gboolean p = FALSE;
            has_adapter = 1;
            if (g_variant_lookup(adapter, "Powered", "b", &p) && p) powered = 1;
            g_variant_unref(adapter);
        }
        GVariant *dev = g_variant_lookup_value(ifaces, "org.bluez.Device1", G_VARIANT_TYPE_VARDICT);
        if (dev) {
            gboolean conn = FALSE;
            if (g_variant_lookup(dev, "Connected", "b", &conn) && conn) {
                const gchar *alias = NULL;
                if (nconnected == 0 && (g_variant_lookup(dev, "Alias", "&s", &alias) ||
                                        g_variant_lookup(dev, "Name", "&s", &alias)) && alias) {
                    snprintf(dev_name, sizeof dev_name, "%s", alias);
                }
                GVariant *bat = g_variant_lookup_value(ifaces, "org.bluez.Battery1", G_VARIANT_TYPE_VARDICT);
                if (bat) {
                    guchar pct = 0;
                    if (nconnected == 0 && g_variant_lookup(bat, "Percentage", "y", &pct)) dev_pct = pct;
                    g_variant_unref(bat);
                }
                nconnected++;
            }
            g_variant_unref(dev);
        }
        g_variant_unref(ifaces);
    }
    g_variant_iter_free(objs);
    g_variant_unref(reply);
    if (!has_adapter || !powered) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(fields.bluetooth, sizeof fields.bluetooth, "%s", _("BT off"));
        snprintf(fields.bt_device, sizeof fields.bt_device, "%s",
                 has_adapter ? _("unpowered") : _("no adapter"));
        snprintf(cached_bt, sizeof cached_bt, "%s", _("BT: off"));
    } else if (nconnected == 0) {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(fields.bluetooth, sizeof fields.bluetooth, "%s", _("BT on"));
        snprintf(fields.bt_device, sizeof fields.bt_device, "%s", _("idle"));
        snprintf(cached_bt, sizeof cached_bt, "%s", _("BT: on"));
    } else {
        if (dev_pct >= 0 && nconnected == 1) {
            /* TRANSLATORS: status bar: keep short. */
            snprintf(fields.bluetooth, sizeof fields.bluetooth, _("BT %d%%"), dev_pct);
        } else if (nconnected > 1) {
            /* TRANSLATORS: status bar: keep short. */
            snprintf(fields.bluetooth, sizeof fields.bluetooth, _("BT (%d)"), nconnected);
        } else {
            /* TRANSLATORS: status bar: keep short. */
            snprintf(fields.bluetooth, sizeof fields.bluetooth, "%s", _("BT on"));
        }
        /* TRANSLATORS: status bar: keep short. */
        snprintf(fields.bt_device, sizeof fields.bt_device, "%s",
                 dev_name[0] ? dev_name : _("connected"));
        snprintf(cached_bt, sizeof cached_bt, _("BT: %s"),
                 dev_name[0] ? dev_name : _("connected"));
    }
#else
    /* TRANSLATORS: status bar: keep short. */
    snprintf(fields.bluetooth, sizeof fields.bluetooth, "%s", _("BT on"));
    snprintf(fields.bt_device, sizeof fields.bt_device, "%s", _("enabled"));
    snprintf(cached_bt, sizeof cached_bt, "%s", _("BT: on"));
#endif
}

static void poll_status(void)
{
    double now = uptime();
    const char *lang = base_language();
    if (strcmp(lang, last_lang) != 0) {
        snprintf(last_lang, sizeof last_lang, "%s", lang);
        last_poll = -10.0;
    }
    if (now - last_poll >= 2.0) {
        last_poll = now;
        update_network();
        update_bluetooth();
        update_battery();
        update_audio();
        if (want_fields) {
            update_memory();
            update_cpu();
        }
    }
}

int base_audio_key(int action_key, char *toast, size_t size)
{
    if (access("/run/user/0/pipewire-0", F_OK) != 0) {
        return -1;
    }
    const char *args[5] = { "wpctl", NULL, "@DEFAULT_AUDIO_SINK@", NULL, NULL };
    if (action_key == K_VOL_UP) {
        pid_t upid = fork();
        if (upid == 0) {
            execl("/usr/bin/wpctl", "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "0", (char *)NULL);
            _exit(127);
        }
        if (upid > 0) waitpid(upid, NULL, 0);
        args[1] = "set-volume";
        args[2] = "-l";
        args[3] = "1.0";
        args[4] = "@DEFAULT_AUDIO_SINK@";
    } else if (action_key == K_VOL_DOWN) {
        args[1] = "set-volume";
        args[3] = "5%-";
    } else if (action_key == K_VOL_MUTE) {
        args[1] = "set-mute";
        args[3] = "toggle";
    } else if (action_key == K_MIC_MUTE) {
        args[1] = "set-mute";
        args[2] = "@DEFAULT_AUDIO_SOURCE@";
        args[3] = "toggle";
    } else {
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (action_key == K_VOL_UP) {
            execl("/usr/bin/wpctl", "wpctl", "set-volume", "-l", "1.0",
                  "@DEFAULT_AUDIO_SINK@", "5%+", (char *)NULL);
        } else {
            execl("/usr/bin/wpctl", args[0], args[1], args[2], args[3], (char *)NULL);
        }
        _exit(127);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
        update_audio();
    }
    if (toast && size) {
        snprintf(toast, size, " [%s] ", cached_audio);
    }
    return pid > 0 ? 0 : -1;
}

/* The local time, in the time zone of /etc/localtime as it is now: tzset
 * reads it again when it has changed (settings, or the automatic time zone),
 * which localtime_r alone does not. */
static struct tm *local_now(struct tm *buf)
{
    time_t t = time(NULL);
    tzset();
    return localtime_r(&t, buf);
}

void base_status(const char *ime_tag, char *out, size_t size)
{
    poll_status();
    struct tm tm_buf;
    struct tm *tm = local_now(&tm_buf);
    char clk[32] = "--:--";
    if (tm != NULL) {
        strftime(clk, sizeof clk, "%Y-%m-%d %H:%M:%S", tm);
    }

    char bt_part[80] = "";
    if (cached_bt[0] != '\0') {
        snprintf(bt_part, sizeof bt_part, "  |  %s", cached_bt);
    }
    if (ime_tag != NULL && ime_tag[0] != '\0') {
        /* TRANSLATORS: status bar: keep short. */
        snprintf(out, size, "%s: [%s]  |  %s%s  |  %s  |  %s  |  %s",
                 _("IME"), ime_tag, cached_audio, bt_part, cached_network, cached_bat, clk);
    } else {
        snprintf(out, size, "%s%s  |  %s  |  %s  |  %s",
                 cached_audio, bt_part, cached_network, cached_bat, clk);
    }
}

void base_status_fields(struct status_fields *out)
{
    if (!want_fields) {
        want_fields = 1;
        last_poll = -10.0; /* read the microphone, memory and CPU now */
    }
    poll_status();
    struct tm tm_buf;
    struct tm *tm = local_now(&tm_buf);
    if (tm != NULL) {
        strftime(fields.time, sizeof fields.time, "%H:%M:%S", tm);
        strftime(fields.date, sizeof fields.date, "%a %Y-%m-%d", tm);
    }
    *out = fields;
}
