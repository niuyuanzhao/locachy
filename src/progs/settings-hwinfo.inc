/*
 * About This System summary and interactive Hardware Information browser
 * (included by settings.c).
 *
 * Reads standard Linux kernel interfaces (/proc, /sys, SMBIOS/DMI binary
 * tables, DRM EDID blocks, hwmon/thermal/RAPL sensors, NVMe/ATA SMART health
 * ioctls, and /usr/share/hwdata/{pci.ids,usb.ids,pnp.ids}) without GTK/Qt or
 * external CLI dependencies.
 */

#include <sys/utsname.h>
#ifdef __linux__
#include <sched.h>
#include <sys/mount.h>
#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>
#endif

static double hw_sqrt(double x)
{
    if (x <= 0.0) return 0.0;
    double g = x > 1.0 ? x * 0.5 : 1.0;
    for (int i = 0; i < 12; i++) g = 0.5 * (g + x / g);
    return g;
}

#ifndef HW_PROC_ROOT
#define HW_PROC_ROOT "/proc"
#endif
#ifndef HW_SYS_ROOT
#define HW_SYS_ROOT "/sys"
#endif
#ifndef HW_HWDATA_DIR
#define HW_HWDATA_DIR "/usr/share/hwdata"
#endif
#ifndef HW_OS_RELEASE
#define HW_OS_RELEASE "/etc/os-release"
#endif
#ifndef HW_SOURCE_COMMIT
#define HW_SOURCE_COMMIT "/var/lib/locachy/source-commit"
#endif
#ifndef HW_NETNS_LOCACHY
#define HW_NETNS_LOCACHY "/run/netns/locachy-net"
#endif

enum {
    HWCAT_SUMMARY,
    HWCAT_CPU,
    HWCAT_MEMORY,
    HWCAT_STORAGE,
    HWCAT_GRAPHICS,
    HWCAT_SENSORS,
    HWCAT_PCI,
    HWCAT_USB,
    HWCAT_NETWORK,
    HWCAT_FIRMWARE,
    NHWCATS
};

static const char *const hwcat_names[NHWCATS] = {
    N_("Summary"),
    N_("Processor"),
    N_("Memory & Slots"),
    N_("Storage & SMART"),
    N_("Graphics & Displays"),
    N_("Sensors (Live)"),
    N_("PCI & PCIe Bus"),
    N_("USB Devices"),
    N_("Network Adapters"),
    N_("Firmware & DMI"),
};

#define HW_MAX_ROWS 384
enum { HWROW_SECTION = 1, HWROW_KV = 2, HWROW_NOTE = 3, HWROW_BAR = 4, HWROW_SENSOR = 5 };

struct hw_row {
    int kind;
    char key[72];
    char val[160];
    uint32_t val_col;
    int bar_pct;    /* 0..100 for HWROW_BAR */
    int sensor_idx; /* >= 0 for HWROW_SENSOR */
};

struct hw_sensor {
    char chip[48];
    char label[64];
    char kind;       /* 'T' temp C, 'F' fan RPM, 'P' power W, 'V' voltage V */
    double val;
    double min_v, max_v, crit_v;
    int has_crit;
};

#define HW_MAX_SENSORS 96
#define HW_SPARK_LEN   120

static struct hw_row hw_rows[HW_MAX_ROWS];
static int nhw_rows;
static struct hw_sensor hw_sensors[HW_MAX_SENSORS];
static int nhw_sensors;
static double hw_spark[HW_SPARK_LEN];
static int hw_spark_n;
static char hw_spark_key[120];

/* RAPL delta state for live CPU package power */
static unsigned long long hw_rapl_prev_uj;
static double hw_rapl_prev_time;
static double hw_rapl_watts = -1.0;

/* Cached one-line strings for the main CAT_SYSTEM "About This System" notes */
static int about_summary_loaded;
static char about_hw_model[128];
static char about_cpu_brand[128];
static char about_mem_total[64];
static char about_kernel_ver[96];
static char about_os_ver[96];

static inline size_t hw_scpy(char *dst, size_t size, const char *src)
{
    if (!size) return 0;
    size_t i = 0;
    if (src) {
        while (i + 1 < size && src[i]) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
    return i;
}

static int hw_read_line(const char *path, char *out, size_t size)
{
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)size, f)) {
        fclose(f);
        out[0] = '\0';
        return -1;
    }
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) out[--len] = '\0';
    char *s = out;
    while (*s == ' ' || *s == '\t') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
    return 0;
}

static long long hw_read_ll(const char *path, int *ok)
{
    char buf[64], *end = NULL;
    if (ok) *ok = 0;
    if (hw_read_line(path, buf, sizeof buf) != 0 || !buf[0]) return 0;
    errno = 0;
    long long v = strtoll(buf, &end, 0);
    if (errno || end == buf) return 0;
    if (ok) *ok = 1;
    return v;
}

static size_t hw_read_bytes(const char *path, uint8_t *buf, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, max, f);
    fclose(f);
    return n;
}

static double hw_mono_sec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int hw_meaningful_dmi(const char *s)
{
    if (!s || !s[0]) return 0;
    static const char *const junk[] = {
        "To be filled by O.E.M.", "To Be Filled By O.E.M.", "Default string",
        "System Product Name", "System Version", "Not Applicable", "None",
        "Unspecified", "Not Specified", "INVALID", "0123456789"
    };
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        if (strcasecmp(s, junk[i]) == 0) return 0;
    }
    return 1;
}

/* Strip ISA/microarch suffix tokens or "(R)"/"(TM)" clutter for clean display */
static void hw_clean_cpu_name(char *s)
{
    static const char *const drop[] = { "(R)", "(TM)", "(tm)", " CPU" };
    for (size_t i = 0; i < sizeof drop / sizeof drop[0]; i++) {
        char *p;
        size_t dlen = strlen(drop[i]);
        while ((p = strstr(s, drop[i])) != NULL) {
            memmove(p, p + dlen, strlen(p + dlen) + 1);
        }
    }
    char *w = s;
    int prev_sp = 1;
    for (const char *r = s; *r; r++) {
        int sp = (*r == ' ' || *r == '\t');
        if (sp && prev_sp) continue;
        *w++ = sp ? ' ' : *r;
        prev_sp = sp;
    }
    if (w > s && w[-1] == ' ') w--;
    *w = '\0';
}

static void load_about_summary(void)
{
    char vendor[64] = "", prod[64] = "", ver[64] = "", bios[64] = "", path[256];
    snprintf(path, sizeof path, "%s/class/dmi/id/sys_vendor", HW_SYS_ROOT);
    hw_read_line(path, vendor, sizeof vendor);
    snprintf(path, sizeof path, "%s/class/dmi/id/product_name", HW_SYS_ROOT);
    hw_read_line(path, prod, sizeof prod);
    snprintf(path, sizeof path, "%s/class/dmi/id/product_version", HW_SYS_ROOT);
    hw_read_line(path, ver, sizeof ver);
    snprintf(path, sizeof path, "%s/class/dmi/id/bios_version", HW_SYS_ROOT);
    hw_read_line(path, bios, sizeof bios);

    about_hw_model[0] = '\0';
    if (hw_meaningful_dmi(vendor) && hw_meaningful_dmi(prod)) {
        if (hw_meaningful_dmi(ver) && strstr(prod, ver) == NULL &&
            (strcasecmp(vendor, "LENOVO") == 0 || strlen(ver) < 24)) {
            snprintf(about_hw_model, sizeof about_hw_model, "%.28s %.36s (%.24s)", vendor, prod, ver);
        } else {
            snprintf(about_hw_model, sizeof about_hw_model, "%.32s %.56s", vendor, prod);
        }
    } else if (hw_meaningful_dmi(prod)) {
        snprintf(about_hw_model, sizeof about_hw_model, "%.80s", prod);
    }
    if (hw_meaningful_dmi(bios) && strlen(about_hw_model) + strlen(bios) + 10 < sizeof about_hw_model) {
        size_t n = strlen(about_hw_model);
        snprintf(about_hw_model + n, sizeof about_hw_model - n, "%sBIOS %s", n ? ", " : "", bios);
    }

    /* Kernel release without architecture suffix on main settings page */
    about_kernel_ver[0] = '\0';
    snprintf(path, sizeof path, "%s/sys/kernel/osrelease", HW_PROC_ROOT);
    if (hw_read_line(path, about_kernel_ver, sizeof about_kernel_ver) != 0) {
        struct utsname u;
        if (uname(&u) == 0) hw_scpy(about_kernel_ver, sizeof about_kernel_ver, u.release);
    }
    /* Strip any architecture token if present so main settings page never names one */
    char *arch_tok = strstr(about_kernel_ver, "x86-64");
    if (arch_tok) *arch_tok = '\0';
    arch_tok = strstr(about_kernel_ver, "znver");
    if (arch_tok) *arch_tok = '\0';

    /* CPU model name */
    about_cpu_brand[0] = '\0';
    snprintf(path, sizeof path, "%s/cpuinfo", HW_PROC_ROOT);
    FILE *fc = fopen(path, "r");
    char line[256];
    int logical_cpus = 0;
    while (fc && fgets(line, sizeof line, fc)) {
        if (strncmp(line, "processor", 9) == 0 && (line[9] == '\t' || line[9] == ' ' || line[9] == ':')) {
            logical_cpus++;
        } else if (!about_cpu_brand[0] && strncmp(line, "model name", 10) == 0) {
            char *colon = strchr(line, ':');
            if (colon) {
                colon++;
                while (*colon == ' ' || *colon == '\t') colon++;
                colon[strcspn(colon, "\r\n")] = '\0';
                hw_scpy(about_cpu_brand, sizeof about_cpu_brand, colon);
                hw_clean_cpu_name(about_cpu_brand);
                char *at = strstr(about_cpu_brand, "x86-64");
                if (at) *at = '\0';
                at = strstr(about_cpu_brand, "znver");
                if (at) *at = '\0';
            }
        }
    }
    if (fc) fclose(fc);
    if (about_cpu_brand[0] && logical_cpus > 0 && strlen(about_cpu_brand) + 16 < sizeof about_cpu_brand) {
        size_t n = strlen(about_cpu_brand);
        snprintf(about_cpu_brand + n, sizeof about_cpu_brand - n, " (%d threads)", logical_cpus);
    }

    /* Total RAM */
    about_mem_total[0] = '\0';
    snprintf(path, sizeof path, "%s/meminfo", HW_PROC_ROOT);
    FILE *fm = fopen(path, "r");
    while (fm && fgets(line, sizeof line, fm)) {
        unsigned long long kb = 0;
        if (sscanf(line, "MemTotal: %llu kB", &kb) == 1 && kb > 0) {
            snprintf(about_mem_total, sizeof about_mem_total, "%.1f GiB", (double)kb / (1024.0 * 1024.0));
            break;
        }
    }
    if (fm) fclose(fm);

    /* OS name + source commit */
    about_os_ver[0] = '\0';
    char pretty[64] = "LOCachy", commit[64] = "";
    FILE *fo = fopen(HW_OS_RELEASE, "r");
    while (fo && fgets(line, sizeof line, fo)) {
        if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
            char *v = line + 12;
            v[strcspn(v, "\r\n")] = '\0';
            if (*v == '"') {
                v++;
                char *q = strchr(v, '"');
                if (q) *q = '\0';
            }
            if (*v) hw_scpy(pretty, sizeof pretty, v);
            break;
        }
    }
    if (fo) fclose(fo);
    if (hw_read_line(HW_SOURCE_COMMIT, commit, sizeof commit) == 0 && strlen(commit) >= 7) {
        commit[7] = '\0';
        snprintf(about_os_ver, sizeof about_os_ver, "%s (%s)", pretty, commit);
    } else {
        hw_scpy(about_os_ver, sizeof about_os_ver, pretty);
    }
    about_summary_loaded = 1;
}

static void add_about_system_notes(void)
{
    if (!about_summary_loaded) load_about_summary();
    char line[128];
    if (about_os_ver[0] || about_kernel_ver[0]) {
        if (about_os_ver[0] && about_kernel_ver[0])
            snprintf(line, sizeof line, _("System: %.52s, Linux %.44s"), about_os_ver, about_kernel_ver);
        else
            snprintf(line, sizeof line, _("System: %.96s"), about_os_ver[0] ? about_os_ver : about_kernel_ver);
        add_entry(E_NOTE, line);
    }
    if (about_hw_model[0]) {
        snprintf(line, sizeof line, _("Model: %.104s"), about_hw_model);
        add_entry(E_NOTE, line);
    }
    if (about_cpu_brand[0] || about_mem_total[0]) {
        if (about_cpu_brand[0] && about_mem_total[0])
            snprintf(line, sizeof line, _("Hardware: %.76s, %.24s RAM"), about_cpu_brand, about_mem_total);
        else
            snprintf(line, sizeof line, _("Hardware: %.96s"), about_cpu_brand[0] ? about_cpu_brand : about_mem_total);
        add_entry(E_NOTE, line);
    }
}

/* --- Row builders for Hardware Information browser --- */

static void hw_clear_rows(void) { nhw_rows = 0; }

static void hw_add_section(const char *title)
{
    if (nhw_rows >= HW_MAX_ROWS) return;
    struct hw_row *r = &hw_rows[nhw_rows++];
    memset(r, 0, sizeof *r);
    r->kind = HWROW_SECTION;
    hw_scpy(r->key, sizeof r->key, title);
    r->val_col = ACCENT;
    r->sensor_idx = -1;
}

static void hw_add_kv_col(const char *key, const char *val, uint32_t col)
{
    if (nhw_rows >= HW_MAX_ROWS || !val || !val[0]) return;
    struct hw_row *r = &hw_rows[nhw_rows++];
    memset(r, 0, sizeof *r);
    r->kind = HWROW_KV;
    hw_scpy(r->key, sizeof r->key, key);
    hw_scpy(r->val, sizeof r->val, val);
    r->val_col = col;
    r->sensor_idx = -1;
}

static void hw_add_kv(const char *key, const char *val)
{
    hw_add_kv_col(key, val, FG);
}

