/*
 * System and peripheral battery readings (/sys/class/power_supply), physical
 * pack identity tracking, removable/dual-battery handoff detection, and
 * persistent 7-day rolling history (/var/lib/locachy/battery/).
 *
 * Keying history by physical pack identity (vendor + model + serial) rather
 * than slot name ("BAT0") alone keeps separate curves and health records when
 * a user swaps batteries (hot-swap on AC or cold-swap across hibernate/off),
 * retains removed spare packs for inspection, marks uninserted intervals as
 * 'R' (removed) rather than sleep gaps, records slot/discharge handoffs in
 * events.tsv, and re-applies configured charge thresholds upon pack insertion.
 */
#ifndef BATTERY_HISTORY_H
#define BATTERY_HISTORY_H

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef POWER_SUPPLY_ROOT
#define POWER_SUPPLY_ROOT "/sys/class/power_supply"
#endif

#ifndef BATTERY_HISTORY_DIR
#define BATTERY_HISTORY_DIR "/var/lib/locachy/battery"
#endif

#define BAT_KEEP_SECONDS   (7 * 86400 + 7200) /* 7 days + 2 h margin */
#define BAT_COMPACT_BYTES  (512 * 1024L)
#define BAT_MAX_DEVICES    16
#define BAT_MAX_SAMPLES    12000
#define BAT_MAX_EVENTS     256

struct bat_device {
    char pack_id[96];        /* physical pack key: vendor_model_serial (or slot fallback) */
    char name[48];           /* kernel slot name ("BAT0", "BAT1"), or last slot for offline pack */
    char vendor[64];
    char model[64];
    char serial[64];
    char technology[32];
    char status[32];         /* Charging, Discharging, Not charging, Full, Removed, Unknown */
    int is_peripheral;       /* scope == "Device" (e.g. wireless mouse/keyboard) */
    int is_offline_pack;     /* 1: previously seen removable pack currently not in any slot */
    int64_t last_seen_ts;    /* epoch seconds when last present */
    int rechargeable;
    int present;
    int capacity_pct;        /* 0..100 */
    int capacity_decipct;    /* 0..1000 (0.1% precision when energy/charge known) */
    double energy_now_wh;    /* < 0 when unknown */
    double energy_full_wh;
    double energy_design_wh;
    double health_pct;       /* energy_full / energy_design * 100, < 0 when unknown */
    double power_w;          /* >= 0, < 0 when unknown */
    double voltage_v;        /* < 0 when unknown */
    double voltage_design_v; /* < 0 when unknown */
    double temp_c;           /* < -100 when unknown */
    int cycles;              /* > 0 when reported, else -1 */
    int has_start_thresh, has_end_thresh;
    int start_thresh, end_thresh;
    char start_attr[40], end_attr[40];
};

struct bat_ac_info {
    int any_adapter;
    int online;
    char name[48];
    double voltage_v;
    double current_a;
};

struct bat_sample {
    int64_t ts;       /* Unix epoch seconds */
    int decipct;      /* 0..1000 */
    int power_mw;     /* >= 0 */
    int energy_mwh;   /* >= 0 */
    int voltage_mv;   /* >= 0 */
    char state;       /* 'C' chg, 'D' dis, 'F' full, 'N' idle, 'R' removed, 'I' inserted, 'U' unk */
    int ac;           /* 0 or 1 */
};

/* Handoff event kinds in /var/lib/locachy/battery/events.tsv:
 *   SWAP      ts slot old_pack old_dpct new_pack new_dpct
 *   REMOVE    ts slot old_pack old_dpct -        ac
 *   INSERT    ts slot -        ac       new_pack new_dpct
 *   DISCHARGE ts slot from_pack from_dpct to_pack to_dpct   (slot = "BATx->BATy") */
struct bat_handoff_event {
    char kind[16];
    int64_t ts;
    char slot[48];
    char pack_a[96];
    int dpct_a;
    char pack_b[96];
    int dpct_b;
};

static inline const char *bat_history_dir(void)
{
    const char *env = getenv("LOCACHY_BATTERY_HISTORY_DIR");
    return env && env[0] ? env : BATTERY_HISTORY_DIR;
}

static inline const char *bat_supply_root(void)
{
    const char *env = getenv("LOCACHY_POWER_SUPPLY_DIR");
    return env && env[0] ? env : POWER_SUPPLY_ROOT;
}

static inline int64_t bat_now_ts(void)
{
    const char *ov = getenv("LOCACHY_NOW_TS");
    if (ov && ov[0]) {
        long long v = strtoll(ov, NULL, 10);
        if (v >= 946684800LL) return (int64_t)v;
    }
    return (int64_t)time(NULL);
}

static inline int bat_safe_name(const char *s)
{
    if (!s || !s[0] || s[0] == '.') return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.')) return 0;
    }
    return 1;
}

static inline void bat_sanitize_token(const char *in, char *out, size_t size)
{
    size_t w = 0;
    int prev_us = 1;
    for (const unsigned char *p = (const unsigned char *)in; *p && w + 1 < size; p++) {
        if (isalnum(*p) || *p == '-') {
            out[w++] = (char)*p;
            prev_us = 0;
        } else if (!prev_us) {
            out[w++] = '_';
            prev_us = 1;
        }
    }
    while (w > 0 && out[w - 1] == '_') w--;
    out[w] = '\0';
}

/* Derive stable physical pack identifier from vendor + model + serial (+ design
 * capacity when serial is omitted). Falls back to slot name when none exist. */
static inline void bat_compute_pack_id(struct bat_device *b)
{
    char v[32] = "", m[36] = "", s[36] = "";
    bat_sanitize_token(b->vendor, v, sizeof v);
    bat_sanitize_token(b->model, m, sizeof m);
    bat_sanitize_token(b->serial, s, sizeof s);

    if (!v[0] && !m[0] && !s[0]) {
        snprintf(b->pack_id, sizeof b->pack_id, "%s", b->name);
        return;
    }
    size_t pos = 0;
    if (v[0]) pos += (size_t)snprintf(b->pack_id + pos, sizeof b->pack_id - pos, "%s", v);
    if (m[0]) pos += (size_t)snprintf(b->pack_id + pos, sizeof b->pack_id - pos, "%s%s", pos ? "_" : "", m);
    if (s[0]) {
        pos += (size_t)snprintf(b->pack_id + pos, sizeof b->pack_id - pos, "%s%s", pos ? "_" : "", s);
    } else if (b->energy_design_wh > 0.5) {
        pos += (size_t)snprintf(b->pack_id + pos, sizeof b->pack_id - pos, "%s%dWh_%s",
                                pos ? "_" : "", (int)(b->energy_design_wh + 0.5), b->name);
    } else {
        pos += (size_t)snprintf(b->pack_id + pos, sizeof b->pack_id - pos, "%s%s", pos ? "_" : "", b->name);
    }
    if (!bat_safe_name(b->pack_id)) snprintf(b->pack_id, sizeof b->pack_id, "%s", b->name);
}

static inline int bat_read_line(const char *dir, const char *sub, const char *attr,
                                char *out, size_t size)
{
    char path[512];
    int n = sub ? snprintf(path, sizeof path, "%s/%s/%s", dir, sub, attr)
                : snprintf(path, sizeof path, "%s/%s", dir, attr);
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
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
    char *st = out;
    while (*st == ' ' || *st == '\t') st++;
    if (st != out) memmove(out, st, strlen(st) + 1);
    return 0;
}

static inline long long bat_read_ll(const char *dir, const char *sub, const char *attr, int *ok)
{
    char buf[64], *end = NULL;
    if (ok) *ok = 0;
    if (bat_read_line(dir, sub, attr, buf, sizeof buf) != 0 || !buf[0]) return 0;
    errno = 0;
    long long v = strtoll(buf, &end, 10);
    if (errno || end == buf || *end != '\0') return 0;
    if (ok) *ok = 1;
    return v;
}