static void hw_add_note(const char *text)
{
    if (nhw_rows >= HW_MAX_ROWS || !text || !text[0]) return;
    struct hw_row *r = &hw_rows[nhw_rows++];
    memset(r, 0, sizeof *r);
    r->kind = HWROW_NOTE;
    hw_scpy(r->val, sizeof r->val, text);
    r->val_col = DIM;
    r->sensor_idx = -1;
}

static void hw_add_bar(const char *key, int pct, const char *val)
{
    if (nhw_rows >= HW_MAX_ROWS) return;
    struct hw_row *r = &hw_rows[nhw_rows++];
    memset(r, 0, sizeof *r);
    r->kind = HWROW_BAR;
    hw_scpy(r->key, sizeof r->key, key);
    hw_scpy(r->val, sizeof r->val, val);
    r->bar_pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    r->val_col = FG;
    r->sensor_idx = -1;
}

static void hw_format_bytes(unsigned long long bytes, char *out, size_t size)
{
    if (bytes >= (1ULL << 40))
        snprintf(out, size, "%.2f TiB (%.2f TB)", (double)bytes / (double)(1ULL << 40), (double)bytes / 1e12);
    else if (bytes >= (1ULL << 30))
        snprintf(out, size, "%.1f GiB (%.1f GB)", (double)bytes / (double)(1ULL << 30), (double)bytes / 1e9);
    else if (bytes >= (1ULL << 20))
        snprintf(out, size, "%.1f MiB", (double)bytes / (double)(1ULL << 20));
    else
        snprintf(out, size, "%llu KiB", bytes / 1024ULL);
}

/* --- 1. Processor collector --- */

static int hw_flag_has(const char *flags, const char *want)
{
    size_t wlen = strlen(want);
    const char *p = flags;
    while ((p = strstr(p, want)) != NULL) {
        int left_ok = (p == flags || p[-1] == ' ' || p[-1] == '\t');
        int right_ok = (p[wlen] == '\0' || p[wlen] == ' ' || p[wlen] == '\t');
        if (left_ok && right_ok) return 1;
        p += wlen;
    }
    return 0;
}

static const char *hw_x86_psabi_level(const char *flags)
{
    int v2 = hw_flag_has(flags, "cx16") && hw_flag_has(flags, "lahf_lm") &&
             hw_flag_has(flags, "popcnt") && hw_flag_has(flags, "sse4_1") &&
             hw_flag_has(flags, "sse4_2") && hw_flag_has(flags, "ssse3");
    int v3 = v2 && hw_flag_has(flags, "avx") && hw_flag_has(flags, "avx2") &&
             hw_flag_has(flags, "bmi1") && hw_flag_has(flags, "bmi2") &&
             hw_flag_has(flags, "f16c") && hw_flag_has(flags, "fma") &&
             hw_flag_has(flags, "abm") && hw_flag_has(flags, "movbe") &&
             hw_flag_has(flags, "xsave");
    int v4 = v3 && hw_flag_has(flags, "avx512f") && hw_flag_has(flags, "avx512bw") &&
             hw_flag_has(flags, "avx512cd") && hw_flag_has(flags, "avx512dq") &&
             hw_flag_has(flags, "avx512vl");
    if (v4) return "x86-64-v4";
    if (v3) return "x86-64-v3";
    if (v2) return "x86-64-v2";
    return "x86-64 (baseline)";
}

static void hw_collect_cpu(void)
{
    char path[256], line[4096];
    char vendor[64] = "", model[128] = "", fam[32] = "", mod_id[32] = "", step[32] = "";
    char ucode[32] = "", flags[4096] = "", bogomips[32] = "";
    int logical = 0, phys_cores = 0;

    snprintf(path, sizeof path, "%s/cpuinfo", HW_PROC_ROOT);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *c = strchr(line, ':');
        if (!c) continue;
        *c++ = '\0';
        while (*c == ' ' || *c == '\t') c++;
        c[strcspn(c, "\r\n")] = '\0';
        size_t kl = strlen(line);
        while (kl > 0 && (line[kl - 1] == ' ' || line[kl - 1] == '\t')) line[--kl] = '\0';

        if (strcmp(line, "processor") == 0) logical++;
        else if (!vendor[0] && strcmp(line, "vendor_id") == 0) hw_scpy(vendor, sizeof vendor, c);
        else if (!model[0] && strcmp(line, "model name") == 0) hw_scpy(model, sizeof model, c);
        else if (!fam[0] && strcmp(line, "cpu family") == 0) hw_scpy(fam, sizeof fam, c);
        else if (!mod_id[0] && strcmp(line, "model") == 0) hw_scpy(mod_id, sizeof mod_id, c);
        else if (!step[0] && strcmp(line, "stepping") == 0) hw_scpy(step, sizeof step, c);
        else if (!ucode[0] && strcmp(line, "microcode") == 0) hw_scpy(ucode, sizeof ucode, c);
        else if (!flags[0] && strcmp(line, "flags") == 0) hw_scpy(flags, sizeof flags, c);
        else if (!bogomips[0] && strcmp(line, "bogomips") == 0) hw_scpy(bogomips, sizeof bogomips, c);
        else if (phys_cores == 0 && strcmp(line, "cpu cores") == 0) phys_cores = atoi(c);
    }
    if (f) fclose(f);

    hw_add_section(_("Processor Identification"));
    if (model[0]) hw_clean_cpu_name(model);
    hw_add_kv(_("Model"), model[0] ? model : _("Unknown"));
    hw_add_kv(_("Vendor"), vendor);
    if (fam[0] || mod_id[0]) {
        char fms[96];
        snprintf(fms, sizeof fms, _("Family %s, Model %s, Stepping %s"),
                 fam[0] ? fam : "?", mod_id[0] ? mod_id : "?", step[0] ? step : "?");
        hw_add_kv(_("Signature"), fms);
    }
    hw_add_kv(_("Microcode"), ucode);

    char topo[96];
    if (phys_cores > 0)
        snprintf(topo, sizeof topo, _("%d physical cores, %d hardware threads"), phys_cores, logical);
    else if (logical > 0)
        snprintf(topo, sizeof topo, _("%d hardware threads"), logical);
    else
        topo[0] = '\0';
    hw_add_kv(_("Topology"), topo);

    if (flags[0]) {
        hw_add_kv_col(_("x86-64 ISA level"), hw_x86_psabi_level(flags), POWER_GOOD_COL);
        static const char *const notable[] = {
            "aes", "vaes", "pclmulqdq", "sha_ni", "avx", "avx2", "fma",
            "avx512f", "avx_vnni", "bmi1", "bmi2", "popcnt", "rdrand", "rdseed",
            "vmx", "svm", "smap", "smep", "ibt", "shstk"
        };
        char feat[160] = "";
        size_t fp = 0;
        for (size_t i = 0; i < sizeof notable / sizeof notable[0]; i++) {
            if (hw_flag_has(flags, notable[i]) && fp + strlen(notable[i]) + 3 < sizeof feat) {
                fp += (size_t)snprintf(feat + fp, sizeof feat - fp, "%s%s", fp ? " " : "", notable[i]);
            }
        }
        hw_add_kv(_("Key extensions"), feat);
    }

    /* Cache hierarchy from cpu0 */
    int any_cache = 0;
    for (int idx = 0; idx < 6; idx++) {
        char cdir[300], lvl[16] = "", ctype[32] = "", csize[32] = "", ways[16] = "", shared[64] = "";
        snprintf(cdir, sizeof cdir, "%s/devices/system/cpu/cpu0/cache/index%d/level", HW_SYS_ROOT, idx);
        if (hw_read_line(cdir, lvl, sizeof lvl) != 0) break;
        snprintf(cdir, sizeof cdir, "%s/devices/system/cpu/cpu0/cache/index%d/type", HW_SYS_ROOT, idx);
        hw_read_line(cdir, ctype, sizeof ctype);
        snprintf(cdir, sizeof cdir, "%s/devices/system/cpu/cpu0/cache/index%d/size", HW_SYS_ROOT, idx);
        hw_read_line(cdir, csize, sizeof csize);
        snprintf(cdir, sizeof cdir, "%s/devices/system/cpu/cpu0/cache/index%d/ways_of_associativity", HW_SYS_ROOT, idx);
        hw_read_line(cdir, ways, sizeof ways);
        snprintf(cdir, sizeof cdir, "%s/devices/system/cpu/cpu0/cache/index%d/shared_cpu_list", HW_SYS_ROOT, idx);
        hw_read_line(cdir, shared, sizeof shared);
        if (!any_cache) { hw_add_section(_("Cache Hierarchy")); any_cache = 1; }
        char k[64], v[128];
        snprintf(k, sizeof k, "L%s %s", lvl, ctype);
        if (ways[0] && shared[0])
            snprintf(v, sizeof v, _("%s (%s-way, CPUs %s)"), csize, ways, shared);
        else
            hw_scpy(v, sizeof v, csize);
        hw_add_kv(k, v);
    }

    /* Frequency scaling */
    char drv[64] = "", gov[64] = "", epp[64] = "";
    snprintf(path, sizeof path, "%s/devices/system/cpu/cpu0/cpufreq/scaling_driver", HW_SYS_ROOT);
    hw_read_line(path, drv, sizeof drv);
    snprintf(path, sizeof path, "%s/devices/system/cpu/cpu0/cpufreq/scaling_governor", HW_SYS_ROOT);
    hw_read_line(path, gov, sizeof gov);
    snprintf(path, sizeof path, "%s/devices/system/cpu/cpu0/cpufreq/energy_performance_preference", HW_SYS_ROOT);
    hw_read_line(path, epp, sizeof epp);
    if (drv[0] || gov[0]) {
        hw_add_section(_("Frequency & Power Scaling"));
        if (drv[0] && gov[0]) {
            char s[140];
            if (epp[0]) snprintf(s, sizeof s, _("%s (governor: %s, EPP: %s)"), drv, gov, epp);
            else snprintf(s, sizeof s, _("%s (governor: %s)"), drv, gov);
            hw_add_kv(_("Scaling driver"), s);
        }
        int shown = 0;
        for (int c = 0; c < logical && c < 32 && shown < 16; c++) {
            int ok_cur = 0, ok_min = 0, ok_max = 0;
            snprintf(path, sizeof path, "%s/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", HW_SYS_ROOT, c);
            long long cur = hw_read_ll(path, &ok_cur);
            snprintf(path, sizeof path, "%s/devices/system/cpu/cpu%d/cpufreq/cpuinfo_min_freq", HW_SYS_ROOT, c);
            long long mn = hw_read_ll(path, &ok_min);
            snprintf(path, sizeof path, "%s/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", HW_SYS_ROOT, c);
            long long mx = hw_read_ll(path, &ok_max);
            if (!ok_cur) continue;
            char k[32], v[96];
            snprintf(k, sizeof k, "CPU %d", c);
            if (ok_min && ok_max && mx > 0)
                snprintf(v, sizeof v, _("%.0f MHz  (range %.0f – %.0f MHz)"), cur / 1000.0, mn / 1000.0, mx / 1000.0);
            else
                snprintf(v, sizeof v, "%.0f MHz", cur / 1000.0);
            hw_add_kv(k, v);
            shown++;
        }
    }

    /* Hardware vulnerabilities */
    snprintf(path, sizeof path, "%s/devices/system/cpu/vulnerabilities", HW_SYS_ROOT);
    DIR *vd = opendir(path);
    if (vd) {
        int sec_added = 0;
        struct dirent *e;
        while ((e = readdir(vd)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char vp[1024], status[160];
            snprintf(vp, sizeof vp, "%s/%s", path, e->d_name);
            if (hw_read_line(vp, status, sizeof status) != 0) continue;
            if (!sec_added) { hw_add_section(_("CPU Security Mitigations")); sec_added = 1; }
            uint32_t col = strncasecmp(status, "Vulnerable", 10) == 0 ? POWER_LOW_COL :
                           (strncasecmp(status, "Not affected", 12) == 0 ? POWER_GOOD_COL : FG);
            hw_add_kv_col(e->d_name, status, col);
        }
        closedir(vd);
    }
}

/* --- 2. Memory & SMBIOS Type 16/17 DIMM parser --- */

static const char *smbios_str(const uint8_t *s_start, size_t max_len, uint8_t idx, size_t *end_off)
{
    const uint8_t *p = s_start;
    const uint8_t *lim = s_start + max_len;
    uint8_t cur = 1;
    const char *found = "";
    if (p + 1 < lim && p[0] == 0 && p[1] == 0) {
        if (end_off) *end_off = 2;
        return "";
    }
    while (p < lim) {
        const char *str = (const char *)p;
        while (p < lim && *p != 0) p++;
        if (p >= lim) break;
        if (idx > 0 && cur == idx) found = str;
        p++; /* skip NUL */
        cur++;
        if (p < lim && *p == 0) {
            p++;
            break;
        }
    }
    if (end_off) *end_off = (size_t)(p - s_start);
    while (*found == ' ') found++;
    return found;
}

static const char *smbios_mem_type(uint8_t code)
{
    switch (code) {
    case 0x12: return "DDR";
    case 0x13: return "DDR2";
    case 0x18: return "DDR3";
    case 0x1A: return "DDR4";
    case 0x1B: return "LPDDR";
    case 0x1C: return "LPDDR2";
    case 0x1D: return "LPDDR3";
    case 0x1E: return "LPDDR4";
    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";
    default:   return "DRAM";
    }
}

static const char *smbios_form_factor(uint8_t code)
{
    switch (code) {
    case 0x09: return "DIMM";
    case 0x0B: return "Soldered";
    case 0x0D: return "SODIMM";
    default:   return "";
    }
}