static inline int bat_write_sysfs_int(const char *dir, const char *sub, const char *attr, int val)
{
    char path[512];
    int n = snprintf(path, sizeof path, "%s/%s/%s", dir, sub, attr);
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int ok = fprintf(f, "%d\n", val) > 0;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

/* Write start/end thresholds in EC-safe order so start never exceeds end. */
static inline int bat_apply_thresholds_to(struct bat_device *b, int new_start, int new_end)
{
    if (b->is_offline_pack || b->is_peripheral) return -1;
    const char *root = bat_supply_root();
    int s = new_start >= 0 ? new_start : b->start_thresh;
    int e = new_end > 0 ? new_end : b->end_thresh;
    if (b->has_start_thresh && b->has_end_thresh && s >= e) return -1;

    if (b->has_end_thresh && new_end > 0 && (!b->has_start_thresh || e >= b->end_thresh)) {
        if (bat_write_sysfs_int(root, b->name, b->end_attr, e) != 0) return -1;
        b->end_thresh = e;
    }
    if (b->has_start_thresh && new_start >= 0) {
        if (bat_write_sysfs_int(root, b->name, b->start_attr, s) != 0) return -1;
        b->start_thresh = s;
    }
    if (b->has_end_thresh && new_end > 0 && b->end_thresh != e) {
        if (bat_write_sysfs_int(root, b->name, b->end_attr, e) != 0) return -1;
        b->end_thresh = e;
    }
    return 0;
}

static inline char bat_state_code(const char *status)
{
    if (!status || !status[0]) return 'U';
    if (strcmp(status, "Charging") == 0) return 'C';
    if (strcmp(status, "Discharging") == 0) return 'D';
    if (strcmp(status, "Full") == 0) return 'F';
    if (strcmp(status, "Not charging") == 0 || strcmp(status, "Idle") == 0) return 'N';
    if (strcmp(status, "Removed") == 0) return 'R';
    return 'U';
}

static inline int bat_cmp_devices(const void *a, const void *b)
{
    const struct bat_device *x = a, *y = b;
    /* Order: inserted system packs -> offline removable packs -> peripherals */
    int rx = x->is_peripheral ? 2 : (x->is_offline_pack ? 1 : 0);
    int ry = y->is_peripheral ? 2 : (y->is_offline_pack ? 1 : 0);
    if (rx != ry) return rx - ry;
    int c = strcmp(x->name, y->name);
    return c != 0 ? c : strcmp(x->pack_id, y->pack_id);
}

/* Scan live /sys/class/power_supply devices only. */
static inline int bat_scan_live(struct bat_device *devs, int max, struct bat_ac_info *ac)
{
    const char *root = bat_supply_root();
    if (ac) {
        memset(ac, 0, sizeof *ac);
        ac->voltage_v = -1.0;
        ac->current_a = -1.0;
    }
    DIR *d = opendir(root);
    if (!d) return 0;
    int count = 0;
    int64_t now_ts = bat_now_ts();
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        char type[32] = "", scope[32] = "";
        if (!bat_safe_name(name)) continue;
        if (bat_read_line(root, name, "type", type, sizeof type) != 0) continue;
        if (strcmp(type, "Battery") != 0) {
            int ok = 0;
            long long on = bat_read_ll(root, name, "online", &ok);
            if (ok && ac) {
                ac->any_adapter = 1;
                if (on > 0) {
                    ac->online = 1;
                    if (!ac->name[0]) snprintf(ac->name, sizeof ac->name, "%.47s", name);
                    int vok = 0, iok = 0;
                    long long uv = bat_read_ll(root, name, "voltage_now", &vok);
                    long long ua = bat_read_ll(root, name, "current_now", &iok);
                    if (!iok) ua = bat_read_ll(root, name, "current_max", &iok);
                    if (vok && uv > 0) ac->voltage_v = (double)uv / 1e6;
                    if (iok && ua > 0) ac->current_a = (double)ua / 1e6;
                } else if (!ac->name[0]) {
                    snprintf(ac->name, sizeof ac->name, "%.47s", name);
                }
            }
            continue;
        }
        if (count >= max) continue;
        int pok = 0;
        long long pres = bat_read_ll(root, name, "present", &pok);
        if (pok && pres == 0) continue;

        struct bat_device *b = &devs[count];
        memset(b, 0, sizeof *b);
        snprintf(b->name, sizeof b->name, "%.47s", name);
        b->present = 1;
        b->last_seen_ts = now_ts;
        b->rechargeable = 1;
        b->energy_now_wh = b->energy_full_wh = b->energy_design_wh = -1.0;
        b->health_pct = b->power_w = b->voltage_v = b->voltage_design_v = -1.0;
        b->temp_c = -1000.0;
        b->cycles = -1;

        if (bat_read_line(root, name, "scope", scope, sizeof scope) == 0 &&
            strcmp(scope, "Device") == 0) {
            b->is_peripheral = 1;
        }
        bat_read_line(root, name, "manufacturer", b->vendor, sizeof b->vendor);
        bat_read_line(root, name, "model_name", b->model, sizeof b->model);
        bat_read_line(root, name, "serial_number", b->serial, sizeof b->serial);
        bat_read_line(root, name, "technology", b->technology, sizeof b->technology);
        if (b->technology[0] && strcasecmp(b->technology, "Unknown") == 0) b->technology[0] = '\0';
        if (bat_read_line(root, name, "status", b->status, sizeof b->status) != 0)
            snprintf(b->status, sizeof b->status, "Unknown");

        int ok = 0;
        long long uv = bat_read_ll(root, name, "voltage_now", &ok);
        if (ok && uv > 0) b->voltage_v = (double)uv / 1e6;
        long long uvd = bat_read_ll(root, name, "voltage_min_design", &ok);
        if (!ok || uvd <= 0) uvd = bat_read_ll(root, name, "voltage_max_design", &ok);
        if (ok && uvd > 0) b->voltage_design_v = (double)uvd / 1e6;
        double ref_v = b->voltage_design_v > 0 ? b->voltage_design_v : (b->voltage_v > 0 ? b->voltage_v : -1.0);

        int eok = 0, fok = 0, dok = 0;
        long long enow = bat_read_ll(root, name, "energy_now", &eok);
        long long efull = bat_read_ll(root, name, "energy_full", &fok);
        long long edes = bat_read_ll(root, name, "energy_full_design", &dok);
        if (eok && enow >= 0) b->energy_now_wh = (double)enow / 1e6;
        if (fok && efull > 0) b->energy_full_wh = (double)efull / 1e6;
        if (dok && edes > 0) b->energy_design_wh = (double)edes / 1e6;

        if (b->energy_full_wh < 0 && ref_v > 0) {
            int cok = 0, cfok = 0, cdok = 0;
            long long cnow = bat_read_ll(root, name, "charge_now", &cok);
            long long cfull = bat_read_ll(root, name, "charge_full", &cfok);
            long long cdes = bat_read_ll(root, name, "charge_full_design", &cdok);
            if (cok && cnow >= 0) b->energy_now_wh = ((double)cnow / 1e6) * ref_v;
            if (cfok && cfull > 0) b->energy_full_wh = ((double)cfull / 1e6) * ref_v;
            if (cdok && cdes > 0) b->energy_design_wh = ((double)cdes / 1e6) * ref_v;
        }

        if (b->energy_full_wh > 0 && b->energy_design_wh > 0) {
            b->health_pct = (b->energy_full_wh / b->energy_design_wh) * 100.0;
            if (b->health_pct > 200.0) b->health_pct = -1.0;
        } else {
            int cfok = 0, cdok = 0;
            long long cfull = bat_read_ll(root, name, "charge_full", &cfok);
            long long cdes = bat_read_ll(root, name, "charge_full_design", &cdok);
            if (cfok && cdok && cfull > 0 && cdes > 0) {
                b->health_pct = ((double)cfull / (double)cdes) * 100.0;
                if (b->health_pct > 200.0) b->health_pct = -1.0;
            }
        }

        int pwok = 0;
        long long uw = bat_read_ll(root, name, "power_now", &pwok);
        if (pwok && uw >= 0) {
            b->power_w = (double)uw / 1e6;
        } else {
            int iok = 0;
            long long ua = bat_read_ll(root, name, "current_now", &iok);
            if (iok && ua < 0) ua = -ua;
            double v = b->voltage_v > 0 ? b->voltage_v : ref_v;
            if (iok && ua >= 0 && v > 0) b->power_w = ((double)ua / 1e6) * v;
        }

        int cap_ok = 0;
        long long cap = bat_read_ll(root, name, "capacity", &cap_ok);
        if (cap_ok && cap >= 0) {
            if (cap > 100) cap = 100;
            b->capacity_pct = (int)cap;
            b->capacity_decipct = (int)cap * 10;
        }
        if (b->energy_now_wh >= 0 && b->energy_full_wh > 0.01) {
            double dp = (b->energy_now_wh / b->energy_full_wh) * 1000.0;
            if (dp < 0) dp = 0;
            if (dp > 1000) dp = 1000;
            b->capacity_decipct = (int)(dp + 0.5);
            b->capacity_pct = (b->capacity_decipct + 5) / 10;
        }

        long long cyc = bat_read_ll(root, name, "cycle_count", &ok);
        if (ok && cyc > 0 && cyc < 60000) b->cycles = (int)cyc;

        long long t10 = bat_read_ll(root, name, "temp", &ok);
        if (ok && t10 > -400 && t10 < 1200) b->temp_c = (double)t10 / 10.0;

        static const char *const start_names[] = {
            "charge_control_start_threshold", "charge_start_threshold"
        };
        static const char *const end_names[] = {
            "charge_control_end_threshold", "charge_stop_threshold"
        };
        for (size_t k = 0; k < 2 && !b->has_start_thresh; k++) {
            long long sv = bat_read_ll(root, name, start_names[k], &ok);
            if (ok && sv >= 0 && sv <= 100) {
                b->has_start_thresh = 1;
                b->start_thresh = (int)sv;
                snprintf(b->start_attr, sizeof b->start_attr, "%s", start_names[k]);
            }
        }
        for (size_t k = 0; k < 2 && !b->has_end_thresh; k++) {
            long long ev = bat_read_ll(root, name, end_names[k], &ok);
            if (ok && ev > 0 && ev <= 100) {
                b->has_end_thresh = 1;
                b->end_thresh = (int)ev;
                snprintf(b->end_attr, sizeof b->end_attr, "%s", end_names[k]);
            }
        }
        bat_compute_pack_id(b);
        count++;
    }
    closedir(d);
    if (count > 1) qsort(devs, (size_t)count, sizeof devs[0], bat_cmp_devices);
    if (ac && !ac->any_adapter && count > 0) {
        for (int i = 0; i < count; i++) {
            if (!devs[i].is_peripheral &&
                (strcmp(devs[i].status, "Charging") == 0 || strcmp(devs[i].status, "Full") == 0)) {
                ac->online = 1;
            }
        }
    }
    return count;
}