static int hw_parse_smbios_memory(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    int slots_found = 0, populated = 0;
    while (off + 4 <= len) {
        uint8_t type = buf[off];
        uint8_t slen = buf[off + 1];
        if (slen < 4 || off + slen > len) break;
        size_t tail = 0;
        (void)smbios_str(buf + off + slen, len - (off + slen), 0, &tail);
        size_t next = off + slen + tail;
        if (type == 127) break;

        if (type == 16 && slen >= 0x0F) {
            uint32_t max_kb = (uint32_t)buf[off + 7] | ((uint32_t)buf[off + 8] << 8) |
                              ((uint32_t)buf[off + 9] << 16) | ((uint32_t)buf[off + 10] << 24);
            uint16_t ndev = (uint16_t)buf[off + 13] | ((uint16_t)buf[off + 14] << 8);
            uint8_t ecc = buf[off + 6];
            unsigned long long cap_b = (unsigned long long)max_kb * 1024ULL;
            if (max_kb == 0x80000000u && slen >= 0x17) {
                cap_b = 0;
                for (int b = 0; b < 8; b++) cap_b |= ((unsigned long long)buf[off + 15 + b]) << (b * 8);
            }
            hw_add_section(_("Physical Memory Array (SMBIOS)"));
            char v[128];
            if (cap_b > 0) {
                snprintf(v, sizeof v, _("%u slots, max supported %.0f GiB (%s)"),
                         ndev, (double)cap_b / (double)(1ULL << 30),
                         ecc == 5 || ecc == 6 ? _("ECC") : _("Non-ECC"));
            } else {
                snprintf(v, sizeof v, _("%u memory slots"), ndev);
            }
            hw_add_kv(_("Controller array"), v);
        } else if (type == 17 && slen >= 0x15) {
            slots_found++;
            const uint8_t *s_area = buf + off + slen;
            size_t s_max = len - (off + slen);
            const char *loc = smbios_str(s_area, s_max, buf[off + 0x10], NULL);
            const char *bank = smbios_str(s_area, s_max, buf[off + 0x11], NULL);
            uint16_t raw_sz = (uint16_t)buf[off + 0x0C] | ((uint16_t)buf[off + 0x0D] << 8);
            unsigned long long mb = 0;
            if (raw_sz == 0x7FFFu && slen >= 0x20) {
                uint32_t ext = (uint32_t)buf[off + 0x1C] | ((uint32_t)buf[off + 0x1D] << 8) |
                               ((uint32_t)buf[off + 0x1E] << 16) | ((uint32_t)buf[off + 0x1F] << 24);
                mb = ext & 0x7FFFFFFFu;
            } else if (raw_sz != 0 && raw_sz != 0xFFFFu) {
                mb = (raw_sz & 0x8000u) ? ((raw_sz & 0x7FFFu) / 1024ULL) : raw_sz;
            }

            char slot_name[72];
            if (loc[0] && bank[0] && hw_meaningful_dmi(bank))
                snprintf(slot_name, sizeof slot_name, "%.32s (%.28s)", loc, bank);
            else if (loc[0])
                snprintf(slot_name, sizeof slot_name, "%.64s", loc);
            else
                snprintf(slot_name, sizeof slot_name, _("Slot %d"), slots_found);

            if (mb == 0) {
                hw_add_kv_col(slot_name, _("Empty"), DIM);
            } else {
                populated++;
                const char *mtype = smbios_mem_type(buf[off + 0x12]);
                const char *ff = smbios_form_factor(buf[off + 0x0E]);
                uint16_t spd = (uint16_t)buf[off + 0x15] | ((uint16_t)buf[off + 0x16] << 8);
                uint16_t cfg_spd = (slen >= 0x22)
                    ? ((uint16_t)buf[off + 0x20] | ((uint16_t)buf[off + 0x21] << 8)) : 0;
                uint16_t use_spd = (cfg_spd > 0 && cfg_spd != 0xFFFFu) ? cfg_spd : spd;
                const char *mfg = slen > 0x17 ? smbios_str(s_area, s_max, buf[off + 0x17], NULL) : "";
                const char *part = slen > 0x1A ? smbios_str(s_area, s_max, buf[off + 0x1A], NULL) : "";
                uint8_t ranks = slen > 0x1B ? (buf[off + 0x1B] & 0x0F) : 0;
                uint16_t mv = slen >= 0x28 ? ((uint16_t)buf[off + 0x26] | ((uint16_t)buf[off + 0x27] << 8)) : 0;

                char desc[160];
                size_t dp = 0;
                if (mb >= 1024 && (mb % 1024 == 0))
                    dp += (size_t)snprintf(desc + dp, sizeof desc - dp, "%llu GiB %s", mb / 1024ULL, mtype);
                else
                    dp += (size_t)snprintf(desc + dp, sizeof desc - dp, "%llu MiB %s", mb, mtype);
                if (use_spd > 0 && use_spd != 0xFFFFu)
                    dp += (size_t)snprintf(desc + dp, sizeof desc - dp, "-%u", use_spd);
                if (hw_meaningful_dmi(mfg))
                    dp += (size_t)snprintf(desc + dp, sizeof desc - dp, " [%.20s%s%.24s]",
                                           mfg, hw_meaningful_dmi(part) ? " " : "",
                                           hw_meaningful_dmi(part) ? part : "");
                else if (hw_meaningful_dmi(part))
                    dp += (size_t)snprintf(desc + dp, sizeof desc - dp, " [%.28s]", part);
                if (ff[0])
                    dp += (size_t)snprintf(desc + dp, sizeof desc - dp, " %s", ff);
                if (ranks > 0)
                    dp += (size_t)snprintf(desc + dp, sizeof desc - dp, ", %uR", ranks);
                if (mv > 0 && mv < 5000)
                    dp += (size_t)snprintf(desc + dp, sizeof desc - dp, ", %.2fV", mv / 1000.0);
                hw_add_kv(slot_name, desc);
            }
        }
        off = next;
    }
    return populated > 0 || slots_found > 0;
}

static void hw_collect_memory(void)
{
    char path[256], line[256];
    unsigned long long mem_total = 0, mem_avail = 0, mem_free = 0, buffers = 0, cached = 0;
    unsigned long long sreclaim = 0, shmem = 0, swap_total = 0, swap_free = 0, zswap = 0, dirty = 0;

    snprintf(path, sizeof path, "%s/meminfo", HW_PROC_ROOT);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        unsigned long long v = 0;
        char k[64];
        if (sscanf(line, "%63[^:]: %llu", k, &v) != 2) continue;
        if (strcmp(k, "MemTotal") == 0) mem_total = v * 1024ULL;
        else if (strcmp(k, "MemAvailable") == 0) mem_avail = v * 1024ULL;
        else if (strcmp(k, "MemFree") == 0) mem_free = v * 1024ULL;
        else if (strcmp(k, "Buffers") == 0) buffers = v * 1024ULL;
        else if (strcmp(k, "Cached") == 0) cached = v * 1024ULL;
        else if (strcmp(k, "SReclaimable") == 0) sreclaim = v * 1024ULL;
        else if (strcmp(k, "Shmem") == 0) shmem = v * 1024ULL;
        else if (strcmp(k, "SwapTotal") == 0) swap_total = v * 1024ULL;
        else if (strcmp(k, "SwapFree") == 0) swap_free = v * 1024ULL;
        else if (strcmp(k, "Zswap") == 0) zswap = v * 1024ULL;
        else if (strcmp(k, "Dirty") == 0) dirty = v * 1024ULL;
    }
    if (f) fclose(f);

    hw_add_section(_("System Memory Usage"));
    if (mem_total > 0) {
        unsigned long long used = mem_total > mem_avail ? mem_total - mem_avail : 0;
        int pct = (int)((double)used * 100.0 / (double)mem_total + 0.5);
        char v[128];
        snprintf(v, sizeof v, _("%.2f / %.2f GiB (%d%%)"),
                 (double)used / (double)(1ULL << 30), (double)mem_total / (double)(1ULL << 30), pct);
        hw_add_bar(_("Physical RAM"), pct, v);

        snprintf(v, sizeof v, _("%.2f GiB available, %.2f GiB page cache, %.1f MiB shared"),
                 (double)mem_avail / (double)(1ULL << 30),
                 (double)(buffers + cached + sreclaim) / (double)(1ULL << 30),
                 (double)shmem / (double)(1ULL << 20));
        hw_add_kv(_("Breakdown"), v);
        (void)mem_free;
        if (dirty > (1ULL << 20)) {
            snprintf(v, sizeof v, "%.1f MiB", (double)dirty / (double)(1ULL << 20));
            hw_add_kv(_("Dirty pages"), v);
        }
    }
    if (swap_total > 0) {
        unsigned long long sused = swap_total > swap_free ? swap_total - swap_free : 0;
        int spct = (int)((double)sused * 100.0 / (double)swap_total + 0.5);
        char v[128];
        snprintf(v, sizeof v, _("%.2f / %.2f GiB (%d%%)"),
                 (double)sused / (double)(1ULL << 30), (double)swap_total / (double)(1ULL << 30), spct);
        hw_add_bar(_("Swap"), spct, v);
        if (zswap > 0) {
            snprintf(v, sizeof v, "%.1f MiB", (double)zswap / (double)(1ULL << 20));
            hw_add_kv(_("Zswap compressed"), v);
        }
    } else {
        hw_add_kv_col(_("Swap"), _("Not configured"), DIM);
    }

    uint8_t dmi[65536];
    snprintf(path, sizeof path, "%s/firmware/dmi/tables/DMI", HW_SYS_ROOT);
    size_t n = hw_read_bytes(path, dmi, sizeof dmi);
    if (n == 0 || !hw_parse_smbios_memory(dmi, n)) {
        hw_add_section(_("Physical Memory Modules"));
        hw_add_note(_("SMBIOS Type 17 table is not exposed on this machine (e.g. virtual machine)."));
    }
}

/* --- 3. Storage & NVMe / ATA SMART health --- */

struct hw_nvme_smart {
    uint8_t critical_warning;
    int temp_c;
    uint8_t avail_spare;
    uint8_t spare_thresh;
    uint8_t pct_used;
    unsigned long long units_read;
    unsigned long long units_written;
    unsigned long long power_cycles;
    unsigned long long power_on_hours;
    unsigned long long unsafe_shutdowns;
    unsigned long long media_errors;
};

static uint64_t hw_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (i * 8);
    return v;
}

static int hw_parse_nvme_smart_log(const uint8_t log[512], struct hw_nvme_smart *out)
{
    uint16_t kelvin = (uint16_t)log[1] | ((uint16_t)log[2] << 8);
    if (kelvin == 0 && log[3] == 0 && hw_le64(log + 112) == 0 && hw_le64(log + 128) == 0)
        return -1;
    out->critical_warning = log[0];
    out->temp_c = kelvin >= 200 && kelvin <= 450 ? (int)kelvin - 273 : -1000;
    out->avail_spare = log[3];
    out->spare_thresh = log[4];
    out->pct_used = log[5];
    out->units_read = hw_le64(log + 32);
    out->units_written = hw_le64(log + 48);
    out->power_cycles = hw_le64(log + 112);
    out->power_on_hours = hw_le64(log + 128);
    out->unsafe_shutdowns = hw_le64(log + 144);
    out->media_errors = hw_le64(log + 160);
    return 0;
}

struct hw_ata_smart {
    int temp_c;
    long long power_on_hours;
    long long power_cycles;
    long long realloc_sectors;
    long long pending_sectors;
    int life_pct; /* 0..100 when SSD wear attribute exists, else -1 */
    unsigned long long bytes_written;
};

static int hw_parse_ata_smart_sector(const uint8_t sec[512], struct hw_ata_smart *out)
{
    out->temp_c = -1000;
    out->power_on_hours = -1;
    out->power_cycles = -1;
    out->realloc_sectors = -1;
    out->pending_sectors = -1;
    out->life_pct = -1;
    out->bytes_written = 0;
    int any = 0;

    for (int i = 0; i < 30; i++) {
        const uint8_t *a = sec + 2 + i * 12;
        uint8_t id = a[0], cur = a[3];
        if (id == 0 || id == 0xFF) continue;
        uint64_t raw = 0;
        for (int b = 0; b < 6; b++) raw |= ((uint64_t)a[5 + b]) << (b * 8);
        any = 1;
        if (id == 5) out->realloc_sectors = (long long)raw;
        else if (id == 9) out->power_on_hours = (long long)(raw & 0xFFFFFFULL);
        else if (id == 12) out->power_cycles = (long long)raw;
        else if (id == 194 || (id == 190 && out->temp_c < -100)) {
            int tc = (int)(raw & 0xFF);
            if (tc > -40 && tc < 120) out->temp_c = tc;
        } else if (id == 197 || id == 198) {
            if (out->pending_sectors < 0) out->pending_sectors = 0;
            out->pending_sectors += (long long)raw;
        } else if ((id == 231 || id == 233 || id == 177 || id == 202) && cur > 0 && cur <= 100) {
            out->life_pct = (id == 202 && cur <= 100) ? (100 - (int)raw) : cur;
            if (out->life_pct < 0 || out->life_pct > 100) out->life_pct = cur;
        } else if (id == 241 && raw > 0) {
            out->bytes_written = raw * 512ULL;
        }
    }
    return any ? 0 : -1;
}

static int hw_read_nvme_smart(const char *disk, struct hw_nvme_smart *out)
{
    /* Allow deterministic binary fixture in unit tests */
    char fpath[1024];
    uint8_t log[512];
    snprintf(fpath, sizeof fpath, "%s/block/%.47s/smart_nvme.bin", HW_SYS_ROOT, disk);
    if (hw_read_bytes(fpath, log, sizeof log) == 512) {
        return hw_parse_nvme_smart_log(log, out);
    }
#if defined(__linux__) && !defined(HW_DISABLE_DEV_IOCTL)
    char ctrl[64];
    hw_scpy(ctrl, sizeof ctrl, disk);
    char *n = strstr(ctrl, "n");
    if (strncmp(ctrl, "nvme", 4) == 0 && n && n > ctrl + 4) *n = '\0';
    char devpath[96];
    snprintf(devpath, sizeof devpath, "/dev/%.47s", ctrl);
    int fd = open(devpath, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    memset(log, 0, sizeof log);
    struct nvme_admin_cmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.opcode = 0x02; /* Get Log Page */
    cmd.nsid = 0xFFFFFFFFu;
    cmd.addr = (uint64_t)(uintptr_t)log;
    cmd.data_len = sizeof log;
    cmd.cdw10 = 0x02u | (127u << 16); /* LID 0x02 SMART/Health, 128 dwords - 1 */
    int rc = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
    close(fd);
    if (rc != 0) return -1;
    return hw_parse_nvme_smart_log(log, out);
#else
    return -1;
#endif
}

static int hw_read_ata_smart(const char *disk, struct hw_ata_smart *out)
{
    char fpath[1024];
    uint8_t sec[512];
    snprintf(fpath, sizeof fpath, "%s/block/%.47s/smart_ata.bin", HW_SYS_ROOT, disk);
    if (hw_read_bytes(fpath, sec, sizeof sec) == 512) {
        return hw_parse_ata_smart_sector(sec, out);
    }
#if defined(__linux__) && !defined(HW_DISABLE_DEV_IOCTL)
    char devpath[96];
    snprintf(devpath, sizeof devpath, "/dev/%.47s", disk);
    int fd = open(devpath, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    memset(sec, 0, sizeof sec);
    uint8_t cdb[12] = {
        0xA1, (4 << 1), 0x0E, 0xD0, 1, 0, 0x4F, 0xC2, 0, 0xB0, 0, 0
    };
    uint8_t sense[32] = { 0 };
    sg_io_hdr_t io;
    memset(&io, 0, sizeof io);
    io.interface_id = 'S';
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.cmd_len = sizeof cdb;
    io.mx_sb_len = sizeof sense;
    io.dxfer_len = sizeof sec;
    io.dxferp = sec;
    io.cmdp = cdb;
    io.sbp = sense;
    io.timeout = 1500;
    int rc = ioctl(fd, SG_IO, &io);
    close(fd);
    if (rc != 0 || (io.info & SG_INFO_OK_MASK) != SG_INFO_OK) return -1;
    return hw_parse_ata_smart_sector(sec, out);
#else
    return -1;
#endif
}

static void hw_collect_storage(void)
{
    char broot[128];
    snprintf(broot, sizeof broot, "%s/block", HW_SYS_ROOT);
    DIR *d = opendir(broot);
    if (!d) {
        hw_add_section(_("Block Devices"));
        hw_add_note(_("No block devices found in /sys/block."));
        return;
    }

    char names[32][48];
    int ndisks = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && ndisks < 32) {
        const char *nm = e->d_name;
        if (nm[0] == '.' || strncmp(nm, "loop", 4) == 0 || strncmp(nm, "ram", 3) == 0 ||
            strncmp(nm, "zram", 4) == 0 || strncmp(nm, "sr", 2) == 0) {
            continue;
        }
        hw_scpy(names[ndisks++], sizeof names[0], nm);
    }
    closedir(d);

    for (int i = 0; i < ndisks - 1; i++) {
        for (int j = i + 1; j < ndisks; j++) {
            if (strcmp(names[i], names[j]) > 0) {
                char tmp[48];
                memcpy(tmp, names[i], sizeof tmp);
                memcpy(names[i], names[j], sizeof tmp);
                memcpy(names[j], tmp, sizeof tmp);
            }
        }
    }

    int shown = 0;
    for (int i = 0; i < ndisks; i++) {
        const char *nm = names[i];
        char p[1024], model[96] = "", vendor[64] = "", rev[48] = "", dm_name[64] = "";
        int ok = 0;
        snprintf(p, sizeof p, "%s/%.47s/size", broot, nm);
        long long sectors = hw_read_ll(p, &ok);
        if (!ok || sectors <= 0) continue;

        /* Skip raw dm-* unless we label its LUKS/mapper name */
        snprintf(p, sizeof p, "%s/%.47s/dm/name", broot, nm);
        if (strncmp(nm, "dm-", 3) == 0) {
            hw_read_line(p, dm_name, sizeof dm_name);
            char title[120], sz[64];
            hw_format_bytes((unsigned long long)sectors * 512ULL, sz, sizeof sz);
            snprintf(title, sizeof title, "/dev/mapper/%.48s (%.32s)", dm_name[0] ? dm_name : nm, nm);
            hw_add_kv(title, sz);
            continue;
        }

        shown++;
        snprintf(p, sizeof p, "%s/%.47s/device/model", broot, nm);
        hw_read_line(p, model, sizeof model);
        snprintf(p, sizeof p, "%s/%.47s/device/vendor", broot, nm);
        hw_read_line(p, vendor, sizeof vendor);
        snprintf(p, sizeof p, "%s/%.47s/device/firmware_rev", broot, nm);
        if (hw_read_line(p, rev, sizeof rev) != 0) {
            snprintf(p, sizeof p, "%s/%.47s/device/rev", broot, nm);
            hw_read_line(p, rev, sizeof rev);
        }
        snprintf(p, sizeof p, "%s/%.47s/queue/rotational", broot, nm);
        int rot_ok = 0;
        long long rot = hw_read_ll(p, &rot_ok);

        char sec_title[140];
        if (vendor[0] && strcasecmp(vendor, "ATA") != 0 && model[0])
            snprintf(sec_title, sizeof sec_title, "/dev/%.32s — %.32s %.56s", nm, vendor, model);
        else if (model[0])
            snprintf(sec_title, sizeof sec_title, "/dev/%.32s — %.80s", nm, model);
        else
            snprintf(sec_title, sizeof sec_title, "/dev/%.47s", nm);
        hw_add_section(sec_title);

        char sz[64];
        hw_format_bytes((unsigned long long)sectors * 512ULL, sz, sizeof sz);
        hw_add_kv(_("Capacity"), sz);
        const char *media = strncmp(nm, "nvme", 4) == 0 ? "NVMe SSD" :
                            strncmp(nm, "mmcblk", 6) == 0 ? "MMC / SD" :
                            (rot_ok && rot == 0 ? "Solid State Drive (SSD)" :
                             rot_ok && rot == 1 ? "Rotational HDD" : "Block device");
        hw_add_kv(_("Type"), media);
        hw_add_kv(_("Firmware"), rev);

        if (strncmp(nm, "nvme", 4) == 0) {
            struct hw_nvme_smart sm;
            if (hw_read_nvme_smart(nm, &sm) == 0) {
                int health = 100 - (int)sm.pct_used;
                if (health < 0) health = 0;
                char hv[128];
                snprintf(hv, sizeof hv, _("%d%% remaining (%u%% endurance used, spare %u%%)"),
                         health, sm.pct_used, sm.avail_spare);
                uint32_t hcol = (sm.critical_warning || health < 20) ? POWER_LOW_COL :
                                (health < 60 ? POWER_WARN_COL : POWER_GOOD_COL);
                hw_add_kv_col(_("SMART health"), hv, hcol);
                if (sm.temp_c > -100) {
                    snprintf(hv, sizeof hv, "%d °C", sm.temp_c);
                    hw_add_kv(_("Temperature"), hv);
                }
                char wr[64], rd[64];
                hw_format_bytes(sm.units_written * 512000ULL, wr, sizeof wr);
                hw_format_bytes(sm.units_read * 512000ULL, rd, sizeof rd);
                hw_add_kv(_("Total data written"), wr);
                hw_add_kv(_("Total data read"), rd);
                snprintf(hv, sizeof hv, _("%llu hours, %llu power cycles, %llu unsafe shutdowns"),
                         sm.power_on_hours, sm.power_cycles, sm.unsafe_shutdowns);
                hw_add_kv(_("Lifetime counters"), hv);
                snprintf(hv, sizeof hv, "%llu", sm.media_errors);
                hw_add_kv_col(_("Media / integrity errors"), hv, sm.media_errors ? POWER_LOW_COL : POWER_GOOD_COL);
            }
        } else if (strncmp(nm, "sd", 2) == 0) {
            struct hw_ata_smart sm;
            if (hw_read_ata_smart(nm, &sm) == 0) {
                char hv[128];
                if (sm.life_pct >= 0) {
                    snprintf(hv, sizeof hv, _("%d%% life remaining"), sm.life_pct);
                    hw_add_kv_col(_("SMART SSD life"), hv,
                                  sm.life_pct >= 50 ? POWER_GOOD_COL : POWER_WARN_COL);
                }
                if (sm.temp_c > -100) {
                    snprintf(hv, sizeof hv, "%d °C", sm.temp_c);
                    hw_add_kv(_("Temperature"), hv);
                }
                if (sm.bytes_written > 0) {
                    hw_format_bytes(sm.bytes_written, hv, sizeof hv);
                    hw_add_kv(_("Total data written"), hv);
                }
                if (sm.power_on_hours >= 0) {
                    snprintf(hv, sizeof hv, _("%lld hours, %lld power cycles"),
                             sm.power_on_hours, sm.power_cycles >= 0 ? sm.power_cycles : 0LL);
                    hw_add_kv(_("Lifetime counters"), hv);
                }
                if (sm.realloc_sectors >= 0 || sm.pending_sectors >= 0) {
                    long long bad = (sm.realloc_sectors > 0 ? sm.realloc_sectors : 0) +
                                    (sm.pending_sectors > 0 ? sm.pending_sectors : 0);
                    snprintf(hv, sizeof hv, _("reallocated %lld, pending %lld"),
                             sm.realloc_sectors >= 0 ? sm.realloc_sectors : 0LL,
                             sm.pending_sectors >= 0 ? sm.pending_sectors : 0LL);
                    hw_add_kv_col(_("Sector health"), hv, bad > 0 ? POWER_LOW_COL : POWER_GOOD_COL);
                }
            }
        }
    }
    if (!shown) {
        hw_add_section(_("Block Devices"));
        hw_add_note(_("No physical disks detected."));
    }
}

/* --- 4. hwdata ID resolvers (pci.ids, usb.ids, pnp.ids) --- */

struct hw_pci_entry {
    char bdf[24];
    unsigned int vendor, device, subven, subdev, class_code;
    char driver[48];
    char cur_speed[32], cur_width[16], max_speed[32], max_width[16];
    char vendor_name[72], device_name[96], class_name[64];
};

static void hw_trim_vendor(char *s)
{
    static const char *const tails[] = {
        " Semiconductor Co., Ltd.", " Electronics Co., Ltd.", " Technology Co., Ltd.",
        " Co., Ltd.", " Co., Ltd", ", Inc.", " Inc.", " Corporation", " Corp.", " Ltd."
    };
    for (size_t i = 0; i < sizeof tails / sizeof tails[0]; i++) {
        char *p = strstr(s, tails[i]);
        if (p) *p = '\0';
    }
}

static void hw_resolve_pci_ids(struct hw_pci_entry *list, int n)
{
    if (n <= 0) return;
    char path[256], line[320];
    snprintf(path, sizeof path, "%s/pci.ids", HW_HWDATA_DIR);
    FILE *f = fopen(path, "r");
    if (!f) return;

    int in_classes = 0;
    unsigned int cur_v = 0xFFFFFFFFu, cur_c = 0xFFFFFFFFu;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == 'C' && line[1] == ' ') {
            in_classes = 1;
            unsigned int cc = 0;
            int off = 0;
            if (sscanf(line + 2, "%2x %n", &cc, &off) >= 1 && off > 0) {
                cur_c = cc;
                const char *nm = line + 2 + off;
                while (*nm == ' ') nm++;
                for (int i = 0; i < n; i++) {
                    if (((list[i].class_code >> 16) & 0xFFu) == cur_c && !list[i].class_name[0])
                        hw_scpy(list[i].class_name, sizeof list[0].class_name, nm);
                }
            }
            continue;
        }
        if (in_classes) {
            if (line[0] == '\t' && line[1] != '\t' && cur_c != 0xFFFFFFFFu) {
                unsigned int sc = 0;
                int off = 0;
                if (sscanf(line + 1, "%2x %n", &sc, &off) >= 1 && off > 0) {
                    const char *nm = line + 1 + off;
                    while (*nm == ' ') nm++;
                    for (int i = 0; i < n; i++) {
                        if (((list[i].class_code >> 8) & 0xFFFFu) == ((cur_c << 8) | sc))
                            hw_scpy(list[i].class_name, sizeof list[0].class_name, nm);
                    }
                }
            }
            continue;
        }
        if (isxdigit((unsigned char)line[0])) {
            unsigned int v = 0;
            int off = 0;
            if (sscanf(line, "%4x %n", &v, &off) >= 1 && off > 0) {
                cur_v = v;
                const char *nm = line + off;
                while (*nm == ' ') nm++;
                for (int i = 0; i < n; i++) {
                    if (list[i].vendor == cur_v) {
                        hw_scpy(list[i].vendor_name, sizeof list[0].vendor_name, nm);
                        hw_trim_vendor(list[i].vendor_name);
                    }
                }
            }
        } else if (line[0] == '\t' && line[1] != '\t' && cur_v != 0xFFFFFFFFu) {
            unsigned int dv = 0;
            int off = 0;
            if (sscanf(line + 1, "%4x %n", &dv, &off) >= 1 && off > 0) {
                const char *nm = line + 1 + off;
                while (*nm == ' ') nm++;
                for (int i = 0; i < n; i++) {
                    if (list[i].vendor == cur_v && list[i].device == dv)
                        hw_scpy(list[i].device_name, sizeof list[0].device_name, nm);
                }
            }
        }
    }
    fclose(f);
}

static void hw_resolve_pnp_id(const char code[4], char *out, size_t size)
{
    out[0] = '\0';
    char path[256], line[256];
    snprintf(path, sizeof path, "%s/pnp.ids", HW_HWDATA_DIR);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, code, 3) == 0 && (line[3] == '\t' || line[3] == ' ')) {
            char *nm = line + 4;
            while (*nm == ' ' || *nm == '\t') nm++;
            nm[strcspn(nm, "\r\n")] = '\0';
            hw_scpy(out, size, nm);
            break;
        }
    }
    fclose(f);
}

/* --- 5. Graphics & EDID binary parser --- */

struct hw_edid_info {
    char mfg_code[4];
    char mfg_name[64];
    uint16_t prod_code;
    int week, year;
    int width_cm, height_cm;
    double diag_inch;
    int bpc; /* bits per primary color, 0 if unknown */
    char name[48];
    char panel_str[48];
    char serial[48];
    int pref_w, pref_h;
    double pref_hz;
};