/* --- Persistent Pack Metadata (.meta) for Removable Pack Tracking --- */

static inline int bat_save_pack_meta(const struct bat_device *b, int inserted, int64_t ts)
{
    if (!bat_safe_name(b->pack_id) || b->is_peripheral) return -1;
    const char *dir = bat_history_dir();
    (void)mkdir(dir, 0755);
    char path[560], tmp[580];
    int n = snprintf(path, sizeof path, "%s/%s.meta", dir, b->pack_id);
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "pack_id=%s\nslot=%s\ninserted=%d\nlast_seen=%lld\n"
               "vendor=%s\nmodel=%s\nserial=%s\ntechnology=%s\n"
               "capacity_decipct=%d\nenergy_now_mwh=%d\nenergy_full_mwh=%d\n"
               "energy_design_mwh=%d\nvoltage_design_mv=%d\ncycles=%d\n",
            b->pack_id, b->name, inserted ? 1 : 0, (long long)ts,
            b->vendor, b->model, b->serial, b->technology,
            b->capacity_decipct,
            b->energy_now_wh >= 0 ? (int)(b->energy_now_wh * 1000.0 + 0.5) : -1,
            b->energy_full_wh > 0 ? (int)(b->energy_full_wh * 1000.0 + 0.5) : -1,
            b->energy_design_wh > 0 ? (int)(b->energy_design_wh * 1000.0 + 0.5) : -1,
            b->voltage_design_v > 0 ? (int)(b->voltage_design_v * 1000.0 + 0.5) : -1,
            b->cycles);
    int ok = fflush(f) == 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static inline int bat_load_pack_meta(const char *meta_path, struct bat_device *b, int *inserted_out)
{
    FILE *f = fopen(meta_path, "r");
    if (!f) return -1;
    memset(b, 0, sizeof *b);
    b->rechargeable = 1;
    b->energy_now_wh = b->energy_full_wh = b->energy_design_wh = -1.0;
    b->health_pct = b->power_w = b->voltage_v = b->voltage_design_v = -1.0;
    b->temp_c = -1000.0;
    b->cycles = -1;
    int ins = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0';
        if (strcmp(line, "pack_id") == 0) snprintf(b->pack_id, sizeof b->pack_id, "%s", eq);
        else if (strcmp(line, "slot") == 0) snprintf(b->name, sizeof b->name, "%s", eq);
        else if (strcmp(line, "inserted") == 0) ins = atoi(eq);
        else if (strcmp(line, "last_seen") == 0) b->last_seen_ts = strtoll(eq, NULL, 10);
        else if (strcmp(line, "vendor") == 0) snprintf(b->vendor, sizeof b->vendor, "%s", eq);
        else if (strcmp(line, "model") == 0) snprintf(b->model, sizeof b->model, "%s", eq);
        else if (strcmp(line, "serial") == 0) snprintf(b->serial, sizeof b->serial, "%s", eq);
        else if (strcmp(line, "technology") == 0) snprintf(b->technology, sizeof b->technology, "%s", eq);
        else if (strcmp(line, "capacity_decipct") == 0) {
            int dp = atoi(eq);
            if (dp >= 0 && dp <= 1000) {
                b->capacity_decipct = dp;
                b->capacity_pct = (dp + 5) / 10;
            }
        } else if (strcmp(line, "energy_now_mwh") == 0 && atoi(eq) >= 0) {
            b->energy_now_wh = atoi(eq) / 1000.0;
        } else if (strcmp(line, "energy_full_mwh") == 0 && atoi(eq) > 0) {
            b->energy_full_wh = atoi(eq) / 1000.0;
        } else if (strcmp(line, "energy_design_mwh") == 0 && atoi(eq) > 0) {
            b->energy_design_wh = atoi(eq) / 1000.0;
        } else if (strcmp(line, "voltage_design_mv") == 0 && atoi(eq) > 0) {
            b->voltage_design_v = atoi(eq) / 1000.0;
        } else if (strcmp(line, "cycles") == 0 && atoi(eq) > 0) {
            b->cycles = atoi(eq);
        }
    }
    fclose(f);
    if (!bat_safe_name(b->pack_id)) return -1;
    if (b->energy_full_wh > 0 && b->energy_design_wh > 0) {
        b->health_pct = (b->energy_full_wh / b->energy_design_wh) * 100.0;
        if (b->health_pct > 200.0) b->health_pct = -1.0;
    }
    snprintf(b->status, sizeof b->status, "Removed");
    b->is_offline_pack = 1;
    if (inserted_out) *inserted_out = ins;
    return 0;
}