static int hw_parse_edid(const uint8_t *b, size_t len, struct hw_edid_info *out)
{
    static const uint8_t magic[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    if (len < 128 || memcmp(b, magic, 8) != 0) return -1;
    memset(out, 0, sizeof *out);

    uint16_t mfg = ((uint16_t)b[8] << 8) | b[9];
    out->mfg_code[0] = (char)('@' + ((mfg >> 10) & 0x1F));
    out->mfg_code[1] = (char)('@' + ((mfg >> 5) & 0x1F));
    out->mfg_code[2] = (char)('@' + (mfg & 0x1F));
    out->mfg_code[3] = '\0';
    hw_resolve_pnp_id(out->mfg_code, out->mfg_name, sizeof out->mfg_name);

    out->prod_code = (uint16_t)b[10] | ((uint16_t)b[11] << 8);
    out->week = (b[16] >= 1 && b[16] <= 54) ? b[16] : 0;
    out->year = 1990 + b[17];

    if ((b[20] & 0x80) && b[18] == 1 && b[19] >= 4) {
        static const int depths[8] = { 0, 6, 8, 10, 12, 14, 16, 0 };
        out->bpc = depths[(b[20] >> 4) & 7];
    }
    out->width_cm = b[21];
    out->height_cm = b[22];
    if (out->width_cm > 0 && out->height_cm > 0) {
        out->diag_inch = hw_sqrt((double)out->width_cm * out->width_cm +
                                 (double)out->height_cm * out->height_cm) / 2.54;
    }

    for (int i = 0; i < 4; i++) {
        const uint8_t *d = b + 54 + i * 18;
        uint16_t pclk10k = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
        if (pclk10k != 0) {
            if (out->pref_w == 0) {
                int ha = d[2] | ((d[4] & 0xF0) << 4);
                int hb = d[3] | ((d[4] & 0x0F) << 8);
                int va = d[5] | ((d[7] & 0xF0) << 4);
                int vb = d[6] | ((d[7] & 0x0F) << 8);
                if (ha > 0 && va > 0 && (ha + hb) > 0 && (va + vb) > 0) {
                    out->pref_w = ha;
                    out->pref_h = va;
                    out->pref_hz = ((double)pclk10k * 10000.0) / ((double)(ha + hb) * (va + vb));
                }
            }
        } else if (d[2] == 0 && (d[3] == 0xFC || d[3] == 0xFE || d[3] == 0xFF)) {
            char txt[16];
            int k = 0;
            for (int j = 5; j < 18 && k < 14; j++) {
                if (d[j] == '\n' || d[j] == '\r' || d[j] == 0) break;
                if (d[j] >= 32 && d[j] < 127) txt[k++] = (char)d[j];
            }
            while (k > 0 && txt[k - 1] == ' ') k--;
            txt[k] = '\0';
            if (!txt[0]) continue;
            if (d[3] == 0xFC && !out->name[0]) hw_scpy(out->name, sizeof out->name, txt);
            else if (d[3] == 0xFE && !out->panel_str[0]) hw_scpy(out->panel_str, sizeof out->panel_str, txt);
            else if (d[3] == 0xFF && !out->serial[0]) hw_scpy(out->serial, sizeof out->serial, txt);
        }
    }
    return 0;
}

static void hw_read_link_basename(const char *path, char *out, size_t size)
{
    out[0] = '\0';
    char buf[512];
    ssize_t n = readlink(path, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    const char *base = strrchr(buf, '/');
    hw_scpy(out, size, base ? base + 1 : buf);
}

static void hw_collect_graphics(void)
{
    /* Collect VGA/3D/Display PCI controllers via class 0x03xxxx */
    char pdir[128];
    snprintf(pdir, sizeof pdir, "%s/bus/pci/devices", HW_SYS_ROOT);
    DIR *pd = opendir(pdir);
    struct hw_pci_entry gpus[8];
    int ngpus = 0;
    struct dirent *e;
    while (pd && (e = readdir(pd)) != NULL && ngpus < 8) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        int ok = 0;
        snprintf(p, sizeof p, "%s/%s/class", pdir, e->d_name);
        long long cc = hw_read_ll(p, &ok);
        if (!ok || ((cc >> 16) & 0xFF) != 0x03) continue;
        struct hw_pci_entry *g = &gpus[ngpus++];
        memset(g, 0, sizeof *g);
        snprintf(g->bdf, sizeof g->bdf, "%.23s", e->d_name);
        g->class_code = (unsigned int)cc;
        snprintf(p, sizeof p, "%s/%s/vendor", pdir, e->d_name);
        g->vendor = (unsigned int)hw_read_ll(p, &ok);
        snprintf(p, sizeof p, "%s/%s/device", pdir, e->d_name);
        g->device = (unsigned int)hw_read_ll(p, &ok);
        snprintf(p, sizeof p, "%s/%s/driver", pdir, e->d_name);
        hw_read_link_basename(p, g->driver, sizeof g->driver);
    }
    if (pd) closedir(pd);
    hw_resolve_pci_ids(gpus, ngpus);

    hw_add_section(_("Graphics Adapters"));
    if (ngpus == 0) {
        hw_add_note(_("No PCI display controller listed."));
    } else {
        for (int i = 0; i < ngpus; i++) {
            char k[72], v[160];
            snprintf(k, sizeof k, "GPU %d (%.23s)", i, gpus[i].bdf);
            snprintf(v, sizeof v, "%.48s %.64s%s%.24s%s",
                     gpus[i].vendor_name[0] ? gpus[i].vendor_name : "",
                     gpus[i].device_name[0] ? gpus[i].device_name : _("Display controller"),
                     gpus[i].driver[0] ? "  [driver: " : "",
                     gpus[i].driver,
                     gpus[i].driver[0] ? "]" : "");
            hw_add_kv(k, v);
        }
    }

    /* DRM connectors & EDID monitors */
    snprintf(pdir, sizeof pdir, "%s/class/drm", HW_SYS_ROOT);
    DIR *dd = opendir(pdir);
    int mon_shown = 0;
    while (dd && (e = readdir(dd)) != NULL) {
        if (strncmp(e->d_name, "card", 4) != 0 || strchr(e->d_name, '-') == NULL) continue;
        char st_path[1024], status[32] = "";
        snprintf(st_path, sizeof st_path, "%s/%s/status", pdir, e->d_name);
        if (hw_read_line(st_path, status, sizeof status) != 0 || strcmp(status, "connected") != 0)
            continue;

        mon_shown++;
        char sec[120];
        snprintf(sec, sizeof sec, _("Display — %.64s"), e->d_name);
        hw_add_section(sec);

        char mpath[1024], first_mode[64] = "";
        snprintf(mpath, sizeof mpath, "%s/%s/modes", pdir, e->d_name);
        hw_read_line(mpath, first_mode, sizeof first_mode);

        uint8_t edid[512];
        snprintf(mpath, sizeof mpath, "%s/%s/edid", pdir, e->d_name);
        size_t elen = hw_read_bytes(mpath, edid, sizeof edid);
        struct hw_edid_info ei;
        if (elen >= 128 && hw_parse_edid(edid, elen, &ei) == 0) {
            char v[160];
            snprintf(v, sizeof v, "%.48s (%.3s, 0x%04X)%s%.36s%s%.36s",
                     ei.mfg_name[0] ? ei.mfg_name : ei.mfg_code, ei.mfg_code, ei.prod_code,
                     ei.name[0] ? " — " : "", ei.name,
                     (!ei.name[0] && ei.panel_str[0]) ? " — " : "",
                     (!ei.name[0] && ei.panel_str[0]) ? ei.panel_str : "");
            hw_add_kv(_("Panel / Monitor"), v);
            if (ei.pref_w > 0 && ei.pref_h > 0) {
                snprintf(v, sizeof v, "%dx%d @ %.2f Hz", ei.pref_w, ei.pref_h, ei.pref_hz);
                hw_add_kv(_("Native timing"), v);
            } else if (first_mode[0]) {
                hw_add_kv(_("Preferred mode"), first_mode);
            }
            if (ei.width_cm > 0 && ei.height_cm > 0) {
                snprintf(v, sizeof v, _("%.1f\" diagonal (%d × %d cm)"),
                         ei.diag_inch, ei.width_cm, ei.height_cm);
                hw_add_kv(_("Physical size"), v);
            }
            if (ei.bpc > 0) {
                snprintf(v, sizeof v, _("%d-bit per channel"), ei.bpc);
                hw_add_kv(_("Color depth"), v);
            }
            if (ei.year >= 1995) {
                if (ei.week > 0) snprintf(v, sizeof v, _("Week %d, %d"), ei.week, ei.year);
                else snprintf(v, sizeof v, "%d", ei.year);
                hw_add_kv(_("Manufactured"), v);
            }
            hw_add_kv(_("Serial"), ei.serial);
        } else {
            hw_add_kv(_("Preferred mode"), first_mode[0] ? first_mode : _("connected"));
        }
    }
    if (dd) closedir(dd);
    if (!mon_shown) {
        hw_add_section(_("Connected Displays"));
        hw_add_note(_("No connected DRM output reported."));
    }
}

/* --- 6. Live Sensors (hwmon + thermal + RAPL package power) --- */

static void hw_collect_sensors(void)
{
    nhw_sensors = 0;

    /* 1. RAPL CPU Package Power delta */
    char rpath[512];
    int rok = 0;
    snprintf(rpath, sizeof rpath, "%s/class/powercap/intel-rapl:0/energy_uj", HW_SYS_ROOT);
    long long e_uj = hw_read_ll(rpath, &rok);
    if (!rok) {
        snprintf(rpath, sizeof rpath, "%s/class/powercap/intel-rapl/intel-rapl:0/energy_uj", HW_SYS_ROOT);
        e_uj = hw_read_ll(rpath, &rok);
    }
    if (rok && e_uj >= 0) {
        double now_t = hw_mono_sec();
        unsigned long long cur_uj = (unsigned long long)e_uj;
        if (hw_rapl_prev_time > 0 && now_t > hw_rapl_prev_time + 0.15 && cur_uj >= hw_rapl_prev_uj) {
            double w = (double)(cur_uj - hw_rapl_prev_uj) / 1e6 / (now_t - hw_rapl_prev_time);
            if (w >= 0.0 && w < 500.0) hw_rapl_watts = w;
        }
        hw_rapl_prev_uj = cur_uj;
        hw_rapl_prev_time = now_t;
        if (hw_rapl_watts >= 0.0 && nhw_sensors < HW_MAX_SENSORS) {
            struct hw_sensor *s = &hw_sensors[nhw_sensors++];
            memset(s, 0, sizeof *s);
            snprintf(s->chip, sizeof s->chip, "RAPL");
            hw_scpy(s->label, sizeof s->label, _("CPU Package Power"));
            s->kind = 'P';
            s->val = hw_rapl_watts;
        }
    }

    /* 2. /sys/class/hwmon/hwmon* */
    char hroot[128];
    snprintf(hroot, sizeof hroot, "%s/class/hwmon", HW_SYS_ROOT);
    DIR *hd = opendir(hroot);
    struct dirent *e;
    while (hd && (e = readdir(hd)) != NULL && nhw_sensors < HW_MAX_SENSORS) {
        if (e->d_name[0] == '.') continue;
        char chip[48] = "", p[600];
        snprintf(p, sizeof p, "%s/%s/name", hroot, e->d_name);
        if (hw_read_line(p, chip, sizeof chip) != 0)
            snprintf(chip, sizeof chip, "%.47s", e->d_name);

        /* Temperatures: temp1..temp16 */
        for (int k = 1; k <= 16 && nhw_sensors < HW_MAX_SENSORS; k++) {
            int ok = 0;
            snprintf(p, sizeof p, "%s/%s/temp%d_input", hroot, e->d_name, k);
            long long mc = hw_read_ll(p, &ok);
            if (!ok || mc <= -100000 || mc >= 200000) continue;
            struct hw_sensor *s = &hw_sensors[nhw_sensors++];
            memset(s, 0, sizeof *s);
            hw_scpy(s->chip, sizeof s->chip, chip);
            snprintf(p, sizeof p, "%s/%s/temp%d_label", hroot, e->d_name, k);
            if (hw_read_line(p, s->label, sizeof s->label) != 0 || !s->label[0])
                snprintf(s->label, sizeof s->label, "Temp %d", k);
            s->kind = 'T';
            s->val = (double)mc / 1000.0;
            snprintf(p, sizeof p, "%s/%s/temp%d_crit", hroot, e->d_name, k);
            long long cr = hw_read_ll(p, &ok);
            if (!ok || cr <= 0) {
                snprintf(p, sizeof p, "%s/%s/temp%d_max", hroot, e->d_name, k);
                cr = hw_read_ll(p, &ok);
            }
            if (ok && cr > 0 && cr < 200000) { s->has_crit = 1; s->crit_v = (double)cr / 1000.0; }
        }

        /* Fans: fan1..fan8 */
        for (int k = 1; k <= 8 && nhw_sensors < HW_MAX_SENSORS; k++) {
            int ok = 0;
            snprintf(p, sizeof p, "%s/%s/fan%d_input", hroot, e->d_name, k);
            long long rpm = hw_read_ll(p, &ok);
            if (!ok || rpm < 0 || rpm > 30000) continue;
            struct hw_sensor *s = &hw_sensors[nhw_sensors++];
            memset(s, 0, sizeof *s);
            hw_scpy(s->chip, sizeof s->chip, chip);
            snprintf(p, sizeof p, "%s/%s/fan%d_label", hroot, e->d_name, k);
            if (hw_read_line(p, s->label, sizeof s->label) != 0 || !s->label[0])
                snprintf(s->label, sizeof s->label, "Fan %d", k);
            s->kind = 'F';
            s->val = (double)rpm;
        }

        /* Power sensors: power1..power4 */
        for (int k = 1; k <= 4 && nhw_sensors < HW_MAX_SENSORS; k++) {
            int ok = 0;
            snprintf(p, sizeof p, "%s/%s/power%d_average", hroot, e->d_name, k);
            long long uw = hw_read_ll(p, &ok);
            if (!ok) {
                snprintf(p, sizeof p, "%s/%s/power%d_input", hroot, e->d_name, k);
                uw = hw_read_ll(p, &ok);
            }
            if (!ok || uw < 0) continue;
            struct hw_sensor *s = &hw_sensors[nhw_sensors++];
            memset(s, 0, sizeof *s);
            hw_scpy(s->chip, sizeof s->chip, chip);
            snprintf(p, sizeof p, "%s/%s/power%d_label", hroot, e->d_name, k);
            if (hw_read_line(p, s->label, sizeof s->label) != 0 || !s->label[0])
                snprintf(s->label, sizeof s->label, "Power %d", k);
            s->kind = 'P';
            s->val = (double)uw / 1e6;
        }

        /* Voltages: in0..in8 */
        for (int k = 0; k <= 8 && nhw_sensors < HW_MAX_SENSORS; k++) {
            int ok = 0;
            snprintf(p, sizeof p, "%s/%s/in%d_input", hroot, e->d_name, k);
            long long mv = hw_read_ll(p, &ok);
            if (!ok || mv <= 0 || mv > 30000) continue;
            struct hw_sensor *s = &hw_sensors[nhw_sensors++];
            memset(s, 0, sizeof *s);
            hw_scpy(s->chip, sizeof s->chip, chip);
            snprintf(p, sizeof p, "%s/%s/in%d_label", hroot, e->d_name, k);
            if (hw_read_line(p, s->label, sizeof s->label) != 0 || !s->label[0])
                snprintf(s->label, sizeof s->label, "in%d", k);
            s->kind = 'V';
            s->val = (double)mv / 1000.0;
        }
    }
    if (hd) closedir(hd);

    /* Fallback to /sys/class/thermal if no hwmon sensors were present */
    if (nhw_sensors == 0) {
        snprintf(hroot, sizeof hroot, "%s/class/thermal", HW_SYS_ROOT);
        DIR *td = opendir(hroot);
        while (td && (e = readdir(td)) != NULL && nhw_sensors < HW_MAX_SENSORS) {
            if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
            char p[1024], tztype[48] = "";
            int ok = 0;
            snprintf(p, sizeof p, "%s/%s/temp", hroot, e->d_name);
            long long mc = hw_read_ll(p, &ok);
            if (!ok) continue;
            snprintf(p, sizeof p, "%s/%s/type", hroot, e->d_name);
            hw_read_line(p, tztype, sizeof tztype);
            struct hw_sensor *s = &hw_sensors[nhw_sensors++];
            memset(s, 0, sizeof *s);
            snprintf(s->chip, sizeof s->chip, "thermal");
            snprintf(s->label, sizeof s->label, "%.63s", tztype[0] ? tztype : e->d_name);
            s->kind = 'T';
            s->val = (double)mc / 1000.0;
        }
        if (td) closedir(td);
    }

    if (nhw_sensors == 0) {
        hw_add_section(_("Hardware Sensors"));
        hw_add_note(_("No hwmon or thermal sensors reported on this system."));
        return;
    }

    char cur_chip[48] = "";
    for (int i = 0; i < nhw_sensors && nhw_rows < HW_MAX_ROWS; i++) {
        const struct hw_sensor *s = &hw_sensors[i];
        if (strcmp(cur_chip, s->chip) != 0) {
            hw_scpy(cur_chip, sizeof cur_chip, s->chip);
            hw_add_section(cur_chip);
        }
        struct hw_row *r = &hw_rows[nhw_rows++];
        memset(r, 0, sizeof *r);
        r->kind = HWROW_SENSOR;
        hw_scpy(r->key, sizeof r->key, s->label);
        if (s->kind == 'T') {
            if (s->has_crit)
                snprintf(r->val, sizeof r->val, _("%.1f °C   (crit %.0f °C)"), s->val, s->crit_v);
            else
                snprintf(r->val, sizeof r->val, "%.1f °C", s->val);
            r->val_col = (s->has_crit && s->val >= s->crit_v - 10.0) ? POWER_LOW_COL :
                         (s->val >= 80.0 ? POWER_WARN_COL : FG);
        } else if (s->kind == 'F') {
            snprintf(r->val, sizeof r->val, "%.0f RPM", s->val);
            r->val_col = FG;
        } else if (s->kind == 'P') {
            snprintf(r->val, sizeof r->val, "%.2f W", s->val);
            r->val_col = POWER_GOOD_COL;
        } else {
            snprintf(r->val, sizeof r->val, "%.3f V", s->val);
            r->val_col = FG;
        }
        r->sensor_idx = i;
    }
}

/* --- 7. PCI & PCIe Link Speeds --- */

static int hw_cmp_pci(const void *a, const void *b)
{
    const struct hw_pci_entry *x = a, *y = b;
    return strcmp(x->bdf, y->bdf);
}

static void hw_collect_pci(void)
{
    char pdir[128];
    snprintf(pdir, sizeof pdir, "%s/bus/pci/devices", HW_SYS_ROOT);
    DIR *d = opendir(pdir);
    struct hw_pci_entry list[96];
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && n < 96) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        int ok = 0;
        struct hw_pci_entry *it = &list[n++];
        memset(it, 0, sizeof *it);
        snprintf(it->bdf, sizeof it->bdf, "%.23s", e->d_name);
        snprintf(p, sizeof p, "%s/%s/vendor", pdir, e->d_name);
        it->vendor = (unsigned int)hw_read_ll(p, &ok);
        snprintf(p, sizeof p, "%s/%s/device", pdir, e->d_name);
        it->device = (unsigned int)hw_read_ll(p, &ok);
        snprintf(p, sizeof p, "%s/%s/class", pdir, e->d_name);
        it->class_code = (unsigned int)hw_read_ll(p, &ok);
        snprintf(p, sizeof p, "%s/%s/driver", pdir, e->d_name);
        hw_read_link_basename(p, it->driver, sizeof it->driver);
        snprintf(p, sizeof p, "%s/%s/current_link_speed", pdir, e->d_name);
        hw_read_line(p, it->cur_speed, sizeof it->cur_speed);
        snprintf(p, sizeof p, "%s/%s/current_link_width", pdir, e->d_name);
        hw_read_line(p, it->cur_width, sizeof it->cur_width);
        snprintf(p, sizeof p, "%s/%s/max_link_speed", pdir, e->d_name);
        hw_read_line(p, it->max_speed, sizeof it->max_speed);
        snprintf(p, sizeof p, "%s/%s/max_link_width", pdir, e->d_name);
        hw_read_line(p, it->max_width, sizeof it->max_width);
    }
    if (d) closedir(d);

    if (n == 0) {
        hw_add_section(_("PCI Devices"));
        hw_add_note(_("No PCI devices reported in /sys/bus/pci/devices."));
        return;
    }
    qsort(list, (size_t)n, sizeof list[0], hw_cmp_pci);
    hw_resolve_pci_ids(list, n);

    char sec[96];
    snprintf(sec, sizeof sec, _("PCI & PCIe Devices (%d total)"), n);
    hw_add_section(sec);

    for (int i = 0; i < n; i++) {
        const struct hw_pci_entry *it = &list[i];
        const char *short_bdf = strncmp(it->bdf, "0000:", 5) == 0 ? it->bdf + 5 : it->bdf;
        char k[72], v[160];
        snprintf(k, sizeof k, "%.23s  %.22s", short_bdf,
                 it->class_name[0] ? it->class_name : "PCI device");
        size_t vp = 0;
        if (it->vendor_name[0] || it->device_name[0]) {
            vp += (size_t)snprintf(v + vp, sizeof v - vp, "%.36s %.52s",
                                   it->vendor_name, it->device_name);
        } else {
            vp += (size_t)snprintf(v + vp, sizeof v - vp, "%04x:%04x", it->vendor, it->device);
        }
        if (it->driver[0] && vp + 24 < sizeof v)
            vp += (size_t)snprintf(v + vp, sizeof v - vp, " [%s]", it->driver);
        if (it->cur_speed[0] && strstr(it->cur_speed, "Unknown") == NULL &&
            it->cur_width[0] && atoi(it->cur_width) > 0 && vp + 24 < sizeof v) {
            char sp[32];
            hw_scpy(sp, sizeof sp, it->cur_speed);
            char *pcie = strstr(sp, " PCIe");
            if (pcie) *pcie = '\0';
            vp += (size_t)snprintf(v + vp, sizeof v - vp, " (%s x%.8s)", sp, it->cur_width);
        }
        hw_add_kv(k, v);
    }
}

/* --- 8. USB Devices --- */

struct hw_usb_entry {
    char port[32];
    unsigned int vid, pid;
    char mfg[64], prod[64], speed[24], power[24];
};

static void hw_resolve_usb_ids(struct hw_usb_entry *list, int n)
{
    if (n <= 0) return;
    char path[256], line[320];
    snprintf(path, sizeof path, "%s/usb.ids", HW_HWDATA_DIR);
    FILE *f = fopen(path, "r");
    if (!f) return;
    unsigned int cur_v = 0xFFFFFFFFu;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        line[strcspn(line, "\r\n")] = '\0';
        if (isxdigit((unsigned char)line[0])) {
            unsigned int v = 0;
            int off = 0;
            if (sscanf(line, "%4x %n", &v, &off) >= 1 && off > 0) {
                cur_v = v;
                const char *nm = line + off;
                while (*nm == ' ') nm++;
                for (int i = 0; i < n; i++) {
                    if (list[i].vid == cur_v && !list[i].mfg[0])
                        hw_scpy(list[i].mfg, sizeof list[0].mfg, nm);
                }
            } else {
                cur_v = 0xFFFFFFFFu;
            }
        } else if (line[0] == '\t' && line[1] != '\t' && cur_v != 0xFFFFFFFFu) {
            unsigned int p = 0;
            int off = 0;
            if (sscanf(line + 1, "%4x %n", &p, &off) >= 1 && off > 0) {
                const char *nm = line + 1 + off;
                while (*nm == ' ') nm++;
                for (int i = 0; i < n; i++) {
                    if (list[i].vid == cur_v && list[i].pid == p && !list[i].prod[0])
                        hw_scpy(list[i].prod, sizeof list[0].prod, nm);
                }
            }
        }
    }
    fclose(f);
}

static void hw_collect_usb(void)
{
    char udir[128];
    snprintf(udir, sizeof udir, "%s/bus/usb/devices", HW_SYS_ROOT);
    DIR *d = opendir(udir);
    struct hw_usb_entry list[64];
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && n < 64) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':') != NULL) continue;
        char p[1024], vs[16] = "", ps[16] = "";
        snprintf(p, sizeof p, "%s/%s/idVendor", udir, e->d_name);
        if (hw_read_line(p, vs, sizeof vs) != 0) continue;
        snprintf(p, sizeof p, "%s/%s/idProduct", udir, e->d_name);
        if (hw_read_line(p, ps, sizeof ps) != 0) continue;

        struct hw_usb_entry *u = &list[n++];
        memset(u, 0, sizeof *u);
        snprintf(u->port, sizeof u->port, "%.31s", e->d_name);
        u->vid = (unsigned int)strtoul(vs, NULL, 16);
        u->pid = (unsigned int)strtoul(ps, NULL, 16);
        snprintf(p, sizeof p, "%s/%s/manufacturer", udir, e->d_name);
        hw_read_line(p, u->mfg, sizeof u->mfg);
        snprintf(p, sizeof p, "%s/%s/product", udir, e->d_name);
        hw_read_line(p, u->prod, sizeof u->prod);
        snprintf(p, sizeof p, "%s/%s/speed", udir, e->d_name);
        hw_read_line(p, u->speed, sizeof u->speed);
        snprintf(p, sizeof p, "%s/%s/bMaxPower", udir, e->d_name);
        hw_read_line(p, u->power, sizeof u->power);
    }
    if (d) closedir(d);

    hw_resolve_usb_ids(list, n);
    char sec[96];
    snprintf(sec, sizeof sec, _("USB Bus & Devices (%d total)"), n);
    hw_add_section(sec);
    if (n == 0) {
        hw_add_note(_("No USB devices reported in /sys/bus/usb/devices."));
        return;
    }
    for (int i = 0; i < n; i++) {
        const struct hw_usb_entry *u = &list[i];
        char k[72], v[160];
        snprintf(k, sizeof k, "%-10.31s %04x:%04x", u->port, u->vid, u->pid);
        snprintf(v, sizeof v, "%.44s%s%.56s  (%.16s Mbps%s%.16s)",
                 u->mfg, (u->mfg[0] && u->prod[0]) ? " " : "", u->prod,
                 u->speed[0] ? u->speed : "?",
                 u->power[0] ? ", " : "", u->power);
        hw_add_kv(k, v);
    }
}

/* --- 9. Network Adapters (both locachy-net and host netns) --- */

static void hw_scan_net_sysfs(const char *sys_net_dir, const char *ns_tag)
{
    DIR *d = opendir(sys_net_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *ifn = e->d_name;
        if (ifn[0] == '.' || strcmp(ifn, "lo") == 0) continue;
        char p[1024], state[32] = "", mac[48] = "", spd[32] = "", drv[48] = "";
        int ok = 0;
        snprintf(p, sizeof p, "%s/%s/operstate", sys_net_dir, ifn);
        hw_read_line(p, state, sizeof state);
        snprintf(p, sizeof p, "%s/%s/address", sys_net_dir, ifn);
        hw_read_line(p, mac, sizeof mac);
        snprintf(p, sizeof p, "%s/%s/speed", sys_net_dir, ifn);
        long long mbps = hw_read_ll(p, &ok);
        if (ok && mbps > 0) snprintf(spd, sizeof spd, "%lld Mbps", mbps);
        snprintf(p, sizeof p, "%s/%s/device/driver", sys_net_dir, ifn);
        hw_read_link_basename(p, drv, sizeof drv);
        snprintf(p, sizeof p, "%s/%s/statistics/rx_bytes", sys_net_dir, ifn);
        long long rx = hw_read_ll(p, &ok);
        snprintf(p, sizeof p, "%s/%s/statistics/tx_bytes", sys_net_dir, ifn);
        long long tx = hw_read_ll(p, &ok);

        char title[96];
        snprintf(title, sizeof title, "%.48s (%.24s)", ifn, ns_tag);
        hw_add_section(title);
        char st_line[128];
        snprintf(st_line, sizeof st_line, "%.24s%s%.24s%s%.32s%s",
                 state[0] ? state : "unknown",
                 spd[0] ? ", " : "", spd,
                 drv[0] ? "  [driver: " : "", drv, drv[0] ? "]" : "");
        hw_add_kv_col(_("Link & driver"), st_line,
                      strcmp(state, "up") == 0 ? POWER_GOOD_COL : FG);
        hw_add_kv(_("MAC address"), mac);
        if (rx > 0 || tx > 0) {
            char rxs[48], txs[48], tr[120];
            hw_format_bytes((unsigned long long)rx, rxs, sizeof rxs);
            hw_format_bytes((unsigned long long)tx, txs, sizeof txs);
            snprintf(tr, sizeof tr, _("RX %s  /  TX %s"), rxs, txs);
            hw_add_kv(_("Traffic"), tr);
        }
    }
    closedir(d);
}