/* Scan live devices AND append any recently seen offline removable battery
 * packs from .meta files in the history directory that are not inserted. */
static inline int bat_scan(struct bat_device *devs, int max, struct bat_ac_info *ac)
{
    int count = bat_scan_live(devs, max, ac);
    DIR *hd = opendir(bat_history_dir());
    if (!hd) return count;

    int64_t now_ts = bat_now_ts();
    struct dirent *e;
    while ((e = readdir(hd)) != NULL && count < max) {
        size_t len = strlen(e->d_name);
        if (len <= 5 || strcmp(e->d_name + len - 5, ".meta") != 0) continue;
        char mpath[560];
        int n = snprintf(mpath, sizeof mpath, "%s/%s", bat_history_dir(), e->d_name);
        if (n <= 0 || (size_t)n >= sizeof mpath) continue;
        struct bat_device saved;
        if (bat_load_pack_meta(mpath, &saved, NULL) != 0) continue;
        if (now_ts >= 946684800LL && saved.last_seen_ts > 0 &&
            now_ts - saved.last_seen_ts > BAT_KEEP_SECONDS) {
            continue;
        }
        int already_live = 0;
        for (int i = 0; i < count; i++) {
            if (!devs[i].is_peripheral && strcmp(devs[i].pack_id, saved.pack_id) == 0) {
                already_live = 1;
                break;
            }
        }
        if (!already_live) devs[count++] = saved;
    }
    closedir(hd);
    if (count > 1) qsort(devs, (size_t)count, sizeof devs[0], bat_cmp_devices);
    return count;
}