static void hw_collect_network(void)
{
    int before = nhw_rows;
#if defined(__linux__) && !defined(HW_DISABLE_DEV_IOCTL)
    /* Physical NICs in LOCachy live in /run/netns/locachy-net: inspect them
     * via a short-lived child that enters locachy-net and mounts a private sysfs. */
    int nsfd = open(HW_NETNS_LOCACHY, O_RDONLY | O_CLOEXEC);
    if (nsfd >= 0) {
        int pfd[2];
        if (pipe2(pfd, O_CLOEXEC) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                close(pfd[0]);
                if (setns(nsfd, CLONE_NEWNET) == 0 && unshare(CLONE_NEWNS) == 0 &&
                    mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0 &&
                    mount("sysfs", "/sys", "sysfs", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0) {
                    DIR *nd = opendir("/sys/class/net");
                    struct dirent *ne;
                    FILE *out = fdopen(pfd[1], "w");
                    while (nd && out && (ne = readdir(nd)) != NULL) {
                        const char *ifn = ne->d_name;
                        if (ifn[0] == '.' || strcmp(ifn, "lo") == 0) continue;
                        char p[512], st[32] = "", mac[48] = "", drv[48] = "";
                        int ok = 0;
                        snprintf(p, sizeof p, "/sys/class/net/%s/operstate", ifn);
                        hw_read_line(p, st, sizeof st);
                        snprintf(p, sizeof p, "/sys/class/net/%s/address", ifn);
                        hw_read_line(p, mac, sizeof mac);
                        snprintf(p, sizeof p, "/sys/class/net/%s/speed", ifn);
                        long long mbps = hw_read_ll(p, &ok);
                        snprintf(p, sizeof p, "/sys/class/net/%s/device/driver", ifn);
                        hw_read_link_basename(p, drv, sizeof drv);
                        snprintf(p, sizeof p, "/sys/class/net/%s/statistics/rx_bytes", ifn);
                        long long rx = hw_read_ll(p, &ok);
                        snprintf(p, sizeof p, "/sys/class/net/%s/statistics/tx_bytes", ifn);
                        long long tx = hw_read_ll(p, &ok);
                        fprintf(out, "%s\t%s\t%s\t%lld\t%s\t%lld\t%lld\n",
                                ifn, st[0] ? st : "unknown", mac[0] ? mac : "-",
                                mbps > 0 ? mbps : -1LL, drv[0] ? drv : "-", rx, tx);
                    }
                    if (nd) closedir(nd);
                    if (out) fclose(out);
                }
                _exit(0);
            }
            close(pfd[1]);
            FILE *in = fdopen(pfd[0], "r");
            char line[320];
            while (in && fgets(line, sizeof line, in)) {
                char ifn[48], st[32], mac[48], drv[48];
                long long mbps = -1, rx = 0, tx = 0;
                if (sscanf(line, "%47[^\t]\t%31[^\t]\t%47[^\t]\t%lld\t%47[^\t]\t%lld\t%lld",
                           ifn, st, mac, &mbps, drv, &rx, &tx) == 7) {
                    char title[96], st_line[140];
                    snprintf(title, sizeof title, "%s (locachy-net)", ifn);
                    hw_add_section(title);
                    size_t sp = (size_t)hw_scpy(st_line, sizeof st_line, st);
                    if (mbps > 0) sp += (size_t)snprintf(st_line + sp, sizeof st_line - sp, ", %lld Mbps", mbps);
                    if (strcmp(drv, "-") != 0) sp += (size_t)snprintf(st_line + sp, sizeof st_line - sp, "  [driver: %s]", drv);
                    hw_add_kv_col(_("Link & driver"), st_line, strcmp(st, "up") == 0 ? POWER_GOOD_COL : FG);
                    if (strcmp(mac, "-") != 0) hw_add_kv(_("MAC address"), mac);
                    if (rx > 0 || tx > 0) {
                        char rxs[48], txs[48], tr[120];
                        hw_format_bytes((unsigned long long)rx, rxs, sizeof rxs);
                        hw_format_bytes((unsigned long long)tx, txs, sizeof txs);
                        snprintf(tr, sizeof tr, _("RX %s  /  TX %s"), rxs, txs);
                        hw_add_kv(_("Traffic"), tr);
                    }
                }
            }
            if (in) fclose(in);
            if (pid > 0) (void)waitpid(pid, NULL, 0);
        }
        close(nsfd);
    }
#endif
    char hnet[128];
    snprintf(hnet, sizeof hnet, "%s/class/net", HW_SYS_ROOT);
    hw_scan_net_sysfs(hnet, "host");
    if (nhw_rows == before) {
        hw_add_section(_("Network Interfaces"));
        hw_add_note(_("No active network adapters found."));
    }
}

/* --- 10. Firmware & DMI + Summary --- */

static void hw_collect_firmware(void)
{
    static const struct { const char *file, *label; } fields[] = {
        { "sys_vendor",          N_("System vendor") },
        { "product_name",        N_("Product name") },
        { "product_version",     N_("Product version") },
        { "product_sku",         N_("Product SKU") },
        { "product_family",      N_("Product family") },
        { "board_vendor",        N_("Board vendor") },
        { "board_name",          N_("Board model") },
        { "board_version",       N_("Board version") },
        { "bios_vendor",         N_("BIOS vendor") },
        { "bios_version",        N_("BIOS version") },
        { "bios_date",           N_("BIOS release date") },
        { "ec_firmware_release", N_("EC firmware release") },
    };

    hw_add_section(_("System & Firmware DMI"));
    int any = 0;
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        char path[512], val[160];
        snprintf(path, sizeof path, "%s/class/dmi/id/%s", HW_SYS_ROOT, fields[i].file);
        if (hw_read_line(path, val, sizeof val) == 0 && hw_meaningful_dmi(val)) {
            hw_add_kv(_(fields[i].label), val);
            any = 1;
        }
    }
    if (!any) hw_add_note(_("No DMI firmware strings reported."));

    hw_add_section(_("Boot & Platform Security"));
    char path[512], val[160];
    snprintf(path, sizeof path, "%s/firmware/efi/fw_platform_size", HW_SYS_ROOT);
    if (hw_read_line(path, val, sizeof val) == 0 && val[0]) {
        char u[64];
        snprintf(u, sizeof u, _("UEFI (%.16s-bit)"), val);
        hw_add_kv(_("Boot mode"), u);
    } else {
        hw_add_kv(_("Boot mode"), _("Legacy BIOS / non-EFI"));
    }
    snprintf(path, sizeof path, "%s/class/tpm/tpm0/tpm_version_major", HW_SYS_ROOT);
    if (hw_read_line(path, val, sizeof val) == 0 && val[0]) {
        char t[48];
        snprintf(t, sizeof t, "TPM %.16s.0", val);
        hw_add_kv(_("TPM hardware"), t);
    }
    snprintf(path, sizeof path, "%s/cmdline", HW_PROC_ROOT);
    if (hw_read_line(path, val, sizeof val) == 0 && val[0])
        hw_add_kv(_("Kernel cmdline"), val);

    snprintf(path, sizeof path, "%s/modules", HW_PROC_ROOT);
    FILE *fm = fopen(path, "r");
    if (fm) {
        int nmods = 0;
        char ln[256];
        while (fgets(ln, sizeof ln, fm)) nmods++;
        fclose(fm);
        snprintf(val, sizeof val, _("%d loaded modules"), nmods);
        hw_add_kv(_("Kernel modules"), val);
    }
}

static void hw_collect_summary(void)
{
    load_about_summary();
    power_refresh_devices();

    hw_add_section(_("System Overview"));
    hw_add_kv(_("Operating system"), about_os_ver[0] ? about_os_ver : "LOCachy");
    char hn[64] = "";
    hw_read_line("/etc/hostname", hn, sizeof hn);
    hw_add_kv(_("Hostname"), hn);

    struct utsname u;
    char kver[128] = "";
    char kpath[256];
    snprintf(kpath, sizeof kpath, "%s/sys/kernel/osrelease", HW_PROC_ROOT);
    if (hw_read_line(kpath, kver, sizeof kver) != 0 && uname(&u) == 0)
        snprintf(kver, sizeof kver, "%.80s (%.32s)", u.release, u.machine);
    hw_add_kv(_("Linux kernel"), kver);

    /* Uptime & load */
    snprintf(kpath, sizeof kpath, "%s/uptime", HW_PROC_ROOT);
    char upbuf[64] = "";
    if (hw_read_line(kpath, upbuf, sizeof upbuf) == 0) {
        long secs = (long)atof(upbuf);
        char utxt[96];
        snprintf(utxt, sizeof utxt, _("%ldd %02ldh %02ldm"),
                 secs / 86400, (secs % 86400) / 3600, (secs % 3600) / 60);
        hw_add_kv(_("Uptime"), utxt);
    }
    snprintf(kpath, sizeof kpath, "%s/loadavg", HW_PROC_ROOT);
    if (hw_read_line(kpath, upbuf, sizeof upbuf) == 0)
        hw_add_kv(_("Load average"), upbuf);

    hw_add_section(_("Hardware Summary"));
    hw_add_kv(_("Computer"), about_hw_model[0] ? about_hw_model : _("Generic x86_64 system"));
    hw_add_kv(_("Processor"), about_cpu_brand);
    hw_add_kv(_("Memory"), about_mem_total);

    /* Batteries at a glance */
    for (int i = 0; i < npower_devs; i++) {
        const struct bat_device *b = &power_devs[i];
        if (b->is_peripheral) continue;
        char k[64], v[160];
        if (b->is_offline_pack) snprintf(k, sizeof k, _("Spare pack (%.36s)"), b->model[0] ? b->model : b->pack_id);
        else snprintf(k, sizeof k, _("Battery %.36s"), b->name);
        if (b->health_pct > 0) {
            snprintf(v, sizeof v, _("%d.%d%% (%s), health %.1f%% (%.1f / %.1f Wh)"),
                     b->capacity_decipct / 10, b->capacity_decipct % 10,
                     power_state_label(b->status), b->health_pct,
                     b->energy_full_wh, b->energy_design_wh);
        } else {
            snprintf(v, sizeof v, "%d%% (%s)", b->capacity_pct, power_state_label(b->status));
        }
        hw_add_kv(k, v);
    }
}

static void hw_populate_category(int cat)
{
    hw_clear_rows();
    switch (cat) {
    case HWCAT_SUMMARY:  hw_collect_summary();  break;
    case HWCAT_CPU:      hw_collect_cpu();      break;
    case HWCAT_MEMORY:   hw_collect_memory();   break;
    case HWCAT_STORAGE:  hw_collect_storage();  break;
    case HWCAT_GRAPHICS: hw_collect_graphics(); break;
    case HWCAT_SENSORS:  hw_collect_sensors();  break;
    case HWCAT_PCI:      hw_collect_pci();      break;
    case HWCAT_USB:      hw_collect_usb();      break;
    case HWCAT_NETWORK:  hw_collect_network();  break;
    case HWCAT_FIRMWARE: hw_collect_firmware(); break;
    }
}

static void hw_build_summary_clipboard(char *buf, size_t size)
{
    load_about_summary();
    power_refresh_devices();
    size_t p = 0;
    p += (size_t)snprintf(buf + p, size - p,
                          "System:    %s\nKernel:    %s\nModel:     %s\nProcessor: %s\nMemory:    %s\n",
                          about_os_ver[0] ? about_os_ver : "LOCachy",
                          about_kernel_ver[0] ? about_kernel_ver : "-",
                          about_hw_model[0] ? about_hw_model : "-",
                          about_cpu_brand[0] ? about_cpu_brand : "-",
                          about_mem_total[0] ? about_mem_total : "-");
    for (int i = 0; i < npower_devs && p + 96 < size; i++) {
        const struct bat_device *b = &power_devs[i];
        if (b->is_peripheral) continue;
        p += (size_t)snprintf(buf + p, size - p,
                              "Battery:   %s %d.%d%% (%s), health %.1f%% (%.1f / %.1f Wh)\n",
                              b->name, b->capacity_decipct / 10, b->capacity_decipct % 10,
                              b->status, b->health_pct > 0 ? b->health_pct : 0.0,
                              b->energy_full_wh > 0 ? b->energy_full_wh : 0.0,
                              b->energy_design_wh > 0 ? b->energy_design_wh : 0.0);
    }
}

static void copy_system_summary_action(void)
{
    char rep[2048];
    hw_build_summary_clipboard(rep, sizeof rep);
    if (clipboard_set && clipboard_set(0, rep, strlen(rep)) == 0)
        hw_scpy(notice, sizeof notice, _("Copied system summary to clipboard."));
    else
        hw_scpy(notice, sizeof notice, _("Clipboard service is not running."));
}

/* --- Interactive Hardware Information Browser UI --- */

static int hw_cat;
static int hw_in_pane; /* 0: left category tree, 1: right detail pane */
static int hw_scroll;
static int hw_sel_row;

static void hw_update_sensor_spark(void)
{
    if (hw_cat != HWCAT_SENSORS || nhw_sensors <= 0) return;
    int sidx = 0;
    if (hw_sel_row >= 0 && hw_sel_row < nhw_rows && hw_rows[hw_sel_row].sensor_idx >= 0)
        sidx = hw_rows[hw_sel_row].sensor_idx;
    const struct hw_sensor *s = &hw_sensors[sidx];
    char id[120];
    snprintf(id, sizeof id, "%.47s:%.63s", s->chip, s->label);
    if (strcmp(id, hw_spark_key) != 0) {
        hw_scpy(hw_spark_key, sizeof hw_spark_key, id);
        hw_spark_n = 0;
    }
    if (hw_spark_n == HW_SPARK_LEN) {
        memmove(hw_spark, hw_spark + 1, (HW_SPARK_LEN - 1) * sizeof hw_spark[0]);
        hw_spark_n = HW_SPARK_LEN - 1;
    }
    hw_spark[hw_spark_n++] = s->val;
}