/* --- Sample & Handoff Event I/O --- */

static inline int bat_parse_sample_line(const char *line, struct bat_sample *out)
{
    long long ts = 0;
    int dp = 0, pmw = 0, emwh = 0, vmv = 0, ac = 0;
    char st = 'U';
    if (sscanf(line, "%lld\t%d\t%d\t%d\t%d\t%c\t%d",
               &ts, &dp, &pmw, &emwh, &vmv, &st, &ac) != 7) {
        return -1;
    }
    if (ts < 946684800LL || dp < 0 || dp > 1000) return -1;
    out->ts = (int64_t)ts;
    out->decipct = dp;
    out->power_mw = pmw < 0 ? 0 : pmw;
    out->energy_mwh = emwh < 0 ? 0 : emwh;
    out->voltage_mv = vmv < 0 ? 0 : vmv;
    out->state = st;
    out->ac = ac ? 1 : 0;
    return 0;
}

static inline void bat_compact_file(const char *path, int64_t cutoff)
{
    char tmp[600], line[256];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof tmp) return;
    FILE *in = fopen(path, "r");
    if (!in) return;
    FILE *out = fopen(tmp, "w");
    if (!out) {
        fclose(in);
        return;
    }
    int ok = 1;
    while (fgets(line, sizeof line, in)) {
        long long ts = 0;
        if (sscanf(line, "%lld", &ts) == 1 && ts >= cutoff) {
            if (fputs(line, out) < 0) { ok = 0; break; }
        } else {
            /* Also support events.tsv where column 2 is ts */
            char kind[24];
            if (sscanf(line, "%23[^\t]\t%lld", kind, &ts) == 2 && ts >= cutoff) {
                if (fputs(line, out) < 0) { ok = 0; break; }
            }
        }
    }
    fclose(in);
    if (fflush(out) != 0) ok = 0;
    if (fclose(out) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) unlink(tmp);
}

static inline int bat_append_sample(const char *pack_id, const struct bat_sample *s)
{
    if (!bat_safe_name(pack_id) || s->ts < 946684800LL) return -1;
    const char *dir = bat_history_dir();
    (void)mkdir(dir, 0755);
    char path[560];
    int n = snprintf(path, sizeof path, "%s/%s.tsv", dir, pack_id);
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
    FILE *f = fopen(path, "a");
    if (!f) return -1;
    int rc = fprintf(f, "%lld\t%d\t%d\t%d\t%d\t%c\t%d\n",
                     (long long)s->ts, s->decipct, s->power_mw,
                     s->energy_mwh, s->voltage_mv, s->state, s->ac);
    long pos = ftell(f);
    if (fclose(f) != 0 || rc < 0) return -1;
    if (pos > BAT_COMPACT_BYTES) bat_compact_file(path, s->ts - BAT_KEEP_SECONDS);
    return 0;
}

static inline int bat_append_handoff(const char *kind, int64_t ts, const char *slot,
                                     const char *pack_a, int dpct_a,
                                     const char *pack_b, int dpct_b)
{
    if (ts < 946684800LL) return -1;
    const char *dir = bat_history_dir();
    (void)mkdir(dir, 0755);
    char path[560];
    int n = snprintf(path, sizeof path, "%s/events.tsv", dir);
    if (n <= 0 || (size_t)n >= sizeof path) return -1;
    FILE *f = fopen(path, "a");
    if (!f) return -1;
    int rc = fprintf(f, "%s\t%lld\t%s\t%s\t%d\t%s\t%d\n",
                     kind, (long long)ts,
                     (slot && slot[0]) ? slot : "-",
                     (pack_a && pack_a[0]) ? pack_a : "-", dpct_a,
                     (pack_b && pack_b[0]) ? pack_b : "-", dpct_b);
    long pos = ftell(f);
    if (fclose(f) != 0 || rc < 0) return -1;
    if (pos > BAT_COMPACT_BYTES / 4) bat_compact_file(path, ts - BAT_KEEP_SECONDS);
    return 0;
}