static int hw_sidebar_w(int cols)
{
    if (cols < 64) return 0;
    int w = 22;
    for (int i = 0; i < NHWCATS; i++) {
        int c = ui_cells(_(hwcat_names[i])) + 3;
        if (c > w) w = c;
    }
    if (w > cols / 3) w = cols / 3;
    return w;
}

static void hwinfo_draw(void)
{
    int cols = screen_cols(), rows = screen_rows();
    int sw = hw_sidebar_w(cols);
    screen_fill(0, 0, cols, rows, BG);
    screen_bar(_("settings — Hardware Information"), NULL);

    if (sw > 0) {
        for (int c = 0; c < NHWCATS && PAGE_TOP + c < rows - 3; c++) {
            int on = (c == hw_cat);
            uint32_t bg = on ? (hw_in_pane ? SIDE_BG : ACCENT) : BG;
            screen_fill(0, PAGE_TOP + c, sw, 1, bg);
            screen_text(1, PAGE_TOP + c, _(hwcat_names[c]), on && !hw_in_pane ? BG : FG, bg);
        }
        screen_fill(sw, PAGE_TOP, 1, rows - 3 - PAGE_TOP, SIDE_BG);
    }

    int px = sw > 0 ? sw + 2 : 2;
    int pw = cols - px - 1;
    int top = PAGE_TOP;

    /* Live rolling sparkline card at top of Sensors category */
    if (hw_cat == HWCAT_SENSORS && nhw_sensors > 0 && rows >= 22 && pw >= 30) {
        int sidx = 0;
        if (hw_sel_row >= 0 && hw_sel_row < nhw_rows && hw_rows[hw_sel_row].sensor_idx >= 0)
            sidx = hw_rows[hw_sel_row].sensor_idx;
        const struct hw_sensor *s = &hw_sensors[sidx];
        int ch_rows = 5;
        screen_fill(px, top, pw, ch_rows, POWER_CARD_BG);

        double mn = hw_spark_n ? hw_spark[0] : s->val;
        double mx = mn;
        for (int i = 1; i < hw_spark_n; i++) {
            if (hw_spark[i] < mn) mn = hw_spark[i];
            if (hw_spark[i] > mx) mx = hw_spark[i];
        }
        const char *unit = s->kind == 'T' ? "°C" : s->kind == 'F' ? "RPM" : s->kind == 'P' ? "W" : "V";
        char shdr[160];
        snprintf(shdr, sizeof shdr, _("Live curve: %.28s / %.36s = %.1f %s  (min %.1f, max %.1f %s)"),
                 s->chip, s->label, s->val, unit, mn, mx, unit);
        screen_text(px + 1, top, shdr, ACCENT, POWER_CARD_BG);

        double lo = mn - (mx - mn) * 0.15 - 1.0;
        double hi = mx + (mx - mn) * 0.15 + 1.0;
        if (s->kind != 'T' && lo < 0) lo = 0;
        if (hi <= lo + 0.5) hi = lo + 2.0;

        int cw = 0, ch = 0, top_px = 0;
        if (screen_cells && screen_fill_px) screen_cells(&cw, &ch, &top_px);
        int plot_cols = pw - 4;
        if (plot_cols > HW_SPARK_LEN) plot_cols = HW_SPARK_LEN;
        if (cw > 0 && ch > 0 && screen_fill_px && hw_spark_n > 0 && plot_cols > 4) {
            int gx0 = (px + 2) * cw;
            int gy0 = top_px + (top + 1) * ch + 2;
            int gw = plot_cols * cw;
            int gh = (ch_rows - 1) * ch - 4;
            if (gh < 6) gh = 6;
            int gy1 = gy0 + gh - 1;
            screen_fill_px(gx0, gy0 + gh / 2, gw, 1, POWER_GRID_COL);
            int prev_y = -1;
            for (int dx = 0; dx < gw; dx++) {
                int idx = (dx * hw_spark_n) / gw;
                if (idx >= hw_spark_n) idx = hw_spark_n - 1;
                double v = hw_spark[idx];
                int y = gy1 - (int)(((v - lo) / (hi - lo)) * (gh - 1) + 0.5);
                px_vline_clamped(gx0 + dx, prev_y >= 0 ? prev_y : y, y, gy0, gy1, POWER_CHG_COL);
                prev_y = y;
            }
        } else if (hw_spark_n > 0 && plot_cols > 4) {
            for (int c = 0; c < plot_cols && c < hw_spark_n; c++) {
                double v = hw_spark[hw_spark_n - 1 - (plot_cols - 1 - c) >= 0 ?
                                    hw_spark_n - 1 - (plot_cols - 1 - c) : 0];
                int rr = top + 1 + (ch_rows - 2) - (int)(((v - lo) / (hi - lo)) * (ch_rows - 2) + 0.5);
                if (rr >= top + 1 && rr < top + ch_rows)
                    screen_text(px + 2 + c, rr, "*", POWER_CHG_COL, POWER_CARD_BG);
            }
        }
        top += ch_rows + 1;
    }

    int avail = rows - 3 - top;
    if (avail < 1) avail = 1;
    if (hw_sel_row >= nhw_rows) hw_sel_row = nhw_rows > 0 ? nhw_rows - 1 : 0;
    if (hw_sel_row < hw_scroll) hw_scroll = hw_sel_row;
    if (hw_sel_row >= hw_scroll + avail) hw_scroll = hw_sel_row - avail + 1;
    if (hw_scroll > nhw_rows - avail) hw_scroll = nhw_rows - avail;
    if (hw_scroll < 0) hw_scroll = 0;

    int key_w = pw >= 100 ? 28 : 22;
    for (int r = 0; r < avail && hw_scroll + r < nhw_rows; r++) {
        int idx = hw_scroll + r;
        const struct hw_row *row = &hw_rows[idx];
        int y = top + r;
        int sel = hw_in_pane && idx == hw_sel_row;
        uint32_t rbg = sel ? ACCENT : BG;
        if (sel) screen_fill(px, y, pw, 1, ACCENT);

        if (row->kind == HWROW_SECTION) {
            screen_text(px, y, row->key, sel ? BG : ACCENT, rbg);
        } else if (row->kind == HWROW_NOTE) {
            screen_text(px + 2, y, row->val, sel ? BG : DIM, rbg);
        } else if (row->kind == HWROW_BAR) {
            char kpad[96], bar[32];
            ui_pad(kpad, sizeof kpad, row->key, key_w);
            int bw = 16, filled = (row->bar_pct * bw + 50) / 100;
            bar[0] = '[';
            for (int b = 0; b < bw; b++) bar[1 + b] = b < filled ? '#' : '-';
            bar[1 + bw] = ']';
            bar[2 + bw] = ' ';
            bar[3 + bw] = '\0';
            screen_text(px + 2, y, kpad, sel ? BG : DIM, rbg);
            screen_text(px + 2 + key_w + 1, y, bar, sel ? BG : POWER_DIS_COL, rbg);
            screen_text(px + 2 + key_w + 1 + bw + 3, y, row->val, sel ? BG : FG, rbg);
        } else {
            char kpad[96];
            ui_pad(kpad, sizeof kpad, row->key, key_w);
            screen_text(px + 2, y, kpad, sel ? BG : DIM, rbg);
            screen_text(px + 2 + key_w + 1, y, row->val, sel ? BG : row->val_col, rbg);
        }
    }

    /* TRANSLATORS: Tab, r, c and Esc are key names; keep them. */
    screen_text(2, rows - 3, cols < 70 ? _("Up/Down: move  Tab: pane  r: refresh  c: copy  Esc: back") :
                _("Up/Down: select   Tab / Left / Right: switch pane   r: refresh   c: copy page   Esc: back"),
                DIM, BG);
    screen_text(2, rows - 2, _(notice), ACCENT, BG);
    screen_flush();
}

static void hw_copy_current_page(void)
{
    char buf[16384];
    size_t pos = (size_t)snprintf(buf, sizeof buf, "=== %s ===\n", _(hwcat_names[hw_cat]));
    for (int i = 0; i < nhw_rows && pos + 200 < sizeof buf; i++) {
        const struct hw_row *r = &hw_rows[i];
        if (r->kind == HWROW_SECTION)
            pos += (size_t)snprintf(buf + pos, sizeof buf - pos, "\n[%s]\n", r->key);
        else if (r->kind == HWROW_NOTE)
            pos += (size_t)snprintf(buf + pos, sizeof buf - pos, "  %s\n", r->val);
        else
            pos += (size_t)snprintf(buf + pos, sizeof buf - pos, "  %-28s %s\n", r->key, r->val);
    }
    if (clipboard_set && clipboard_set(0, buf, pos) == 0)
        snprintf(notice, sizeof notice, _("Copied %s to clipboard."), _(hwcat_names[hw_cat]));
    else
        hw_scpy(notice, sizeof notice, _("Clipboard service is not running."));
}

static void hwinfo_page(void)
{
    hw_cat = HWCAT_SUMMARY;
    hw_in_pane = 0;
    hw_scroll = 0;
    hw_sel_row = 0;
    hw_spark_n = 0;
    hw_spark_key[0] = '\0';
    notice[0] = '\0';

    hw_populate_category(hw_cat);
    hwinfo_draw();

    for (;;) {
        int timeout_ms = (hw_cat == HWCAT_SENSORS || hw_cat == HWCAT_CPU || hw_cat == HWCAT_MEMORY) ? 1000 : 5000;
        int key = key_poll_timeout(-1, timeout_ms);
        if (key == 0 || key == K_SHOWN) {
            if (hw_cat == HWCAT_SENSORS || hw_cat == HWCAT_CPU || hw_cat == HWCAT_MEMORY) {
                hw_populate_category(hw_cat);
                hw_update_sensor_spark();
            }
            hwinfo_draw();
            continue;
        }
        if (key == K_POINTER) {
            const struct pointer_event *ev = pointer_last();
            int cols = screen_cols(), sw = hw_sidebar_w(cols);
            int on_side = sw > 0 && ev->col < sw;
            if (ev->wheel) {
                if (on_side) {
                    int nc = hw_cat - ev->wheel;
                    if (nc < 0) nc = 0;
                    if (nc >= NHWCATS) nc = NHWCATS - 1;
                    if (nc != hw_cat) {
                        hw_cat = nc;
                        hw_scroll = hw_sel_row = 0;
                        hw_populate_category(hw_cat);
                        hw_update_sensor_spark();
                    }
                } else {
                    hw_sel_row -= ev->wheel * 2;
                    if (hw_sel_row < 0) hw_sel_row = 0;
                    if (hw_sel_row >= nhw_rows) hw_sel_row = nhw_rows > 0 ? nhw_rows - 1 : 0;
                    hw_update_sensor_spark();
                }
                hwinfo_draw();
                continue;
            }
            if (ev->button == BTN_LEFT && ev->pressed) {
                if (on_side) {
                    int c = ev->row - PAGE_TOP;
                    if (c >= 0 && c < NHWCATS) {
                        hw_cat = c;
                        hw_in_pane = 1;
                        hw_scroll = hw_sel_row = 0;
                        hw_populate_category(hw_cat);
                        hw_update_sensor_spark();
                        hwinfo_draw();
                    }
                } else {
                    hw_in_pane = 1;
                    int top = PAGE_TOP + (hw_cat == HWCAT_SENSORS && nhw_sensors > 0 && screen_rows() >= 22 ? 6 : 0);
                    int r = ev->row - top;
                    if (r >= 0 && hw_scroll + r < nhw_rows) {
                        hw_sel_row = hw_scroll + r;
                        hw_update_sensor_spark();
                        hwinfo_draw();
                    }
                }
            }
            continue;
        }
        if (key == 'q' || (key == 27 && !hw_in_pane)) {
            notice[0] = '\0';
            return;
        }
        if (key == 27 && hw_in_pane) {
            hw_in_pane = 0;
            hwinfo_draw();
            continue;
        }
        if (key == '\t') {
            hw_in_pane = !hw_in_pane;
        } else if (!hw_in_pane && key == K_RIGHT) {
            hw_in_pane = 1;
        } else if (hw_in_pane && key == K_LEFT) {
            hw_in_pane = 0;
        } else if (!hw_in_pane && (key == K_UP || key == K_DOWN)) {
            int nc = hw_cat + (key == K_UP ? -1 : 1);
            if (nc < 0) nc = 0;
            if (nc >= NHWCATS) nc = NHWCATS - 1;
            if (nc != hw_cat) {
                hw_cat = nc;
                hw_scroll = hw_sel_row = 0;
                hw_populate_category(hw_cat);
                hw_update_sensor_spark();
            }
        } else if (!hw_in_pane && key == '\n') {
            hw_in_pane = 1;
        } else if (hw_in_pane && (key == K_UP || key == K_DOWN)) {
            hw_sel_row += (key == K_UP ? -1 : 1);
            if (hw_sel_row < 0) hw_sel_row = 0;
            if (hw_sel_row >= nhw_rows) hw_sel_row = nhw_rows > 0 ? nhw_rows - 1 : 0;
            hw_update_sensor_spark();
        } else if (hw_in_pane && (key == K_PGUP || key == K_PGDN)) {
            int step = screen_rows() - 8;
            if (step < 4) step = 4;
            hw_sel_row += (key == K_PGUP ? -step : step);
            if (hw_sel_row < 0) hw_sel_row = 0;
            if (hw_sel_row >= nhw_rows) hw_sel_row = nhw_rows > 0 ? nhw_rows - 1 : 0;
            hw_update_sensor_spark();
        } else if (hw_in_pane && (key == K_BOL || key == K_EOL)) {
            hw_sel_row = (key == K_BOL) ? 0 : (nhw_rows > 0 ? nhw_rows - 1 : 0);
            hw_update_sensor_spark();
        } else if (key == 'r' || key == 'R') {
            hw_populate_category(hw_cat);
            hw_update_sensor_spark();
            hw_scpy(notice, sizeof notice, _("Hardware readings refreshed."));
        } else if (key == 'c' || key == 'C') {
            hw_copy_current_page();
        } else {
            continue;
        }
        hwinfo_draw();
    }
}