static inline int bat_load_handoffs(int64_t since_ts, struct bat_handoff_event *out, int max)
{
    if (max <= 0) return 0;
    char path[560], line[384];
    int n = snprintf(path, sizeof path, "%s/events.tsv", bat_history_dir());
    if (n <= 0 || (size_t)n >= sizeof path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int count = 0;
    while (fgets(line, sizeof line, f)) {
        struct bat_handoff_event ev;
        memset(&ev, 0, sizeof ev);
        long long ts = 0;
        if (sscanf(line, "%15[^\t]\t%lld\t%47[^\t]\t%95[^\t]\t%d\t%95[^\t\r\n]\t%d",
                   ev.kind, &ts, ev.slot, ev.pack_a, &ev.dpct_a, ev.pack_b, &ev.dpct_b) != 7) {
            continue;
        }
        ev.ts = (int64_t)ts;
        if (ev.ts < since_ts) continue;
        if (count == max) {
            memmove(out, out + 1, (size_t)(max - 1) * sizeof out[0]);
            count = max - 1;
        }
        out[count++] = ev;
    }
    fclose(f);
    return count;
}

static inline void bat_sample_from_device(const struct bat_device *b, int ac_online,
                                          int64_t now_ts, struct bat_sample *out)
{
    out->ts = now_ts;
    out->decipct = b->capacity_decipct;
    out->power_mw = b->power_w >= 0 ? (int)(b->power_w * 1000.0 + 0.5) : 0;
    out->energy_mwh = b->energy_now_wh >= 0 ? (int)(b->energy_now_wh * 1000.0 + 0.5) : 0;
    out->voltage_mv = b->voltage_v > 0 ? (int)(b->voltage_v * 1000.0 + 0.5) : 0;
    out->state = bat_state_code(b->status);
    out->ac = ac_online ? 1 : 0;
}

/* Load samples for pack_id (falling back to legacy slot_name.tsv if pack_id.tsv
 * does not exist yet) with ts >= since_ts. */
static inline int bat_load_history(const char *pack_id, const char *slot_fallback,
                                   int64_t since_ts, struct bat_sample *out, int max)
{
    if (!bat_safe_name(pack_id) || max <= 0) return 0;
    char path[560], line[256];
    int n = snprintf(path, sizeof path, "%s/%s.tsv", bat_history_dir(), pack_id);
    if (n <= 0 || (size_t)n >= sizeof path) return 0;
    FILE *f = fopen(path, "r");
    if (!f && slot_fallback && bat_safe_name(slot_fallback) && strcmp(pack_id, slot_fallback) != 0) {
        snprintf(path, sizeof path, "%s/%s.tsv", bat_history_dir(), slot_fallback);
        f = fopen(path, "r");
    }
    if (!f) return 0;
    struct bat_sample prev = { 0 };
    int has_prev = 0, count = 0;
    while (fgets(line, sizeof line, f)) {
        struct bat_sample s;
        if (bat_parse_sample_line(line, &s) != 0) continue;
        if (s.ts < since_ts) {
            prev = s;
            has_prev = 1;
            continue;
        }
        if (has_prev && count == 0 && (prev.state == 'R' || since_ts - prev.ts <= 600)) {
            out[count++] = prev;
            has_prev = 0;
        }
        if (count > 0 && s.ts < out[count - 1].ts) continue;
        if (count == max) {
            memmove(out, out + 1, (size_t)(max - 1) * sizeof out[0]);
            count = max - 1;
        }
        out[count++] = s;
    }
    if (count == 0 && has_prev && prev.state == 'R') out[count++] = prev;
    fclose(f);
    return count;
}

#endif
